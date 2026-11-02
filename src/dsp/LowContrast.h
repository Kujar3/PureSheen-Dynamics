#pragma once

#include "dsp/Module.h"

#include <memory>

// Low Contrast: time-domain contrast of the low end inside an action region (20..300 Hz; channel modes Stereo,
// M/S, T/S). Punchy at positive contrast lifts the first ~20 ms of each low-end note or hit (an onset shaper) and
// dips the body slightly after it; Smooth at positive contrast does the opposite on steady notes (onset slightly
// down, sustain up); negative contrast reverses each mode, about half as strongly. Owner: dsp-lowc. Implementation:
// LowContrast.cpp. Tests: tests/LowContrastTests.cpp, prefix "lowc.".
//
//   y = x delayed by N  +  sum_b (g_b - 1) xb_b       (every g_b exactly 1 -> bit-exact delayed input)
// Region bands: xb_b = ISTFT (W W_b X), N = 4096 at 44.1 / 48 kHz, 8192 at 88.2 / 96, 16384 above, hop N / 4, periodic
// Hann analysis and synthesis (linear phase). W: ramps linear in frequency, 0 at 0.8 low to 1 at 2 low and 1 at
// 0.5 high to 0 at 2 high, multiplied (as the reference's region; edges glide 50 ms); W_b: 4 bands equally spaced in
// log f inside the region, cosine^2 crossfades 0.9 octave wide, sum 1. Latency = N exactly (93 ms at 44.1 kHz) for any block size, constant.
// Gains (per band; Stereo / T/S: one detector on (eL + eR) / 2 and the same gains for both; M/S: per channel and set):
//   1. detector band: Butterworth high pass (2nd order) and low pass (4th order) at the band's edges (the top band's
//      at 1.25 high, where W = 0.5), on the undelayed input; e = squared quadrature envelope (ripple-free on steady tones)
//   2. r = one-pole of e; fast F and slow S follow r with their own attack / release (per mode)
//   3. gDb = k c clamp (10 log10 ((F + eps) / (S + eps)), lo, hi) (c = contrast / 100, glides 50 ms; negative c:
//      x neg, plus off |c|), one-pole smoothed, minus m x its slower one-pole (the dip after an onset lift)
//   4. the output reads each band's gain D ms ahead of its content (lookahead inside the latency N)
//   5. g_b = 10^((gDb_b + mDb) / 20), mDb = gain + autoGainDb (both smoothed 50 ms)
// The per-mode constants are fitted to renders of the reference (LowContrast.cpp). Mode switches glide 50 ms between
// both modes' gains (both always run). Auto gain: region power before / after the contrast gains (one-pole 2 s),
// autoGainDb = clamp (10 log10 (Pin / Pout), -12, +12), frozen below -70 dBFS, glides 1 s.
// Neutral: contrast 0 and gain 0 (the defaults) -> the input delayed by N, bit-exact.
// Monitors: soloRegion = sum_b xb_b (the unprocessed region, delayed by N; no dry path). Delta: the slot's.
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
