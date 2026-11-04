#pragma once

#include "ui/Widgets.h"

#include <array>
#include <functional>
#include <vector>

// Building blocks of the Maximizer panel (owner: ui-max; internal to MaximizerPanel, not a shared contract).
//
//   MaxLevelScale      the dB scale shared by the history display and the meter columns (so the Gain threshold line,
//                      the Output Level line and the meters line up across the three components)
//   MaxLevelEditor     the Gain / Output Level edits (drag, wheel, reset, typed values), Link aware: with "max.link"
//                      on, a change of one moves the other inversely, in the same gesture (one undo step)
//   MaxHistoryDisplay  the scrolling level history: the signal after Gain, before limiting (grey; the part above
//                      Output Level in amber), the output (mint), the gain reduction as a curtain from the top
//                      (amber), the Output Level line (draggable)
//   MaxMeterColumn     the Gain column (input meter + threshold handle + big Gain readout) and the Output column
//                      (output meter + Output Level handle + readout)
//   MaxGrColumn        L / R gain reduction meters hanging from 0 dB, held peak readout (click to reset)
//   MaxChoiceGrid      a choice parameter as a grid of pills (mode list, style chips, soft clip modes)
//   MaxSwitch          a small on / off switch bound to a bool parameter (card headers)
//   MaxLearnButton     Learn Input Gain with a progress ring
//
// None of them takes keyboard focus; only left clicks edit (right-click opens a menu where there is one).
namespace ps::ui
{
//==============================================================================
struct MaxLevelScale
{
    float minDb = -30.0f, maxDb = 6.0f;
    float top = 0.0f, bottom = 100.0f;   // plot rows in the component's own coordinates

    float dbToY (float db) const noexcept
    {
        const float t = (juce::jlimit (minDb, maxDb, db) - minDb) / (maxDb - minDb);
        return bottom - t * (bottom - top);
    }

    float pixelsPerDb() const noexcept { return (bottom - top) / (maxDb - minDb); }
    float height() const noexcept { return bottom - top; }
};

//==============================================================================
class MaxLevelEditor final : private juce::Timer
{
public:
    enum class Target { gain, ceiling };

    explicit MaxLevelEditor (PureSheenProcessor&);
    ~MaxLevelEditor() override;

    float getGain() const;
    float getCeiling() const;
    bool isLinked() const;

    // Drag: begin, then dragTo (value of the target the drag would give without limits), then end.
    void begin (Target);
    void dragTo (float targetValue);
    void end();
    bool isActive() const noexcept { return active; }
    Target getActiveTarget() const noexcept { return target; }

    void wheel (Target, float deltaDb);               // gesture ends ~450 ms after the last wheel event
    void setComplete (Target, float value);           // reset / typed value: one complete gesture
    void resetToDefault (Target);
    void showMenu (Target, juce::Component& target);  // right-click: reset / enter value
    void promptValue (Target, juce::Component& target);   // double-click and the menu: type a value

    std::function<void()> onChange;

private:
    void timerCallback() override;
    void beginInternal (Target, bool withLink);
    void apply (float targetValue);

    PureSheenProcessor& processor;
    juce::RangedAudioParameter* gainParam = nullptr;
    juce::RangedAudioParameter* ceilingParam = nullptr;
    Target target = Target::gain;
    bool active = false, linkedGesture = false, wheelGesture = false;
    float startGain = 0.0f, startCeiling = -1.0f, wheelValue = 0.0f;
};

//==============================================================================
// One history column (one 1/30 s step of audio). Levels in dBFS (-200 = silence).
struct MaxHistoryEntry
{
    float unlimitedDb = -200.0f; // after Gain, before limiting, on the output's scale (= output - GR)
    float outputDb = -200.0f;    // module output
    float grDb = 0.0f;           // limiter gain change, <= 0
};

class MaxHistoryDisplay final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kColumnsPerSecond = 30;
    static constexpr int kCapacity = 512;

    MaxHistoryDisplay (MaxLevelEditor&, juce::Colour accent);

