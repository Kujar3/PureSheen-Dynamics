#pragma once

#include "dsp/Module.h"

#include <memory>

// Classic Compressor: a feedback compressor with a detection filter in the loop, program-dependent release and three
// knee / ballistics modes (Stereo, M/S). Owner: mod-ccmp. Implementation: ClassicComp.cpp. Tests:
// tests/ClassicCompTests.cpp, prefix "ccmp.". Every coefficient below named k... lives in ONE constant block at the top
// of ClassicComp.cpp (first-principles values until the reference renders are fitted).
//
// Per channel set s (Stereo: both channels with set A, linked; M/S: ch0 = Mid with set A, ch1 = Side with set B, each
// set alone), per sample n:
//   x[n] (input) -> gain stage: v = x g[n] -> mode saturator (Balanced / Smooth) -> auto gain -> makeup -> y[n]
//   detector (feedback): d = sidechain EQ (v[n - 1]), the gain-stage output of the PREVIOUS sample (before the mode
//     saturator, auto gain and makeup, so none of them changes the compression); one sample of loop delay, no extra
//     latency. The level followers are gain-referred (they average d / g and are read times the latest g: exactly the
//     output level at a steady gain, never a stale pre-reduction level while the gain moves; ClassicComp.cpp).
//   sidechain EQ (per channel, double precision, matched designs of Filters.h): 12 dB/oct high-pass at sc.hpf
//     (Butterworth Q 0.707), a bell at sc.bellHz / sc.bellDb with Q 0.7, a high shelf at sc.shelfHz / sc.shelfDb
//     (Q 0.707). At sc.hpf 20 Hz the high-pass is still in the path (the detector never sees DC).
//   level: Stereo = max over |L|, |R| of the filtered detector (linked: one gain for both channels); M/S = each channel
//     on its own. L = kDetRmsMix x RMS (one-pole mean square, kDetRmsMs, read as sqrt (2 x mean square) so a sine
//     reads its peak) + (1 - kDetRmsMix) x peak (instant |d| with a kDetPeakReleaseMs release), in dB: levels,
//     threshold and the static curve are in sine-peak dBFS.
//   gain computer on L (the OUTPUT level): knee width W = kKneeDb[mode] (Sharp 2, Balanced 6, Smooth 12 dB) centred
//     on the threshold T. Feedback ratio mapping (closed form): a reduction r = -c (L - T) above the knee settles at
//     the fixed point y - T = (x - T) / (1 + c) for a steady tone, so c = ratio - 1 gives the knob's ratio exactly
//     above the knee; inside the knee r = -c (L - T + W / 2)^2 / (2 W); below it r = 0. ratio 1 -> c = 0 -> r = 0.
//   ballistics on r (dB): attack one-pole, time attackMs x kAttackScale[mode], while the target reduction deepens
//     (the closed-loop time: the coefficient is divided by 1 + the gain computer's local slope, which also keeps the
//     one-sample loop stable at every ratio); program-dependent release while it recovers: time = (1 - h) x release +
//     h x kSlowReleaseMul x release, where h (0..1) is the "held" share: a one-pole toward 1 (kHoldTauMs, 300 ms)
//     while the gain computer's target is deeper than kHoldMinDb, toward 0 (kHoldDecayMs) otherwise. So a short hit
//     recovers at the release knob, a long stretch of compression up to kSlowReleaseMul times slower.
//   mode saturator: Sharp none; Balanced kBalancedDrive, Smooth kSmoothDrive. A low-order (<= 3rd order) odd + even
//     polynomial whose drive is scaled by the current reduction depth (|r| / kSatFullDb, clamped to 1), so it is
//     exactly linear while nothing is reduced, and exactly linear below -30 dBFS at any reduction, once the 5 Hz DC
//     tracker's residual from earlier saturation has decayed. Not oversampled (latency 0); at full drive a 0 dBFS
//     gain-stage sine has harmonics at least 40 dB below the fundamental. This is a level-specific bound, not an
//     antialias filter.
//   auto gain (autoGain): input vs saturator output mean squares over 3 s, clamped +-12 dB, frozen while the input is
//     below -60 dBFS, glides 1 s (as the Bass Shaper's); measured before makeup, so Makeup stays a deliberate offset on top.
// Latency 0 for every parameter value.
// Neutral: ratio 1, makeup 0, auto gain off (any mode, any sidechain EQ): output = input (error < -120 dB; bit-exact
// is not required).
// Parameter changes: threshold, makeup and the sidechain EQ glide over 20 ms (SmoothedBiquad for the EQ); ratio and
// mode are smoothed through the knee / drive values; attack / release glide in log time per sample. A mode change
// never clicks. The Chain fades to dry and resets the module when switching Stereo / M/S (Module.h / Chain.cpp),
// since detector histories belong to the channel domain in which they were recorded.
// Monitors: ModuleMonitor::soloDetectionFilter = hear the sidechain: the output is the module INPUT through the
// sidechain EQ (per channel, in the channel domain; 20 ms crossfade in and out), as the Dynamics module does.
// T/S: not offered (no twin).
namespace ps
{
struct ClassicCompMeters
{
    // Per channel set (index 0 = set A; set B is used in M/S only and rests at its idle value in Stereo).
    AtomicFloat gainReductionDb[kNumChannelSets];          // current reduction r (<= 0)
    AtomicFloat gainReductionHoldDb[kNumChannelSets];      // deepest reduction of the last 1 s (<= 0)
    AtomicFloat sidechainDb[kNumChannelSets] { AtomicFloat { -100.0f }, AtomicFloat { -100.0f } };   // level L (dBFS)
    AtomicFloat autoGainDb[kNumChannelSets];               // current auto-gain correction (0 when off)
};

class ClassicComp final : public Module
{
public:
    ClassicComp();
    ~ClassicComp() override;

    ModuleType getType() const noexcept override { return ModuleType::ClassicComp; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const ClassicCompMeters& getMeters() const noexcept;

    // For the panel's sidechain curve: magnitude (dB) of the detection filter of one set's settings at `hz` (the
    // analog prototype the matched designs follow; independent of the sample rate). Any thread.
    static double detectorResponseDb (const ClassicCompSetParams&, double hz) noexcept;

    // For the panel's transfer display: the static output level (dBFS) of a steady tone at `inputDb` for one set's
    // threshold / ratio / mode (the knee as above, the feedback fixed point solved; no makeup, no auto gain).
    static double staticCurveDb (const ClassicCompSetParams&, double inputDb) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
