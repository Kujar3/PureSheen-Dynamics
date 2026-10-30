#include "dsp/CurveFir.h"

#include "dsp/MatchAnalysis.h"

#include <algorithm>
#include <cmath>

// Curve -> FIR residual designer of the Match EQ (dsp-meq). See CurveFir.h. Everything is computed in double; the
// taps are rounded to float only at the end (the Convolver runs in float).
namespace ps::fir
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kEdgeFadePoints = 24.0;   // 1/2 octave on the 1/48-octave grid

    double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double q = 0.25 * x * x;
        for (int k = 1; k < 200; ++k)
        {
            term *= q / ((double) k * (double) k);
            sum += term;
            if (term < sum * 1.0e-17)
                break;
        }
        return sum;
    }
} // namespace

int matchFirLatency (MatchPhase phase, int numTaps) noexcept
{
    const int p = std::max (16, numTaps / 16);
    return phase == MatchPhase::Minimum ? p : numTaps / 2 + p;
}

double curveDbAt (const float* curveDb, double hz) noexcept
{
    if (curveDb == nullptr || ! (hz > 0.0))
        return 0.0;
    const double x = 48.0 * std::log2 (hz / 10.0);   // grid index
    constexpr int last = kMatchPoints - 1;
    if (x <= 0.0)
        return (double) curveDb[0] * std::max (0.0, 1.0 + x / kEdgeFadePoints);
    if (x >= (double) last)
        return (double) curveDb[last] * std::max (0.0, 1.0 - (x - last) / kEdgeFadePoints);
    const int i = (int) x;
    const double u = x - i;
    return (double) curveDb[i] + u * ((double) curveDb[i + 1] - (double) curveDb[i]);
}

void CurveKernelDesigner::prepare (int maxTaps)
{
    int n = 16;
    while (n < maxTaps)
        n <<= 1;
    maxN = n;
    spec.assign ((size_t) (2 * maxN), cd {});
    work.assign ((size_t) (2 * maxN), cd {});
    twiddle.assign ((size_t) maxN, cd {});
    bitrev.assign ((size_t) (2 * maxN), 0);
    linearWindow.assign ((size_t) maxN, 0.0);
    minimumWindow.assign ((size_t) maxN, 0.0);
    curN = 0;
    prepareSize (maxN);
}

void CurveKernelDesigner::prepareSize (int numTaps) noexcept
{
    if (numTaps == curN || numTaps > maxN)
        return;
    curN = numTaps;
    fftSize = 2 * numTaps;
    log2Size = 0;
    while ((1 << log2Size) < fftSize)
        ++log2Size;

    for (int k = 0; k < fftSize / 2; ++k)
        twiddle[(size_t) k] = std::polar (1.0, -2.0 * kPi * (double) k / (double) fftSize);
    for (int i = 0; i < fftSize; ++i)
    {
        int r = 0;
        for (int b = 0; b < log2Size; ++b)
            if ((i & (1 << b)) != 0)
                r |= 1 << (log2Size - 1 - b);
        bitrev[(size_t) i] = r;
    }

    const double i0Beta = besselI0 (kKaiserBeta);
    const double d = (double) numTaps / 2.0;
    linearWindow[0] = 0.0;
    for (int n = 1; n < numTaps; ++n)
    {
        const double u = ((double) n - d) / d;
        linearWindow[(size_t) n] = besselI0 (kKaiserBeta * std::sqrt (std::max (0.0, 1.0 - u * u))) / i0Beta;
    }
    for (int n = 0; n < numTaps; ++n)
    {
        const double u = (double) n / (double) numTaps;
        minimumWindow[(size_t) n] = besselI0 (kKaiserBeta * std::sqrt (std::max (0.0, 1.0 - u * u))) / i0Beta;
    }
}

void CurveKernelDesigner::transform (cd* data, bool inverse) const noexcept
{
    const int n = fftSize;
    for (int i = 0; i < n; ++i)
    {
        const int j = bitrev[(size_t) i];
        if (i < j)
            std::swap (data[i], data[j]);
    }
    for (int size = 2; size <= n; size <<= 1)
    {
        const int half = size >> 1, step = n / size;
        for (int start = 0; start < n; start += size)
            for (int j = 0; j < half; ++j)
            {
                cd w = twiddle[(size_t) (j * step)];
                if (inverse)
                    w = std::conj (w);
                const cd a = data[start + j], b = data[start + j + half] * w;
                data[start + j] = a + b;
                data[start + j + half] = a - b;
            }
    }
    if (inverse)
    {
        const double s = 1.0 / (double) n;
        for (int i = 0; i < n; ++i)
            data[i] *= s;
    }
}

bool CurveKernelDesigner::design (const float* curveDb, double sampleRate, int numTaps, MatchPhase phase,
                                  float* out) noexcept
{
    if (out == nullptr || numTaps <= 0)
        return false;
    std::fill (out, out + numTaps, 0.0f);
    if (curveDb == nullptr || numTaps > maxN || (numTaps & (numTaps - 1)) != 0 || ! (sampleRate > 0.0))
        return false;

    bool flat = true;
    for (int i = 0; i < kMatchPoints && flat; ++i)
        flat = curveDb[i] == 0.0f;
    if (flat)
        return false;

    prepareSize (numTaps);
    const int M = fftSize, N = numTaps;
    cd* s = spec.data();

    if (phase == MatchPhase::Linear)
    {
        // H_k = |G_k| e^{-j 2 pi k D / M}, D = N / 2, M = 2N  ->  e^{-j pi k / 2}
        static const cd rot[4] { { 1.0, 0.0 }, { 0.0, -1.0 }, { -1.0, 0.0 }, { 0.0, 1.0 } };
        for (int k = 0; k <= N; ++k)
        {
            const double mag = std::pow (10.0, curveDbAt (curveDb, (double) k * sampleRate / (double) M) / 20.0);
            s[k] = mag * rot[k & 3];
        }
        for (int k = 1; k < N; ++k)
            s[M - k] = std::conj (s[k]);
        transform (s, true);

        const int d = N / 2;
        for (int n = 0; n < N; ++n)
            out[n] = (float) (s[n].real() * linearWindow[(size_t) n] - (n == d ? 1.0 : 0.0));
    }
    else
    {
        // real cepstrum of ln |G|
        for (int k = 0; k <= N; ++k)
        {
            const double db = curveDbAt (curveDb, (double) k * sampleRate / (double) M);
            s[k] = cd { db * (std::log (10.0) / 20.0), 0.0 };
        }
        for (int k = 1; k < N; ++k)
            s[M - k] = s[k];
        transform (s, true);

        // fold: causal part x 2
        cd* w = work.data();
        w[0] = cd { s[0].real(), 0.0 };
        for (int n = 1; n < N; ++n)
            w[n] = cd { 2.0 * s[n].real(), 0.0 };
        w[N] = cd { s[N].real(), 0.0 };
        for (int n = N + 1; n < M; ++n)
            w[n] = cd {};
        transform (w, false);
        for (int k = 0; k < M; ++k)
            w[k] = std::exp (w[k]);
        transform (w, true);

        for (int n = 0; n < N; ++n)
            out[n] = (float) (w[n].real() * minimumWindow[(size_t) n] - (n == 0 ? 1.0 : 0.0));
    }

    for (int n = 0; n < N; ++n)
        if (! std::isfinite (out[n]))
        {
            std::fill (out, out + N, 0.0f);
            return false;
        }
    return true;
}
} // namespace ps::fir
