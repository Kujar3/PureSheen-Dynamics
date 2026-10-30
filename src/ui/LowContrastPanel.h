#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Low Contrast panel. Owner: ui-lowend. Implementation: LowContrastPanel.cpp (+ LowRegionDisplay, LowEndWidgets).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::LowContrast; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Delta, Reset).
//
//   top    : LowRegionDisplay (16 Hz .. 1 kHz, input / output spectra, the action region with its two handles, the
//            1/3-octave taper and the region solo button, the difference curve of the selected channel set)
//   bottom : "Contrast" card (Punchy | Smooth, Contrast (bipolar), Gain), "Region" card (Low / High cutoff knobs, Solo),
//            "Level" card (Auto Gain, region in / out meters, the change and the auto gain applied)
// Per-set parameters ("lowc.<a|b>.mode / contrast / gain") follow the header's A/B (Mid / Side, Transient / Sustain)
// tab; with Link on every edit applies to both sets. The region and Auto Gain are shared by both sets. Never takes
// keyboard focus; left-click-only edits.
namespace ps::ui
{
class LowRegionDisplay;

class LowContrastPanel : public ModulePanel
{
public:
    LowContrastPanel (PureSheenProcessor&, EditorServices&);
    ~LowContrastPanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    void channelSetChanged() override;

    LowRegionDisplay& getRegionDisplay() noexcept;   // test / tooling access
    int getNumControls() const noexcept;             // parameter controls (tests / snapshot checks)

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
