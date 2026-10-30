// STUB (skeleton): replaced by ui-dynamics
#pragma once

#include "ui/ModulePanel.h"

// Dynamics module panel.
// Until its owner replaces it, it is the automatic GenericModulePanel (every parameter usable).
namespace ps::ui
{
class DynamicsPanel final : public GenericModulePanel
{
public:
    DynamicsPanel (PureSheenProcessor&, EditorServices&);
};
} // namespace ps::ui
