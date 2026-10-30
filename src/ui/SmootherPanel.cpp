#include "ui/SmootherPanel.h"

#include "dsp/Smoother.h"
#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/SpectralCommon.h"
#include "ui/SpectralDisplay.h"
#include "ui/SpectralWidgets.h"

#include <array>
#include <cmath>

// Smoother panel (owner: ui-spectral). Layout and interactions: see SmootherPanel.h.
// Meters come from the module of the selected set (moduleForChannelSet: the T/S twin for Sustain), polled at 30 Hz in
// tick() (which also feeds the Trace view and runs Learn); the display repaints in frameTick() with the spectra.
namespace ps::ui
{
namespace
{
constexpr auto kType = ps::ModuleType::Smoother;
constexpr int kBands = ps::kSmootherBands;
constexpr int kGap = 8;
constexpr float kLeftMin = -78.0f, kLeftMax = 6.0f;    // spectrum level (grid every 12 dB)
constexpr float kRightScale = 1.0f / 6.0f;             // reduction dB per left dB: 0 on the 0 dB line, -12 at -72
constexpr float kMinRatio = 1.12246204831f;            // 2^(1/6): readSmootherParams keeps high >= low x this
constexpr int kLearnTicks = 6 * 30;                    // 6 s at 30 Hz
constexpr int kHoldTicks = 30;

using spectral::setParamId;
using spectral::twinParamId;
} // namespace

//==============================================================================
struct SmootherPanel::Impl
{
    Impl (SmootherPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), services (s), colour (moduleColour (kType)),
          display (p, s, kType),
          viewSwitch ({ "Spectrum", "Trace" }, colour),
          meters ("Input / GR"),
          learn ("Learn"),
          reductionCard ("Reduction", colour),
          timingCard ("Timing", colour),
          regionCard ("Region", colour),
          mode (p, "smth.a.mode", { "Gentle", "Medium", "Strong" }),
          amount (p, "smth.a.amount", "Amount"),
          tone (p, "smth.a.tone", "Tone"),
          attack (p, "smth.a.att", "Attack"),
          release (p, "smth.a.rel", "Release"),
          low (p, "smth.a.low", "Low"),
          high (p, "smth.a.high", "High")
    {
        display.setLeftScale (kLeftMin, kLeftMax);
        display.setRightScale (0.0f, kRightScale, "Reduction");
        display.setSpectra (true, true);
        display.paintOverlay = [this] (juce::Graphics& g) { paintBars (g); };
        display.setBaseTooltip ("Smoother: grey = input, coloured line = output, bars = the reduction of each of the 32 "
                                "bands (right scale). Drag the region handles to choose where it works.");

        trace.setAccentColour (colour);
        trace.setTitle ("Trace");
        trace.setLevelRange (-60.0f, 0.0f);
        trace.setGrRange (-18.0f);
        trace.setTooltip ("Trace: the loudest band level inside the region (grey) and the deepest reduction (colour) over "
                          "the last seconds.");

        viewSwitch.onChange = [this] (int index) { setTrace (index == 1); };
        viewSwitch.setTooltip ("Spectrum: the reduction per band. Trace: the reduction over time.");
        meters.setTooltip ("Input: the loudest band inside the region. GR: the deepest reduction right now.");
        learn.setTooltip ("Learn: listens for 6 s (click again to stop early) and places the region where the music is "
                          "harshest.");
        learn.onClick = [this] { learning ? finishLearn (true) : beginLearn(); };
        mode.setTooltip ("Gentle / Medium / Strong: how hard harsh peaks are reduced (ratio and maximum reduction).");
        amount.setTooltip ("Amount: how much harshness is reduced (0 = no change). Higher also lowers the threshold.");
        tone.setTooltip ("Tone: positive leaves the highs more open (brighter), negative reduces them more (darker).");
        attack.setTooltip ("Attack: how quickly a reduction starts.");
        release.setTooltip ("Release: how quickly a reduction lets go.");
        low.setTooltip ("Low: where the action region starts.");
        high.setTooltip ("High: where the action region ends.");

        owner.addAndMakeVisible (display);
        owner.addChildComponent (trace);
        owner.addAndMakeVisible (viewSwitch);
        owner.addAndMakeVisible (meters);
        owner.addAndMakeVisible (learn);
        owner.addAndMakeVisible (reductionCard);
        owner.addAndMakeVisible (timingCard);
        owner.addAndMakeVisible (regionCard);
        reductionCard.addAndMakeVisible (mode);
        reductionCard.addAndMakeVisible (amount);
        reductionCard.addAndMakeVisible (tone);
        timingCard.addAndMakeVisible (attack);
        timingCard.addAndMakeVisible (release);
        regionCard.addAndMakeVisible (low);
        regionCard.addAndMakeVisible (high);

        meters.setBars ({ { "Input", -60.0f, 0.0f, -60.0f, Colours::input, -1, 12.0f, false },
                          { "GR", -18.0f, 0.0f, 0.0f, colour, 1, 6.0f, false } });

        gr.fill (0.0f);
        holds.fill (0.0f);
        holdTicks.fill (0);
        scores.fill (0.0f);
        rebind();
    }

