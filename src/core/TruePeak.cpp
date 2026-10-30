#include "core/TruePeak.h"

#include <algorithm>
#include <cmath>

//==============================================================================
// True peak
//
// The interpolator is a 64-tap Kaiser-windowed sinc (beta 7, cut-off at the original Nyquist frequency) split into
// 4 polyphase branches of 16 taps. Being even-length and symmetric around tap 31.5, its branches interpolate at
// 1/8, 3/8, 5/8 and 7/8 of a sample; together with the raw samples (which also count) no point of the signal is more
// than 1/8 sample from an evaluated point. Each branch is normalised to unity DC gain.
//==============================================================================

namespace ps
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kKaiserBeta = 7.0;

// Modified Bessel function of the first kind, order 0 (power series).
double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double halfX = 0.5 * x;

    for (int k = 1; k < 64; ++k)
    {
        const double f = halfX / k;
        term *= f * f;
        sum += term;

        if (term < 1.0e-14 * sum)
            break;
    }

    return sum;
}
} // namespace

TruePeakMeter::TruePeakMeter()
{
    constexpr int numTaps = kOversampling * kTapsPerPhase;
    const double centre = 0.5 * (numTaps - 1);
    const double i0Beta = besselI0 (kKaiserBeta);

    std::array<double, (size_t) numTaps> h {};

    for (int n = 0; n < numTaps; ++n)
    {
        const double x = (n - centre) / kOversampling;              // in input samples
        const double sinc = std::abs (x) < 1.0e-12 ? 1.0 : std::sin (kPi * x) / (kPi * x);
        const double r = (n - centre) / centre;                       // -1..1 over the filter
        const double kaiser = besselI0 (kKaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        h[(size_t) n] = sinc * kaiser;
    }

    for (int p = 0; p < kOversampling; ++p)
    {
        double sum = 0.0;
        for (int k = 0; k < kTapsPerPhase; ++k)
            sum += h[(size_t) (p + k * kOversampling)];

        for (int k = 0; k < kTapsPerPhase; ++k)
            phases[(size_t) p][(size_t) k] = (float) (h[(size_t) (p + k * kOversampling)] / sum);
    }
}

void TruePeakMeter::prepare (double) noexcept
{
    reset();
}

void TruePeakMeter::reset() noexcept
{
    for (auto& h : history)
        h.fill (0.0f);

    truePeak = 0.0f;
    samplePeak = 0.0f;
}

void TruePeakMeter::process (const float* left, const float* right, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    if (left != nullptr)
        processChannel (0, left, numSamples);

    if (right != nullptr)
        processChannel (1, right, numSamples);
}

void TruePeakMeter::processChannel (int channel, const float* data, int numSamples) noexcept
{
    auto& h = history[(size_t) channel];
    float tp = truePeak, sp = samplePeak;

    for (int i = 0; i < numSamples; ++i)
    {
        // NaN / inf would poison 16 interpolations (inf: the peak, forever). They count as silence.
        const float x = std::isfinite (data[i]) ? data[i] : 0.0f;

        for (int k = kTapsPerPhase - 1; k > 0; --k)
            h[(size_t) k] = h[(size_t) (k - 1)];

        h[0] = x;

        const float ax = std::abs (x);
        if (ax > sp)
            sp = ax;

        for (const auto& phase : phases)
        {
            float y = 0.0f;
            for (int k = 0; k < kTapsPerPhase; ++k)
                y += phase[(size_t) k] * h[(size_t) k];

            const float ay = std::abs (y);
            if (ay > tp)
                tp = ay;
        }
    }

    samplePeak = sp;
    truePeak = std::max (tp, sp);
}

float TruePeakMeter::truePeakDb() const noexcept
{
    return gainToDb (truePeak);
}

float TruePeakMeter::samplePeakDb() const noexcept
{
    return gainToDb (samplePeak);
}

//==============================================================================
float crestFactorDb (const float* left, const float* right, long long numSamples)
{
    if (left == nullptr || numSamples <= 0)
        return kNoValue;

    double peak = 0.0, sumSquares = 0.0;
    const int numChannels = right != nullptr ? 2 : 1;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* data = ch == 0 ? left : right;

        for (long long i = 0; i < numSamples; ++i)
        {
            const double x = data[i];
            peak = std::max (peak, std::abs (x));
            sumSquares += x * x;
        }
    }

    const double rms = std::sqrt (sumSquares / (double) (numSamples * numChannels));

    if (! (peak > 1.0e-10) || ! (rms > 1.0e-10))
        return kNoValue;

    return (float) (20.0 * std::log10 (peak / rms));
}

} // namespace ps
