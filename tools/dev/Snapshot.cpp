#include "core/Loudness.h"
#include "ui/MatchDisplay.h"   // ui-meq block (Match EQ spectra, display interaction check)
#include "ui/DynamicEqDisplay.h"   // ui-deq block
#include "ui/DynamicEqPanel.h"     // ui-deq block
#include "ui/SmootherPanel.h"   // ui-spectral block
#include "ui/SpectralDisplay.h"   // ui-spectral block
#include "ui/BassScope.h"          // ui-lowend block
#include "ui/LowEndWidgets.h"      // ui-lowend block
#include "ui/LowRegionDisplay.h"   // ui-lowend block
#include "plugin/PluginProcessor.h"
#include "ui/ChainStrip.h"
#include "ui/ModulePanel.h"
#include "ui/PluginEditor.h"

#include <cmath>
#include <iostream>

// PureSheenDev snapshot <outDir> (owner: ui-shell)
//
// Renders the editor to PNG files in several states: the default chain, every module selected (Equalizer 2 is added
// for its shot), Mid/Side mode, the Dither panel, the History popup, the Help overlay, a chain drag in progress,
// meter / spectrum option variants, and the sizes 980x600, 1180x720, 1600x1000 and 1800x1100.
//
// A deterministic synthetic song (kick, bass, chords, hats; LCG noise, no time) is played through the real processor
// in 1600-sample blocks between editor ticks, so meters and spectra show real measurements (the chain feeds its own UI
// taps and meters).
//
// UI checks (any failure fails the command): every module panel builds controls, the chain feeds its UI taps, nothing
// in the editor takes keyboard focus, the History popup and Help overlay open, a chain drag reorders the processor's
// chain.
namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
}

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 1600;   // one 30 Hz tick at 48 kHz

void pumpMessages (int milliseconds)
{
   #if JUCE_WINDOWS || JUCE_LINUX
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) milliseconds;

    while (juce::Time::getMillisecondCounter() < end)
        if (! juce::detail::dispatchNextMessageOnSystemQueue (true))
            juce::Thread::sleep (2);
   #else
    juce::ignoreUnused (milliseconds);
   #endif
}

bool writeImage (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat png;
    const bool ok = out.openedOk() && png.writeImageToStream (image, out);
    std::cout << (ok ? "wrote " : "FAILED ") << file.getFullPathName() << " (" << image.getWidth() << "x"
              << image.getHeight() << ")\n";
    return ok;
}

bool writePng (juce::Component& c, const juce::File& file)
{
    return writeImage (c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f), file);
}

//==============================================================================
// Deterministic synthetic master-bus material at 124 BPM.
struct SongGenerator
{
    struct Lcg
    {
        juce::uint32 state = 0x2545f491u;
        float next() noexcept   // -1..1
        {
            state = state * 1664525u + 1013904223u;
            return (float) (state >> 8) / (float) (1u << 23) - 1.0f;
        }
    };

    void render (float* l, float* r, int n)
    {
        const double beat = 60.0 / 124.0;
        static const double chordHz[4][3] = { { 220.0, 261.6, 329.6 }, { 174.6, 220.0, 261.6 },
                                              { 196.0, 246.9, 293.7 }, { 164.8, 207.7, 246.9 } };

        for (int i = 0; i < n; ++i, ++sample)
        {
            const double t = (double) sample / kSampleRate;
            const double inBeat = std::fmod (t, beat);
            const int bar = (int) (t / (beat * 4.0)) % 4;

            // Kick: pitch sweep 120 -> 45 Hz, fast decay.
            kickPhase += juce::MathConstants<double>::twoPi * (45.0 + 75.0 * std::exp (-inBeat * 30.0)) / kSampleRate;
            const float kick = (float) (0.85 * std::sin (kickPhase) * std::exp (-inBeat * 7.0));

            // Bass: root of the chord one octave down twice, off-beat pump.
            const double bassHz = chordHz[bar][0] * 0.25;
            bassPhase += juce::MathConstants<double>::twoPi * bassHz / kSampleRate;
            const float pump = (float) (1.0 - 0.7 * std::exp (-inBeat * 9.0));
            const float bass = (float) (0.32 * pump * (std::sin (bassPhase) + 0.25 * std::sin (2.0 * bassPhase)));

            // Chords: three detuned saw-ish voices, slightly different left / right.
            float padL = 0.0f, padR = 0.0f;

            for (int v = 0; v < 3; ++v)
            {
                padPhase[v] += chordHz[bar][v] / kSampleRate;
                padPhase[v] -= std::floor (padPhase[v]);
                const double ph = padPhase[v];
                const float saw = (float) (std::sin (juce::MathConstants<double>::twoPi * ph) + 0.5 * std::sin (2.0 * juce::MathConstants<double>::twoPi * ph)
                                           + 0.25 * std::sin (3.0 * juce::MathConstants<double>::twoPi * ph) + 0.12 * std::sin (5.0 * juce::MathConstants<double>::twoPi * ph));
                padL += saw * (v == 2 ? 0.6f : 1.0f);
                padR += saw * (v == 0 ? 0.6f : 1.0f);
            }

            padL *= 0.055f * pump;
            padR *= 0.055f * pump;

            // Hats: high-passed noise on the off-beats.
            const double hatT = std::fmod (t + beat * 0.5, beat);
            const float noise = rng.next();
            hatHp = 0.86f * (hatHp + noise - lastNoise);
            lastNoise = noise;
            const float hat = (float) (0.16 * hatHp * std::exp (-hatT * 40.0));
            const float air = 0.006f * rng.next();

            l[i] = kick + bass + padL + hat * 0.8f + air;
            r[i] = kick + bass + padR + hat + air;
        }
    }

    long long sample = 0;
    double kickPhase = 0.0, bassPhase = 0.0, padPhase[3] {};
    float hatHp = 0.0f, lastNoise = 0.0f;
    Lcg rng;
};

//==============================================================================
struct Rig
{
    Rig()
    {
        processor.setPlayConfigDetails (2, 2, kSampleRate, kBlock);
        processor.prepareToPlay (kSampleRate, kBlock);
        buffer.setSize (2, kBlock);
    }

    // Plays `blocks` blocks through the processor, ticking the editor after each.
    void play (PureSheenEditor& editor, int blocks)
    {
        auto& chain = processor.getChain();

        for (int b = 0; b < blocks; ++b)
        {
            song.render (buffer.getWritePointer (0), buffer.getWritePointer (1), kBlock);
            const auto before = chain.getOutputFifo().getWriteCount();
            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);

            if (chain.getOutputFifo().getWriteCount() == before)
                tapsFed = false;

            editor.updateNow();
        }
    }

    PureSheenProcessor processor;
    SongGenerator song;
    juce::AudioBuffer<float> buffer;
    bool tapsFed = true;
};

void set (PureSheenProcessor& p, const juce::String& id, float v)
{
    if (p.getParameter (id) == nullptr)
        std::cout << "warning: no parameter " << id << "\n";

    p.setParameterValue (id, v);
}

// Settings that make every panel look in use (a typical gentle master).
void dialInMaster (PureSheenProcessor& p)
{
    set (p, "eq1.a.b1.on", 1.0f);  set (p, "eq1.a.b1.shape", (float) ps::EqShape::HighPassFlat); set (p, "eq1.a.b1.freq", 28.0f);
    set (p, "eq1.a.b2.on", 1.0f);  set (p, "eq1.a.b2.shape", (float) ps::EqShape::LowShelfAnalog); set (p, "eq1.a.b2.freq", 90.0f); set (p, "eq1.a.b2.gain", 1.5f);
    set (p, "eq1.a.b3.on", 1.0f);  set (p, "eq1.a.b3.freq", 320.0f); set (p, "eq1.a.b3.gain", -2.0f); set (p, "eq1.a.b3.q", 1.4f);
    set (p, "eq1.a.b4.on", 1.0f);  set (p, "eq1.a.b4.freq", 3200.0f); set (p, "eq1.a.b4.gain", 1.2f);
    set (p, "eq1.a.b5.on", 1.0f);  set (p, "eq1.a.b5.shape", (float) ps::EqShape::HighShelfBaxandall); set (p, "eq1.a.b5.freq", 11000.0f); set (p, "eq1.a.b5.gain", 2.0f);

    set (p, "dyn.bands", 3.0f);
    set (p, "dyn.xover1", 150.0f);
    set (p, "dyn.xover2", 2500.0f);
    set (p, "dyn.a.b1.comp.thr", -18.0f); set (p, "dyn.a.b1.comp.ratio", 2.5f);
    set (p, "dyn.a.b2.comp.thr", -14.0f); set (p, "dyn.a.b2.comp.ratio", 1.8f);
    set (p, "dyn.a.b3.comp.thr", -20.0f); set (p, "dyn.a.b3.lim.on", 1.0f);

    set (p, "img.bands", 3.0f);
    set (p, "img.b1.width", -40.0f);
    set (p, "img.b2.width", 10.0f);
    set (p, "img.b3.width", 25.0f);

    set (p, "exc.bands", 2.0f);
    set (p, "exc.xover1", 4000.0f);
    set (p, "exc.a.b1.drive", 12.0f);
    set (p, "exc.a.b2.mode", (float) ps::ExciterMode::Tape);
    set (p, "exc.a.b2.drive", 30.0f);

    set (p, "max.gain", 4.0f);
    set (p, "max.ceiling", -1.0f);

    set (p, "in.gainL", -1.5f);
    set (p, "in.gainR", -1.5f);
}

bool checkNoFocus (juce::Component& root)
{
    if (dynamic_cast<juce::TextEditor*> (&root) == nullptr && (root.getWantsKeyboardFocus() || root.getMouseClickGrabsKeyboardFocus()))
    {
        std::cout << "FAILED: \"" << root.getTitle() << "\" (" << typeid (root).name() << ") can take keyboard focus\n";
        return false;
    }

    bool ok = true;

    for (auto* c : root.getChildren())
        ok = checkNoFocus (*c) && ok;

    return ok;
}

