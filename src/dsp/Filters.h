#pragma once

#include <complex>

// Biquad building blocks (pure C++17). Coefficient design is done in double precision; the biquads run in double
// (transposed direct form II), which is cheap enough for mastering and avoids low-frequency noise of float biquads.
// Owner: dsp-common. Implementation: Filters.cpp.
namespace ps
{
struct BiquadCoeffs
{
    // y = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    std::complex<double> response (double freqHz, double sampleRate) const noexcept;
    double magnitudeDb (double freqHz, double sampleRate) const noexcept;
};

// Designs. All take the corner / centre frequency in Hz; they clamp it to (1 Hz .. 0.49 * sampleRate).
// "Matched" designs follow the analog prototype's magnitude up to Nyquist (no bilinear cramping at high frequencies)
// using the Vicanek matched second-order method; mastering EQs must not change shape near 20 kHz at 44.1 kHz.
namespace design
{
    BiquadCoeffs peak (double sr, double hz, double q, double gainDb) noexcept;          // matched
    BiquadCoeffs lowShelf (double sr, double hz, double q, double gainDb) noexcept;      // matched, q 0.1..
    BiquadCoeffs highShelf (double sr, double hz, double q, double gainDb) noexcept;     // matched
    BiquadCoeffs highPass (double sr, double hz, double q) noexcept;                     // matched 2nd order
    BiquadCoeffs lowPass (double sr, double hz, double q) noexcept;                      // matched 2nd order
    BiquadCoeffs highPass1 (double sr, double hz) noexcept;                              // 1st order (b2 = a2 = 0)
    BiquadCoeffs lowPass1 (double sr, double hz) noexcept;
    BiquadCoeffs allPass (double sr, double hz, double q) noexcept;                      // 2nd order
    BiquadCoeffs allPass1 (double sr, double hz) noexcept;
    BiquadCoeffs lowShelf1 (double sr, double hz, double gainDb) noexcept;               // 1st order shelf
    BiquadCoeffs highShelf1 (double sr, double hz, double gainDb) noexcept;
    // Baxandall shelves, first order, measured from the reference (Filters.cpp): the low shelf's corners and depth
    // and the high shelf's lower corner follow the gain; Q is not used.
    BiquadCoeffs baxandallLow (double sr, double hz, double gainDb) noexcept;
    BiquadCoeffs baxandallHigh (double sr, double hz, double gainDb) noexcept;

    // Q of section k (0-based) of an N-th order Butterworth cascade (N even: N/2 biquads; N odd: one 1st-order
    // section first, then (N-1)/2 biquads with these Q values).
    double butterworthQ (int order, int sectionIndex) noexcept;
}

class Biquad
{
public:
    void setCoeffs (const BiquadCoeffs& c) noexcept { coeffs = c; }
    const BiquadCoeffs& getCoeffs() const noexcept { return coeffs; }
    void reset() noexcept { z1 = z2 = 0.0; }

    inline double processSample (double x) noexcept
    {
        const double y = coeffs.b0 * x + z1;
        z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
        z2 = coeffs.b2 * x - coeffs.a2 * y;
        return y;
    }

    void process (float* data, int numSamples) noexcept;

private:
    BiquadCoeffs coeffs;
    double z1 = 0.0, z2 = 0.0;
};

// Click-free coefficient changes: after setTarget the filter glides to the new design over `rampLength` samples.
// Unconditionally stable under any modulation (any targets, any rate of setTarget calls, any ramp length): every
// design is realised as a trapezoidal (TPT) state-variable filter, y = m0 x + m1 band + m2 low, with
// g = tan (w0 / 2) > 0 and damping k > 0 recovered from the denominator; the glide interpolates g, k, m0..m2. For every
// g > 0, k > 0 the SVF's state-transition matrix has spectral norm exactly 1 (it can never amplify its state), so no
// sequence of coefficients can make it grow. A static SmoothedBiquad has the transfer function of its BiquadCoeffs (to
// double rounding). Cost: ~9 multiplies per sample, + 1 division per sample while ramping. Targets must be stable
// designs (all design:: functions are); a non-finite or unstable target is ignored.
class SmoothedBiquad
{
public:
    void setRampLength (int samples) noexcept { rampLength = samples > 1 ? samples : 1; }
    void setTarget (const BiquadCoeffs&) noexcept;
    void snapToTarget() noexcept;
    void reset() noexcept;
    double processSample (double x) noexcept;
    bool isRamping() const noexcept { return remaining > 0; }

private:
    struct Svf
    {
        double g = 0.1, k = 1.0, m0 = 1.0, m1 = 0.0, m2 = 0.0;
    };
    void updateGains() noexcept;   // a1..a3 of `current`

    Svf current, target, step;
    double a1 = 1.0, a2 = 0.0, a3 = 0.0;   // SVF gains of current
    double ic1 = 0.0, ic2 = 0.0;          // trapezoidal integrator states
    int rampLength = 64, remaining = 0;
};

} // namespace ps
