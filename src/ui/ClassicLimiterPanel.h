#pragma once

#include "ui/ModulePanel.h"

// Classic Limiter panel. Owner: mod-clim. Implementation: ClassicLimiterPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::ClassicLimiter; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: threshold and ceiling (with the link toggle), character, a 3-segment mode control, the True Peak
// toggle and a gain-reduction history (ps::ClassicLimiter::getHistoryFifo; the Maximizer's history view where it can
// be shared cleanly).
// Every control gets a tooltip in plain language. It may stop deriving from GenericModulePanel (derive from
// ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class ClassicLimiterPanel : public GenericModulePanel
{
public:
    ClassicLimiterPanel (PureSheenProcessor&, EditorServices&);
    ~ClassicLimiterPanel() override;
};
} // namespace ps::ui
