#pragma once

#include "ui/Widgets.h"

// Exciter panel parts: the saturation-model picker and the transfer-curve graph. Owner: ui-stereo (internal).
namespace ps::ui
{
// Display names and one-line descriptions of the seven saturation models (ps::ExciterMode order).
juce::String exciterModeName (int mode);
juce::String exciterModeDescription (int mode);

//==============================================================================
// Grid of the seven models (7 x 1 when wide, 4 + 3 otherwise), each cell with a small curve glyph. Bound to a
// "exc.<a|b>.b<n>.mode" parameter (and its A/B twin with Link); a left click selects (one undo step).
class ExciterModeGrid final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ExciterModeGrid (PureSheenProcessor&);

    void setParamIds (const juce::String& paramId, const juce::String& linkedId);
    void setColour (juce::Colour);
    void update();                                  // repaints when the value changed (call from the 30 Hz tick)
    int getColumns() const noexcept;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> cellBounds (int index) const;
    int cellAt (juce::Point<float>) const;
    int currentValue() const;

    PureSheenProcessor& processor;
    juce::String paramId, linkedId;
    juce::Colour colour;
    int shownValue = -1, hoverCell = -1;
};

//==============================================================================
// Static transfer curve of a model at a drive (ps::Exciter::transferCurve), input -1..+1 across, output up, with the
// straight line (no saturation) for reference and the bent part shaded.
class ExciterCurveView final : public juce::Component
{
public:
    ExciterCurveView();

    void setCurve (int mode, float drivePercent, juce::Colour, bool bandOn);

    void paint (juce::Graphics&) override;

private:
    int mode = 4;
    float drive = 0.0f;
    bool on = true;
    juce::Colour colour;
};
} // namespace ps::ui
