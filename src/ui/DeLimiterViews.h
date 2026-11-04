#pragma once

#include "ui/Widgets.h"

#include <array>
#include <functional>

// Building blocks of the De-limiter panel (owner: mod-dlim; internal to DeLimiterPanel, not a contract).
//
//   LearnButton  "Learn Threshold": idle, waiting for the first sound, or listening (filled with the progress)
//   Display      the input (grey, after Input Gain) and the lift (module colour, up from 0 dB) over the last seconds on
//                one +9 .. -12 dB scale, the Threshold line, the held lift's readout. The line is a control: drag it
//                (Ctrl: fine), double-click it or its value to type a value, right-click for reset / enter value
//
// None of them takes keyboard focus; only left clicks edit.
namespace ps::ui::dlim
{
//==============================================================================
class LearnButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit LearnButton (juce::Colour accent);

    enum class State { idle, waiting, listening };
    void setState (State, float progress);   // progress 0..1 while listening

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

//==============================================================================
class Display final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kColumnsPerSecond = 30;
    static constexpr int kCapacity = 512;

    Display (PureSheenProcessor&, juce::Colour accent);
    ~Display() override;

    void push (float inputDb, float liftDb);            // one column (one 1/30 s step)
    void setReadout (float heldLiftDb, bool valid);
    void update();                                      // threshold changes, 30 Hz

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    struct Column { float input = -120.0f, lift = 0.0f; };

    juce::Rectangle<float> plotArea() const;
    float dbToY (float db) const noexcept;
    float yToDb (float y) const noexcept;
    bool hitsLine (juce::Point<float>) const noexcept;
    const Column& columnAt (int age) const noexcept;    // age 0 = newest
    void endDrag();
    void showMenu();
    void promptValue();

    PureSheenProcessor& processor;
    juce::Colour accent;
    juce::RangedAudioParameter* threshold = nullptr;
    std::array<Column, kCapacity> ring {};
    int head = 0, count = 0;
    float shownThreshold = 0.0f, readout = 0.0f, dragValue = 0.0f, dragStartY = 0.0f;
    bool readoutValid = false, dragging = false, hover = false;
};
} // namespace ps::ui::dlim
