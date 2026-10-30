#pragma once

#include <complex>
#include <vector>

// Small radix-2 complex FFT (iterative, in place). Twiddles and bit-reversal table are computed in the constructor;
// forward() / inverse() do not allocate and are const (one Fft may be shared by several threads).
namespace ps
{
class Fft
{
public:
    explicit Fft (int order);   // size = 2^order, order 1..20

    int size() const noexcept { return n; }
    int order() const noexcept { return log2n; }

    // In-place forward transform, X[k] = sum_n x[n] e^{-2 pi i k n / N} (no scaling). data must hold size() values.
    void forward (std::complex<float>* data) const noexcept;

    // In-place inverse transform, x[n] = (1/N) sum_k X[k] e^{+2 pi i k n / N}, so inverse (forward (x)) == x.
    void inverse (std::complex<float>* data) const noexcept;

    // Two real signals in one complex transform: after forward() of z[n] = a[n] + i b[n], recovers the half spectra
    // A[k], B[k] for k = 0..N/2 (the other bins are their conjugates). a / b must hold N/2 + 1 values.
    void splitPacked (const std::complex<float>* z, std::complex<float>* a, std::complex<float>* b) const noexcept;

    // Inverse of splitPacked: builds the full spectrum Z = A + i B (k = 0..N-1) from the half spectra of two real
    // signals, so inverse (z) gives a[n] in the real part and b[n] in the imaginary part.
    void mergePacked (const std::complex<float>* a, const std::complex<float>* b, std::complex<float>* z) const noexcept;

private:
    int n = 0, log2n = 0;
    std::vector<std::complex<float>> twiddles;   // e^{-2 pi i k / N}, k < N/2
    std::vector<int> bitReversed;
};

} // namespace ps
