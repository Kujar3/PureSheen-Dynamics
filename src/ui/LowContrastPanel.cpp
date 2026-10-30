#include "ui/LowContrastPanel.h"

#include "dsp/LowContrast.h"
#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/LowEndWidgets.h"
#include "ui/LowRegionDisplay.h"

#include <cmath>

// Low Contrast panel. Owner: ui-lowend. Layout and interactions: see LowContrastPanel.h.
//
// Meters: LowContrastMeters of the instance that processes the selected channel set (moduleForChannelSet: the T/S
// twin for the Sustain part), index = the set (Stereo / T/S mirror set A into set B). The spectra come from the
// editor's module taps (EditorServices); this panel pulls no FIFO.
namespace ps::ui
{
namespace
{
constexpr int kGap = 8;
constexpr float kThird = 1.25992105f;   // the module keeps high >= low x 2^(1/3)
const juce::String kLowId ("lowc.low"), kHighId ("lowc.high");

const char* const kModeNotes[] = {
    "Fast response: tightens kicks and bass hits.",
    "Slow response: shapes sustained bass notes.",
};

//==============================================================================
// A region edge knob: dragging one edge into the other pushes the other along (same gesture), as in the display.
class EdgeKnob final : public ParamKnob
{
public:
    EdgeKnob (PureSheenProcessor& p, bool low)
        : ParamKnob (p, low ? kLowId : kHighId, low ? "Low" : "High"), processor (p), isLow (low)
    {
    }

    ~EdgeKnob() override { endPush(); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        ParamKnob::mouseDrag (e);

        if (! isLeftClick (e))
            return;

        const float lo = processor.getParameterValue (kLowId), hi = processor.getParameterValue (kHighId);
        const bool needs = isLow ? hi < lo * kThird : lo > hi / kThird;

        if (! needs)
            return;

        auto* other = processor.getParameter (isLow ? kHighId : kLowId);

        if (other == nullptr)
            return;

        if (pushed == nullptr)
        {
            pushed = other;
            pushed->beginChangeGesture();
        }

        const float target = isLow ? juce::jmin (300.0f, lo * kThird) : juce::jmax (20.0f, hi / kThird);
        pushed->setValueNotifyingHost (pushed->convertTo0to1 (target));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        ParamKnob::mouseUp (e);
        endPush();
    }

private:
    void endPush()
    {
        if (pushed != nullptr)
            pushed->endChangeGesture();

        pushed = nullptr;
    }

    PureSheenProcessor& processor;
    bool isLow;
    juce::RangedAudioParameter* pushed = nullptr;
};

//==============================================================================
// "Caption ........ value" row (the Maximizer's stats style).
class StatRow final : public juce::Component
{
public:
    explicit StatRow (const juce::String& c) : caption (c)
    {
        setInterceptsMouseClicks (false, false);
    }

