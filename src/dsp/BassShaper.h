#pragma once

#include "dsp/Module.h"

#include <memory>

// Bass Shaper: low-end balance, punch, sustain power and peak control below a cutoff (Stereo only). Owner: dsp-bass.
// Implementation: BassShaper.cpp (+ Quadrature.h for its detectors). Tests: tests/BassShaperTests.cpp, prefix
// "bass.".
//
//   x -> linear-phase split at the cutoff (low = FIR, high = x delayed - low) -> high (L, R) ---------> delay --+
//                                                       -> low (L, R) x balance gain                           |
//        detectors on the low-band mid m = (L + R) / 2, undelayed:                                              |
//          Pf = QuadratureEnvelope (m) through attack 2 / release 25 ms; Ps = the same through 80 / 300 ms      |
//          r = 10 log10 ((Pf + eps) / (Ps + eps)); w = clamp ((r - 1.5) / 6, 0, 1) smoothed (0.5 / 30 ms)       |
//        low delayed by L1 = round (1 ms fs): transient t = w low, sustain s = (1 - w) low (t + s == low exactly)|
//        punch: t x 10^(punch / 20) (smoothed 20 ms; no threshold)                                              |
//        sustain power (sus.on): Ls = level of (1 - w) m (attack 10 / release 600 ms); lift = clamp ((thr - Ls) x |
//          amount / 100, 0, 12) dB, faded out between thr - 36 and thr - 48 dB (no noise lift), falls fast (10 ms),|
//          rises slowly (1 s); s x 10^(lift / 20). Character < 100: harmonics h = A d f (s (u) - u), an odd,       |
//          compressive curve s (u) = k u / (1 + |k u|^p)^(1/p) / s (1) (k 2.85, p 1.85), d = (1 - char / 100)^2 x |
//          amount / 100, f = 10^(-0.31 (Ls - thr)^2 / 20) above the threshold (1 below), u = lifted s / A,       |
//          A = 10^(thr / 20), high-passed at 20 Hz, low-passed at 8 x cutoff, added to the shaped low part BEFORE  |
//          peak control (all measured: the reference's tone-test renders)                                      |
//          (dsp-bass: A is in fact A_eff = max (A, held peak of the lifted sustain: 30 ms hold, 300 ms release), so |
//          |u| <= 1 for a sustain above the threshold; at or below the threshold this is the formula above.        |
//          Pf's input is max (QuadratureEnvelope (m), m^2) (Ps keeps the quadrature power): the all-pass pair lags  |
//          ~10 ms at 55 Hz; m^2 never exceeds a steady tone's squared envelope but rises within a quarter cycle)   |
//        peak control (peak.on), lookahead L2 = round (1.5 ms fs): Clean = brickwall lookahead limiter on        |
//          max (|L|, |R|) of the low part INCLUDING h (hold over L2, 50 ms release, box <= L2: <= threshold);    |
//          Dirty = 4x minimum-phase oversampled clipper with a 0.5 dB knee; out = char x clean + (1 - char) x      |
//          dirty, then mix with the aligned pre-peak signal; off: the low part delayed by L2 + OS latency          |
//   y = high delayed + low out (harmonics already included and delayed by the peak stage), then auto gain
//   (input vs output RMS, 3 s; clamped +-12 dB, frozen below
//   -60 dBFS, glides 1 s)
// Latency D = split (FIR half-length + convolver partition: 2304 samples at 44.1 / 48 kHz, x2 at 88.2 / 96) + L1 + L2
// + oversampler latency, constant for every parameter value (sections off still delay).
// Neutral switch: when balance == 0, punch == 0 and both sections are off (after the smoothing settles) the output is
// the input delayed by D exactly; the split is complementary, so the switch is a plain 20 ms crossfade.
// The return to neutral waits for 250 ms of settled neutral controls.
// Stereo: bands stay stereo; mask and sustain gains from the mid (linked); the limiter from the channel maximum; the
// clipper per channel. Meters work while every section is off (the UI's sweet-spot meters can be used alone).
// Monitors: none of its own. Delta: the slot's.
namespace ps
{
struct BassShaperMeters
{
    AtomicFloat balanceDb { -60.0f };      // 10 log10 (P_low / P_total) of the module INPUT, one-pole 3 s
    AtomicFloat crestDb;                   // low-band crest factor of the input: peak (1 s decaying hold) - RMS (1 s)
    AtomicFloat balanceOutDb { -60.0f };   // the same on the output (the UI's hollow ball)
    AtomicFloat crestOutDb;
    AtomicFloat transientShare;            // mean w over 300 ms (0..1)
    AtomicFloat sustainLiftDb;             // current sustain lift (>= 0)
    AtomicFloat peakReductionDb;           // current clean-branch reduction (<= 0)
    AtomicFloat lowPeakDb { -200.0f };     // low-band peak since the UI last exchanged it
};

// Sweet-spot target zones of the meters (our own, to calibrate on our own reference material): balance centre
// -4 dB + 3 dB x log2 (cutoff / 120), +-3 dB; crest 8..13 dB.
double bassShaperBalanceTargetDb (double cutoffHz) noexcept;

class BassShaper final : public Module
{
public:
    BassShaper();
    ~BassShaper() override;

    ModuleType getType() const noexcept override { return ModuleType::BassShaper; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const BassShaperMeters& getMeters() const noexcept;
    BassShaperMeters& getMeters() noexcept;   // (added by dsp-bass) for the UI's exchange() of lowPeakDb
    // Oscilloscope data (single consumer: the panel). Per 16 output samples one pair: L = min, R = max of the
    // low-band output mid. And per 16 samples: L = transient mask w, R = low-band envelope (for colouring).
    StereoSampleFifo& getScopeFifo() noexcept;
    StereoSampleFifo& getMaskFifo() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
