#include "plugin/MatchEqState.h"

#include "dsp/Chain.h"
#include "dsp/MatchEq.h"
#include "plugin/UndoHistory.h"

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>

// Match EQ parts of the processor (see MatchEqState.h). Choices made where the contract does not decide:
// - Only valid spectra are stored; an invalid one (cleared, or a capture shorter than 1 s) counts as empty.
// - seconds / sampleRate are written as the shortest decimal that round-trips the double exactly (std::to_chars), the
//   PSD arrays as base64 of little-endian float32, so a state round trip is bit-exact.
// - A preset load replaces the References only when the preset carries a "MatchEq" child (see the header).
// - Undo descriptions: "Match EQ: captured the reference", "... captured the current spectrum", "... cleared the
//   reference", "... loaded reference <file name>", "... changed the reference" (any other source), with
//   " (Sustain)" for the T/S twin's spectra and "Match EQ: changed the spectra" when several changed at once.
// - Loader errors (plain text): "The file could not be found.", "This file type is not supported, or the file is
//   damaged.", "The file contains no audio.", "The audio in the file could not be read.", "The file has less than one
//   second of audible audio (above -70 dBFS)." A cancelled load is not an error.
namespace psplug
{
namespace
{
    const juce::Identifier kMatchEqTag ("MatchEq"), kSpectrumTag ("Spectrum"), kVersion ("version"), kPart ("part"),
        kRole ("role"), kSource ("source"), kSeconds ("seconds"), kSampleRate ("sampleRate"), kValid ("valid");
    const juce::Identifier kChannelIds[4] { "L", "R", "M", "S" };
    constexpr int kVersionValue = 1;
    constexpr double kMaxFileSeconds = 20.0 * 60.0;
    constexpr int kChunk = 65536;

    juce::String encodeFloats (const std::array<float, ps::kMatchPoints>& values)
    {
        juce::MemoryOutputStream raw;
        for (float v : values)
            raw.writeFloat (v);   // little endian
        juce::MemoryOutputStream text;
        juce::Base64::convertToBase64 (text, raw.getData(), raw.getDataSize());
        return text.toString();
    }

    bool decodeFloats (const juce::String& text, std::array<float, ps::kMatchPoints>& values)
    {
        juce::MemoryOutputStream raw;
        if (! juce::Base64::convertFromBase64 (raw, text) || raw.getDataSize() != sizeof (float) * (size_t) ps::kMatchPoints)
            return false;
        juce::MemoryInputStream in (raw.getData(), raw.getDataSize(), false);
        for (auto& v : values)
        {
            const float f = in.readFloat();
            v = std::isfinite (f) ? std::max (f, ps::kMatchNoValue) : ps::kMatchNoValue;
        }
        return true;
    }

    juce::String exactDouble (double v)
    {
        char buf[64] {};
        const auto r = std::to_chars (buf, buf + sizeof (buf) - 1, v);
        *r.ptr = 0;
        return juce::String (buf);
    }

    double parseDouble (const juce::var& v)
    {
        const auto s = v.toString().toStdString();
        if (s.empty())
            return 0.0;
        double d = 0.0;
        const auto result = std::from_chars (s.data(), s.data() + s.size(), d);
        return result.ec == std::errc() && result.ptr == s.data() + s.size() && std::isfinite (d) ? d : 0.0;
    }

    std::string asciiTag (const juce::String& s)
    {
        std::string out;
        for (auto c : s)
            out += (c >= 32 && c < 127) ? (char) c : '_';
        return out;
    }

    juce::String sourceOf (const ps::MatchSpectrum& s)
    {
        return juce::String (juce::CharPointer_UTF8 (s.source.data()), (size_t) strnlen (s.source.data(), s.source.size()));
    }

