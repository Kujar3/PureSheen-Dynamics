#pragma once

#include "dsp/Module.h"

#include <memory>

// Classic Compressor: a feedback compressor with a detection filter in the loop, a program-dependent (two-capacitor)
// detector and three knee modes (Stereo, M/S). Owner: mod-ccmp. Implementation: ClassicComp.cpp. Tests:
// tests/ClassicCompTests.cpp, prefix "ccmp.". Every coefficient below named k... lives in ONE constant block at the top
// of ClassicComp.cpp; the model and the constants are fitted to the reference's renders (ClassicComp.cpp says what
// the measurements decided).
//
// Per channel set s (Stereo: both channels with set A, linked; M/S: ch0 = Mid with set A, ch1 = Side with set B, each
// set alone), per sample n:
//   x[n] (input) -> gain stage: v = x g[n] -> auto gain -> makeup -> y[n]
//   detector (feedback): d = sidechain EQ (mean of the set's v), the gain-stage output, read at the next sample (one
//     sample of loop delay, no extra latency; Stereo: (L + R) / 2, one gain for both channels). As in the reference,
//     a tone at or below the high-pass corner at ratio >= 10 and attack <= 5 ms modulates the gain non-periodically
//     (bounded; ClassicComp.cpp).
//   sidechain EQ (per set, double precision, bilinear designs at the knob frequencies): 1st-order high-pass at sc.hpf,
//     a wide bell (Q kBellQ) with kBellGainScale x sc.bellDb, a high shelf (Q kShelfQ) with kShelfGainScale x
//     sc.shelfDb. At sc.hpf 20 Hz the high-pass is still in the path (the detector never sees DC).
//   detector: a diode peak detector V1 (charges toward |d| while |d| > V1 with kChargePerAttack x attack; discharges
//     toward 0 at all times with the release law) coupled to a slow capacitor V2 (time and load ratio from the
//     release laws): a short hit releases fast, held reduction creeps deeper and then releases slower. Level
//     L = 20 log10 (V1) in dBFS (a steady sine reads a little below its peak: the charge / discharge balance).
//   gain computer on L (the OUTPUT level): c = ratio - 1, so a steady tone's static slope above the knee is 1 / ratio
//     exactly; Sharp / Balanced: quadratic knee kKneeDb[mode] wide centred on the threshold T; Smooth: softplus knee
//     c W ln (1 + e^((L - T - kSmoothThresholdDb) / W)). The charge step is capped at the loop's dead-beat value
//     1 / (1 + local slope), a local linear bound; nonlinear onsets can still overshoot.
//   auto gain (autoGain): 20 log10 (A / B), A / B = kAutoGainMs one-pole averages of |x| and |v| (mean over the set's
//     channels), clamped 0..kAutoGainMaxDb; in silence both decay alike, so it holds. Measured on the gain-stage
//     output, before makeup, so Makeup stays a deliberate offset on top.
//   No saturator: the odd harmonics while compressing are the detector ripple modulating the gain.
// Latency 0 for every parameter value.
// Neutral: ratio 1, makeup 0, auto gain off (any mode, any sidechain EQ): output = input (bit-exact).
// Parameter changes: threshold and ratio glide S-shaped over 100 ms (the feedback loop's detector cannot follow a
// faster jump without dipping the gain); knee width, the Smooth share (a mode change crossfades the knees), makeup
// and the auto-gain share over 20 ms; the sidechain EQ through SmoothedBiquad; attack / release in log time per
// sample. The Chain fades to dry and resets the module when switching Stereo / M/S (Module.h / Chain.cpp), since
// detector histories belong to the channel domain in which they were recorded.
// Monitors: ModuleMonitor::soloDetectionFilter = hear the sidechain: the output is the module INPUT through the
// sidechain EQ (per channel, in the channel domain; 20 ms crossfade in and out), as the Dynamics module does.
// T/S: not offered (no twin).
namespace ps
{
struct ClassicCompMeters
{
    // Per channel set (index 0 = set A; set B is used in M/S only and rests at its idle value in Stereo).
    AtomicFloat gainReductionDb[kNumChannelSets];          // current reduction (<= 0)
    AtomicFloat gainReductionHoldDb[kNumChannelSets];      // deepest reduction of the last 1 s (<= 0)
    AtomicFloat sidechainDb[kNumChannelSets] { AtomicFloat { -100.0f }, AtomicFloat { -100.0f } };   // detector level L (dBFS)
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
    // analog prototypes of the bilinear designs; independent of the sample rate). Any thread.
    static double detectorResponseDb (const ClassicCompSetParams&, double hz) noexcept;

    // For the panel's transfer display: approximate static output (dBFS) of a steady tone at `inputDb` for one set's
    // threshold / ratio / mode / attack / release (the knee as above, the detector's steady-sine reading and the
    // feedback fixed point solved; no makeup, no auto gain). Assumes flat sidechain magnitude and small detector
    // ripple; finite-rate charge capping and low-frequency ripple are not represented in this rate-independent curve.
    static double staticCurveDb (const ClassicCompSetParams&, double inputDb) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
