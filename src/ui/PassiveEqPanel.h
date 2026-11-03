#pragma once

#include "ui/ModulePanel.h"

// Passive EQ panel. Owner: mod-peq. Implementation: PassiveEqPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::PassiveEq; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: two rows of knobs (top section 8 controls, bottom section 6; stepped frequency knobs or segment
// selectors for the choices) under the combined response curve (ps::PassiveEq::getMagnitudeDb, drawn with the shared
// EqModel.h helpers fillEqBand / strokeEqCurve / snapEqZeroLine), plus Drive.
// Every control gets a tooltip in plain language. It may stop deriving from GenericModulePanel (derive from
// ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class PassiveEqPanel : public GenericModulePanel
{
public:
    PassiveEqPanel (PureSheenProcessor&, EditorServices&);
    ~PassiveEqPanel() override;
};
} // namespace ps::ui
