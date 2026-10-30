#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Equalizer 1 and Equalizer 2 panel (one class, the module type picks the parameter prefix). Owner: ui-eq.
//
// Content: a large graphic display (ui/EqDisplay.h: spectra, exact response curves, numbered nodes, mouse editing)
// above a strip with the selected band's controls (band chips, on, shape, slope, freq, gain, Q, phase in Digital
// mode) and the module settings (Analog / Digital, FIR size in Digital mode, soft saturation, amount).
// Follows the header's channel-set tab; with Link on (M/S, L/R) every edit goes to both channel sets.
namespace ps::ui
{
class EqPanel final : public ModulePanel
{
public:
    EqPanel (PureSheenProcessor&, EditorServices&, ps::ModuleType eq1or2);
    ~EqPanel() override;

    void tick() override;
    void refresh() override;
    void channelSetChanged() override;
    void paint (juce::Graphics&) override;

    // (tests / snapshot checks) the band the strip edits, 0..7.
    int getSelectedBand() const noexcept;

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
