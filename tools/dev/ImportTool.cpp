#include "dsp/MatchEq.h"
#include "plugin/PluginProcessor.h"
#include "plugin/PluginState.h"
#include "plugin/import/XmlPresetImport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

int runImportProcessorTests();

namespace psplug
{
// Exercise the production one-shot completion without opening a system dialog or synthesising mouse input.
struct XmlImportChooserTestAccess
{
    static std::function<void (const juce::File&)> begin (
        XmlImportChooser& chooser, PureSheenProcessor& processor,
        std::function<void (const XmlImportResult&)> done, const ps::imp::ImportOptions& options)
    {
        return chooser.begin (processor, -1, std::move (done), options);
    }

    static juce::File folder (const XmlImportChooser& chooser) { return chooser.startingFolder(); }
    static void cancel (XmlImportChooser& chooser) { chooser.cancelPending(); }
};
}

// Preset importer commands of the dev tool (owner: importer framework).
//
//   PureSheenDev import <file-or-folder> ... [--verbose] [--values] [--no-audio]
//       Imports every .xml found (folders recursively) into one processor, one after the other (each file starts from
//       the default state), prints one line per file and a summary: files OK / not importable / failed, modules
//       imported and skipped, mapped / unmapped / ignored / clamped values, and the unmapped values grouped by section
//       and ParamID template (strings from the files). Each imported state then plays 0.25 s of noise, which must
//       stay finite. Exit code 0 when no file failed (a file for a module we do not have is "not importable", not a
//       failure). --verbose adds the result note, notes and every unmapped value per file; --values prints every
//       parameter of the imported modules that differs from our default (id = plain value), to check a table.
//   PureSheenDev import --defaults
//       Lists every Likely / Guess foreign default in the tables (our words), for out/importer/defaults-to-confirm.md.
//   PureSheenDev importkey "<ParamID or section name>" ...
//       Prints the template, set, number and key the importer computes (for writing table constants).
//   PureSheenDev importtest
//       The XML text path and processor-level checks: juce::XmlDocument parsing, decimal commas, hostile text (fuzz),
//       one undo step per import, chain replace / module insert, preset name, finite audio. Then, only when the
//       user's local folder of real-product check saves exists (a "checks" folder inside a git-ignored "ignore_*"
//       folder of the repository, or the folder named by the PURESHEEN_IMPORT_CHECKS environment variable), every
//       check save is imported and compared with what the user confirmed (module power, L/R, band shapes, detection,
//       knee, limiter algorithm, Reset defaults). The files are read in place, never copied.
namespace
{
using namespace ps::imp;

int failures = 0;

void check (bool ok, const std::string& name, const std::string& detail = {})
{
    std::cout << (ok ? "  PASS " : "  FAIL ") << name;
    if (! detail.empty())
        std::cout << "  (" << detail << ")";
    std::cout << "\n";
    if (! ok)
        ++failures;
}

std::string hex (Key k)
{
    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "0x%016llxull", (unsigned long long) k);
    return buffer;
}

struct Proc
{
    PureSheenProcessor p;
    std::uint32_t seed = 1u;

    Proc()
    {
        p.setRateAndBufferSizeDetails (48000.0, 512);
        p.prepareToPlay (48000.0, 512);
    }

    void toDefaults()
    {
        p.applyImportedPreset (-1, psplug::defaultValues(), psplug::defaultChain(), "Default");
        p.flushPendingChanges (true);
    }

    // 0.25 s of deterministic noise at -12 dBFS through the processor; false when any output sample is not finite.
    bool playsFinite (int blocks = 24)
    {
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                {
                    seed = seed * 1664525u + 1013904223u;
                    buffer.setSample (ch, i, 0.25f * ((float) (seed >> 8) / 8388608.0f - 1.0f));
                }
            p.processBlock (buffer, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    if (! std::isfinite (buffer.getSample (ch, i)))
                        return false;
        }
        return true;
    }
};

std::string templateOf (const std::string& rawParam)
{
    const auto n = splitParamName (rawParam);
    return (n.set == 1 ? "Aux: " : "") + n.templ;
}

//==============================================================================
int runDefaults()
{
    std::vector<const ModuleSpec*> specs = allModuleSpecs();
    specs.push_back (&globalSpec());
    int count = 0;
    for (const auto* s : specs)
    {
        const std::string module = (int) s->type < ps::kNumModuleTypes ? ps::moduleInfo (s->type).name : "Global";
        for (const auto& r : *s->rules)
        {
            if (r.def.conf == Conf::Known || r.act == Act::Ignore || r.act == Act::Unsupported)
                continue;
            std::ostringstream value;
            if (r.def.fn != nullptr)
                value << "version-dependent";
            else if (std::isnan (r.def.value))
                value << "none (our default kept)";
            else
                value << r.def.value;
            std::cout << module << " | " << r.what << " | " << value.str() << " | "
                      << (r.def.conf == Conf::Likely ? "likely" : "guess") << "\n";
            ++count;
        }
    }
    std::cout << count << " defaults to confirm\n";
    return 0;
}

int runKeys (const juce::StringArray& args)
{
    for (const auto& a : args)
    {
        const auto s = a.toStdString();
        const auto n = splitParamName (s);
        std::cout << hex (n.key) << "  template \"" << n.templ << "\"  set " << (n.set == 0 ? "A" : "B")
                  << "  number " << n.number << "   (whole string: " << hex (importKey (s)) << ")\n";
    }
    return 0;
}
} // namespace

