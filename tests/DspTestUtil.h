#pragma once

// Measurement helpers for the DSP tests (added by dsp-common): a double-precision FFT that is independent of the code
// under test, tone measurement and level helpers.

#include "TestSignals.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <vector>

namespace pstest
{
using cd = std::complex<double>;

// In-place radix-2 FFT in double precision (size must be a power of two). inverse = true includes the 1/N scale.
inline void fftInPlace (std::vector<cd>& a, bool inverse = false)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPi / (double) len * (inverse ? 1.0 : -1.0);
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k)
            {
                const cd w = std::polar (1.0, ang * (double) k);
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }

    if (inverse)
        for (auto& x : a)
            x /= (double) n;
}

// Spectrum of a (zero-padded) real impulse response.
template <typename T>
std::vector<cd> spectrumOf (const std::vector<T>& x, size_t fftSize)
{
    std::vector<cd> a (fftSize);
    for (size_t i = 0; i < std::min (fftSize, x.size()); ++i)
        a[i] = (double) x[i];
    fftInPlace (a);
    return a;
}

inline double dbOf (double magnitude) { return magnitude > 1.0e-15 ? 20.0 * std::log10 (magnitude) : -300.0; }

// Complex amplitude of a sinusoid at freq in y[start .. start + n) (correlation; exact for whole cycles).
template <typename T>
cd toneOf (const std::vector<T>& y, size_t start, size_t n, double freq, double sr)
{
    cd acc {};
    for (size_t i = 0; i < n; ++i)
        acc += (double) y[start + i] * std::polar (1.0, -2.0 * kPi * freq * (double) (start + i) / sr);
    return acc * (2.0 / (double) n);
}

template <typename T>
double rmsOf (const std::vector<T>& y, size_t start, size_t n)
{
    double s = 0.0;
    for (size_t i = 0; i < n; ++i)
        s += (double) y[start + i] * (double) y[start + i];
    return std::sqrt (s / (double) std::max<size_t> (n, 1));
}

template <typename T>
double peakOf (const std::vector<T>& y, size_t start, size_t n)
{
    double p = 0.0;
    for (size_t i = 0; i < n && start + i < y.size(); ++i)
        p = std::max (p, std::abs ((double) y[start + i]));
    return p;
}

struct Stopwatch
{
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    double seconds() const
    {
        return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    }
};
} // namespace pstest
