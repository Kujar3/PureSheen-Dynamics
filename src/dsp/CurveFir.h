#pragma once

#include "dsp/Types.h"

#include <complex>
#include <vector>

// Magnitude curve -> FIR residual kernel for the Match EQ (owner: dsp-meq; pure C++17, double-precision design).
// Generic part of the Match EQ filter realisation (MatchEq.h): a gain curve in dB on the Match EQ grid
// (MatchAnalysis.h, kMatchPoints points at matchGridHz (i)) becomes the residual r = h - delta of a linear- or
// minimum-phase FIR of N taps:
//   |G (f)| = 10^(curve / 20) sampled on a 2N-point FFT grid (curveDbAt: log-f interpolation of the grid points;
//            outside the grid (below 10 Hz, above 23.6 kHz) the end value fades linearly in log f to 0 dB over
//            1/2 octave, continuing the region taper of the curve; DC is 0 dB)
//   Linear : H = |G| e^{-j w D}, inverse FFT, symmetric Kaiser (beta 9) window centred at D = N / 2 over taps 1 .. N-1
//            (tap 0 = 0, so the kernel is exactly symmetric about D), r = h - delta (D)
//   Minimum: real cepstrum of ln |G| on the 2N grid, folded (causal part x 2), exp, inverse FFT, right half-Kaiser
//            (beta 9) over the N taps, r = h - delta (0)
//   a curve that is exactly 0 dB everywhere -> r = 0 (no FFT work)
// prepare() allocates; design() is allocation-free (one designer per thread: it owns its work buffers).
namespace ps::fir
{
inline constexpr double kKaiserBeta = 9.0;

// Linear: D + P, Minimum: P, with P = numTaps / 16 (the convolver partition).
int matchFirLatency (MatchPhase, int numTaps) noexcept;

// The curve value (dB) at hz, as the designer samples it (see above). curveDb: kMatchPoints values.
double curveDbAt (const float* curveDb, double hz) noexcept;

class CurveKernelDesigner
{
public:
    void prepare (int maxTaps);   // allocates for numTaps <= maxTaps (power of two, >= 16)

    // Writes numTaps residual taps (numTaps: a power of two <= maxTaps). Returns false when the curve is flat (r = 0
    // exactly, out zeroed).
    bool design (const float* curveDb, double sampleRate, int numTaps, MatchPhase, float* residualOut) noexcept;

    int getMaxTaps() const noexcept { return maxN; }

private:
    using cd = std::complex<double>;

    void prepareSize (int numTaps) noexcept;   // twiddles / bit reversal / windows for 2 * numTaps (inside buffers)
    void transform (cd* data, bool inverse) const noexcept;

    int maxN = 0, curN = 0, fftSize = 0, log2Size = 0;
    std::vector<cd> spec, work, twiddle;
    std::vector<int> bitrev;
    std::vector<double> linearWindow, minimumWindow;
};

} // namespace ps::fir