    juce::String describeChange (const MatchSpectraSet& from, const MatchSpectraSet& to)
    {
        int changed = 0, part = 0, role = 0;
        for (int p = 0; p < 2; ++p)
            for (int r = 0; r < 2; ++r)
                if (! sameSpectrum (from.spectra[p][r], to.spectra[p][r]))
                {
                    ++changed;
                    part = p;
                    role = r;
                }
        if (changed != 1)
            return "Match EQ: changed the spectra";

        const auto& s = to.spectra[part][role];
        const juce::String what = role == 0 ? "the reference" : "the current spectrum";
        const auto src = sourceOf (s);
        juce::String d;
        if (! s.valid)
            d = "Match EQ: cleared " + what;
        else if (src.startsWith ("file:"))
            d = "Match EQ: loaded reference " + src.fromFirstOccurrenceOf ("file:", false, false);
        else if (src == "capture")
            d = "Match EQ: captured " + what;
        else
            d = "Match EQ: changed " + what;
        return part == 1 ? d + " (Sustain)" : d;
    }
} // namespace

//==============================================================================
bool sameSpectrum (const ps::MatchSpectrum& a, const ps::MatchSpectrum& b) noexcept
{
    if (! a.valid && ! b.valid)
        return true;
    if (a.valid != b.valid || a.seconds != b.seconds || a.sampleRate != b.sampleRate)
        return false;
    if (std::strncmp (a.source.data(), b.source.data(), a.source.size()) != 0)
        return false;
    for (int c = 0; c < 4; ++c)
        if (std::memcmp (a.psdDb[c].data(), b.psdDb[c].data(), sizeof (float) * (size_t) ps::kMatchPoints) != 0)
            return false;
    return true;
}

ps::MatchSpectrum emptySpectrum() noexcept
{
    ps::MatchSpectrum s;
    for (auto& ch : s.psdDb)
        ch.fill (ps::kMatchNoValue);
    return s;
}

juce::ValueTree matchSpectraToTree (const MatchSpectraSet& set, bool includeCurrent)
{
    juce::ValueTree tree (kMatchEqTag);
    tree.setProperty (kVersion, kVersionValue, nullptr);
    for (int part = 0; part < 2; ++part)
        for (int role = 0; role < (includeCurrent ? 2 : 1); ++role)
        {
            const auto& s = set.spectra[part][role];
            if (! s.valid)
                continue;
            juce::ValueTree t (kSpectrumTag);
            t.setProperty (kPart, part, nullptr);
            t.setProperty (kRole, role == 0 ? "reference" : "current", nullptr);
            t.setProperty (kSource, sourceOf (s), nullptr);
            t.setProperty (kSeconds, exactDouble (s.seconds), nullptr);
            t.setProperty (kSampleRate, exactDouble (s.sampleRate), nullptr);
            t.setProperty (kValid, 1, nullptr);
            for (int c = 0; c < 4; ++c)
                t.setProperty (kChannelIds[c], encodeFloats (s.psdDb[c]), nullptr);
            tree.appendChild (t, nullptr);
        }
    return tree;
}

bool matchSpectraFromTree (const juce::ValueTree& tree, MatchSpectraSet& set, bool includeCurrent)
{
    for (int part = 0; part < 2; ++part)
        for (int role = 0; role < (includeCurrent ? 2 : 1); ++role)
            set.spectra[part][role] = emptySpectrum();
    if (! tree.isValid())
        return true;
    if (! tree.hasType (kMatchEqTag) || (int) tree.getProperty (kVersion, 0) != kVersionValue)
        return false;

    for (const auto& t : tree)
    {
        if (! t.hasType (kSpectrumTag))
            continue;
        const int part = (int) t.getProperty (kPart, -1);
        const auto roleName = t.getProperty (kRole).toString();
        const int role = roleName == "reference" ? 0 : (roleName == "current" ? 1 : -1);
        if (part < 0 || part > 1 || role < 0 || (role == 1 && ! includeCurrent))
            continue;

        ps::MatchSpectrum s = emptySpectrum();
        bool ok = true;
        for (int c = 0; c < 4 && ok; ++c)
            ok = decodeFloats (t.getProperty (kChannelIds[c]).toString(), s.psdDb[c]);
        if (! ok)
            continue;
        s.valid = (int) t.getProperty (kValid, 1) != 0;
        s.seconds = parseDouble (t.getProperty (kSeconds));
        s.sampleRate = parseDouble (t.getProperty (kSampleRate));
        ps::setMatchSource (s, asciiTag (t.getProperty (kSource).toString()).c_str());
        set.spectra[part][role] = s;
    }
    return true;
}

bool factoryMatchReference (const juce::String& name, ps::MatchSpectrum& out)
{
    if (name == "Pink Balance")
        out = ps::makeMatchTiltSpectrum (-3.0, 0.0, 60.0, "synth:Pink Balance");
    else if (name == "Warm Tilt")
        out = ps::makeMatchTiltSpectrum (-4.5, 0.0, 60.0, "synth:Warm Tilt");
    else if (name == "Bright Tilt")
        out = ps::makeMatchTiltSpectrum (-2.0, -2.0, 60.0, "synth:Bright Tilt");
    else
        return false;
    return true;
}

//==============================================================================
class MatchEqState::Loader final : public juce::Thread
{
public:
    Loader() : juce::Thread ("PureSheen Match EQ reference file") { formats.registerBasicFormats(); }
    ~Loader() override { cancel(); }

