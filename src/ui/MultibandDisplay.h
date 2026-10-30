#pragma once

#include "ui/Widgets.h"

#include <functional>
#include <memory>

// Band display shared by the multiband panels (Dynamics, Imager, Exciter). Owner: ui-dynamics.
// Implementation: MultibandDisplay.cpp. ui-stereo uses it for the Imager and Exciter panels.
//
//   - background: the module's input (pre tap, grey) and output (post tap, module colour) spectra from
//     EditorServices, on a FreqAxis (makeFreqAxis) with the frequency grid
//   - crossover handles: vertical lines with a grab handle; drag = one gesture on "<prefix>.xover<n>" (kept ascending,
//     at least 1/6 octave apart), double-click = default, right-click = Reset / Enter value; hover shows the frequency
//   - band count: "-" / "+" buttons bound to "<prefix>.bands"; bands above the count are not shown
//   - per band: a header strip with the band's name and range ("Band 2  120 Hz - 1.0 kHz"), an S (solo) button
//     (processor.setMonitor soloBand, exclusive, Alt-click anywhere in a band = solo while held) and selection
//     (click in a band selects it; the selected band is tinted)
//   - overlay hook: the owning panel paints band-specific content (threshold lines, width bars, drive meters...) into
//     each band's area through paintBandOverlay, and may handle drags itself through the band mouse hooks
// Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class MultibandDisplay : public juce::Component
{
public:
    MultibandDisplay (PureSheenProcessor&, EditorServices&, ps::ModuleType);
    ~MultibandDisplay() override;

    void tick();                                     // 30 Hz: spectra, solo state, parameter changes
    void refresh();                                  // channel set / parameters changed

    int getNumBands() const noexcept;                // current "<prefix>.bands"
    int getSelectedBand() const noexcept;
    void setSelectedBand (int band);
    juce::Rectangle<float> getBandArea (int band) const noexcept;   // x range of the band over the plot area
    juce::Rectangle<float> getPlotArea() const noexcept;
    float xForFrequency (float hz) const noexcept;
    float frequencyForX (float x) const noexcept;

    void setShowSpectrum (bool shouldShow);          // default true

    // Hooks for the owning panel.
    std::function<void (juce::Graphics&, int band, juce::Rectangle<float> bandArea)> paintBandOverlay;
    std::function<void (int band)> onBandSelected;
    // Return true when the panel handles the gesture (then the display does not select / drag crossovers).
    std::function<bool (int band, const juce::MouseEvent&)> onBandMouseDown;
    std::function<void (int band, const juce::MouseEvent&)> onBandMouseDrag;
    std::function<void (int band, const juce::MouseEvent&)> onBandMouseUp;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
