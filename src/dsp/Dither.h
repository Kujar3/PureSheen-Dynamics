#pragma once

#include "dsp/Params.h"
#include "dsp/Fifo.h"

#include <cstdint>

// Output dither (reference-suite parity; our own algorithms). Owner: dsp-maximizer. Implementation: Dither.cpp.
//   - quantises to the chosen bit depth (the output stays float, holding exactly representable values)
//   - Amount Strong / Medium / Low: TPDF noise of 1.0 / 0.7 / 0.5 LSB peak-to-peak scale (deterministic LCG, seeded
//     per channel); Off: plain rounding, optionally with Harmonic Suppression (high-pass TPDF dither: +-1 LSB
//     triangular, spectrum rising toward Nyquist; removes quantisation harmonics and idle tones for any input, with
//     a low-frequency floor close to plain rounding's)
//   - Noise shaping 0..100: error-feedback filter blended from flat (0) to a 9th-order psychoacoustic curve (100,
//     ~ 14 dB less audible noise in the 1..6 kHz region, noise pushed above 15 kHz); the filter is designed for 44.1
//     and 48 kHz families, flat above 96 kHz
//   - Limit Peaks: keeps |shaped noise| within 4 LSB so aggressive shaping cannot create spurious peaks
//   - Auto-blank: output is exact digital silence when the input has been exactly 0 for >= 0.7 s
//   - Bit meter: per channel, which of the 24 bits were active in the last block (and a peak-hold mask, cleared by
//     resetPeakBits and whenever the bit depth or the on switch changes)
// Deterministic: same input -> same output (tests rely on it).
namespace ps
{
struct DitherMeters
{
    std::atomic<std::uint32_t> activeBits[2] { {0}, {0} };      // bit k set = bit k (0 = MSB) toggled
    std::atomic<std::uint32_t> peakBits[2] { {0}, {0} };        // OR of activeBits since resetPeakBits
    AtomicFloat dcOffset[2];                                    // input DC estimate (linear), for the DC meter
};

class Dither
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void setParams (const DitherParams&) noexcept;
    void process (float* const* channels, int numSamples) noexcept;   // in place, stereo
    const DitherMeters& getMeters() const noexcept { return meters; }
    void resetPeakBits() noexcept;

private:
    DitherParams params;
    double sampleRate = 48000.0;
    std::uint32_t rng[2] { 0x1234567u, 0x7654321u };
    float errorHistory[2][12] {};
    int silentSamples = 0;
    DitherMeters meters;

    // (added by the implementation, private)
    static constexpr int kShapeOrder = 9;
    static constexpr int kShapeSteps = 101;                 // noise shaping 0..100 in steps of 1
    void designShapingTable();
    double shapeTable[kShapeSteps][kShapeOrder] {};         // error-feedback coefficients a1..a9 per amount
    double coeffs[kShapeOrder] {};                          // current (interpolated) coefficients
    double feedbackLimit = 3.5;                             // Limit Peaks bound of the feedback term, in LSB
    double ditherScale = 0.7;                               // TPDF scale (0 = Off)
    double dcState[2] {};
    double dcCoeff = 0.0;
    int blankThreshold = 33600;                             // 0.7 s in samples
    double hpPrev[2] {};                                    // Harmonic Suppression: previous uniform value (high-pass TPDF)
    int lastBits = -1;
    bool lastOn = false;
};

} // namespace ps