template <typename T>
T* findChild (juce::Component& root)
{
    if (auto* t = dynamic_cast<T*> (&root))
        return t;

    for (auto* c : root.getChildren())
        if (auto* t = findChild<T> (*c))
            return t;

    return nullptr;
}
} // namespace

//==============================================================================
int runSnapshot (const juce::File& outputDir)
{
    if (! outputDir.createDirectory())
    {
        std::cout << "FAILED: cannot create " << outputDir.getFullPathName() << "\n";
        return 1;
    }

    Rig rig;
    auto& proc = rig.processor;
    dialInMaster (proc);

    std::unique_ptr<juce::AudioProcessorEditor> holder (proc.createEditor());
    auto* editor = dynamic_cast<PureSheenEditor*> (holder.get());

    if (editor == nullptr)
    {
        std::cout << "FAILED: createEditor() did not return a PureSheenEditor\n";
        return 1;
    }

    bool ok = true;
    const auto file = [&] (const juce::String& name) { return outputDir.getChildFile (name + ".png"); };

    // Warm up: ~4 s of audio so loudness, averaging and peak holds settle.
    editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
    editor->selectModule ((int) ps::ModuleType::Equalizer1);
    rig.play (*editor, 120);

    // 01: default chain
    ok &= writePng (*editor, file ("01_default_1180x720"));

    // ---- BEGIN ui-spectral: Tone Balance, Definition and Smoother panels (every size with live meters, Tone Balance
    //      Cut / M/S Side / T/S Sustain, Definition region solo and M/S Side, Smoother Trace view, Learn, region solo) ----
    // Set PURESHEEN_SNAPSHOT_ONLY=ui-spectral to stop after this block (quick iteration on these shots).
    {
        struct SpecSize { int w, h; const char* name; };
        const SpecSize specSizes[] = { { 980, 600, "980x600" }, { 1180, 720, "1180x720" }, { 1600, 1000, "1600x1000" },
                                       { 1800, 1100, "1800x1100" } };
        const auto specMode = [&] (ps::ModuleType t, ps::ChannelMode m)
        {
            for (const auto& idAndValue : ps::channelModeParamValues (t, m))
                set (proc, idAndValue.first, idAndValue.second);
        };
        const auto specShot = [&] (const juce::String& name) { ok &= writePng (*editor, file (name)); };
        const auto specSolo = [&] (ps::ModuleType t, bool on)
        {
            auto m = proc.getMonitor (t);
            m.soloRegion = on;
            proc.setMonitor (t, m);
        };

        proc.addModule (ps::ModuleType::ToneBalance, 1);
        proc.addModule (ps::ModuleType::Definition, 2);
        proc.addModule (ps::ModuleType::Smoother, 3);
        pumpMessages (30);

        // A typical use of each: Tone Balance toward Pop, a little Definition with a brighter tilt, Smoother on the highs.
        // EQ 1 (first in the chain) brightens the hats and adds a narrow +12 dB resonance at 4.5 kHz, so the Smoother
        // has a harsh spot to tame.
        set (proc, "eq1.a.b6.on", 1.0f);
        set (proc, "eq1.a.b6.freq", 4500.0f);
        set (proc, "eq1.a.b6.gain", 12.0f);
        set (proc, "eq1.a.b6.q", 6.0f);
        set (proc, "eq1.a.b7.on", 1.0f);
        set (proc, "eq1.a.b7.shape", (float) ps::EqShape::HighShelfAnalog);
        set (proc, "eq1.a.b7.freq", 2500.0f);
        set (proc, "eq1.a.b7.gain", 12.0f);
        set (proc, "tbal.target", (float) ps::ToneTarget::Pop);
        set (proc, "tbal.a.amount", 80.0f);
        set (proc, "tbal.a.high", 70.0f);
        set (proc, "defn.a.amount", 70.0f);
        set (proc, "defn.a.tilt", 1.5f);
        set (proc, "defn.a.low", 420.0f);
        set (proc, "smth.a.amount", 100.0f);
        set (proc, "smth.a.mode", (float) ps::SmootherMode::Strong);
        set (proc, "smth.a.low", 1800.0f);
        set (proc, "smth.a.high", 14000.0f);

        for (auto type : { ps::ModuleType::ToneBalance, ps::ModuleType::Definition, ps::ModuleType::Smoother })
        {
            editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
            editor->selectModule ((int) type);
            rig.play (*editor, 90);

            for (const auto& sz : specSizes)
            {
                editor->setSize (sz.w, sz.h);
                rig.play (*editor, 4);
                specShot (juce::String ("40_") + ps::moduleInfo (type).idPrefix + "_" + sz.name);
            }
        }

        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);

        // Mouse checks on the displays (Stereo): a cutoff handle drag, a region pill drag (both cutoffs), a zone drag
        // are one undo step each; a double-click resets a cutoff; a right-click edits nothing; S toggles region solo.
        {
            const auto event = [] (juce::Component* c, juce::Point<float> pos, juce::Point<float> downPos, juce::ModifierKeys mods,
                                   int clicks)
            {
                const auto now = juce::Time::getCurrentTime();
                return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                                         juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, c, c, now, downPos,
                                         now, clicks, pos != downPos);
            };
            const juce::ModifierKeys left (juce::ModifierKeys::leftButtonModifier);
            const juce::ModifierKeys right (juce::ModifierKeys::rightButtonModifier);
            const auto value = [&] (const char* id) { return proc.getParameterValue (id); };
            const auto steps = [&]
            {
                pumpMessages (300);
                rig.play (*editor, 1);
                return (int) proc.getHistory().size();
            };
            const auto report = [&] (bool pass, const char* what)
            {
                std::cout << (pass ? "spectral display: " : "FAILED: spectral display: ") << what << (pass ? ": ok\n" : "\n");
                ok &= pass;
            };
            const auto drag = [&] (ps::ui::SpectralDisplay* d, juce::Point<float> from, juce::Point<float> by)
            {
                d->mouseDown (event (d, from, from, left, 1));
                for (int i = 1; i <= 10; ++i)
                    d->mouseDrag (event (d, from + by * ((float) i / 10.0f), from, left, 1));
                d->mouseUp (event (d, from + by, from, left, 1));
            };

            editor->selectModule ((int) ps::ModuleType::Definition);
            rig.play (*editor, 4);
            auto* defnPanel = editor->getCurrentPanel();
            auto* d = defnPanel != nullptr ? findChild<ps::ui::SpectralDisplay> (*defnPanel) : nullptr;

            if (d == nullptr)
            {
                std::cout << "FAILED: no spectral display in the Definition panel\n";
                ok = false;
            }
            else
            {
                proc.clearHistory();
                int h = steps();
                const float high0 = value ("defn.a.high");
                drag (d, d->getHandleArea (true).getCentre(), { -120.0f, 0.0f });
                int h2 = steps();
                std::cout << "  high cutoff " << high0 << " -> " << value ("defn.a.high") << " Hz, history " << h << " -> " << h2 << std::endl;
                report (value ("defn.a.high") < high0 * 0.8f && h2 == h + 1, "drag the high cutoff in one undo step");

                h = h2;
                const float low1 = value ("defn.a.low"), high1 = value ("defn.a.high");
                drag (d, d->getRegionPillArea().getCentre().translated (-20.0f, 0.0f), { 60.0f, 0.0f });
                h2 = steps();
                report (value ("defn.a.low") > low1 * 1.05f && value ("defn.a.high") > high1 * 1.05f
                            && std::abs (value ("defn.a.high") / value ("defn.a.low") - high1 / low1) < 0.02f * high1 / low1
                            && h2 == h + 1, "drag the region pill = both cutoffs, ratio kept, one undo step");

                h = h2;
                const auto lowHandle = d->getHandleArea (false).getCentre();
                d->mouseDoubleClick (event (d, lowHandle, lowHandle, left, 2));
                h2 = steps();
                report (std::abs (value ("defn.a.low") - 300.0f) < 0.5f && h2 == h + 1, "double-click resets the low cutoff");

                h = h2;
                const float high2 = value ("defn.a.high");
                const auto hh = d->getHandleArea (true).getCentre();
                d->mouseDown (event (d, hh, hh, right, 1));
                d->mouseDrag (event (d, hh.translated (-80.0f, 0.0f), hh, right, 1));
                d->mouseUp (event (d, hh.translated (-80.0f, 0.0f), hh, right, 1));
                pumpMessages (50);
                juce::PopupMenu::dismissAllActiveMenus();
                h2 = steps();
                report (value ("defn.a.high") == high2 && h2 == h, "right-click edits nothing");

                const auto solo = d->getSoloButtonArea().getCentre();
                d->mouseDown (event (d, solo, solo, left, 1));
                d->mouseUp (event (d, solo, solo, left, 1));
                const bool soloOn = proc.getMonitor (ps::ModuleType::Definition).soloRegion;
                d->mouseDown (event (d, solo, solo, left, 1));
                d->mouseUp (event (d, solo, solo, left, 1));
                report (soloOn && ! proc.getMonitor (ps::ModuleType::Definition).soloRegion, "S toggles the region solo");

                set (proc, "defn.a.low", 420.0f);
                set (proc, "defn.a.high", 20000.0f);
            }

            editor->selectModule ((int) ps::ModuleType::ToneBalance);
            rig.play (*editor, 4);
            auto* tbalPanel = editor->getCurrentPanel();
            auto* t = tbalPanel != nullptr ? findChild<ps::ui::SpectralDisplay> (*tbalPanel) : nullptr;

            if (t == nullptr)
            {
                std::cout << "FAILED: no spectral display in the Tone Balance panel\n";
                ok = false;
            }
            else
            {
                int h = steps();
                drag (t, t->getZoneHeaderArea (1).getCentre(), { -44.0f, 0.0f });
                const int h2 = steps();
                report (std::abs (value ("tbal.a.mid") - 80.0f) < 1.0f && h2 == h + 1, "drag a zone = its amount in one undo step");
                set (proc, "tbal.a.mid", 100.0f);
            }
        }

        // Tone Balance: Cut mode; M/S with the Side tab (its own amount and Low region at 40 %); T/S with Sustain.
        editor->selectModule ((int) ps::ModuleType::ToneBalance);
        set (proc, "tbal.mode", (float) ps::ToneBalanceMode::Cut);
        set (proc, "tbal.a.sens", 80.0f);
        rig.play (*editor, 60);
        specShot ("41_tbal_cut_1180x720");
        set (proc, "tbal.mode", (float) ps::ToneBalanceMode::Shape);
        set (proc, "tbal.target", (float) ps::ToneTarget::Flat);
        specMode (ps::ModuleType::ToneBalance, ps::ChannelMode::MidSide);
        set (proc, "tbal.link", 0.0f);
        set (proc, "tbal.b.amount", 60.0f);
        set (proc, "tbal.b.low", 40.0f);
        rig.play (*editor, 6);
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (1);
        rig.play (*editor, 60);
        specShot ("41_tbal_midside_side_1180x720");
        editor->setSize (PureSheenEditor::kMinWidth, PureSheenEditor::kMinHeight);
        rig.play (*editor, 4);
        specShot ("41_tbal_midside_side_980x600");
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        specMode (ps::ModuleType::ToneBalance, ps::ChannelMode::TransientSustain);
        rig.play (*editor, 60);
        specShot ("41_tbal_ts_sustain_1180x720");
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (0);
        specMode (ps::ModuleType::ToneBalance, ps::ChannelMode::Stereo);

        // Definition: region solo; M/S with the Side tab (darker tilt, narrower region).
        editor->selectModule ((int) ps::ModuleType::Definition);
        specSolo (ps::ModuleType::Definition, true);
        rig.play (*editor, 30);
        specShot ("42_defn_region_solo_1180x720");
        specSolo (ps::ModuleType::Definition, false);
        specMode (ps::ModuleType::Definition, ps::ChannelMode::MidSide);
        set (proc, "defn.link", 0.0f);
        set (proc, "defn.b.amount", 50.0f);
        set (proc, "defn.b.tilt", -2.0f);
        set (proc, "defn.b.low", 1200.0f);
        set (proc, "defn.b.high", 9000.0f);
        rig.play (*editor, 6);
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (1);
        rig.play (*editor, 40);
        specShot ("42_defn_midside_side_1180x720");
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (0);
        specMode (ps::ModuleType::Definition, ps::ChannelMode::Stereo);

        // Smoother: Trace view; Learn in progress, then its result; region solo at 980x600.
        editor->selectModule ((int) ps::ModuleType::Smoother);
        rig.play (*editor, 10);
        auto* smoother = dynamic_cast<ps::ui::SmootherPanel*> (editor->getCurrentPanel());

        if (smoother == nullptr)
        {
            std::cout << "FAILED: the Smoother panel is not a SmootherPanel\n";
            ok = false;
        }
        else
        {
            smoother->setTraceView (true);
            rig.play (*editor, 150);
            specShot ("43_smth_trace_1180x720");
            smoother->setTraceView (false);

            const float lowBefore = proc.getParameterValue ("smth.a.low");
            smoother->startLearn();
            rig.play (*editor, 100);
            const bool learningShown = smoother->isLearning();
            specShot ("43_smth_learning_1180x720");
            rig.play (*editor, 100);
            const float lowAfter = proc.getParameterValue ("smth.a.low"), highAfter = proc.getParameterValue ("smth.a.high");
            const bool learned = learningShown && ! smoother->isLearning() && lowAfter != lowBefore && highAfter > lowAfter;
            std::cout << (learned ? "smoother learn wrote a region: ok" : "FAILED: smoother learn did not write a region")
                      << " (" << lowAfter << " - " << highAfter << " Hz)" << std::endl;
            ok &= learned;
            specShot ("43_smth_learned_1180x720");

            editor->setSize (PureSheenEditor::kMinWidth, PureSheenEditor::kMinHeight);
            specSolo (ps::ModuleType::Smoother, true);
            rig.play (*editor, 20);
            specShot ("43_smth_region_solo_980x600");
            editor->selectModule ((int) ps::ModuleType::Equalizer1);   // (the Smoother panel is destroyed here)
            rig.play (*editor, 2);
            const bool cleared = ! proc.getMonitor (ps::ModuleType::Smoother).soloRegion;
            std::cout << (cleared ? "Smoother region solo cleared with its panel: ok"
                                  : "FAILED: the Smoother region solo outlived its panel") << std::endl;
            ok &= cleared;
            specSolo (ps::ModuleType::Smoother, false);
        }

        ok &= checkNoFocus (*editor);

        for (auto type : { ps::ModuleType::ToneBalance, ps::ModuleType::Definition, ps::ModuleType::Smoother })
            proc.resetModule (type);

        for (const char* id : { "eq1.a.b6.on", "eq1.a.b6.freq", "eq1.a.b6.gain", "eq1.a.b6.q", "eq1.a.b7.on",
                                "eq1.a.b7.shape", "eq1.a.b7.freq", "eq1.a.b7.gain" })
            set (proc, id, ps::paramSpecs()[(size_t) ps::paramIndex (id)].defaultValue);

        proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
        pumpMessages (30);
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        editor->selectModule ((int) ps::ModuleType::Equalizer1);
        rig.play (*editor, 4);

        if (juce::SystemStats::getEnvironmentVariable ("PURESHEEN_SNAPSHOT_ONLY", {}) == "ui-spectral")
        {
            holder.reset();
            std::cout << (ok ? "snapshot (ui-spectral only): OK\n" : "snapshot (ui-spectral only): FAILED\n");
            return ok ? 0 : 1;
        }
    }
    // ---- END ui-spectral ----

    // ---- BEGIN ui-deq: Dynamic EQ panel (bands in Stereo at every size, Digital, M/S Side, T/S Sustain) ----
    // Set PURESHEEN_SNAPSHOT_ONLY=ui-deq to stop after this block (quick iteration on the Dynamic EQ shots).
    {
        struct DeqSize { int w, h; const char* name; };
        const DeqSize deqSizes[] = { { 980, 600, "980x600" }, { 1180, 720, "1180x720" }, { 1600, 1000, "1600x1000" },
                                     { 1800, 1100, "1800x1100" } };
        proc.addModule (ps::ModuleType::DynamicEq, 1);
        pumpMessages (30);
        editor->selectModule ((int) ps::ModuleType::DynamicEq);

        const auto band = [&] (const char* channelSet, int b, const char* name, float v)
        {
            set (proc, juce::String ("deq.") + channelSet + ".b" + juce::String (b) + "." + name, v);
        };
        // A gentle dynamic master: low shelf boost that backs off on loud bass, mud cut, harshness cut, air that grows
        // when loud, one static band.
        band ("a", 1, "on", 1.0f); band ("a", 1, "shape", (float) ps::DynEqShape::LowShelfBaxandall); band ("a", 1, "freq", 90.0f);
        band ("a", 1, "gain", 3.0f); band ("a", 1, "thr", -24.0f); band ("a", 1, "ratio", 3.0f);
        band ("a", 2, "on", 1.0f); band ("a", 2, "freq", 320.0f); band ("a", 2, "gain", -5.0f); band ("a", 2, "q", 1.2f);
        band ("a", 2, "thr", -30.0f); band ("a", 2, "ratio", 4.0f);
        band ("a", 3, "on", 1.0f); band ("a", 3, "shape", (float) ps::DynEqShape::Bell); band ("a", 3, "freq", 3200.0f);
        band ("a", 3, "gain", -6.0f); band ("a", 3, "q", 2.0f); band ("a", 3, "thr", -42.0f); band ("a", 3, "ratio", 6.0f);
        band ("a", 4, "on", 1.0f); band ("a", 4, "freq", 1500.0f); band ("a", 4, "gain", 1.0f); band ("a", 4, "dyn", 0.0f);
        band ("a", 5, "on", 1.0f); band ("a", 5, "shape", (float) ps::DynEqShape::HighShelfBaxandall); band ("a", 5, "freq", 10000.0f);
        band ("a", 5, "gain", 3.0f); band ("a", 5, "dir", 1.0f); band ("a", 5, "thr", -60.0f);

        if (auto* deqPanel = dynamic_cast<ps::ui::DynamicEqPanel*> (editor->getCurrentPanel()))
            deqPanel->setSelectedBand (2);
        else
        {
            std::cout << "FAILED: the Dynamic EQ panel is not a DynamicEqPanel\n";
            ok = false;
        }

        for (const auto& sz : deqSizes)
        {
            editor->setSize (sz.w, sz.h);
            rig.play (*editor, 60);
            ok &= writePng (*editor, file (juce::String ("30_deq_") + sz.name));
        }

        // Digital mode with lookahead
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        set (proc, "deq.mode", 1.0f);
        set (proc, "deq.lookahead", 5.0f);
        rig.play (*editor, 60);
        ok &= writePng (*editor, file ("30_deq_digital_1180x720"));
        set (proc, "deq.mode", 0.0f);
        set (proc, "deq.lookahead", 0.0f);

        // Mid/Side, Link off, the Side tab: a side de-esser and a static side low cut
        set (proc, "deq.chMode", 1.0f);
        set (proc, "deq.link", 0.0f);
        band ("b", 1, "on", 1.0f); band ("b", 1, "freq", 6500.0f); band ("b", 1, "gain", -8.0f); band ("b", 1, "q", 2.5f);
        band ("b", 1, "thr", -55.0f); band ("b", 1, "ratio", 5.0f);
        band ("b", 2, "on", 1.0f); band ("b", 2, "shape", (float) ps::DynEqShape::LowShelfAnalog); band ("b", 2, "freq", 160.0f);
        band ("b", 2, "gain", -6.0f); band ("b", 2, "dyn", 0.0f);

        for (const auto& sz : { deqSizes[1], deqSizes[0] })
        {
            editor->setSize (sz.w, sz.h);
            rig.play (*editor, 6);
            if (auto* panel = editor->getCurrentPanel())
                panel->setChannelSet (1);
            if (auto* deqPanel = dynamic_cast<ps::ui::DynamicEqPanel*> (editor->getCurrentPanel()))
                deqPanel->setSelectedBand (0);
            rig.play (*editor, 60);
            ok &= writePng (*editor, file (juce::String ("30_deq_midside_side_") + sz.name));
            if (auto* panel = editor->getCurrentPanel())
                panel->setChannelSet (0);
        }

        // Transient/Sustain, the Sustain tab (set B)
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        set (proc, "deq.chMode", 3.0f);
        rig.play (*editor, 6);
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (1);
        rig.play (*editor, 60);
        ok &= writePng (*editor, file ("30_deq_transient_sustain_1180x720"));
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (0);

        // Mouse checks on the display (Stereo, band 3 selected): a node drag moves freq + gain in one undo step, the
        // wheel changes Q (one step per burst), the threshold bar drags the threshold, Alt-click solos a band while
        // held, a right-click edits nothing, a double-click on empty space turns on the next free band.
        {
            set (proc, "deq.chMode", 0.0f);
            rig.play (*editor, 4);
            auto* deqPanel = dynamic_cast<ps::ui::DynamicEqPanel*> (editor->getCurrentPanel());
            auto* display = deqPanel != nullptr ? findChild<ps::ui::DynamicEqDisplay> (*deqPanel) : nullptr;

            if (display == nullptr)
            {
                std::cout << "FAILED: no Dynamic EQ display\n";
                ok = false;
            }
            else
            {
                deqPanel->setSelectedBand (2);
                rig.play (*editor, 2);
                auto event = [display] (juce::Point<float> pos, juce::Point<float> downPos, juce::ModifierKeys mods, int clicks)
                {
                    const auto now = juce::Time::getCurrentTime();
                    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, display, display,
                                             now, downPos, now, clicks, pos != downPos);
                };
                const juce::ModifierKeys left (juce::ModifierKeys::leftButtonModifier);
                const auto value = [&] (const char* id) { return proc.getParameterValue (id); };
                const auto steps = [&]
                {
                    pumpMessages (300);
                    rig.play (*editor, 1);
                    return (int) proc.getHistory().size();
                };
                const auto report = [&] (bool pass, const char* what)
                {
                    std::cout << (pass ? "deq display: " : "FAILED: deq display: ") << what << (pass ? ": ok\n" : "\n");
                    ok &= pass;
                };
                proc.clearHistory();

                // node drag: freq + gain, one undo step
                int h = steps();
                const float f0 = value ("deq.a.b3.freq"), g0 = value ("deq.a.b3.gain");
                const auto p0 = display->getNodePosition (2);
                display->mouseDown (event (p0, p0, left, 1));
                for (int i = 1; i <= 10; ++i)
                    display->mouseDrag (event (p0 + juce::Point<float> (6.0f * (float) i, -2.0f * (float) i), p0, left, 1));
                display->mouseUp (event (p0 + juce::Point<float> (60.0f, -20.0f), p0, left, 1));
                int h2 = steps();
                report (value ("deq.a.b3.freq") > f0 * 1.05f && value ("deq.a.b3.gain") > g0 + 0.5f && h2 == h + 1,
                        "drag a node = freq + gain in one undo step");

                // wheel over the node: Q up, one step per burst (ended by the panel tick after 450 ms)
                h = h2;
                const float q0 = value ("deq.a.b3.q");
                const auto p1 = display->getNodePosition (2);
                juce::MouseWheelDetails wheel { 0.0f, 0.25f, false, false, false };
                display->mouseWheelMove (event (p1, p1, {}, 0), wheel);
                display->mouseWheelMove (event (p1, p1, {}, 0), wheel);
                pumpMessages (500);
                h2 = steps();
                report (value ("deq.a.b3.q") > q0 * 1.05f && h2 == h + 1, "wheel = Q in one undo step");

                // threshold bar drag
                h = h2;
                const float t0 = value ("deq.a.b3.thr");
                const auto th = display->getThresholdHandlePosition();
                display->mouseDown (event (th, th, left, 1));
                for (int i = 1; i <= 5; ++i)
                    display->mouseDrag (event (th - juce::Point<float> (0.0f, 4.0f * (float) i), th, left, 1));
                display->mouseUp (event (th - juce::Point<float> (0.0f, 20.0f), th, left, 1));
                h2 = steps();
                report (th.x > 0.0f && value ("deq.a.b3.thr") > t0 + 2.0f && h2 == h + 1, "drag the threshold bar in one undo step");

                // Alt-click a node: band solo while held
                const auto p2 = display->getNodePosition (2);
                display->mouseDown (event (p2, p2, left.withFlags (juce::ModifierKeys::altModifier), 1));
                const bool soloOn = proc.getMonitor (ps::ModuleType::DynamicEq).soloBand == 2;
                display->mouseUp (event (p2, p2, left.withFlags (juce::ModifierKeys::altModifier), 1));
                report (soloOn && proc.getMonitor (ps::ModuleType::DynamicEq).soloBand == -1, "Alt-click = band solo while held");

                // right-click: a menu, no edit
                h = steps();
                const float f1 = value ("deq.a.b3.freq"), g1 = value ("deq.a.b3.gain");
                display->mouseDown (event (p2, p2, juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier), 1));
                display->mouseUp (event (p2, p2, juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier), 1));
                pumpMessages (50);
                juce::PopupMenu::dismissAllActiveMenus();
                h2 = steps();
                report (value ("deq.a.b3.freq") == f1 && value ("deq.a.b3.gain") == g1 && h2 == h, "right-click edits nothing");

                // double-click on empty space: band 6 turns on there, selected
                const auto empty = juce::Point<float> ((float) display->getWidth() * 0.45f, (float) display->getHeight() * 0.25f);
                display->mouseDoubleClick (event (empty, empty, left, 2));
                h2 = steps();
                report (value ("deq.a.b6.on") > 0.5f && value ("deq.a.b6.gain") > 0.5f && deqPanel->getSelectedBand() == 5
                            && h2 == h + 1, "double-click adds a band in one undo step");
            }
        }

        ok &= checkNoFocus (*editor);
        proc.resetModule (ps::ModuleType::DynamicEq);
        proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
        pumpMessages (30);
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        editor->selectModule ((int) ps::ModuleType::Equalizer1);
        rig.play (*editor, 4);

        if (juce::SystemStats::getEnvironmentVariable ("PURESHEEN_SNAPSHOT_ONLY", {}) == "ui-deq")
        {
            holder.reset();
            std::cout << (ok ? "snapshot (ui-deq only): OK\n" : "snapshot (ui-deq only): FAILED\n");
            return ok ? 0 : 1;
        }
    }
    // ---- END ui-deq ----

    // 02: every module (Equalizer 2 added for its own shot)
    proc.addModule (ps::ModuleType::Equalizer2, 1);
    for (int t = ps::kNumCoreModuleTypes; t < ps::kNumModuleTypes; ++t)   // (phase 2 modules)
        proc.addModule ((ps::ModuleType) t);
    pumpMessages (30);

    for (int t = 0; t < ps::kNumModuleTypes; ++t)
    {
        const auto type = (ps::ModuleType) t;
        editor->selectModule (t);
        rig.play (*editor, 24);
        auto* panel = editor->getCurrentPanel();

        if (panel == nullptr || panel->getModuleType() != type)
        {
            std::cout << "FAILED: no panel for " << ps::moduleInfo (type).name << "\n";
            ok = false;
            continue;
        }

        if (auto* generic = dynamic_cast<ps::ui::GenericModulePanel*> (panel))
        {
            const int controls = generic->getNumControls();
            std::cout << ps::moduleInfo (type).name << ": generic panel, " << controls << " controls\n";
            ok &= controls > 0;
        }

        ok &= writePng (*editor, file (juce::String ("02_") + ps::moduleInfo (type).idPrefix + "_1180x720"));
    }

    proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
    pumpMessages (30);

    // 03: EQ 1 in Mid/Side mode (channel tabs, Side selected)
    editor->selectModule ((int) ps::ModuleType::Equalizer1);
    set (proc, "eq1.chMode", 1.0f);
    set (proc, "eq1.link", 0.0f);
    rig.play (*editor, 4);

    if (auto* panel = editor->getCurrentPanel())
        panel->setChannelSet (1);

    rig.play (*editor, 2);
    ok &= writePng (*editor, file ("03_eq1_midside_1180x720"));

    if (auto* panel = editor->getCurrentPanel())
        panel->setChannelSet (0);

    set (proc, "eq1.chMode", 0.0f);
    set (proc, "eq1.link", 1.0f);

    // 04: Dither panel
    set (proc, "dith.on", 1.0f);
    editor->selectModule (-1);
    rig.play (*editor, 12);
    ok &= writePng (*editor, file ("04_dither_1180x720"));

    // 05: History popup (a few changes first so it has entries)
    editor->selectModule ((int) ps::ModuleType::Maximizer);
    set (proc, "max.gain", 5.0f);
    set (proc, "max.character", 4.0f);
    proc.setChainOrder ({ ps::ModuleType::Equalizer1, ps::ModuleType::Dynamics, ps::ModuleType::Exciter,
                          ps::ModuleType::Imager, ps::ModuleType::Maximizer });
    pumpMessages (30);
    rig.play (*editor, 4);
    editor->setHistoryVisible (true);
    rig.play (*editor, 2);
    ok &= writePng (*editor, file ("05_history_1180x720"));
    editor->setHistoryVisible (false);
    proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
    pumpMessages (30);

    // 06: Help overlay
    editor->setHelpVisible (true);
    rig.play (*editor, 2);
    ok &= writePng (*editor, file ("06_help_1180x720"));
    editor->setHelpVisible (false);

    // 07: a chain drag in progress (Imager dragged towards the front), then dropped
    {
        editor->selectModule ((int) ps::ModuleType::Dynamics);
        rig.play (*editor, 24);
        auto* strip = findChild<ps::ui::ChainStrip> (*editor);

        if (strip == nullptr)
        {
            std::cout << "FAILED: no chain strip\n";
            ok = false;
        }
        else
        {
            const float toX = (float) strip->getTileBounds (1).getCentreX() - 30.0f;
            strip->simulateDrag (2, toX, false);
            ok &= writePng (*editor, file ("07_chain_drag_1180x720"));
            strip->simulateDrag (2, toX, true);
            pumpMessages (30);
            const auto order = proc.getChainOrder();
            const bool moved = order.size() >= 2 && order[1] == ps::ModuleType::Imager;
            std::cout << (moved ? "chain drag reorders the chain: ok\n" : "FAILED: chain drag did not reorder the chain\n");
            ok &= moved;
            proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
            pumpMessages (30);
        }
    }

    // 08: sizes
    struct Size { int w, h; const char* name; };
    const Size sizes[] = { { 980, 600, "980x600" }, { 1600, 1000, "1600x1000" }, { 1800, 1100, "1800x1100" } };

    for (const auto& s : sizes)
    {
        editor->setSize (s.w, s.h);

        for (auto type : { ps::ModuleType::Equalizer1, ps::ModuleType::Dynamics, ps::ModuleType::Imager,
                           ps::ModuleType::Exciter, ps::ModuleType::Maximizer })
        {
            editor->selectModule ((int) type);
            rig.play (*editor, 6);
            ok &= writePng (*editor, file (juce::String ("08_") + ps::moduleInfo (type).idPrefix + "_" + s.name));
        }
    }

    editor->setSize (PureSheenEditor::kMinWidth, PureSheenEditor::kMinHeight);
    editor->selectModule (-1);
    rig.play (*editor, 4);
    ok &= writePng (*editor, file ("08_dither_980x600"));

    // 09: option variants: K-14 meters, 1/3-octave spectrum with peak hold, Mid/Side meters
    {
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        editor->selectModule ((int) ps::ModuleType::Exciter);
        auto o = editor->getOptions();
        o.meterType = ps::ui::UiOptions::MeterType::KSystem;
        o.meterMidSide = true;
        o.spectrumType = ps::ui::UiOptions::SpectrumType::ThirdOctave;
        o.spectrumPeakHold = true;
        editor->setOptions (o);
        rig.play (*editor, 40);
        ok &= writePng (*editor, file ("09_options_k14_thirdoctave_1180x720"));

        o.meterType = ps::ui::UiOptions::MeterType::ShortTerm;
        o.meterScale = ps::ui::UiOptions::MeterScale::Ebu9;
        o.meterMidSide = false;
        o.spectrumType = ps::ui::UiOptions::SpectrumType::Critical;
        o.freqScale = ps::ui::UiOptions::FreqScale::Mel;
        editor->setOptions (o);
        rig.play (*editor, 40);
        ok &= writePng (*editor, file ("09b_options_ebu9_critical_mel_1180x720"));

        editor->setOptions (ps::ui::UiOptions {});
    }

    // 10: UiOptions round trip
    {
        ps::ui::UiOptions o;
        o.meterType = ps::ui::UiOptions::MeterType::Integrated;
        o.meterScale = ps::ui::UiOptions::MeterScale::Ebu18;
        o.peakHoldMs = 5000.0f;
        o.rmsWindowMs = 1475.0f;
        o.kSystemType = 20;
        o.spectrumType = ps::ui::UiOptions::SpectrumType::Octave;
        o.freqScale = ps::ui::UiOptions::FreqScale::ExtendedLog;
        o.spectrumAveragingS = -1.0f;
        o.fftSize = 16384;
        o.historyDepth = 500;
        o.showTooltips = false;
        const auto back = ps::ui::UiOptions::fromValueTree (o.toValueTree());
        const bool same = back.meterType == o.meterType && back.meterScale == o.meterScale && back.peakHoldMs == o.peakHoldMs
                          && back.rmsWindowMs == o.rmsWindowMs && back.kSystemType == o.kSystemType
                          && back.spectrumType == o.spectrumType && back.freqScale == o.freqScale
                          && back.spectrumAveragingS == o.spectrumAveragingS && back.fftSize == o.fftSize
                          && back.historyDepth == o.historyDepth && back.showTooltips == o.showTooltips;
        std::cout << (same ? "UiOptions round trip: ok\n" : "FAILED: UiOptions round trip\n");
        ok &= same;
    }

    // 10b: spectrum analyser calibration: a bin-centred sine at -6.02 dBFS reads -6.0 dB (tilt off), 1/3-octave and
    //      averaging paths produce data, a silent input stays far below.
    {
        ps::ui::SpectrumAnalyser analyser;
        ps::ui::UiOptions o;
        o.spectrumTiltDbPerOct = 0.0f;
        analyser.setOptions (o);
        analyser.setSampleRate (kSampleRate);
        const double hz = 86.0 * kSampleRate / 4096.0;   // bin 86 = 1007.8 Hz
        std::vector<float> sine (16384);
        for (size_t i = 0; i < sine.size(); ++i)
            sine[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (double) i / kSampleRate);
        analyser.process (sine.data(), sine.data(), (int) sine.size());
        const float level = analyser.levelDbAt ((float) hz);
        const float away = analyser.levelDbAt (8000.0f);
        const bool calibrated = std::abs (level - (-6.02f)) < 0.1f && away < -100.0f;
        std::cout << (calibrated ? "spectrum calibration: ok" : "FAILED: spectrum calibration") << " (sine " << level
                  << " dB, 8 kHz " << away << " dB)" << std::endl;
        ok &= calibrated;

        o.spectrumType = ps::ui::UiOptions::SpectrumType::ThirdOctave;
        analyser.setOptions (o);
        analyser.process (sine.data(), sine.data(), (int) sine.size());
        const float band = analyser.levelDbAt ((float) hz);
        std::cout << "1/3-octave band around 1 kHz: " << band << " dB (mean power of the band's bins)" << std::endl;
        ok &= band > -30.0f && band < -6.0f;

        // Cadence: pulled once per 60 Hz display frame (800 samples at 48 kHz, arriving in 512-sample host blocks), every
        // pull yields a new frame at every FFT size, so the display never holds a spectrum for two frames.
        for (int size : { 2048, 4096, 8192, 16384 })
        {
            ps::ui::SpectrumAnalyser cadence;
            o.spectrumType = ps::ui::UiOptions::SpectrumType::Linear;
            o.fftSize = size;
            cadence.setOptions (o);
            cadence.setSampleRate (kSampleRate);
            int fresh = 0, pulls = 0, consumed = 0;

            for (int frame = 1; frame <= 120; ++frame)
            {
                const int arrived = frame * 800 / 512 * 512;   // whole host blocks delivered by this display frame
                const int n = arrived - consumed;               // 512 or 1024
                const float* x = sine.data() + consumed % 8192;
                const bool changed = cadence.process (x, x, n);
                consumed = arrived;

                if (frame >= 30)   // past the first fill of a 16384-point window
                    ++pulls, fresh += changed ? 1 : 0;
            }

            const bool smooth = fresh == pulls;
            std::cout << (smooth ? "spectrum cadence " : "FAILED: spectrum cadence ") << size << ": " << fresh << " / "
                      << pulls << " pulls gave a new frame" << std::endl;
            ok &= smooth;
        }
    }

    // 10c: mode variants: Maximizer Spectral, EQ 1 Digital, Dynamics 4 bands in Mid/Side (Side shown), Imager with
    //      Stereoize and 4 bands, Exciter 4 bands in Mid/Side
    {
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);

        editor->selectModule ((int) ps::ModuleType::Maximizer);
        set (proc, "max.mode", (float) ps::MaximizerMode::Spectral);
        rig.play (*editor, 40);
        ok &= writePng (*editor, file ("10_max_spectral_1180x720"));
        set (proc, "max.mode", (float) ps::MaximizerMode::Transparent);

        editor->selectModule ((int) ps::ModuleType::Equalizer1);
        set (proc, "eq1.mode", 1.0f);
        rig.play (*editor, 40);
        ok &= writePng (*editor, file ("10_eq1_digital_1180x720"));
        set (proc, "eq1.mode", 0.0f);

        editor->selectModule ((int) ps::ModuleType::Dynamics);
        set (proc, "dyn.bands", 4.0f);
        set (proc, "dyn.xover3", 8000.0f);
        set (proc, "dyn.chMode", 1.0f);
        set (proc, "dyn.link", 0.0f);
        set (proc, "dyn.b.b4.comp.thr", -26.0f);
        set (proc, "dyn.b.b4.comp.ratio", 3.0f);
        rig.play (*editor, 6);
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (1);
        rig.play (*editor, 30);
        ok &= writePng (*editor, file ("10_dyn_4band_midside_1180x720"));
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (0);

        editor->selectModule ((int) ps::ModuleType::Imager);
        set (proc, "img.bands", 4.0f);
        set (proc, "img.b4.width", 40.0f);
        set (proc, "img.stz.on", 1.0f);
        rig.play (*editor, 40);
        ok &= writePng (*editor, file ("10_img_4band_1180x720"));

        editor->selectModule ((int) ps::ModuleType::Exciter);
        set (proc, "exc.xover1", 150.0f);
        set (proc, "exc.xover2", 1200.0f);
        set (proc, "exc.xover3", 5000.0f);
        set (proc, "exc.a.b3.drive", 20.0f);
        set (proc, "exc.a.b4.mode", (float) ps::ExciterMode::Tube);
        set (proc, "exc.a.b4.drive", 35.0f);
        set (proc, "exc.bands", 4.0f);
        set (proc, "exc.chMode", 1.0f);
        rig.play (*editor, 40);
        ok &= writePng (*editor, file ("10_exc_4band_midside_1180x720"));
    }

    // 13: phase 2: every module in the chain (the strip with 14 tiles) at every size, the new panels at 980x600, and
    //     the Transient/Sustain channel mode (EQ 1 with the Sustain tab, Exciter with its T/S switch)
    {
        set (proc, "exc.chMode", 0.0f);
        std::vector<ps::ModuleType> all { ps::kDefaultChain.begin(), ps::kDefaultChain.end() - 1 };
        all.push_back (ps::ModuleType::Equalizer2);
        for (int t = ps::kNumCoreModuleTypes; t < ps::kNumModuleTypes; ++t)
            all.push_back ((ps::ModuleType) t);
        all.push_back (ps::ModuleType::Maximizer);
        proc.setChainOrder (all);
        pumpMessages (30);
        const bool every = (int) proc.getChainOrder().size() == ps::kNumModuleTypes;
        std::cout << (every ? "all 14 modules in the chain: ok\n" : "FAILED: not every module is in the chain\n");
        ok &= every;

        const Size allSizes[] = { { 980, 600, "980x600" }, { 1180, 720, "1180x720" }, { 1800, 1100, "1800x1100" } };
        for (const auto& sz : allSizes)
        {
            editor->setSize (sz.w, sz.h);
            editor->selectModule ((int) ps::ModuleType::DynamicEq);
            rig.play (*editor, 8);
            ok &= writePng (*editor, file (juce::String ("13_all_modules_") + sz.name));
        }

        editor->setSize (PureSheenEditor::kMinWidth, PureSheenEditor::kMinHeight);
        editor->setHelpVisible (true);
        rig.play (*editor, 2);
        ok &= writePng (*editor, file ("13_help_980x600"));
        editor->setHelpVisible (false);

        for (int t = ps::kNumCoreModuleTypes; t < ps::kNumModuleTypes; ++t)
        {
            editor->selectModule (t);
            rig.play (*editor, 6);
            ok &= writePng (*editor, file (juce::String ("13_") + ps::moduleInfo ((ps::ModuleType) t).idPrefix + "_980x600"));
        }

        for (const auto& sz : { allSizes[1], allSizes[0] })
        {
            editor->setSize (sz.w, sz.h);
            editor->selectModule ((int) ps::ModuleType::Equalizer1);
            set (proc, "eq1.chMode", 3.0f);
            set (proc, "eq1.link", 0.0f);
            set (proc, "eq1.a.b2.on", 1.0f);
            set (proc, "eq1.a.b2.freq", 3200.0f);
            set (proc, "eq1.a.b2.gain", -2.0f);
            set (proc, "eq1.b.b6.on", 1.0f);
            set (proc, "eq1.b.b6.shape", (float) ps::EqShape::HighShelfBaxandall);
            set (proc, "eq1.b.b6.freq", 9000.0f);
            set (proc, "eq1.b.b6.gain", 1.5f);
            rig.play (*editor, 6);
            if (auto* panel = editor->getCurrentPanel())
                panel->setChannelSet (1);
            rig.play (*editor, 30);
            ok &= writePng (*editor, file (juce::String ("13_eq1_transient_sustain_") + sz.name));
            if (auto* panel = editor->getCurrentPanel())
                panel->setChannelSet (0);
        }
        set (proc, "eq1.chMode", 0.0f);
        set (proc, "eq1.link", 1.0f);

        editor->selectModule ((int) ps::ModuleType::Exciter);
        set (proc, "exc.ts.on", 1.0f);
        rig.play (*editor, 20);
        ok &= writePng (*editor, file ("13_exc_transient_sustain_980x600"));
        set (proc, "exc.ts.on", 0.0f);

        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
        pumpMessages (30);
    }

    // ---- BEGIN ui-punch: Punch panel (1 and 4 bands, Stereo and M/S, several sizes) ----
    {
        proc.addModule (ps::ModuleType::Punch);
        pumpMessages (30);
        editor->selectModule ((int) ps::ModuleType::Punch);

        const Size punchSizes[] = { { 980, 600, "980x600" }, { 1180, 720, "1180x720" }, { 1600, 1000, "1600x1000" },
                                    { 1800, 1100, "1800x1100" } };

        // One band, Stereo, a punchy setting.
        set (proc, "pnch.bands", 1.0f);
        set (proc, "pnch.a.b1.amount", 45.0f);
        set (proc, "pnch.env", 80.0f);

        for (const auto& sz : punchSizes)
        {
            editor->setSize (sz.w, sz.h);
            rig.play (*editor, 100);
            ok &= writePng (*editor, file (juce::String ("20_pnch_1band_") + sz.name));
        }

        // Four bands, Stereo: mixed amounts (one band off), Auto Gain, Stereo Link, synced envelope.
        set (proc, "pnch.xover1", 120.0f);
        set (proc, "pnch.xover2", 1000.0f);
        set (proc, "pnch.xover3", 6000.0f);
        set (proc, "pnch.bands", 4.0f);
        set (proc, "pnch.a.b1.amount", 60.0f);
        set (proc, "pnch.a.b2.amount", -35.0f);
        set (proc, "pnch.a.b3.amount", 20.0f);
        set (proc, "pnch.a.b4.on", 0.0f);
        set (proc, "pnch.autoGain", 1.0f);
        set (proc, "pnch.stLink", 1.0f);
        set (proc, "pnch.sync", 1.0f);
        set (proc, "pnch.syncNote", 10.0f);

        for (const auto& sz : punchSizes)
        {
            editor->setSize (sz.w, sz.h);
            rig.play (*editor, 100);
            ok &= writePng (*editor, file (juce::String ("20_pnch_4band_") + sz.name));
        }

        // Four bands, Mid/Side, Link off, the Side tab shown (set B has its own amounts).
        set (proc, "pnch.sync", 0.0f);
        set (proc, "pnch.chMode", 1.0f);
        set (proc, "pnch.link", 0.0f);
        set (proc, "pnch.b.b1.amount", -20.0f);
        set (proc, "pnch.b.b3.amount", 70.0f);
        set (proc, "pnch.b.b4.amount", 30.0f);

        for (const auto& sz : { punchSizes[1], punchSizes[0] })
        {
            editor->setSize (sz.w, sz.h);
            rig.play (*editor, 6);
            if (auto* panel = editor->getCurrentPanel())
                panel->setChannelSet (1);
            rig.play (*editor, 100);
            ok &= writePng (*editor, file (juce::String ("20_pnch_4band_midside_") + sz.name));
            if (auto* panel = editor->getCurrentPanel())
                panel->setChannelSet (0);
        }

        // Two bands, Mid/Side (Mid tab), Link Bands on.
        set (proc, "pnch.bands", 2.0f);
        set (proc, "pnch.bandLink", 1.0f);
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        rig.play (*editor, 100);
        ok &= writePng (*editor, file ("20_pnch_2band_midside_1180x720"));

        proc.resetModule (ps::ModuleType::Punch);
        proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
        pumpMessages (30);
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
    }
    // ---- END ui-punch ----

    // ---- BEGIN ui-meq: Match EQ panel (empty, reference only, matched, capturing, file load, M/S Side, T/S) ----
    {
        proc.addModule (ps::ModuleType::MatchEq);
        pumpMessages (30);
        editor->selectModule ((int) ps::ModuleType::MatchEq);
        auto& meqA = static_cast<ps::MatchEq&> (proc.getChain().getModule (ps::ModuleType::MatchEq, 0));
        auto& meqB = static_cast<ps::MatchEq&> (proc.getChain().getModule (ps::ModuleType::MatchEq, 1));

        for (auto* m : { &meqA, &meqB })
        {
            m->clearSpectrum (ps::MatchRole::Reference);
            m->clearSpectrum (ps::MatchRole::Current);
        }

        const Size meqSizes[] = { { 980, 600, "980x600" }, { 1180, 720, "1180x720" }, { 1600, 1000, "1600x1000" },
                                  { 1800, 1100, "1800x1100" } };
        const auto settle = [&] (int blocks)
        {
            rig.play (*editor, 4);
            pumpMessages (250);   // the module's worker designs the kernel; the processor records the spectrum change
            rig.play (*editor, blocks);
        };

        // Synthesised spectra: a warm reference with a Side channel, a brighter "current" mix with a low-mid bump.
        auto reference = ps::makeMatchTiltSpectrum (-4.2, 2.5, 70.0, "file:Reference Master");
        reference.seconds = 182.4;
        reference.sampleRate = 44100.0;
        auto current = ps::makeMatchTiltSpectrum (-2.6, -3.0, 90.0, "capture");
        current.seconds = 31.0;
        current.sampleRate = 48000.0;

        for (int i = 0; i < ps::kMatchPoints; ++i)
        {
            const double oct = std::log2 (ps::matchGridHz (i) / 1000.0);
            const auto bump = [oct] (double centreOct, double widthOct, double db)
            { return db * std::exp (-0.5 * std::pow ((oct - centreOct) / widthOct, 2.0)); };
            for (int ch = 0; ch < 3; ++ch)
                current.psdDb[ch][(size_t) i] += (float) (bump (std::log2 (0.3), 0.45, 3.5) + bump (std::log2 (3.2), 0.35, -2.5)
                                                          + 0.6 * std::sin (oct * 9.0));
            reference.psdDb[3][(size_t) i] = reference.psdDb[2][(size_t) i] - (float) (8.0 + 1.5 * oct);
            current.psdDb[3][(size_t) i] = current.psdDb[2][(size_t) i] - (float) (11.0 + 0.5 * oct);
        }

        // Empty
        for (const auto& sz : meqSizes)
        {
            editor->setSize (sz.w, sz.h);
            rig.play (*editor, 20);
            ok &= writePng (*editor, file (juce::String ("21_meq_empty_") + sz.name));
        }

        // Reference only
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        meqA.setSpectrum (ps::MatchRole::Reference, reference);
        settle (10);
        ok &= writePng (*editor, file ("21_meq_reference_only_1180x720"));

        // Both: the matched curve, a narrowed region
        meqA.setSpectrum (ps::MatchRole::Current, current);
        set (proc, "meq.a.amount", 70.0f);
        set (proc, "meq.a.smooth", 35.0f);
        set (proc, "meq.a.lo", 35.0f);
        set (proc, "meq.a.hi", 14000.0f);

        for (const auto& sz : meqSizes)
        {
            editor->setSize (sz.w, sz.h);
            settle (20);
            ok &= writePng (*editor, file (juce::String ("21_meq_matched_") + sz.name));
        }

        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        settle (4);

        // Synthetic mouse events (left / right button) on a component, in its own coordinates.
        const auto mouse = [] (juce::Component& c, juce::Point<float> pos, bool right, int kind)   // 0 down, 1 drag, 2 up
        {
            const auto mods = juce::ModifierKeys (right ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys::leftButtonModifier);
            const auto now = juce::Time::getCurrentTime();
            const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      &c, &c, now, pos, now, 1, kind == 1);
            if (kind == 0) c.mouseDown (e);
            else if (kind == 1) c.mouseDrag (e);
            else c.mouseUp (e);
        };
        const auto findByTooltip = [] (juce::Component& root, const juce::String& prefix) -> juce::Component*
        {
            std::function<juce::Component* (juce::Component&)> walk = [&] (juce::Component& c) -> juce::Component*
            {
                if (auto* t = dynamic_cast<juce::SettableTooltipClient*> (&c))
                    if (t->getTooltip().startsWith (prefix) && c.isVisible())
                        return &c;
                for (auto* child : c.getChildren())
                    if (auto* f = walk (*child))
                        return f;
                return nullptr;
            };
            return walk (root);
        };

        // Dragging the low cutoff handle: one gesture = one undo step, the value follows the mouse.
        if (auto* display = findChild<ps::ui::MatchDisplay> (*editor))
        {
            pumpMessages (60);
            proc.clearHistory();   // (the history may be at its depth limit after the earlier shots)
            const int before = (int) proc.getHistory().size();
            const float x0 = display->getCutoffX (false), y = display->getPlotArea().getCentreY();
            mouse (*display, { x0, y }, false, 0);
            for (int i = 1; i <= 8; ++i)
                mouse (*display, { x0 + 6.0f * (float) i, y }, false, 1);
            mouse (*display, { x0 + 48.0f, y }, false, 2);
            pumpMessages (450);   // the processor commits a gesture ~250 ms after it ended
            rig.play (*editor, 2);
            const float lo = proc.getParameterValue ("meq.a.lo");
            const int steps = (int) proc.getHistory().size() - before;
            const bool dragOk = lo > 45.0f && steps == 1;
            std::cout << (dragOk ? "Match EQ cutoff drag: ok" : "FAILED: Match EQ cutoff drag") << " (low " << lo << " Hz, "
                      << steps << " undo step)\n";
            ok &= dragOk;
            set (proc, "meq.a.lo", 35.0f);
        }
        else
        {
            std::cout << "FAILED: no Match EQ display\n";
            ok = false;
        }

        // Capture buttons: a right-click does nothing, a left click starts capturing the Current spectrum from the
        // playing song (the running spectrum animates), a second click stops it.
        if (auto* capture = findByTooltip (*editor, "Capture the Current spectrum"))
        {
            const auto centre = capture->getLocalBounds().getCentre().toFloat();
            mouse (*capture, centre, true, 0);
            mouse (*capture, centre, true, 2);
            const bool rightIgnored = ! meqA.isCapturing();
            mouse (*capture, centre, false, 0);
            mouse (*capture, centre, false, 2);
            const bool started = meqA.isCapturing();
            std::cout << (rightIgnored && started ? "Match EQ capture button: ok\n" : "FAILED: Match EQ capture button\n");
            ok &= rightIgnored && started;
        }
        else
        {
            std::cout << "FAILED: no Match EQ capture button\n";
            ok = false;
            meqA.startCapture (ps::MatchRole::Current);
        }

        rig.play (*editor, 50);
        pumpMessages (400);
        rig.play (*editor, 10);
        pumpMessages (300);
        rig.play (*editor, 2);
        ok &= writePng (*editor, file ("21_meq_capturing_1180x720"));

        if (auto* stop = findByTooltip (*editor, "Stop capturing: the average so far becomes the Current"))
        {
            const auto centre = stop->getLocalBounds().getCentre().toFloat();
            mouse (*stop, centre, false, 0);
            mouse (*stop, centre, false, 2);
        }
        else
        {
            meqA.stopCapture();
        }

        settle (10);
        ok &= writePng (*editor, file ("21_meq_captured_song_1180x720"));
        const bool captured = ! meqA.isCapturing() && meqA.getSpectrum (ps::MatchRole::Current).valid;
        std::cout << (captured ? "Match EQ capture of the song: ok\n" : "FAILED: Match EQ capture gave no spectrum\n");
        ok &= captured;

        // Reference from an audio file (16-bit WAV, 150 s of pink-ish noise): mid-load progress, then the result
        {
            const auto wav = outputDir.getChildFile ("meq_reference_noise.wav");
            {
                wav.deleteFile();
                juce::FileOutputStream out (wav);
                const int rate = 44100, seconds = 150, frames = rate * seconds;
                const auto le32 = [&out] (int v) { out.writeInt (v); };
                const auto le16 = [&out] (int v) { out.writeShort ((short) v); };
                out.write ("RIFF", 4); le32 (36 + frames * 4); out.write ("WAVEfmt ", 8);
                le32 (16); le16 (1); le16 (2); le32 (rate); le32 (rate * 4); le16 (4); le16 (16);
                out.write ("data", 4); le32 (frames * 4);
                juce::uint32 seed = 12345u;
                double b0 = 0, b1 = 0, b2 = 0;
                juce::HeapBlock<short> block (8192 * 2);

                for (int done = 0; done < frames;)
                {
                    const int n = juce::jmin (8192, frames - done);
                    for (int k = 0; k < n; ++k)
                    {
                        seed = seed * 1664525u + 1013904223u;
                        const double white = ((double) (seed >> 8) / 8388608.0 - 1.0);
                        b0 = 0.99765 * b0 + white * 0.0990460;
                        b1 = 0.96300 * b1 + white * 0.2965164;
                        b2 = 0.57000 * b2 + white * 1.0526913;
                        const double pink = (b0 + b1 + b2 + white * 0.1848) * 0.08;
                        block[(size_t) (2 * k)] = (short) juce::jlimit (-32767, 32767, (int) (pink * 32767.0));
                        block[(size_t) (2 * k + 1)] = (short) juce::jlimit (-32767, 32767, (int) (pink * 0.9 * 32767.0));
                    }
                    out.write (block.get(), (size_t) n * 4);
                    done += n;
                }
            }

            const bool started = proc.startMatchReferenceFile (wav, 0);
            pumpMessages (15);
            rig.play (*editor, 2);
            ok &= writePng (*editor, file ("21_meq_loading_file_1180x720"));

            for (int i = 0; i < 400 && proc.getMatchReferenceFileProgress() >= 0.0f; ++i)
                pumpMessages (25);

            settle (10);
            const auto loaded = meqA.getSpectrum (ps::MatchRole::Reference);
            const bool fileOk = started && loaded.valid && juce::String (loaded.source.data()).startsWith ("file:");
            std::cout << (fileOk ? "Match EQ reference from a file: ok\n" : "FAILED: Match EQ reference file did not load\n");
            ok &= fileOk;
            ok &= writePng (*editor, file ("21_meq_file_loaded_1180x720"));
            wav.deleteFile();
        }

        // Mid/Side, Link off, the Side tab (Side spectra and set B's own settings)
        meqA.setSpectrum (ps::MatchRole::Reference, reference);
        meqA.setSpectrum (ps::MatchRole::Current, current);
        set (proc, "meq.chMode", 1.0f);
        set (proc, "meq.link", 0.0f);
        set (proc, "meq.b.amount", 100.0f);
        set (proc, "meq.b.smooth", 70.0f);
        set (proc, "meq.b.boost", 6.0f);

        for (const auto& sz : { meqSizes[1], meqSizes[0] })
        {
            editor->setSize (sz.w, sz.h);
            rig.play (*editor, 4);
            if (auto* panel = editor->getCurrentPanel())
                panel->setChannelSet (1);
            settle (20);
            ok &= writePng (*editor, file (juce::String ("21_meq_midside_side_") + sz.name));
            if (auto* panel = editor->getCurrentPanel())
                panel->setChannelSet (0);
        }

        // Mid/Side, Side tab, a mono reference (no Side): the flat-curve note
        meqA.setSpectrum (ps::MatchRole::Reference, ps::makeMatchTiltSpectrum (-4.0, 0.0, 60.0, "synth"));
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        rig.play (*editor, 4);
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (1);
        settle (10);
        ok &= writePng (*editor, file ("21_meq_midside_side_mono_reference_1180x720"));
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (0);

        // Transient/Sustain: the Sustain instance (twin) has only a reference so far
        set (proc, "meq.chMode", 3.0f);
        meqB.setSpectrum (ps::MatchRole::Reference, ps::makeMatchTiltSpectrum (-3.0, 0.0, 60.0, "synth"));
        editor->setSize (PureSheenEditor::kMinWidth, PureSheenEditor::kMinHeight);
        rig.play (*editor, 4);
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (1);
        settle (20);
        ok &= writePng (*editor, file ("21_meq_ts_sustain_980x600"));
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (0);

        for (auto* m : { &meqA, &meqB })
        {
            m->clearSpectrum (ps::MatchRole::Reference);
            m->clearSpectrum (ps::MatchRole::Current);
        }

        proc.resetModule (ps::ModuleType::MatchEq);
        proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
        pumpMessages (30);
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
    }
    // ---- END ui-meq ----

    // ---- BEGIN ui-lowend: Low Contrast and Bass Shaper panels (every size, Low Contrast M/S Side, T/S Sustain and
    //      region solo, Bass Shaper with neutral and active sections) ----
    {
        proc.addModule (ps::ModuleType::LowContrast);
        proc.addModule (ps::ModuleType::BassShaper);
        pumpMessages (30);

        const Size lowendSizes[] = { { 980, 600, "980x600" }, { 1180, 720, "1180x720" }, { 1600, 1000, "1600x1000" },
                                     { 1800, 1100, "1800x1100" } };

        // Neutral Bass Shaper (meters only), 1180x720
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        editor->selectModule ((int) ps::ModuleType::BassShaper);
        rig.play (*editor, 130);
        ok &= writePng (*editor, file ("40_bass_neutral_1180x720"));

        if (dynamic_cast<ps::ui::GenericModulePanel*> (editor->getCurrentPanel()) != nullptr)
        {
            std::cout << "FAILED: Bass Shaper still shows the generic panel\n";
            ok = false;
        }

        set (proc, "lowc.low", 32.0f);
        set (proc, "lowc.high", 160.0f);
        set (proc, "lowc.a.contrast", 70.0f);
        set (proc, "lowc.a.gain", 1.5f);
        set (proc, "bass.balance", 2.0f);
        set (proc, "bass.punch", 3.5f);
        set (proc, "bass.sus.on", 1.0f);
        set (proc, "bass.sus.thr", -26.0f);
        set (proc, "bass.sus.amount", 60.0f);
        set (proc, "bass.sus.char", 70.0f);
        set (proc, "bass.peak.on", 1.0f);
        set (proc, "bass.peak.thr", -10.0f);

        for (const auto& sz : lowendSizes)
        {
            editor->setSize (sz.w, sz.h);
            editor->selectModule ((int) ps::ModuleType::LowContrast);
            rig.play (*editor, 40);
            ok &= writePng (*editor, file (juce::String ("40_lowc_") + sz.name));
            editor->selectModule ((int) ps::ModuleType::BassShaper);
            rig.play (*editor, 130);
            ok &= writePng (*editor, file (juce::String ("40_bass_") + sz.name));
        }

        if (dynamic_cast<ps::ui::GenericModulePanel*> (editor->getCurrentPanel()) != nullptr)
        {
            std::cout << "FAILED: Low Contrast / Bass Shaper still show the generic panel\n";
            ok = false;
        }

        // Low Contrast: Mid/Side with Side selected (Smooth, negative contrast), then Transient/Sustain (Sustain tab),
        // then a region solo
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        editor->selectModule ((int) ps::ModuleType::LowContrast);
        set (proc, "lowc.chMode", 1.0f);
        set (proc, "lowc.link", 0.0f);
        set (proc, "lowc.b.mode", 1.0f);
        set (proc, "lowc.b.contrast", -60.0f);
        rig.play (*editor, 4);
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (1);
        rig.play (*editor, 40);
        ok &= writePng (*editor, file ("40_lowc_midside_side_1180x720"));

        set (proc, "lowc.chMode", 2.0f);
        rig.play (*editor, 40);
        ok &= writePng (*editor, file ("40_lowc_ts_sustain_1180x720"));
        editor->setSize (PureSheenEditor::kMinWidth, PureSheenEditor::kMinHeight);
        rig.play (*editor, 6);
        ok &= writePng (*editor, file ("40_lowc_ts_sustain_980x600"));
        if (auto* panel = editor->getCurrentPanel())
            panel->setChannelSet (0);

        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
        set (proc, "lowc.chMode", 0.0f);
        set (proc, "lowc.link", 1.0f);
        {
            auto m = proc.getMonitor (ps::ModuleType::LowContrast);
            m.soloRegion = true;
            proc.setMonitor (ps::ModuleType::LowContrast, m);
        }
        rig.play (*editor, 30);
        ok &= writePng (*editor, file ("40_lowc_region_solo_1180x720"));
        {
            auto m = proc.getMonitor (ps::ModuleType::LowContrast);
            m.soloRegion = false;
            proc.setMonitor (ps::ModuleType::LowContrast, m);
        }

        // Leaving the panel clears a region solo it showed.
        {
            auto m = proc.getMonitor (ps::ModuleType::LowContrast);
            m.soloRegion = true;
            proc.setMonitor (ps::ModuleType::LowContrast, m);
            rig.play (*editor, 2);
            editor->selectModule ((int) ps::ModuleType::BassShaper);
            rig.play (*editor, 2);
            const bool cleared = ! proc.getMonitor (ps::ModuleType::LowContrast).soloRegion;
            std::cout << (cleared ? "Low Contrast region solo cleared with its panel: ok\n"
                                  : "FAILED: Low Contrast region solo outlived its panel\n");
            ok &= cleared;
        }

        // Interactions: one undo step per drag; a section's first edit switches it on (same step); region edges push
        {
            const juce::ModifierKeys left (juce::ModifierKeys::leftButtonModifier);
            const auto event = [] (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, juce::ModifierKeys mods)
            {
                const auto now = juce::Time::getCurrentTime();
                return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                                         juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, downPos,
                                         now, 1, pos != downPos);
            };
            const auto drag = [&] (juce::Component& c, juce::Point<float> from, juce::Point<float> delta)
            {
                c.mouseDown (event (c, from, from, left));
                for (int i = 1; i <= 10; ++i)
                    c.mouseDrag (event (c, from + delta * ((float) i / 10.0f), from, left));
                c.mouseUp (event (c, from + delta, from, left));
            };
            const auto steps = [&]
            {
                pumpMessages (300);
                rig.play (*editor, 1);
                return (int) proc.getHistory().size();
            };
            const auto report = [&] (bool pass, const char* what)
            {
                std::cout << (pass ? "lowend: " : "FAILED: lowend: ") << what << (pass ? ": ok\n" : "\n");
                ok &= pass;
            };
            const auto value = [&] (const char* id) { return proc.getParameterValue (id); };

            editor->selectModule ((int) ps::ModuleType::BassShaper);
            set (proc, "bass.peak.on", 0.0f);
            set (proc, "bass.peak.thr", -6.0f);
            set (proc, "bass.sus.on", 0.0f);
            set (proc, "bass.sus.thr", -18.0f);
            rig.play (*editor, 4);
            proc.clearHistory();

            if (auto* scope = findChild<ps::ui::BassScope> (*editor))
            {
                const int h = steps();
                const juce::Point<float> p0 ((float) scope->getWidth() * 0.4f, scope->getThresholdY (false, true));
                drag (*scope, p0, { 0.0f, 40.0f });
                const int h2 = steps();
                report (value ("bass.peak.thr") < -7.0f && value ("bass.peak.on") >= 0.5f && h2 == h + 1,
                        "dragging the scope's Peak line = threshold + Peak on in one undo step");
            }
            else
            {
                report (false, "no Bass Shaper scope");
            }

            if (auto* knob = findChild<ps::ui::lowend::SectionKnob> (*editor))
            {
                const int h = steps();
                const float before = value ("bass.sus.thr");
                const auto c = knob->getLocalBounds().getCentre().toFloat();
                drag (*knob, c, { 0.0f, -40.0f });
                const int h2 = steps();
                report (value ("bass.sus.thr") > before + 1.0f && value ("bass.sus.on") >= 0.5f && h2 == h + 1,
                        "dragging a Sustain knob switches Sustain Power on in the same undo step");
            }
            else
            {
                report (false, "no section knob");
            }

            editor->selectModule ((int) ps::ModuleType::LowContrast);
            set (proc, "lowc.low", 40.0f);
            set (proc, "lowc.high", 200.0f);
            rig.play (*editor, 4);

            if (auto* display = findChild<ps::ui::LowRegionDisplay> (*editor))
            {
                const float y = display->getPlotArea().getCentreY();
                int h = steps();
                const juce::Point<float> hi (display->xForFrequency (200.0f), y);
                drag (*display, hi, { display->xForFrequency (35.0f) - hi.x, 0.0f });
                int h2 = steps();
                const float lo1 = value ("lowc.low"), hi1 = value ("lowc.high");
                report (hi1 < 40.0f && lo1 < 30.0f && hi1 >= lo1 * 1.2592f * 0.999f && h2 == h + 1,
                        "dragging the high edge into the low edge pushes it, one undo step");

                set (proc, "lowc.low", 40.0f);
                set (proc, "lowc.high", 120.0f);
                rig.play (*editor, 2);
                h = steps();
                const juce::Point<float> mid (display->xForFrequency (std::sqrt (40.0f * 120.0f)), y);
                drag (*display, mid, { 60.0f, 0.0f });
                h2 = steps();
                const float lo2 = value ("lowc.low"), hi2 = value ("lowc.high");
                report (lo2 > 45.0f && std::abs (hi2 / lo2 - 3.0f) < 0.03f && h2 == h + 1,
                        "dragging the region moves both edges (ratio kept), one undo step");
            }
            else
            {
                report (false, "no Low Contrast region display");
            }
        }

        proc.resetModule (ps::ModuleType::LowContrast);
        proc.resetModule (ps::ModuleType::BassShaper);
        proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
        pumpMessages (30);
        editor->setSize (PureSheenEditor::kDefaultWidth, PureSheenEditor::kDefaultHeight);
    }
    // ---- END ui-lowend ----

    // 11: nothing takes keyboard focus
    ok &= checkNoFocus (*editor);

    // 12: the editor size is remembered in the UI state
    {
        editor->setSize (1300, 800);
        const auto ui = proc.getUiState();
        const bool saved = (int) ui.getProperty ("editorW") == 1300 && (int) ui.getProperty ("editorH") == 800;
        std::cout << (saved ? "editor size saved in the UI state: ok\n" : "FAILED: editor size not saved\n");
        ok &= saved;
    }

    std::cout << (rig.tapsFed ? "chain feeds its UI taps: ok" : "FAILED: the chain did not feed its output FIFO") << "\n";
    ok &= rig.tapsFed;

    holder.reset();
    std::cout << (ok ? "snapshot: OK\n" : "snapshot: FAILED\n");
    return ok ? 0 : 1;
}
