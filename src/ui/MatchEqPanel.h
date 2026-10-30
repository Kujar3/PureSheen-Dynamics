#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Match EQ panel. Owner: ui-meq. Implementation: MatchEqPanel.cpp (+ MatchDisplay, MatchEqModel).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::MatchEq; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Delta, Reset).
//
// Content: a MatchDisplay (Reference and Current spectra, the running spectrum while capturing, the installed matched
// curve on its own dB axis, the shaded region outside the Low / High cutoff handles, capture / file-load status)
// above four cards:
//   REFERENCE   Capture / Stop, Load file... (async juce::FileChooser -> processor.startMatchReferenceFile, progress
//               and Cancel while the processor analyses it, plain-text errors), Clear; source, length, sample rate
//   CURRENT     Capture / Stop, Clear; length, gaps while capturing
//   FINE TUNE   Amount, Smoothing, Limit Boost, Limit Cut (+ Low / High Cutoff knobs when the panel is wide; the
//               display's handles edit them at every size)
//   FILTER      Linear | Minimum phase, FIR size, latency
// The header's channel tab picks the channel set (M/S: the Mid / Side curves, T/S: the Transient / Sustain instance
// with its own spectra); with Link on every edit, capture and Clear goes to both sets / parts.
namespace ps::ui
{
class MatchEqPanel final : public ModulePanel
{
public:
    MatchEqPanel (PureSheenProcessor&, EditorServices&);
    ~MatchEqPanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    void channelSetChanged() override;
    void paint (juce::Graphics&) override;

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
