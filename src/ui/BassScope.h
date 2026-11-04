#pragma once

#include "ui/Widgets.h"

#include <memory>

// Oscilloscope of the Bass Shaper's low band. Owner: ui-lowend. Implementation: BassScope.cpp.
//
//   - data: ps::BassShaper::getScopeFifo (per 16 output samples: min / max of the low-band mid) and getMaskFifo (per
//     16 samples: the transient mask w and the low-band envelope), and the meter lowPeakDb (exchanged). Exactly one
//     consumer pulls them: a feed shared by every BassScope of the same processor (a second editor window shares it
//     too); it lives while at least one scope exists and discards what piled up before it was created.
//   - view: the last 4 s (newest at the right, standing still while the host does not process), one min / max column
//     per pixel, coloured by the mask (transient portions in the accent colour, sustain grey); a symmetric amplitude
//     axis in dB (0 .. -48 dBFS, both polarities) with ticks at 0, -6, -12, -18, -24 and -36 dBFS
//   - lines: +-Sustain threshold dashed (sky), +-Peak threshold solid (coral); a section that is off draws its lines
//     dimmed. Drag a line vertically = one gesture on its threshold that also switches its section on; Ctrl / Shift =
//     fine; double-click = type a value; right-click = Reset / Enter value; hover shows the value.
// Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class BassScope final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit BassScope (PureSheenProcessor&);
    ~BassScope() override;

    void setColour (juce::Colour);
    // Pulls new scope data (the shared feed) and repaints when something changed. Call from tick() / frameTick().
    void update();

    bool hasSignal() const noexcept;              // something above -60 dBFS in the shown 4 s
    float getLowPeakDb() const noexcept;          // low-band peak, held for ~1.5 s (dBFS, -200 = silence)
    bool getLatestValues (float& minimum, float& maximum, float& mask) const noexcept; // tooling / shared-feed checks
    float getThresholdY (bool sustain, bool upper) const noexcept;   // y of a threshold line (tests / tooling)

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
