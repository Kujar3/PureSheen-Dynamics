#include "ui/ToneBalancePanel.h"

#include "dsp/ToneBalance.h"
#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/SpectralCommon.h"
#include "ui/SpectralDisplay.h"
#include "ui/SpectralWidgets.h"

#include <array>
#include <cmath>

// Tone Balance panel (owner: ui-spectral). Layout and interactions: see ToneBalancePanel.h.
// Meters come from the module of the selected set (moduleForChannelSet: the T/S twin for Sustain), polled at 30 Hz in
// tick(); the display repaints in frameTick() with the spectra.
namespace ps::ui
{
namespace
{
constexpr auto kType = ps::ModuleType::ToneBalance;
constexpr int kPoints = ps::kToneDisplayPoints;
constexpr int kGap = 8;
constexpr float kLeftMin = -27.0f, kLeftMax = 18.0f;   // level re pink (1 kHz = 0 dB)
constexpr float kRightScale = 0.5f;                    // correction dB per left dB (right axis -13.5 .. +9)
constexpr float kSpectrumSpread = 2.0f;                // dB of the output spectrum per dB of the left scale

using spectral::setParamId;
using spectral::twinParamId;

// The target menu: genre families (index = choice value = ps::ToneTarget).
struct TargetGroup
{
    const char* name;
    std::vector<ps::ToneTarget> targets;
};

const std::vector<TargetGroup>& targetGroups()
{
    using T = ps::ToneTarget;
    static const std::vector<TargetGroup> groups {
        { "Pop", { T::Pop, T::DancePop, T::Electropop, T::IndiePop, T::KpopJpop, T::Hyperpop } },
        { "Electronic", { T::Edm, T::House, T::Techno, T::DrumAndBass, T::Dubstep, T::FutureBass, T::Ambient } },
        { "Hip-Hop and RnB", { T::HipHop, T::ClassicHipHop, T::Trap, T::LoFi, T::RnbSoul } },
        { "Latin and Caribbean", { T::LatinPop, T::Reggaeton, T::Reggae } },
        { "Rock and Metal", { T::Rock, T::AltRock, T::ClassicRock, T::IndieRock, T::PunkRock, T::PostRock, T::Metal } },
        { "Acoustic, Jazz and Film", { T::Country, T::PopCountry, T::Folk, T::Jazz, T::VocalJazz, T::Cinematic } },
    };
    return groups;
}

juce::String targetName (int index)
{
    const int i = ps::paramIndex ("tbal.target");

    if (i >= 0)
    {
        const auto& choices = ps::paramSpecs()[(size_t) i].choices;

        if (index >= 0 && index < (int) choices.size())
            return choices[(size_t) index];
    }

    return {};
}

juce::String targetGroupName (int index)
{
    if (index == (int) ps::ToneTarget::Balanced || index == (int) ps::ToneTarget::Flat)
        return "General";

    for (const auto& g : targetGroups())
        for (auto t : g.targets)
            if ((int) t == index)
                return g.name;

    return {};
}
} // namespace

//==============================================================================
struct ToneBalancePanel::Impl
{
    Impl (ToneBalancePanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), services (s), colour (moduleColour (kType)),
          display (p, s, kType),
          targetCard ("Target", colour),
          mode (p, "tbal.mode", { "Shape", "Cut", "Boost" }),
          tame (p, "tbal.a.tame", "Tame Transients"),
          range ("Range"),
          correctionCard ("Correction", colour),
          regionCard ("Regions", colour),
          amount (p, "tbal.a.amount", "Amount"),
          speed (p, "tbal.a.speed", "Speed"),
          cutSpeed (p, "tbal.a.cutSpeed", "Cut Speed"),
          smooth (p, "tbal.a.smooth", "Smoothing"),
          sens (p, "tbal.a.sens", "Sensitivity"),
          low (p, "tbal.a.low", "Low"),
          mid (p, "tbal.a.mid", "Mid"),
          high (p, "tbal.a.high", "High")
    {
        for (int i = 0; i < kPoints; ++i)
            hz[(size_t) i] = (float) ps::toneBalanceDisplayHz (i);

        display.setLeftScale (kLeftMin, kLeftMax);
        display.setRightScale (0.0f, kRightScale, "Correction");
        display.setSpectra (false, true);
        display.setSpectrumStyle (0.0f, 0.42f, true);
        display.paintOverlay = [this] (juce::Graphics& g) { paintCurves (g); };
        display.setBaseTooltip ("Tone Balance: the shaded corridor is the target, the light line the music's measured "
                                "balance, the bold line the correction being applied (right scale).");

        targetChip.onOpenMenu = [this] (MenuChip& chip) { showTargetMenu (chip); };
        targetChip.setTooltip ("Target: the tonal balance the module steers the music toward (our own curves, by genre "
                               "family). Flat follows pink noise.");
        mode.setTooltip ("Shape: boosts and cuts toward the target. Cut: only cuts what rises above the corridor "
                         "(resonances). Boost: only lifts what falls below it.");
        tame.setTooltip ("Tame Transients: reacts at once to sudden hits instead of letting them through.");
        range.setTooltip ("The largest boost and cut of the current correction.");
        amount.setTooltip ("Amount: how much of the correction is applied (0 = no change).");
        speed.setTooltip ("Speed: how quickly the correction follows the music. Faster is more precise in time but can "
                          "be audible.");
        cutSpeed.setTooltip ("Cut Speed: how quickly cuts react to peaks.");
        smooth.setTooltip ("Smoothing: high values give a few broad moves, low values many narrow ones.");
        sens.setTooltip ("Sensitivity: low values cut only the worst peaks, high values cut anything above the target.");
        low.setTooltip ("Low: how much the correction acts below 100 Hz.");
        mid.setTooltip ("Mid: how much the correction acts from 100 Hz to 5.6 kHz.");
        high.setTooltip ("High: how much the correction acts above 5.6 kHz.");

        owner.addAndMakeVisible (display);
        owner.addAndMakeVisible (targetCard);
        targetCard.addAndMakeVisible (targetChip);
        targetCard.addAndMakeVisible (mode);
        targetCard.addAndMakeVisible (tame);
        owner.addAndMakeVisible (range);
        owner.addAndMakeVisible (correctionCard);
        owner.addAndMakeVisible (regionCard);

        for (auto* k : { &amount, &speed, &cutSpeed, &smooth, &sens })
            correctionCard.addAndMakeVisible (*k);

        for (auto* k : { &low, &mid, &high })
            regionCard.addAndMakeVisible (*k);

        range.setBars ({ { "Boost", -12.0f, 9.0f, 0.0f, colour, -1, 3.0f, true },
                         { "Cut", -12.0f, 9.0f, 0.0f, Colours::gainReduction, 0, 3.0f, true } });

        rebind();
    }

