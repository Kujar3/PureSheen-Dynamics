#pragma once

#include "ui/EditorContext.h"

#include <memory>

// The Assistant page (owner: assistant). Shown in the module-panel area when the header's view switch is on
// "Assistant" (EditorContext selection kAssistantSelection); the chain strip and the I/O panel stay visible.
//
// Pages:
//   Start    two cards: Quick (listen now, detected target) and Custom (targets first)
//   Custom   target list (Auto, a reference track, every tone target), the modules the Assistant may use, intensity,
//            Maximizer / loudness / output level / true peak, analysis time; Back and Start
//   Listen   "Play the loudest part of your track", a progress ring, Cancel, Analyse a file instead
//   Results  suggestion cards across the top (click = build that chain, one undo step); the target list (click =
//            rebuild with that target); Tone (match amount, tilt, the target's band display), Loudness (target LUFS,
//            readouts) and the other macros (dynamics match, width match + four band widths, compress, definition,
//            tone balance); Loudness matched, Relearn, Bypass (hold to compare, level matched), Open chain
// The page follows the Assistant's phase unless the user picked Start / Custom.
namespace ps::ui
{
class AssistantPanel final : public juce::Component
{
public:
    enum class Page { Start, Custom, Listen, Results };

    explicit AssistantPanel (EditorContext&);
    ~AssistantPanel() override;

    void tick();                       // 30 Hz: phase, progress, cards, macro positions
    void showPage (Page);              // also a snapshot / test hook
    Page getPage() const noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
