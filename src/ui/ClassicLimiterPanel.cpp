#include "ui/ClassicLimiterPanel.h"

#include "dsp/ClassicLimiter.h"
#include "ui/ClassicLimiterViews.h"
#include "ui/ImagerExciterCommon.h"

#include <cmath>
#include <vector>

// Classic Limiter panel. Owner: mod-clim. Layout and interactions: see ClassicLimiterPanel.h.
//
// Meters: ClassicLimiterMeters (both stages' current reduction; the output peak, exchanged once per tick and held for
// 1 s here). The history FIFO has exactly one consumer: this panel (stale data is dropped when it opens).
namespace ps::ui
{
namespace
{
constexpr int kGap = 8;
constexpr int kPeakHoldTicks = 30;   // 1 s at 30 Hz

const char* modeDescription (int mode)
{
    switch (mode)
    {
        case 1:  return "Smooth tube limiting. Its warm colour (mostly 2nd harmonic) grows with the gain reduction.";
        case 2:  return "Half the tube colour, blended with a clean limiter: thick, yet transparent on the peaks.";
        default: return "Fast and clean. The detector ignores the deep lows, so the bass stays tight.";
    }
}

// A few lines of dim, wrapped text (the mode description).
class TextNote final : public juce::Component
{
public:
    TextNote() { setInterceptsMouseClicks (false, false); }

    void setText (const juce::String& t)
    {
        if (t != text)
        {
            text = t;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (font (11.5f, 400));
        g.setColour (Colours::textDim);
        g.drawFittedText (text, getLocalBounds(), juce::Justification::topLeft, juce::jmax (1, getHeight() / 15), 1.0f);
    }

private:
    juce::String text;
};

// The held output peak as a large readout with its caption.
class PeakReadout final : public juce::Component, public juce::SettableTooltipClient
{
public:
    PeakReadout()
    {
        setTooltip ("Output peak: the highest sample level of the last second. It never goes above the Ceiling.");
    }

    void setValue (float db, bool isValid)
    {
        if (! juce::approximatelyEqual (db, value) || isValid != valid)
        {
            value = db;
            valid = isValid;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setFont (font (11.0f, 600));
        g.setColour (Colours::textSubtle);
        g.drawFittedText ("Peak", r.removeFromBottom (15), juce::Justification::centred, 1, 0.8f);
        const bool show = valid && value > -150.0f;
        g.setFont (font (juce::jlimit (16.0f, 24.0f, (float) r.getHeight() * 0.7f), 700));
        g.setColour (show ? Colours::text : Colours::textFaint);
        g.drawFittedText (show ? juce::String (value, 1) + " dB" : juce::String ("--"), r, juce::Justification::centred, 1, 0.7f);
    }

private:
    float value = -200.0f;
    bool valid = false;
};
} // namespace

//==============================================================================
struct ClassicLimiterPanel::Impl
{
    Impl (ClassicLimiterPanel& o, PureSheenProcessor& p)
        : owner (o), processor (p), colour (moduleColour (ps::ModuleType::ClassicLimiter)), link (p),
          history (colour),
          levelCard ("Level", colour),
          threshold (p, link, clim::LevelLink::Target::threshold, "Threshold"),
          ceiling (p, link, clim::LevelLink::Target::ceiling, "Ceiling"),
          linkToggle (p, "clim.link", "Link"),
          modeCard ("Mode", colour),
          mode (p, "clim.mode", { "Analog", "Tube", "Modern" }),
          characterCard ("Character", colour),
          character (p, "clim.character", "Character"),
          outputCard ("Output", colour),
          truePeak (p, "clim.truePeak", "True Peak"),
          pullL (4096), pullR (4096)
    {
        threshold.setColour (colour);
        ceiling.setColour (colour);
        character.setColour (colour);
        character.setCaption ({});   // the card's title says it
        linkToggle.setOnColour (colour);
        mode.setOnColour (colour);
        truePeak.setOnColour (colour);

        threshold.setTooltip ("Threshold: where the limiting starts. Lowering it adds the same amount of gain, so the "
                              "music gets louder while the peaks stay at the ceiling.\nDrag to change (Ctrl: fine), "
                              "double-click to reset.");
        ceiling.setTooltip ("Ceiling: the highest level the output may reach. Use about -0.3 dB with dither, -0.6 to "
                            "-0.8 dB before MP3 or AAC.\nDrag to change (Ctrl: fine), double-click to reset.");
        linkToggle.setTooltip ("Link: Threshold and Ceiling move together by the same amount, so the drive into the "
                               "limiter stays the same while you change the output level.");
        mode.setTooltip ("Mode. Analog: fast attack, clean, tight lows. Tube: smooth, warm limiting whose colour depends "
                         "on how hard it works. Modern: the tube colour blended with clean, transparent limiting.");
        character.setTooltip ("Character: the attack and release of the tube stage, from Fast (0) to Slow (10). The "
                              "actual times depend on the mode; a long stretch of limiting always releases slower.\n"
                              "Drag to change (Ctrl: fine), double-click to reset.");
        truePeak.setTooltip ("True Peak: also keeps the peaks between the samples under the ceiling (they can be more "
                             "than 3 dB higher after conversion to analog or to MP3 / AAC). Uses a little more CPU.");

        owner.addAndMakeVisible (history);

        owner.addAndMakeVisible (levelCard);
        levelCard.addAndMakeVisible (threshold);
        levelCard.addAndMakeVisible (linkToggle);
        levelCard.addAndMakeVisible (ceiling);

        owner.addAndMakeVisible (modeCard);
        modeCard.addAndMakeVisible (mode);
        modeCard.addAndMakeVisible (description);

        owner.addAndMakeVisible (characterCard);
        characterCard.addAndMakeVisible (character);

        owner.addAndMakeVisible (outputCard);
        outputCard.addAndMakeVisible (peak);
        outputCard.addAndMakeVisible (truePeak);

        if (auto* lim = limiter())
        {
            lim->getHistoryFifo().pull (pullL.data(), pullR.data(), (int) pullL.size());   // drop stale history
            historyReadEnd = lim->getHistoryFifo().getReadCount();
        }

        updateControls();
        updateMeters();
    }

