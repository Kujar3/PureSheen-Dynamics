#include "ui/DefinitionPanel.h"

#include "dsp/Definition.h"
#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/SpectralCommon.h"
#include "ui/SpectralDisplay.h"
#include "ui/SpectralWidgets.h"

#include <array>
#include <cmath>

// Definition panel (owner: ui-spectral). Layout and interactions: see DefinitionPanel.h.
// Meters come from the module of the selected set (moduleForChannelSet: the T/S twin for Sustain), polled at 30 Hz in
// tick(); the display repaints in frameTick() with the spectra.
namespace ps::ui
{
namespace
{
constexpr auto kType = ps::ModuleType::Definition;
constexpr int kPoints = ps::kDefinitionDisplayPoints;
constexpr int kGap = 8;
constexpr float kLeftMin = -84.0f, kLeftMax = 0.0f;    // spectrum level (grid every 12 dB)
constexpr float kRightZero = -36.0f;                   // the lift's 0 dB sits on the -36 dB line
constexpr float kRightScale = 1.0f / 6.0f;             // right axis -8 .. +6 dB (labels every 2 dB)
constexpr float kMinRatio = 1.25992105f;               // 2^(1/3): readDefinitionParams keeps high >= low x this
constexpr float kProcessedFromHz = 300.0f;

using spectral::setParamId;
using spectral::twinParamId;

juce::String tiltText (float tilt)
{
    if (std::abs (tilt) < 0.05f)
        return "pink";

    return "pink " + juce::String (tilt > 0.0f ? "+" : "") + juce::String (tilt, 1) + " dB/oct";
}
} // namespace

//==============================================================================
struct DefinitionPanel::Impl
{
    Impl (DefinitionPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), services (s), colour (moduleColour (kType)),
          display (p, s, kType),
          activity ("Activity"),
          amountCard ("Definition", colour),
          timingCard ("Timing", colour),
          regionCard ("Region", colour),
          amount (p, "defn.a.amount", "Amount"),
          tilt (p, "defn.a.tilt", "Tilt"),
          attack (p, "defn.a.att", "Attack"),
          release (p, "defn.a.rel", "Release"),
          low (p, "defn.a.low", "Low"),
          high (p, "defn.a.high", "High")
    {
        for (int i = 0; i < kPoints; ++i)
            hz[(size_t) i] = (float) ps::definitionDisplayHz (i);

        display.setLeftScale (kLeftMin, kLeftMax);
        display.setRightScale (kRightZero, kRightScale, "Lift");
        display.setSpectra (true, true);
        display.setVeilBelow (kProcessedFromHz, "Not processed");
        display.paintOverlay = [this] (juce::Graphics& g) { paintCurves (g); };
        display.setBaseTooltip ("Definition: grey = input, coloured line = output, dashed = the target (pink noise plus "
                                "Tilt) through the input level, bold = the lift being applied (right scale).");

        activity.setTooltip ("The largest boost and cut of the current lift curve.");
        amount.setTooltip ("Amount: how strongly the response is moved toward the target (0 = no change).");
        tilt.setTooltip ("Tilt: the target's slope against pink noise. Positive is brighter, negative darker.");
        attack.setTooltip ("Attack: how quickly the lift reacts.");
        release.setTooltip ("Release: how quickly the lift returns when the music changes back.");
        low.setTooltip ("Low: where the action region starts (Definition works above 300 Hz).");
        high.setTooltip ("High: where the action region ends.");

        owner.addAndMakeVisible (display);
        owner.addAndMakeVisible (activity);
        owner.addAndMakeVisible (amountCard);
        owner.addAndMakeVisible (timingCard);
        owner.addAndMakeVisible (regionCard);
        amountCard.addAndMakeVisible (amount);
        amountCard.addAndMakeVisible (tilt);
        timingCard.addAndMakeVisible (attack);
        timingCard.addAndMakeVisible (release);
        regionCard.addAndMakeVisible (low);
        regionCard.addAndMakeVisible (high);

        activity.setBars ({ { "Boost", -6.0f, 6.0f, 0.0f, colour, -1, 2.0f, true },
                            { "Cut", -6.0f, 6.0f, 0.0f, Colours::gainReduction, 0, 2.0f, true } });

        rebind();
    }

