#include "core/Fft.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ps
{
Fft::Fft (int order)
    : n (1 << std::clamp (order, 1, 20)),
      log2n (std::clamp (order, 1, 20))
{
    constexpr double kPi = 3.14159265358979323846;

    twiddles.resize ((size_t) (n / 2));
    for (int k = 0; k < n / 2; ++k)
    {
        const double angle = -2.0 * kPi * k / n;
        twiddles[(size_t) k] = { (float) std::cos (angle), (float) std::sin (angle) };
    }

    bitReversed.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        int reversed = 0;
        for (int b = 0; b < log2n; ++b)
            if ((i & (1 << b)) != 0)
                reversed |= 1 << (log2n - 1 - b);

        bitReversed[(size_t) i] = reversed;
    }
}

void Fft::forward (std::complex<float>* data) const noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const int j = bitReversed[(size_t) i];
        if (i < j)
            std::swap (data[i], data[j]);
    }

    // Iterative radix-2 decimation in time. The complex multiply is spelled out (no library NaN/inf handling).
    for (int size = 2; size <= n; size <<= 1)
    {
        const int half = size >> 1;
        const int step = n / size;

        for (int start = 0; start < n; start += size)
        {
            for (int j = 0; j < half; ++j)
            {
                const std::complex<float> w = twiddles[(size_t) (j * step)];
                std::complex<float>& a = data[start + j];
                std::complex<float>& b = data[start + j + half];

                const float tr = w.real() * b.real() - w.imag() * b.imag();
                const float ti = w.real() * b.imag() + w.imag() * b.real();

                b = { a.real() - tr, a.imag() - ti };
                a = { a.real() + tr, a.imag() + ti };
            }
        }
    }
}

// inverse (X) = swap (forward (swap (X))) / N, where swap exchanges the real and imaginary parts (swap (z) = i conj (z)).
void Fft::inverse (std::complex<float>* data) const noexcept
{
    for (int i = 0; i < n; ++i)
        data[i] = { data[i].imag(), data[i].real() };

    forward (data);

    const float scale = 1.0f / (float) n;
    for (int i = 0; i < n; ++i)
        data[i] = { data[i].imag() * scale, data[i].real() * scale };
}

void Fft::splitPacked (const std::complex<float>* z, std::complex<float>* a, std::complex<float>* b) const noexcept
{
    // A[k] = (Z[k] + conj Z[N-k]) / 2,  B[k] = (Z[k] - conj Z[N-k]) / (2i)
    for (int k = 0; k <= n / 2; ++k)
    {
        const std::complex<float> zk = z[k];
        const std::complex<float> zc = std::conj (z[(n - k) & (n - 1)]);
        a[k] = { 0.5f * (zk.real() + zc.real()), 0.5f * (zk.imag() + zc.imag()) };
        b[k] = { 0.5f * (zk.imag() - zc.imag()), -0.5f * (zk.real() - zc.real()) };
    }
}

void Fft::mergePacked (const std::complex<float>* a, const std::complex<float>* b, std::complex<float>* z) const noexcept
{
    // Z[k] = A[k] + i B[k] for k <= N/2; Z[N-k] = conj A[k] + i conj B[k]
    for (int k = 0; k <= n / 2; ++k)
    {
        const std::complex<float> ak = a[k], bk = b[k];
        z[k] = { ak.real() - bk.imag(), ak.imag() + bk.real() };

        if (k > 0 && k < n / 2)
            z[n - k] = { ak.real() + bk.imag(), -ak.imag() + bk.real() };
    }
}

} // namespace ps