    //==============================================================================
    spectral::SetState state() const { return spectral::setState (processor, kType, owner.getChannelSet()); }

    int currentMode() const { return juce::jlimit (0, 2, juce::roundToInt (processor.getParameterValue ("tbal.mode"))); }
    int currentTarget() const
    {
        return juce::jlimit (0, (int) ps::ToneTarget::Count - 1, juce::roundToInt (processor.getParameterValue ("tbal.target")));
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
        bind (speed, "speed");
        bind (cutSpeed, "cutSpeed");
        bind (smooth, "smooth");
        bind (sens, "sens");
        bind (low, "low");
        bind (mid, "mid");
        bind (high, "high");
        tame.setParamId (setParamId (kType, s.set, "tame"));
        tame.setLinkedParamId (twinParamId (kType, s, "tame"));
        tame.setOnColour (c);
        mode.setOnColour (colour);

        display.setAccentColour (c);
        display.setZones ({
            { 20.0f, 100.0f, "Low", setParamId (kType, s.set, "low"), twinParamId (kType, s, "low"),
              "Low (below 100 Hz): drag to set how much the correction acts here. Double-click resets it." },
            { 100.0f, 5600.0f, "Mid", setParamId (kType, s.set, "mid"), twinParamId (kType, s, "mid"),
              "Mid (100 Hz to 5.6 kHz): drag to set how much the correction acts here. Double-click resets it." },
            { 5600.0f, 20000.0f, "High", setParamId (kType, s.set, "high"), twinParamId (kType, s, "high"),
              "High (above 5.6 kHz): drag to set how much the correction acts here. Double-click resets it." } });
        display.setLegend ({ { "Target", c.withAlpha (0.6f) }, { "Measured", Colours::text.withAlpha (0.75f) },
                             { "Output", c.withAlpha (0.45f) }, { "Correction", c } });
        range.setBarColour (0, c);

        if (s.set != shown.set || s.mode != shown.mode)
        {
            range.resetHolds();
            haveMeasured = false;
        }

        shown = s;
        shownMode = -1;
        shownTarget = -1;
        updateTexts();
        layoutKnobs();
        display.repaint();
    }

