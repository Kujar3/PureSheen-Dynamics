#pragma once

#include "dsp/Module.h"

#include <memory>

// Low Contrast: time-frequency contrast of the low end inside an action region (20..300 Hz; channel modes Stereo,
// M/S, T/S). Positive contrast makes loud low-end moments (a kick) stand out more from the quiet ones around them
// (tighter, punchier); negative contrast evens them out (rounder, denser). Owner: dsp-lowc. Implementation:
// LowContrast.cpp. Tests: tests/LowContrastTests.cpp, prefix "lowc.".
//
//   y = x delayed by N  +  ISTFT ((G - 1) X)      ("delta" form: every G exactly 1 -> bit-exact delayed input)
// STFT: N = 4096 at 44.1 / 48 kHz, 8192 at 88.2 / 96, 16384 at 176.4 / 192 (~11 Hz bins at every rate), hop N / 8,
// periodic Hann analysis and synthesis (sum of w^2 = 3: synthesis scaled by 1/3), both channels in one complex FFT,
// only the bins with region weight > 0 computed. Latency = N exactly (85 ms at 48 kHz) for any block size, constant.
// Per hop, per bin k (Stereo / T/S: one detector on |X_L|^2 + |X_R|^2 and the same G for both; M/S: per channel and
// set):
//   1. P = |X|^2 smoothed across bins [0.25 0.5 0.25]
//   2. fast envelope F (per-hop one-pole, attack / release) and slow reference S (one-pole, then averaged over
//      +-1/3 octave of bins): Punchy F 5 / 60 ms, S 400 ms, depth 1.0; Smooth F 40 / 200 ms, S 1500 ms, depth 0.6
//   3. rel = 10 log10 ((F + eps) / (S + eps)) clamped to [-30, +20] dB
//   4. c = contrast / 100 (one-pole 50 ms, snapped to 0 below 1e-6); gDb = clamp (c x depth x rel, -24, +12)
//   5. region weight W (k): 1 inside [low, high], raised-cosine to 0 over 1/3 octave outside (log f), 0 beyond;
//      region edges glide (one-pole 50 ms in log f); make-up mDb = gain + autoGainDb (both smoothed 50 ms)
//   6. G (k) = 10^(W (gDb + mDb) / 20): exactly 1 where W = 0 or gDb + mDb == 0
// Mode switches only change coefficients (no click). Auto gain: region in / out power (Pin = sum W P, Pout =
// sum W |G|^2 P, one-pole 2 s), autoGainDb = clamp (10 log10 (Pin / Pout), -12, +12), frozen below -70 dBFS, glides 1 s.
// Neutral: contrast 0 and gain 0 (the defaults) -> the input delayed by N, bit-exact.
// Monitors: soloRegion = ISTFT (W X) (the unprocessed region, delayed by N; no dry path). Delta: the slot's.
// T/S: the twin (getParamSet() == 1) reads set B for both channels (Module.h).
namespace ps
{
struct LowContrastMeters
{
    static constexpr int kPoints = 48;                               // log-spaced 16 Hz .. 400 Hz (fixed axis)
    AtomicFloat diffDb[kNumChannelSets][kPoints];                    // time-averaged W (gDb + mDb), 150 ms one-pole
    AtomicFloat regionInDb[kNumChannelSets], regionOutDb[kNumChannelSets];   // region RMS (300 ms)
    AtomicFloat autoGainDb[kNumChannelSets];
};                                                                   // Stereo / T/S: set B mirrors set A

double lowContrastDisplayHz (int index) noexcept;                    // the kPoints axis

class LowContrast final : public Module
{
public:
    LowContrast();
    ~LowContrast() override;

    ModuleType getType() const noexcept override { return ModuleType::LowContrast; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const LowContrastMeters& getMeters() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
