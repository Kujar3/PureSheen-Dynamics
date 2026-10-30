#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Definition panel. Owner: ui-spectral. Implementation: DefinitionPanel.cpp (+ SpectralDisplay, SpectralWidgets).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::Definition; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Delta, Reset).
//
//   top left  : SpectralDisplay: input (pre tap, grey) and output (post tap) spectra on the left level scale, the
//               target line (pink + Tilt, dashed) laid through the input level inside the region, the live lift curve
//               (gainDb, bold, filled to zero) on the right +-6 dB scale, the veil below 300 Hz (not processed), and
//               the action region: shaded outside, low / high handles ("defn.<set>.low/high", one gesture per drag),
//               the header pill (drag = move both) with its S button (ModuleMonitor::soloRegion).
//   top right : the Activity meter (the curve's max boost / max cut).
//   bottom    : Amount card (Amount, Tilt), Timing card (Attack, Release), Region card (Low, High).
// Per-set controls and curves follow the header's A/B tab (set B in the side-channel colour); with Link on every edit
// applies to both sets. Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class DefinitionPanel : public ModulePanel
{
public:
    DefinitionPanel (PureSheenProcessor&, EditorServices&);
    ~DefinitionPanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    void channelSetChanged() override;

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
