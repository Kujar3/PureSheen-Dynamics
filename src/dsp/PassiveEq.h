#pragma once

#include "dsp/Module.h"

#include <memory>

// Passive EQ: a passive LC program EQ (top section) above a passive midrange EQ (bottom section), followed by a tube
// make-up stage (Stereo, M/S, L/R, T/S). Owner: mod-peq. Implementation: PassiveEq.cpp. Tests: tests/PassiveEqTests.cpp,
// prefix "peq.". Every coefficient below named k... lives in ONE constant block at the top of PassiveEq.cpp, fitted to
// the reference's renders (the measured model and its laws are described there). Every amount is in dB as labelled
// (0..10), but the actual gains follow the measured laws (low boost 5 = +8.7 dB at DC, high boost 5 = +3.3 dB peak).
//
// Per channel (Stereo: both channels with set A; M/S, L/R: ch0 with set A, ch1 with set B; T/S twin: set B), in this
// order, each an analog (s-domain) prototype realised as a biquad (the low sections bilinear, the others the matched
// designs of design:: in Filters.h: magnitude matched up to Nyquist, no bilinear cramping), double precision, through
// SmoothedBiquad:
//   1. low boost   first-order low shelf 1 + cB / (F + pB) at position kPassiveEqLowHz[lowHz] (the input plus a
//                  low-passed copy; the amount moves the pole); with the low cut on, this section is the boost
//                  relative to the cut, including the measured interaction of the two (the boost with a dip above it)
//   2. low cut     first-order low shelf cut 1 - cC / (F + pC)
//   3. high boost  analog peak at the position's measured centre (the 8k position peaks near 6.2 kHz), its gain, Q and
//                  centre following the amount and High Q (0 = broad, 10 = narrow and higher)
//   4. high cut    first-order high shelf cut at kPassiveEqHighCutHz[highCutHz], times the high boost's measured
//                  upper skirt (a first-order shelf tied to the boost; this section is unity only when both are 0).
//                  Its two first-order factors glide independently, keeping the other shelf's realisation unchanged.
//   5. low-mid     peak boost at kPassiveEqLowMidHz[lmHz] x a measured ratio (300 -> 364 Hz at 5)
//   6. mid cut     peak cut at kPassiveEqMidHz[midHz]
//   7. high-mid    peak boost at kPassiveEqHighMidHz[hmHz] x a measured ratio (3k -> 3.68 kHz)
//                  5 - 7: the boosts widen as the amount rises, the cut narrows (measured at 5 and 10)
//   A section at amount 0 is exactly unity (numerator = denominator: the SVF's output weights are exactly 1, 0, 0), so
//   it adds nothing to the path; its state keeps running, so engaging it starts from a warm state.
//   Centres above 0.49 x sampleRate are clamped there (design::); the reference's own roll-off above 17 kHz (at 44.1 kHz)
//   is not modelled.
//   8. tube make-up stage (our addition: the reference has none, it is fully linear): 2x oversampled (Oversampler,
//      MinimumPhase), a soft asymmetric saturator, mostly 2nd harmonic,
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
