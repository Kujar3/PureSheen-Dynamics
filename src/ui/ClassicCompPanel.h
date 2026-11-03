#pragma once

#include "ui/ModulePanel.h"

// Classic Compressor panel. Owner: mod-ccmp. Implementation: ClassicCompPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::ClassicComp; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: a large gain-reduction meter (a needle-style arc in our style), knobs for threshold / ratio / attack /
// release / makeup, the mode as a 3-segment control, the Auto Gain toggle and a sidechain EQ strip (three nodes on a
// mini curve from ps::ClassicComp::detectorResponseDb, or six knobs) with a listen button (soloDetectionFilter).
// Every control gets a tooltip in plain language. It may stop deriving from GenericModulePanel (derive from
// ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class ClassicCompPanel : public GenericModulePanel
{
public:
    ClassicCompPanel (PureSheenProcessor&, EditorServices&);
    ~ClassicCompPanel() override;
};
} // namespace ps::ui
