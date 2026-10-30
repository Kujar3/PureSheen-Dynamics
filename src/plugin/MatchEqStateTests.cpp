// Plug-in level tests of the Match EQ state child, spectrum undo, snapshots, presets and the reference-file loader
// (owner: dsp-meq). Compiled into PureSheenDev only (PURESHEEN_DEV_TOOL); the plug-in build sees an empty file.
// Entry point: psplug::runMatchEqStateTests() (MatchEqState.h).
#if PURESHEEN_DEV_TOOL

#include "plugin/MatchEqState.h"

#include "dsp/MatchEq.h"
#include "plugin/PluginProcessor.h"

#include <atomic>
#include <clocale>
#include <cmath>
#include <iostream>
#include <thread>

namespace psplug
{
namespace
{
    int failures = 0;

    void check (bool ok, const juce::String& name, const juce::String& detail = {})
    {
        std::cout << (ok ? "  PASS " : "  FAIL ") << name;
        if (detail.isNotEmpty())
            std::cout << "  (" << detail << ")";
        std::cout << "\n";
        if (! ok)
            ++failures;
    }

    ps::MatchEq& meq (PureSheenProcessor& p, int part)
    {
        return static_cast<ps::MatchEq&> (p.getChain().getModule (ps::ModuleType::MatchEq, part));
    }

    ps::MatchSpectrum tagged (double slope, const char* source, double seconds, double rate)
    {
        auto s = ps::makeMatchTiltSpectrum (slope, -1.5, 45.0, source);
        s.seconds = seconds;
        s.sampleRate = rate;
        for (int i = 0; i < ps::kMatchPoints; ++i)   // a Side curve with odd values (round trip of every bit)
            s.psdDb[3][(size_t) i] = (float) (-71.0 + 0.123456789 * i + slope * 0.1);
        s.psdDb[0][17] = 1.0e-7f;
        return s;
    }

    struct Proc
    {
        PureSheenProcessor p;
        explicit Proc (double rate = 48000.0)
        {
            p.setRateAndBufferSizeDetails (rate, 512);
            p.prepareToPlay (rate, 512);
        }
        void flush() { p.flushPendingChanges (true); }
        juce::MemoryBlock state()
        {
            juce::MemoryBlock mb;
            p.getStateInformation (mb);
            return mb;
        }
        void load (const juce::MemoryBlock& mb) { p.setStateInformation (mb.getData(), (int) mb.getSize()); }
        juce::String lastStep() const
        {
            const auto h = p.getHistory();
            return h.empty() ? juce::String() : h[(size_t) p.getHistoryPosition()].description;
        }
        void play (const std::vector<float>& l, const std::vector<float>& r)
        {
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            int blocks = 0;
            for (size_t pos = 0; pos < l.size(); pos += 512, ++blocks)
            {
                const int n = (int) std::min ((size_t) 512, l.size() - pos);
                buffer.setSize (2, n, false, false, true);
                std::copy (l.begin() + (long long) pos, l.begin() + (long long) pos + n, buffer.getWritePointer (0));
                std::copy (r.begin() + (long long) pos, r.begin() + (long long) pos + n, buffer.getWritePointer (1));
                p.processBlock (buffer, midi);
                if (blocks % 16 == 15)
                    juce::Thread::sleep (2);   // lets the module worker drain its capture FIFO
            }
        }
    };

    std::vector<float> pink (double seconds, double rate, unsigned seed)
    {
        std::vector<float> v ((size_t) (seconds * rate));
        unsigned state = seed;
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (auto& x : v)
        {
            state = state * 1664525u + 1013904223u;
            const double white = (double) (state >> 8) / 8388608.0 - 1.0;
            b0 = 0.99886 * b0 + white * 0.0555179;
            b1 = 0.99332 * b1 + white * 0.0750759;
            b2 = 0.96900 * b2 + white * 0.1538520;
            b3 = 0.86650 * b3 + white * 0.3104856;
            b4 = 0.55000 * b4 + white * 0.5329522;
            b5 = -0.7616 * b5 - white * 0.0168980;
            x = (float) (0.05 * (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362));
            b6 = white * 0.115926;
        }
        return v;
    }

