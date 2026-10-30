#pragma once

#include "ui/ModulePanel.h"

// Match EQ panel. Owner: ui-meq. Implementation: MatchEqPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::MatchEq; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Learn, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: a MatchDisplay (live spectrum, reference / current curves, matched curve on its own axis,
// cutoff handles, capture state) above the Reference (Capture / Stop, Load file..., Clear), Current (Capture / Stop,
// Clear), Fine Tune (amount, smoothing, limits) and Filter (phase, FIR size, latency) cards.
// It may stop deriving from GenericModulePanel (derive from ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class MatchEqPanel : public GenericModulePanel
{
public:
    MatchEqPanel (PureSheenProcessor&, EditorServices&);
    ~MatchEqPanel() override;
};
} // namespace ps::ui
