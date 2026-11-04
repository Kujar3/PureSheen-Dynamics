#pragma once

#include "ui/Widgets.h"

#include <memory>

// De-limiter panel. Owner: mod-dlim. Implementation: DeLimiterPanel.cpp (+ DeLimiterViews).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::DeLimiter; it is a
// ModulePanel (the base draws the header: name, presets, Delta, Reset; Stereo only, so no channel modes or tabs).
//
//   left   : "De-limiter" card: the large Amount knob, Input Gain and Output Gain, the Learn Threshold button
//   centre : the display (dlim::Display): the input after Input Gain and the lift over the last seconds, the Threshold
//            line with its handle and value, the held lift's readout (the largest lift, held 1 s)
// Every DeLimiterParams parameter has a control; "dlim.on" lives in the chain strip. Every control has a tooltip.
// Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class DeLimiterPanel : public ModulePanel
{
public:
    DeLimiterPanel (PureSheenProcessor&, EditorServices&);
    ~DeLimiterPanel() override;

    void tick() override;
    void refresh() override;
    void paint (juce::Graphics&) override;

    int getNumControls() const noexcept;              // parameter controls (tests / snapshot checks)

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
