#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Exciter module panel. Owner: ui-stereo. Implementation: ExciterPanel.cpp (+ ExciterParts, ImagerExciterCommon).
//
//   top    : MultibandDisplay (spectra, crossovers, band count, solo, selection) with a drive overlay per band: the
//            band's model as a chip under the band header, its drive as a column from the bottom with a value handle
//            (drag vertically anywhere in the band = "exc.<a|b>.b<n>.drive", one gesture per drag, Ctrl / Shift = fine,
//            double-click = default, right-click = Reset / Enter value) and the harmonics it adds (ExciterMeters)
//            along the bottom edge
//   bottom : the selected band's card (power, the seven models as a grid, Drive, Mix), the model's transfer curve at
//            that drive, and the global card (Oversampling, Post filter on / freq / gain, crossover type)
// Per-set parameters follow the header's A/B (Mid / Side) tab; with Link on in M/S every edit applies to both sets.
// Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class MultibandDisplay;

class ExciterPanel final : public ModulePanel
{
public:
    ExciterPanel (PureSheenProcessor&, EditorServices&);
    ~ExciterPanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    void channelSetChanged() override;
    bool hasLearn() const override { return true; }

    MultibandDisplay& getBandDisplay() noexcept;   // test / tooling access

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
