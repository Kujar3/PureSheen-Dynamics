#include "dsp/Quadrature.h"

#include <algorithm>
#include <cmath>

// Quadrature envelope (Quadrature.h): a 90-degree all-pass pair from an elliptic polyphase half-band design.
//
// Design: H(z) = (A0(z^2) + z^-1 A1(z^2)) / 2 is an elliptic half-band low pass with pass band < 0.25 - t and stop band
// > 0.25 + t (t = lowestHz / fs), A0 / A1 chains of first-order all-passes (c + z^-2) / (1 + c z^-2) taking the
// even / odd coefficients. In the stop band the branches are in anti-phase within delta = 2 x 10^(-att / 20) rad
// (|H| = |sin (delta / 2)|), in the pass band in phase. Shifting the response by fs / 4 (z^2 -> -z^2) turns that into
// a 90-degree difference between a = A0(-z^2) x and b = z^-1 A1(-z^2) x on [t fs, (0.5 - t) fs]. A section in -z^2
// is y[n] = c (x[n] + y[n-2]) - x[n-2]. att = 70 dB -> delta = 0.036 degree, far below the 0.3 degree target, for
// 14 coefficients at 48 kHz / 20 Hz (the count grows only logarithmically with fs / lowestHz).
namespace ps
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kAttenuationDb = 70.0;
constexpr int kMaxCoeffs = 48;

double arithmeticGeometricMean (double a, double b) noexcept
{
    for (int i = 0; i < 20 && a != b; ++i)
    {
        const double nextA = 0.5 * (a + b);
        b = std::sqrt (a * b);
        a = nextA;
    }
    return a;
}

void transitionParams (double transition, double& k, double& q)
{
    k = std::tan ((1.0 - transition * 2.0) * kPi / 4.0);
    k *= k;
    // q = exp (-pi K(k') / K(k)). A truncated nome series loses phase accuracy for narrow transitions
    // (notably 5 Hz at 96 / 192 kHz); the arithmetic-geometric mean evaluates both elliptic integrals.
    const double complementaryK = std::sqrt ((1.0 - k) * (1.0 + k));
    q = std::exp (-kPi * arithmeticGeometricMean (1.0, complementaryK) / arithmeticGeometricMean (1.0, k));
}

int orderFor (double attenuationDb, double q)
{
    const double attnP2 = std::pow (10.0, -attenuationDb / 10.0);
    const double a = attnP2 / (1.0 - attnP2);
    int order = (int) std::ceil (std::log (a * a / 16.0) / std::log (q));
    if ((order & 1) == 0)
        ++order;
    return std::max (order, 3);
}

double accNum (double q, int order, int c)
{
    double acc = 0.0;
    double sign = 1.0;
    for (int i = 0; i < 100; ++i)
    {
        const double term = std::pow (q, (double) i * (i + 1)) * std::sin ((i * 2 + 1) * c * kPi / order) * sign;
        acc += term;
        sign = -sign;
        if (std::abs (term) < 1.0e-100)
            break;
    }
    return acc;
}

double accDen (double q, int order, int c)
{
    double acc = 0.0;
    double sign = -1.0;
    for (int i = 1; i < 100; ++i)
    {
        const double term = std::pow (q, (double) i * i) * std::cos (i * 2 * c * kPi / order) * sign;
        acc += term;
        sign = -sign;
        if (std::abs (term) < 1.0e-100)
            break;
    }
    return acc;
}

