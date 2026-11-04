#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Classic Limiter panel. Owner: mod-clim. Implementation: ClassicLimiterPanel.cpp (+ ClassicLimiterViews).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::ClassicLimiter; it is a
// ModulePanel (the base draws the header: name, presets, Delta, Reset; Stereo only, so no channel modes or tabs).
//
//   left   : "Level" card: Threshold, the Link toggle, Ceiling (with Link on, either knob moves both by the same
//            amount, one undo step), the resulting gain (Ceiling - Threshold) as the card's subtitle
//   centre : the gain-reduction history (ps::ClassicLimiter::getHistoryFifo: the slow stage in the module colour, the
//            fast stage's share in amber; 5 s, 0 dB at the top) with the two stage meters and the output peak
//   right  : "Character" card: the mode (Analog | Tube | Modern) with a one-line description, the Character knob
//            (Fast .. Slow), True Peak at the bottom
// Every ClassicLimiterParams parameter has a control; "clim.on" lives in the chain strip. Every control has a tooltip.
// Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class ClassicLimiterPanel : public ModulePanel
{
public:
    ClassicLimiterPanel (PureSheenProcessor&, EditorServices&);
    ~ClassicLimiterPanel() override;

    void tick() override;
    void refresh() override;
    void paint (juce::Graphics&) override;

    int getNumControls() const noexcept;             // parameter controls (tests / snapshot checks)

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
