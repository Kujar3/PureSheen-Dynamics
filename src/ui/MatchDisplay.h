#pragma once

#include "ui/MatchEqModel.h"

#include <memory>

// The Match EQ's display (owner: ui-meq). Internal to MatchEqPanel (not a team contract).
//
//   - grid on the matched curve's dB axis (left labels, +/- 6 / 12 / 24 dB, top-right switch)
//   - the module's output spectrum (post tap, EditorServices) faint in the background
//   - the Reference (rose) and Current (sky) spectra of the shown channel set, smoothed with the current Smoothing,
//     level-normalised and tilted like the analyser (UiOptions::spectrumTiltDbPerOct), on their own scale; while a
//     capture runs, the running spectrum of that role animates (the stored one stays dashed underneath)
//   - the matched curve that is installed (MatchEqModel::getCurve = ps::computeMatchCurve), thick, in the set colour
//   - outside the Low / High cutoffs: shaded (lighter over the half-octave taper of the curve)
//   - status chip top-left: capturing (seconds, gaps), loading a reference file, designing the filter
//   - empty states: "Capture or load a reference to start" / "Now capture your track"
//
// Mouse (left button only edits; right-click only opens menus; never takes keyboard focus):
//   drag a cutoff handle    Low / High cutoff, one gesture per drag (Shift / Ctrl: fine); 1/6 octave apart at least
//   double-click a handle   back to 20 Hz / 20 kHz
//   right-click a handle    Reset / Enter value...
//   right-click elsewhere   display range, reset both cutoffs
//   Alt + drag              listen to a narrow range around the mouse (band-pass audition of the module input)
// Linked edits (Link on in M/S, L/R, T/S) go to both channel sets.
namespace ps::ui
{
class MatchDisplay final : public juce::Component, public juce::SettableTooltipClient
{
public:
    MatchDisplay (MatchEqModel&, EditorServices&);
    ~MatchDisplay() override;

    // From the panel's 30 Hz tick with the model's change mask (repaints what moved; animates captures).
    void update (int modelChanges);

    int getDisplayRangeDb() const noexcept;    // 6, 12 or 24 (+/-)
    void setDisplayRangeDb (int);

    // (tests / snapshot checks) x of a cutoff handle, and the plot area.
    float getCutoffX (bool high) const;
    juce::Rectangle<float> getPlotArea() const;

    // Colours of the two spectra (the panel's cards use them too).
    static juce::Colour referenceColour();
    static juce::Colour currentColour();

    juce::String getTooltip() override;
    void paint (juce::Graphics&) override;
    void resized() override;
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
