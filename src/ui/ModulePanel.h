#pragma once

#include "ui/Widgets.h"

// GenericModulePanel (owner: ui-shell): a complete, usable panel for ANY module, built automatically from
// ps::paramSpecs(). Module panel agents may derive from it (or use it as a fallback until their own panel is done).
//
// Content:
//   - top: a spectrum view of the module's input (grey) and output (module colour) with the frequency grid, and the
//     crossover frequencies of multiband modules (bands beyond "<prefix>.bands" hidden)
//   - below, in a scrollable area, one card per group: module settings ("Settings", crossover controls in
//     "Crossover", sub-sections such as "Soft Clip" / "Stereoize" by their id prefix), then one card per band
//     ("Band 1  20 Hz - 120 Hz"), with sub-sections ("Compressor", "Limiter") inside.
//   - floats -> ParamKnob, bools -> ParamToggle, choices -> ParamChoice (2..3 short labels -> ParamSegments)
//   - per channel set ("<prefix>.<a|b>.") controls follow the header's A/B tab; with Link on (and a non-Stereo mode)
//     every edit is applied to both sets.
// The slot parameters (power, channel mode, channel on, link) are not repeated here: the chain strip and the panel
// header own them.
namespace ps::ui
{
class GenericModulePanel : public ModulePanel
{
public:
    GenericModulePanel (PureSheenProcessor&, EditorServices&, ps::ModuleType);
    ~GenericModulePanel() override;

    void tick() override;
    void refresh() override;
    void channelSetChanged() override;
    bool hasLearn() const override;      // true for the multiband modules (Dynamics, Imager, Exciter)

    // Number of parameter controls built (tests / snapshot checks).
    int getNumControls() const noexcept;

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

    // Height of the spectrum view for a content height (0 hides it). Derived panels can override.
    virtual int spectrumHeightFor (int contentHeight) const;

private:
    struct GenericImpl;
    std::unique_ptr<GenericImpl> generic;
};
} // namespace ps::ui