    void updateTexts()
    {
        const int m = currentMode();
        const int t = currentTarget();

        if (m != shownMode)
        {
            shownMode = m;
            static const char* subtitles[] { "Boosts and cuts toward the target", "Cuts peaks above the corridor",
                                             "Lifts dips below the corridor" };
            correctionCard.setSubtitle (subtitles[m]);
            layoutKnobs();
        }

        if (t != shownTarget)
        {
            shownTarget = t;
            targetChip.setText ({}, targetName (t));
            const auto group = targetGroupName (t);
            targetCard.setSubtitle (group != targetName (t) ? group : juce::String());

            for (int i = 0; i < kPoints; ++i)
            {
                double tol = 0.0;
                target[(size_t) i] = (float) ps::toneTargetDb ((ps::ToneTarget) t, hz[(size_t) i], &tol);
                tolLow[(size_t) i] = target[(size_t) i] - (float) tol;
                tolHigh[(size_t) i] = target[(size_t) i] + (float) tol;
            }

            // Mean of the target over 100 Hz .. 5.6 kHz (where the module takes its level offset).
            double sum = 0.0;
            int n = 0;

            for (int i = 0; i < kPoints; ++i)
                if (hz[(size_t) i] >= 100.0f && hz[(size_t) i] <= 5600.0f)
                {
                    sum += target[(size_t) i];
                    ++n;
                }

            targetMid = n > 0 ? (float) (sum / n) : 0.0f;
            display.repaint();
        }

        const auto s = state();
        regionCard.setSubtitle (s.split ? channelSetName (s.mode, s.set) : juce::String ("Where it corrects"));
    }