//==============================================================================
int runImport (const juce::StringArray& args)
{
    bool verbose = false, audio = true, dumpValues = false;
    juce::Array<juce::File> files;
    std::vector<juce::File> roots;

    for (const auto& a : args)
    {
        if (a == "--verbose")
            verbose = true;
        else if (a == "--values")
            dumpValues = true;
        else if (a == "--no-audio")
            audio = false;
        else if (a == "--defaults")
            return runDefaults();
        else
        {
            const auto f = juce::File::getCurrentWorkingDirectory().getChildFile (a);
            roots.push_back (f);
            if (f.isDirectory())
                f.findChildFiles (files, juce::File::findFiles, true, "*.xml");
            else if (f.existsAsFile())
                files.add (f);
            else
                std::cout << "not found: " << a << "\n";
        }
    }

    if (files.isEmpty())
    {
        std::cout << "usage: PureSheenDev import <file-or-folder> ... [--verbose] [--values] [--no-audio] | import --defaults\n";
        return 2;
    }

    struct Less
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            return a.getFullPathName().compareNatural (b.getFullPathName());
        }
    } less;
    files.sort (less);

    Proc proc;
    int ok = 0, notImportable = 0, failed = 0, globalFiles = 0, moduleFiles = 0, audioFailures = 0;
    long long mapped = 0, unmapped = 0, ignored = 0, clamped = 0, defaulted = 0, references = 0;
    std::map<std::string, int> unmappedGroups, skippedCounts, importedCounts, notes;

    auto relative = [&roots] (const juce::File& f)
    {
        for (const auto& r : roots)
            if (f.isAChildOf (r))
                return f.getRelativePathFrom (r).toStdString();
        return f.getFileName().toStdString();
    };

    for (const auto& file : files)
    {
        auto plan = psplug::planXmlPresetFile (file);
        const auto rel = relative (file);

        if (! plan.ok)
        {
            const bool refused = ! plan.report.skipped.empty() || plan.holdsNoModules;
            (refused ? notImportable : failed)++;
            for (const auto& s : plan.report.skipped)
                ++skippedCounts[s + " (module file)"];
            std::cout << (refused ? "SKIP " : "FAIL ") << rel << ": " << plan.error << "\n";
            continue;
        }

        const auto report = plan.report;
        if (audio)
            proc.toDefaults();
        const auto result = psplug::applyImportPlan (proc.p, std::move (plan), file.getFileNameWithoutExtension(), -1);
        proc.p.flushPendingChanges (true);
        if (! result.ok)
        {
            ++failed;
            std::cout << "FAIL " << rel << ": " << result.message << "\n";
            continue;
        }

        bool finite = true;
        if (audio)
        {
            finite = proc.playsFinite();
            if (! finite)
                ++audioFailures;
        }

        ++ok;
        (report.globalFile ? globalFiles : moduleFiles)++;
        mapped += report.mapped;
        unmapped += report.unmapped;
        ignored += report.ignored;
        clamped += report.clamped;
        defaulted += report.defaulted;
        if (result.plan.matchReference != nullptr)
            ++references;
        for (const auto t : report.imported)
            ++importedCounts[ps::moduleInfo (t).name];
        for (const auto& s : report.skipped)
            ++skippedCounts[s];
        for (const auto& n : report.notes)
            ++notes[n];
        for (const auto& u : report.unmappedList)
            ++unmappedGroups[u.section + " / " + templateOf (u.param) + " (" + u.reason + ")"];

        std::cout << (finite ? "OK   " : "NAN  ") << (report.globalFile ? "chain  " : "module ")
                  << std::setw (2) << report.imported.size() << " mod";
        if (! report.skipped.empty())
            std::cout << " (" << report.skipped.size() << " skipped)";
        std::cout << "  mapped " << report.mapped << ", unmapped " << report.unmapped << ", ignored " << report.ignored
                  << ", clamped " << report.clamped << "  " << rel << "\n";

        if (verbose)
        {
            std::cout << "     " << result.message << "\n";
            for (const auto& n : report.notes)
                std::cout << "     note: " << n << "\n";
            for (const auto& u : report.unmappedList)
                std::cout << "     unmapped: " << u.section << " / " << u.param << " = " << u.value << " (" << u.reason << ")\n";
        }

        if (dumpValues)
        {
            const auto& specs = ps::paramSpecs();
            const auto& values = result.plan.values;
            for (size_t i = 0; i < specs.size() && i < values.size(); ++i)
            {
                const auto t = (ps::ModuleType) specs[i].module;
                const bool inScope = result.plan.scope < 0
                                         ? std::find (report.imported.begin(), report.imported.end(), t) != report.imported.end()
                                         : specs[i].module == result.plan.scope;
                if (specs[i].module >= 0 && inScope && std::abs (values[i] - specs[i].defaultValue) > 1.0e-6f)
                    std::cout << "     " << specs[i].id << " = " << values[i] << "\n";
            }
        }
    }

    std::cout << "\n==== import summary\n"
              << "files " << files.size() << ": " << ok << " imported (" << globalFiles << " chain, " << moduleFiles
              << " single module), " << notImportable << " not importable (module we do not have, or no module), "
              << failed << " failed\n";
    if (audio)
        std::cout << "audio check: " << (ok - audioFailures) << " of " << ok << " imported states play finite audio\n";
    std::cout << "values: " << mapped << " mapped, " << unmapped << " unmapped, " << ignored << " ignored, " << clamped
              << " clamped; " << defaulted << " of our parameters set from foreign defaults\n";
    std::cout << "Match EQ reference spectra imported: " << references << "\n";

    std::cout << "modules imported:";
    for (const auto& [name, n] : importedCounts)
        std::cout << " " << name << " " << n << ";";
    std::cout << "\nmodules skipped:";
    for (const auto& [name, n] : skippedCounts)
        std::cout << " " << name << " " << n << ";";
    std::cout << "\n";
    for (const auto& [n, count] : notes)
        std::cout << "note x" << count << ": " << n << "\n";

    std::vector<std::pair<int, std::string>> groups;
    for (const auto& [name, n] : unmappedGroups)
        groups.emplace_back (n, name);
    std::sort (groups.begin(), groups.end(), [] (const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    const size_t shown = verbose ? groups.size() : std::min<size_t> (groups.size(), 60);
    std::cout << "unmapped values by section / ParamID template (" << groups.size() << " groups";
    if (shown < groups.size())
        std::cout << ", top " << shown << "; --verbose for all";
    std::cout << "):\n";
    for (size_t i = 0; i < shown; ++i)
        std::cout << std::setw (7) << groups[i].first << "  " << groups[i].second << "\n";

    return failed == 0 && audioFailures == 0 ? 0 : 1;
}

int runImportKey (const juce::StringArray& args) { return runKeys (args); }

//==============================================================================
namespace
{
// Our generic names for the Imager and the format's sections (the keys come from the importer's tables).
const Aliases& testAliases()
{
    static const Aliases a = []
    {
        auto specOf = [] (ps::ModuleType t) -> const ModuleSpec&
        {
            for (const auto* s : allModuleSpecs())
                if (s->type == t)
                    return *s;
            return globalSpec();
        };
        auto key = [&specOf] (ps::ModuleType t, const char* what) -> Key
        {
            for (const auto& r : *specOf (t).rules)
                if (std::string (r.what) == what)
                    return r.key;
            return 0;
        };
        const auto img = ps::ModuleType::Imager;
        Aliases m;
        m["Spread"] = specOf (img).tagKey;
        m["SpreadChain"] = specOf (img).elementKey;
        m["Tone"] = specOf (ps::ModuleType::Equalizer1).tagKey;
        m["ToneChain"] = specOf (ps::ModuleType::Equalizer1).elementKey;
        m["Late"] = specOf (ps::ModuleType::Equalizer2).tagKey;
        m["Limit"] = specOf (ps::ModuleType::Maximizer).tagKey;
        m["LimitChain"] = specOf (ps::ModuleType::Maximizer).elementKey;
        m["Common"] = kGlobalSectionKey;
        m["Order"] = kChainBlobKey;
        m["Restorer"] = skippedModuleKey ("mix rebalancing");
        m["Width #"] = key (img, "band width");
        m["Present #"] = key (img, "band present");
        m["Edge #"] = key (img, "crossover frequency");
        m["Split"] = key (img, "channel mode");

        // Match EQ and the stored reference spectrum (record member names: our words)
        const auto meq = ps::ModuleType::MatchEq;
        m["Copy"] = specOf (meq).tagKey;
        m["CopyChain"] = specOf (meq).elementKey;
        m["Strength"] = key (meq, "match amount");
        m["Slot # Used"] = key (meq, "reference spectrum slot");
        m["Picture"] = 0xce256a8918a7f18dull;   // element of the binary child holding the spectrum records
        m["levels"] = 0x8c229383aecc23feull;    // per-bin levels in dB
        m["hz"] = 0xfccde9563d73f10cull;        // bin frequencies
        m["used"] = 0x70d885806b56289eull;      // the record holds a curve
        m["data"] = 0x41f7dca46d3f1b4aull;      // the value of a {kind, value} pair
        return m;
    }();
    return a;
}

ImportOptions testOptions()
{
    ImportOptions o;
    o.aliases = &testAliases();
    return o;
}

// "AAoAAAB..." chain blob for names.
juce::String chainData (const juce::StringArray& names)
{
    juce::MemoryOutputStream out;
    for (const auto& n : names)
    {
        out.writeByte (0);
        out.writeInt ((int) n.getNumBytesAsUTF8());   // little endian
        out.write (n.toRawUTF8(), n.getNumBytesAsUTF8());
    }
    return juce::Base64::toBase64 (out.getData(), out.getDataSize());
}

const char* const kChainFile =
    "<?xml version=\"1.0\" standalone=\"yes\" ?>\r\n"
    "<Root PresetVer=\"3\" PluginVer=\"100000\" PluginBuild=\"1\" Comments=\"A &apos;test&apos; &amp; more\">\r\n"
    "    <Common Enabled=\"1\">\r\n"
    "        <ExtraBytes ElementID=\"Order\" Data=\"%CHAIN%\" />\r\n"
    "    </Common>\r\n"
    "    <Tone Enabled=\"1\" />\r\n"
    "    <Spread Enabled=\"1\">\r\n"
    "        <Param ElementID=\"SpreadChain\" ParamID=\"Glow 1\" Value=\"1\" />\r\n"
    "        <Param ElementID=\"SpreadChain\" ParamID=\"Present 1\" Value=\"1\" />\r\n"
    "        <Param ElementID=\"SpreadChain\" ParamID=\"Edge 1\" Value=\"250,00000000\" />\r\n"
    "        <Param ElementID=\"SpreadChain\" ParamID=\"Width 2\" Value=\"40,50000000\" />\r\n"
    "        <Param ElementID=\"SpreadChain\" ParamID=\"Aux: Width 1\" Value=\"-20.00000000\" />\r\n"
    "        <Param ElementID=\"SpreadChain\" ParamID=\"Split\" Value=\"1\" />\r\n"
    "        <Unknown Thing=\"1\"><Deeper/></Unknown>\r\n"
    "    </Spread>\r\n"
    "    <Limit Enabled=\"1\" />\r\n"
    "    <Restorer Enabled=\"1\" />\r\n"
    "    <Late Enabled=\"1\" />\r\n"
    "</Root>\r\n";

juce::String chainFileText()
{
    return juce::String (kChainFile).replace ("%CHAIN%", chainData ({ "Restorer", "ToneChain", "SpreadChain", "LimitChain" }));
}

juce::String moduleFileText (const juce::String& version)
{
    return "<Root PresetVer=\"1\" PluginVer=\"" + version + "\">\n"
           "  <Common Enabled=\"0\"><ExtraBytes ElementID=\"Order\" Data=\"" + chainData ({ "ToneChain", "SpreadChain" }) + "\"/></Common>\n"
           "  <Tone Enabled=\"0\"/>\n"
           "  <Spread Enabled=\"1\"><Param ElementID=\"SpreadChain\" ParamID=\"Width 1\" Value=\"55\"/></Spread>\n"
           "</Root>\n";
}

juce::MemoryBlock packSpectrumJson (const juce::String& json)
{
    juce::MemoryOutputStream packed;
    {
        juce::GZIPCompressorOutputStream z (packed, 6, 0);
        z.write (json.toRawUTF8(), json.getNumBytesAsUTF8());
    }
    juce::MemoryOutputStream record;
    record.writeInt ((int) json.getNumBytesAsUTF8());
    record.write (packed.getData(), packed.getDataSize());
    return record.getMemoryBlock();
}

// One spectrum record: [u32 raw size][zlib of typed JSON] with `bins` linear bins up to 24 kHz of a -3 dB/oct tilt.
juce::MemoryBlock spectrumRecord (int bins, bool used, int levelsAt = -1, const char* levelsText = nullptr)
{
    juce::String json = "{\n \"used\" : { \"kind\" : \"Bool\", \"data\" : " + juce::String (used ? "true" : "false") + " },\n";
    juce::String levels, hz;
    for (int k = 0; k < bins; ++k)
    {
        const double f = k * 24000.0 / bins;
        const double db = f > 0.0 ? -30.0 - 3.0 * std::log2 (f / 1000.0) : -200.0;
        levels << (k > 0 ? ", " : "") << "{ \"kind\" : \"Float\", \"data\" : "
               << (k == levelsAt && levelsText != nullptr ? juce::String (levelsText) : juce::String (db, 6)) << " }";
        hz << (k > 0 ? ", " : "") << "{ \"kind\" : \"Float\", \"data\" : " << juce::String (f, 6) << " }";
    }
    json << " \"levels\" : { \"kind\" : \"Array\", \"data\" : [ " << levels << " ] },\n"
         << " \"hz\" : { \"kind\" : \"Array\", \"data\" : [ " << hz << " ] }\n}\n";

    return packSpectrumJson (json);
}

// The binary child: the records, each as [u32 size][bytes].
juce::String spectrumData (const std::vector<juce::MemoryBlock>& records)
{
    juce::MemoryOutputStream out;
    for (const auto& r : records)
    {
        out.writeInt ((int) r.getSize());
        out.write (r.getData(), r.getSize());
    }
    return juce::Base64::toBase64 (out.getData(), out.getDataSize());
}

juce::String matchFileText (int slot, const juce::String& data)
{
    return "<Root PresetVer=\"1\" PluginVer=\"9030\">\n"
           "  <Common Enabled=\"0\"/>\n"
           "  <Copy Enabled=\"1\">\n"
           "    <Param ElementID=\"CopyChain\" ParamID=\"Strength\" Value=\"40\"/>\n"
           "    <Param ElementID=\"Picture\" ParamID=\"Slot " + juce::String (slot) + " Used\" Value=\"1\"/>\n"
           "    <ExtraBytes ElementID=\"Picture\" Data=\"" + data + "\"/>\n"
           "  </Copy>\n"
           "</Root>\n";
}

float valueOf (const ImportPlan& p, const char* id)
{
    const int i = ps::paramIndex (id);
    return i >= 0 && i < (int) p.values.size() ? p.values[(size_t) i] : -1.0e9f;
}

bool inRange (const ImportPlan& p)
{
    const auto& specs = ps::paramSpecs();
    if (p.values.size() != specs.size())
        return false;
    for (size_t i = 0; i < specs.size(); ++i)
        if (! std::isfinite (p.values[i]) || p.values[i] < specs[i].minValue - 1.0e-3f || p.values[i] > specs[i].maxValue + 1.0e-3f)
            return false;
    return true;
}

void runChooserTests()
{
    using Chooser = psplug::XmlImportChooser;
    using Access = psplug::XmlImportChooserTestAccess;
    using Completion = std::function<void (const juce::File&)>;
    std::cout << "== importer: chooser lifetime\n";
    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("PureSheen chooser test", ".xml");
    check (temp.replaceWithText (chainFileText()), "import: chooser fixture written");
    int delivered = 0;
    auto done = [&] (const psplug::XmlImportResult& r) { if (r.ok) ++delivered; };
    {
        PureSheenProcessor processor;
        Chooser chooser;
        auto first = Access::begin (chooser, processor, done, testOptions());
        auto overlapping = Access::begin (chooser, processor, done, testOptions());
        check (first != nullptr && overlapping == nullptr,
               "import: a second chooser launch leaves the open request alone");
        first (temp);
        const auto history = processor.getHistory().size();
        auto second = Access::begin (chooser, processor, done, testOptions());
        first (temp);
        check (delivered == 1 && processor.getHistory().size() == history && second != nullptr
                   && Access::begin (chooser, processor, done, testOptions()) == nullptr,
               "import: a completed callback cannot run twice or cancel a later dialog");
        second ({});
        auto afterCancel = Access::begin (chooser, processor, done, testOptions());
        check (afterCancel != nullptr && delivered == 1,
               "import: cancellation permits another launch without applying a preset");
        afterCancel ({});

        Completion reentered;
        auto reenter = Access::begin (chooser, processor, [&] (const psplug::XmlImportResult& r)
        {
            done (r);
            reentered = Access::begin (chooser, processor, done, testOptions());
        }, testOptions());
        reenter (temp);
        check (reentered != nullptr && Access::begin (chooser, processor, done, testOptions()) == nullptr,
               "import: a result callback can launch another dialog without old cleanup closing it");
        reentered ({});

        Chooser sibling;
        check (Access::folder (sibling) == temp.getParentDirectory(),
               "import: live header and panel choosers share their last folder");
    }
    {
        Chooser fresh;
        check (Access::folder (fresh) == juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
               "import: last-folder storage is released with the last importer");
    }
    {
        PureSheenProcessor processor;
        Chooser chooser;
        auto pending = Access::begin (chooser, processor, done, testOptions());
        const int before = delivered;
        // Native cancellation can dispatch messages while joining its worker. The owner must already be inert.
        Access::cancel (chooser);
        const auto reentered = Access::begin (chooser, processor, done, testOptions());
        pending (temp);
        check (reentered == nullptr && delivered == before,
               "import: teardown refuses a reentrant launch or completion during native cancellation");
    }
    {
        auto processor = std::make_unique<PureSheenProcessor>();
        auto chooser = std::make_unique<Chooser>();
        auto late = Access::begin (*chooser, *processor, done, testOptions());
        const int before = delivered;
        chooser.reset();
        processor.reset();
        late (temp);
        check (delivered == before, "import: a late callback after chooser and processor destruction is inert");
    }
    {
        PureSheenProcessor processor;
        auto chooser = std::make_unique<Chooser>();
        auto complete = Access::begin (*chooser, processor, [&] (const psplug::XmlImportResult& r)
        {
            done (r);
            chooser.reset();
        }, testOptions());
        const int before = delivered;
        complete (temp);
        check (chooser == nullptr && delivered == before + 1,
               "import: result delivery can destroy its chooser safely");
    }
    {
        PureSheenProcessor processor;
        auto chooser = std::make_unique<Chooser>();
        struct CloseDuringImport final : juce::AudioProcessorParameter::Listener
        {
            explicit CloseDuringImport (std::unique_ptr<Chooser>& c) : chooser (c) {}
            void parameterValueChanged (int, float) override { chooser.reset(); }
            void parameterGestureChanged (int, bool) override {}
            std::unique_ptr<Chooser>& chooser;
        } listener (chooser);
        auto* width = processor.getParameter ("img.b2.width");
        width->addListener (&listener);
        auto complete = Access::begin (*chooser, processor, done, testOptions());
        const int before = delivered;
        complete (temp);
        width->removeListener (&listener);
        check (chooser == nullptr && delivered == before
                   && std::abs (processor.getParameterValue ("img.b2.width") - 40.5f) < 0.01f,
               "import: closing the UI during application suppresses result delivery to the destroyed owner");
    }
    temp.deleteFile();
}
} // namespace

//==============================================================================
namespace
{
// The user's local folder of real-product check saves: PURESHEEN_IMPORT_CHECKS, else "<dir>/ignore_*/checks" for the
// working directory, the dev tool's folder and their parents. Empty when there is none.
juce::File findCheckSaves()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("PURESHEEN_IMPORT_CHECKS", {});
    if (env.isNotEmpty())
        return juce::File (env).isDirectory() ? juce::File (env) : juce::File();

    std::vector<juce::File> starts { juce::File::getCurrentWorkingDirectory(),
                                     juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory() };
    for (auto dir : starts)
        for (int up = 0; up < 6 && dir.exists(); ++up, dir = dir.getParentDirectory())
        {
            for (const auto& sub : dir.findChildFiles (juce::File::findDirectories, false, "ignore_*"))
                if (sub.getChildFile ("checks").isDirectory())
                    return sub.getChildFile ("checks");
            if (dir.getParentDirectory() == dir)
                break;
        }
    return {};
}

// The choice label our parameter `id` has in the plan ("" when not a choice).
std::string choiceLabel (const ImportPlan& p, const char* id)
{
    const int i = ps::paramIndex (id);
    if (i < 0 || i >= (int) p.values.size())
        return {};
    const auto& choices = ps::paramSpecs()[(size_t) i].choices;
    const int c = (int) std::lround (p.values[(size_t) i]);
    return c >= 0 && c < (int) choices.size() ? choices[(size_t) c] : std::string();
}

bool nearValue (const ImportPlan& p, const char* id, double expected, double tolerance = 1.0e-3)
{
    return std::abs ((double) valueOf (p, id) - expected) <= tolerance;
}

bool inChain (const ImportPlan& p, ps::ModuleType t)
{
    return std::find (p.order.begin(), p.order.end(), t) != p.order.end();
}

// Each check save is named after what the user set ("<group>-<what>.xml"); the expected result is read from the name
// in our words (shape, style and detection names are the same as ours). Groups: F format (power, solo, L/R, a moved
// band, I/O), E1 band shapes (band 2), E6 band phase, D detection and knee, M limiter algorithm and style, R Reset.
void runCheckSaveTests()
{
    std::cout << "== importer: real-product check saves\n";
    const auto folder = findCheckSaves();
    if (folder == juce::File())
    {
        std::cout << "  SKIP no local folder of check saves (ignore_*/checks or PURESHEEN_IMPORT_CHECKS); nothing compared\n";
        return;
    }

    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.xml");
    struct Less
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            return a.getFileName().compareNatural (b.getFileName());
        }
    } less;
    files.sort (less);

    int compared = 0;
    for (const auto& file : files)
    {
        const auto name = file.getFileNameWithoutExtension();
        const auto label = "check save " + name.toStdString();
        const auto plan = psplug::planXmlPresetFile (file);
        ++compared;

        if (name.startsWith ("F-d-"))
        {
            // Input / output and mono are not stored; the save holds an empty chain: refused as holding no module.
            check (! plan.ok && plan.holdsNoModules, label + ": empty chain refused as holding no module", plan.error);
            continue;
        }
        if (! plan.ok)
        {
            check (false, label + ": imports", plan.error);
            continue;
        }
        check (plan.report.unmapped == 0 && inRange (plan), label + ": every stored value imported, all in range",
               std::to_string (plan.report.unmapped) + " not imported");

        if (name.startsWith ("F-a-"))
            check (inChain (plan, ps::ModuleType::Equalizer1) && nearValue (plan, "eq1.on", 0.0)
                       && nearValue (plan, "max.on", 1.0),
                   label + ": the module's power switch turns the equalizer off (the next module stays on)");
        else if (name.startsWith ("F-a2-"))
            check (nearValue (plan, "eq1.on", 1.0) && nearValue (plan, "max.on", 1.0),
                   label + ": solo is not stored, both modules on");
        else if (name.startsWith ("F-b-"))
            check (nearValue (plan, "eq1.chMode", (double) ps::ChannelMode::LeftRight)
                       && nearValue (plan, "eq1.b.b1.on", 1.0) && nearValue (plan, "eq1.a.b1.on", 0.0)
                       && std::abs (valueOf (plan, "eq1.b.b1.gain")) > 0.5f,
                   label + ": L/R mode, the second-channel band lands in set B",
                   "chMode " + std::to_string (valueOf (plan, "eq1.chMode")));
        else if (name.startsWith ("F-c-"))
            check (nearValue (plan, "eq1.a.b2.on", 1.0) && std::abs (valueOf (plan, "eq1.a.b2.gain")) > 0.0f,
                   label + ": a control moved and set back near its default is imported");
        else if (name.startsWith ("E1-"))
        {
            // "E1-<family> - <variant>": the bell family names the bell variants alone.
            const auto family = name.fromFirstOccurrenceOf ("E1-", false, false).upToFirstOccurrenceOf (" - ", false, false);
            const auto variant = name.fromFirstOccurrenceOf (" - ", false, false);
            const auto expected = (family == "Bell" ? variant : family + " " + variant).toStdString();
            const auto got = choiceLabel (plan, "eq1.a.b2.shape");
            check (got == expected && nearValue (plan, "eq1.a.b2.on", 1.0), label + ": band 2 shape " + expected, got);
        }
        else if (name.startsWith ("E6-"))
        {
            const double percent = name.fromLastOccurrenceOf (" ", false, false).getDoubleValue();
            check (nearValue (plan, "eq1.a.b1.phase", percent, 0.01) && nearValue (plan, "eq1.mode", 1.0),
                   label + ": band 1 phase " + std::to_string ((int) percent) + " % (digital mode)",
                   std::to_string (valueOf (plan, "eq1.a.b1.phase")));
        }
        else if (name.startsWith ("D-"))
        {
            const auto detect = choiceLabel (plan, "dyn.detect");
            if (name.endsWith ("-Env"))
                check (detect == "Envelope", label + ": envelope detection", detect);
            else if (name.endsWith ("-Peak"))
                check (detect == "Peak" && nearValue (plan, "dyn.a.b1.comp.knee", 10.0)
                           && nearValue (plan, "dyn.a.b1.lim.knee", 10.0),
                       label + ": peak detection (nothing stored), both knees at the default 10", detect);
            else if (name.endsWith ("-RMS"))
                check (detect == "RMS", label + ": RMS detection", detect);
            else if (name.contains ("knee-comp"))
            {
                const double knee = name.fromLastOccurrenceOf ("-", false, false).getDoubleValue();
                check (nearValue (plan, "dyn.a.b1.comp.knee", knee, 0.05) && nearValue (plan, "dyn.a.b1.lim.knee", 10.0),
                       label + ": compressor knee " + std::to_string ((int) knee), std::to_string (valueOf (plan, "dyn.a.b1.comp.knee")));
            }
            else if (name.contains ("knee-lim"))
            {
                const double knee = name.fromLastOccurrenceOf ("-", false, false).getDoubleValue();
                check (nearValue (plan, "dyn.a.b1.lim.knee", knee, 0.05), label + ": limiter knee " + std::to_string ((int) knee),
                       std::to_string (valueOf (plan, "dyn.a.b1.lim.knee")));
            }
        }
        else if (name.startsWith ("M-"))
        {
            // "M-<algorithm> [<n>] [- <style>]": n 1..5 = our Smooth .. Multiband; low latency named in words.
            using M = ps::MaximizerMode;
            const auto rest = name.fromFirstOccurrenceOf ("M-", false, false);
            const auto algorithm = rest.upToFirstOccurrenceOf (" - ", false, false);
            const auto style = rest.contains (" - ") ? rest.fromFirstOccurrenceOf (" - ", false, false) : juce::String();
            const int n = algorithm.retainCharacters ("0123456789").getIntValue();
            const M modes[] { M::Smooth, M::Transparent, M::Aggressive, M::Spectral, M::Multiband };
            const M expected = algorithm.containsIgnoreCase ("low latency") ? M::LowLatency
                                                                            : (n >= 1 && n <= 5 ? modes[n - 1] : M::Count);
            bool ok = nearValue (plan, "max.mode", (double) expected);
            std::string detail = choiceLabel (plan, "max.mode");
            if (expected == M::Aggressive)
            {
                ok = ok && choiceLabel (plan, "max.style3") == style.toStdString();
                detail += " / " + choiceLabel (plan, "max.style3");
            }
            else if (expected == M::Spectral)
            {
                ok = ok && choiceLabel (plan, "max.style4") == style.toStdString();
                detail += " / " + choiceLabel (plan, "max.style4");
            }
            check (ok, label + ": limiter mode" + (style.isEmpty() ? std::string() : " and style " + style.toStdString()),
                   detail);
        }
        else if (name.startsWith ("R-"))
        {
            // Reset saves: the module at its defaults, on.
            bool on = ! plan.order.empty();
            for (const auto t : plan.order)
                on = on && nearValue (plan, (std::string (ps::moduleInfo (t).idPrefix) + ".on").c_str(), 1.0);
            check (on, label + ": every module of the chain on");
            if (name == "R-Dynamics")
                check (nearValue (plan, "dyn.bands", 1.0) && nearValue (plan, "dyn.a.b1.comp.thr", -12.0)
                           && nearValue (plan, "dyn.a.b1.comp.ratio", 2.0) && nearValue (plan, "dyn.a.b1.comp.rel", 60.0)
                           && nearValue (plan, "dyn.a.b1.comp.knee", 10.0) && nearValue (plan, "dyn.a.b1.lim.att", 20.0)
                           && choiceLabel (plan, "dyn.detect") == "Peak",
                       label + ": Dynamics Reset defaults (one band, -12 dB 2:1 20 / 60 ms knee 10, peak)");
            else if (name == "R-Maximizer")
                check (choiceLabel (plan, "max.mode") == "Smooth" && choiceLabel (plan, "max.style3") == "Balanced"
                           && choiceLabel (plan, "max.style4") == "Modern" && nearValue (plan, "max.character", 2.0)
                           && nearValue (plan, "max.clip.amount", 0.0) && nearValue (plan, "max.upward", 0.0),
                       label + ": Maximizer Reset defaults");
            else if (name == "R-Match EQ")
                check (nearValue (plan, "meq.a.amount", 10.0) && nearValue (plan, "meq.a.smooth", 50.0),
                       label + ": Match EQ Reset defaults (amount 10 %, smoothing 50 %)");
        }
    }
    std::cout << "  " << compared << " check saves compared (" << folder.getFullPathName() << ")\n";
}
} // namespace

