// STUB (skeleton): replaced by ui-eq
#pragma once

#include "ui/ModulePanel.h"

// Equalizer 1 and Equalizer 2 panel (one class, the module type picks the parameter prefix).
// Until its owner replaces it, it is the automatic GenericModulePanel (every parameter usable).
namespace ps::ui
{
class EqPanel final : public GenericModulePanel
{
public:
    EqPanel (PureSheenProcessor&, EditorServices&, ps::ModuleType eq1or2);
};
} // namespace ps::ui
