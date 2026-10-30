#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Bass Shaper panel. Owner: ui-lowend. Implementation: BassShaperPanel.cpp (+ BassScope, LowEndWidgets).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::BassShaper; it is a
// ModulePanel (the base draws the header: name, presets, Delta, Reset; Stereo only, so no channel modes or tabs).
//
//   left   : "Balance" card (sweet-spot meter of the low end against the rest: target zone, input ball, output hollow
//            ball, Light / Heavy; Balance knob) and "Punch" card (crest-factor meter, Dense / Spiky; Punch knob, the
//            transient share)
//   centre : BassScope (the low band over 4 s, hits tinted from the transient mask, both thresholds draggable) above
//            the Cutoff strip (a low-frequency axis with the input spectrum, the shaped band and the cutoff handle)
//   right  : "Sustain Power" card (power switch, Threshold, Amount, Character Dirty .. Clean, the current lift) and
//            "Peak Control" card (power switch, Threshold, Mix, Character Dirty .. Clean, the current reduction), Auto
//            Gain at the bottom
// Editing a switched-off section's control (a knob or fader drag, or a threshold line in the scope) switches the section
// on in the same undo step. Every BassShaperParams parameter has a control; "bass.on" lives in the chain strip.
// Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class BassScope;

class BassShaperPanel : public ModulePanel
{
public:
    BassShaperPanel (PureSheenProcessor&, EditorServices&);
    ~BassShaperPanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    void paint (juce::Graphics&) override;

    BassScope& getScope() noexcept;                  // test / tooling access
    int getNumControls() const noexcept;             // parameter controls (tests / snapshot checks)

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
