#include "plugin/import/XmlPresetImport.h"

#include "plugin/PluginProcessor.h"
#include "plugin/PluginState.h"

#include <juce_core/zip/juce_zlib.h>

#include <cmath>
#include <cstdint>

// Preset importer, JUCE side. See XmlPresetImport.h; the engine and its conventions: ImportEngine.h, ImportCommon.h.
namespace psplug
{
namespace
{
    void convert (const juce::XmlElement& e, ps::imp::XmlNode& out, int depth)
    {
        out.tag = e.getTagName().toStdString();
        const int numAttributes = e.getNumAttributes();
        out.attributes.reserve ((size_t) juce::jmax (0, numAttributes));
        for (int i = 0; i < numAttributes; ++i)
            out.attributes.emplace_back (e.getAttributeName (i).toStdString(), e.getAttributeValue (i).toStdString());

        if (depth >= ps::imp::kMaxDepth)
            return;

        for (const auto* child : e.getChildIterator())
        {
            if (child->isTextElement())
                continue;
            out.children.emplace_back();
            convert (*child, out.children.back(), depth + 1);
        }
    }

    //==========================================================================
    // The stored Match EQ reference spectrum (an attachment of the Match EQ table, ImportMapEq.cpp). The binary child
    // holds records [u32 LE size A][A bytes]; a record is [u32 LE size B][zlib stream inflating to B bytes of typed
    // JSON]: an object whose members are {kind, value} pairs, with the per-bin levels in dB, the bin frequencies in Hz
    // (a linear grid) and an "in use" flag. Member names are matched by key, like every foreign string.
    constexpr ps::imp::Key kSpectrumLevels = 0x8c229383aecc23feull;   // per-bin levels in dB (array)
    constexpr ps::imp::Key kSpectrumHz     = 0xfccde9563d73f10cull;   // bin centre frequencies in Hz (array)
    constexpr ps::imp::Key kSpectrumInUse  = 0x70d885806b56289eull;   // the record holds a curve (bool)
    constexpr ps::imp::Key kTypedValue     = 0x41f7dca46d3f1b4aull;   // the value of a {kind, value} pair

    constexpr size_t kMaxRecordBytes = 4u * 1024u * 1024u;   // a stored curve inflates to ~0.4 MB
    constexpr int kMaxRecords = 64;
    constexpr int kMaxBins = 65536;

    std::uint32_t readU32 (const unsigned char* p) noexcept
    {
        return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8) | ((std::uint32_t) p[2] << 16)
               | ((std::uint32_t) p[3] << 24);
    }

    const juce::var* member (const juce::var& v, ps::imp::Key key, const ps::imp::Aliases* aliases)
    {
        if (auto* object = v.getDynamicObject())
            for (const auto& p : object->getProperties())
                if (ps::imp::sectionKey (p.name.toString().toStdString(), aliases) == key)
                    return &p.value;
        return nullptr;
    }

    // A {kind, value} pair gives its value; anything else is taken as it is.
    const juce::var& valueOf (const juce::var& v, const ps::imp::Aliases* aliases)
    {
        if (const auto* inner = member (v, kTypedValue, aliases))
            return *inner;
        return v;
    }

    bool readNumbers (const juce::var& v, const ps::imp::Aliases* aliases, std::vector<float>& out)
    {
        const auto* array = valueOf (v, aliases).getArray();
        if (array == nullptr || array->size() < 2 || array->size() > kMaxBins)
            return false;
        out.reserve ((size_t) array->size());
        for (const auto& e : *array)
        {
            const auto& x = valueOf (e, aliases);
            if (! (x.isDouble() || x.isInt() || x.isInt64()))
                return false;
            const double d = (double) x;
            if (! std::isfinite (d) || std::abs (d) > 1.0e7)
                return false;
            out.push_back ((float) d);
        }
        return true;
    }

