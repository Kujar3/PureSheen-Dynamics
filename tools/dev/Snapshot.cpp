#include "core/Loudness.h"
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