    void showTargetMenu (MenuChip& chip)
    {
        const int current = currentTarget();
        juce::PopupMenu menu;
        menu.addSectionHeader ("Target");
        menu.addItem ((int) ps::ToneTarget::Balanced + 1, targetName ((int) ps::ToneTarget::Balanced), true,
                      current == (int) ps::ToneTarget::Balanced);
        menu.addItem ((int) ps::ToneTarget::Flat + 1, targetName ((int) ps::ToneTarget::Flat), true,
                      current == (int) ps::ToneTarget::Flat);
        menu.addSeparator();

        for (const auto& group : targetGroups())
        {
            juce::PopupMenu sub;
            bool containsCurrent = false;

            for (auto t : group.targets)
            {
                sub.addItem ((int) t + 1, targetName ((int) t), true, (int) t == current);
                containsCurrent = containsCurrent || (int) t == current;
            }

            menu.addSubMenu (group.name, sub, true, nullptr, containsCurrent);
        }

        chip.setMenuOpen (true);
        juce::Component::SafePointer<MenuChip> safe (&chip);
        showMenu (menu, chip, [this, safe] (int result)
        {
            if (safe != nullptr)
                safe->setMenuOpen (false);

            if (result > 0)
                processor.setParameterValue ("tbal.target", (float) (result - 1));
        }, 200);
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
        const auto* tb = dynamic_cast<const ps::ToneBalance*> (&moduleForChannelSet (processor, kType, s.set));
        bool valid = false;

        if (running && tb != nullptr)
        {
            const auto& m = tb->getMeters();
            const float offset = m.targetOffsetDb[s.set].load();

            for (int i = 0; i < kPoints && ! valid; ++i)
                valid = spectral::levelValid (m.levelDb[s.set][i].load());

            if (valid)
            {
                for (int i = 0; i < kPoints; ++i)
                {
                    const float level = m.levelDb[s.set][i].load();
                    const float rel = spectral::levelValid (level) ? level - offset : target[(size_t) i];
                    auto& mv = measured[(size_t) i];
                    mv = haveMeasured ? mv + 0.5f * (rel - mv) : rel;
                    const float gv = m.gainDb[s.set][i].load();
                    gain[(size_t) i] = std::isfinite (gv) ? gv : 0.0f;
                }

                haveMeasured = true;
            }

            range.setValue (0, m.maxBoostDb[s.set].load(), valid);
            range.setValue (1, m.maxCutDb[s.set].load(), valid);
        }
        else
        {
            range.setValue (0, 0.0f, false);
            range.setValue (1, 0.0f, false);
        }

        if (! valid)
            haveMeasured = false;

        metersValid = valid;

        // The module's limits at this Amount: +9 x Amount (Shape / Boost) and -12 x Amount (Shape / Cut).
        const float amountNow = processor.getParameterValue (setParamId (kType, s.set, "amount")) / 100.0f;
        const int modeNow = currentMode();
        const float boostCap = modeNow == (int) ps::ToneBalanceMode::Cut ? 0.0f : 9.0f * amountNow;
        const float cutCap = modeNow == (int) ps::ToneBalanceMode::Boost ? 0.0f : -12.0f * amountNow;
        range.setLimit (0, boostCap);
        range.setLimit (1, cutCap);
        range.setNote (amountNow < 0.0005f ? juce::String ("Amount 0: off")
                                           : "Limit " + spectral::dbNumber (boostCap) + " / " + spectral::dbNumber (cutCap));

        // Slide the output spectrum onto the corridor: its mean over 100 Hz .. 5.6 kHz sits on the target's mean.
        auto& post = services.getModulePostSpectrum();

        if (post.hasData())
        {
            double sum = 0.0;
            int n = 0;

            for (int i = 0; i < kPoints; ++i)
                if (hz[(size_t) i] >= 100.0f && hz[(size_t) i] <= 5600.0f)
                {
                    const float v = post.levelDbAt (hz[(size_t) i]);

                    if (v > -150.0f)
                    {
                        sum += v;
                        ++n;
                    }
                }

            if (n > 4)
            {
                const float k = targetMid - (float) (sum / n);
                specShift = haveShift ? specShift + 0.15f * (k - specShift) : k;
                haveShift = true;
            }
        }

        // The spectrum is a backdrop: drawn at half the dB scale of the curves, centred on the target mean.
        display.setSpectrumRange (targetMid - specShift + kSpectrumSpread * (kLeftMin - targetMid),
                                  targetMid - specShift + kSpectrumSpread * (kLeftMax - targetMid));

        juce::String status;

        if (! processor.isModuleInChain (kType) || processor.getParameterValue ("tbal.on") < 0.5f)
            status = "Tone Balance is off";
        else if (! valid)
            status = "Play audio to see the balance";

        display.setStatusText (status);
    }

