#pragma once

#include "dsp/Module.h"

#include <complex>
#include <memory>

// Dynamic EQ (6 dynamic bands per channel set; channel modes Stereo, M/S, L/R, T/S). Owner: dsp-deq.
// Implementation: DynamicEq.cpp (+ more files if needed). Tests: tests/DynamicEqTests.cpp, prefix "deq.".
//
// Band = an Equalizer band (same design code: eqdesign::designAnalog / eqBandResponse with dynEqToEqShape, so a static
// band measures exactly like the Equalizer) whose gain moves with a detector level:
//   L      detector level (dB): RMS of the sidechain signal through the band's detector filter (below)
//   x      = L - thr;  k = 1 - 1 / ratio;  K = knee
//   c(x)   = 0 (x <= -K/2),  k (x + K/2)^2 / (2K) (|x| < K/2),  k x (x >= K/2)        movement in dB
//   t      = min (c, |G|) / |G|  (0 when |G| < 0.01 dB)                                trigger 0..1, G = band gain
//   Down, G < 0: g = G t          (0 dB at rest, cuts toward G: de-esser)
//   Down, G > 0: g = G (1 - t)    (boost at rest, backs off when loud)
//   Up,   G > 0: g = G t          (0 dB at rest, boosts when loud)
//   Up,   G < 0: g = G (1 - t)    (full cut at rest, opens when loud)
//   band gain = clamp (offset + g, -30, +15) dB;   dynamic == false: offset + G (static, no detector)
// Ballistics act on the movement in dB (double one-pole: attack while it rises, release while it falls; time = time to
// 1 - 1/e). Auto Scale: effective time = set time x clamp (sqrt (1000 / f), 0.25, 4).
// Detector (identical in Analog and Digital mode), on the module INPUT (feed-forward, pre-EQ):
//   Bell / Proportional Q / Band Shelf: 4th-order band-pass (two 2nd-order band-pass sections at freq, Q' = 0.644 Qd,
//   Qd = the band's Q, Proportional Q: its effective Q at the band's gain, clamped 0.3..8); low shelves: LR4 low-pass at
//   freq; high shelves: LR4 high-pass. Mean square one-pole tau = clamp (2 / f, 1 ms, 50 ms), 10 log10. Calibration: a
//   sine at the band centre with RMS R dBFS reads R. Stereo / T/S: one level per band = max of both channels' mean
//   squares, one gain for both channels; M/S, L/R: per channel, set A on ch0, set B on ch1.
//   Sidechain (DynEqSidechain): Own, "Band n" = the detector of band n of the same set (its filter settings, even when
//   band n is off), OtherChannel = this band's detector filter on the other channel (M/S, L/R; Own otherwise).
// Analog mode: zero latency (+ lookahead): per band a matched SVF cascade redesigned at a control rate (32 samples at
//   <= 50 kHz, 64 at <= 100 kHz, 128 above), parameters interpolated per sample in between; frequency / Q smoothed
//   (5 ms); at most 16 section designs per tick (round robin). Off bands are skipped; a band at exactly 0 dB is exact
//   unity. On / off and shape changes crossfade that band over 15 ms.
// Digital mode: linear phase. N = firSize (x2 at 88.2/96 kHz, x4 above), D = N / 2, partition P = N / 16,
//   Dt = D + P. Let la = round (lookaheadMs * fs / 1000), xLa (n) = x (n - la). Then
//   y (n) = x (n - Dt - la) + sum_b c_b (n) (w_b * xLa)(n), including the convolver's partition delay P,
//   c_b (n) = 10^(gain_b (n - Dt) / 20) - 1. The gain stream is delayed by Dt, so it leads its corresponding audio
//   by exactly la (as in Analog mode); do not subtract la from the gain delay as well.
//   w_b is the band's unit kernel, designed on the module's worker thread at its extreme gain g_ref:
//   W_b = (|H_b,ref| - 1) / (10^(g_ref / 20) - 1), with zero kernel when g_ref is zero. Mode / FIR size switches
//   fade out 4 ms, select already-prepared buffers / convolvers, then fade in (never allocate in setParams/process).
// Lookahead (0..10 ms): the audio path is delayed, the detector is not. Latency = la (Analog) or Dt + la (Digital);
//   constant while only band parameters move. Lookahead changes crossfade delay taps over 10 ms and update latency
//   to the new whole-sample delay; latency may also change on mode / FIR size switches.
// Neutral: every band off (the default) -> bit-exact (Analog) / exact delay (Digital); a band at gain 0, offset 0 too.
// Monitors: soloBand = that band's sidechain signal (what the band listens to), delayed by the module latency, 10 ms
//   crossfade; audition (auditionOn / auditionHz / auditionQ / auditionChannelSet) = band-pass audition of the input as
//   in the Equalizer. Delta: the slot's (no handlesDelta()).
// T/S: the twin (getParamSet() == 1) reads set B for both channels (Module.h).
namespace ps
{
struct DynamicEqMeters
{
    AtomicFloat bandGainDb[kNumChannelSets][kDynEqBands];   // current total band gain (offset + dynamic), end of block
    AtomicFloat dynamicDb[kNumChannelSets][kDynEqBands];    // current dynamic part g (signed)
    AtomicFloat detectorDb[kNumChannelSets][kDynEqBands];   // detector level, max over the block (threshold meter)
};                                                          // Stereo / T/S: set B mirrors set A

// Static response of one band at a given band gain (the Equalizer's shape, dynEqToEqShape). The UI draws the rest
// curve (gainDb = the band's rest gain), the live curve (the meter's bandGainDb) and the trigger range with it.
std::complex<double> dynEqBandResponse (const DynamicEqBandParams&, double bandGainDb, double freqHz,
                                        double sampleRate) noexcept;

// Rest gain (no trigger) and full-trigger gain of a band in dB, including the offset and the clamp (gain law above).
double dynEqRestGainDb (const DynamicEqBandParams&) noexcept;
double dynEqTriggeredGainDb (const DynamicEqBandParams&) noexcept;

class DynamicEq final : public Module
{
public:
    DynamicEq();
    ~DynamicEq() override;

    ModuleType getType() const noexcept override { return ModuleType::DynamicEq; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const DynamicEqMeters& getMeters() const noexcept;
    bool isDesignPending() const noexcept;      // Digital: band kernels still being designed / installed

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
