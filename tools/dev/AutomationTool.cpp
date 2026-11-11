#include "AllocationTracking.h"
#include "dsp/Params.h"
#include "plugin/PluginProcessor.h"
#include "plugin/Product.h"
#include "ui/PluginEditor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <vector>

// PureSheenDev automationtest: what a host's automation sees and does (checks; exit code 0 when all pass).
// PureSheenDev automationscan <out.tsv>: a measurement, not a pass / fail check. Sweeps one parameter of every kind
// the way a clip does, block by block, and writes how much sharper the output's steps get than at the same settings
// held still, and whether the visible panel changed. Band 2 and up and channel set B run the same code, so they are
// left out.
#include "MessagePump.h"

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

int numPassed = 0, numFailed = 0;

void check (bool ok, const juce::String& name, const juce::String& detail = {})
{
    if (ok)
    {
        ++numPassed;
        std::cout << "PASS " << name << "\n";
    }
    else
    {
        ++numFailed;
        std::cout << "FAIL " << name << (detail.isEmpty() ? juce::String() : ": " + detail) << "\n";
    }
}

void pumpMessages (int milliseconds) { ps::dev::pumpMessages (milliseconds); }

// A value change the way a host's automation delivers it: no gesture, and the host is told nothing back.
void automate (juce::RangedAudioParameter& p, float normalised)
{
    p.setValue (normalised);
    p.sendValueChangedMessageToListeners (normalised);
}

//==============================================================================
// Two tones per channel. They hold still, so anything sharp in the output comes from the parameter moving.
struct Tones
{
    juce::int64 sample = 0;

    void fill (juce::AudioBuffer<float>& b)
    {
        constexpr double twoPi = juce::MathConstants<double>::twoPi;
        for (int i = 0; i < b.getNumSamples(); ++i, ++sample)
        {
            const double t = (double) sample / kRate;
            b.setSample (0, i, (float) (0.25 * std::sin (twoPi * 997.0 * t) + 0.12 * std::sin (twoPi * 110.0 * t)));
            b.setSample (1, i, (float) (0.25 * std::sin (twoPi * 1013.0 * t) + 0.12 * std::sin (twoPi * 113.0 * t)));
        }
    }
};

// The largest second difference of the output: a step or a corner shows up in it at once, a smooth tone barely.
struct SharpnessMeter
{
    float last[2][2] {};
    int seen[2] {};   // the first two samples have no history to compare with
    float peak = 0.0f, largest = 0.0f;
    bool finite = true;

    void add (const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* d = b.getReadPointer (ch);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                finite = finite && std::isfinite (d[i]);
                if (seen[ch] >= 2)
                    peak = juce::jmax (peak, std::abs (d[i] - 2.0f * last[ch][0] + last[ch][1]));
                else
                    ++seen[ch];
                largest = juce::jmax (largest, std::abs (d[i]));
                last[ch][1] = last[ch][0];
                last[ch][0] = d[i];
            }
        }
    }
};

struct ScanContext
{
    ps::ModuleType type;
    std::vector<std::pair<const char*, float>> settings;   // so that the module is doing something
};

const std::vector<ScanContext>& scanContexts()
{
    using T = ps::ModuleType;
    static const std::vector<ScanContext> contexts {
        { T::Equalizer1, { { "eq1.a.b1.on", 1 }, { "eq1.a.b1.gain", 6 } } },
        { T::Equalizer2, { { "eq2.a.b1.on", 1 }, { "eq2.a.b1.gain", 6 } } },
        { T::Dynamics, { { "dyn.bands", 2 }, { "dyn.a.b1.comp.thr", -24 }, { "dyn.a.b1.comp.ratio", 4 } } },
        { T::Imager, { { "img.bands", 2 }, { "img.b1.width", 50 } } },
        { T::Exciter, { { "exc.bands", 2 }, { "exc.a.b1.drive", 50 } } },
        { T::Maximizer, { { "max.gain", 6 } } },
        { T::DynamicEq, { { "deq.a.b1.on", 1 }, { "deq.a.b1.gain", -12 }, { "deq.a.b1.thr", -40 } } },
        { T::MatchEq, {} },
        { T::ToneBalance, { { "tbal.a.amount", 50 } } },
        { T::Definition, { { "defn.a.amount", 50 } } },
        { T::Smoother, { { "smth.a.amount", 50 } } },
        { T::Punch, { { "pnch.bands", 2 }, { "pnch.a.b1.amount", 50 } } },
        { T::LowContrast, { { "lowc.a.contrast", 50 } } },
        { T::BassShaper, { { "bass.balance", 6 } } },
        { T::ClassicComp, { { "ccmp.a.thr", -24 } } },
        { T::PassiveEq, { { "peq.a.lowBoost", 5 }, { "peq.a.highBoost", 5 } } },
        { T::ClassicLimiter, { { "clim.thr", -8 } } },
        { T::Tape, { { "tape.drive", 6 } } },
        { T::DeLimiter, { { "dlim.amount", 100 }, { "dlim.thr", -6 } } },
    };
    return contexts;
}