    ~Impl()
    {
        if (learning)
            finishLearn (false);
    }

    //==============================================================================
    spectral::SetState state() const { return spectral::setState (processor, kType, owner.getChannelSet()); }

    const ps::Smoother* smoother (int set) const
    {
        return dynamic_cast<const ps::Smoother*> (&moduleForChannelSet (processor, kType, set));
    }

    void rebind()
    {
        const auto s = state();
        const auto c = spectral::setColour (kType, s);

        auto bind = [&] (ParamKnob& k, const char* name)
        {
            k.setParamId (setParamId (kType, s.set, name));
            k.setLinkedParamId (twinParamId (kType, s, name));
            k.setColour (c);
        };

        bind (amount, "amount");
        bind (tone, "tone");
        bind (attack, "att");
        bind (release, "rel");
        bind (low, "low");
        bind (high, "high");
        mode.setParamId (setParamId (kType, s.set, "mode"));
        mode.setLinkedParamId (twinParamId (kType, s, "mode"));
        mode.setOnColour (c);

        display.setAccentColour (c);
        display.setChannelSet (s.set);
        display.setRegion (setParamId (kType, s.set, "low"), setParamId (kType, s.set, "high"),
                           twinParamId (kType, s, "low"), twinParamId (kType, s, "high"), kMinRatio, true);
        display.setLegend ({ { "Input", Colours::input }, { "Output", c }, { "Reduction", c.withAlpha (0.7f) } });
        trace.setAccentColour (c);
        meters.setBarColour (1, c);
        learn.setOnColour (c);

        if (s.set != shown.set || s.mode != shown.mode)
        {
            meters.resetHolds();
            trace.clear();
            holds.fill (0.0f);
        }

        shown = s;
        updateTexts();
        display.repaint();
    }

    void updateTexts()
    {
        const auto s = state();
        const int m = juce::jlimit (0, 2, juce::roundToInt (processor.getParameterValue (setParamId (kType, s.set, "mode"))));
        static const char* caps[] { "Gentle: cuts up to 6 dB", "Medium: cuts up to 12 dB", "Strong: cuts up to 18 dB" };
        display.setHeaderText (caps[m]);
        timingCard.setSubtitle (s.split ? channelSetName (s.mode, s.set) : juce::String ("How it moves"));
        regionCard.setSubtitle (fmt::hz (processor.getParameterValue (setParamId (kType, s.set, "low"))) + " - "
                                + fmt::hz (processor.getParameterValue (setParamId (kType, s.set, "high"))));
    }

    void setTrace (bool on)
    {
        showTrace = on;
        viewSwitch.setSelected (on ? 1 : 0);
        display.setVisible (! on);
        trace.setVisible (on);
    }

    //==============================================================================
    // Learn
    void beginLearn()
    {
        if (! spectral::moduleRunning (processor, kType))
        {
            owner.showHeaderHint ("Turn the Smoother on and play audio to learn the region");
            return;
        }

        auto m = processor.getMonitor (kType);
        m.learnRegion = false;              // a fresh start clears the scores
        processor.setMonitor (kType, m);
        m.learnRegion = true;
        processor.setMonitor (kType, m);
        learning = true;
        learnTicks = 0;
        learnSet = state().set;
        scores.fill (0.0f);
        learn.setOn (true);
        owner.showHeaderHint ("Learning: play the harshest part of the song");
    }