    bool start (const juce::File& file, int part, bool linked)
    {
        cancel();
        state.store (kIdle);
        progress.store (-1.0f);
        setError ({});

        if (! file.existsAsFile())
            return fail ("The file could not be found.");
        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (file));
        if (r == nullptr)
            return fail ("This file type is not supported, or the file is damaged.");
        if (r->lengthInSamples <= 0 || r->numChannels == 0 || ! (r->sampleRate > 0.0))
            return fail ("The file contains no audio.");

        reader = std::move (r);
        targetPart = juce::jlimit (0, 1, part);
        linkedParts = linked;
        name = file.getFileNameWithoutExtension();
        progress.store (0.0f);
        state.store (kRunning);
        if (startThread (juce::Thread::Priority::low))
            return true;
        reader.reset();
        state.store (kIdle);
        progress.store (-1.0f);
        return fail ("The reference file loader could not be started.");
    }

    void cancel()
    {
        // A timed stop force-terminates a slow decoder, potentially while it owns resultLock. Always join before
        // reusing / destroying its state, and discard a result even if it finished just before cancellation.
        stopThread (-1);
        reader.reset();
        state.store (kIdle);
        progress.store (-1.0f);
        setError ({});
    }

    bool takeResult (int& part, bool& linked, ps::MatchSpectrum& out)
    {
        if (state.load() != kDone)
            return false;
        stopThread (-1);   // publication precedes run() returning; join before reusing its data
        {
            const juce::ScopedLock sl (resultLock);
            out = result;
        }
        part = targetPart;
        linked = linkedParts;
        state.store (kIdle);
        progress.store (-1.0f);
        return true;
    }

    float getProgress() const { return state.load() == kRunning ? progress.load() : -1.0f; }

    juce::String getError() const
    {
        const juce::ScopedLock sl (resultLock);
        return error;
    }

    void run() override
    {
        auto r = std::move (reader);
        const double sr = r->sampleRate;
        const auto total = std::min<juce::int64> (r->lengthInSamples, (juce::int64) std::floor (kMaxFileSeconds * sr));
        const bool stereo = r->numChannels > 1;

        analyser.prepare (sr);
        juce::AudioBuffer<float> buffer (2, kChunk);
        for (juce::int64 pos = 0; pos < total; pos += kChunk)
        {
            if (threadShouldExit())
            {
                state.store (kIdle);
                return;
            }
            const int n = (int) std::min<juce::int64> (kChunk, total - pos);
            buffer.clear();
            if (! r->read (&buffer, 0, n, pos, true, stereo))
            {
                finishWithError ("The audio in the file could not be read.");
                return;
            }
            analyser.push (buffer.getReadPointer (0), stereo ? buffer.getReadPointer (1) : nullptr, n);
            progress.store ((float) ((double) (pos + n) / (double) total));
        }

        ps::MatchSpectrum s;
        analyser.spectrum (s);
        if (threadShouldExit())
        {
            state.store (kIdle);
            return;
        }
        if (! s.valid)
        {
            finishWithError ("The file has less than one second of audible audio (above -70 dBFS).");
            return;
        }
        ps::setMatchSource (s, ("file:" + asciiTag (name)).c_str());
        {
            const juce::ScopedLock sl (resultLock);
            result = s;
        }
        state.store (kDone);
    }

private:
    static constexpr int kIdle = 0, kRunning = 1, kDone = 2;