// One parameter of each kind: channel set A, band 1, crossover 1.
bool isRepresentative (const ps::ParamSpec& s)
{
    const juce::String id (s.id);
    if (id.contains (".b."))
        return false;
    for (int band = 2; band <= 8; ++band)
        if (id.contains (".b" + juce::String (band) + "."))
            return false;
    return ! (id.endsWith ("xover2") || id.endsWith ("xover3"));
}

struct Scan
{
    PureSheenProcessor proc;
    juce::AudioBuffer<float> buffer { 2, kBlock };
    juce::MidiBuffer midi;
    Tones tones;

    Scan() { proc.prepareToPlay (kRate, kBlock); }
    ~Scan() { proc.releaseResources(); }

    void run (int blocks, SharpnessMeter* meter = nullptr, const std::function<void (int)>& before = {})
    {
        for (int i = 0; i < blocks; ++i)
        {
            if (before)
                before (i);
            tones.fill (buffer);
            proc.processBlock (buffer, midi);
            if (meter != nullptr)
                meter->add (buffer);
        }
    }

    // Held still: the sharpest the output gets at this setting, after it has settled.
    float heldSharpness (juce::RangedAudioParameter& p, float normalised)
    {
        automate (p, normalised);
        run ((int) (0.6 * kRate / kBlock));
        SharpnessMeter m;
        run ((int) (0.3 * kRate / kBlock), &m);
        return m.peak;
    }
};

struct SweepResult
{
    float held = 0.0f, swept = 0.0f, largest = 0.0f;
    bool finite = true;
    float ratio() const noexcept { return swept / juce::jmax (1.0e-6f, held); }
};

const int kSettleBlocks = (int) (0.2 * kRate / kBlock);

// A clip from the bottom to the top in 1.5 s and back, against the same settings held still.
SweepResult measureSweep (Scan& scan, juce::RangedAudioParameter& p)
{
    const auto sweep = (int) (1.5 * kRate / kBlock);
    SweepResult r;
    r.held = juce::jmax (scan.heldSharpness (p, 0.0f), scan.heldSharpness (p, 0.5f), scan.heldSharpness (p, 1.0f));

    SharpnessMeter m;
    automate (p, 0.0f);
    scan.run (kSettleBlocks);
    scan.run (sweep, &m, [&] (int i) { automate (p, (float) i / (float) (sweep - 1)); });
    scan.run (sweep, &m, [&] (int i) { automate (p, 1.0f - (float) i / (float) (sweep - 1)); });
    r.swept = m.peak;
    r.largest = m.largest;
    r.finite = m.finite;
    return r;
}

// module -1: the I/O panel's parameters and the Dither's, in front of a Maximizer.
void scanModule (int module, const std::vector<std::pair<const char*, float>>& settings, juce::FileOutputStream& out)
{
    Scan scan;
    scan.proc.setChainOrder ({ module >= 0 ? (ps::ModuleType) module : ps::ModuleType::Maximizer });

    std::map<juce::String, float> contextValues;
    for (const auto& [id, value] : settings)
        contextValues[id] = value;

    for (const auto& s : ps::paramSpecs())
    {
        if (s.module != module || ! isRepresentative (s))
            continue;
        const juce::String id (s.id);
        auto* p = scan.proc.getParameter (id);
        if (p == nullptr)
            continue;

        for (const auto& [cid, value] : contextValues)
            scan.proc.setParameterValue (cid, value);
        const auto powerId = juce::String (s.id).upToFirstOccurrenceOf (".", false, false) + ".on";
        scan.proc.setParameterValue (powerId, 1.0f);
        scan.run (kSettleBlocks);

        const float start = p->getValue();
        const auto r = measureSweep (scan, *p);

        const char* kind = s.kind == ps::ParamKind::Float ? "float" : (s.kind == ps::ParamKind::Choice ? "choice" : "switch");
        out << id << "\t" << kind << "\t" << juce::String (r.held, 5) << "\t" << juce::String (r.swept, 5) << "\t"
            << juce::String (r.ratio(), 2) << "\t" << juce::String (r.largest, 3) << "\t" << (r.finite ? "ok" : "NONFINITE") << "\n";
        automate (*p, start);
        scan.run (kSettleBlocks);
    }
    scan.proc.setChainOrder ({});
    out.flush();
}

