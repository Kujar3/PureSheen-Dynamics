#pragma once

#include "ui/Widgets.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

// Spectrum view shared by the Tone Balance, Definition and Smoother panels. Owner: ui-spectral.
// Implementation: SpectralDisplay.cpp. (Internal to the spectral panels, not a contract; other panels may reuse it.)
//
// One dark rounded surface: a header strip (kHeaderH) over the plot.
//   - plot: the frequency grid on the user's axis (makeFreqAxis: Log / Mel / Extended Log) with the dB lines and labels
//     of the LEFT scale (setLeftScale); the module's input (pre tap, grey fill) and output (post tap, accent line)
//     spectra from EditorServices (the editor stays the only FIFO consumer), drawn on their own level range
//     (setSpectrumRange; panels may move it every tick to sit the spectra on a target curve)
//   - optional RIGHT scale (setRightScale): a linear map of the left scale (right = (left - leftAtZero) x scale) whose
//     labels are drawn at the left scale's grid lines, for a correction curve; getRightAxis() maps it
//   - paintOverlay: the panel draws its curves / bars there (helpers: makeCurvePath, fillBetween), after the spectra,
//     zone tint, veil and region shading, before the region handles, labels and legend
//   - region (setRegion): two parameters (low / high cutoff, plus the A/B Link twins). Outside the region the plot is
//     shaded; each cutoff is a vertical line with a grab handle: drag = one gesture on that parameter (they keep
//     high >= low x minRatio), double-click = default, right-click = Reset / Enter value. The header shows a pill over
//     the region's x range ("2.00 kHz - 12.0 kHz"): drag it to move both cutoffs together (ratio kept, one gesture).
//     With region solo, the pill carries an S button (ModuleMonitor::soloRegion, toggles; Alt + click on the pill =
//     solo while held); destroying the display (leaving the panel, closing the editor) ends the region solo
//   - zones (setZones): fixed frequency zones with one 0..100 % strength parameter each (Tone Balance Low / Mid / High):
//     dashed dividers in the plot, the plot tinted darker where a zone is weaker, and one header segment per zone with
//     its name, value and a strength bar; drag on a segment (up / right = more, Ctrl or Shift = fine) = one gesture,
//     double-click = default, right-click = Reset / Enter value
//   - veil (setVeilBelow): the plot below a frequency is dimmed and captioned ("Not processed")
//   - legend (setLegend) at the bottom right, a centred status message (setStatusText)
// Never takes keyboard focus; only left clicks edit; right-click never acts as a left click.
namespace ps::ui
{
class SpectralDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    SpectralDisplay (PureSheenProcessor&, EditorServices&, ps::ModuleType);
    ~SpectralDisplay() override;

    static constexpr int kHeaderH = 28;

    // 30 Hz from the panel: region solo state and parameter changes (repaints only when something changed).
    void tick();
    void setChannelSet (int set);     // selected T/S part for region monitoring

    //==============================================================================
    // Look
    void setAccentColour (juce::Colour);
    juce::Colour getAccentColour() const noexcept;
    void setLeftScale (float minDb, float maxDb, const juce::String& caption = {});
    void setRightScale (float leftDbAtZero, float rightPerLeftDb, const juce::String& caption);
    void clearRightScale();
    void setSpectra (bool showInput, bool showOutput);
    void setSpectrumRange (float minDb, float maxDb);   // the level range the spectra are drawn on (default = left)
    void setSpectrumStyle (float inputOpacity, float outputOpacity, bool outputFill);   // default 0.55, 0.9, no fill
    void setVeilBelow (float hz, const juce::String& caption);   // hz <= 0: none
    void setLegend (std::vector<std::pair<juce::String, juce::Colour>>);
    void setStatusText (const juce::String&);           // centred message over the plot, empty = none
    void setHeaderText (const juce::String&);           // left of the header when there is no region / zone
    void setBaseTooltip (const juce::String&);          // tooltip away from the handles / pill / zones

    //==============================================================================
    // Region (empty ids = none). linked ids: the A/B Link twins (empty = none).
    void setRegion (const juce::String& lowId, const juce::String& highId, const juce::String& linkedLowId,
                    const juce::String& linkedHighId, float minRatio, bool withSolo);
    void clearRegion();
    bool hasRegion() const noexcept;
    float getRegionLowHz() const;
    float getRegionHighHz() const;
    bool isRegionSoloed() const;

    struct Zone
    {
        float lowHz = 20.0f, highHz = 20000.0f;
        juce::String name, paramId, linkedId;
        juce::String tooltip;
    };
    void setZones (std::vector<Zone>);

    //==============================================================================
    // Geometry for overlays
    juce::Rectangle<float> getPlotArea() const noexcept;
    FreqAxis getFreqAxis() const;
    DbAxis getLeftAxis() const;
    DbAxis getRightAxis() const;                        // == left when no right scale is set
    float xForHz (float hz) const;
    // Hit areas (tooling / tests): a cutoff's grab handle, the region pill, its S button, a zone's header segment.
    juce::Rectangle<float> getHandleArea (bool high) const;
    juce::Rectangle<float> getRegionPillArea() const;
    juce::Rectangle<float> getSoloButtonArea() const;
    juce::Rectangle<float> getZoneHeaderArea (int zone) const;

    // Smooth path through (hz, dB) points on `axis` (monotone cubic in x, clipped to the plot's frequency range).
    juce::Path makeCurvePath (const float* hz, const float* db, int count, const DbAxis& axis) const;
    // Closed path between two curves sampled at the same frequencies (corridors, curve-to-zero fills).
    juce::Path makeBandPath (const float* hz, const float* lowDb, const float* highDb, int count, const DbAxis& axis) const;

    std::function<void (juce::Graphics&)> paintOverlay;
    std::function<void()> onRegionChanged;              // a region drag / reset changed the cutoffs (UI feedback)

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    juce::MouseCursor getMouseCursor() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
