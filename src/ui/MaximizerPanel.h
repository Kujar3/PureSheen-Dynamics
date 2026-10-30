#pragma once

#include "ui/ModulePanel.h"

// Maximizer module panel (owner: ui-max). Implementation: MaximizerPanel.cpp, MaximizerWidgets.h/.cpp.
//
// Layout (content area):
//   +--------+------------------------------------------+----+--------+-----------+
//   | GAIN   | legend                       loudness    | GR | OUTPUT | MODE      |
//   | +4.0dB |                                          |-2.1| -1.0dB | list of 6 |
//   | input  |  scrolling level history (5 s):          | L R| output | styles    |
//   | meter  |  input, output, gain reduction from top, | GR | meter  | (Aggr. /  |
//   | + thr. |  threshold (= -Gain) and Output Level    |    | + Out  |  Spectral)|
//   | handle |  lines (draggable)                       |    | handle | or a      |
//   | [Link] |  time ruler                              |    | [TP]   | summary   |
//   +--------+------------------------------------------+----+--------+-----------+
//   | LIMITER        | STEREO INDEPENDENCE | SOFT CLIP  [o] | TRANSIENTS [o] | LEARN INPUT GAIN |
//   | Character Upw. | Transient  Sustain  | modes  Amount  | Amount         | (ring)  Target   |
//   +----------------+---------------------+----------------+----------------+------------------+
// Every MaximizerParams parameter has a control here; the slot parameter "max.on" lives in the chain strip.
namespace ps::ui
{
class MaximizerPanel final : public ModulePanel
{
public:
    MaximizerPanel (PureSheenProcessor&, EditorServices&);
    ~MaximizerPanel() override;

    void tick() override;
    void refresh() override;
    void paint (juce::Graphics&) override;

    // Number of parameter controls (tests / snapshot checks).
    int getNumControls() const noexcept;

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
