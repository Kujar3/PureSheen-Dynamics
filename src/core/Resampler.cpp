#include "core/Resampler.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ps
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

double besselI0 (double x) noexcept
{
    // Power series of the modified Bessel function of the first kind, order 0 (converges quickly for x < 30).
    double sum = 1.0, term = 1.0;
    const double q = x * x * 0.25;

    for (int k = 1; k < 64; ++k)
    {
        term *= q / ((double) k * (double) k);
        sum += term;
        if (term < sum * 1.0e-16)
            break;
    }

    return sum;
}
} // namespace

SincResampler::SincResampler (double inRate, double outRate)
    : ratio (inRate / outRate)
{
    // Exact rational ratio when both rates are integers with a small common denominator.
    const auto inInt = (long long) std::llround (inRate), outInt = (long long) std::llround (outRate);

    if (std::abs ((double) inInt - inRate) < 1.0e-9 && std::abs ((double) outInt - outRate) < 1.0e-9 && inInt > 0
        && outInt > 0)
    {
        const auto g = std::gcd (inInt, outInt);
        if (outInt / g <= kMaxExactPhases)
        {
            exact = true;
            upFactor = outInt / g;     // L: output steps per input "period"
            downFactor = inInt / g;    // M
        }
    }

    numPhases = exact ? (int) upFactor : (int) kMaxExactPhases;

    cutoff = kRolloff * std::min (1.0, outRate / inRate);    // fraction of the input Nyquist
    halfTaps = (int) std::ceil (kZeroCrossings / cutoff) + 1;
    numTaps = 2 * halfTaps;

    const double i0Beta = besselI0 (kKaiserBeta);
    phases.assign ((size_t) numPhases * (size_t) numTaps, 0.0f);
    std::vector<double> tmp ((size_t) numTaps);

    for (int p = 0; p < numPhases; ++p)
    {
        const double frac = (double) p / (double) numPhases;
        float* w = phases.data() + (size_t) p * (size_t) numTaps;
        double sum = 0.0;

        for (int j = 0; j < numTaps; ++j)
        {
            // Distance (in input samples) between the output position and input tap j.
            const double d = frac + (double) (halfTaps - 1) - (double) j;
            const double u = cutoff * d;
            double v = 0.0;

            if (std::abs (u) < kZeroCrossings)
            {
                const double sinc = std::abs (u) < 1.0e-12 ? 1.0 : std::sin (kPi * u) / (kPi * u);
                const double r = u / kZeroCrossings;
                const double window = besselI0 (kKaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
                v = cutoff * sinc * window;
            }

            tmp[(size_t) j] = v;
            sum += v;
        }

        // Unity DC gain for every phase.
        for (int j = 0; j < numTaps; ++j)
            w[j] = (float) (sum != 0.0 ? tmp[(size_t) j] / sum : 0.0);
    }
}

long long SincResampler::outputLength (long long numInput) const noexcept
{
    if (exact)
        return (numInput * upFactor + downFactor - 1) / downFactor;

    return (long long) std::ceil ((double) numInput / ratio - 1.0e-9);
}

void SincResampler::process (const float* const* in, long long numInput, float* const* out, int numChannels,
                             long long outStart, long long outEnd) const noexcept
{
    for (long long n = outStart; n < outEnd; ++n)
    {
        long long i0 = 0;
        int phase = 0;
        position (n, i0, phase);

        const float* w = phases.data() + (size_t) phase * (size_t) numTaps;
        const long long first = i0 - halfTaps + 1;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* x = in[ch];
            double acc = 0.0;

            if (first >= 0 && first + numTaps <= numInput)
            {
                const float* xs = x + first;
                float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
                int j = 0;

                for (; j + 4 <= numTaps; j += 4)
                {
                    a0 += w[j] * xs[j];
                    a1 += w[j + 1] * xs[j + 1];
                    a2 += w[j + 2] * xs[j + 2];
                    a3 += w[j + 3] * xs[j + 3];
                }

                for (; j < numTaps; ++j)
                    a0 += w[j] * xs[j];

                acc = (double) a0 + (double) a1 + (double) a2 + (double) a3;
            }
            else
            {
                // Near the edges: zero padding outside the input.
                for (int j = 0; j < numTaps; ++j)
                {
                    const long long k = first + j;
                    if (k >= 0 && k < numInput)
                        acc += (double) w[j] * (double) x[k];
                }
            }

            out[ch][n] = (float) acc;
        }
    }
}

void SincResampler::position (long long n, long long& i0, int& phase) const noexcept
{
    if (exact)
    {
        const long long num = n * downFactor;
        i0 = num / upFactor;
        phase = (int) (num % upFactor);
        return;
    }

    const double t = (double) n * ratio;
    i0 = (long long) std::floor (t);
    phase = (int) std::lround ((t - (double) i0) * numPhases);

    if (phase >= numPhases)
    {
        phase = 0;
        ++i0;
    }
}
} // namespace ps
