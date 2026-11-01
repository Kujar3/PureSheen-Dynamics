#pragma once

#include "dsp/Module.h"

#include <memory>

// Smoother: a 32-band spectral compressor inside an action region (channel modes Stereo, M/S, T/S), measured from the
// reference: an absolute threshold that Amount moves, per-Hz detection, Tone tilts it. It only cuts. Owner: dsp-smth.
// Implementation: Smoother.cpp (on ps::SpectralEngine, base size 1024).
// Tests: tests/SmootherTests.cpp, prefix "smth.".
//
// Engine: residual STFT, N = 1024 at 44.1 / 48 kHz (x2 / x4 above), hop N / 4; latency N, constant (also with region
// solo). Bands: kSmootherBands Mel bands (edges equally spaced on 2595 log10 (1 + f / 700) between 20 Hz and
// min (20 kHz, 0.475 fs)), triangular weights. Per hop, per lane:
//   1. X (b) = band level (octave density, dB), band power integrated over max (5 ms, min (40 ms, 4 / bandwidth))
//      (a stable estimate in the narrow low bands; about 2 hops at 3 kHz, 1 hop in the top octave)
//   2-5. (measured: the reference's renders of pink noise in level steps) the detector reads per-Hz level against an
//      ABSOLUTE threshold that Amount moves: e = X + 6.98 - 3.44 log2 (f_b / 1 kHz) - 0.173 tone log2 (f_b / f_c) - T,
//      T = -65.1 + (100 - amount) dB, f_c = the region's centre (+ tone = the cut moves down = brighter);
//      GR* = -M tanh (r softknee (e, K) / M): Gentle r 0.534 K 2.5 M 18.1, Medium r 0.772 K 6 M 17.6, Strong r 1
//      K 2.9 M 19.8 (a compressor that saturates at M). Amount 100 cuts pink noise at -52 dBFS by ~5 dB in the
//      region and at -22 dBFS by the full ~16 dB; Amount 60 starts near -24 dBFS
//   6. gate: bands below -80 dBFS density or > 50 dB under the frame's loudest band get no new reduction (they release)
//   7. region mask: raised-cosine edges 1.3 octaves wide centred on low and high (half the reduction at each limit,
//      as measured; by band centre)
//   8. ballistics per band: attack while GR deepens, release otherwise (per-hop coefficients; times below one hop act
//      per hop, the frame itself is the lookahead); release floored at 2 hops where a neighbour also releases
//   9. [0.2 0.6 0.2] smoothing across bands (edges replicate; an isolated one-band peak therefore gets at most
//      0.8 x maxGR); bin gains g (k) = sum_b w_b (k) 10^(GR_b / 20) / sum_b w_b (k) (<= 1)
//      where the weight sum is positive; uncovered bins (including DC and Nyquist) get exactly unity
//  10. residual synthesis. Never above 0 dB.
// Stereo / T/S: one curve for both channels (power-sum detector, scaled by 1/2 so a mono signal reads its channel
// level). M/S: Mid with set A, Side with set B. Amount ramps over 50 ms, Tone / Low / High glide (30 ms), a mode
// change glides the curve (80 ms one-pole for 300 ms, eased away over the last 80 ms to restore normal ballistics).
// Meters: X in dB re a full-scale sine per octave.
// Neutral: Amount 0 (the default) -> exact null against the input delayed by N (also after automation back to 0,
// once every band has released: a band within 1e-6 dB of 0 snaps to exactly 0).
// Against the reference's pink-noise level steps (Amount 80, Medium) the region's cut is within ~1 dB at every level
// (out/importer/checks-findings.md).
// Monitors: soloRegion = ISTFT (region mask x X): the input inside the region, before processing, same latency.
//   learnRegion: accumulates learnScore[set][b] += max (0, X (b) - mean X - 3 dB) per hop (the frame's mean over the
//   ungated bands) and publishes it; the UI picks the region (smallest contiguous span holding 75 % of the
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

// (added by ui-spectral) The Learn rule, shared by the panel and tests/SmootherTests.cpp: from learnScore[set], the
// smallest contiguous band span holding 75 % of the total score (the lowest such span on a tie), widened by half a band
// on each side (the geometric mean of the edge band's centre and its outer neighbour's centre; beyond the outermost
// band, the same ratio mirrored). Returns false and leaves lowHz / highHz unchanged when nothing was learned (total
// score <= 0 or not finite). Any thread, no allocation.
bool smootherLearnedRegion (const SmootherMeters&, int set, double sampleRate, double& lowHz, double& highHz) noexcept;

} // namespace ps
