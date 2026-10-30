#pragma once

#include "dsp/Module.h"

#include <memory>

// Smoother: a 32-band adaptive reduction of harsh, narrow peaks inside an action region (channel modes Stereo, M/S,
// T/S). It only cuts. Owner: dsp-smth. Implementation: Smoother.cpp (on ps::SpectralEngine, base size 1024).
// Tests: tests/SmootherTests.cpp, prefix "smth.".
//
// Engine: residual STFT, N = 1024 at 44.1 / 48 kHz (x2 / x4 above), hop N / 4; latency N, constant (also with region
// solo). Bands: kSmootherBands Mel bands (edges equally spaced on 2595 log10 (1 + f / 700) between 20 Hz and
// min (20 kHz, 0.475 fs)), triangular weights. Per hop, per lane:
//   1. X (b) = band level (octave density, dB)
//   2. tone weighting of the detector: X' = X - (3 dB/oct x tone / 100) log2 (f_b / 1 kHz) (+ tone = brighter result)
//   3. reference R (b) = power mean of X' over the bands within +-1 octave of f_b, excluding b and its two neighbours,
//      then a 250 ms one-pole in dB (a narrow peak stands out, a broad tilt does not). If that set is empty (the first
//      two Mel bands at 48 kHz), use X' (b) itself and request no new reduction; any old reduction releases toward 0
//   4. excess E = X' - R - 12 dB x (1 - A), A = amount / 100 (more Amount = lower effective threshold)
//   5. gain computer: GR* = -A x min (maxGR, (1 - 1 / ratio) softknee (E, knee)); Gentle 1.6:1 knee 6 max 6 dB,
//      Medium 2.5:1 knee 6 max 12 dB, Strong 5:1 knee 4 max 18 dB
//   6. gate: bands below -80 dBFS density or > 50 dB under the frame's loudest band get no new reduction (they release)
//   7. region mask: 1 inside [low, high], raised-cosine to 0 over 1/6 octave outside (by band centre)
//   8. ballistics per band: attack while GR deepens, release otherwise (per-hop coefficients; times below one hop act
//      per hop, the frame itself is the lookahead); release floored at 2 hops where a neighbour also releases
//   9. [0.2 0.6 0.2] smoothing across bands; bin gains g (k) = sum_b w_b (k) 10^(GR_b / 20) / sum_b w_b (k) (<= 1)
//      where the weight sum is positive; uncovered bins (including DC and Nyquist) get exactly unity
//  10. residual synthesis. Never above 0 dB.
// Stereo / T/S: one curve for both channels (power-sum detector). M/S: Mid with set A, Side with set B.
// Neutral: Amount 0 (the default) -> exact null against the input delayed by N.
// Monitors: soloRegion = ISTFT (region mask x X): the input inside the region, before processing, same latency.
//   learnRegion: accumulates learnScore[set][b] += max (0, E_full (b)) per hop (E_full with the region forced to
//   20 Hz .. 20 kHz and A = 1) and publishes it; the UI picks the region (smallest contiguous span holding 75 % of the
//   score, widened by half a band) and writes "<set>.low" / "<set>.high" as one undo step. Starting learnRegion clears
//   the scores. No effect on the audio.
// T/S: the twin (getParamSet() == 1) reads set B for both channels (Module.h).
namespace ps
{
inline constexpr int kSmootherBands = 32;
double smootherBandHz (int band, double sampleRate) noexcept;   // centre of a Mel band at this rate

struct SmootherMeters
{
    AtomicFloat grDb[kNumChannelSets][kSmootherBands];          // current reduction per band (<= 0)
    AtomicFloat bandLevelDb[kNumChannelSets][kSmootherBands];   // X (b)
    AtomicFloat regionPeakDb[kNumChannelSets];                  // highest band level inside the region
    AtomicFloat maxGrDb[kNumChannelSets];                       // deepest reduction of the last hop (<= 0)
    AtomicFloat learnScore[kNumChannelSets][kSmootherBands];    // learnRegion accumulation
};                                                              // Stereo / T/S: set B mirrors set A

class Smoother final : public Module
{
public:
    Smoother();
    ~Smoother() override;

    ModuleType getType() const noexcept override { return ModuleType::Smoother; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const SmootherMeters& getMeters() const noexcept;
    double getSampleRate() const noexcept;                      // for smootherBandHz in the UI

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