    //==============================================================================
    spectral::SetState state() const { return spectral::setState (processor, kType, owner.getChannelSet()); }

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
        bind (tilt, "tilt");
        bind (attack, "att");
        bind (release, "rel");
        bind (low, "low");
        bind (high, "high");

        display.setAccentColour (c);
        display.setChannelSet (s.set);
        display.setRegion (setParamId (kType, s.set, "low"), setParamId (kType, s.set, "high"),
                           twinParamId (kType, s, "low"), twinParamId (kType, s, "high"), kMinRatio, true);
        display.setLegend ({ { "Input", Colours::input }, { "Output", c }, { "Target", Colours::text.withAlpha (0.6f) },
                             { "Lift", c } });
        activity.setBarColour (0, c);

        if (s.set != shown.set || s.mode != shown.mode)
        {
            activity.resetHolds();
            haveLine = false;
        }

        shown = s;
        updateTexts();
        display.repaint();
    }

    void updateTexts()
    {
        const auto s = state();
        const float t = processor.getParameterValue (setParamId (kType, s.set, "tilt"));
        const float a = processor.getParameterValue (setParamId (kType, s.set, "amount"));
        const auto setName = channelSetName (s.mode, s.set);

        amountCard.setSubtitle (a < 0.05f ? juce::String ("Off at 0 %") : "Toward " + tiltText (t));
        timingCard.setSubtitle (s.split ? setName : juce::String ("How it moves"));
        regionCard.setSubtitle (fmt::hz (processor.getParameterValue (setParamId (kType, s.set, "low"))) + " - "
                                + fmt::hz (processor.getParameterValue (setParamId (kType, s.set, "high"))));
        display.setHeaderText ("Target: " + tiltText (t));
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
        const auto* defn = dynamic_cast<const ps::Definition*> (&moduleForChannelSet (processor, kType, s.set));
        bool valid = false;

        if (running && defn != nullptr)
        {
            const auto& m = defn->getMeters();

            for (int i = 0; i < kPoints && ! valid; ++i)
                valid = spectral::levelValid (m.levelDb[s.set][i].load());

            for (int i = 0; i < kPoints; ++i)
            {
                const float gv = valid ? m.gainDb[s.set][i].load() : 0.0f;
                gain[(size_t) i] = std::isfinite (gv) ? gv : 0.0f;
            }

            activity.setValue (0, m.maxBoostDb[s.set].load(), valid);
            activity.setValue (1, m.maxCutDb[s.set].load(), valid);
        }
        else
        {
            activity.setValue (0, 0.0f, false);
            activity.setValue (1, 0.0f, false);
        }

        metersValid = valid;

        const float cap = (float) ps::definitionMaxGainDb (processor.getParameterValue (setParamId (kType, s.set, "amount")) / 100.0f);
        activity.setLimit (0, cap);
        activity.setLimit (1, -cap);
        activity.setNote (cap < 0.005f ? juce::String ("Amount 0: off") : "Limit +/-" + spectral::dbNumber (cap, false) + " dB");

        // The target line through the input level inside the region (on the spectrum's scale).
        auto& pre = services.getModulePreSpectrum();
        const float t = processor.getParameterValue (setParamId (kType, s.set, "tilt"));
        const float lo = juce::jmax (kProcessedFromHz, display.getRegionLowHz());
        const float hi = juce::jmax (lo * 1.1f, display.getRegionHighHz());
        lineTilt = t;
        lineLo = lo;
        lineHi = hi;

        if (pre.hasData())
        {
            double sum = 0.0;
            int n = 0;

            for (int i = 0; i < 48; ++i)
            {
                const float f = lo * std::pow (hi / lo, (float) i / 47.0f);
                const float v = pre.levelDbAt (f);

                if (v > -150.0f)
                {
                    sum += v - t * std::log2 (f / 1000.0f);
                    ++n;
                }
            }

            if (n > 8)
            {
                const float m = (float) (sum / n);
                lineOffset = haveLine ? lineOffset + 0.2f * (m - lineOffset) : m;
                haveLine = true;
            }
        }
        else
        {
            haveLine = false;
        }

        juce::String status;

        if (! processor.isModuleInChain (kType) || processor.getParameterValue ("defn.on") < 0.5f)
            status = "Definition is off";
        else if (! valid && ! pre.hasData())
            status = "Play audio to see the spectrum";

        display.setStatusText (status);
    }