// Whether the panel of a module draws something different after a host moved the parameter.
void scanPanels (juce::FileOutputStream& out)
{
    PureSheenProcessor proc;
    proc.prepareToPlay (kRate, kBlock);
    std::vector<ps::ModuleType> all;
    for (const auto& c : scanContexts())
        all.push_back (c.type);
    proc.setChainOrder (all);

    std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
    auto* editor = dynamic_cast<PureSheenEditor*> (holder.get());
    editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);

    auto picture = [&]
    {
        pumpMessages (3);
        editor->updateNow();
        pumpMessages (3);
        editor->updateNow();
        return editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    };
    auto same = [] (const juce::Image& a, const juce::Image& b)
    {
        const juce::Image::BitmapData da (a, juce::Image::BitmapData::readOnly), db (b, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < a.getHeight(); ++y)
            if (std::memcmp (da.getLinePointer (y), db.getLinePointer (y), (size_t) a.getWidth() * (size_t) da.pixelStride) != 0)
                return false;
        return true;
    };

    std::vector<int> modules { -1 };
    for (const auto& c : scanContexts())
        modules.push_back ((int) c.type);

    for (const int module : modules)
    {
        editor->selectModule (module);   // -1: the Dither panel; the I/O panel is always there
        for (const auto& s : ps::paramSpecs())
        {
            if (s.module != module || ! isRepresentative (s))
                continue;
            auto* p = proc.getParameter (juce::String (s.id));
            if (p == nullptr)
                continue;
            const auto start = p->getValue();
            auto movesPanel = [&]
            {
                const auto before = picture();
                automate (*p, start < 0.5f ? 1.0f : 0.0f);
                const auto after = picture();
                automate (*p, start);
                picture();
                return ! same (before, after);
            };

            juce::String shownWhen = movesPanel() ? "moved" : "NOT SHOWN";

            // Some controls appear only in a channel mode, a filter mode or with more bands: try those switches.
            if (shownWhen == "NOT SHOWN")
                for (const auto& other : ps::paramSpecs())
                {
                    const juce::String otherId (other.id);
                    if (other.module != module || other.kind == ps::ParamKind::Bool || shownWhen != "NOT SHOWN"
                        || ! (otherId.endsWith (".chMode") || otherId.endsWith (".mode") || otherId.endsWith (".bands")))
                        continue;
                    auto* revealer = proc.getParameter (otherId);
                    if (revealer == nullptr || revealer == p)
                        continue;
                    const auto revealerStart = revealer->getValue();
                    const int steps = juce::jlimit (2, 8, revealer->getNumSteps());   // a continuous one reports billions
                    for (int i = 0; i < steps && shownWhen == "NOT SHOWN"; ++i)
                    {
                        automate (*revealer, (float) i / (float) (steps - 1));
                        picture();
                        if (movesPanel())
                            shownWhen = "moved with " + otherId + "=" + revealer->getCurrentValueAsText();
                    }
                    automate (*revealer, revealerStart);
                    picture();
                }

            out << juce::String (s.id) << "\t" << shownWhen << "\n";
        }
        out.flush();
    }
    holder.reset();
    proc.releaseResources();
}
} // namespace

int runAutomationScan (const juce::File& output)
{
    output.deleteFile();
    juce::FileOutputStream out (output);
    if (! out.openedOk())
        return 1;
    out << "id\tkind\theld\tswept\tratio\tlargest\tfinite\n";
    scanModule (-1, {}, out);
    for (const auto& context : scanContexts())
    {
        std::cout << ps::moduleInfo (context.type).idPrefix << std::endl;
        scanModule ((int) context.type, context.settings, out);
    }
    out << "\nid\tpanel\n";
    scanPanels (out);
    return 0;
}

