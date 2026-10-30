#pragma once

#include "ui/ModulePanel.h"

// Bass Shaper panel. Owner: ui-lowend. Implementation: BassShaperPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::BassShaper; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Learn, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: three columns: Balance and Punch sweet-spot meters with their knobs, the BassScope
// oscilloscope with both thresholds and the Cutoff fader, and the Sustain Power and Peak Control cards with Auto Gain.
// It may stop deriving from GenericModulePanel (derive from ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class BassShaperPanel : public GenericModulePanel
{
public:
    BassShaperPanel (PureSheenProcessor&, EditorServices&);
    ~BassShaperPanel() override;
};
} // namespace ps::ui