    void paintCurves (juce::Graphics& g)
    {
        const auto left = display.getLeftAxis();
        const auto right = display.getRightAxis();
        const auto c = display.getAccentColour();

        // Target corridor.
        g.setColour (c.withAlpha (0.11f));
        g.fillPath (display.makeBandPath (hz.data(), tolLow.data(), tolHigh.data(), kPoints, left));
        g.setColour (c.withAlpha (0.32f));
        g.strokePath (display.makeCurvePath (hz.data(), tolLow.data(), kPoints, left), juce::PathStrokeType (1.0f));
        g.strokePath (display.makeCurvePath (hz.data(), tolHigh.data(), kPoints, left), juce::PathStrokeType (1.0f));
        {
            juce::Path dashed;
            const float dashes[] { 5.0f, 4.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, display.makeCurvePath (hz.data(), target.data(), kPoints, left),
                                                            dashes, 2);
            g.setColour (c.withAlpha (0.6f));
            g.fillPath (dashed);
        }

        if (! metersValid)
            return;

        // Measured balance (what the module hears, relative to the floating target).
        g.setColour (Colours::text.withAlpha (0.72f));
        g.strokePath (display.makeCurvePath (hz.data(), measured.data(), kPoints, left),
                      juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Correction on the right scale, filled to zero.
        g.setColour (c.withAlpha (0.18f));
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

        const int sideW = juce::jlimit (176, 236, juce::roundToInt ((float) c.getWidth() * 0.2f));
        auto side = c.removeFromRight (sideW);
        c.removeFromRight (kGap);
        display.setBounds (c);

        const int targetH = juce::jmin (side.getHeight() / 2 + 20, stereo::Card::kTitleH + 26 + 6 + 24 + 6 + 24 + 12);
        targetCard.setBounds (side.removeFromTop (targetH));
        side.removeFromTop (kGap);
        range.setBounds (side);

        {
            auto body = targetCard.getBody();
            targetChip.setBounds (body.removeFromTop (26));
            body.removeFromTop (6);
            mode.setBounds (body.removeFromTop (24));
            body.removeFromTop (6);
            tame.setBounds (body.removeFromTop (24));
        }

        const int half = (cards.getWidth() - kGap) / 2;
        correctionCard.setBounds (cards.removeFromLeft (half));
        cards.removeFromLeft (kGap);
        regionCard.setBounds (cards);
        layoutKnobs();
    }

    void layoutKnobs()
    {
        const bool cut = currentMode() == (int) ps::ToneBalanceMode::Cut;
        speed.setVisible (! cut);
        smooth.setVisible (! cut);
        cutSpeed.setVisible (cut);
        sens.setVisible (cut);

        const auto body = correctionCard.getBody();

        if (body.getHeight() > 8)
        {
            spectral::layoutRow (body, 96, amount, cut ? cutSpeed : speed, cut ? sens : smooth);
            (cut ? speed : cutSpeed).setBounds ((cut ? cutSpeed : speed).getBounds());
            (cut ? smooth : sens).setBounds ((cut ? sens : smooth).getBounds());
        }

        const auto rbody = regionCard.getBody();

        if (rbody.getHeight() > 8)
            spectral::layoutRow (rbody, 96, low, mid, high);
    }

    ToneBalancePanel& owner;
    PureSheenProcessor& processor;
    EditorServices& services;
    juce::Colour colour;

    SpectralDisplay display;
    stereo::Card targetCard;
    MenuChip targetChip;
    ParamSegments mode;
    ParamToggle tame;
    SpectralMeterColumn range;
    stereo::Card correctionCard, regionCard;
    ParamKnob amount, speed, cutSpeed, smooth, sens, low, mid, high;

    std::array<float, kPoints> hz {}, target {}, tolLow {}, tolHigh {}, measured {}, gain {}, zeros {};
    float targetMid = 0.0f, specShift = 0.0f;
    bool haveMeasured = false, haveShift = false, metersValid = false;
    spectral::SetState shown { ps::ChannelMode::Stereo, -1, false, false };
    int shownMode = -1, shownTarget = -1;
};

//==============================================================================
ToneBalancePanel::ToneBalancePanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::ToneBalance), impl (std::make_unique<Impl> (*this, p, s))
{
    neverTakeKeyboardFocus (*this);
}

ToneBalancePanel::~ToneBalancePanel() = default;

void ToneBalancePanel::tick()
{
    ModulePanel::tick();
    impl->update();
}

void ToneBalancePanel::frameTick() { impl->display.repaint(); }

void ToneBalancePanel::refresh()
{
    ModulePanel::refresh();
    impl->rebind();
}

void ToneBalancePanel::channelSetChanged() { impl->rebind(); }

void ToneBalancePanel::layoutContent (juce::Rectangle<int> contentArea) { impl->layout (contentArea); }

} // namespace ps::ui
