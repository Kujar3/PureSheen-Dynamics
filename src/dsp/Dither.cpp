#include "dsp/Dither.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Output dither. Design notes (our own algorithm; the research only fixes the controls):
//
// Quantiser with error feedback, per channel, all arithmetic in double:
//     s = sum_k a_k * e[n-k]            (k = 1..9, feedback of the total error)
//     u = x + s                          (Limit Peaks clamps s, see below)
//     y = q * floor ((u + d) / q + 0.5) (d = dither, q = 2^-(bits-1))
//     e = y - u
// so the output noise y - x = e[n] + sum a_k e[n-k] = A(z) e: A(z) is the noise transfer function (monic FIR).
//
// Noise shaping: A(z) is designed per sample rate in prepare() as the 9th-order linear-prediction (Levinson) whitening
// filter of a psychoacoustic weighting W(f). Minimising the W-weighted noise power over monic A(z) is exactly the LPC
// problem, so the result is minimum phase and optimal for its order (Gerzon-Craven). W(f) in dB is the negative of a
// hearing-threshold curve (Terhardt), held flat below 1 kHz (no point pushing noise into the bass) and capped at
// +25 dB (limits the high-frequency boost). The amount 0..100 scales that curve in dB (0 = flat = no shaping), so
// every intermediate setting is itself an optimal design rather than a blend of coefficients. Measured at 100:
// about -14 dB (44.1 kHz) / -15 dB (48 kHz) noise in 1..6 kHz, +11 dB above 15 kHz. The 88.2 / 96 kHz family gets the
// same curve on its wider band (most noise goes above 20 kHz). Above 96 kHz the shaping is flat (off).
//
// Dither: TPDF (sum of two uniform LCG values, seeded per channel), +-1 LSB peak (Strong), scaled 0.7 (Medium) and
// 0.5 (Low). "1.0 / 0.7 / 0.5" in the header are read as scale factors of standard full TPDF.
// Harmonic Suppression (Amount Off only): a deterministic Nyquist-rate pattern (-1)^n * a[n] with a sawtooth
// amplitude a[n] = 0.5 LSB * (n mod 61) / 61. Averaged over the pattern the quantiser's low-frequency transfer becomes
// linear (like rectangular dither) so the in-band harmonics of low-level signals drop by ~20..35 dB, while the residue
// lands around Nyquist (lines at fs/2 +- k * fs/61, decaying with k) instead of a broadband floor. Measured in-band
// floor is below plain rounding and ~5 dB below TPDF.
// Limit Peaks: clamps the feedback term so that |y - x| <= 4 LSB.
// Auto-blank: after >= 0.7 s of input frames that are exactly 0 on both channels, the output is exactly 0 (no dither,
// error history cleared) until the input is non-zero again.
// Bit meter (always, also with dither off): 24-bit two's complement of the output; bit k (0 = MSB) is active when it
// differed from the block's first sample at least once. DC meter: input one-pole mean, 1 s time constant.
namespace ps
{
namespace
{
inline double uniform01 (std::uint32_t& state) noexcept
{
    state = state * 1664525u + 1013904223u;
    return (double) (state >> 8) * (1.0 / 16777216.0);
}

double thresholdOfHearingDb (double hz) noexcept
{
    const double f = std::max (hz, 1000.0) * 0.001;   // flat below 1 kHz
    const double ath = 3.64 * std::pow (f, -0.8) - 6.5 * std::exp (-0.6 * (f - 3.3) * (f - 3.3)) + 1.0e-3 * f * f * f * f;
    return std::min (ath, 25.0);
}

constexpr int kDitherSeed0 = 0x1234567;
constexpr int kDitherSeed1 = 0x7654321;
} // namespace

void Dither::designShapingTable()
{
    constexpr int gridSize = 1024;
    constexpr int order = kShapeOrder;
    std::vector<double> curve ((size_t) gridSize + 1);
    std::vector<double> cosTable ((size_t) (order + 1) * (gridSize + 1));

    for (auto& row : shapeTable)
        for (auto& c : row)
            c = 0.0;

    if (sampleRate > 96000.0 + 1.0)
        return;   // flat above 96 kHz

    for (int k = 0; k <= gridSize; ++k)
    {
        curve[(size_t) k] = thresholdOfHearingDb ((double) k * sampleRate / (2.0 * gridSize));
        for (int m = 0; m <= order; ++m)
            cosTable[(size_t) (m * (gridSize + 1) + k)] = std::cos (3.14159265358979323846 * (double) k * (double) m / (double) gridSize);
    }

    for (int step = 1; step < kShapeSteps; ++step)
    {
        const double alpha = (double) step / (double) (kShapeSteps - 1);
        double r[order + 1] {};

        for (int k = 0; k <= gridSize; ++k)
        {
            const double weight = (k == 0 || k == gridSize) ? 0.5 : 1.0;
            const double p = weight * std::pow (10.0, -alpha * curve[(size_t) k] / 10.0);
            for (int m = 0; m <= order; ++m)
                r[m] += p * cosTable[(size_t) (m * (gridSize + 1) + k)];
        }

        r[0] *= 1.0 + 1.0e-9;

        // Levinson-Durbin
        double a[order + 1] { 1.0 };
        double err = r[0];
        for (int i = 1; i <= order && err > 0.0; ++i)
        {
            double acc = r[i];
            for (int j = 1; j < i; ++j)
                acc += a[j] * r[i - j];
            const double kRefl = -acc / err;

            double next[order + 1];
            for (int j = 0; j <= order; ++j)
                next[j] = a[j];
            for (int j = 1; j < i; ++j)
                next[j] = a[j] + kRefl * a[i - j];
            next[i] = kRefl;
            for (int j = 0; j <= order; ++j)
                a[j] = next[j];

            err *= 1.0 - kRefl * kRefl;
        }

        for (int j = 0; j < order; ++j)
            shapeTable[step][j] = a[j + 1];
    }
}

void Dither::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    designShapingTable();
    blankThreshold = (int) std::ceil (0.7 * sampleRate);
    dcCoeff = 1.0 - std::exp (-1.0 / sampleRate);   // 1 s time constant
    reset();
    setParams (params);
}

