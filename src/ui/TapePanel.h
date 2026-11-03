#pragma once

#include "ui/ModulePanel.h"

// Tape panel. Owner: mod-tape. Implementation: TapePanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::Tape; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: a 3-segment speed selector, a large Drive knob, Bias, Harmonics, Low / High Emphasis, a small response
// view of the current emphasis curve (ps::Tape::responseDb) and a saturation meter (ps::TapeMeters).
// Every control gets a tooltip in plain language. It may stop deriving from GenericModulePanel (derive from
// ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class TapePanel : public GenericModulePanel
{
public:
    TapePanel (PureSheenProcessor&, EditorServices&);
    ~TapePanel() override;
};
} // namespace ps::ui
