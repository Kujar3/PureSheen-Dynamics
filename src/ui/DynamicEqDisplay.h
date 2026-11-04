#pragma once

#include "ui/DynamicEqModel.h"

#include <functional>
#include <memory>

// The Dynamic EQ's graphic display and its GAIN column (owner: ui-deq). Internal to DynamicEqPanel (not a team
// contract). Built on the Equalizer display's look and mouse model (ui/EqDisplay.h).
//
// Drawing (shown channel set; the other set's live curve dimmed and dashed in M/S, L/R, T/S):
//   spectra       module input (grey fill) and output (set colour) from EditorServices, behind everything
//   rest curve    thin dashed line: every band at its rest gain (no trigger)
//   range         per band a soft fill between its rest and its full-trigger response (the band's travel)
//   live curve    thick line: every band at its current gain (the module's meters, smoothed for the eye)
//   nodes         at (frequency, offset + gain), numbered in the band colours, with a small arrow above (Up) or below
//                 (Down) for dynamic bands; a rail from the rest to the full-trigger gain with a dot at the live gain
//   threshold     for the selected (or hovered) dynamic band: its detector level as a translucent column over the
//                 band's -3 dB width and the threshold as a draggable bar, both on the spectrum's level scale (dBFS,
//                 right-hand ruler)
//
// Mouse (left button only edits; right-click only opens menus; never takes keyboard focus):
//   drag a node            frequency + gain (one gesture = one undo step); Shift = fine; Ctrl = one axis only
//   drag the threshold bar threshold (one gesture); double-click it = type a value
//   wheel over a node      Q (one undo step per wheel burst); Ctrl + wheel = threshold
//   double-click empty     turns on the first unused band there (shape by frequency, gain from the height)
//   double-click a node    gain back to 0 dB
//   right-click a node     shape, direction, sidechain, dynamic on / off, auto scale, reset, turn off
//   right-click empty      add a band here, display range
//   Alt + drag (empty)     listen to a narrow range around the mouse (wheel = width)
//   Alt + click a node     listen to what that band's detector hears (band solo) while the button is held
namespace ps::ui
{
// Live values of the bands (read from the module's meters at 30 Hz, smoothed for the eye), shared by the display,
// the GAIN column and the panel's threshold meter.
struct DynamicEqLive
{
    float gainDb[ps::kNumChannelSets][ps::kDynEqBands] {};       // total band gain now (offset + dynamic)
    float dynamicDb[ps::kNumChannelSets][ps::kDynEqBands] {};    // dynamic part now
    float detectorDb[ps::kNumChannelSets][ps::kDynEqBands] {};   // detector level (rises at once, falls 30 dB/s)
    bool running = false;                                         // the module is in the chain and powered
};

class DynamicEqDisplay final : public juce::Component, public juce::SettableTooltipClient
{
public:
    // `channelSet` returns the channel set the panel edits (header tab).
    DynamicEqDisplay (DynamicEqModel&, EditorServices&, std::function<int()> channelSet);
    ~DynamicEqDisplay() override;

    void parametersChanged();          // the model changed: recompute the curves that depend on it
    void tick();                       // 30 Hz: meters, live curves, design indicator, wheel bursts

    const DynamicEqLive& getLive() const noexcept;

    int getSelectedBand() const noexcept;
    void setSelectedBand (int band);
    std::function<void (int)> onSelectionChanged;

    // (tests / snapshot checks) where a node of the shown channel set is drawn
    juce::Point<float> getNodePosition (int band) const;
    // (tests) centre of the selected band's threshold bar; (-1, -1) when it has none (off or static)
    juce::Point<float> getThresholdHandlePosition() const;

    int getDisplayRangeDb() const noexcept;    // 6, 12, 18 or 30 (+/-)
    void setDisplayRangeDb (int);

    juce::String getTooltip() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

//==============================================================================
// GAIN column: one vertical bar per band (+15 .. -30 dB, 0 dB line) filled from 0 to the band's live gain in the band
// colour, its travel (rest .. full trigger) as a faint range; click a bar to select its band.
class DynamicEqGainColumn final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit DynamicEqGainColumn (DynamicEqModel&);

    void update (int set, const DynamicEqLive&, int selectedBand);
    std::function<void (int)> onSelect;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> barArea (int band) const;
    float yOf (float db) const;

    DynamicEqModel& model;
    int set = 0, selected = 0;
    float gain[ps::kDynEqBands] {};
    float rest[ps::kDynEqBands] {};
    float trig[ps::kDynEqBands] {};
    bool on[ps::kDynEqBands] {};
};
} // namespace ps::ui