int runImportTests()
{
    failures = 0;
    failures += runImportProcessorTests();
    runChooserTests();
    std::cout << "== importer: text path\n";

    {
        const auto plan = psplug::planXmlPresetText (chainFileText(), testOptions());
        check (plan.ok, "import: a whole-chain file parses (entities, CRLF, decimal commas)", plan.error);
        check (plan.order == std::vector<ps::ModuleType> { ps::ModuleType::Equalizer1, ps::ModuleType::Imager, ps::ModuleType::Maximizer },
               "import: our chain follows the file's chain order, skipped module dropped");
        check (plan.report.skipped == std::vector<std::string> { "mix rebalancing" }, "import: skipped module in our words");
        check (std::abs (valueOf (plan, "img.xover1") - 250.0f) < 0.01f && std::abs (valueOf (plan, "img.b2.width") - 40.5f) < 0.01f,
               "import: decimal-comma values", std::to_string (valueOf (plan, "img.xover1")));
        check (std::abs (valueOf (plan, "img.b.b1.width") + 20.0f) < 0.01f, "import: set B prefix -> set B");
        check (valueOf (plan, "img.chMode") == 1.0f, "import: channel mode");
        check (plan.report.mapped == 5 && plan.report.unmapped == 1, "import: counts (one unknown control)",
               std::to_string (plan.report.mapped) + " / " + std::to_string (plan.report.unmapped));
        check (ps::imp::summariseReport (plan) == "Imported 3 modules, 5 settings (1 not imported). Skipped: mix rebalancing.",
               "import: summary line", ps::imp::summariseReport (plan));
    }

    {
        int crashesAvoided = 0, okPlans = 0, badRange = 0;
        const auto text = chainFileText();
        const auto bytes = text.toStdString();

        // Every truncation of the file.
        for (size_t n = 0; n < bytes.size(); n += 3)
        {
            const auto plan = psplug::planXmlPresetText (juce::String::fromUTF8 (bytes.data(), (int) n), testOptions());
            ++crashesAvoided;
            if (plan.ok && ! inRange (plan))
                ++badRange;
        }

        // Random byte mutations (deterministic LCG).
        std::uint32_t seed = 99u;
        for (int round = 0; round < 3000; ++round)
        {
            auto m = bytes;
            const int edits = 1 + (int) (seed % 8u);
            for (int e = 0; e < edits; ++e)
            {
                seed = seed * 1664525u + 1013904223u;
                const size_t at = (seed >> 8) % m.size();
                const char pool[] = "<>/\"'=&;#x0123456789-.,eE AuxBand \r\n\xff\x01";
                m[at] = pool[(seed >> 3) % (sizeof (pool) - 1)];
            }
            const auto plan = psplug::planXmlPresetText (juce::String::fromUTF8 (m.data(), (int) m.size()), testOptions());
            ++crashesAvoided;
            if (plan.ok)
            {
                ++okPlans;
                if (! inRange (plan))
                    ++badRange;
            }
        }
        check (badRange == 0, "import: fuzz (every truncation + 3000 mutations): no crash, every value in range",
               std::to_string (crashesAvoided) + " inputs, " + std::to_string (okPlans) + " mutated plans imported");

        juce::String deep;
        for (int i = 0; i < 100000; ++i)
            deep << "<a>";
        check (! psplug::planXmlPresetText (deep).ok, "import: 100000 nested elements refused (no stack overflow)");
        const auto hiddenDepth = "<a <!-- ' --> >" + deep + juce::String::repeatedString ("</a>", 100000)
                                 + "<!-- ' --></a>";
        check (! psplug::planXmlPresetText (hiddenDepth).ok,
               "import: comments inside a start tag cannot conceal recursive XML children");

        const juce::String bomb = "<!DOCTYPE r [<!ENTITY a \"aaaaaaaaaa\"><!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;\">]>"
                                  "<Root PresetVer=\"1\" PluginVer=\"1\">&b;</Root>";
        check (! psplug::planXmlPresetText (bomb).ok, "import: entity declarations refused");
        check (! psplug::planXmlPresetText ("").ok && ! psplug::planXmlPresetText ("not xml at all").ok
                   && ! psplug::planXmlPresetText ("<Root/>").ok,
               "import: empty / non-XML / rootless text refused with a message");
        check (! psplug::planXmlPresetText (juce::String::fromUTF8 ("\x89PNG\r\n\x1a\n\0\0\0", 11)).ok, "import: binary data refused");
    }

    std::cout << "== importer: processor\n";
    {
        Proc proc;
        auto& p = proc.p;
        p.flushPendingChanges (true);
        const auto historyBefore = p.getHistory().size();
        const auto chainBefore = p.getChainOrder();
        const float widthBefore = p.getParameterValue ("img.b2.width");

        auto plan = psplug::planXmlPresetText (chainFileText(), testOptions());
        const auto expectedOrder = plan.order;
        const auto result = psplug::applyImportPlan (p, std::move (plan), "My Imported", -1);
        p.flushPendingChanges (true);
        check (result.ok, "import: a whole chain applies", result.message.toStdString());
        check (p.getHistory().size() == historyBefore + 1, "import: one undo step",
               std::to_string (p.getHistory().size() - historyBefore) + " steps");
        check (p.getHistory().back().description == "Imported preset My Imported", "import: history entry text",
               p.getHistory().back().description.toStdString());
        check (p.getChainOrder() == expectedOrder, "import: the chain is replaced in the file's order");
        check (std::abs (p.getParameterValue ("img.b2.width") - 40.5f) < 0.01f, "import: values reach the parameters");
        check (p.getCurrentPresetName() == "My Imported" && p.isPresetModified(), "import: shows as an unsaved preset named after the file");
        check (result.message.startsWith ("\"My Imported\": Imported 3 modules"), "import: result note", result.message.toStdString());
        check (proc.playsFinite (40), "import: the imported state plays finite audio");

        p.undo();
        p.flushPendingChanges (true);
        check (p.getChainOrder() == chainBefore && std::abs (p.getParameterValue ("img.b2.width") - widthBefore) < 1.0e-4f,
               "import: one undo restores the chain and every value");
        p.redo();
        p.flushPendingChanges (true);
        check (p.getChainOrder() == expectedOrder, "import: redo re-applies it");

        // A single-module file for a module that is not in the chain: inserted (our order), switched on, one step.
        p.setChainOrder ({ ps::ModuleType::Equalizer1, ps::ModuleType::Dynamics, ps::ModuleType::Maximizer });
        p.setParameterValue ("img.on", 0.0f);
        p.flushPendingChanges (true);
        const auto nameBefore = p.getCurrentPresetName();
        const auto stepsBefore = p.getHistory().size();
        auto modulePlan = psplug::planXmlPresetText (moduleFileText ("110000"), testOptions());
        const auto r2 = psplug::applyImportPlan (p, std::move (modulePlan), "Wide Sides", -1);
        p.flushPendingChanges (true);
        check (r2.ok && p.getHistory().size() == stepsBefore + 1, "import: module file = one undo step", r2.message.toStdString());
        check (p.getChainOrder() == std::vector<ps::ModuleType> { ps::ModuleType::Equalizer1, ps::ModuleType::Dynamics,
                                                                   ps::ModuleType::Imager, ps::ModuleType::Maximizer },
               "import: the module is inserted at its usual place");
        check (p.getParameterValue ("img.on") == 1.0f, "import: an inserted module is switched on");
        check (std::abs (p.getParameterValue ("img.b1.width") - 55.0f) < 0.01f, "import: the module's values are applied");
        check (p.getCurrentPresetName() == nameBefore, "import: a module file keeps the preset name");
        check (r2.message == "Imported Imager, 1 setting.", "import: module result note", r2.message.toStdString());
        check (proc.playsFinite (20), "import: module import plays finite audio");
        p.undo();
        p.flushPendingChanges (true);
        check (! p.isModuleInChain (ps::ModuleType::Imager), "import: undo removes the inserted module again");

        // From a module's own preset menu: only its own module files.
        const auto stepsNow = p.getHistory().size();
        const auto wrong = psplug::applyImportPlan (p, psplug::planXmlPresetText (moduleFileText ("110000"), testOptions()),
                                                    "x", (int) ps::ModuleType::Dynamics);
        const auto chain = psplug::applyImportPlan (p, psplug::planXmlPresetText (chainFileText(), testOptions()), "x",
                                                    (int) ps::ModuleType::Imager);
        p.flushPendingChanges (true);
        check (! wrong.ok && wrong.message.contains ("Imager") && ! chain.ok && p.getHistory().size() == stepsNow,
               "import: a module menu refuses other modules' files and whole chains (no step)",
               wrong.message.toStdString() + " | " + chain.message.toStdString());

        // From a file on disk: the name is the file name.
        const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("PureSheen import test.xml");
        temp.replaceWithText (chainFileText());
        const auto fromFile = psplug::importXmlPreset (p, temp, -1, testOptions());
        p.flushPendingChanges (true);
        temp.deleteFile();
        check (fromFile.ok && p.getCurrentPresetName() == "PureSheen import test", "import: from a file, named after it",
               fromFile.message.toStdString());
        check (! psplug::importXmlPreset (p, temp, -1).ok, "import: a missing file is reported");
        auto binaryTail = chainFileText().toStdString();
        binaryTail.push_back ('\0');
        binaryTail += "<Ignored/>";
        temp.replaceWithData (binaryTail.data(), binaryTail.size());
        const auto beforeRefused = p.getHistory().size();
        const auto beforeRefusedOrder = p.getChainOrder();
        const auto nulResult = psplug::importXmlPreset (p, temp, -1, testOptions());
        temp.deleteFile();
        auto damagedOrderText = juce::String (kChainFile).replace ("%CHAIN%", "AAAAAA==");
        const auto damagedOrder = psplug::applyImportPlan (p, psplug::planXmlPresetText (damagedOrderText, testOptions()), "Broken");
        p.flushPendingChanges (true);
        check (! nulResult.ok && ! damagedOrder.ok && p.getHistory().size() == beforeRefused
                   && p.getChainOrder() == beforeRefusedOrder,
               "import: embedded NUL and damaged chain are refused without a state or undo change");

    }

    std::cout << "== importer: Match EQ reference spectrum\n";
    {
        Proc proc;
        auto& p = proc.p;
        p.flushPendingChanges (true);
        auto reference = [&p] (int part)
        {
            return static_cast<ps::MatchEq&> (p.getChain().getModule (ps::ModuleType::MatchEq, part))
                .getSpectrum (ps::MatchRole::Reference);
        };
        check (! reference (0).valid, "import: no reference before the import");

        // slot 2 of three records (1 empty, 2 the curve, 3 empty)
        const auto data = spectrumData ({ spectrumRecord (64, false), spectrumRecord (2049, true), spectrumRecord (64, false) });
        auto plan = psplug::planXmlPresetText (matchFileText (2, data), testOptions());
        check (plan.ok && plan.matchReference != nullptr && plan.matchReference->valid,
               "import: the flagged record decodes into a reference", plan.error);
        check (plan.report.mapped == 2 && plan.report.unmapped == 0 && plan.report.attachments.empty(),
               "import: the spectrum flag counts as imported", std::to_string (plan.report.mapped) + " mapped, "
                   + std::to_string (plan.report.unmapped) + " unmapped");
        if (plan.matchReference != nullptr)
        {
            // -3 dB per octave per bin of a linear grid stays -3 dB per octave as a density (constant bin width)
            const auto& r = *plan.matchReference;
            const float at1k = r.psdDb[0][(int) std::lround (48.0 * std::log2 (1000.0 / 10.0))];
            const float at2k = r.psdDb[0][(int) std::lround (48.0 * std::log2 (2000.0 / 10.0))];
            check (std::abs ((at1k - at2k) - 3.0f) < 0.3f && r.psdDb[0][300] == r.psdDb[2][300]
                       && r.psdDb[1][300] == r.psdDb[0][300] && r.psdDb[3][300] == ps::kMatchNoValue,
                   "import: the curve keeps its shape (L = R = M, no side)",
                   std::to_string (at1k) + " / " + std::to_string (at2k));
        }

        const auto steps = p.getHistory().size();
        const auto result = psplug::applyImportPlan (p, std::move (plan), "Pink Target", -1);
        p.flushPendingChanges (true);
        check (result.ok && p.isModuleInChain (ps::ModuleType::MatchEq) && p.getHistory().size() == steps + 1,
               "import: Match EQ file with its reference = one undo step", result.message.toStdString());
        check (reference (0).valid && reference (1).valid && juce::String (reference (0).source.data()) == "import:Pink Target",
               "import: both parts get the reference, named after the preset",
               juce::String (reference (0).source.data()).toStdString());
        check (std::abs (p.getParameterValue ("meq.a.amount") - 40.0f) < 0.01f, "import: Match EQ settings arrive with it");
        check (result.message.contains ("capture your track as Current"),
               "import: the note tells the user to capture the Current spectrum", result.message.toStdString());
        check (proc.playsFinite (40), "import: Match EQ with an imported reference plays finite audio");
        p.undo();
        p.flushPendingChanges (true);
        check (! reference (0).valid && ! reference (1).valid && ! p.isModuleInChain (ps::ModuleType::MatchEq),
               "import: one undo removes the reference and the inserted module");
        p.redo();
        p.flushPendingChanges (true);
        check (reference (0).valid, "import: redo restores the reference");

        // damaged records: never a crash, the settings still import, the flag counts as not imported with a note
        auto good = spectrumRecord (256, true);
        const juce::MemoryBlock cut (good.getData(), good.getSize() / 2);
        juce::MemoryBlock wrongSize (good);
        wrongSize[0] = (char) -1; wrongSize[1] = (char) -1; wrongSize[2] = (char) -1; wrongSize[3] = (char) 0x7f;
        juce::MemoryBlock garbage (good);
        for (size_t i = 8; i < garbage.getSize(); i += 7)
            garbage[i] = (char) (garbage[i] ^ 0x5a);
        juce::String deep = "{ \"used\" : true, \"levels\" : ";
        for (int i = 0; i < 200000; ++i)
            deep << "[";
        juce::MemoryOutputStream deepPacked;
        {
            juce::GZIPCompressorOutputStream z (deepPacked, 6, 0);
            z.write (deep.toRawUTF8(), deep.getNumBytesAsUTF8());
        }
        juce::MemoryOutputStream deepRecord;
        deepRecord.writeInt ((int) deep.getNumBytesAsUTF8());
        deepRecord.write (deepPacked.getData(), deepPacked.getDataSize());

        const juce::String smallJson = "{\"used\":true,\"levels\":[-30,-33,-36],\"hz\":[0,100,200]}";
        check (psplug::planXmlPresetText (matchFileText (1, spectrumData ({ packSpectrumJson (smallJson) })),
                                          testOptions()).matchReference != nullptr,
               "import: a small reference record is usable before hostile variants");
        {
            auto outOfRange = psplug::planXmlPresetText (matchFileText (1, spectrumData ({ packSpectrumJson (
                smallJson.replace ("-30,-33,-36", "2000,2000,2000")) })), testOptions());
            const bool attachmentRefused = outOfRange.ok && outOfRange.matchReference == nullptr
                                           && outOfRange.report.mapped == 1 && outOfRange.report.unmapped == 1;
            const auto applied = psplug::applyImportPlan (p, std::move (outOfRange), "Damaged target");
            check (attachmentRefused && applied.ok,
                   "import: an out-of-range reference is reported while usable settings still import");

            const auto oneSlot = matchFileText (1, spectrumData ({ packSpectrumJson (smallJson) }));
            const auto auxOnly = oneSlot.replace ("ParamID=\"Slot 1 Used\"", "ParamID=\"Aux: Slot 1 Used\"");
            const auto bothSlots = oneSlot.replace ("<ExtraBytes",
                "<Param ElementID=\"Picture\" ParamID=\"Aux: Slot 1 Used\" Value=\"1\"/><ExtraBytes");
            const auto auxPlan = psplug::planXmlPresetText (auxOnly, testOptions());
            const auto bothPlan = psplug::planXmlPresetText (bothSlots, testOptions());
            check (auxPlan.ok && auxPlan.matchReference != nullptr && auxPlan.report.mapped == 2
                       && bothPlan.ok && bothPlan.matchReference != nullptr && bothPlan.report.mapped == 3
                       && auxPlan.report.unmapped == 0 && bothPlan.report.unmapped == 0,
                   "import: Aux-only and matching A/B reference flags decode the shared attachment");
            const auto brokenBoth = bothSlots.replace (spectrumData ({ packSpectrumJson (smallJson) }),
                                                       spectrumData ({ packSpectrumJson ("{}") }));
            const auto brokenPlan = psplug::planXmlPresetText (brokenBoth, testOptions());
            check (brokenPlan.ok && brokenPlan.matchReference == nullptr && brokenPlan.report.mapped == 1
                       && brokenPlan.report.unmapped == 2 && brokenPlan.report.unmappedList.size() == 2,
                   "import: damaged shared reference records report both A/B flags as not imported");
        }
        {
            auto padded = packSpectrumJson (smallJson);
            const juce::MemoryBlock zeros (818, true);   // the writer zero-pads each record after its stream
            padded.append (zeros.getData(), zeros.getSize());
            check (psplug::planXmlPresetText (matchFileText (1, spectrumData ({ padded })), testOptions()).matchReference
                       != nullptr,
                   "import: zero padding after a record's stream is accepted");
        }
        juce::String hiddenDepth = "{\"padding\":'";
        hiddenDepth += juce::String::repeatedString ("]", 20000);
        hiddenDepth += "',\"nested\":" + juce::String::repeatedString ("[", 20000) + "0"
                       + juce::String::repeatedString ("]", 20000) + "}";
        const juce::MemoryBlock noChecksum (good.getData(), good.getSize() - 4);
        juce::MemoryBlock trailing (good);
        trailing.append ("junk", 4);

        int refused = 0, total = 0;
        const std::vector<juce::String> damaged {
            spectrumData ({ cut }), spectrumData ({ wrongSize }), spectrumData ({ garbage }),
            spectrumData ({ noChecksum }), spectrumData ({ trailing }),
            spectrumData ({ packSpectrumJson (hiddenDepth) }),
            spectrumData ({ packSpectrumJson (smallJson.replace ("true", "1")) }),
            spectrumData ({ spectrumRecord (2049, true, 1000, "4000") }),
            spectrumData ({ packSpectrumJson (smallJson.replace ("-33", "9999999999999999999999999")) }),
            spectrumData ({ packSpectrumJson (smallJson.replace ("100,200", "-100,200")) }),
            spectrumData ({ packSpectrumJson (smallJson.replace ("100,200", "100,0")) }),
            spectrumData ({ packSpectrumJson (smallJson + "{}") }),
            spectrumData ({ deepRecord.getMemoryBlock() }), spectrumData ({ spectrumRecord (256, false) }),
            spectrumData ({ spectrumRecord (256, true, 5, "\"text\"") }), spectrumData ({ spectrumRecord (1, true) }),
            spectrumData ({ spectrumRecord (256, true) }).substring (0, 40), "AAAAAA==", ""
        };
        for (const auto& d : damaged)
        {
            ++total;
            const auto bad = psplug::planXmlPresetText (matchFileText (1, d), testOptions());
            if (bad.ok && bad.matchReference == nullptr && bad.report.unmapped == 1 && bad.report.mapped == 1
                && ! bad.report.notes.empty())
                ++refused;
        }
        check (refused == total, "import: damaged / hostile spectrum records are reported, never applied",
               std::to_string (refused) + " of " + std::to_string (total));
        const auto noSlot = psplug::planXmlPresetText (matchFileText (3, data), testOptions());
        check (noSlot.ok && noSlot.matchReference == nullptr, "import: a flag for an empty / missing record is not applied");
    }

    runCheckSaveTests();

    std::cout << "\nimporttest: " << (failures == 0 ? "all passed" : std::to_string (failures) + " FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
