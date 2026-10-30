#pragma once

#include "dsp/Module.h"

#include <memory>

// Punch: multiband micro-dynamics (channel modes Stereo, M/S). Each band compares a short-term and a long-term
// envelope of itself and scales that ratio in dB: positive amounts make the hits stand out more (punchier), negative
// amounts flatten them (denser). No threshold: the result does not depend on the level. Owner: dsp-punch.
// Implementation: Punch.cpp (+ Quadrature.cpp). Tests: tests/PunchTests.cpp, prefix "pnch.".
//
//   in (2 ch, module domain) -> ps::Crossover (bands, xover, type) -> per band b, channel c:
//     detector d = stereo link (Stereo mode) ? the band's mid (L + R) / 2 : the band channel itself
//     P   = max (QuadratureEnvelope (d), d^2 / 4) (ripple-free power; the quarter of the instantaneous power only
//           takes over on fast onsets, where the all-pass chains' group delay lags the quadrature power by > 6 dB)
//     As  = one-pole (P; attack tau_a, release tau_r), Al = one-pole (P; tau_l)          (double)
//     rel = 10 log10 ((As + 1e-9) / (Al + 1e-9)), clamped to [-40, +24] dB
//     k   = band amount / 100 x amount / 100   (20 ms linear smoothing per sample)
//     g   = clamp (k x rel, -24, +12) dB;  y = x_band x 10^(g / 20) x makeup
//     band off: y = x_band (still from the crossover, so the band sum stays the crossover's sum)
//   -> sum of the bands
// Ballistics from the envelope E (ms; with sync: kPunchSyncFractions[syncNote] x 4 x 60000 / bpm, clamped to
// 10..1000 ms, bpm <= 0 or not finite -> 120): tau_l = E, tau_r = max (1 ms, E / 10), tau_a = clamp (E / 100, 0.2,
// 2) ms. Envelope / sync changes recompute the coefficients (no reset).
// Auto gain (per band and channel; linked in linked Stereo): Pin = one-pole (3 s) of x_band^2, Pout of
// (y / makeup)^2, target makeup = clamp (10 log10 (Pin / Pout), -12, +12) dB, frozen while Pin < 1e-7, makeup follows
// with a 500 ms one-pole; off: glides to 0 dB.
// Stereo, link off: each channel its own detector, both use set A; link on: one gain for both from the mid. M/S:
// ch0 = Mid with set A, ch1 = Side with set B, unlinked.
// Latency = the crossover's (Analog / Hybrid 0, Digital fixed per rate), reported from the moment of a type switch.
// Neutral: 1 band (the default) with every amount 0 -> bit-exact passthrough; N bands Analog -> the crossover's
// all-pass sum; Digital -> the input delayed.
// Monitors: soloBand = that band only (processed). Learn (crossovers) is the processor's learnCrossovers. Delta: the
// slot's.
// (dsp-punch) Structural changes (prepare / reset (), band count, crossover type, channel mode) "settle": the effect
// fades out (10 ms; a band-count change reaches the crossover only then), the long envelopes are held equal to the
// short ones for 30 ms (rel = 0 while the detectors warm up), then the effect fades in (20 ms). Band on / off ramps
// over 20 ms, band solo over 10 ms, Stereo Link crossfades the complete gains (including make-up) over 20 ms.
// Band-count changes apply at the exact sample boundary after the fade; while soloing, the monitor also fades out
// for 10 ms before band membership changes and back in for 10 ms afterwards. reset () keeps the
// learned auto-gain make-up. Meters in Stereo: unlinked, gainDb is the channel with the larger |gain| and the micro
// meters the larger of the two channels; the gain meters show the effective gain (0 dB for a band that is off).
// Measured (tests/PunchTests.cpp): step -30 -> -10 dBFS at amount +50, E = 100 ms: peak +7.6 dB 1.5 ms after the
// step, +1.0 dB at 100 ms, +0.11 dB at 300 ms; steady 40 Hz / 1 kHz sines within 0.002 dB, THD+N -82 / -107 dB at
// E = 10 ms, amount +100. CPU (48 kHz stereo): 1 band ~175x realtime, 4 bands Analog M/S ~94x.
namespace ps
{
struct PunchMeters
{
    AtomicFloat gainDb[kNumChannelSets][kMaxBands];       // current applied gain (incl. makeup)
    AtomicFloat gainMinDb[kNumChannelSets][kMaxBands];    // block extremes since the UI last read them (the UI
    AtomicFloat gainMaxDb[kNumChannelSets][kMaxBands];    //   exchanges with +200 / -200 to draw gain traces)
    AtomicFloat microInDb[kNumChannelSets][kMaxBands];    // std dev of rel (dB) over ~3 s on the input
    AtomicFloat microOutDb[kNumChannelSets][kMaxBands];   // the same measured on the band output
    AtomicFloat makeupDb[kNumChannelSets][kMaxBands];
};                                                        // Stereo: set B mirrors set A

// The envelope time in ms for the current settings (sync note at a host tempo, or envelopeMs), clamped to 10..1000.
double punchEnvelopeMs (const PunchParams&, double bpm) noexcept;

class Punch final : public Module
{
public:
    Punch();
    ~Punch() override;

    ModuleType getType() const noexcept override { return ModuleType::Punch; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;
    int getLatencySamplesForParams (ParamValues) const noexcept override;

    const PunchMeters& getMeters() const noexcept;
    // (added by dsp-punch) Non-const access for the UI: gainMinDb / gainMaxDb are read with exchange (+200 / -200).
    PunchMeters& getMeters() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
