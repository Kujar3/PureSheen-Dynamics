#pragma once

#include "dsp/Module.h"

#include <memory>

// Maximizer: the final loudness limiter (reference-suite parity; our own algorithms). Owner: dsp-maximizer.
// Implementation: Maximizer.cpp (+ MaximizerModes.cpp etc. as needed).
//
// Signal flow:  x * gain -> [Upward Compress] -> [Transient Emphasis] -> [Soft Clip (4x OS)] -> limiter (mode) ->
//               [True Peak stage] -> * outputLevel
// Contract on the output: with True Peak on, the 4x-oversampled true peak of the output never exceeds outputLevel
// (+0.05 dB tolerance measured by ps::TruePeakMeter); with it off, the sample peak never exceeds outputLevel.
// Modes (see Types.h): all share a lookahead peak limiter core (SlidingMax + smoothed gain, gain applied to the delayed
// signal) and differ in release behaviour / band structure:
//   LowLatency  : ~1 ms lookahead, program-independent release from Character
//   Smooth      : release right after each peak, from very fast (Character 0) to slow (10)
//   Transparent : the Smooth core a little slower at low Character
//   Aggressive  : longest lookahead, fastest recovery up to Character 1.4; styles Clipping / Crisp / Balanced /
//                 Pumping (the last two calm sustained bass)
//   Spectral    : per critical band group stage (currently no share: the reference matched a broadband core) + the
//                 Smooth core; styles Classic / Modern / Transient
//   Multiband   : 4-band stage (low / mid peaks on material with sparse highs) + the Aggressive core
// Character 0..10: attack/release scaling (0 = clip-like fastest .. 10 = very slow), in every mode.
// Stereo Independence: amount of unlinked gain (0 = fully linked) separately for transient and sustained gain
//   reduction.
// Upward Compress: lifts passages by their PEAK level (after Gain): upwardCompressDb in full below ~-17 dBFS peak,
//   fading to nothing at 0 dBFS (so 0 dBFS peaks stay put and limited material gets little lift).
// Delta (handlesDelta): ProcessContext::deltaOut receives what the limiting stages removed / added (excluding Gain
//   and Output Level), aligned with the output; the normal output is unchanged.
// Learn Input Gain: startLearn() listens for 5 s of input (playing audio only), then publishes the gain that brings
//   the output to learnTargetLufs (integrated, ps::LoudnessMeter on a simulated output); the processor writes it to
//   "max.gain".
// Latency: per mode, constant for a mode and sample rate (reported; the Spectral / Multiband modes are the largest).
namespace ps
{
struct MaximizerMeters
{
    AtomicFloat gainReductionDb[2];      // L, R current limiter gain change (<= 0)
    AtomicFloat upwardGainDb;            // current upward compression gain (>= 0)
    AtomicFloat peakGainReductionDb;     // max GR since the UI last read it (exchange)
};

class Maximizer final : public Module
{
public:
    Maximizer();
    ~Maximizer() override;

    ModuleType getType() const noexcept override { return ModuleType::Maximizer; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;
    bool handlesDelta() const noexcept override { return true; }

    const MaximizerMeters& getMeters() const noexcept;
    MaximizerMeters& getMeters() noexcept;             // (added) for the UI's exchange() of the peak GR

    // Learn Input Gain (message thread starts it; the audio thread does the listening).
    void startLearn() noexcept;
    void cancelLearn() noexcept;
    bool isLearning() const noexcept;
    float getLearnProgress() const noexcept;           // 0..1
    bool takeLearnedGain (float& gainDb) noexcept;     // true once, when a result is ready

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