void Dither::reset() noexcept
{
    rng[0] = (std::uint32_t) kDitherSeed0;
    rng[1] = (std::uint32_t) kDitherSeed1;
    for (auto& ch : errorHistory)
        for (auto& e : ch)
            e = 0.0f;
    silentSamples = 0;
    harmonicPhase = 0;
    dcState[0] = dcState[1] = 0.0;
    for (int c = 0; c < 2; ++c)
    {
        meters.activeBits[c].store (0, std::memory_order_relaxed);
        meters.peakBits[c].store (0, std::memory_order_relaxed);
        meters.dcOffset[c].store (0.0f);
    }
}

void Dither::setParams (const DitherParams& p) noexcept
{
    params = p;

    const int bits = kDitherBitDepth[(size_t) std::clamp ((int) p.bits, 0, (int) kDitherBitDepth.size() - 1)];
    if (bits != lastBits || p.on != lastOn)
    {
        for (auto& ch : errorHistory)
            for (auto& e : ch)
                e = 0.0f;
        lastBits = bits;
        lastOn = p.on;
    }

    switch (p.amount)
    {
        case DitherAmount::Strong: ditherScale = 1.0; break;
        case DitherAmount::Medium: ditherScale = 0.7; break;
        case DitherAmount::Low:    ditherScale = 0.5; break;
        case DitherAmount::Off:
        default:                   ditherScale = 0.0; break;
    }

    const bool harmonic = ditherScale == 0.0 && p.harmonicSuppression;
    const double ditherPeak = ditherScale > 0.0 ? ditherScale : (harmonic ? 0.5 : 0.0);
    feedbackLimit = 4.0 - 0.5 - ditherPeak;

    const double amount = std::clamp ((double) p.noiseShaping, 0.0, 100.0);
    const int i0 = std::min ((int) amount, kShapeSteps - 2);
    const double frac = amount - (double) i0;
    for (int k = 0; k < kShapeOrder; ++k)
        coeffs[k] = shapeTable[i0][k] + frac * (shapeTable[i0 + 1][k] - shapeTable[i0][k]);
}