std::vector<double> designCoefficients (double transition)
{
    double k = 0.0, q = 0.0;
    transitionParams (transition, k, q);
    const int order = orderFor (kAttenuationDb, q);
    const int numCoeffs = std::min ((order - 1) / 2, kMaxCoeffs);

    std::vector<double> c ((size_t) numCoeffs);
    for (int i = 0; i < numCoeffs; ++i)
    {
        const int ci = i + 1;
        const double num = accNum (q, order, ci) * std::pow (q, 0.25);
        const double den = accDen (q, order, ci) + 0.5;
        const double ww = num / den;
        const double wwsq = ww * ww;
        const double x = std::sqrt ((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
        c[(size_t) i] = (1.0 - x) / (1.0 + x);
    }
    return c;
}

// arg of (c - z^-2) / (1 - c z^-2) at z = e^{jw}
double sectionPhase (double c, double w) noexcept
{
    const double cr = std::cos (2.0 * w), ci = std::sin (2.0 * w);
    // numerator c - e^{-j2w} = (c - cos 2w) + j sin 2w; denominator 1 - c e^{-j2w} = (1 - c cos 2w) + j c sin 2w
    return std::atan2 (ci, c - cr) - std::atan2 (c * ci, 1.0 - c * cr);
}

} // namespace

void QuadratureEnvelope::prepare (double sampleRate, double lowestHz)
{
    sr = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 48000.0;
    const double low = std::clamp (std::isfinite (lowestHz) ? lowestHz : 20.0, 5.0, 0.1 * sr);
    coeffs = designCoefficients (low / sr);
    const int n = (int) coeffs.size();
    numA = (n + 1) / 2;
    numB = n / 2;
    stateA.assign ((size_t) (2 * (numA + 1)), 0.0);
    stateB.assign ((size_t) (2 * (numB + 1)), 0.0);
    reset();
}

void QuadratureEnvelope::reset() noexcept
{
    std::fill (stateA.begin(), stateA.end(), 0.0);
    std::fill (stateB.begin(), stateB.end(), 0.0);
    delayedX = 0.0;
    flushCounter = 0;
}

void QuadratureEnvelope::flushIfSilent() noexcept
{
    flushCounter = 0;
    // Fast stages decay independently of the slower stages. Waiting for the whole cascade to become silent
    // allows their recursive tails to go subnormal even while the final output remains well above that range.
    if (std::abs (delayedX) < 1.0e-100)
        delayedX = 0.0;
    for (double& v : stateA)
        if (std::abs (v) < 1.0e-100)
            v = 0.0;
    for (double& v : stateB)
        if (std::abs (v) < 1.0e-100)
            v = 0.0;
}

int QuadratureEnvelope::getNumCoefficients() const noexcept { return (int) coeffs.size(); }

double QuadratureEnvelope::designPhaseDifferenceDegrees (double hz) const noexcept
{
    const double w = 2.0 * kPi * hz / sr;
    double pa = 0.0, pb = -w;
    for (size_t i = 0; i < coeffs.size(); ++i)
        ((i & 1) == 0 ? pa : pb) += sectionPhase (coeffs[i], w);
    return std::remainder (pa - pb, 2.0 * kPi) * 180.0 / kPi;
}

//==============================================================================
void QuadratureEnvelope2::prepare (double sampleRate, double lowestHz)
{
    QuadratureEnvelope design;
    design.prepare (sampleRate, lowestHz);
    coeffs = design.getCoefficients();
    const int n = (int) coeffs.size();
    numA = (n + 1) / 2;
    numB = n / 2;
    stateA.assign ((size_t) (4 * (numA + 1)), 0.0);
    stateB.assign ((size_t) (4 * (numB + 1)), 0.0);
    reset();
}

void QuadratureEnvelope2::reset() noexcept
{
    std::fill (stateA.begin(), stateA.end(), 0.0);
    std::fill (stateB.begin(), stateB.end(), 0.0);
    delayed[0] = delayed[1] = 0.0;
    flushCounter = 0;
}

void QuadratureEnvelope2::flushIfSilent() noexcept
{
    flushCounter = 0;
    // Flush each lane independently too: sustained audio in one channel must not keep the other's tail alive.
    for (double& v : delayed)
        if (std::abs (v) < 1.0e-100)
            v = 0.0;
    for (double& v : stateA)
        if (std::abs (v) < 1.0e-100)
            v = 0.0;
    for (double& v : stateB)
        if (std::abs (v) < 1.0e-100)
            v = 0.0;
}
} // namespace ps
