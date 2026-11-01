#pragma once

#include "dsp/Module.h"

#include <memory>

// Tone Balance: an adaptive EQ that keeps the music's long-term tonal balance near a target curve (channel modes
// Stereo, M/S, T/S). Owner: dsp-tone. Implementation: ToneBalance.cpp, ToneTargets.cpp (on ps::SpectralEngine,
// base size 2048). Tests: tests/ToneBalanceTests.cpp, prefix "tbal.".
//
// Engine: residual STFT (SpectralEngine.h), N = 2048 at 44.1 / 48 kHz (x2 at 88.2 / 96, x4 above), hop N / 4;
// latency N (42.7 ms at 48 kHz), constant. Band grid: 1/12 octave from 20 Hz to min (20 kHz, 0.45 fs), at least 2
// bins per band (narrower low bands merged). Per hop, per lane:
//   1. L (b) = octave-density band level (Stereo / T/S: power sum of L + R minus 3 dB), gated (below -80 dBFS density
//      or more than 60 dB under the loudest band: hold, no detection)
//   2. time smoothing (one-pole on band power, per-hop coefficient): Shape / Boost tauS = 4000 ms x (60 / 4000)^(speed / 100)
//      (0 = 4 s, 50 = 490 ms, 100 = 60 ms) -> S (b); Cut: fast F (b) with tauC = 400 ms x (5 / 400)^(cutSpeed / 100)
//      plus S (b) at 1.5 s. Tame Transients: bands with L - S > 6 dB (Cut: L - F) attack at once (Cut: the cut lands
//      in the onset's frame); in Shape boosts are frozen for 3 hops in flagged bands
//   3. target T (b) = toneTargetDb (target, f_b); level offset o = octave-width weighted mean of S - T over
//      100 Hz .. 5.6 kHz (gated bands excluded); deviation D = T + o - S (positive = needs a boost)
//   4. Shape: C = 0.8 D with a 0.5 dB soft dead band (the reference corrects 80 % of the deviation at Amount 100).
//      Boost: C = max (0, (T + o - tol) - S). Cut: reference R = T + o + (tol + 10 dB) (1 - sens / 100)^1.5,
//      excess E = F - R, C = -0.8 softknee (E, 3 dB), cuts only
//   5. frequency smoothing (Shape / Boost): Gaussian over log2 f, sigma = 0.08 x (1.5 / 0.08)^(smooth / 100) octaves;
//      Cut: [0.25 0.5 0.25] across bands
//   6. region amounts: C x mix of low / mid / high % with 1/3-octave raised-cosine crossfades at 100 Hz and 5.6 kHz
//   7. Shape only: power neutral (unweighted, as measured), C -= 10 log10 (sum P 10^(C / 10) / sum P)
//   8. C = amount / 100 x clamp (C, -12, +9) (at most +9 dB boost); no boost in bands more than 40 dB under the
//      frame's band maximum
//   9. ballistics on the final curve: boosts rise at most 1 dB per hop; release toward 0 with max (2 hops, tauS / 2)
//      (Cut: tauC); cut attacks per hop. Mode / target changes glide the final curve (80 ms one-pole for 300 ms)
//  10. band -> bin gains (linear in log f between band centres), residual synthesis
// Stereo / T/S: one curve for both channels (power-sum detector). M/S: Mid with set A, Side with set B.
// Neutral: Amount 0 (the default) -> exact null against the input delayed by N.
// Monitors: none of its own (Delta and channel solo are the slot's).
// T/S: the twin (getParamSet() == 1) reads set B for both channels (Module.h).
// (dsp-tone) Implementation notes (details at the top of ToneBalance.cpp): steps 2 and 4 and the targets follow
// measurements of the reference (noise renders per target); step 7 is solved on the amount-scaled, region-weighted,
// limited curve (neutral at every Amount; a region at 0 % gets no shift either); step 8 limits scale with Amount
// (+9 x Amount / 100 dB at most); boosts also fade out in bands that are gated.
// Silent frames hold detector history while control ramps and gain releases continue, so edits during a pause
// have settled when audio resumes.
// Measured behaviour (tests/ToneBalanceTests.cpp, 48 kHz, Amount 100, defaults unless noted):
//   - Shape, Flat, pink noise with a +6 dB one-octave bell at 2 kHz: the output bump is <= 3 dB at Smoothing 50
//     (2.8 measured) and <= 2 dB at Smoothing 0 (1.8); the correction reaches 63 % of its final value 0.5 .. 1.5 tauS
//     after the bump appears (0.69 s at tauS 0.49 s).
//   - Speed: 63 % time of a level step 4.6 s at Speed 0, 0.53 s at 50, 75 ms at 100.
//   - Shape is power neutral within 0.5 dB (unweighted output power; K-weighted loudness falls where highs are cut,
//     like the reference). The solve uses each bin's interpolated dB gain before conversion to power.
//   - Flat target on pink noise: every 1/3-octave band within +-0.75 dB (0.35).
//   - Cut, Sensitivity 100: a steady sine 12 dB over its 1/12-octave band is cut >= 5 dB (5.4), an octave away < 1 dB;
//     Sensitivity 50: >= 2 dB (2.8); Sensitivity 0: a peak is left alone until it is tol + 10 dB over the target.
//   - Smoothing 0: a +10 dB 1/6-octave bump gets a >= 4 dB correction <= 0.5 octave wide at half depth (4.2, 0.37);
//     Smoothing 100: >= 2 octaves wide (2.2).
//   - Tame Transients (Cut, Cut Speed 30): >= 70 % of a hit's cut is in place in the first hop whose frame centre has
//     passed the onset (0.75; 0.12 without Tame).
//   - Steady pink, every mode: gain std at the display points < 0.5 dB over 2 s (0.36), output spectral flux <= 1.1 x
//     the input's (1.01). CPU: ~170x realtime (Stereo), ~100x (M/S with Smoothing automated every block).
namespace ps
{
// Display grid of the meters: kToneDisplayPoints log-spaced points 20 Hz .. 20 kHz.
inline constexpr int kToneDisplayPoints = 64;
double toneBalanceDisplayHz (int index) noexcept;

// Target curve of a target in dB relative to pink noise (octave density, 1 kHz = 0 dB) at hz; *tolDb receives the
// tolerance half-width there (null allowed). Our own curves (7 numbers per target: sub 30 Hz, bass 80 Hz, low-mid
// 300 Hz, presence extra at 3 kHz, slope above 1 kHz, air extra at 14 kHz, tolerance), monotone cubic in log2 f.
// The UI draws the corridor with it; the phase 3 Assistant reuses it.
double toneTargetDb (ToneTarget, double hz, double* tolDb = nullptr) noexcept;

struct ToneBalanceMeters
{
    AtomicFloat gainDb[kNumChannelSets][kToneDisplayPoints];    // current correction at the display points
    AtomicFloat levelDb[kNumChannelSets][kToneDisplayPoints];   // S: the smoothed, octave-normalised measurement
                                                                // (Cut: F, which the cut compares with T + o + margin)
    AtomicFloat targetOffsetDb[kNumChannelSets];                // o (the target floats with the program level)
    AtomicFloat maxBoostDb[kNumChannelSets], maxCutDb[kNumChannelSets];   // of the current curve: boost >= 0, cut <= 0
};                                                              // Stereo / T/S: set B mirrors set A

class ToneBalance final : public Module
{
public:
    ToneBalance();
    ~ToneBalance() override;

    ModuleType getType() const noexcept override { return ModuleType::ToneBalance; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const ToneBalanceMeters& getMeters() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
