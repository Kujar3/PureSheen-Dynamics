#pragma once

#include "ui/Widgets.h"

#include <functional>
#include <memory>

// Small building blocks of the Low Contrast and Bass Shaper panels. Owner: ui-lowend (internal, not a contract).
// Implementation: LowEndWidgets.cpp.
//
//   SectionKnob / SectionFader  a ParamKnob / ParamFader whose first drag also switches a section's power parameter
//                               on (in the same undo step), so editing a switched-off section is heard at once
//   SweetSpotMeter              a horizontal meter with a target zone, a filled ball (input) and a hollow ball (output)
//   CutoffStrip                 a low-frequency axis (20 Hz .. 1 kHz) with the module's input spectrum, the band below
//                               a cutoff parameter shaded, and a draggable cutoff handle (one gesture per drag)
//   LevelBar                    a slim dB bar with a caption and a value (region in / out readouts)
// None of them takes keyboard focus; only left clicks edit; right-click opens a Reset / Enter value menu.
namespace ps::ui::lowend
{
//==============================================================================
class SectionKnob : public ParamKnob
{
public:
    SectionKnob (PureSheenProcessor&, const juce::String& paramId, const juce::String& caption, const juce::String& powerId);
    ~SectionKnob() override;

    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    PureSheenProcessor& processor;
    juce::String powerId;
    juce::RangedAudioParameter* powerGesture = nullptr;
};

class SectionFader : public ParamFader
{
public:
    SectionFader (PureSheenProcessor&, const juce::String& paramId, const juce::String& caption, const juce::String& powerId);
    ~SectionFader() override;

    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    PureSheenProcessor& processor;
    juce::String powerId;
    juce::RangedAudioParameter* powerGesture = nullptr;
};

// Switches `powerId` on inside an open gesture (returns the parameter whose gesture is open, or nullptr when the
// section was already on). The caller ends it with endChangeGesture() when its own gesture ends.
juce::RangedAudioParameter* beginSectionPower (PureSheenProcessor&, const juce::String& powerId);

//==============================================================================
class SweetSpotMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    SweetSpotMeter();

    void setRange (float minValue, float maxValue);
    void setZone (float low, float high);
    void setLabels (const juce::String& left, const juce::String& right);
    void setColour (juce::Colour);
    void setFormatter (std::function<juce::String (float)>);
    // Per tick: the input value (filled ball) and the output value (hollow ball). valid = false greys the meter.
    void setValues (float input, float output, bool valid);

    static constexpr int kIdealHeight = 40;

    void paint (juce::Graphics&) override;

private:
    float minValue = -12.0f, maxValue = 12.0f, zoneLow = -3.0f, zoneHigh = 3.0f;
    float shownIn = 0.0f, shownOut = 0.0f;
    bool valid = false, primed = false;
    juce::String leftLabel, rightLabel;
    juce::Colour colour = Colours::accent;
    std::function<juce::String (float)> formatter;
};

//==============================================================================
class CutoffStrip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    CutoffStrip (PureSheenProcessor&, EditorServices&, ps::ModuleType, const juce::String& paramId);
    ~CutoffStrip() override;

    void setColour (juce::Colour);
    void tick();                                  // repaints when the cutoff changed (automation, undo)
    float xForFrequency (float hz) const noexcept;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

//==============================================================================
class LevelBar final : public juce::Component
{
public:
    LevelBar (const juce::String& caption, juce::Colour);

    void setColour (juce::Colour);
    void setLevel (float db, bool valid);         // per tick; dB in -60..0
    void paint (juce::Graphics&) override;

private:
    juce::String caption;
    juce::Colour colour;
    float shown = -60.0f;
    bool valid = false;
};

// Helpers
juce::String hzText (float hz);                   // "62 Hz", "120 Hz", "1.2 kHz"
void drawValueChip (juce::Graphics&, juce::Rectangle<float> chip, const juce::String& text, juce::Colour, bool active);
void drawGrabHandle (juce::Graphics&, juce::Rectangle<float> handle, juce::Colour, bool active, bool horizontalGrip = false);

} // namespace ps::ui::lowend
