#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Smoother panel. Owner: ui-spectral. Implementation: SmootherPanel.cpp (+ SpectralDisplay, SpectralWidgets).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::Smoother; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Delta, Reset).
//
//   top left  : Spectrum | Trace view.
//               Spectrum: SpectralDisplay with the input (pre tap, grey) and output (post tap) spectra, the 32 Mel-band
//               reductions (grDb) as bars hanging from the right scale's 0 dB line (0 .. -18 dB, with a held peak),
//               the action region (shaded outside, low / high handles on "smth.<set>.low/high", the header pill =
//               move both, S = region solo through ModuleMonitor::soloRegion) and, while learning, the learn scores.
//               Trace: a scrolling history of the region's peak level and the deepest reduction (6 s).
//   top right : the view switch, the Input / GR meter (regionPeakDb, maxGrDb) and Learn: ModuleMonitor::learnRegion
//               for 6 s (or until clicked again), then the region from smootherLearnedRegion (Smoother.h, the rule
//               the tests check) is written to "<set>.low" / "<set>.high" (and the Link twins) as one undo step.
//               (Learn lives in the panel, not the base header: the header's Learn button learns crossovers.)
//   bottom    : Reduction card (Gentle / Medium / Strong in its title row, Amount, Tone), Timing card (Attack,
//               Release), Region card (Low, High).
// Per-set controls and meters follow the header's A/B tab (set B in the side-channel colour); with Link on every edit
// applies to both sets. Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class SmootherPanel : public ModulePanel
{
public:
    SmootherPanel (PureSheenProcessor&, EditorServices&);
    ~SmootherPanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    void channelSetChanged() override;

    // Tooling (dev snapshot): the view (false = Spectrum, true = Trace) and Learn as the buttons do it.
    void setTraceView (bool);
    void startLearn();
    bool isLearning() const noexcept;

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