    bool writeWav (const juce::File& f, const std::vector<float>& l, const std::vector<float>* r, double rate, int bits)
    {
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (f.createOutputStream().release());
        if (out == nullptr)
            return false;
        const int channels = r != nullptr ? 2 : 1;
        const auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (channels).withBitsPerSample (bits)
                                 .withSampleFormat (bits == 32 ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                               : juce::AudioFormatWriterOptions::SampleFormat::integral);
        auto w = wav.createWriterFor (out, options);
        if (w == nullptr)
            return false;
        const float* ch[2] { l.data(), r != nullptr ? r->data() : nullptr };
        return w->writeFromFloatArrays (ch, channels, (int) l.size());
    }

    double integratedDb (const ps::MatchSpectrum& s, double lo, double hi)
    {
        double p = 0.0;
        for (int i = 0; i < ps::kMatchPoints; ++i)
        {
            const double f = ps::matchGridHz (i);
            const float v = s.valueDb (ps::MatchChannel::Left, i);
            if (f >= lo && f <= hi && v > ps::kMatchNoValue)
                p += std::pow (10.0, v / 10.0) * ps::matchGridCellHz (i);
        }
        return 10.0 * std::log10 (std::max (p, 1.0e-30));
    }

    double slope (const ps::MatchSpectrum& s, double lo, double hi)
    {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int n = 0;
        for (int i = 0; i < ps::kMatchPoints; ++i)
        {
            const double f = ps::matchGridHz (i);
            const float v = s.valueDb (ps::MatchChannel::StereoMean, i);
            if (f < lo || f > hi || v <= ps::kMatchNoValue)
                continue;
            const double x = std::log2 (f / 1000.0);
            sx += x; sy += v; sxx += x * x; sxy += x * v;
            ++n;
        }
        return n > 1 ? (n * sxy - sx * sy) / (n * sxx - sx * sx) : 0.0;
    }

    // max |a - b| per 1/6 octave band (dB) between lo and hi
    double bandDiff (const ps::MatchSpectrum& a, const ps::MatchSpectrum& b, double lo, double hi)
    {
        double worst = 0.0;
        for (double f = lo; f < hi; f *= std::pow (2.0, 1.0 / 6.0))
            worst = std::max (worst, std::abs (integratedDb (a, f, f * std::pow (2.0, 1.0 / 6.0))
                                               - integratedDb (b, f, f * std::pow (2.0, 1.0 / 6.0))));
        return worst;
    }

    bool waitForLoad (Proc& h, std::vector<float>* progressSeen = nullptr, int maxMs = 30000)
    {
        for (int t = 0; t < maxMs; t += 5)
        {
            const float pr = h.p.getMatchReferenceFileProgress();
            if (progressSeen != nullptr && pr >= 0.0f)
                progressSeen->push_back (pr);
            h.flush();
            if (pr < 0.0f)
                return true;
            juce::Thread::sleep (5);
        }
        return false;
    }

