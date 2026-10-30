#include "ui/BassShaperPanel.h"

#include "dsp/BassShaper.h"
#include "ui/BassScope.h"
#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/LowEndWidgets.h"
#include "ui/MaximizerWidgets.h"

#include <cmath>

// Bass Shaper panel. Owner: ui-lowend. Layout and interactions: see BassShaperPanel.h.
//
// Meters: BassShaperMeters (balance / crest of the input and the output, transient share, sustain lift, peak
// reduction). The output-side meters and the scope read the low part before auto gain (BassShaper.cpp), so they line up
// with the thresholds. The scope FIFOs and lowPeakDb have exactly one consumer: the BassScope feed.
namespace ps::ui
{
namespace
{
constexpr int kGap = 8;

void layoutCharacter (stereo::Card& card, juce::Component& fader, juce::Rectangle<int> area)
{
    // The fader box, with "Dirty" / "Clean" under its two ends.
    fader.setBounds (area.removeFromTop (24));
    const auto f = font (11.5f, 600);
    const auto captions = area.withHeight (16);
    const int dw = (int) textWidth (f, "Dirty") + 4, cw = (int) textWidth (f, "Clean") + 4;
    card.setCaptions ({ { "Dirty", captions.withWidth (dw) }, { "Clean", captions.withLeft (captions.getRight() - cw) } });
}
} // namespace

//==============================================================================
struct BassShaperPanel::Impl
{
    Impl (BassShaperPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), colour (moduleColour (ps::ModuleType::BassShaper)),
          balanceCard ("Balance", colour), balance (p, "bass.balance", "Balance"),
          punchCard ("Punch", colour), punch (p, "bass.punch", "Punch"),
          scope (p), cutoff (p, s, ps::ModuleType::BassShaper, "bass.cutoff"),
          susCard ("Sustain Power", Colours::info), susOn (p, "bass.sus.on", Colours::info),
          susThr (p, "bass.sus.thr", "Threshold", "bass.sus.on"), susAmount (p, "bass.sus.amount", "Amount", "bass.sus.on"),
          susChar (p, "bass.sus.char", "Character", "bass.sus.on"),
          peakCard ("Peak Control", Colours::major), peakOn (p, "bass.peak.on", Colours::major),
          peakThr (p, "bass.peak.thr", "Threshold", "bass.peak.on"), peakMix (p, "bass.peak.mix", "Mix", "bass.peak.on"),
          peakChar (p, "bass.peak.char", "Character", "bass.peak.on"),
          autoGain (p, "bass.autoGain", "Auto Gain")
    {
        balanceMeter.setLabels ("Light", "Heavy");
        balanceMeter.setColour (colour);
        balanceMeter.setFormatter ([] (float v) { return fmt::db (v, 1, false); });
        balanceMeter.setTooltip ("Balance meter: the level of the low end (below the cutoff) against the whole mix. The "
                                 "green zone is a typical balance for this cutoff; the hollow ball shows the output.");
        punchMeter.setLabels ("Dense", "Spiky");
        punchMeter.setColour (colour);
        punchMeter.setRange (2.0f, 20.0f);
        punchMeter.setZone (8.0f, 13.0f);
        punchMeter.setFormatter ([] (float v) { return juce::String (v, 1) + " dB"; });
        punchMeter.setTooltip ("Punch meter: the crest factor of the low end (peak against average). Left = dense and "
                               "compressed, right = spiky and dynamic; the hollow ball shows the output.");

        balance.setColour (colour);
        punch.setColour (colour);
        balance.setTooltip ("Balance: the level of everything below the cutoff.\nDrag to change (Ctrl: fine), "
                            "double-click to reset.");
        punch.setTooltip ("Punch: the level of the low-end hits (kicks, plucked bass) without touching the sustained "
                          "body.\nDrag to change (Ctrl: fine), double-click to reset.");

        for (auto* k : { &susThr, &susAmount })
            k->setColour (Colours::info);

        for (auto* k : { &peakThr, &peakMix })
            k->setColour (Colours::major);

        susChar.setColour (Colours::info);
        peakChar.setColour (Colours::major);
        cutoff.setColour (colour);
        scope.setColour (colour);
        autoGain.setOnColour (colour);

        susOn.setTooltip ("Sustain Power on / off: lifts the quiet sustained low end up toward the threshold.");
        susThr.setTooltip ("Sustain threshold (the dashed lines in the scope): sustained low end below it is lifted.");
        susAmount.setTooltip ("Sustain amount: how strongly the sustain is lifted (up to 12 dB).");
        susChar.setTooltip ("Sustain character: Clean keeps the tone; toward Dirty it adds harmonics that help the bass "
                            "come through on small speakers.");
        peakOn.setTooltip ("Peak Control on / off: a fast limiter on the low band only.");
        peakThr.setTooltip ("Peak threshold (the solid lines in the scope): low-band peaks are held at this level.");
        peakMix.setTooltip ("Peak mix: blend of the limited low band with the unlimited one.");
        peakChar.setTooltip ("Peak character: Clean limits transparently; toward Dirty it clips, so kicks cut through.");
        autoGain.setTooltip ("Auto Gain: keeps the overall level the same while you shape the low end.");

        owner.addAndMakeVisible (balanceCard);
        balanceCard.addAndMakeVisible (balanceMeter);
        balanceCard.addAndMakeVisible (balance);
        owner.addAndMakeVisible (punchCard);
        punchCard.addAndMakeVisible (punchMeter);
        punchCard.addAndMakeVisible (punch);

        owner.addAndMakeVisible (scope);
        owner.addAndMakeVisible (cutoff);

        owner.addAndMakeVisible (susCard);
        susCard.addAndMakeVisible (susOn);
        susCard.addAndMakeVisible (susThr);
        susCard.addAndMakeVisible (susAmount);
        susCard.addAndMakeVisible (susChar);
        susCard.setTitleReserve (44);

        owner.addAndMakeVisible (peakCard);
        peakCard.addAndMakeVisible (peakOn);
        peakCard.addAndMakeVisible (peakThr);
        peakCard.addAndMakeVisible (peakMix);
        peakCard.addAndMakeVisible (peakChar);
        peakCard.setTitleReserve (44);

        owner.addAndMakeVisible (autoGain);
        update();
    }