    void setScale (const MaxLevelScale&);
    void setSeconds (float);
    void setLines (float gainDb, float ceilingDb);
    void push (const MaxHistoryEntry&);
    void clear();
    void setStatus (const juce::String& text);        // overlay pill (learning progress), empty = none
    void setLegendRight (const juce::String& text);   // header right text (output loudness)
    bool hasData() const noexcept { return count > 0; }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    bool isOnCeilingLine (juce::Point<float>) const;
    juce::Rectangle<float> plotArea() const;
    const MaxHistoryEntry& entryAt (int age) const noexcept;   // age 0 = newest
    void updateHover (bool);

    MaxLevelEditor& editor;
    juce::Colour accent;
    MaxLevelScale scale;
    float seconds = 5.0f;
    float gainDb = 0.0f, ceilingDb = -1.0f;
    std::array<MaxHistoryEntry, kCapacity> ring {};
    int head = 0, count = 0;
    juce::String status, legendRight;
    bool hover = false, dragging = false;
    float dragStartY = 0.0f, dragStartValue = 0.0f;
};

//==============================================================================
class MaxMeterColumn final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Kind { gain, output };

    MaxMeterColumn (Kind, MaxLevelEditor&, juce::Colour accent);

    void setScale (const MaxLevelScale&);
    void setHeaderHeight (int);
    void setLevels (float dbL, float dbR);    // per tick; ballistics inside
    void setHandle (float gainDb, float ceilingDb);
    bool needsRepaint() const noexcept { return dirty; }

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    MaxLevelEditor::Target target() const noexcept;
    float handleDb() const noexcept;

    Kind kind;
    MaxLevelEditor& editor;
    juce::Colour accent;
    MaxLevelScale scale;
    int headerHeight = 44;
    float gainDb = 0.0f, ceilingDb = -1.0f;
    float shown[2] { -200.0f, -200.0f }, hold[2] { -200.0f, -200.0f };
    int holdTicks[2] { 0, 0 };
    bool dirty = true, dragging = false;
    float dragStartY = 0.0f, dragStartValue = 0.0f;
};

//==============================================================================
class MaxGrColumn final : public juce::Component, public juce::SettableTooltipClient
{
public:
    MaxGrColumn();

    void setScale (const MaxLevelScale&);
    void setHeaderHeight (int);
    void setGainReduction (float grL, float grR, float grPeakThisTick);   // dB, <= 0
    void resetPeak();
    float getHeldPeak() const noexcept { return heldPeak; }

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    MaxLevelScale scale;
    int headerHeight = 44;
    float shown[2] { 0.0f, 0.0f };
    float heldPeak = 0.0f;
};

//==============================================================================
class MaxChoiceGrid final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Style { list, chips };

    // values: the parameter value of each label (default 0, 1, 2 ...).
    MaxChoiceGrid (PureSheenProcessor&, const juce::String& paramId, juce::StringArray labels, int columns,
                   Style, juce::Colour accent);

    void setColumns (int);
    void setItemTooltips (juce::StringArray);          // per item, shown on hover
    void refreshValue();                                // repaints when the parameter changed
    int getSelectedIndex() const;
    int getNumItems() const noexcept { return labels.size(); }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> cell (int index) const;
    int indexAt (juce::Point<float>) const;

    PureSheenProcessor& processor;
    juce::String paramId;
    juce::StringArray labels, itemTips;
    int columns = 1;
    Style style;
    juce::Colour accent;
    int shownIndex = -1, hoverIndex = -1;
};

//==============================================================================
class MaxSwitch final : public juce::Component, public juce::SettableTooltipClient
{
public:
    MaxSwitch (PureSheenProcessor&, const juce::String& paramId, juce::Colour accent);

    void refreshValue();
    bool isOn() const;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent&) override;

private:
    PureSheenProcessor& processor;
    juce::String paramId;
    juce::Colour accent;
    bool shownOn = false;
};

//==============================================================================
class MaxLearnButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit MaxLearnButton (juce::Colour accent);

    enum class State { idle, learning, done };
    void setState (State, float progress);   // progress 0..1 while learning

    std::function<void()> onClick;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Colour accent;
    State state = State::idle;
    float progress = 0.0f;
};

} // namespace ps::ui
