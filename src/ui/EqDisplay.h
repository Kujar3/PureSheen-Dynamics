#pragma once

#include "ui/EqModel.h"

#include <functional>
#include <memory>

// The Equalizer's graphic display (owner: ui-eq): spectra, grid, the exact response curves (ps::eqBandResponse with
// the current mode and amount), numbered nodes and all mouse editing. Internal to EqPanel (not a team contract).
//
// Mouse (left button only edits; right-click only opens menus; never takes keyboard focus):
//   drag a node            frequency + gain (pass filters: frequency; Resonant pass: vertical = resonance)
//                          one gesture per drag = one undo step; Shift = fine; Ctrl / Cmd = one axis only
//   wheel over a node      Q (pass filters: slope); one undo step per wheel burst
//   double-click empty     adds a band there (first unused band, default shape for that frequency)
//   double-click a node    gain back to 0 dB (Resonant pass: resonance back to default)
//   right-click a node     shape, slope, phase (Digital), turn off, reset band
//   right-click empty      add a band here, display range
//   Alt + drag             listen to a narrow range around the mouse (on a node: around that band); wheel = width
namespace ps::ui
{
class EqDisplay final : public juce::Component,
                        public juce::SettableTooltipClient,
                        private juce::Timer
{
public:
    // `channelSet` returns the channel set the panel edits (header A/B tab).
    EqDisplay (EqModel&, EditorServices&, std::function<int()> channelSet);
    ~EqDisplay() override;

    void parametersChanged();          // the model changed: recompute the curves that depend on it
    void tick();                       // 30 Hz: spectra, design indicator, repaint

    int getSelectedBand() const noexcept;
    void setSelectedBand (int band);
    std::function<void (int)> onSelectionChanged;

    // (tests) where a node of the shown channel set is drawn, and the display point of a frequency / gain.
    juce::Point<float> getNodePosition (int band) const;
    juce::Point<float> pointFor (float hz, float db) const;

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
    void timerCallback() override;

    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
