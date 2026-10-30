// STUB (contracts): replaced by dsp-punch
#include "dsp/Quadrature.h"

#include <cmath>

// Functional fallback of the quadrature envelope contract (Quadrature.h): 2 x a mean-square one-pole of x^2 (time
// constant 1 / (2 lowestHz)), so a steady sine of amplitude A reads about A^2, but with the 2f ripple the real
// all-pass pair removes. Dependent modules can be built on it; the real design replaces it.
namespace ps
{
void QuadratureEnvelope::prepare (double sampleRate, double lowestHz)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double tau = 1.0 / (2.0 * (lowestHz > 1.0 ? lowestHz : 20.0));
    smoothCoeff = 1.0 - std::exp (-1.0 / (tau * sr));
    coeffs.clear();
    reset();
}

void QuadratureEnvelope::reset() noexcept { smooth = 0.0; }

double QuadratureEnvelope::process (double x) noexcept
{
    smooth += smoothCoeff * (x * x - smooth);
    if (std::abs (smooth) < 1.0e-30)
        smooth = 0.0;
    return 2.0 * smooth;
}

int QuadratureEnvelope::getNumCoefficients() const noexcept { return (int) coeffs.size(); }
} // namespace ps
