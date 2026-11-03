#pragma once

#include "plugin/TrackReferencing.h"
#include "ui/Theme.h"

#include <memory>

// The waveform strip of the Reference page (owner: referencing UI; ported from RefMatch's waveform strip).
//
//   Gutter (left): what plays ("Whole track" / "Loop B 0:16"), the position, and the loop buttons A B C D E:
//     click an empty letter = arm it (a drag on the waveform then draws that loop; click again to disarm),
//     click a set letter = play that loop (the active one again = the whole track), right-click a letter = clear it.
//   Waveform: the overview of the selected reference with its short-term loudness (silhouette + line), the five loops
//     as coloured regions with their letter (the active one bright, the rest of the track dimmed), the playhead and a
//     time ruler. Click = jump there; drag = draw a loop (the armed letter, else the first empty one, else the active
//     one); drag a loop's edge = move it. Loading / error / missing states draw in the waveform area.
namespace ps::ui
{
class RefWaveform final : public juce::Component, public juce::TooltipClient
{
public:
    explicit RefWaveform (psplug::TrackReferencing&);
    ~RefWaveform() override;

    // The selected reference (nullptr = none) and its draw data. Cheap when nothing changed.
    void setEntry (const psplug::TrackReferencing::Entry*, int infoVersion);
    void tick();                                   // 30 Hz: playhead, spinners, a committed loop's hold time
    int getArmedLetter() const noexcept;           // -1 = none (snapshot / test hook)
    void setArmedLetter (int);

    std::function<void()> onRelink;                // the Missing / Error message was clicked

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
