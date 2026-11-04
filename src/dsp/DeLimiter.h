#pragma once

#include "dsp/MaximizerLearn.h"
#include "dsp/Module.h"

#include <memory>

// De-limiter: brings back the peaks a limiter took from a finished master, Stereo only. Owner: mod-dlim. Implementation:
// DeLimiter.cpp. Tests: tests/DeLimiterTests.cpp, prefix "dlim.". The reference does this with a neural network; this is
// a small DSP model of what that network was measured to do (black-box renders only). Every fitted coefficient is in one
// constant block at the top of DeLimiter.cpp. Scripts and results: out/delimit/.
//
//   x -> Input Gain -> delay D -> one gain curve g (t), the same for both channels, never below 0 dB
//     -> y = x (1 + A (g - 1)), A = Amount / 100 (so 100 % = g, 200 % = twice the linear lift, 0 % = no change)
//     -> Output Gain
//
// The gain curve, evaluated once per hop (16 samples at 44.1 kHz, scaled with the rate), from the mid (L + R) / 2 after
// Input Gain (a signal only in the side gets no lift, as the reference) and relative to Threshold (only the level over
// the threshold counts: Input Gain +6 acts as Threshold -6, apart from the level itself):
//   r  rms over 6 hops, p  peak over 6 hops, s  slow one-pole level (60 ms), c = r - s  the contrast against it
//   G = gate (p) x (sum of fitted hinges on r, p, s and c), at least 0
//   flat tops: the share of samples sitting still over a long one-pole (300 ms) raises G by up to 2.5x on clipped material
//   a final curve restores the height of the spikes, then G is capped at kMaxGainDb
//   g = 10^(G / 20), box-smoothed over 3 hops, interpolated per sample
//
// Learn Threshold (measured): pressing it waits, for as long as it takes, for the first sample above silence. From that
// sample a fixed clock of 6.00 s runs on, whatever the audio does (silence or a stopped host does not pause it, as long
// as the host keeps processing). The result is the highest sample peak of the louder channel in that window, after
// Input Gain, in dB, clamped to the Threshold range -12 .. 0. The audio thread only compares samples and publishes the
// result; the processor writes it to "dlim.thr" as one undo step on the message thread.
//
// Latency: latencyFor (rate) = 5 hops (80 / 85 / 175 / 350 samples at 44.1 / 48 / 96 / 192 kHz), constant.
// Neutral: Amount 0 %, Input Gain 0, Output Gain 0 (the defaults): output = input delayed by D, bit-exact.
// Below about 14 dB under the threshold the module adds nothing at any Amount.
// Parameter changes: Amount and both gains glide over 20 ms, Threshold moves the curve over 20 ms.
// Delta: the slot's generic processed - dry, so it includes Input Gain and Output Gain.
// T/S: not offered (no twin). The model was fitted at 44.1 kHz; other rates scale its times.
namespace ps
{
inline constexpr double kDeLimiterLearnSeconds = 6.0;
inline constexpr float kDeLimiterMinThresholdDb = -12.0f, kDeLimiterMaxThresholdDb = 0.0f;

struct DeLimiterMeters
{
    AtomicFloat gainDb;              // the lift being applied now, after Amount (dB, >= 0)
    AtomicFloat heldGainDb;          // the largest lift, held for 1 s (dB, >= 0): the panel's readout
};

class DeLimiter final : public Module
{
public:
    DeLimiter();
    ~DeLimiter() override;

    ModuleType getType() const noexcept override { return ModuleType::DeLimiter; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    static int latencyFor (double sampleRate) noexcept;   // getLatencySamples() == latencyFor (prepared rate)

    const DeLimiterMeters& getMeters() const noexcept;

    // For the panel (single consumer): per 4 hops one pair, L = the loudest input sample after Input Gain (dBFS, floor
    // -120), R = the largest lift after Amount (dB).
    StereoSampleFifo& getHistoryFifo() noexcept;

    // Learn Threshold. Start / cancel / take: message thread. See above.
    void startLearn() noexcept;
    void cancelLearn() noexcept;
    bool isLearning() const noexcept;                      // from the press until the result was taken
    bool isWaitingForAudio() const noexcept;               // learning, and no audio has arrived yet
    float getLearnProgress() const noexcept;               // 0..1 of the 6 s
    bool takeLearnedThreshold (float& thresholdDb) noexcept;   // true once, when a result is ready

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