void Dither::process (float* const* channels, int numSamples) noexcept
{
    if (channels == nullptr || numSamples <= 0)
        return;

    const int bits = kDitherBitDepth[(size_t) std::clamp ((int) params.bits, 0, (int) kDitherBitDepth.size() - 1)];
    const double q = std::ldexp (1.0, -(bits - 1));
    const double invQ = 1.0 / q;
    const double limit = feedbackLimit * q;
    const bool harmonic = ditherScale == 0.0 && params.harmonicSuppression;
    const bool shaping = std::any_of (coeffs, coeffs + kShapeOrder, [] (double c) { return c != 0.0; });

    std::uint32_t firstWord[2] {}, toggled[2] {};

    for (int i = 0; i < numSamples; ++i)
    {
        const bool silentFrame = channels[0][i] == 0.0f && channels[1][i] == 0.0f;
        silentSamples = silentFrame ? std::min (silentSamples + 1, blankThreshold) : 0;
        const bool blanked = params.on && params.autoBlank && silentSamples >= blankThreshold;

        if (blanked && silentSamples == blankThreshold)
            for (auto& ch : errorHistory)
                for (auto& e : ch)
                    e = 0.0f;

        const double harmonicValue = harmonic ? ((harmonicPhase & 1) != 0 ? -1.0 : 1.0) * 0.5 * q
                                                    * (double) (harmonicPhase % 61) / 61.0
                                              : 0.0;
        if (harmonic)
            harmonicPhase = (harmonicPhase + 1) % 122;

        for (int c = 0; c < 2; ++c)
        {
            float* data = channels[c];
            const double x = (double) data[i];
            dcState[c] += dcCoeff * (x - dcState[c]);

            double y = x;

            if (blanked)
            {
                y = 0.0;
            }
            else if (params.on)
            {
                float* eh = errorHistory[c];
                double s = 0.0;
                if (shaping)
                {
                    for (int k = 0; k < kShapeOrder; ++k)
                        s += coeffs[k] * (double) eh[k];
                    if (params.limitPeaks)
                        s = std::clamp (s, -limit, limit);
                }

                const double u = x + s;
                double d = harmonicValue;
                if (ditherScale > 0.0)
                {
                    const double r1 = uniform01 (rng[c]);
                    const double r2 = uniform01 (rng[c]);
                    d = (r1 + r2 - 1.0) * ditherScale * q;
                }

                y = std::floor ((u + d) * invQ + 0.5) * q;
                const double e = y - u;

                for (int k = kShapeOrder - 1; k > 0; --k)
                    eh[k] = eh[k - 1];
                eh[0] = (float) e;
            }

            data[i] = (float) y;

            // bit meter (24-bit two's complement of the output)
            const double scaled = std::clamp ((double) data[i], -1.0, 1.0) * 8388608.0;
            const long long word = std::clamp (std::llround (scaled), -8388608LL, 8388607LL);
            const auto w = (std::uint32_t) word & 0xFFFFFFu;
            if (i == 0)
                firstWord[c] = w;
            toggled[c] |= w ^ firstWord[c];
        }
    }

    for (int c = 0; c < 2; ++c)
    {
        std::uint32_t mask = 0;
        for (int k = 0; k < 24; ++k)
            if ((toggled[c] >> (23 - k)) & 1u)
                mask |= 1u << k;
        meters.activeBits[c].store (mask, std::memory_order_relaxed);
        meters.peakBits[c].fetch_or (mask, std::memory_order_relaxed);
        meters.dcOffset[c].store ((float) dcState[c]);
    }
}

void Dither::resetPeakBits() noexcept
{
    meters.peakBits[0].store (0);
    meters.peakBits[1].store (0);
}
} // namespace ps
