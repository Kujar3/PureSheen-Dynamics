#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Imager module panel. Owner: ui-stereo. Implementation: ImagerPanel.cpp (+ Vectorscope, ImagerExciterCommon).
//
//   left, top    : MultibandDisplay (input / output spectra, crossovers, band count, solo) with a width overlay per
//                  band: the band's width as a bar from the centre line (up = wider, down = narrower) with a value
//                  handle; drag vertically anywhere in the band = "img.b<n>.width" (one gesture per drag, Ctrl / Shift
//                  = fine, double-click = default, right-click = Reset / Enter value); a correlation bar per band
//                  (ImagerMeters) along the band's bottom edge
//   left, bottom : cards: Width (Amount, Prevent antiphase), Stereoize (On, Mode I / II, Amount; dimmed while no band
//                  is wider than 0), Recover Sides (On, Gain; dimmed while no band is narrowed), Crossover (type)
//   right        : Vectorscope of the final output (Polar Sample / Polar Level / Lissajous), correlation history,
//                  correlation and balance meters
// The Imager is Stereo only: there are no A/B channel sets. Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class MultibandDisplay;
class Vectorscope;

class ImagerPanel final : public ModulePanel
{
public:
    ImagerPanel (PureSheenProcessor&, EditorServices&);
    ~ImagerPanel() override;

    void tick() override;
    void refresh() override;
    bool hasLearn() const override { return true; }

    // Test / tooling access.
    MultibandDisplay& getBandDisplay() noexcept;
    Vectorscope& getVectorscope() noexcept;

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
