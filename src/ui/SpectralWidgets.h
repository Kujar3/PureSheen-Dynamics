#pragma once

#include "ui/Widgets.h"

#include <array>
#include <limits>
#include <vector>

// Small views shared by the spectral panels (Tone Balance, Definition, Smoother). Owner: ui-spectral.
// Implementation: SpectralWidgets.cpp. Internal, not a contract.
//   - SpectralMeterColumn: a dark well with a title and a few vertical bar meters (boost / cut range, input level,
//     gain reduction), each on its own dB scale, filled from a base value, with a value readout and a peak hold
//   - SpectralTraceView: a scrolling history (one column per 30 Hz tick) of a level, drawn as a filled envelope from
//     the bottom, and of a gain reduction, drawn from the top (the Smoother's Trace view)
// Display only: no mouse edits, never takes keyboard focus.
namespace ps::ui
{
class SpectralMeterColumn final : public juce::Component, public juce::SettableTooltipClient
{
public:
    struct Bar
    {
        juce::String label;                 // under the bar ("Boost", "GR")
        float minDb = -12.0f, maxDb = 12.0f;
        float baseDb = 0.0f;                // the bar fills from here to the value
        juce::Colour colour;
        int scaleSide = 0;                  // tick labels: -1 left of the bar, +1 right, 0 none
        float tickStep = 3.0f;
        bool signedReadout = true;          // "+2.1" (else "-24.0")
    };

    explicit SpectralMeterColumn (const juce::String& title);

    void setTitle (const juce::String&);
    void setBars (std::vector<Bar>);
    void setBarColour (int bar, juce::Colour);
    // Per tick. Fast rise, slower fall toward the value; the readout shows the held peak (1.5 s).
    void setValue (int bar, float db, bool valid);
    void resetHolds();
    void setNote (const juce::String&);     // one short line under the labels (empty = none)
    // A limit marker on a bar (the most the module can apply right now); NaN = none.
    void setLimit (int bar, float db);

    void paint (juce::Graphics&) override;

private:
    struct State
    {
        float shown = 0.0f, hold = 0.0f;
        float limit = std::numeric_limits<float>::quiet_NaN();
        int holdTicks = 0;
        bool valid = false;
    };

    juce::Rectangle<float> barArea (int bar) const;
    int notePlacement() const;              // 0 none, 1 in the title row, 2 under the labels

    juce::String title, note;
    std::vector<Bar> bars;
    std::vector<State> states;
};

//==============================================================================
class SpectralTraceView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kColumnsPerSecond = 30;
    static constexpr int kCapacity = 30 * 20;
    static constexpr int kHeaderH = 28;

    SpectralTraceView();

    void setAccentColour (juce::Colour);
    void setTitle (const juce::String&);
    void setLevelRange (float minDb, float maxDb);   // left scale (level, from the bottom)
    void setGrRange (float minDb);                   // right scale (0 at the top .. minDb at the bottom)
    void setSeconds (float);
    void push (float levelDb, float grDb, bool valid);
    void clear();
    void setStatusText (const juce::String&);
    bool hasData() const noexcept { return count > 0; }

    void paint (juce::Graphics&) override;

private:
    struct Column
    {
        float levelDb = -200.0f, grDb = 0.0f;
        bool valid = false;
    };

    const Column& columnAt (int age) const noexcept;   // age 0 = newest
    juce::Rectangle<float> plotArea() const;
    void paintPlot (juce::Graphics&) const;

    juce::Colour accent;
    juce::String title, status;
    float levelMin = -60.0f, levelMax = 0.0f, grMin = -18.0f, seconds = 6.0f;
    std::array<Column, kCapacity> ring {};
    int head = 0, count = 0;
};
} // namespace ps::ui
