#pragma once

#include "ui/ModulePanel.h"

// Tone Balance panel. Owner: ui-spectral. Implementation: ToneBalancePanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::ToneBalance; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Learn, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: a SpectralDisplay (target corridor, measured level, output spectrum, correction curve, Low /
// Mid / High regions with strength bars) with the target menu, Shape / Cut / Boost and Tame Transients on the right and
// the Amount, Speed or Cut Speed, Smoothing or Sensitivity, Low / Mid / High knobs below.
// It may stop deriving from GenericModulePanel (derive from ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class ToneBalancePanel : public GenericModulePanel
{
public:
    ToneBalancePanel (PureSheenProcessor&, EditorServices&);
    ~ToneBalancePanel() override;
};
} // namespace ps::ui