    void finishLearn (bool apply)
    {
        auto m = processor.getMonitor (kType);
        m.learnRegion = false;
        processor.setMonitor (kType, m);
        learning = false;
        learn.setOn (false);
        learn.setText ("Learn");

        if (! apply)
            return;

        const auto* sm = smoother (learnSet);
        double lo = 0.0, hi = 0.0;

        if (sm == nullptr || ! ps::smootherLearnedRegion (sm->getMeters(), learnSet, sm->getSampleRate(), lo, hi))
        {
            owner.showHeaderHint ("Nothing learned: play audio while learning");
            return;
        }

        const float l = juce::jlimit (20.0f, 20000.0f / kMinRatio, (float) lo);
        const float h = juce::jlimit (l * kMinRatio, 20000.0f, (float) hi);
        const auto s = spectral::setState (processor, kType, learnSet);
        std::vector<std::pair<juce::String, float>> changes { { setParamId (kType, learnSet, "low"), l },
                                                              { setParamId (kType, learnSet, "high"), h } };

        if (s.linked)
        {
            changes.push_back ({ setParamId (kType, 1 - learnSet, "low"), l });
            changes.push_back ({ setParamId (kType, 1 - learnSet, "high"), h });
        }

        processor.setParameterValues (changes, "Smoother region: learned " + fmt::hz (l) + " - " + fmt::hz (h));
        owner.showHeaderHint ("Region learned: " + fmt::hz (l) + " - " + fmt::hz (h));
    }

    //==============================================================================
    void update()
    {
        const auto s = state();

        if (s.set != shown.set || s.linked != shown.linked || s.split != shown.split || s.mode != shown.mode)
            rebind();
        else
            updateTexts();

        display.tick();

        const bool running = spectral::moduleRunning (processor, kType);
        const auto* sm = smoother (s.set);
        bool valid = false;
        float regionPeak = -200.0f, maxGr = 0.0f;

        if (running && sm != nullptr)
        {
            const auto& m = sm->getMeters();
            sampleRate = sm->getSampleRate();

            for (int b = 0; b < kBands && ! valid; ++b)
                valid = spectral::levelValid (m.bandLevelDb[s.set][b].load());

            for (int b = 0; b < kBands; ++b)
            {
                const float v = valid ? m.grDb[s.set][b].load() : 0.0f;
                gr[(size_t) b] = std::isfinite (v) ? juce::jlimit (-24.0f, 0.0f, v) : 0.0f;

                if (gr[(size_t) b] <= holds[(size_t) b])
                {
                    holds[(size_t) b] = gr[(size_t) b];
                    holdTicks[(size_t) b] = kHoldTicks;
                }
                else if (--holdTicks[(size_t) b] <= 0)
                {
                    holds[(size_t) b] += (gr[(size_t) b] - holds[(size_t) b]) * 0.25f;
                }
            }

            regionPeak = m.regionPeakDb[s.set].load();
            maxGr = m.maxGrDb[s.set].load();
        }
        else
        {
            gr.fill (0.0f);
            holds.fill (0.0f);
        }

        metersValid = valid;
        {
            static const float caps[] { 6.0f, 12.0f, 18.0f };
            const int m = juce::jlimit (0, 2, juce::roundToInt (processor.getParameterValue (setParamId (kType, s.set, "mode"))));
            const float cap = -caps[m] * processor.getParameterValue (setParamId (kType, s.set, "amount")) / 100.0f;
            meters.setLimit (1, cap);
            meters.setNote (cap > -0.005f ? juce::String ("Amount 0: off") : "Limit " + spectral::dbNumber (cap) + " dB");
        }
        meters.setValue (0, regionPeak, valid && regionPeak > -140.0f);
        meters.setValue (1, maxGr, valid);
        trace.push (regionPeak, maxGr, valid);

        if (showTrace)
            trace.repaint();

        // Learn: progress, live scores and the region it would pick.
        if (learning)
        {
            ++learnTicks;
            const auto* lm = smoother (learnSet);
            float peak = 0.0f;

            if (lm != nullptr)
            {
                for (int b = 0; b < kBands; ++b)
                {
                    scores[(size_t) b] = juce::jmax (0.0f, lm->getMeters().learnScore[learnSet][b].load());
                    peak = juce::jmax (peak, scores[(size_t) b]);
                }

                double lo = 0.0, hi = 0.0;
                previewValid = ps::smootherLearnedRegion (lm->getMeters(), learnSet, lm->getSampleRate(), lo, hi);
                previewLo = (float) lo;
                previewHi = (float) hi;
            }

            scorePeak = peak;
            const int left = juce::jmax (1, (kLearnTicks - learnTicks + 29) / 30);
            learn.setText ("Learning " + juce::String (left) + " s");

            if (learnTicks >= kLearnTicks || ! running)
                finishLearn (running);
        }

        juce::String status;

        if (! processor.isModuleInChain (kType) || processor.getParameterValue ("smth.on") < 0.5f)
            status = "Smoother is off";
        else if (! valid && ! services.getModulePreSpectrum().hasData())
            status = "Play audio to see the spectrum";

        display.setStatusText (status);
        trace.setStatusText (status);
    }

