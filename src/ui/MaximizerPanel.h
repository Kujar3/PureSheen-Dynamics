// STUB (skeleton): replaced by ui-maximizer
#pragma once

#include "ui/ModulePanel.h"

// Maximizer module panel.
// Until its owner replaces it, it is the automatic GenericModulePanel (every parameter usable).
namespace ps::ui
{
class MaximizerPanel final : public GenericModulePanel
{
public:
    MaximizerPanel (PureSheenProcessor&, EditorServices&);
};
} // namespace ps::ui