    void setValue (const juce::String& v, juce::Colour c)
    {
        if (v != value || c != colour)
        {
            value = v;
            colour = c;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds();
        g.setFont (font (11.0f, 600));
        g.setColour (Colours::textSubtle);
        g.drawText (caption, r, juce::Justification::centredLeft, false);
        g.setFont (font (11.5f, 700));
        g.setColour (colour);
        g.drawText (value, r, juce::Justification::centredRight, false);
    }

private:
    juce::String caption, value;
    juce::Colour colour = Colours::textFaint;
};
} // namespace

//==============================================================================
struct LowContrastPanel::Impl
{
    Impl (LowContrastPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), colour (moduleColour (ps::ModuleType::LowContrast)),
          display (p, s),
          contrastCard ("Contrast", colour),
          mode (p, "lowc.a.mode", { "Punchy", "Smooth" }),
          contrast (p, "lowc.a.contrast", "Contrast"),
          gain (p, "lowc.a.gain", "Gain"),
          regionCard ("Region", colour),
          lowKnob (p, true), highKnob (p, false),
          solo ("Solo"),
          levelCard ("Level", colour),
          autoGain (p, "lowc.autoGain", "Auto Gain"),
          regionIn ("Region in", Colours::input),
          regionOut ("Region out", colour),
          change ("Change"), autoApplied ("Auto gain")
    {
        mode.setTooltip ("Punchy: a fast response that tightens kicks and bass hits.\nSmooth: a slow response that "
                         "shapes sustained bass notes; subtler on short hits.");
        contrast.setTooltip ("Contrast: positive values make the loud moments of the low end stand out from the quiet "
                             "ones (tighter, punchier); negative values even them out (rounder, denser).\nDrag to change "
                             "(Ctrl: fine), double-click to reset.");
        gain.setTooltip ("Gain: make-up gain of the action region.\nDrag to change (Ctrl: fine), double-click to reset.");
        lowKnob.setTooltip ("Low cutoff: where the action region starts (it fades in over 1/3 octave below).");
        highKnob.setTooltip ("High cutoff: where the action region ends (it fades out over 1/3 octave above). It stays "
                             "at least 1/3 octave above the low cutoff.");
        solo.setTooltip ("Solo region: hear only the input inside the action region, before processing.");
        autoGain.setTooltip ("Auto Gain: keeps the level of the region the same while Contrast changes it, so you hear "
                             "the change in shape, not in loudness.");

        note.setFont (font (11.0f, 400));
        note.setColour (juce::Label::textColourId, Colours::textSubtle);
        note.setJustificationType (juce::Justification::centred);
        note.setInterceptsMouseClicks (false, false);

        solo.onClick = [this] { display.setRegionSolo (! display.isRegionSoloed()); updateSolo(); };
        solo.setOnColour (Colours::minor);
        solo.setFilledWhenOn (true);

        owner.addAndMakeVisible (display);

        owner.addAndMakeVisible (contrastCard);
        contrastCard.addAndMakeVisible (mode);
        contrastCard.addAndMakeVisible (note);
        contrastCard.addAndMakeVisible (contrast);
        contrastCard.addAndMakeVisible (gain);

        owner.addAndMakeVisible (regionCard);
        regionCard.addAndMakeVisible (lowKnob);
        regionCard.addAndMakeVisible (highKnob);
        regionCard.addAndMakeVisible (solo);
        regionCard.setTitleReserve (64);

        owner.addAndMakeVisible (levelCard);
        levelCard.addAndMakeVisible (autoGain);
        levelCard.addAndMakeVisible (regionIn);
        levelCard.addAndMakeVisible (regionOut);
        levelCard.addAndMakeVisible (change);
        levelCard.addAndMakeVisible (autoApplied);
        levelCard.setTitleReserve (110);

        for (auto* k : { &lowKnob, &highKnob })
            k->setColour (colour);

        autoGain.setOnColour (colour);
        rebind();
        updateSolo();
    }

    ~Impl()
    {
        // A region solo does not outlive the panel that shows it.
        if (display.isRegionSoloed())
            display.setRegionSolo (false);
    }

    //==========================================================================
    bool isSplit() const { return isDualChannelMode (currentChannelMode (processor, ps::ModuleType::LowContrast)); }
    int currentSet() const { return isSplit() ? owner.getChannelSet() : 0; }
    bool isLinked() const { return isSplit() && processor.getParameterValue ("lowc.link") >= 0.5f; }
    juce::Colour setColour() const { return currentSet() == 1 ? Colours::sideChannel : colour; }

    juce::String setId (int set, const char* name) const { return juce::String ("lowc.") + (set == 0 ? "a." : "b.") + name; }
    juce::String twinId (const char* name) const { return isLinked() ? setId (1 - currentSet(), name) : juce::String(); }

