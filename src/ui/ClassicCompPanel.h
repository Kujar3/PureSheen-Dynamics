#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Classic Compressor panel. Owner: mod-ccmp. Implementation: ClassicCompPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::ClassicComp; it is a
// ModulePanel (the base draws the header: name, presets, Stereo | M/S, the Mid | Side tabs with Link, Delta, Reset).
//
//   top    : "Gain Reduction" card (a needle on an arc, 0 .. -20 dB on a square-root scale, the 1 s hold tick, the
//            reduction in large type, the detector level and the auto-gain correction), "Transfer" card (the static
//            curve of ps::ClassicComp::staticCurveDb with its knee, the threshold node to drag, a ball at the live
//            input / output level) and "Detection Filter" card (the sidechain curve of detectorResponseDb with three
//            nodes: high-pass (frequency), bell and shelf (frequency and gain); the Listen pill =
//            ModuleMonitor::soloDetectionFilter)
//   bottom : "Compressor" card: Threshold, Ratio, Attack, Release, Makeup knobs; Mode (Sharp | Balanced | Smooth) and
//            Auto Gain
// Every control follows the selected channel set (M/S) and edits both sets with Link on. Every ClassicCompSetParams
// parameter has a control; "ccmp.on" lives in the chain strip, the channel mode and Link in the header. Node drags are
// one undo step; double-click types a value (a detection node resets), right-click opens a menu. Never takes keyboard focus; left-click-only edits.
// The detector solo ends when the panel closes.
namespace ps::ui
{
class ClassicCompPanel : public ModulePanel
{
public:
    ClassicCompPanel (PureSheenProcessor&, EditorServices&);
    ~ClassicCompPanel() override;

    void tick() override;
    void refresh() override;
    void channelSetChanged() override;
    void paint (juce::Graphics&) override;

    int getNumControls() const noexcept;             // parameter controls (tests / snapshot checks)

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