    ps::ClassicLimiter* limiter() const
    {
        return dynamic_cast<ps::ClassicLimiter*> (&processor.getChain().getModule (ps::ModuleType::ClassicLimiter));
    }

    //==========================================================================
    void updateControls()
    {
        const float thr = processor.getParameterValue ("clim.thr");
        const float ceil = processor.getParameterValue ("clim.ceiling");
        levelCard.setSubtitle ("Gain " + fmt::db (ceil - thr, 1, true));
        description.setText (modeDescription (juce::roundToInt (processor.getParameterValue ("clim.mode"))));
    }

    // Once per tick: the history column, the stage meters and the held output peak.
    void updateMeters()
    {
        const bool running = processor.isModuleInChain (ps::ModuleType::ClassicLimiter)
                             && processor.getParameterValue ("clim.on") >= 0.5f && processor.getChain().getSampleRate() > 0.0;
        auto* lim = limiter();
        const bool valid = running && lim != nullptr;

        if (lim == nullptr)
        {
            history.setMeters (0.0f, 0.0f, false);
            peak.setValue (-200.0f, false);
            return;
        }

        // Pull even while not running, so nothing stale is shown when it starts again.
        auto& fifo = lim->getHistoryFifo();
        const int got = fifo.pull (pullL.data(), pullR.data(), (int) pullL.size());
        const auto end = fifo.getReadCount();
        const double sr = processor.getChain().getSampleRate();
        if (! valid || sr != historyRate || end - (unsigned long long) got != historyReadEnd)
        {
            history.clear();
            historyPhase = 0.0;
            historyMu = historyTotal = 0.0f;
            historyRate = sr;
        }
        historyReadEnd = end;

        if (valid && got > 0)
        {
            for (int i = 0; i < got; ++i)
            {
                historyMu = juce::jmin (historyMu, pullL[(size_t) i]);
                historyTotal = juce::jmin (historyTotal, pullR[(size_t) i]);
                // Each FIFO pair represents 64 audio samples. Timer jitter and
                // large host blocks must not compress whole seconds into one column.
                historyPhase += 64.0 * clim::HistoryView::kColumnsPerSecond;
                if (historyPhase >= sr)
                {
                    history.push (historyMu, historyTotal);
                    historyPhase -= sr;
                    historyMu = historyTotal = 0.0f;
                }
            }
        }

        auto& m = lim->getMeters();
        history.setMeters (m.muReductionDb.load(), m.brickwallReductionDb.load(), valid);
        const float p = m.outputPeakDb.exchange (-200.0f);

        if (p >= heldPeak || --heldTicks <= 0)
        {
            heldPeak = std::isfinite (p) ? p : -200.0f;
            heldTicks = kPeakHoldTicks;
        }

        peak.setValue (heldPeak, valid);
    }

