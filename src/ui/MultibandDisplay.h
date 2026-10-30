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
//
// (added by ui-dynamics) Mouse priority in the plot: a crossover line (+-6 px around it) and the band count buttons
// always win; Alt + left click is the solo-while-held gesture (never passed to the hooks); otherwise onBandMouseDown
// gets every LEFT click first (right clicks go to onBandPopupMenu instead). Once onBandMouseDown returned true, the
// drag and the release of that gesture go to onBandMouseDrag / onBandMouseUp with the same band (even outside it).
// paintBandOverlay is called with the graphics clipped to the band's area, after the spectra, before the crossovers.
// Adding a band (the "+" button) also moves the new crossover when its stored value would not be ascending.
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
    void setChannelSet (int set);                    // selected T/S part for band solo; other modes keep both channels
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

    //==============================================================================
    // (added by ui-dynamics) Optional extras.
    void setAccentColour (juce::Colour);             // tint, crossovers, output spectrum (default moduleColour; panels
                                                     // pass Colours::sideChannel while channel set B is edited)
    void setLevelRange (float minDb, float maxDb);   // dB scale of the spectrum and of the grid (default -84 .. 0)
    DbAxis getLevelAxis() const noexcept;            // that scale over the plot area (for overlays in dB)
    juce::Rectangle<float> getBandHeaderArea (int band) const noexcept;   // the band's header strip
    int getSoloBand() const noexcept;                // -1 = none
    int getHoverBand() const noexcept;               // band under the mouse in the plot, -1 = none

    // Mouse moves over the plot (band -1: the mouse left the plot or is over a crossover line). For hover effects.
    std::function<void (int band, const juce::MouseEvent&)> onBandMouseMove;
    // Cursor over a band (not called over crossover lines). Unset = normal cursor.
    std::function<juce::MouseCursor (int band, juce::Point<float> position)> getBandCursor;
    // Left double-click in a band (not on a crossover line). Return true when handled.
    std::function<bool (int band, const juce::MouseEvent&)> onBandDoubleClick;
    // Right-click / ctrl-click in a band (not on a crossover line): show a menu. Return true when handled.
    std::function<bool (int band, const juce::MouseEvent&)> onBandPopupMenu;

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