    // Record `slot` (1-based) -> our reference (Left = Right = Mid = the curve). false when it is missing, damaged,
    // not in use or holds no usable curve.
    bool decodeReferenceRecord (const std::vector<unsigned char>& bytes, int slot, const ps::imp::Aliases* aliases,
                                ps::MatchSpectrum& out)
    {
        size_t pos = 0;
        for (int index = 1; index <= kMaxRecords && pos + 4 <= bytes.size(); ++index)
        {
            const size_t a = readU32 (bytes.data() + pos);
            pos += 4;
            if (a == 0 || a > bytes.size() - pos)
                return false;   // the end of the records, or a damaged size
            if (index != slot)
            {
                pos += a;
                continue;
            }

            if (a < 6)
                return false;
            const size_t rawSize = readU32 (bytes.data() + pos);
            if (rawSize == 0 || rawSize > kMaxRecordBytes)
                return false;

            // The stream adapter cannot distinguish a truncated stream from successful completion. Use the
            // bundled decoder's result and consumed length so missing checksums and trailing bytes are rejected.
            std::vector<unsigned char> raw (rawSize + 1);
            juce::zlibNamespace::z_stream stream {};
            stream.next_in = bytes.data() + pos + 4;
            stream.avail_in = (juce::zlibNamespace::uInt) (a - 4);
            stream.next_out = raw.data();
            stream.avail_out = (juce::zlibNamespace::uInt) raw.size();
            if (juce::zlibNamespace::inflateInit_ (&stream, ZLIB_VERSION, (int) sizeof (stream)) != Z_OK)
                return false;
            const int status = juce::zlibNamespace::inflate (&stream, Z_FINISH);
            // The writer pads each record with zero bytes after the stream; anything else after it is damage.
            bool onlyPadding = true;
            for (auto* p = stream.next_in; p != nullptr && p < stream.next_in + stream.avail_in; ++p)
                onlyPadding = onlyPadding && *p == 0;
            const bool complete = status == Z_STREAM_END && stream.total_out == rawSize && onlyPadding;
            juce::zlibNamespace::inflateEnd (&stream);
            if (! complete)
                return false;

            const auto* text = reinterpret_cast<const char*> (raw.data());
            if (! ps::imp::prescanJsonText ({ text, rawSize })
                || ! juce::CharPointer_UTF8::isValidString (text, (int) rawSize))
                return false;
            juce::var json;
            if (juce::JSON::parse (juce::String::fromUTF8 (text, (int) rawSize), json).failed())
                return false;

            const auto* inUse = member (json, kSpectrumInUse, aliases);
            const auto* levels = member (json, kSpectrumLevels, aliases);
            const auto* hz = member (json, kSpectrumHz, aliases);
            if (inUse == nullptr || levels == nullptr || hz == nullptr
                || ! valueOf (*inUse, aliases).isBool() || ! (bool) valueOf (*inUse, aliases))
                return false;

            std::vector<float> l, f;
            if (! readNumbers (*levels, aliases, l) || ! readNumbers (*hz, aliases, f) || l.size() != f.size())
                return false;

            // The grid starts at DC: keep the bins above 0 Hz, which must ascend.
            std::vector<float> binHz, binDb;
            for (size_t i = 0; i < f.size(); ++i)
            {
                // Power conversion must not overflow and silently turn a damaged bin into a missing value.
                if (l[i] > 1000.0f)
                    return false;
                if (f[i] < 0.0f || (i > 0 && f[i] <= f[i - 1]))
                    return false;
                if (f[i] == 0.0f)
                    continue;
                if (! binHz.empty() && f[i] <= binHz.back())
                    return false;
                binHz.push_back (f[i]);
                binDb.push_back (l[i]);
            }
            if (binHz.size() < 2)
                return false;

            // Per-bin powers of a linear grid: the bin width only offsets the level, which matching normalises.
            const double width = ((double) binHz.back() - (double) binHz.front()) / (double) (binHz.size() - 1);
            ps::setMatchSpectrumFromBins (out, binHz.data(), binDb.data(), (int) binHz.size(), width, "import");
            // The processor accepts bounded spectra. A damaged attachment must be reported here so that the
            // otherwise usable module settings still import. Check after density conversion / interpolation.
            for (const auto& channel : out.psdDb)
                for (const float db : channel)
                    if (! std::isfinite (db) || db < ps::kMatchNoValue || db > 1000.0f)
                        return false;
            return out.valid;
        }
        return false;
    }

    // Decodes the plan's Match EQ attachment into plan.matchReference; an unreadable one counts as not imported.
    void resolveAttachments (ps::imp::ImportPlan& plan, const ps::imp::Aliases* aliases)
    {
        for (const auto& a : plan.report.attachments)
        {
            if (a.type != ps::ModuleType::MatchEq || plan.matchReference != nullptr)
                continue;
            auto spectrum = std::make_shared<ps::MatchSpectrum>();
            if (decodeReferenceRecord (a.bytes, a.slot, aliases, *spectrum))
            {
                plan.matchReference = std::move (spectrum);
            }
            else
            {
                plan.report.mapped = juce::jmax (0, plan.report.mapped - a.mappedControls);
                plan.report.unmapped += a.mappedControls;
                for (int i = 0; i < a.mappedControls; ++i)
                    plan.report.unmappedList.push_back ({ "(Match EQ)", "reference spectrum",
                                                          "slot " + std::to_string (a.slot), "value" });
                plan.report.note ("Match EQ: the stored reference spectrum could not be read; capture or load a "
                                  "reference again");
            }
        }
        plan.report.attachments.clear();   // decoded (or reported); the plan does not carry the raw bytes on
    }