    std::array<float, kBands + 1> bandEdges() const
    {
        std::array<float, kBands + 1> e {};
        std::array<float, kBands> c {};

        for (int b = 0; b < kBands; ++b)
            c[(size_t) b] = (float) ps::smootherBandHz (b, sampleRate);

        for (int b = 1; b < kBands; ++b)
            e[(size_t) b] = std::sqrt (c[(size_t) b - 1] * c[(size_t) b]);

        e[0] = c[0] * std::sqrt (c[0] / c[1]);
        e[(size_t) kBands] = c[(size_t) kBands - 1] * std::sqrt (c[(size_t) kBands - 1] / c[(size_t) kBands - 2]);
        return e;
    }

    void paintBars (juce::Graphics& g)
    {
        const auto right = display.getRightAxis();
        const auto plot = display.getPlotArea();
        const auto c = display.getAccentColour();
        const auto edges = bandEdges();
        const float y0 = right.dbToY (0.0f);

        // Learn scores from the bottom while learning, with the region they point to.
        if (learning && scorePeak > 0.0f)
        {
            const float maxH = plot.getHeight() * 0.32f;

            for (int b = 0; b < kBands; ++b)
            {
                const float x0 = display.xForHz (edges[(size_t) b]) + 1.0f, x1 = display.xForHz (edges[(size_t) b + 1]) - 1.0f;
                const float h = maxH * scores[(size_t) b] / scorePeak;

                if (x1 - x0 < 1.0f || h < 0.5f)
                    continue;

                const auto r = juce::Rectangle<float> (x0, plot.getBottom() - 16.0f - h, x1 - x0, h);
                g.setGradientFill (juce::ColourGradient::vertical (Colours::info.withAlpha (0.5f), r.getY(),
                                                                   Colours::info.withAlpha (0.12f), r.getBottom()));
                g.fillRect (r);
            }

            if (previewValid)
            {
                const float x0 = display.xForHz (previewLo), x1 = display.xForHz (previewHi);
                const auto r = juce::Rectangle<float> (x0, plot.getY() + 4.0f, juce::jmax (2.0f, x1 - x0), plot.getHeight() - 22.0f);
                juce::Path outline, dashed;
                outline.addRoundedRectangle (r, 5.0f);
                const float dashes[] { 5.0f, 4.0f };
                juce::PathStrokeType (1.3f).createDashedStroke (dashed, outline, dashes, 2);
                g.setColour (Colours::info.withAlpha (0.9f));
                g.fillPath (dashed);
                g.setFont (font (10.5f, 700));
                g.drawText ("LEARNING", r.withHeight (16.0f).translated (0.0f, 4.0f), juce::Justification::centred, false);
            }
        }

        if (! metersValid)
            return;

        for (int b = 0; b < kBands; ++b)
        {
            const float x0 = display.xForHz (edges[(size_t) b]) + 1.0f, x1 = display.xForHz (edges[(size_t) b + 1]) - 1.0f;

            if (x1 - x0 < 1.0f)
                continue;

            const float v = gr[(size_t) b];

            if (v < -0.05f)
            {
                const float y = right.dbToY (v);
                const auto bar = juce::Rectangle<float> (x0, y0, x1 - x0, y - y0);
                g.setGradientFill (juce::ColourGradient::vertical (c.withAlpha (0.28f), y0, c.withAlpha (0.62f), y));
                g.fillRect (bar);
                g.setColour (c);
                g.fillRect (bar.withTop (y - 2.0f).withHeight (2.0f));
            }

            // The held peak: a ghost of the bar with a thin cap.
            if (holds[(size_t) b] < -0.3f && holds[(size_t) b] < v - 0.2f)
            {
                const float y = std::round (right.dbToY (holds[(size_t) b]));
                g.setColour (c.withAlpha (0.1f));
                g.fillRect (juce::Rectangle<float> (x0, juce::jmax (y0, right.dbToY (v)), x1 - x0,
                                                    juce::jmax (0.0f, y - juce::jmax (y0, right.dbToY (v)))));
                g.setColour (c.brighter (0.4f).withAlpha (0.8f));
                g.fillRect (juce::Rectangle<float> (x0, y - 0.5f, x1 - x0, 1.5f));
            }
        }
    }

