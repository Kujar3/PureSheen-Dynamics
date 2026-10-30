#pragma once

#include <vector>

#if defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
 #include <emmintrin.h>
 #define PS_QUAD_SSE2 1
#else
 #define PS_QUAD_SSE2 0
#endif

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
//
// Implementation notes (dsp-punch): the elliptic half-band H(z) = (A0(z^2) + z^-1 A1(z^2)) / 2 is designed for a
// transition half width t = lowestHz / fs and 70 dB stop-band attenuation; its two branches shifted by fs / 4
// (z^2 -> -z^2) differ by 90 degrees +- 2 x 10^(-70 / 20) rad (0.036 degree) on [lowestHz, fs / 2 - lowestHz].
// Measured (QuadratureTests): 14 coefficients at 44.1 / 48 kHz, 15 at 88.2 / 96 kHz, 17 at 192 kHz; worst phase error
// 0.031 degree (48 kHz, 20 Hz .. 0.45 fs), power ripple 0.004 dB p-p at 30 Hz; the power reaches 90 % of a level step
// after 0.9 ms at 1 kHz and 19 ms at 40 Hz (the detector lag). lowestHz is clamped to [5 Hz, 0.1 fs].
// Each stage's history is independently flushed below 1e-100 every 256 samples, before its recursive tail reaches
// denormals; stereo lanes are cleared independently too. The per-sample code is inline (once per band and channel).
namespace ps
{
class QuadratureEnvelope
{
public:
    void prepare (double sampleRate, double lowestHz = 20.0);
    void reset() noexcept;
    double process (double x) noexcept;      // returns a^2 + b^2 (squared amplitude)
    int getNumCoefficients() const noexcept;

    // (added by dsp-punch) The quadrature pair itself: a and b are 90 degrees apart (b lags a), each with unit
    // magnitude response; a^2 + b^2 is what process() returns. Same state as process() (call one or the other).
    void processPair (double x, double& a, double& b) noexcept;

    // (added by dsp-punch) Design-side phase difference arg A(f) - arg B(f) in degrees at hz (90 in the design band),
    // from the coefficients (not realtime: trigonometry per section).
    double designPhaseDifferenceDegrees (double hz) const noexcept;

    // (added by dsp-punch) The design (even index -> chain a, odd -> chain b; sections (c - z^-2) / (1 - c z^-2)).
    const std::vector<double>& getCoefficients() const noexcept { return coeffs; }

private:
    void flushIfSilent() noexcept;

    std::vector<double> coeffs, stateA, stateB;   // coeffs: even index -> chain A, odd -> chain B
    double sr = 48000.0;
    double delayedX = 0.0;                        // the z^-1 in front of chain B
    int numA = 0, numB = 0, flushCounter = 0;
};

//==============================================================================
// (added by dsp-punch) Two independent channels in one pass (SSE2 lanes on x86 / x64): the same design and, sample
// for sample, the same output as two QuadratureEnvelope instances (bit-identical, including independent silence
// flushing in each lane), at about the cost of one.
class QuadratureEnvelope2
{
public:
    void prepare (double sampleRate, double lowestHz = 20.0);
    void reset() noexcept;
    // x0 / x1 -> the pairs (a0, b0), (a1, b1) of each channel
    void processPair (double x0, double x1, double& a0, double& b0, double& a1, double& b1) noexcept;
#if PS_QUAD_SSE2
    // the same with the channels in the two lanes (lane 0 = channel 0)
    void processPair (__m128d x, __m128d& a, __m128d& b) noexcept;
#endif
    int getNumCoefficients() const noexcept { return (int) coeffs.size(); }

private:
    void flushIfSilent() noexcept;