    void rebind()
    {
        const int set = currentSet();
        const auto c = setColour();

        mode.setParamId (setId (set, "mode"));
        mode.setLinkedParamId (twinId ("mode"));
        mode.setOnColour (c);
        contrast.setParamId (setId (set, "contrast"));
        contrast.setLinkedParamId (twinId ("contrast"));
        contrast.setColour (c);
        gain.setParamId (setId (set, "gain"));
        gain.setLinkedParamId (twinId ("gain"));
        gain.setColour (c);

        display.setChannelSet (set);
        display.setAccentColour (c);
        contrastCard.setColour (c);
        regionOut.setColour (c);

        shownSet = set;
        shownLinked = isLinked();
        shownSplit = isSplit();
        shownMode = -1;
        updateTexts();
    }

    void updateTexts()
    {
        const int set = currentSet();
        const int m = juce::jlimit (0, 1, juce::roundToInt (processor.getParameterValue (setId (set, "mode"))));

        if (m != shownMode)
        {
            shownMode = m;
            note.setText (kModeNotes[m], juce::dontSendNotification);
        }

        juce::String sub;

        if (isSplit())
            sub = channelSetName (currentChannelMode (processor, ps::ModuleType::LowContrast), set) + (isLinked() ? " (linked)" : "");

        contrastCard.setSubtitle (sub);

        const float lo = processor.getParameterValue (kLowId);
        const float hi = juce::jmin (300.0f, juce::jmax (processor.getParameterValue (kHighId), lo * kThird));
        regionCard.setSubtitle (juce::String (juce::roundToInt (lo)) + " - " + juce::String (juce::roundToInt (hi)) + " Hz");
    }

    void updateSolo()
    {
        solo.setOn (display.isRegionSoloed());
    }

    void updateMeters()
    {
        const auto type = ps::ModuleType::LowContrast;
        const bool running = processor.isModuleInChain (type) && processor.getParameterValue ("lowc.on") >= 0.5f
                             && processor.getChain().getSampleRate() > 0.0;
        const auto* lc = dynamic_cast<const ps::LowContrast*> (&moduleForChannelSet (processor, type, currentSet()));
        const bool valid = running && lc != nullptr;
        const int s = juce::jlimit (0, ps::kNumChannelSets - 1, currentSet());

        float in = -100.0f, out = -100.0f, ag = 0.0f;

        if (valid)
        {
            const auto& m = lc->getMeters();
            in = m.regionInDb[s].load();
            out = m.regionOutDb[s].load();
            ag = m.autoGainDb[s].load();
        }

        const bool audible = valid && std::isfinite (in) && in > -70.0f;
        regionIn.setLevel (in, valid);
        regionOut.setLevel (out, valid);

        if (audible && std::isfinite (out))
        {
            const float d = out - in;
            change.setValue (fmt::db (std::abs (d) < 0.05f ? 0.0f : d), std::abs (d) < 0.05f ? Colours::text.withAlpha (0.8f)
                                                                         : (d > 0.0f ? Colours::good : Colours::minor));
        }
        else
        {
            change.setValue ("--", Colours::textFaint);
        }

        const bool agOn = processor.getParameterValue ("lowc.autoGain") >= 0.5f;

        if (valid && agOn && std::isfinite (ag))
            autoApplied.setValue (fmt::db (std::abs (ag) < 0.05f ? 0.0f : ag), Colours::text.withAlpha (0.9f));
        else
            autoApplied.setValue (agOn ? "--" : "Off", Colours::textFaint);
    }

    void update()
    {
        if (currentSet() != shownSet || isLinked() != shownLinked || isSplit() != shownSplit)
            rebind();
        else
            updateTexts();

        updateSolo();
        updateMeters();
    }