    bool fail (const juce::String& message)
    {
        setError (message);
        return false;
    }
    void setError (const juce::String& message)
    {
        const juce::ScopedLock sl (resultLock);
        error = message;
    }
    void finishWithError (const juce::String& message)
    {
        setError (message);
        state.store (kIdle);
    }

    juce::AudioFormatManager formats;
    std::unique_ptr<juce::AudioFormatReader> reader;
    ps::MatchAnalyser analyser;
    int targetPart = 0;
    bool linkedParts = false;
    juce::String name;
    std::atomic<int> state { kIdle };
    std::atomic<float> progress { -1.0f };
    juce::CriticalSection resultLock;
    ps::MatchSpectrum result;
    juce::String error;
};

//==============================================================================
MatchEqState::MatchEqState (ps::Chain& c) : chain (c), loader (std::make_unique<Loader>())
{
    accepted = readModules();
    presetBaseline = accepted;
    noteModuleGenerations();
}

MatchEqState::~MatchEqState()
{
    loader.reset();   // joins the loader thread
}

ps::MatchEq& MatchEqState::module (int part) const
{
    return static_cast<ps::MatchEq&> (chain.getModule (ps::ModuleType::MatchEq, part));
}

MatchSpectraPtr MatchEqState::readModules() const
{
    const juce::ScopedLock sl (lock);
    auto set = std::make_shared<MatchSpectraSet>();
    for (int part = 0; part < 2; ++part)
        for (int role = 0; role < 2; ++role)
        {
            auto s = module (part).getSpectrum ((ps::MatchRole) role);
            set->spectra[part][role] = s.valid ? s : emptySpectrum();
        }
    return set;
}

void MatchEqState::noteModuleGenerations()
{
    for (int part = 0; part < 2; ++part)
        seenGeneration[part] = module (part).getSpectrumGeneration();
}

bool MatchEqState::applyToModules (const MatchSpectraSet& target, bool stopCaptures)
{
    bool changed = false;
    for (int part = 0; part < 2; ++part)
    {
        // A restored project, snapshot or history entry replaces the whole capture context. Stop before comparing
        // spectra, so even an unchanged stored spectrum cannot later be overwritten by audio from the old context.
        if (stopCaptures)
            module (part).stopCapture();
        for (int role = 0; role < 2; ++role)
        {
            auto& m = module (part);
            const auto r = (ps::MatchRole) role;
            if (sameSpectrum (m.getSpectrum (r), target.spectra[part][role]))
                continue;
            if (target.spectra[part][role].valid)
                m.setSpectrum (r, target.spectra[part][role]);
            else
                m.clearSpectrum (r);
            changed = true;
        }
    }
    noteModuleGenerations();
    return changed;
}

void MatchEqState::prune (const UndoHistory& history)
{
    for (auto it = tags.begin(); it != tags.end();)
    {
        bool present = false;
        for (int i = 0; i < history.size() && ! present; ++i)
            present = history[i].id == it->first;
        it = present ? std::next (it) : tags.erase (it);
    }
}

//==============================================================================
juce::ValueTree MatchEqState::createStateTree() const
{
    return matchSpectraToTree (*readModules(), true);
}

void MatchEqState::restoreStateTree (const juce::ValueTree& tree)
{
    const juce::ScopedLock sl (lock);
    loader->cancel();   // an old load must not overwrite a newly restored project
    MatchSpectraSet set;
    matchSpectraFromTree (tree, set, true);
    applyToModules (set);
    accepted = readModules();
    presetBaseline = accepted;
    pendingChange.reset();
}

void MatchEqState::storeSnapshot (int slot)
{
    if (slot < 0 || slot >= 4)
        return;
    const juce::ScopedLock sl (lock);
    snapshots[slot] = readModules();
}