    bool waitForUnpolledLoad (Proc& h)
    {
        for (int t = 0; t < 30000; t += 5)
        {
            if (h.p.getMatchReferenceFileProgress() < 0.0f)
                return true;
            juce::Thread::sleep (5);
        }
        return false;
    }
} // namespace

int runMatchEqStateTests()
{
    failures = 0;
    std::cout << "\n-- Match EQ state, undo, snapshots, presets, reference files\n";
    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("PureSheenMatchEqTests");
    temp.deleteRecursively();
    temp.createDirectory();

    const auto ref0 = tagged (-3.0, "file:Song A", 182.4, 44100.0);
    const auto cur0 = tagged (-4.0, "capture", 31.25, 48000.0);
    const auto ref1 = tagged (-2.0, "import:Pink Noise", 0.0, 0.0);
    const auto cur1 = tagged (-5.0, "capture", 12.5, 96000.0);

    // Decimal state values use a fixed wire format even when the host selects a comma decimal locale.
    {
        MatchSpectraSet set, decoded;
        set.spectra[0][0] = ref0;
        const auto tree = matchSpectraToTree (set, true);
        const std::string previousLocale (std::setlocale (LC_NUMERIC, nullptr));
        const bool changedLocale = std::setlocale (LC_NUMERIC, "German_Germany.1252") != nullptr
                                   || std::setlocale (LC_NUMERIC, "de_DE.UTF-8") != nullptr;
        const bool restored = matchSpectraFromTree (tree, decoded, true);
        std::setlocale (LC_NUMERIC, previousLocale.c_str());
        check (changedLocale && restored && sameSpectrum (decoded.spectra[0][0], ref0),
               "state: decimal metadata round-trips under a comma decimal host locale");
    }

    // A host save must see one complete spectrum transaction while another thread restores the set.
    {
        ps::Chain chain;
        MatchEqState state (chain);
        MatchSpectraSet first, second;
        for (int part = 0; part < 2; ++part)
            for (int role = 0; role < 2; ++role)
            {
                first.spectra[part][role] = ref0;
                second.spectra[part][role] = ref1;
            }
        const auto a = matchSpectraToTree (first, true), b = matchSpectraToTree (second, true);
        state.restoreStateTree (a);
        std::atomic<bool> done { false };
        std::atomic<int> writes { 0 };
        std::thread writer ([&]
        {
            while (! done.load())
            {
                state.restoreStateTree ((writes.load() & 1) == 0 ? b : a);
                ++writes;
                std::this_thread::yield();
            }
        });
        bool coherent = true;
        for (int i = 0; i < 200; ++i)
        {
            MatchSpectraSet snapshot;
            coherent = matchSpectraFromTree (state.createStateTree(), snapshot, true) && coherent;
            for (int part = 0; part < 2; ++part)
                for (int role = 0; role < 2; ++role)
                    coherent = sameSpectrum (snapshot.spectra[0][0], snapshot.spectra[part][role]) && coherent;
            std::this_thread::yield();
        }
        done.store (true);
        writer.join();
        check (coherent && writes.load() > 0, "state: 200 concurrent host saves contain complete spectrum sets",
               juce::String (writes.load()) + " restores");
    }

    // ---- state round trip (both parts, both roles, bit-exact), 0.1.x state without the child
    {
        Proc a, b;
        meq (a.p, 0).setSpectrum (ps::MatchRole::Reference, ref0);
        meq (a.p, 0).setSpectrum (ps::MatchRole::Current, cur0);
        meq (a.p, 1).setSpectrum (ps::MatchRole::Reference, ref1);
        meq (a.p, 1).setSpectrum (ps::MatchRole::Current, cur1);
        const auto mb = a.state();
        b.load (mb);
        const bool exact = sameSpectrum (meq (b.p, 0).getSpectrum (ps::MatchRole::Reference), ref0)
                           && sameSpectrum (meq (b.p, 0).getSpectrum (ps::MatchRole::Current), cur0)
                           && sameSpectrum (meq (b.p, 1).getSpectrum (ps::MatchRole::Reference), ref1)
                           && sameSpectrum (meq (b.p, 1).getSpectrum (ps::MatchRole::Current), cur1);
        check (exact, "state: both spectra of both parts round-trip bit-exact (psd, source, seconds, sample rate)");

        std::unique_ptr<juce::XmlElement> xml (juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize()));
        auto* child = xml != nullptr ? xml->getChildByName ("MatchEq") : nullptr;
        check (child != nullptr && child->getNumChildElements() == 4 && child->getIntAttribute ("version") == 1,
               "state: one \"MatchEq\" child (version 1) with four <Spectrum/>",
               juce::String (child != nullptr ? child->getNumChildElements() : -1));

        // the same state without the child (a 0.1.x state) clears every spectrum
        if (xml != nullptr && child != nullptr)
        {
            xml->removeChildElement (child, true);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*xml, old);
            b.load (old);
            bool cleared = true;
            for (int part = 0; part < 2; ++part)
                for (int role = 0; role < 2; ++role)
                    cleared = cleared && ! meq (b.p, part).getSpectrum ((ps::MatchRole) role).valid;
            check (cleared, "state: a 0.1.x state (no MatchEq child) loads with every spectrum cleared");
            check (b.p.getHistory().size() == 1, "state: loading leaves one history entry", juce::String ((int) b.p.getHistory().size()));
        }
    }

    // ---- undo: spectrum changes on the module are history steps
    {
        Proc h;
        meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, ref0);
        h.p.undo();   // no housekeeping tick has happened yet
        check (! meq (h.p, 0).getSpectrum (ps::MatchRole::Reference).valid && h.p.canRedo(),
               "undo: an immediate undo records and reverses the pending first spectrum edit");
        h.p.redo();
        meq (h.p, 0).clearSpectrum (ps::MatchRole::Reference);
        h.p.undo();
        check (sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), ref0),
               "undo: an immediate undo of Clear returns to the preceding spectrum step");
        meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, ref1);
        h.p.redo();
        check (sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), ref1) && ! h.p.canRedo(),
               "undo: a pending spectrum edit replaces the redo branch before redo runs");
    }
    {
        Proc h;
        meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, ref0);
        h.flush();
        check (h.lastStep() == "Match EQ: loaded reference Song A", "undo: setting a file reference is a step", h.lastStep());
        h.p.setParameterValue ("meq.a.amount", 75.0f);
        meq (h.p, 1).clearSpectrum (ps::MatchRole::Reference);   // no change: already empty
        h.flush();
        meq (h.p, 0).clearSpectrum (ps::MatchRole::Reference);
        h.flush();
        check (h.lastStep() == "Match EQ: cleared the reference", "undo: Clear is a step", h.lastStep());
        h.p.undo();
        check (sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), ref0), "undo: undo of Clear restores the reference");
        h.p.undo();   // the amount change
        check (std::abs (h.p.getParameterValue ("meq.a.amount") - 50.0f) < 1e-3f
                   && sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), ref0),
               "undo: a parameter step keeps the spectra of its time");
        h.p.undo();
        check (! meq (h.p, 0).getSpectrum (ps::MatchRole::Reference).valid, "undo: undo of the load clears the reference again");
        h.p.redo();
        h.p.redo();
        h.p.redo();
        check (! meq (h.p, 0).getSpectrum (ps::MatchRole::Reference).valid && std::abs (h.p.getParameterValue ("meq.a.amount") - 75.0f) < 1e-3f,
               "undo: redo to the end gives the cleared state with amount 75");
        h.p.jumpToHistory (1);
        check (sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), ref0), "undo: jumping restores that entry's spectra");
        const auto entries = (int) h.p.getHistory().size();
        h.flush();
        check ((int) h.p.getHistory().size() == entries, "undo: restoring spectra by undo / jump records no new step");

        // a real capture through processBlock (Match EQ in the chain), stop = one step
        h.p.addModule (ps::ModuleType::MatchEq);
        h.flush();
        meq (h.p, 0).startCapture (ps::MatchRole::Current);
        const auto l = pink (3.0, 48000.0, 5), r = pink (3.0, 48000.0, 6);
        h.play (l, r);
        meq (h.p, 0).stopCapture();
        h.flush();
        check (h.lastStep() == "Match EQ: captured the current spectrum", "undo: a capture stop is a step", h.lastStep());
        const auto captured = meq (h.p, 0).getSpectrum (ps::MatchRole::Current);
        check (captured.valid && std::abs (slope (captured, 100.0, 10000.0) + 3.0) < 0.2,
               "undo: the live capture through the processor measures the pink slope",
               juce::String (slope (captured, 100.0, 10000.0), 3));
        h.p.undo();
        check (! meq (h.p, 0).getSpectrum (ps::MatchRole::Current).valid, "undo: undo of the capture clears the current spectrum");
    }

    // ---- snapshots
    {
        Proc a, b;
        meq (a.p, 0).setSpectrum (ps::MatchRole::Reference, ref0);
        meq (a.p, 1).setSpectrum (ps::MatchRole::Current, cur1);
        a.flush();
        a.p.storeSnapshot (0);
        meq (a.p, 0).setSpectrum (ps::MatchRole::Reference, ref1);
        meq (a.p, 1).clearSpectrum (ps::MatchRole::Current);
        // No timer tick: recall must preserve these pending edits for undo.
        a.p.recallSnapshot (0);
        check (sameSpectrum (meq (a.p, 0).getSpectrum (ps::MatchRole::Reference), ref0)
                   && sameSpectrum (meq (a.p, 1).getSpectrum (ps::MatchRole::Current), cur1),
               "snapshots: recall restores the spectra stored with the snapshot");
        check (a.lastStep() == "Recalled snapshot A", "snapshots: the recall is one step", a.lastStep());
        a.p.undo();
        check (sameSpectrum (meq (a.p, 0).getSpectrum (ps::MatchRole::Reference), ref1), "snapshots: undo of a recall");
        b.load (a.state());
        check (sameSpectrum (meq (b.p, 0).getSpectrum (ps::MatchRole::Reference), ref1), "snapshots: state carries the live spectra");
        b.p.recallSnapshot (0);
        check (sameSpectrum (meq (b.p, 0).getSpectrum (ps::MatchRole::Reference), ref0)
                   && sameSpectrum (meq (b.p, 1).getSpectrum (ps::MatchRole::Current), cur1),
               "snapshots: the saved state carries the snapshot's spectra");
        // snapshot from a history entry
        b.p.storeSnapshotFromHistory (1, 0);   // the initial state after loading: ref1
        b.p.recallSnapshot (1);
        check (sameSpectrum (meq (b.p, 0).getSpectrum (ps::MatchRole::Reference), ref1), "snapshots: a snapshot taken from a history entry");
    }

    // ---- host-thread saves while the message thread replaces a snapshot
    {
        Proc h;
        h.p.setParameterValue ("meq.a.amount", 20.0f);
        meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, ref0);
        h.p.storeSnapshot (0);
        std::atomic<bool> done { false };
        std::atomic<int> saves { 0 };
        bool coherent = true;
        std::thread host ([&]
        {
            while (! done.load() || saves.load() < 100)
            {
                const auto mb = h.state();
                const auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
                const auto tree = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree();
                const auto snapshot = tree.getChildWithName ("Snapshots").getChild (0);
                const float amount = (float) snapshot.getChildWithName ("Params")
                                         .getChildWithProperty ("id", "meq.a.amount").getProperty ("v", -1.0f);
                MatchSpectraSet spectra;
                const bool decoded = matchSpectraFromTree (snapshot.getChildWithName ("MatchEq"), spectra, true);
                const auto& expected = amount < 50.0f ? ref0 : ref1;
                coherent = decoded && sameSpectrum (spectra.spectra[0][0], expected) && coherent;
                ++saves;
                std::this_thread::yield();
            }
        });
        for (int i = 0; i < 400; ++i)
        {
            h.p.setParameterValue ("meq.a.amount", (i & 1) == 0 ? 80.0f : 20.0f);
            meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, (i & 1) == 0 ? ref1 : ref0);
            h.p.storeSnapshot (0);
            std::this_thread::yield();
        }
        done.store (true);
        host.join();
        check (coherent, "snapshots: concurrent host saves pair snapshot parameters with their spectra",
               juce::String (saves.load()) + " saves / 400 stores");
    }

    // ---- a host save must pair live APVTS parameters with the live reference during complete state changes
    {
        Proc h;
        h.p.setParameterValue ("meq.a.amount", 20.0f);
        meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, ref0);
        h.flush();
        h.p.storeSnapshot (0);
        h.p.setParameterValue ("meq.a.amount", 80.0f);
        meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, ref1);
        h.flush();
        h.p.storeSnapshot (1);
        const auto secondState = h.state();
        h.p.recallSnapshot (0);
        const auto firstState = h.state();

        const auto checkConcurrentTransactions = [&] (bool projectRestores)
        {
            std::atomic<bool> done { false }, started { false };
            std::atomic<int> saves { 0 }, overlappingSaves { 0 };
            bool coherent = true;
            std::thread host ([&]
            {
                started.store (true);
                while (! done.load() || saves.load() < 100)
                {
                    const auto mb = h.state();
                    if (! done.load())
                        ++overlappingSaves;
                    const auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
                    const auto tree = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree();
                    const float amount = (float) tree.getChildWithName ("PARAMS")
                                             .getChildWithProperty ("id", "meq.a.amount").getProperty ("value", -1.0f);
                    MatchSpectraSet spectra;
                    const bool decoded = matchSpectraFromTree (tree.getChildWithName ("MatchEq"), spectra, true);
                    const bool isFirst = std::abs (amount - 20.0f) < 1.0e-3f;
                    const bool isSecond = std::abs (amount - 80.0f) < 1.0e-3f;
                    coherent = decoded && (isFirst || isSecond)
                               && sameSpectrum (spectra.spectra[0][0], isFirst ? ref0 : ref1) && coherent;
                    ++saves;
                    std::this_thread::yield();
                }
            });
            while (! started.load())
                std::this_thread::yield();
            for (int i = 0; i < 200; ++i)
            {
                if (projectRestores)
                    h.load ((i & 1) == 0 ? secondState : firstState);
                else
                    h.p.recallSnapshot ((i & 1) == 0 ? 1 : 0);
                std::this_thread::yield();
            }
            done.store (true);
            host.join();
            check (coherent && overlappingSaves.load() > 0,
                   projectRestores ? "state: host saves pair live parameters and spectra during project restores"
                                   : "state: host saves pair live parameters and spectra during snapshot recalls",
                   juce::String (saves.load()) + " saves / " + juce::String (overlappingSaves.load())
                       + " overlapping saves / 200 transactions");
        };
        checkConcurrentTransactions (false);
        checkConcurrentTransactions (true);
    }

    // ---- presets: global and Match EQ presets carry the references (not the current spectra)
    {
        Proc h;
        h.p.setPresetRoot (temp.getChildFile ("Presets"));
        meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, ref0);
        meq (h.p, 0).setSpectrum (ps::MatchRole::Current, cur0);
        meq (h.p, 1).setSpectrum (ps::MatchRole::Reference, ref1);
        h.flush();
        const int scope = (int) ps::ModuleType::MatchEq;
        check (h.p.savePreset (scope, "Ref Test"), "presets: save a Match EQ preset");
        check (h.p.savePreset (-1, "Global Ref Test"), "presets: save a global preset");
        check (h.p.savePreset ((int) ps::ModuleType::Dynamics, "Dyn Test"), "presets: save a Dynamics preset");

        meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, ref1);
        meq (h.p, 0).setSpectrum (ps::MatchRole::Current, cur1);
        meq (h.p, 1).clearSpectrum (ps::MatchRole::Reference);
        h.flush();
        const auto find = [&] (int sc, const juce::String& name)
        {
            for (const auto& info : h.p.getPresets (sc))
                if (info.name == name)
                    return info;
            return PureSheenProcessor::PresetInfo {};
        };
        h.p.loadPreset ((int) ps::ModuleType::Dynamics, find ((int) ps::ModuleType::Dynamics, "Dyn Test"));
        check (sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), ref1), "presets: another module's preset leaves the reference");
        h.p.loadPreset (scope, find (scope, "Ref Test"));
        check (sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), ref0)
                   && sameSpectrum (meq (h.p, 1).getSpectrum (ps::MatchRole::Reference), ref1),
               "presets: a Match EQ preset restores the references of both parts");
        check (sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Current), cur1), "presets: the current spectrum is not touched");
        check (h.lastStep().startsWith ("Loaded Match EQ preset Ref Test"), "presets: the load is one step", h.lastStep());
        h.p.undo();
        check (sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), ref1), "presets: undo of the preset load");

        meq (h.p, 0).clearSpectrum (ps::MatchRole::Reference);
        h.flush();
        h.p.loadPreset (-1, find (-1, "Global Ref Test"));
        check (sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), ref0), "presets: a global preset restores the reference");
        check (! h.p.isPresetModified(), "presets: a loaded global preset baselines its reference spectra");
        meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, ref1);
        h.flush();
        check (h.p.isPresetModified(), "presets: a spectrum-only edit marks the preset modified");
        const auto modifiedState = h.state();
        const auto modifiedXml = juce::AudioProcessor::getXmlFromBinary (modifiedState.getData(), (int) modifiedState.getSize());
        check (modifiedXml != nullptr && modifiedXml->getBoolAttribute ("presetModified"),
               "presets: host state preserves the spectrum-only modified flag");
        h.p.undo();
        check (! h.p.isPresetModified(), "presets: undo back to the saved references clears the modified flag");

        meq (h.p, 0).setSpectrum (ps::MatchRole::Reference, cur0);
        h.p.loadPreset (scope, find (scope, "Ref Test"));   // the pending edit has not been polled
        h.p.undo();
        check (sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), cur0),
               "presets: undo of a preset load restores an unpolled spectrum edit");
        check (h.p.savePreset (-1, "Changed Reference") && ! h.p.isPresetModified(),
               "presets: saving a global preset baselines the changed references");

        const auto file = temp.getChildFile ("Presets").getChildFile ("meq").getChildFile ("Ref Test.pspreset");
        auto xml = juce::XmlDocument::parse (file);
        auto* child = xml != nullptr ? xml->getChildByName ("MatchEq") : nullptr;
        check (child != nullptr && child->getNumChildElements() == 2, "presets: the file holds the two references only",
               juce::String (child != nullptr ? child->getNumChildElements() : -1));

        ps::MatchSpectrum synth;
        check (factoryMatchReference ("Pink Balance", synth) && synth.valid && std::abs (slope (synth, 100.0, 10000.0) + 3.0) < 1e-3,
               "presets: factory reference \"Pink Balance\" is -3 dB/oct");
        check (! factoryMatchReference ("Low End Only", synth), "presets: other factory names carry no reference");
    }

    // ---- reference file loader
    {
        Proc h (44100.0);
        const double rate = 44100.0;
        const auto l = pink (10.0, rate, 11), r = pink (10.0, rate, 12);
        const auto wavFile = temp.getChildFile ("Pink Test.wav");
        check (writeWav (wavFile, l, &r, rate, 32), "files: write a 32-bit float stereo WAV");

        check (h.p.getMatchReferenceFileProgress() == -1.0f, "files: progress is -1 when idle");
        check (h.p.startMatchReferenceFile (wavFile, 0), "files: start loading a WAV", h.p.getMatchReferenceFileError());
        std::vector<float> seen;
        check (waitForLoad (h, &seen), "files: the load finishes");
        const auto loaded = meq (h.p, 0).getSpectrum (ps::MatchRole::Reference);
        check (loaded.valid && juce::String (loaded.source.data()) == "file:Pink Test" && std::abs (loaded.sampleRate - rate) < 1e-9,
               "files: the reference is set with source \"file:<name>\"", juce::String (loaded.source.data()));
        check (h.lastStep() == "Match EQ: loaded reference Pink Test", "files: the load is one undo step", h.lastStep());
        check (h.p.getMatchReferenceFileError().isEmpty(), "files: no error after a good file");
        bool monotonic = ! seen.empty();
        for (size_t i = 1; i < seen.size(); ++i)
            monotonic = monotonic && seen[i] >= seen[i - 1] && seen[i] <= 1.0f;
        check (monotonic, "files: progress rises within 0..1", juce::String ((int) seen.size()) + " readings");

        // equals the analyser run directly on the same samples (the live analysis path)
        ps::MatchAnalyser direct;
        direct.prepare (rate);
        direct.push (l.data(), r.data(), (int) l.size());
        ps::MatchSpectrum ds;
        direct.spectrum (ds);
        bool samePsd = true;
        for (int c = 0; c < 4; ++c)
            samePsd = samePsd && std::memcmp (ds.psdDb[c].data(), loaded.psdDb[c].data(), sizeof (float) * ps::kMatchPoints) == 0;
        check (samePsd, "files: the file spectrum equals the analyser on the same samples (bit-exact)");

        h.p.undo();
        const bool undone = ! meq (h.p, 0).getSpectrum (ps::MatchRole::Reference).valid;
        h.p.redo();
        check (undone && sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), loaded),
               "files: undo / redo of the loaded reference");

        // file vs live capture of the same audio through the processor (Match EQ in the chain)
        h.p.addModule (ps::ModuleType::MatchEq);
        h.flush();
        meq (h.p, 0).startCapture (ps::MatchRole::Current);
        h.play (l, r);
        meq (h.p, 0).stopCapture();
        h.flush();
        const auto live = meq (h.p, 0).getSpectrum (ps::MatchRole::Current);
        const double fileVsLive = bandDiff (live, loaded, 20.0, 20000.0);
        check (fileVsLive < 0.2, "files: file vs live capture within 0.2 dB per 1/6 octave (20 Hz - 20 kHz)",
               juce::String (fileVsLive, 3) + " dB");


        // mono 16-bit file into the twin (part 1): L = R, no Side
        const auto monoFile = temp.getChildFile ("Mono.wav");
        writeWav (monoFile, l, nullptr, rate, 16);
        check (h.p.startMatchReferenceFile (monoFile, 1) && waitForLoad (h), "files: a mono 16-bit WAV loads into part 1");
        const auto mono = meq (h.p, 1).getSpectrum (ps::MatchRole::Reference);
        double sideMax = -400.0;
        for (int i = 0; i < ps::kMatchPoints; ++i)
            sideMax = std::max (sideMax, (double) mono.valueDb (ps::MatchChannel::Side, i));
        check (mono.valid && mono.valueDb (ps::MatchChannel::Left, 300) == mono.valueDb (ps::MatchChannel::Right, 300)
                   && sideMax == ps::kMatchNoValue,
               "files: mono = L = R, Side has no value", juce::String (sideMax));
        check (h.lastStep() == "Match EQ: loaded reference Mono (Sustain)", "files: part 1 step description", h.lastStep());

        // a 96 kHz file and a 44.1 kHz file of the same 1 kHz tone read the same level
        const auto toneAt = [&] (double sr)
        {
            std::vector<float> t ((size_t) (3.0 * sr));
            for (size_t i = 0; i < t.size(); ++i)
                t[i] = (float) (0.1 * std::sin (2.0 * 3.14159265358979323846 * 1000.0 * (double) i / sr));
            const auto f = temp.getChildFile ("Tone" + juce::String ((int) sr) + ".wav");
            writeWav (f, t, &t, sr, 32);
            h.p.startMatchReferenceFile (f, 0);
            waitForLoad (h);
            return integratedDb (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), 900.0, 1100.0);
        };
        const double t44 = toneAt (44100.0), t96 = toneAt (96000.0);
        check (std::abs (t44 + 23.0103) < 0.1 && std::abs (t96 - t44) < 0.05, "files: 44.1 and 96 kHz files read the same tone level",
               juce::String (t44, 3) + " / " + juce::String (t96, 3) + " dB");

        // cancel: a long file, cancelled at once -> idle, no error, no change
        const auto before = meq (h.p, 0).getSpectrum (ps::MatchRole::Reference);
        const auto longFile = temp.getChildFile ("Long.wav");
        writeWav (longFile, pink (240.0, rate, 3), nullptr, rate, 16);
        const bool started = h.p.startMatchReferenceFile (longFile, 0);
        juce::Thread::sleep (20);
        const float midway = h.p.getMatchReferenceFileProgress();
        h.p.cancelMatchReferenceFile();
        h.flush();
        check (started && midway >= 0.0f && midway < 1.0f && h.p.getMatchReferenceFileProgress() == -1.0f,
               "files: cancel stops a running load", juce::String (midway, 3));
        check (h.p.getMatchReferenceFileError().isEmpty() && sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), before),
               "files: a cancelled load changes nothing and is no error");

        // Completion races cancellation: Done has not yet been consumed by the message-thread poll.
        const bool completed = h.p.startMatchReferenceFile (wavFile, 0) && waitForUnpolledLoad (h);
        h.p.cancelMatchReferenceFile();
        h.flush();
        check (completed && sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), before),
               "files: cancelling a completed unpolled load discards its result");

        const auto savedState = h.state();
        const bool readyBeforeRestore = h.p.startMatchReferenceFile (wavFile, 0) && waitForUnpolledLoad (h);
        h.load (savedState);
        h.flush();
        check (readyBeforeRestore && sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), before)
                   && h.p.getHistory().size() == 1,
               "files: restoring a project discards a previous pending file result");

        bool restarted = true;
        for (int i = 0; i < 8; ++i)
        {
            restarted = h.p.startMatchReferenceFile (longFile, i & 1) && restarted;
            h.p.cancelMatchReferenceFile();
        }
        h.flush();
        check (restarted && h.p.getMatchReferenceFileProgress() == -1.0f
                   && sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), before),
               "files: eight immediate restart/cancel cycles join cleanly and publish no result");

        // errors
        const auto bogus = temp.getChildFile ("Bogus.wav");
        bogus.replaceWithText ("this is not audio");
        const bool bogusRefused = ! h.p.startMatchReferenceFile (bogus, 0);
        const auto bogusError = h.p.getMatchReferenceFileError();
        check (bogusRefused && bogusError.isNotEmpty(), "files: a bogus file is refused with a message", bogusError);
        const bool missingRefused = ! h.p.startMatchReferenceFile (temp.getChildFile ("Missing.flac"), 0);
        const auto missingError = h.p.getMatchReferenceFileError();
        check (missingRefused && missingError == "The file could not be found.", "files: a missing file is refused", missingError);
        const auto silentFile = temp.getChildFile ("Silent.wav");
        writeWav (silentFile, std::vector<float> ((size_t) (3.0 * rate), 0.0f), nullptr, rate, 16);
        const bool silentStarted = h.p.startMatchReferenceFile (silentFile, 0);
        waitForLoad (h);
        check (silentStarted && h.p.getMatchReferenceFileError().startsWith ("The file has less than one second")
                   && sameSpectrum (meq (h.p, 0).getSpectrum (ps::MatchRole::Reference), before),
               "files: a silent file ends with a message and changes nothing", h.p.getMatchReferenceFileError());
    }

    temp.deleteRecursively();
    std::cout << "  Match EQ state tests: " << failures << " failed\n";
    return failures;
}
} // namespace psplug

#endif