    std::vector<double> coeffs;
    std::vector<double> stateA, stateB;   // per stage: [n-1 ch0, n-1 ch1, n-2 ch0, n-2 ch1]
    double delayed[2] { 0.0, 0.0 };
    int numA = 0, numB = 0, flushCounter = 0;
};

//==============================================================================
namespace quadrature_detail
{
// One chain of sections in -z^-2, y[n] = c (x[n] + y[n-2]) - x[n-2]. hist holds (numSections + 1) pairs
// [2k] = s_k[n-1], [2k+1] = s_k[n-2] (s_0 = chain input, s_{k+1} = output of section k).
inline double runChain (const double* c, int numSections, double* hist, double x) noexcept
{
    double cur = x;
    for (int i = 0; i < numSections; ++i)
    {
        double* in = hist + 2 * i;
        const double y = c[2 * i] * (cur + in[3]) - in[1];
        in[1] = in[0];
        in[0] = cur;
        cur = y;
    }
    double* last = hist + 2 * numSections;
    last[1] = last[0];
    last[0] = cur;
    return cur;
}

#if PS_QUAD_SSE2
// The same for two channels: hist holds (numSections + 1) x [n-1 ch0, n-1 ch1, n-2 ch0, n-2 ch1].
inline __m128d runChain2 (const double* c, int numSections, double* hist, __m128d x) noexcept
{
    __m128d cur = x;
    for (int i = 0; i < numSections; ++i)
    {
        double* in = hist + 4 * i;
        const __m128d y = _mm_sub_pd (_mm_mul_pd (_mm_set1_pd (c[2 * i]), _mm_add_pd (cur, _mm_loadu_pd (in + 6))),
                                      _mm_loadu_pd (in + 2));
        _mm_storeu_pd (in + 2, _mm_loadu_pd (in));
        _mm_storeu_pd (in, cur);
        cur = y;
    }
    double* last = hist + 4 * numSections;
    _mm_storeu_pd (last + 2, _mm_loadu_pd (last));
    _mm_storeu_pd (last, cur);
    return cur;
}
#endif
} // namespace quadrature_detail

inline void QuadratureEnvelope::processPair (double x, double& a, double& b) noexcept
{
    if (stateA.empty())
    {
        a = x;
        b = 0.0;
        return;
    }
    a = quadrature_detail::runChain (coeffs.data(), numA, stateA.data(), x);
    b = quadrature_detail::runChain (coeffs.data() + 1, numB, stateB.data(), delayedX);
    delayedX = x;
    if (++flushCounter >= 256)
        flushIfSilent();
}

inline double QuadratureEnvelope::process (double x) noexcept
{
    double a = 0.0, b = 0.0;
    processPair (x, a, b);
    return a * a + b * b;
}

#if PS_QUAD_SSE2
inline void QuadratureEnvelope2::processPair (__m128d x, __m128d& a, __m128d& b) noexcept
{
    if (stateA.empty())
    {
        a = x;
        b = _mm_setzero_pd();
        return;
    }
    a = quadrature_detail::runChain2 (coeffs.data(), numA, stateA.data(), x);
    b = quadrature_detail::runChain2 (coeffs.data() + 1, numB, stateB.data(), _mm_loadu_pd (delayed));
    _mm_storeu_pd (delayed, x);
    if (++flushCounter >= 256)
        flushIfSilent();
}
#endif

inline void QuadratureEnvelope2::processPair (double x0, double x1, double& a0, double& b0, double& a1,
                                              double& b1) noexcept
{
    if (stateA.empty())
    {
        a0 = x0;
        a1 = x1;
        b0 = b1 = 0.0;
        return;
    }
#if PS_QUAD_SSE2
    const __m128d a = quadrature_detail::runChain2 (coeffs.data(), numA, stateA.data(), _mm_set_pd (x1, x0));
    const __m128d b = quadrature_detail::runChain2 (coeffs.data() + 1, numB, stateB.data(),
                                                    _mm_loadu_pd (delayed));
    _mm_storeu_pd (delayed, _mm_set_pd (x1, x0));
    a0 = _mm_cvtsd_f64 (a);
    a1 = _mm_cvtsd_f64 (_mm_unpackhi_pd (a, a));
    b0 = _mm_cvtsd_f64 (b);
    b1 = _mm_cvtsd_f64 (_mm_unpackhi_pd (b, b));
#else
    double h[2];
    // scalar fallback: interleaved layout handled lane by lane
    for (int ch = 0; ch < 2; ++ch)
    {
        const double xin[2] { x0, x1 };
        for (int chain = 0; chain < 2; ++chain)
        {
            double* hist = chain == 0 ? stateA.data() : stateB.data();
            const int n = chain == 0 ? numA : numB;
            double cur = chain == 0 ? xin[ch] : delayed[ch];
            for (int i = 0; i < n; ++i)
            {
                double* in = hist + 4 * i;
                const double y = coeffs[(size_t) (2 * i + chain)] * (cur + in[6 + ch]) - in[2 + ch];
                in[2 + ch] = in[ch];
                in[ch] = cur;
                cur = y;
            }
            double* last = hist + 4 * n;
            last[2 + ch] = last[ch];
            last[ch] = cur;
            h[chain] = cur;
        }
        (ch == 0 ? a0 : a1) = h[0];
        (ch == 0 ? b0 : b1) = h[1];
    }
    delayed[0] = x0;
    delayed[1] = x1;
#endif
    if (++flushCounter >= 256)
        flushIfSilent();
}
} // namespace ps
