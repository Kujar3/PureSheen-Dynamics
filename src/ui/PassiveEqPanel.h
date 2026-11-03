#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Passive EQ panel. Owner: mod-peq. Implementation: PassiveEqPanel.cpp.
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::PassiveEq; it is a
// ModulePanel (the base draws the header: name, presets, channel modes Stereo / M/S / L/R / T/S, the channel tabs with
// Link, Delta, Reset).
//
//   top    : the response view: the input / output spectra, the combined curve of the shown channel set (the other set
//            dashed in M/S, L/R and T/S) over a soft area per section in its group colour, a dot at each active
//            section's frequency, the set name and a cursor readout. Not editable (the knobs edit).
//   row 1  : the program EQ (top section): Low (Freq, Boost, Cut), High Boost (Freq, Boost, Bandwidth), High Cut (Freq,
//            Cut)
//   row 2  : the midrange EQ (bottom section): Low-Mid (Freq, Boost), Mid Cut (Freq, Cut), High-Mid (Freq, Boost), and
//            Tube (Drive, shared by both channel sets)
// The frequency selectors are stepped knobs (a detent dot per choice). Per-set knobs follow the header's channel tab
// and, with Link on, edit both sets in the same gesture. The curves come from the same section designs the DSP runs
// (dsp/PassiveEqDesign.h), so they match PassiveEq::getMagnitudeDb exactly. Every control has a plain-language
// tooltip. Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class PassiveEqPanel : public ModulePanel
{
public:
    PassiveEqPanel (PureSheenProcessor&, EditorServices&);
    ~PassiveEqPanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    void channelSetChanged() override;

    int getNumControls() const noexcept;   // parameter controls (tests / snapshot checks)

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
