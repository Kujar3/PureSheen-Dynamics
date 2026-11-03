#pragma once

#include "dsp/Module.h"

#include <memory>

// Tape: a two-head tape machine (record path, tape, reproduce head), Stereo only. Owner: mod-tape. Implementation:
// Tape.cpp. Tests: tests/TapeTests.cpp, prefix "tape.". Every coefficient below named k... lives in ONE constant
// block at the top of Tape.cpp (first-principles values until the reference renders are fitted).
//
// Per channel (L and R processed alike, unlinked), the whole chain 4x oversampled (Oversampler, MinimumPhase):
//   x -> record EQ (pre-emphasis: first-order high shelf rising above the speed's corner kPreEmphasisHz: 7.5 ips
//          500 Hz, 15 ips 1 kHz, 30 ips 2 kHz, the documented equal-distortion frequencies; +kPreEmphasisDb)
//     -> drive (driveDb)
//     -> tape nonlinearity: a memory-ful saturator with light hysteresis (the curve depends on the sign of the slope;
//          the loop width is set by bias); bias also shapes the curve: negative = earlier high-frequency saturation and
//          a slight HF lift, positive = softer and more compressed, near +10 = strongly limited dynamics
//          (over-biased); harmonics (0..10) adds an even-order term (an asymmetric offset of the curve, its DC removed)
//     -> reproduce EQ (de-emphasis: the exact inverse of the record EQ, so the small-signal path is flat)
//     -> head bump: a resonant peak at kBumpHz (7.5 ips 40, 15 ips 60, 30 ips 110 Hz) of 0 .. +10 dB for lowEmph
//          0 .. 10 (law kBumpLaw), and the low roll-off below it (kept at lowEmph 0)
//     -> gap / spacing loss: a speed-dependent gentle high roll-off (kHfLossHz per speed)
//     -> high emphasis: highEmph 4 = the loss compensated (flat highs), below = gentle roll-off, above = lift
//          ("shimmer"), up to kHighEmphMaxDb
//     -> make-up: a static gain from drive, bias, harmonics and speed (no signal-dependent auto gain) chosen so a
//          steady -18 dBFS RMS pink noise comes out at -18 dBFS RMS (+-0.5 dB) at every drive
// No noise, no wow / flutter, no crosstalk.
// Latency: kTapeLatency (the 4x minimum-phase oversampler's round trip, 5 samples) at every sample rate, constant.
// Neutral: none (it saturates and shapes by design, rule 6). Small-signal (-40 dBFS) response at 15 ips, lowEmph 0,
// highEmph 4, bias 0: flat within +-0.5 dB from 200 Hz to 10 kHz (the head-bump roll-off and the HF loss lie outside).
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

    // For the panel's response view: the small-signal magnitude (dB) of the whole linear path (record and reproduce
    // EQ, head bump, gap loss, high emphasis, make-up excluded) for the given settings at `hz`, from the analog
    // prototypes (independent of the sample rate). Any thread.
    static double responseDb (const TapeParams&, double hz) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
