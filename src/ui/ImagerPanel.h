// STUB (skeleton): replaced by ui-stereo
#pragma once

#include "ui/ModulePanel.h"

// Imager module panel.
// Until its owner replaces it, it is the automatic GenericModulePanel (every parameter usable).
namespace ps::ui
{
class ImagerPanel final : public GenericModulePanel
{
public:
    ImagerPanel (PureSheenProcessor&, EditorServices&);
};
} // namespace ps::ui