int runAutomationTests()
{
    psdev::installHeapHooks();

    // What the host lists, in every plugin.
    for (const auto& product : psplug::allProducts())
    {
        const juce::String tag (product.id);
        PureSheenProcessor proc (product);
        std::set<juce::String> names;
        bool allAutomatable = true, endsOk = true, unique = true, stepsOk = true;
        juce::String firstProblem;
        const auto& list = proc.getParameters();

        for (auto* base : list)
        {
            auto* p = dynamic_cast<juce::RangedAudioParameter*> (base);
            if (p == nullptr)
            {
                allAutomatable = false;
                continue;
            }
            const auto name = p->getName (128);
            unique = names.insert (name).second && unique;
            allAutomatable = allAutomatable && p->isAutomatable();

            const int index = ps::paramIndex (p->getParameterID().toStdString());
            if (index < 0)
            {
                endsOk = false;
                firstProblem = p->getParameterID();
                continue;
            }
            const auto& s = ps::paramSpecs()[(size_t) index];
            // The bottom and the top of a clip are the bottom and the top of the control.
            const float low = p->convertFrom0to1 (0.0f), high = p->convertFrom0to1 (1.0f);
            if (std::abs (low - s.minValue) > 1.0e-4f * (s.maxValue - s.minValue)
                || std::abs (high - s.maxValue) > 1.0e-4f * (s.maxValue - s.minValue))
            {
                endsOk = false;
                firstProblem = p->getParameterID();
            }
            // Every choice can be reached, and it takes a step the host can show.
            if (s.kind == ps::ParamKind::Choice && p->getNumSteps() != (int) s.choices.size())
                stepsOk = false;
        }

        check (! list.isEmpty() && allAutomatable, tag + ": every parameter can be automated");
        check (unique, tag + ": every name in the host's list is different");
        check (endsOk, tag + ": a clip's 0 and 100 are the control's bottom and top", firstProblem);
        check (stepsOk, tag + ": every choice can be reached from a clip");
    }

    // The sharpness meter sees what it is there to see: a gain jumping a quarter dB per block, as an unsmoothed control
    // would under a clip.
    {
        juce::AudioBuffer<float> buffer (2, kBlock);
        Tones tones;
        SharpnessMeter held, stepped;
        const auto blocks = (int) (1.5 * kRate / kBlock);
        for (int i = 0; i < blocks; ++i)
        {
            tones.fill (buffer);
            held.add (buffer);
        }
        for (int i = 0; i < blocks; ++i)
        {
            tones.fill (buffer);
            buffer.applyGain (juce::Decibels::decibelsToGain ((float) i * 0.25f - 12.0f));
            stepped.add (buffer);
        }
        check (stepped.peak > 3.0f * held.peak, "the sweep's meter sees a gain moving in steps",
               juce::String (stepped.peak) + " against " + juce::String (held.peak));
    }

    // Clips on controls that scale the signal directly, where a missing glide would be easiest to hear. (automationscan
    // measures every kind of parameter.)
    {
        Scan scan;
        for (const auto& [id, order] : std::vector<std::pair<const char*, ps::ModuleType>> {
                 { "in.gainL", ps::ModuleType::Maximizer }, { "out.gainR", ps::ModuleType::Maximizer },
                 { "max.gain", ps::ModuleType::Maximizer }, { "eq1.a.b1.gain", ps::ModuleType::Equalizer1 },
                 { "img.b1.width", ps::ModuleType::Imager }, { "bass.balance", ps::ModuleType::BassShaper },
                 { "tape.drive", ps::ModuleType::Tape }, { "ccmp.a.makeup", ps::ModuleType::ClassicComp } })
        {
            scan.proc.setChainOrder ({ order });
            scan.proc.setParameterValue ("eq1.a.b1.on", 1.0f);
            auto* p = scan.proc.getParameter (id);
            const auto r = measureSweep (scan, *p);
            check (r.finite && r.ratio() < 3.0f, juce::String ("a clip on ") + id + " moves without steps",
                   juce::String (r.swept) + " against " + juce::String (r.held));
            automate (*p, p->convertTo0to1 (ps::paramSpecs()[(size_t) ps::paramIndex (id)].defaultValue));
            scan.run (kSettleBlocks);
        }
    }

    // A clip moves the sound and the state, without leaving anything in the history.
    {
        PureSheenProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;
        Tones tones;
        auto* gain = proc.getParameter ("max.gain");
        const auto before = proc.getHistory().size();
        psdev::AllocationCounts counts = psdev::getAllocationCounts();

        for (int i = 0; i < 200; ++i)
        {
            tones.fill (buffer);
            psdev::setAllocationTracking (true);   // the value arrives on the audio thread too
            automate (*gain, (float) i / 199.0f);
            proc.processBlock (buffer, midi);
            psdev::setAllocationTracking (false);
        }
        const auto after = psdev::getAllocationCounts();
        check (after.allocations == counts.allocations && after.frees == counts.frees,
               "the audio thread does not allocate while a clip moves a parameter");
        proc.flushPendingChanges (true);
        check (proc.getHistory().size() == before, "a clip by itself adds nothing to the history");
        check (std::abs (proc.getParameterValue ("max.gain") - 20.0f) < 1.0e-3f, "a clip's last value is the parameter's value");

        // The clip keeps playing while the user edits another control: undo takes back the edit, and leaves the
        // automated control to its clip.
        auto* ceiling = proc.getParameter ("max.ceiling");
        automate (*gain, 0.25f);
        ceiling->beginChangeGesture();
        ceiling->setValueNotifyingHost (0.5f);
        ceiling->endChangeGesture();
        automate (*gain, 0.75f);
        proc.flushPendingChanges (true);
        const auto automated = proc.getParameterValue ("max.gain");
        proc.undo();
        check (std::abs (proc.getParameterValue ("max.gain") - automated) < 1.0e-3f, "undo leaves a control under automation alone",
               juce::String (proc.getParameterValue ("max.gain")) + " after undo, was " + juce::String (automated));

        // Taking hold of it hands the control back to undo.
        juce::Thread::sleep (600);
        gain->beginChangeGesture();
        gain->setValueNotifyingHost (0.5f);
        gain->endChangeGesture();
        proc.flushPendingChanges (true);
        check (std::abs (proc.getParameterValue ("max.gain") - 10.0f) < 1.0e-3f, "a grabbed control takes the user's value");
        proc.undo();
        check (std::abs (proc.getParameterValue ("max.gain") - 10.0f) > 0.1f, "undo takes back an edit of a control a clip was moving");

        // A host echoing the end of a drag is not a clip: the edit stays undoable.
        auto* character = proc.getParameter ("max.character");
        character->beginChangeGesture();
        character->setValueNotifyingHost (0.2f);
        character->setValueNotifyingHost (0.4f);
        character->endChangeGesture();
        automate (*character, 0.2f);
        automate (*character, 0.4f);
        proc.flushPendingChanges (true);
        const auto dragged = proc.getParameterValue ("max.character");
        proc.undo();
        check (std::abs (proc.getParameterValue ("max.character") - dragged) > 0.1f, "a host echoing a drag does not count as a clip");

        // Our own edits are echoed too: a late echo after a click keeps the click undoable.
        juce::Thread::sleep (600);
        proc.setParameterValue ("max.upward", 3.0f);
        automate (*proc.getParameter ("max.upward"), proc.getParameter ("max.upward")->convertTo0to1 (2.0f));
        automate (*proc.getParameter ("max.upward"), proc.getParameter ("max.upward")->convertTo0to1 (3.0f));
        proc.flushPendingChanges (true);
        proc.undo();
        check (std::abs (proc.getParameterValue ("max.upward") - 3.0f) > 0.1f, "a host echoing a click does not count as a clip");

        // A forced commit while the user still holds a control: its movement stays the user's.
        juce::Thread::sleep (600);
        auto* softClip = proc.getParameter ("max.clip.amount");
        auto* held = proc.getParameter ("max.trans.amount");
        softClip->beginChangeGesture();
        softClip->setValueNotifyingHost (0.3f);
        softClip->endChangeGesture();
        held->beginChangeGesture();
        held->setValueNotifyingHost (0.3f);
        proc.flushPendingChanges (true);
        juce::Thread::sleep (600);
        held->setValueNotifyingHost (0.6f);
        held->endChangeGesture();
        proc.flushPendingChanges (true);
        const auto heldValue = proc.getParameterValue ("max.trans.amount");
        proc.undo();
        check (std::abs (proc.getParameterValue ("max.trans.amount") - heldValue) > 1.0f,
               "a control held through a forced commit stays undoable");

        // A restored project starts with nothing under automation, and a clip does not mark the preset as modified.
        juce::MemoryBlock saved;
        proc.getStateInformation (saved);
        proc.setStateInformation (saved.getData(), (int) saved.getSize());
        const auto restoredModified = proc.isPresetModified();
        juce::Thread::sleep (600);
        automate (*proc.getParameter ("max.upward"), 0.7f);
        check (proc.isPresetModified() == restoredModified, "a clip does not mark the preset as modified");
        proc.releaseResources();
    }

    std::cout << numPassed << " passed, " << numFailed << " failed\n";
    return numFailed == 0 ? 0 : 1;
}
