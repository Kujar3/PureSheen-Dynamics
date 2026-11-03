#pragma once

#include "dsp/Module.h"

#include <memory>

// Passive EQ: a passive LC program EQ (top section) above a passive midrange EQ (bottom section), followed by a tube
// make-up stage (Stereo, M/S, L/R, T/S). Owner: mod-peq. Implementation: PassiveEq.cpp. Tests: tests/PassiveEqTests.cpp,
// prefix "peq.". Every coefficient below named k... lives in ONE constant block at the top of PassiveEq.cpp
// (first-principles values until the reference renders are fitted).
//
// Per channel (Stereo: both channels with set A; M/S, L/R: ch0 with set A, ch1 with set B; T/S twin: set B), in this
// order, each an analog (s-domain) prototype realised as a matched biquad (design:: in Filters.h: magnitude matched up
// to Nyquist, no bilinear cramping), double precision, through SmoothedBiquad:
//   1. low boost   resonant low shelf at f = kPassiveEqLowHz[lowHz], gain kLowBoostMaxDb (13.5) x law (lowBoost / 10),
//                  shelf Q kLowBoostQ
//   2. low cut     low shelf cut at f x kLowCutRatio, gain -kLowCutMaxDb (17.5) x law (lowCut / 10), Q kLowCutQ; with
//                  1 and 2 together the response is the classic boost with a dip just above it (they never cancel)
//   3. high boost  peak at kPassiveEqHighHz[highHz], gain kHighBoostMaxDb (16) x law (highBoost / 10);
//                  Q = kHighQBroad (0.4) at highQ 0 .. kHighQSharp (2.5) at highQ 10 (log interpolation), then
//                  narrowed as the boost rises: Q x (1 + kBwBoostCoupling x law (highBoost / 10))
//   4. high cut    high shelf cut at kPassiveEqHighCutHz[highCutHz], gain -kHighCutMaxDb (16) x law (highCut / 10)
//   5. low-mid     peak boost at kPassiveEqLowMidHz[lmHz], up to kMidMaxDb (10) dB
//   6. mid cut     peak cut at kPassiveEqMidHz[midHz], up to -kMidMaxDb dB
//   7. high-mid    peak boost at kPassiveEqHighMidHz[hmHz], up to kMidMaxDb dB
//                  5 - 7 proportional Q: Q = kMidQLow + (kMidQHigh - kMidQLow) x law (amount / 10) (kMidQHigh >
//                  kMidQLow: broad at a touch, narrower as the amount rises)
//   law (u) = the knob law, table kKnobLaw[11] over 0..10 interpolated linearly (identity until fitted); law (0) = 0.
//   A section at amount 0 is exactly unity (numerator = denominator: the SVF's output weights are exactly 1, 0, 0), so
//   it adds nothing to the path; its state keeps running, so engaging it starts from a warm state.
//   Frequencies above 0.45 x sampleRate are clamped there (the matched design keeps the shape below Nyquist).
//   8. tube make-up stage: 2x oversampled (Oversampler, MinimumPhase), a soft asymmetric saturator, mostly 2nd harmonic,
//      scaled by drive (0..10, shared by both sets; kDriveMax sets the curve at 10), level-compensated so a -18 dBFS
//      sine at 1 kHz keeps its level. drive 0: the stage is bypassed and the signal is delayed by an exact integer
//      delay of kPassiveEqLatency samples (bit-exact). The oversampled path carries only the compensated nonlinear
//      residual, added to this same clean delay; crossing 0 fades that correction over 20 ms without combing the clean
//      signal. The residual has the minimum-phase oversampler's frequency-dependent phase.
// Latency: kPassiveEqLatency (3 samples on the clean path, the rounded oversampler latency for the residual) at every
// sample rate, whatever the drive, so it never jumps (reported in the chain; it depends on no parameter).
// Neutral: every amount 0 and drive 0 (the defaults): output = input delayed by kPassiveEqLatency exactly.
// Parameter changes: designs are recomputed only when a set's values change and glide over 10 ms (SmoothedBiquad); a
// frequency step (choice) glides the same way, no click. The first parameters after prepare/reset snap, including Drive.
// T/S twin (Module.h rules): with getParamSet() == 1 setParams copies set B over set A; the latency is shared.
// Meters: none of its own (the slot's difference meter only).
namespace ps
{
inline constexpr int kPassiveEqLatency = 3;   // samples at every sample rate (2x minimum-phase oversampler)

class PassiveEq final : public Module
{
public:
    PassiveEq();
    ~PassiveEq() override;

    ModuleType getType() const noexcept override { return ModuleType::PassiveEq; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    // The combined response (dB) of sections 1 - 7 of channel set `set` (0 = A, 1 = B) at `hz`, from the designs the
    // audio thread last computed in setParams (target coefficients, not the glide state). Every instance designs BOTH
    // sets from the parameter array whatever the channel mode, so the panel queries instance A (Chain::getModule
    // (type, 0)) for both sets; a twin answers set B for both. Thread-safe: the audio thread publishes the coefficients
    // through a seqlock (it never waits; the reader retries while a write is in progress), any thread may call this.
    // 0 dB before the first setParams.
    double getMagnitudeDb (int set, double hz) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