    //==============================================================================
    void layout (juce::Rectangle<int> content)
    {
        auto c = content.reduced (12, 6).withTrimmedBottom (6);
        const int cardsH = juce::jlimit (124, 156, juce::roundToInt ((float) c.getHeight() * 0.27f));
        auto cards = c.removeFromBottom (cardsH);
        c.removeFromBottom (kGap);

        const int sideW = juce::jlimit (140, 180, juce::roundToInt ((float) c.getWidth() * 0.17f));
        auto side = c.removeFromRight (sideW);
        c.removeFromRight (kGap);
        display.setBounds (c);
        trace.setBounds (c);

        viewSwitch.setBounds (side.removeFromTop (26));
        side.removeFromTop (kGap);
        learn.setBounds (side.removeFromBottom (28));
        side.removeFromBottom (kGap);
        meters.setBounds (side);

        const int total = cards.getWidth() - 2 * kGap;
        const int reductionW = juce::roundToInt ((float) total * 0.42f);
        const int timingW = (total - reductionW) / 2;
        reductionCard.setBounds (cards.removeFromLeft (reductionW));
        cards.removeFromLeft (kGap);
        timingCard.setBounds (cards.removeFromLeft (timingW));
        cards.removeFromLeft (kGap);
        regionCard.setBounds (cards);

        const int segW = juce::jlimit (150, 220, reductionCard.getWidth() - 110);
        reductionCard.setTitleReserve (segW + 8);
        mode.setBounds (reductionCard.getTitleRowRight (segW));
        spectral::layoutRow (reductionCard.getBody(), 96, amount, tone);
        spectral::layoutRow (timingCard.getBody(), 96, attack, release);
        spectral::layoutRow (regionCard.getBody(), 96, low, high);
        updateTexts();
    }

    SmootherPanel& owner;
    PureSheenProcessor& processor;
    EditorServices& services;
    juce::Colour colour;

    SpectralDisplay display;
    SpectralTraceView trace;
    stereo::ChoiceSegments viewSwitch;
    SpectralMeterColumn meters;
    PillButton learn;
    stereo::Card reductionCard, timingCard, regionCard;
    ParamSegments mode;
    ParamKnob amount, tone, attack, release, low, high;

    std::array<float, kBands> gr {}, holds {}, scores {};
    std::array<int, kBands> holdTicks {};
    double sampleRate = 48000.0;
    bool metersValid = false, showTrace = false, learning = false, previewValid = false;
    int learnTicks = 0, learnSet = 0;
    float scorePeak = 0.0f, previewLo = 0.0f, previewHi = 0.0f;
    spectral::SetState shown { ps::ChannelMode::Stereo, -1, false, false };
};

//==============================================================================
SmootherPanel::SmootherPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::Smoother), impl (std::make_unique<Impl> (*this, p, s))
{
    neverTakeKeyboardFocus (*this);
}

SmootherPanel::~SmootherPanel() = default;

void SmootherPanel::tick()
{
    ModulePanel::tick();
    impl->update();
}

void SmootherPanel::frameTick()
{
    if (impl->showTrace)
        impl->trace.repaint();
    else
        impl->display.repaint();
}

void SmootherPanel::refresh()
{
    ModulePanel::refresh();
    impl->rebind();
}

void SmootherPanel::channelSetChanged() { impl->rebind(); }

void SmootherPanel::setTraceView (bool on) { impl->setTrace (on); }

void SmootherPanel::startLearn()
{
    if (! impl->learning)
        impl->beginLearn();
}

bool SmootherPanel::isLearning() const noexcept { return impl->learning; }

void SmootherPanel::layoutContent (juce::Rectangle<int> contentArea) { impl->layout (contentArea); }

} // namespace ps::ui
