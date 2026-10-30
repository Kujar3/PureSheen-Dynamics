#pragma once

#include "ui/ModulePanel.h"

// Dynamic EQ panel. Owner: ui-deq. Implementation: DynamicEqPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::DynamicEq; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Learn, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: a DynamicEqDisplay (curves at rest / live, nodes with Up / Down arrows, threshold bar and
// detector level per band, GAIN column) above a band strip (on, shape, direction, freq / gain / Q, threshold meter,
// ratio, knee, attack, release, auto, offset, dynamic, sidechain) and the Analog / Digital, FIR size and lookahead
// settings.
// It may stop deriving from GenericModulePanel (derive from ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class DynamicEqPanel : public GenericModulePanel
{
public:
    DynamicEqPanel (PureSheenProcessor&, EditorServices&);
    ~DynamicEqPanel() override;
};
} // namespace ps::ui