    //==========================================================================
    // History on top (full width); the cards in a row below it: Level | Mode | Character | Output.
    void layout (juce::Rectangle<int> content)
    {
        auto r = content.reduced (12, 6).withTrimmedBottom (6);
        const int rowH = juce::jlimit (148, 184, juce::roundToInt ((float) r.getHeight() * 0.3f));
        auto row = r.removeFromBottom (rowH);
        r.removeFromBottom (kGap);
        history.setBounds (r);

        const int W = row.getWidth() - 3 * kGap;
        auto take = [&row] (int w)
        {
            auto a = row.removeFromLeft (w);
            row.removeFromLeft (kGap);
            return a;
        };
        const int levelW = juce::roundToInt ((float) W * 0.34f);
        const int modeW = juce::roundToInt ((float) W * 0.28f);
        const int charW = juce::roundToInt ((float) W * 0.19f);
        levelCard.setBounds (take (levelW));
        modeCard.setBounds (take (modeW));
        characterCard.setBounds (take (charW));
        outputCard.setBounds (row);

        // Level: Threshold | Link | Ceiling
        {
            auto body = levelCard.getBody();
            const int knobH = juce::jmin (body.getHeight(), 104);
            body = body.withSizeKeepingCentre (body.getWidth(), knobH);
            const int toggleW = juce::jlimit (56, 76, body.getWidth() / 4);
            const int kw = juce::jmin (110, (body.getWidth() - toggleW - 2 * kGap) / 2);
            const auto group = body.withSizeKeepingCentre (2 * kw + toggleW + 2 * kGap, knobH);
            auto g = group;
            threshold.setBounds (g.removeFromLeft (kw));
            ceiling.setBounds (g.removeFromRight (kw));
            // the toggle on the knobs' centre line
            const int knobCentreY = group.getY() + 16 + 2 + (knobH - 16 - 2 - 17 - 1) / 2;
            linkToggle.setBounds (g.withSizeKeepingCentre (toggleW, 26).withY (knobCentreY - 13));
        }

        // Mode: segments, the description below
        {
            auto body = modeCard.getBody();
            mode.setBounds (body.removeFromTop (28));
            body.removeFromTop (8);
            description.setBounds (body.withHeight (juce::jmin (body.getHeight(), 62)));
        }

        // Character: the knob, "Fast" / "Slow" on its value row either side of the value
        {
            auto body = characterCard.getBody();
            const int knobH = juce::jmin (body.getHeight(), 104);
            const int kw = juce::jmin (150, body.getWidth());
            const auto knob = body.withSizeKeepingCentre (kw, knobH);
            character.setBounds (knob);
            const auto f = font (11.5f, 600);
            const int fw = (int) textWidth (f, "Fast") + 4, sw = (int) textWidth (f, "Slow") + 4;
            const int cx = knob.getCentreX(), y = knob.getBottom() - 17;
            const int half = juce::jmin (32, body.getWidth() / 2 - fw);
            characterCard.setCaptions ({ { "Fast", { cx - half - fw, y, fw, 17 } }, { "Slow", { cx + half, y, sw, 17 } } });
        }

        // Output: the held peak, True Peak below it
        {
            auto body = outputCard.getBody();
            truePeak.setBounds (body.removeFromBottom (26));
            body.removeFromBottom (6);
            peak.setBounds (body.withSizeKeepingCentre (body.getWidth(), juce::jmin (body.getHeight(), 52)));
        }
    }

    ClassicLimiterPanel& owner;
    PureSheenProcessor& processor;
    juce::Colour colour;
    clim::LevelLink link;

    clim::HistoryView history;

    stereo::Card levelCard;
    clim::LevelKnob threshold, ceiling;
    ParamToggle linkToggle;

    stereo::Card modeCard;
    ParamSegments mode;
    TextNote description;

    stereo::Card characterCard;
    ParamKnob character;

    stereo::Card outputCard;
    PeakReadout peak;
    ParamToggle truePeak;

    std::vector<float> pullL, pullR;
    unsigned long long historyReadEnd = 0;
    double historyRate = 0.0, historyPhase = 0.0;
    float historyMu = 0.0f, historyTotal = 0.0f;
    float heldPeak = -200.0f;
    int heldTicks = 0;
};

//==============================================================================
ClassicLimiterPanel::ClassicLimiterPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::ClassicLimiter), impl (std::make_unique<Impl> (*this, p))
{
    neverTakeKeyboardFocus (*this);
}

ClassicLimiterPanel::~ClassicLimiterPanel() = default;

int ClassicLimiterPanel::getNumControls() const noexcept
{
    // threshold, ceiling, link, mode, character, true peak
    return 6;
}

void ClassicLimiterPanel::tick()
{
    ModulePanel::tick();
    impl->updateControls();
    impl->updateMeters();
}

void ClassicLimiterPanel::refresh()
{
    ModulePanel::refresh();
    impl->updateControls();
}

void ClassicLimiterPanel::paint (juce::Graphics& g) { ModulePanel::paint (g); }

void ClassicLimiterPanel::layoutContent (juce::Rectangle<int> contentArea) { impl->layout (contentArea); }

} // namespace ps::ui
