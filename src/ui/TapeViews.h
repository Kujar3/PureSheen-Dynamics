#pragma once

#include "ui/Widgets.h"

#include "dsp/Params.h"

#include <vector>

// Views of the Tape panel. Owner: mod-tape. Implementation: TapeViews.cpp. Internal (not a contract).
namespace ps::ui::tape
{
// Reads the Tape parameters from the processor (plain values, message thread).
ps::TapeParams readParams (const PureSheenProcessor&);

//==============================================================================
// The small-signal response of the whole linear path (ps::Tape::responseDb: head bump and roll-off, the high end with
// its loss / emphasis and the bias tilt) on the shared frequency grid, drawn with the EQ helpers (soft area + light
// line), with markers at the speed's head-bump and high-end frequencies. Read only (no mouse edits).
class ResponseView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ResponseView (PureSheenProcessor&, EditorServices&);

    void setColour (juce::Colour);
    void update();                 // 30 Hz: repaints when a parameter, the size or the frequency scale changed
    void paint (juce::Graphics&) override;
    void resized() override;

    // (tests / snapshot checks) the dB value drawn at hz for the current parameters
    float shownDbAt (float hz) const;

private:
    void rebuild();

    PureSheenProcessor& processor;
    EditorServices& services;
    juce::Colour colour;
    ps::TapeParams shown;
    bool valid = false;
    FreqAxis fx;
    DbAxis dy;
    std::vector<float> xs, db;
    juce::Path curve;
};

//==============================================================================
// The large Drive knob: a ParamKnob (same mouse handling, menu, undo) drawn bigger, with a dB scale around the arc.
class DriveKnob final : public ParamKnob
{
public:
    DriveKnob (PureSheenProcessor&, const juce::String& paramId, const juce::String& caption);

    void setColour (juce::Colour);
    void paint (juce::Graphics&) override;

private:
    PureSheenProcessor& processor;
    juce::String id, captionText;
    juce::Colour colour;
    float minValue = -30.0f, maxValue = 20.0f;
};

//==============================================================================
// Two horizontal meters: the level into the tape (peak dBFS after the record EQ and the drive, -36 .. +18) and the
// saturation (dB of compression on the peaks, 0 .. 12), each with its value on the right.
class SaturationMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    SaturationMeter();

    void setColour (juce::Colour);
    void setValues (float driveDb, float saturationDb, bool valid);
    void paint (juce::Graphics&) override;

    static constexpr int kIdealHeight = 74;

private:
    juce::Colour colour;
    float drive = -100.0f, saturation = 0.0f;
    bool active = false;
};
} // namespace ps::ui::tape