    //==========================================================================
    void layout (juce::Rectangle<int> content)
    {
        auto c = content.reduced (12, 6).withTrimmedBottom (6);
        const int cardsH = juce::jlimit (178, 224, juce::roundToInt ((float) c.getHeight() * 0.36f));
        auto cards = c.removeFromBottom (cardsH);
        c.removeFromBottom (kGap);
        display.setBounds (c);

        const int total = cards.getWidth() - 2 * kGap;
        const int contrastW = juce::roundToInt ((float) total * 0.40f);
        const int regionW = juce::roundToInt ((float) total * 0.27f);
        contrastCard.setBounds (cards.removeFromLeft (contrastW));
        cards.removeFromLeft (kGap);
        regionCard.setBounds (cards.removeFromLeft (regionW));
        cards.removeFromLeft (kGap);
        levelCard.setBounds (cards);

        auto knobPair = [] (juce::Rectangle<int> area, juce::Component& a, juce::Component& b, int gap)
        {
            const int knobW = juce::jmin (100, (area.getWidth() - gap) / 2);
            auto knobs = area.withSizeKeepingCentre (knobW * 2 + gap, juce::jmin (area.getHeight(), 100));
            a.setBounds (knobs.removeFromLeft (knobW));
            b.setBounds (knobs.removeFromRight (knobW));
        };

        {
            auto body = contrastCard.getBody();
            mode.setBounds (body.removeFromTop (26).withSizeKeepingCentre (juce::jmin (body.getWidth(), 240), 26));
            body.removeFromTop (2);
            note.setBounds (body.removeFromTop (18));
            body.removeFromTop (2);
            knobPair (body, contrast, gain, 28);
        }
        {
            solo.setBounds (regionCard.getTitleRowRight (56));
            knobPair (regionCard.getBody().withTrimmedTop (4), lowKnob, highKnob, 12);
        }
        {
            autoGain.setBounds (levelCard.getTitleRowRight (104));
            auto body = levelCard.getBody();
            const int rowH = 26, statH = 20;
            const int contentH = 2 * rowH + 6 + 12 + 2 * statH + 2;
            body = body.withSizeKeepingCentre (body.getWidth(), juce::jmin (body.getHeight(), contentH));
            regionIn.setBounds (body.removeFromTop (rowH));
            body.removeFromTop (6);
            regionOut.setBounds (body.removeFromTop (rowH));
            body.removeFromTop (12);
            change.setBounds (body.removeFromTop (statH));
            body.removeFromTop (2);
            autoApplied.setBounds (body.removeFromTop (20));
        }
    }

    LowContrastPanel& owner;
    PureSheenProcessor& processor;
    juce::Colour colour;

    LowRegionDisplay display;

    stereo::Card contrastCard;
    ParamSegments mode;
    juce::Label note;
    ParamKnob contrast, gain;

    stereo::Card regionCard;
    EdgeKnob lowKnob, highKnob;
    PillButton solo;

    stereo::Card levelCard;
    ParamToggle autoGain;
    lowend::LevelBar regionIn, regionOut;
    StatRow change, autoApplied;

    int shownSet = -1, shownMode = -1;
    bool shownLinked = false, shownSplit = false;
};

//==============================================================================
LowContrastPanel::LowContrastPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::LowContrast), impl (std::make_unique<Impl> (*this, p, s))
{
    neverTakeKeyboardFocus (*this);
}

LowContrastPanel::~LowContrastPanel() = default;

LowRegionDisplay& LowContrastPanel::getRegionDisplay() noexcept { return impl->display; }

int LowContrastPanel::getNumControls() const noexcept
{
    // mode, contrast, gain, low, high (knobs + display edges), auto gain
    return 6;
}

void LowContrastPanel::tick()
{
    ModulePanel::tick();
    impl->display.tick();
    impl->update();
}

void LowContrastPanel::frameTick() { impl->display.repaint(); }

void LowContrastPanel::refresh()
{
    ModulePanel::refresh();
    impl->rebind();
}

void LowContrastPanel::channelSetChanged() { impl->rebind(); }

void LowContrastPanel::layoutContent (juce::Rectangle<int> contentArea) { impl->layout (contentArea); }

} // namespace ps::ui
