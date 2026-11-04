#pragma once

#include "dsp/Module.h"

#include <memory>

// Tape: a two-head tape machine (record path, tape, reproduce head), Stereo only. Owner: mod-tape. Implementation:
// Tape.cpp. Tests: tests/TapeTests.cpp, prefix "tape.". Every coefficient named k... lives in ONE constant block at
// the top of Tape.cpp, fitted to the reference's renders (out/vintage/fit-tape.md; tools out/vintage/tools/fit_tape*.py).
//
// Per channel (L and R processed alike, unlinked), 4x oversampled (Oversampler, MinimumPhase) up to the downsampler:
//   x -> bias tilt (record side; bias 0 = flat): a high shelf at ~1.2 kHz and a low shelf, measured per bias
//     -> record pre-emphasis: a 2nd-order high shelf of +1.58 dB centred 1.32 kHz x (speed / 30 ips): the highs reach
//          the curve hotter above a corner that moves with the speed
//     -> drive (driveDb) and the bias drive (bias drives the curve harder on both sides of 0: +5.95 dB at -10, +15.87 dB
//          at +10, uncompensated)
//     -> tape curve: memoryless (no hysteresis was found), the quadratic soft clipper f (u) = u - u|u| / 4 (flat
//          beyond |u| = 2); harmonics (0..10) shifts its operating point (f (u + b) - f (b): second harmonic alone at
//          low levels), divided by the same input gain
//     -> de-emphasis (the exact inverse of the pre-emphasis) and the speed's reproduce bells (mid, broad top: the
//          gap / spacing loss at the slow speeds)
//     -> high emphasis: a 1st-order high shelf, highEmph 4 = exactly flat, 0 = -4.96 dB / 10 = +4.96 dB shelves
//     -> top roll-off (-0.6 dB at 19 kHz, -2 dB at 20 kHz at 44.1 kHz, scaled with the sample rate)
//   then at the base rate: the speed's low end (high-passes about 16 dB / oct below the bump, low shelf, ripple and
//   contour bells) and the head bump: a peak of G = 1 + a x lowEmph at 15 / 25 / 44 Hz (7.5 / 15 / 30 ips; lowEmph 0 =
//   no peak, the roll-off kept); the high-pass also removes the DC of the even harmonics
//     -> level: the speed's small-signal level times the drive compensation c (drive) / c (0) (static; it holds a
//          sine of about -9 dBFS, so high drives come out louder, as the reference does)
// No noise, no wow / flutter, no crosstalk.
// Latency: kTapeLatency (the 4x minimum-phase oversampler's round trip, 5 samples) at every sample rate, constant; the
// reproduce filters' own delay at 2 kHz is kept under 4 us (half a sample at 96 kHz with margin) by the fit.
// At 192 kHz their frequency-dependent phase can exceed half a sample; it is not an additional buffering delay.
// (The reference also leads by a constant ~15 degrees from 20 Hz to 2 kHz with zero group delay, and its top
// roll-off is linear phase. Matching those phases would require more latency than this minimum-phase model allows;
// the music null against it stays near -10 dB.)
// Neutral: none (it saturates and shapes by design, rule 6). Small-signal response at the defaults (30 ips, lowEmph 2,
// highEmph 4): +0.66 dB at 1 kHz, within +0.45 .. +0.9 dB from 200 Hz to 16 kHz, head bump +2.1 dB at 50 Hz, -13.9 dB
// at 20 Hz.
// Parameter changes: drive / bias / harmonics / emphasis glide over 20 ms; a speed change crossfades two settings of
// the filters over 20 ms (no click).
// T/S: not offered (no twin).
namespace ps
{
inline constexpr int kTapeLatency = 5;   // samples at every sample rate (4x minimum-phase oversampler)

struct TapeMeters
{
    AtomicFloat inputDriveDb { -100.0f };   // level into the nonlinearity (after the record EQ and drive): peak dBFS,
                                            //   300 ms release
    AtomicFloat saturationDb;               // compression of the peaks by the tape curve (>= 0 dB): peak level the
                                            //   linear path would have minus the actual one, 300 ms release
};

class Tape final : public Module
{
public:
    Tape();
    ~Tape() override;

    ModuleType getType() const noexcept override { return ModuleType::Tape; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const TapeMeters& getMeters() const noexcept;

    // For the panel's response view: the small-signal magnitude (dB) of the whole linear path (bias tilt, reproduce
    // sections, head bump, high emphasis) for the given settings at `hz` relative to 1 kHz (the level and the
    // sample-rate dependent top roll-off excluded), from the analog prototypes. Any thread.
    static double responseDb (const TapeParams&, double hz) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
