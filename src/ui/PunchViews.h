#pragma once

#include "ui/ImagerExciterCommon.h"
#include "ui/Widgets.h"

#include <array>
#include <functional>
#include <vector>

// Parts of the Punch panel (owner: ui-punch; internal to PunchPanel, not a contract):
//   - PunchAmountGesture: one drag over a group of band amounts as ONE host gesture / undo step. Every parameter of
//     the group moves by the same amount from its own start value (Link Bands); each may carry an A/B twin that
//     follows it to the same value (channel Link).
//   - PunchGainHistory: the per band and channel set ring of applied-gain extremes behind the scrolling gain traces
//     (one entry per 30 Hz tick, from PunchMeters gainMinDb / gainMaxDb).
//   - PunchBandKnob: the band card's Amount knob (a ParamKnob); with Link Bands on, its drags and wheel moves go
//     through a PunchAmountGesture over every visible band instead.
//   - PunchBandCard: one band's card (badge, name, On toggle, range, Amount knob, live gain meter and readout,
//     micro-dynamics in / out meters, auto-gain make-up). A click on the card's surface selects the band.
// Nothing here takes keyboard focus; only left clicks edit.
namespace ps::ui
{
//==============================================================================
class PunchAmountGesture
{
public:
    explicit PunchAmountGesture (PureSheenProcessor&);
    ~PunchAmountGesture();

    // Arms a drag. ids[0] is the parameter under the mouse; twins (same size as ids, entries may be empty) follow
    // their parameter to the same value. Nothing changes until the first dragBy with a real change.
    void start (const std::vector<juce::String>& ids, const std::vector<juce::String>& twins);
    void dragBy (float deltaPlain);     // every parameter = clamp (its start value + delta)
    void end();
    bool isArmed() const noexcept { return ! items.empty(); }
    bool isActive() const noexcept { return active; }
    float startValueOfMain() const noexcept { return items.empty() ? 0.0f : items.front().start; }

private:
    struct Item
    {
        juce::RangedAudioParameter* param = nullptr;
        juce::RangedAudioParameter* twin = nullptr;
        float start = 0.0f;
    };

    PureSheenProcessor& processor;
    std::vector<Item> items;
    bool active = false;
};

//==============================================================================
struct PunchGainHistory
{
    static constexpr int kCapacity = 128;     // > the 90 points (3 s) the traces show
    struct Point
    {
        float lo = 0.0f, hi = 0.0f;
        bool valid = false;
    };

    // One tick: values[set][band] for every set and band at once.
    void push (const Point (&values)[ps::kNumChannelSets][ps::kMaxBands]) noexcept;
    // age 0 = newest; an invalid point past the recorded length.
    Point get (int set, int band, int age) const noexcept;
    void clear() noexcept;

private:
    std::array<std::array<std::array<Point, kCapacity>, ps::kMaxBands>, ps::kNumChannelSets> points {};
    int head = 0, count = 0;
};

//==============================================================================
class PunchBandKnob final : public ParamKnob
{
public:
    PunchBandKnob (PureSheenProcessor&, const juce::String& paramId, const juce::String& caption);
    ~PunchBandKnob() override;

    // Link Bands: fills ids (this knob's parameter first) and twins; leave ids empty for a plain knob.
    std::function<void (std::vector<juce::String>& ids, std::vector<juce::String>& twins)> getGroup;

    // Ends a wheel gesture ~450 ms after the last wheel step (called from the panel's 30 Hz tick).
    void tickWheel();
    void cancelGroupGesture();      // a channel-set / Link change invalidates the stored group targets

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    bool armGroup();

    PureSheenProcessor& processor;
    PunchAmountGesture gesture;
    bool groupDrag = false, groupWheel = false;
    float dragDelta = 0.0f, lastY = 0.0f, wheelDelta = 0.0f;
    double lastWheelMs = 0.0;
};

//==============================================================================
class PunchBandCard final : public stereo::Card
{
public:
    PunchBandCard (PureSheenProcessor&, int band, juce::Colour colour);
    ~PunchBandCard() override;

    struct Live
    {
        bool valid = false;          // the module runs (in the chain, on, prepared)
        bool bandOn = true;
        float gainDb = 0.0f;         // smoothed readout (largest |gain| of the last few ticks)
        float lo = 0.0f, hi = 0.0f;  // the last tick's gain range (dB)
        float microInDb = 0.0f, microOutDb = 0.0f;
        float makeupDb = 0.0f;
        bool autoGain = false;
    };

    // Rebinds the knob and toggle only when the set / link changed (a rebind ends a gesture in progress).
    void bind (int channelSet, int linkedSetOrMinus1, juce::Colour setColour);
    void setRange (const juce::String& range);
    void setSelected (bool);
    void setLive (const Live&);      // repaints only when something visible changed
    void setCompact (bool);          // small editor sizes: compact knob, tighter readouts

    PunchBandKnob& getKnob() noexcept { return amount; }
    ParamToggle& getOnToggle() noexcept { return onToggle; }

    std::function<void (int band)> onSelect;

    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::String bandId (int set, const char* name) const;

    PureSheenProcessor& processor;
    const int band;
    juce::Colour colour;
    PunchBandKnob amount;
    ParamToggle onToggle;
    Live live, shown;
    juce::String rangeText;
    bool selected = false, compact = false, wide = false;
    int boundSet = -1, boundLinked = -2;
    juce::Rectangle<int> rangeArea, gainArea, microArea;
};

// Text helpers shared by the panel and the cards.
juce::String punchAmountText (float percent);           // "+25 %", "0 %", "-40 %"
juce::String punchDbText (float db);                    // "+2.4 dB", "0.0 dB"
juce::String punchHzText (float hz);                    // "120 Hz", "1.2 kHz", "20 kHz"

} // namespace ps::ui