    void paintCurves (juce::Graphics& g)
    {
        const auto left = display.getLeftAxis();
        const auto right = display.getRightAxis();
        const auto c = display.getAccentColour();

        if (haveLine)
        {
            std::array<float, 33> fx {}, db {};

            for (size_t i = 0; i < fx.size(); ++i)
            {
                fx[i] = lineLo * std::pow (lineHi / lineLo, (float) i / (float) (fx.size() - 1));
                db[i] = lineOffset + lineTilt * std::log2 (fx[i] / 1000.0f);
            }

            juce::Path dashed;
            const float dashes[] { 6.0f, 4.0f };
            juce::PathStrokeType (1.4f).createDashedStroke (dashed, display.makeCurvePath (fx.data(), db.data(), (int) fx.size(), left),
                                                            dashes, 2);
            g.setColour (Colours::text.withAlpha (0.6f));
            g.fillPath (dashed);
        }

        if (! metersValid)
            return;

        g.setColour (c.withAlpha (0.2f));
        g.fillPath (display.makeBandPath (hz.data(), zeros.data(), gain.data(), kPoints, right));
        const auto curve = display.makeCurvePath (hz.data(), gain.data(), kPoints, right);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.strokePath (curve, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c);
        g.strokePath (curve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    //==============================================================================
    void layout (juce::Rectangle<int> content)
    {
        auto c = content.reduced (12, 6).withTrimmedBottom (6);
        const int cardsH = juce::jlimit (124, 156, juce::roundToInt ((float) c.getHeight() * 0.27f));
        auto cards = c.removeFromBottom (cardsH);
        c.removeFromBottom (kGap);

        const int sideW = juce::jlimit (132, 176, juce::roundToInt ((float) c.getWidth() * 0.16f));
        activity.setBounds (c.removeFromRight (sideW));
        c.removeFromRight (kGap);
        display.setBounds (c);

        const int w = (cards.getWidth() - 2 * kGap) / 3;
        amountCard.setBounds (cards.removeFromLeft (w));
        cards.removeFromLeft (kGap);
        timingCard.setBounds (cards.removeFromLeft (w));
        cards.removeFromLeft (kGap);
        regionCard.setBounds (cards);

        spectral::layoutRow (amountCard.getBody(), 96, amount, tilt);
        spectral::layoutRow (timingCard.getBody(), 96, attack, release);
        spectral::layoutRow (regionCard.getBody(), 96, low, high);
    }

    DefinitionPanel& owner;
    PureSheenProcessor& processor;
    EditorServices& services;
    juce::Colour colour;

    SpectralDisplay display;
    SpectralMeterColumn activity;
    stereo::Card amountCard, timingCard, regionCard;
    ParamKnob amount, tilt, attack, release, low, high;

    std::array<float, kPoints> hz {}, gain {}, zeros {};
    float lineOffset = 0.0f, lineTilt = 0.0f, lineLo = 300.0f, lineHi = 20000.0f;
    bool haveLine = false, metersValid = false;
    spectral::SetState shown { ps::ChannelMode::Stereo, -1, false, false };
};

//==============================================================================
DefinitionPanel::DefinitionPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::Definition), impl (std::make_unique<Impl> (*this, p, s))
{
    neverTakeKeyboardFocus (*this);
}

DefinitionPanel::~DefinitionPanel() = default;

void DefinitionPanel::tick()
{
    ModulePanel::tick();
    impl->update();
}

void DefinitionPanel::frameTick() { impl->display.repaint(); }

void DefinitionPanel::refresh()
{
    ModulePanel::refresh();
    impl->rebind();
}

void DefinitionPanel::channelSetChanged() { impl->rebind(); }

void DefinitionPanel::layoutContent (juce::Rectangle<int> contentArea) { impl->layout (contentArea); }

} // namespace ps::ui
