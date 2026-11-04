#pragma once

#include "ui/Widgets.h"

#include <array>

// Building blocks of the Classic Limiter panel (owner: mod-clim; internal to ClassicLimiterPanel, not a contract).
//
//   LevelLink     Threshold / Ceiling edits with "clim.link": while it is on, a change of one moves the other by the
//                 same amount in the same gesture (one undo step); the move stops where either reaches its limit
//   LevelKnob     a ParamKnob for Threshold or Ceiling: its own drag / wheel / double-click / menu while linked (through
//                 LevelLink), the plain ParamKnob behaviour otherwise
//   HistoryView   the scrolling gain-reduction history (0 dB at the top): the slow stage's reduction in the module
//                 colour, the fast stage's share below it in the gain-reduction amber, a time ruler, and two meters on
//                 the right (Slow, Fast) on the same scale
//
// None of them takes keyboard focus; only left clicks edit (right-click opens a menu).
namespace ps::ui::clim
{
//==============================================================================
class LevelLink final : private juce::Timer
{
public:
    enum class Target { threshold, ceiling };

    explicit LevelLink (PureSheenProcessor&);
    ~LevelLink() override;

    bool isLinked() const;
    float get (Target) const;

    void begin (Target);
    void dragTo (float value);                        // the target's value the gesture would give without limits
    void end();
    bool isActive() const noexcept { return active; }

    void wheel (Target, float deltaDb);               // the gesture ends ~450 ms after the last wheel event
    void setComplete (Target, float value);           // reset / typed value: one complete gesture

private:
    void timerCallback() override;
    void apply (float value);
    juce::RangedAudioParameter* param (Target) const noexcept;

    PureSheenProcessor& processor;
    juce::RangedAudioParameter* thrParam = nullptr;
    juce::RangedAudioParameter* ceilParam = nullptr;
    Target target = Target::threshold;
    bool active = false, linkedGesture = false, wheelGesture = false;
    float startThr = 0.0f, startCeil = 0.0f, wheelValue = 0.0f;
};

//==============================================================================
class LevelKnob final : public ParamKnob
{
public:
    LevelKnob (PureSheenProcessor&, LevelLink&, LevelLink::Target, const juce::String& caption);

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void showLinkedMenu();
    void promptLinkedValue();

    PureSheenProcessor& processor;
    LevelLink& link;
    LevelLink::Target target;
    bool ownDrag = false;
    float dragValue = 0.0f;
    juce::Point<float> lastPos;
};

//==============================================================================
class HistoryView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kColumnsPerSecond = 30;
    static constexpr int kCapacity = 512;

    explicit HistoryView (juce::Colour accent);

    // One column (one 1/30 s step): reductions in dB (<= 0); total <= mu.
    void push (float muDb, float totalDb);
    void clear();
    // Current meter values (dB, <= 0; ballistics inside); valid = false shows "--".
    void setMeters (float muDb, float wallDb, bool valid);
    void setSeconds (float);

    void paint (juce::Graphics&) override;

private:
    struct Column { float mu = 0.0f, total = 0.0f; };
    juce::Rectangle<float> plotArea() const;
    juce::Rectangle<float> meterArea() const;
    float dbToY (float db, juce::Rectangle<float> plot) const noexcept;
    const Column& columnAt (int age) const noexcept;   // age 0 = newest

    juce::Colour accent;
    std::array<Column, kCapacity> ring {};
    int head = 0, count = 0;
    float seconds = 5.0f, rangeDb = 12.0f;
    float shownMu = 0.0f, shownWall = 0.0f;
    bool metersValid = false;
};
} // namespace ps::ui::clim
