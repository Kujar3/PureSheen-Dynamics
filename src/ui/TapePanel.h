#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Tape panel. Owner: mod-tape. Implementation: TapePanel.cpp (+ TapeViews).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::Tape; it is a
// ModulePanel (the base draws the header: name, presets, Delta, Reset; Stereo only, so no channel modes or tabs).
//
//   left   : "Tape" card: the speed (7.5 | 15 | 30 ips segments), the large Drive knob, the saturation meter (level
//            into the tape and the compression of its peaks, ps::TapeMeters)
//   centre : the response view (ps::Tape::responseDb: head bump, roll-off and high end of the current settings, the
//            bump's peak marked) above the "Emphasis" card (Low Emphasis = head bump, High Emphasis, 4 = flat)
//   right  : "Bias" card (Under .. Over, a note on the current regime) above the "Harmonics" card
// Every TapeParams parameter has a control; "tape.on" lives in the chain strip. Every control has a tooltip.
// Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
namespace tape
{
    class ResponseView;
}

class TapePanel : public ModulePanel
{
public:
    TapePanel (PureSheenProcessor&, EditorServices&);
    ~TapePanel() override;

    void tick() override;
    void refresh() override;
    void paint (juce::Graphics&) override;

    tape::ResponseView& getResponseView() noexcept;   // test / tooling access
    int getNumControls() const noexcept;              // parameter controls (tests / snapshot checks)

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
