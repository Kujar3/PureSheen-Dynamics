#pragma once

#include <vector>

// Ripple-free amplitude envelope for fast detectors on low frequencies (Punch, Bass Shaper; pure C++17).
// Owner: dsp-punch. Implementation: Quadrature.cpp. Tests: tests/QuadratureTests.cpp, prefix "quad.".
//
// A detector that squares or rectifies a 40 Hz tone ripples at 80 Hz; with a 1 ms attack that ripple becomes audible
// gain modulation (distortion on bass). QuadratureEnvelope instead splits x into two signals a, b that are 90 degrees
// apart (two chains of first-order all-passes in z^-2, the classic polyphase IIR "Hilbert" pair; coefficients from an
// elliptic half-band design computed in prepare for the requested lowest frequency) and returns a^2 + b^2, the
// squared amplitude envelope: a steady sine of amplitude A reads A^2 with no ripple.
// Design target: phase-difference error < 0.3 degree from lowestHz to 0.45 fs (power ripple < 0.05 dB for a steady
// tone at 40 Hz and 1 kHz); roughly 14-20 coefficients at 44.1 / 48 kHz, more at 192 kHz (measured, reported by
// getNumCoefficients). The chains delay the detector by a few ms at 20-50 Hz: a detector lag, not a signal delay.
// Realtime: prepare() allocates; reset() / process() do not. Double precision, denormal-safe.
namespace ps
{
class QuadratureEnvelope
{
public:
    void prepare (double sampleRate, double lowestHz = 20.0);
    void reset() noexcept;
    double process (double x) noexcept;      // returns a^2 + b^2 (squared amplitude)
    int getNumCoefficients() const noexcept;

private:
    std::vector<double> coeffs, stateA, stateB;
    double sr = 48000.0;
    double smooth = 0.0, smoothCoeff = 0.0;  // (stub fallback)
};
} // namespace ps
