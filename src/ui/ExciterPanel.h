// STUB (skeleton): replaced by ui-stereo
#pragma once

#include "ui/ModulePanel.h"

// Exciter module panel.
// Until its owner replaces it, it is the automatic GenericModulePanel (every parameter usable).
namespace ps::ui
{
class ExciterPanel final : public GenericModulePanel
{
public:
    ExciterPanel (PureSheenProcessor&, EditorServices&);
};
} // namespace ps::ui