    juce::String moduleNameOf (int type)
    {
        return type >= 0 && type < ps::kNumModuleTypes ? juce::String (ps::moduleInfo ((ps::ModuleType) type).name)
                                                       : juce::String ("module");
    }
} // namespace

//==============================================================================
bool parseImportXml (const juce::String& text, ps::imp::XmlNode& root, juce::String& error)
{
    const auto utf8 = text.toStdString();
    if (const auto problem = ps::imp::prescanXmlText (utf8); ! problem.empty())
    {
        error = problem;
        return false;
    }

    juce::XmlDocument doc (text);
    const auto xml = doc.getDocumentElement();
    if (xml == nullptr)
    {
        error = "The file is not valid XML.";
        return false;
    }

    root = {};
    convert (*xml, root, 0);
    return true;
}

ps::imp::ImportPlan planXmlPresetText (const juce::String& text, const ps::imp::ImportOptions& options)
{
    ps::imp::XmlNode root;
    juce::String error;
    if (! parseImportXml (text, root, error))
    {
        ps::imp::ImportPlan plan;
        plan.error = error.toStdString();
        return plan;
    }
    auto plan = ps::imp::buildImportPlan (root, options);
    resolveAttachments (plan, options.aliases);
    return plan;
}

ps::imp::ImportPlan planXmlPresetFile (const juce::File& file, const ps::imp::ImportOptions& options)
{
    ps::imp::ImportPlan plan;
    if (! file.existsAsFile())
    {
        plan.error = "The file could not be found.";
        return plan;
    }
    if (file.getSize() > (juce::int64) ps::imp::kMaxFileBytes)
    {
        plan.error = "The file is too large to be a preset.";
        return plan;
    }

    juce::MemoryBlock data;
    if (! file.loadFileAsData (data))
    {
        plan.error = "The file could not be read.";
        return plan;
    }
    if (data.getSize() > ps::imp::kMaxFileBytes)
    {
        plan.error = "The file is too large to be a preset.";
        return plan;
    }

    // Scan bytes before String conversion, which would silently truncate at an embedded NUL.
    const auto bytes = std::string_view (static_cast<const char*> (data.getData()), data.getSize());
    if (const auto error = ps::imp::prescanXmlText (bytes); ! error.empty())
    {
        plan.error = error;
        return plan;
    }
    if (! juce::CharPointer_UTF8::isValidString (bytes.data(), (int) bytes.size()))
    {
        plan.error = "The file is not valid UTF-8 text.";
        return plan;
    }
    return planXmlPresetText (data.toString(), options);
}

//==============================================================================
XmlImportResult applyImportPlan (PureSheenProcessor& proc, ps::imp::ImportPlan plan, const juce::String& name, int scope)
{
    XmlImportResult result;
    result.name = name.trim().isNotEmpty() ? name.trim() : juce::String ("Imported preset");

    const auto* messages = juce::MessageManager::getInstanceWithoutCreating();
    if (plan.ok && (messages == nullptr || ! messages->isThisTheMessageThread()))
    {
        plan.ok = false;
        plan.error = "Presets must be applied on the message thread.";
    }
    if (plan.ok && (scope < -1 || scope >= ps::kNumModuleTypes || plan.scope < -1 || plan.scope >= ps::kNumModuleTypes
                    || plan.values.size() != ps::paramSpecs().size()))
    {
        plan.ok = false;
        plan.error = "The preset plan is invalid.";
    }

    if (plan.ok && scope >= 0)
    {
        const bool eqSwap = (plan.scope == (int) ps::ModuleType::Equalizer1 && scope == (int) ps::ModuleType::Equalizer2)
                            || (plan.scope == (int) ps::ModuleType::Equalizer2 && scope == (int) ps::ModuleType::Equalizer1);
        if (plan.scope < 0)
        {
            plan.ok = false;
            plan.error = "This file holds a whole chain. Import it from the preset menu at the top.";
        }
        else if (eqSwap)
        {
            // Equalizer 1 and 2 share their parameter layout (like our own module presets).
            const auto from = moduleIdPrefix ((ps::ModuleType) plan.scope).toStdString();
            const auto to = moduleIdPrefix ((ps::ModuleType) scope).toStdString();
            auto moved = defaultValues();
            const auto& specs = ps::paramSpecs();
            for (size_t i = 0; i < specs.size() && i < plan.values.size(); ++i)
                if (specs[i].module == plan.scope)
                {
                    const int j = ps::paramIndex (to + specs[i].id.substr (from.size()));
                    if (j >= 0)
                        moved[(size_t) j] = plan.values[i];
                }
            plan.values = std::move (moved);
            plan.scope = scope;
            plan.order = { (ps::ModuleType) scope };
            plan.report.imported = { (ps::ModuleType) scope };
        }
        else if (plan.scope != scope)
        {
            plan.ok = false;
            plan.error = "This file is a preset for " + moduleNameOf (plan.scope).toStdString() + ", not for "
                         + moduleNameOf (scope).toStdString() + ".";
        }
    }

    bool referenceApplied = false;
    if (plan.ok)
    {
        const auto order = plan.scope < 0 ? plan.order
                                          : ps::imp::insertIntoChain (proc.getChainOrder(), (ps::ModuleType) plan.scope);
        // The reference spectrum is tagged with the preset name (shown as its source in the Match EQ panel).
        std::shared_ptr<const ps::MatchSpectrum> reference;
        if (plan.matchReference != nullptr && (plan.scope < 0 || plan.scope == (int) ps::ModuleType::MatchEq))
        {
            auto named = std::make_shared<ps::MatchSpectrum> (*plan.matchReference);
            const auto ascii = result.name.retainCharacters (
                " !#$%&'()+,-.0123456789;=@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`abcdefghijklmnopqrstuvwxyz{}~");
            ps::setMatchSource (*named, ("import:" + ascii).toRawUTF8());
            reference = std::move (named);
        }
        plan.ok = proc.applyImportedPreset (plan.scope, plan.values, order, result.name, reference.get());
        if (! plan.ok)
            plan.error = "The preset could not be applied.";
        referenceApplied = plan.ok && reference != nullptr;
    }

    result.ok = plan.ok;
    result.message = juce::String (ps::imp::summariseReport (plan));
    if (result.ok && plan.scope < 0)
        result.message = "\"" + result.name + "\": " + result.message;
    if (referenceApplied)   // ours matches against a captured Current spectrum (the foreign module analyses live)
        result.message += " Match EQ: capture your track as Current to apply the imported reference.";
    result.plan = std::move (plan);
    return result;
}

