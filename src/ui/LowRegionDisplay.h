#pragma once

#include "ui/Widgets.h"

#include <memory>

// Low-frequency display of the Low Contrast panel. Owner: ui-lowend. Implementation: LowRegionDisplay.cpp.
//
//   - frequency axis 16 Hz .. 1 kHz (log, fixed), level axis -96 .. 0 dB for the module's input (pre tap, grey) and
//     output (post tap, accent) spectra from EditorServices; a second axis +-12 dB (right edge) for the difference
//     curve
//   - the action region "lowc.low" .. "lowc.high" as a shaded band whose edges fade over 1/3 octave (the module's
//     raised-cosine taper, drawn where it acts), with a header strip ("Action region  20 Hz - 250 Hz") holding the S
//     (region solo) button, a grab handle on each edge and a value chip above each edge
//   - the difference curve: LowContrastMeters::diffDb of the selected channel set (the T/S twin's meters for the
//     Sustain part), filled toward 0 dB (teal for boost, amber for cut)
// Mouse: drag an edge = one gesture on its cutoff (the other edge is pushed to stay 1/3 octave away, inside the same
// gesture); drag inside the region = both edges (their ratio kept); Ctrl / Shift = fine; double-click an edge = type a
// value, inside the region = both defaults; right-click = Reset / Enter value (an edge) or the region menu;
// Alt + left click = region solo while held; S = region solo on / off. Never takes keyboard focus; left-click-only
// edits.
namespace ps::ui
{
class LowRegionDisplay final : public juce::Component, public juce::SettableTooltipClient
{
public:
    LowRegionDisplay (PureSheenProcessor&, EditorServices&);
    ~LowRegionDisplay() override;

    void tick();                                  // 30 Hz: the difference curve, region solo, parameters
    void setChannelSet (int set);                 // whose difference curve is shown (0 in Stereo)
    void setAccentColour (juce::Colour);          // output spectrum and region tint (set B: Colours::sideChannel)

    bool isRegionSoloed() const noexcept;
    void setRegionSolo (bool);
    juce::Rectangle<float> getPlotArea() const noexcept;
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
} // namespace ps::ui
