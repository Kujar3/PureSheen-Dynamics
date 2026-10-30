#pragma once

#include "ui/ModulePanel.h"

// Low Contrast panel. Owner: ui-lowend. Implementation: LowContrastPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::LowContrast; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Learn, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: a LowRegionDisplay (16 Hz .. 1 kHz, input / output spectra, the region with two handles and
// a solo button, the difference curve) above Punchy / Smooth, Contrast, Gain, Auto Gain and the region in / out
// readouts.
// It may stop deriving from GenericModulePanel (derive from ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class LowContrastPanel : public GenericModulePanel
{
public:
    LowContrastPanel (PureSheenProcessor&, EditorServices&);
    ~LowContrastPanel() override;
};
} // namespace ps::ui
