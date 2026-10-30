#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Tone Balance panel. Owner: ui-spectral. Implementation: ToneBalancePanel.cpp (+ SpectralDisplay, SpectralWidgets).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::ToneBalance; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Delta, Reset).
//
//   top left  : SpectralDisplay. Left scale = level relative to pink noise (1 kHz = 0 dB): the target corridor
//               (toneTargetDb +- tolerance, module colour fill, dashed centre), the measured balance S - o (the module's
//               levelDb minus its target offset, light line) and the output spectrum (post tap) slid onto the corridor.
//               Right scale (0.5 x) = the live correction (gainDb, bold, filled to zero). Header: the Low / Mid / High
//               zones (< 100 Hz, 100 Hz - 5.6 kHz, > 5.6 kHz) with their strength; drag = "tbal.<set>.low/mid/high".
//   top right : Target card (the 36 targets in a menu grouped by genre family, Flat and Balanced on top; Shape / Cut /
//               Boost; Tame Transients) and the Range meter (the curve's max boost / max cut).
//   bottom    : Correction card (Amount, Speed or Cut Speed, Smoothing or Sensitivity: the knob of the current mode is
//               shown, the other keeps its value) and Regions card (Low, Mid, High).
// Per-set controls and curves follow the header's A/B tab (set B curves in the side-channel colour); with Link on every
// edit applies to both sets. Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class ToneBalancePanel : public ModulePanel
{
public:
    ToneBalancePanel (PureSheenProcessor&, EditorServices&);
    ~ToneBalancePanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    void channelSetChanged() override;

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
