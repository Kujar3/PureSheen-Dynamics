#pragma once

#include "ui/Widgets.h"

#include <memory>

// Vectorscope of the final output (Imager panel). Owner: ui-stereo.
//
// Feed: the chain's output FIFO has a single consumer, the editor, which pulls it once per tick for the I/O meters
// and the output spectrum. So that the scope never becomes a second consumer (it would steal blocks from the meters),
// the editor forwards the block it pulled, right after pulling it:
//
//     n = chain.getOutputFifo().pull (scratchL.data(), scratchR.data(), kScratch);
//     ...
//     ps::ui::Vectorscope::publishOutput (processor, scratchL.data(), scratchR.data(), n);   // <- one line
//
// publishOutput hands the block to every live Vectorscope of that processor (message thread only, no locks; a scope
// registers itself on construction and unregisters on destruction). Without that line the scope stays empty
// ("Play audio to see the stereo image") and hasFeed() is false.
//
// Display modes:
//   Polar Sample  every sample as a dot on a half disc: angle = stereo position (straight up = mono / centre,
//                 the 45 degree diagonals = hard left / right, horizontal = out of phase), radius = level (dB scale)
//   Polar Level   rays per direction: length = the level coming from that direction (with ballistics)
//   Lissajous     classic X/Y goniometer rotated 45 degrees (M up, S across), level-compressed (dB scale)
// Persistence: dots accumulate into an intensity buffer that decays with a ~120 ms time constant (rendered at the
// display's pixel density).
// Under the scope: a correlation history (last 8 s, when there is spare height), then the phase correlation
// (-1..+1, 300 ms) and L/R balance meters.
namespace ps::ui
{
class Vectorscope final : public juce::Component
{
public:
    enum class Mode { PolarSample = 0, PolarLevel, Lissajous };

    Vectorscope (const PureSheenProcessor&, juce::Colour colour);
    ~Vectorscope() override;

    // Message thread: forwards one block of the final output to the scopes of `processor`.
    static void publishOutput (const PureSheenProcessor& processor, const float* left, const float* right, int numSamples);

    // Direct feed (tests, or an owner that has its own audio).
    void pushSamples (const float* left, const float* right, int numSamples);

    void tick();                                  // 30 Hz: consumes the queued audio, decays, repaints

    void setMode (Mode);
    Mode getMode() const noexcept;

    bool hasFeed() const noexcept;                // audio has been delivered at least once
    bool hasSignal() const noexcept;              // recent output above -70 dBFS
    float getCorrelation() const noexcept;        // -1..+1 (smoothed)
    float getBalance() const noexcept;            // -1 (left) .. +1 (right)

    // Height of the meter strip under the scope area (correlation + balance).
    static constexpr int kMeterHeight = 44;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Vectorscope)
};
} // namespace ps::ui