void MatchEqState::storeSnapshotFromHistory (int slot, const UndoHistory& history, int historyIndex)
{
    if (slot < 0 || slot >= 4 || historyIndex < 0 || historyIndex >= history.size())
        return;
    const juce::ScopedLock sl (lock);
    const auto it = tags.find (history[historyIndex].id);
    snapshots[slot] = it != tags.end() ? it->second : readModules();
}

bool MatchEqState::recallSnapshot (int slot)
{
    if (slot < 0 || slot >= 4)
        return false;
    const juce::ScopedLock sl (lock);
    if (snapshots[slot] == nullptr)
        return false;
    loader->cancel();
    const bool changed = applyToModules (*snapshots[slot]);
    accepted = readModules();
    pendingChange.reset();
    return changed;
}

void MatchEqState::clearSnapshot (int slot)
{
    if (slot < 0 || slot >= 4)
        return;
    const juce::ScopedLock sl (lock);
    snapshots[slot].reset();
}

void MatchEqState::clearAllSnapshots()
{
    const juce::ScopedLock sl (lock);
    for (auto& s : snapshots)
        s.reset();
}

juce::ValueTree MatchEqState::snapshotTree (int slot) const
{
    const juce::ScopedLock sl (lock);
    if (slot < 0 || slot >= 4 || snapshots[slot] == nullptr)
        return {};
    return matchSpectraToTree (*snapshots[slot], true);
}

void MatchEqState::restoreSnapshotTree (int slot, const juce::ValueTree& tree)
{
    if (slot < 0 || slot >= 4)
        return;
    const juce::ScopedLock sl (lock);
    if (! tree.isValid())
    {
        snapshots[slot].reset();   // a snapshot without Match EQ data leaves the spectra alone when recalled
        return;
    }
    auto set = std::make_shared<MatchSpectraSet>();
    if (matchSpectraFromTree (tree, *set, true))
        snapshots[slot] = set;
    else
        snapshots[slot].reset();
}

//==============================================================================
bool MatchEqState::appendToPresetFile (const juce::File& file, int scope) const
{
    if (scope != -1 && scope != (int) ps::ModuleType::MatchEq)
        return true;
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr)
        return false;
    for (auto* old = xml->getChildByName (kMatchEqTag); old != nullptr; old = xml->getChildByName (kMatchEqTag))
        xml->removeChildElement (old, true);
    if (auto child = matchSpectraToTree (*readModules(), false).createXml())
        xml->addChildElement (child.release());
    return xml->writeTo (file);
}

void MatchEqState::beginPresetLoad (int scope, bool factory, const juce::String& name, const juce::File& file)
{
    const juce::ScopedLock sl (lock);
    presetPending = scope == -1 || scope == (int) ps::ModuleType::MatchEq;
    presetHasData = false;
    if (! presetPending)
        return;

    if (factory)
    {
        ps::MatchSpectrum ref;
        if (scope == (int) ps::ModuleType::MatchEq && factoryMatchReference (name, ref))
        {
            presetSpectra.spectra[0][0] = ref;
            presetSpectra.spectra[1][0] = ref;
            presetHasData = true;
        }
        return;
    }

    if (auto xml = juce::XmlDocument::parse (file))
        if (auto* child = xml->getChildByName (kMatchEqTag))
            presetHasData = matchSpectraFromTree (juce::ValueTree::fromXml (*child), presetSpectra, false);
}

void MatchEqState::beginImportedReference (const ps::MatchSpectrum& reference)
{
    const juce::ScopedLock sl (lock);
    presetPending = true;
    presetHasData = reference.valid;
    presetSpectra.spectra[0][0] = reference;
    presetSpectra.spectra[1][0] = reference;
}

