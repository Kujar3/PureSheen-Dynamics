#pragma once

#include "ui/ModulePanel.h"

// Smoother panel. Owner: ui-spectral. Implementation: SmootherPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::Smoother; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Learn, Delta, Reset).
// For now it is the GenericModulePanel (every parameter as a control, built from ps::paramSpecs()). The dedicated
// panel replaces the body: a Spectrum | Trace view (input / output spectra with 32 reduction bars and the region
// handles + solo; or a scrolling waveform with the reduction trace), the input / reduction meter, Learn in the header,
// then Gentle / Medium / Strong, Amount, Tone, Attack, Release and the region values.
// It may stop deriving from GenericModulePanel (derive from ModulePanel directly) as long as the constructor stays.
namespace ps::ui
{
class SmootherPanel : public GenericModulePanel
{
public:
    SmootherPanel (PureSheenProcessor&, EditorServices&);
    ~SmootherPanel() override;
};
} // namespace ps::ui