    ps::BassShaper* shaper() const
    {
        return dynamic_cast<ps::BassShaper*> (&processor.getChain().getModule (ps::ModuleType::BassShaper));
    }

    //==========================================================================
    void update()
    {
        susOn.refreshValue();
        peakOn.refreshValue();
        cutoff.tick();

        const bool running = processor.isModuleInChain (ps::ModuleType::BassShaper)
                             && processor.getParameterValue ("bass.on") >= 0.5f && processor.getChain().getSampleRate() > 0.0;
        const auto* bs = shaper();
        const bool valid = running && bs != nullptr;

        const float hz = processor.getParameterValue ("bass.cutoff");
        const float centre = (float) ps::bassShaperBalanceTargetDb (hz);
        balanceMeter.setRange (centre - 12.0f, centre + 12.0f);
        balanceMeter.setZone (centre - 3.0f, centre + 3.0f);
        balanceCard.setSubtitle ("Target " + fmt::db (centre, 1, false));

        float share = 0.0f, lift = 0.0f, reduction = 0.0f;

        if (valid)
        {
            const auto& m = bs->getMeters();
            balanceMeter.setValues (m.balanceDb.load(), m.balanceOutDb.load(), m.balanceDb.load() > -59.0f);
            punchMeter.setValues (m.crestDb.load(), m.crestOutDb.load(), m.balanceDb.load() > -59.0f);
            share = std::isfinite (m.transientShare.load()) ? m.transientShare.load() : 0.0f;
            lift = std::isfinite (m.sustainLiftDb.load()) ? m.sustainLiftDb.load() : 0.0f;
            reduction = std::isfinite (m.peakReductionDb.load()) ? m.peakReductionDb.load() : 0.0f;
        }
        else
        {
            balanceMeter.setValues (0.0f, 0.0f, false);
            punchMeter.setValues (0.0f, 0.0f, false);
        }

        punchCard.setSubtitle (valid ? "Hits " + juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, share) * 100.0f)) + " %"
                                     : juce::String());

        const bool sOn = susOn.isOn(), pOn = peakOn.isOn();
        susCard.setSubtitle (! sOn ? juce::String ("Off") : (valid ? "Lift " + fmt::db (juce::jmax (0.0f, lift)) : juce::String()));
        peakCard.setSubtitle (! pOn ? juce::String ("Off")
                                    : (valid ? "Reduction " + fmt::db (juce::jmin (0.0f, reduction) > -0.05f ? 0.0f : reduction)
                                             : juce::String()));

        for (auto* c : std::initializer_list<juce::Component*> { &susThr, &susAmount, &susChar })
            c->setAlpha (sOn ? 1.0f : 0.55f);

        for (auto* c : std::initializer_list<juce::Component*> { &peakThr, &peakMix, &peakChar })
            c->setAlpha (pOn ? 1.0f : 0.55f);

        scope.update();
    }

    //==========================================================================
    void layout (juce::Rectangle<int> content)
    {
        auto r = content.reduced (12, 6).withTrimmedBottom (6);
        const int W = r.getWidth();
        const int leftW = juce::jlimit (180, 270, juce::roundToInt ((float) W * 0.22f));
        const int rightW = juce::jlimit (230, 330, juce::roundToInt ((float) W * 0.27f));

        auto left = r.removeFromLeft (leftW);
        r.removeFromLeft (kGap);
        auto right = r.removeFromRight (rightW);
        r.removeFromRight (kGap);
        auto centre = r;

        // Left: Balance and Punch
        {
            const int h = (left.getHeight() - kGap) / 2;
            balanceCard.setBounds (left.removeFromTop (h));
            left.removeFromTop (kGap);
            punchCard.setBounds (left);

            for (auto* pair : { &balancePair, &punchPair })
            {
                auto& card = *pair->card;
                auto body = card.getBody();
                // Meter and knob as one group, centred in the card (no empty band between them on large sizes).
                body = body.withSizeKeepingCentre (body.getWidth(), juce::jmin (body.getHeight(), lowend::SweetSpotMeter::kIdealHeight + 14 + 104));
                pair->meter->setBounds (body.removeFromTop (lowend::SweetSpotMeter::kIdealHeight));
                body.removeFromTop (juce::jmin (14, juce::jmax (4, body.getHeight() - 96)));
                const int kw = juce::jmin (110, body.getWidth());
                pair->knob->setBounds (body.withSizeKeepingCentre (kw, juce::jmin (body.getHeight(), 104)));
            }
        }

        // Centre: scope above the cutoff strip
        {
            const int stripH = juce::jlimit (54, 72, juce::roundToInt ((float) centre.getHeight() * 0.12f));
            cutoff.setBounds (centre.removeFromBottom (stripH));
            centre.removeFromBottom (kGap);
            scope.setBounds (centre);
        }

        // Right: Sustain Power, Peak Control, Auto Gain
        {
            autoGain.setBounds (right.removeFromBottom (26));
            right.removeFromBottom (kGap);
            const int h = (right.getHeight() - kGap) / 2;
            susCard.setBounds (right.removeFromTop (h));
            right.removeFromTop (kGap);
            peakCard.setBounds (right);

            auto section = [] (stereo::Card& card, MaxSwitch& power, juce::Component& a, juce::Component& b, juce::Component& fader)
            {
                power.setBounds (card.getTitleRowRight (36));
                auto body = card.getBody();
                body = body.withSizeKeepingCentre (body.getWidth(), juce::jmin (body.getHeight(), 100 + 16 + 40));
                auto bottom = body.removeFromBottom (40);
                layoutCharacter (card, fader, bottom);
                body.removeFromBottom (4);
                const int kw = juce::jmin (100, (body.getWidth() - 12) / 2);
                auto knobs = body.withSizeKeepingCentre (kw * 2 + 12, juce::jmin (body.getHeight(), 100));
                a.setBounds (knobs.removeFromLeft (kw));
                b.setBounds (knobs.removeFromRight (kw));
            };

            section (susCard, susOn, susThr, susAmount, susChar);
            section (peakCard, peakOn, peakThr, peakMix, peakChar);
        }
    }

    struct MeterPair
    {
        stereo::Card* card;
        lowend::SweetSpotMeter* meter;
        ParamKnob* knob;
    };

    BassShaperPanel& owner;
    PureSheenProcessor& processor;
    juce::Colour colour;

    stereo::Card balanceCard;
    lowend::SweetSpotMeter balanceMeter;
    ParamKnob balance;
    stereo::Card punchCard;
    lowend::SweetSpotMeter punchMeter;
    ParamKnob punch;

    BassScope scope;
    lowend::CutoffStrip cutoff;

    stereo::Card susCard;
    MaxSwitch susOn;
    lowend::SectionKnob susThr, susAmount;
    lowend::SectionFader susChar;

    stereo::Card peakCard;
    MaxSwitch peakOn;
    lowend::SectionKnob peakThr, peakMix;
    lowend::SectionFader peakChar;

    ParamToggle autoGain;

    MeterPair balancePair { &balanceCard, &balanceMeter, &balance };
    MeterPair punchPair { &punchCard, &punchMeter, &punch };
};

//==============================================================================
BassShaperPanel::BassShaperPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::BassShaper), impl (std::make_unique<Impl> (*this, p, s))
{
    neverTakeKeyboardFocus (*this);
}

BassShaperPanel::~BassShaperPanel() = default;

BassScope& BassShaperPanel::getScope() noexcept { return impl->scope; }

int BassShaperPanel::getNumControls() const noexcept
{
    // cutoff, balance, punch, sustain on / threshold (knob + scope lines) / amount / character, peak on / threshold
    // (knob + scope lines) / character / mix, auto gain
    return 12;
}

void BassShaperPanel::tick()
{
    ModulePanel::tick();
    impl->update();
}

void BassShaperPanel::frameTick()
{
    impl->scope.update();
    impl->cutoff.repaint();
}

void BassShaperPanel::refresh()
{
    ModulePanel::refresh();
    impl->update();
}

void BassShaperPanel::paint (juce::Graphics& g) { ModulePanel::paint (g); }

void BassShaperPanel::layoutContent (juce::Rectangle<int> contentArea) { impl->layout (contentArea); }

} // namespace ps::ui