bool MatchEqState::applyPendingPreset()
{
    const juce::ScopedLock sl (lock);
    const bool use = presetPending && presetHasData;
    presetPending = presetHasData = false;
    if (! use)
        return false;

    loader->cancel();   // a pending older file cannot supersede this explicit reference choice
    MatchSpectraSet target = *readModules();
    for (int part = 0; part < 2; ++part)
        target.spectra[part][0] = presetSpectra.spectra[part][0];
    // Presets replace references only. A Current capture remains useful, but an in-progress Reference capture must
    // not overwrite the explicitly selected preset later.
    for (int part = 0; part < 2; ++part)
    {
        ps::MatchRole role = ps::MatchRole::Reference;
        if (module (part).isCapturing (&role) && role == ps::MatchRole::Reference)
            module (part).stopCapture();
    }
    const bool changed = applyToModules (target, false);
    accepted = readModules();
    pendingChange.reset();
    return changed;
}

void MatchEqState::setPresetBaseline()
{
    const juce::ScopedLock sl (lock);
    presetBaseline = readModules();
}

bool MatchEqState::isPresetModified() const
{
    const juce::ScopedLock sl (lock);
    for (int part = 0; part < 2; ++part)
        if (! sameSpectrum (module (part).getSpectrum (ps::MatchRole::Reference), presetBaseline->spectra[part][0]))
            return true;
    return false;
}

//==============================================================================
void MatchEqState::historyReset (const UndoHistory& history)
{
    const juce::ScopedLock sl (lock);
    accepted = readModules();
    pendingChange.reset();
    noteModuleGenerations();
    tags.clear();
    if (history.size() > 0)
        tags[history.current().id] = accepted;
}

void MatchEqState::historyRecorded (const UndoHistory& history)
{
    const juce::ScopedLock sl (lock);
    if (history.size() > 0)
        tags[history.current().id] = accepted;
    prune (history);
}

bool MatchEqState::applyHistory (const UndoHistory& history)
{
    const juce::ScopedLock sl (lock);
    if (history.size() == 0)
        return false;
    const auto it = tags.find (history.current().id);
    if (it == tags.end() || it->second == nullptr)
        return false;
    // A running file load is not cancelled by undo / redo: it is a separate request and lands as a new step when it
    // finishes (a project load, snapshot recall or preset reference does cancel it).
    const bool changed = applyToModules (*it->second);
    accepted = it->second;
    pendingChange.reset();
    return changed;
}

bool MatchEqState::checkChange (juce::String& description)
{
    const juce::ScopedLock sl (lock);
    int part = 0;
    bool linked = false;
    ps::MatchSpectrum loaded;
    if (loader->takeResult (part, linked, loaded))
    {
        for (int p = 0; p < 2; ++p)
            if (p == part || linked)
            {
                ps::MatchRole role = ps::MatchRole::Reference;
                if (module (p).isCapturing (&role) && role == ps::MatchRole::Reference)
                    module (p).stopCapture();
                module (p).setSpectrum (ps::MatchRole::Reference, loaded);
            }
    }

    bool moved = false;
    for (int p = 0; p < 2; ++p)
        moved = moved || module (p).getSpectrumGeneration() != seenGeneration[p];
    if (! moved)
        return false;
    noteModuleGenerations();

    auto fresh = readModules();
    bool differs = false;
    for (int p = 0; p < 2 && ! differs; ++p)
        for (int r = 0; r < 2 && ! differs; ++r)
            differs = ! sameSpectrum (fresh->spectra[p][r], accepted->spectra[p][r]);
    if (! differs)
        return false;

    description = describeChange (*accepted, *fresh);
    pendingChange = std::move (fresh);
    return true;
}

void MatchEqState::acceptChange()
{
    const juce::ScopedLock sl (lock);
    if (pendingChange != nullptr)
        accepted = std::move (pendingChange);
    pendingChange.reset();
}

//==============================================================================
bool MatchEqState::startFile (const juce::File& file, int part, bool linkedParts)
{
    const juce::ScopedLock sl (lock);
    return loader->start (file, part, linkedParts);
}
void MatchEqState::cancelFile()
{
    const juce::ScopedLock sl (lock);
    loader->cancel();
}
float MatchEqState::getFileProgress() const { return loader->getProgress(); }
juce::String MatchEqState::getFileError() const { return loader->getError(); }
} // namespace psplug
