#pragma once

#include "dsp/Module.h"

#include <memory>

// Multiband Dynamics (reference-suite parity): up to 4 bands, per band a compressor (downward / upward) and a limiter (downward /
// upward expansion), per band parallel mix and gain. Owner: dsp-dynamics. Implementation: Dynamics.cpp.
//
//   - Crossover: ps::Crossover (Analog / Digital / Hybrid); one band = no split
//   - Detection: ps::LevelDetector (Peak / Envelope / RMS) on the detection-filtered signal (None / HighPass at
//     detectionFilterHz with 6..48 dB/oct / Tilt of detectionTiltDb per octave around detectionFilterHz), split by
//     the same crossover as the audio
//   - Stereo mode: detector = max (|L|, |R|) per band, one gain for both channels. M/S: independent, ch1 uses set B
//   - Lookahead 0..10 ms: the audio is delayed, the detector is not (latency = lookahead + crossover latency)
//   - Compressor: GainCurve + GainSmoother (attack/release, adaptive release); Limiter: second GainCurve after the
//     compressor with its own times (fast, high ratio), not brickwall (reference-suite parity: 30:1 may overshoot)
//   - Auto gain: per band makeup = - (average gain change of that band at a -18 dBFS RMS pink reference level through
//     the static curves), recomputed when the curve changes (no level-dependent pumping)
//   - Parallel: band out = dry_band * (1 - mix) + wet_band * mix (same latency)
//   - Band solo (ModuleMonitor::soloBand): output = that band only; soloDetectionFilter: output = detector signal
namespace ps
{
struct DynamicsMeters
{
    AtomicFloat gainReductionDb[kNumChannelSets][kMaxBands];   // current total gain change (compressor + limiter),
                                                               // <= 0 downward, > 0 upward
    AtomicFloat compGainDb[kNumChannelSets][kMaxBands];
    AtomicFloat limGainDb[kNumChannelSets][kMaxBands];
    AtomicFloat detectorDb[kNumChannelSets][kMaxBands];        // detector level (threshold meters)
};

class Dynamics final : public Module
{
public:
    Dynamics();
    ~Dynamics() override;

    ModuleType getType() const noexcept override { return ModuleType::Dynamics; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const DynamicsMeters& getMeters() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
