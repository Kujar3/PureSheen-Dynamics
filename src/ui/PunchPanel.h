#pragma once

#include "ui/ModulePanel.h"

// Punch panel. Owner: ui-dynamics. Implementation: PunchPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::Punch; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Learn, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: a MultibandDisplay with per-band gain traces and amount bars, a Settings card (Amount,
// Envelope, Sync and note, Stereo Link, Auto Gain, Link Bands, crossover type) and one card per band (amount, on, live
// gain, micro-dynamics in / out).
// It may stop deriving from GenericModulePanel (derive from ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class PunchPanel : public GenericModulePanel
{
public:
    PunchPanel (PureSheenProcessor&, EditorServices&);
    ~PunchPanel() override;
};
} // namespace ps::ui
