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
//   1. X (b) = band level (octave density, dB), band power integrated over max (5 ms, min (40 ms, 4 / bandwidth))
//      (a stable estimate in the narrow low bands; about 2 hops at 3 kHz, 1 hop in the top octave)
//   2. tone weighting of the detector: X' = X - (3 dB/oct x tone / 100) log2 (f_b / 1 kHz) (+ tone = brighter result)
//   3. reference R (b) = power mean of this hop's (not integrated) unweighted band densities over the bands within
//      +-1 octave of f_b, excluding b and its two neighbours (unweighted: a tilted reference would cancel the tone
//      weighting over the symmetric neighbourhood), then a 250 ms one-pole in dB on a falling reference (a rising one
//      is taken at once, so a broadband hit never stands out and transients pass); a narrow peak stands out, a broad
//      tilt does not. If that set is empty (the first two Mel bands at 48 kHz), use X' (b) itself and request no new
//      reduction; any old reduction releases toward 0
//   4. excess E = X' - R - 12 dB x (1 - A), A = amount / 100 (more Amount = lower effective threshold)
//   5. gain computer: GR* = -A x min (maxGR, (1 - 1 / ratio) softknee (E, knee)); Gentle 1.6:1 knee 6 max 6 dB,
//      Medium 2.5:1 knee 6 max 12 dB, Strong 5:1 knee 4 max 18 dB. The knee is centred on E = 0, so at Amount 100
//      flat material (E ~ 0) gets a small broadband background (pink noise: mean 0.2 / 0.3 / 0.3 dB, deepest band
//      0.6 / 0.9 / 1.0 dB for Gentle / Medium / Strong); at Amount 50 it is untouched
//   6. gate: bands below -80 dBFS density or > 50 dB under the frame's loudest band get no new reduction (they release)
//   7. region mask: 1 inside [low, high], raised-cosine to 0 over 1/6 octave outside (by band centre)
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
// Reference numbers (48 kHz, pink noise + a +9 dB third-octave resonance at 3 kHz, region 2 .. 10 kHz, Amount 100,
// attack 8 / release 30 ms; tests/SmootherTests.cpp): the resonance's third octave is cut by about 3.4 dB (Medium) /
// 4.5 dB (Strong), the 3 kHz band meter reads -4.1 / -5.4 dB, the third octaves at 1.5 and 6 kHz move < 0.15 dB.
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