XmlImportResult importXmlPreset (PureSheenProcessor& proc, const juce::File& file, int scope,
                                 const ps::imp::ImportOptions& options)
{
    return applyImportPlan (proc, planXmlPresetFile (file, options), file.getFileNameWithoutExtension(), scope);
}

//==============================================================================
struct XmlImportChooser::State
{
    struct Folder { juce::File last; };
    struct Request { std::unique_ptr<juce::FileChooser> chooser; };

    juce::SharedResourcePointer<Folder> folder;
    std::shared_ptr<Request> pending;
    bool alive = true;
};

XmlImportChooser::XmlImportChooser() : state (std::make_shared<State>()) {}

XmlImportChooser::~XmlImportChooser()
{
    cancelPending();
}

void XmlImportChooser::cancelPending()
{
    state->alive = false;
    state->pending.reset();   // closes and joins a native dialog before the owner / plug-in can go away
}

juce::File XmlImportChooser::startingFolder() const
{
    return state->folder->last.isDirectory() ? state->folder->last
                                             : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
}

std::function<void (const juce::File&)> XmlImportChooser::begin (
    PureSheenProcessor& proc, int scope, std::function<void (const XmlImportResult&)> done,
    const ps::imp::ImportOptions& options)
{
    const auto* messages = juce::MessageManager::getInstanceWithoutCreating();
    if (messages == nullptr || ! messages->isThisTheMessageThread() || ! state->alive || state->pending != nullptr)
        return {};

    state->pending = std::make_shared<State::Request>();
    const std::weak_ptr<State> owner = state;
    const std::weak_ptr<State::Request> request = state->pending;
    return [owner, request, &proc, scope, options, done = std::move (done)] (const juce::File& file)
    {
        const auto s = owner.lock();
        const auto completed = request.lock();
        if (s == nullptr || ! s->alive || completed == nullptr || s->pending != completed)
            return;

        // Keep this dialog alive through all result processing, including callbacks that destroy the owning UI
        // or launch another dialog. JUCE's finished() does not touch the chooser after calling us. No queued
        // cleanup can cancel a later dialog or survive the plug-in's lifetime.
        s->pending.reset();
        if (file.existsAsFile())
        {
            s->folder->last = file.getParentDirectory();
            const auto result = importXmlPreset (proc, file, scope, options);
            if (s->alive && done != nullptr)
                done (result);
        }
    };
}

void XmlImportChooser::launch (PureSheenProcessor& proc, int scope, std::function<void (const XmlImportResult&)> done)
{
    auto complete = begin (proc, scope, std::move (done));
    if (complete == nullptr)
        return;
    auto& chooser = state->pending->chooser;
    chooser = std::make_unique<juce::FileChooser> ("Import preset (.xml)", startingFolder(), "*.xml");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [complete = std::move (complete)] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        complete (file);
    });
}
} // namespace psplug
