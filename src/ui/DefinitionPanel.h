#pragma once

#include "ui/ModulePanel.h"

// Definition panel. Owner: ui-spectral. Implementation: DefinitionPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::Definition; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Learn, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: a SpectralDisplay (input / output spectra, pink + tilt target line, correction curve, region
// handles, the veil below 300 Hz) with boost / cut activity bars, then Amount, Tilt, Attack, Release and the region
// values.
// It may stop deriving from GenericModulePanel (derive from ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class DefinitionPanel : public GenericModulePanel
{
public:
    DefinitionPanel (PureSheenProcessor&, EditorServices&);
    ~DefinitionPanel() override;
};
} // namespace ps::ui
