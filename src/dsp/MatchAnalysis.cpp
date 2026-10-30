#include "dsp/MatchAnalysis.h"

#include "core/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

// Match EQ analysis (dsp-meq): the long-term spectrum analyser and the matched curve, as documented in MatchAnalysis.h.
//
// Choices made where neither the header nor the research decides:
// - Analyser frame gate: the mean power of the (unwindowed) frame over both channels, (sum L^2 + sum R^2) / 2N, below
//   1e-7 (-70 dBFS) skips the frame. getSeconds() counts accepted frames x hop / sampleRate (the non-silent audio the
//   average is made of), so a 20 s capture of continuous music reads ~19.8 s (the first frame needs N samples).
// - Bin PSD: one-sided |X_k|^2 x 2 / (fs sum w^2) (DC and Nyquist x 1), so the integral of the PSD over frequency is
//   the mean-square level of the signal; grid point i is the kernel-share weighted mean of the bin PSDs (the RefMatch
//   mapping: share = integral of the raised cosine over the bin's extent in log2 f, divided by the kernel mass).
//   Powers at or below -200 dB (digital silence in a band, e.g. above an encoder's low-pass) become "no value".
// - Curve step 1 ("points without a value are held flat"): a missing point takes the power of the nearest valid
//   point (above Nyquist: the last valid one), so the smoothing never sees a cliff; step 7 then zeroes those points.
//   Step 2 renormalises the kernel at the two grid ends. The mean-level check of a set (< -150 dB = flat) is the
//   power mean of the valid points between 20 Hz and 20 kHz. Step 3 sums only points where both spectra have a value.
// - K-weighting: the magnitude of the BS.1770 48 kHz pre-filter and RLB high-pass evaluated at each grid frequency
//   (only 20 Hz .. 20 kHz is used, well below 24 kHz).
namespace ps
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kNoValuePower = 1.0e-20;     // -200 dB
    constexpr double kGateMeanPower = 1.0e-7;     // -70 dBFS
    constexpr double kMinSetLevelDb = -150.0;
    constexpr double kRegionTaperOctaves = 0.5;

    inline bool hasValue (float db) noexcept { return std::isfinite (db) && db > kMatchNoValue; }

    inline float dbOrNoValue (double power) noexcept
    {
        if (! (power > kNoValuePower) || ! std::isfinite (power))
            return kMatchNoValue;
        return (float) (10.0 * std::log10 (power));
    }

    // |H|^2 of the BS.1770 K-weighting (48 kHz coefficients) at f Hz.
    double kWeightPower (double hz) noexcept
    {
        const double w = 2.0 * kPi * hz / 48000.0;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
        const auto biquad = [&] (double b0, double b1, double b2, double a1, double a2)
        {
            return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
        };
        const auto shelf = biquad (1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
                                   0.73248077421585);
        const auto hp = biquad (1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621);
        return std::norm (shelf * hp);
    }

    struct GridTables
    {
        double hz[kMatchPoints];
        double cellHz[kMatchPoints];
        double kWeight2[kMatchPoints];

        GridTables() noexcept
        {
            const double cell = std::exp2 (1.0 / 96.0) - std::exp2 (-1.0 / 96.0);
            for (int i = 0; i < kMatchPoints; ++i)
            {
                hz[i] = 10.0 * std::exp2 ((double) i / 48.0);
                cellHz[i] = hz[i] * cell;
                kWeight2[i] = kWeightPower (hz[i]);
            }
        }
    };

    const GridTables& grid() noexcept
    {
        static const GridTables t;   // no heap: plain arrays
        return t;
    }
} // namespace

//==============================================================================
double matchGridHz (int index) noexcept
{
    return 10.0 * std::pow (2.0, (double) index / 48.0);
}

double matchGridCellHz (int index) noexcept
{
    return index >= 0 && index < kMatchPoints ? grid().cellHz[index] : 0.0;
}

MatchChannel matchChannelFor (ChannelMode mode, int channelSet) noexcept
{
    switch (mode)
    {
        case ChannelMode::MidSide:   return channelSet == 1 ? MatchChannel::Side : MatchChannel::Mid;
        case ChannelMode::LeftRight: return channelSet == 1 ? MatchChannel::Right : MatchChannel::Left;
        case ChannelMode::Stereo:
        case ChannelMode::TransientSustain:
        default: break;
    }
    return MatchChannel::StereoMean;
}

float MatchSpectrum::valueDb (MatchChannel ch, int index) const noexcept
{
    if (index < 0 || index >= kMatchPoints)
        return kMatchNoValue;
    if (ch != MatchChannel::StereoMean)
    {
        const int c = std::clamp ((int) ch, 0, 3);
        const float v = psdDb[c][(size_t) index];
        return hasValue (v) ? v : kMatchNoValue;
    }

    const float l = psdDb[0][(size_t) index], r = psdDb[1][(size_t) index];
    const bool hasL = hasValue (l), hasR = hasValue (r);
    if (! hasL && ! hasR)
        return kMatchNoValue;
    // A silent channel has no PSD value, but still contributes zero power to the stereo mean.
    const double p = 0.5 * ((hasL ? std::pow (10.0, l / 10.0) : 0.0)
                         + (hasR ? std::pow (10.0, r / 10.0) : 0.0));
    return dbOrNoValue (p);
}

double matchSmoothingOctaves (float smoothPercent) noexcept
{
    const double s = std::clamp (std::isfinite (smoothPercent) ? (double) smoothPercent : 50.0, 0.0, 100.0) / 100.0;
    return (1.0 / 48.0) * std::pow (96.0, s);
}

//==============================================================================
struct MatchAnalyser::Impl
{
    using cf = std::complex<float>;

    double sr = 0.0;
    int N = 0, half = 0, hop = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> window;
    double windowPower = 1.0;   // sum w^2

    std::vector<float> bufL, bufR;
    int fill = 0;
    std::vector<cf> work, specL, specR;
    std::vector<double> accLL, accRR, accLR;
    long long frames = 0;

    // grid mapping: point i uses bins firstBin[i] .. firstBin[i] + (start[i+1] - start[i]) - 1
    int weightStart[kMatchPoints + 1] {};
    int firstBin[kMatchPoints] {};
    std::vector<double> weights;

    void prepareWeights()
    {
        const double binHz = sr / N;
        const double nyquist = 0.5 * sr;
        constexpr double tinyHz = 1.0e-9;

        weights.clear();
        weights.reserve ((size_t) kMatchPoints * 16);
        std::vector<double> shares;

        for (int i = 0; i < kMatchPoints; ++i)
        {
            weightStart[i] = (int) weights.size();
            firstBin[i] = 0;
            const double fc = grid().hz[i];
            if (fc >= nyquist)
                continue;

            const double b = std::max (1.0 / 48.0, std::log2 (1.0 + 1.5 * binHz / fc));
            const auto kernelIntegral = [b] (double u)
            {
                u = std::clamp (u, -b, b);
                return 0.5 * (u + b) + b / (2.0 * kPi) * std::sin (kPi * u / b);
            };

            const double lo = fc * std::exp2 (-b), hi = std::min (fc * std::exp2 (b), nyquist);
            const int k0 = std::clamp ((int) std::floor (lo / binHz + 0.5), 0, half);
            const int k1 = std::clamp ((int) std::floor (hi / binHz + 0.5), k0, half);

            shares.assign ((size_t) (k1 - k0 + 1), 0.0);
            double mass = 0.0;
            for (int k = k0; k <= k1; ++k)
            {
                const double binLo = std::max (0.0, (k - 0.5) * binHz), binHi = std::min (nyquist, (k + 0.5) * binHz);
                if (! (binHi > binLo))
                    continue;
                const double share = std::max (0.0, kernelIntegral (std::log2 (binHi / fc))
                                                        - kernelIntegral (std::log2 (std::max (binLo, tinyHz) / fc)));
                const double oneSided = (k == 0 || k == half) ? 1.0 : 2.0;
                shares[(size_t) (k - k0)] = share * oneSided;
                mass += share;
            }
            if (! (mass > 0.0))
                continue;

            firstBin[i] = k0;
            for (double s : shares)
                weights.push_back (s / mass);
        }
        weightStart[kMatchPoints] = (int) weights.size();
    }

    void processFrame() noexcept
    {
        double power = 0.0;
        for (int i = 0; i < N; ++i)
            power += (double) bufL[(size_t) i] * bufL[(size_t) i] + (double) bufR[(size_t) i] * bufR[(size_t) i];
        if (! std::isfinite (power) || ! (power / (2.0 * N) >= kGateMeanPower))
            return;

        for (int i = 0; i < N; ++i)
            work[(size_t) i] = cf { bufL[(size_t) i] * window[(size_t) i], bufR[(size_t) i] * window[(size_t) i] };
        fft->forward (work.data());
        fft->splitPacked (work.data(), specL.data(), specR.data());

        // A corrupt float file can contain finite samples large enough to overflow the FFT. Reject the whole
        // frame before changing any accumulator, so later clean audio can still produce a usable spectrum.
        for (int k = 0; k <= half; ++k)
            if (! std::isfinite (specL[(size_t) k].real()) || ! std::isfinite (specL[(size_t) k].imag())
                || ! std::isfinite (specR[(size_t) k].real()) || ! std::isfinite (specR[(size_t) k].imag()))
                return;

        for (int k = 0; k <= half; ++k)
        {
            const double lr = specL[(size_t) k].real(), li = specL[(size_t) k].imag();
            const double rr = specR[(size_t) k].real(), ri = specR[(size_t) k].imag();
            accLL[(size_t) k] += lr * lr + li * li;
            accRR[(size_t) k] += rr * rr + ri * ri;
            accLR[(size_t) k] += lr * rr + li * ri;   // Re (L conj R)
        }
        ++frames;
    }
};

MatchAnalyser::MatchAnalyser() : impl (std::make_unique<Impl>()) {}
MatchAnalyser::~MatchAnalyser() = default;

void MatchAnalyser::prepare (double sampleRate)
{
    auto& m = *impl;
    m.sr = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 48000.0;
    const int order = m.sr <= 50000.0 ? 14 : (m.sr <= 100000.0 ? 15 : 16);
    m.N = 1 << order;
    m.half = m.N / 2;
    m.hop = m.N / 2;
    m.fft = std::make_unique<Fft> (order);

    m.window.assign ((size_t) m.N, 0.0f);
    m.windowPower = 0.0;
    for (int i = 0; i < m.N; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * i / m.N);   // periodic Hann (exact 50 % overlap-add)
        m.window[(size_t) i] = (float) w;
        m.windowPower += (double) m.window[(size_t) i] * m.window[(size_t) i];
    }

    m.bufL.assign ((size_t) m.N, 0.0f);
    m.bufR.assign ((size_t) m.N, 0.0f);
    m.work.assign ((size_t) m.N, {});
    m.specL.assign ((size_t) m.half + 1, {});
    m.specR.assign ((size_t) m.half + 1, {});
    m.accLL.assign ((size_t) m.half + 1, 0.0);
    m.accRR.assign ((size_t) m.half + 1, 0.0);
    m.accLR.assign ((size_t) m.half + 1, 0.0);
    m.prepareWeights();
    reset();
}

void MatchAnalyser::reset() noexcept
{
    auto& m = *impl;
    std::fill (m.accLL.begin(), m.accLL.end(), 0.0);
    std::fill (m.accRR.begin(), m.accRR.end(), 0.0);
    std::fill (m.accLR.begin(), m.accLR.end(), 0.0);
    m.frames = 0;
    m.fill = 0;
}

void MatchAnalyser::push (const float* left, const float* right, int numSamples) noexcept
{
    auto& m = *impl;
    if (m.N == 0 || left == nullptr)
        return;

    int done = 0;
    while (done < numSamples)
    {
        const int take = std::min (numSamples - done, m.N - m.fill);
        std::memcpy (m.bufL.data() + m.fill, left + done, sizeof (float) * (size_t) take);
        std::memcpy (m.bufR.data() + m.fill, (right != nullptr ? right : left) + done, sizeof (float) * (size_t) take);
        m.fill += take;
        done += take;

        if (m.fill == m.N)
        {
            m.processFrame();
            const int keep = m.N - m.hop;
            std::memmove (m.bufL.data(), m.bufL.data() + m.hop, sizeof (float) * (size_t) keep);
            std::memmove (m.bufR.data(), m.bufR.data() + m.hop, sizeof (float) * (size_t) keep);
            m.fill = keep;
        }
    }
}

double MatchAnalyser::getSeconds() const noexcept
{
    const auto& m = *impl;
    return m.sr > 0.0 ? (double) m.frames * m.hop / m.sr : 0.0;
}

void MatchAnalyser::spectrum (MatchSpectrum& out) const noexcept
{
    const auto& m = *impl;
    out.sampleRate = m.sr;
    out.seconds = getSeconds();
    out.valid = m.frames > 0 && out.seconds >= 1.0;
    for (auto& ch : out.psdDb)
        ch.fill (kMatchNoValue);
    if (m.frames == 0 || m.N == 0)
        return;

    const double norm = 1.0 / ((double) m.frames * m.sr * m.windowPower);
    for (int i = 0; i < kMatchPoints; ++i)
    {
        const int begin = m.weightStart[i], end = m.weightStart[i + 1];
        if (begin >= end)
            continue;

        double ll = 0.0, rr = 0.0, lr = 0.0;
        for (int j = begin, k = m.firstBin[i]; j < end; ++j, ++k)
        {
            const double w = m.weights[(size_t) j];
            ll += w * m.accLL[(size_t) k];
            rr += w * m.accRR[(size_t) k];
            lr += w * m.accLR[(size_t) k];
        }
        out.psdDb[0][(size_t) i] = dbOrNoValue (norm * ll);
        out.psdDb[1][(size_t) i] = dbOrNoValue (norm * rr);
        out.psdDb[2][(size_t) i] = dbOrNoValue (norm * std::max (0.0, ll + rr + 2.0 * lr) * 0.25);
        out.psdDb[3][(size_t) i] = dbOrNoValue (norm * std::max (0.0, ll + rr - 2.0 * lr) * 0.25);
    }
}

//==============================================================================
double matchWeightedLevelDb (const float* psdDb, const bool* mask) noexcept
{
    if (psdDb == nullptr)
        return kMatchNoValue;
    const auto& g = grid();
    double sum = 0.0;
    bool any = false;
    for (int i = 0; i < kMatchPoints; ++i)
    {
        if (g.hz[i] < 20.0 || g.hz[i] > 20000.0 || ! hasValue (psdDb[i]) || (mask != nullptr && ! mask[i]))
            continue;
        sum += g.kWeight2[i] * std::pow (10.0, (double) psdDb[i] / 10.0) * g.cellHz[i];
        any = true;
    }
    return any && sum > 0.0 ? 10.0 * std::log10 (sum) : (double) kMatchNoValue;
}

void computeMatchCurve (const MatchSpectrum& reference, const MatchSpectrum& current, MatchChannel channel,
                        const MatchEqSetParams& params, float* outDb) noexcept
{
    if (outDb == nullptr)
        return;
    std::fill (outDb, outDb + kMatchPoints, 0.0f);
    if (! reference.valid || ! current.valid)
        return;

    const auto& g = grid();
    double R[kMatchPoints], C[kMatchPoints];
    bool hasR[kMatchPoints], hasC[kMatchPoints];

    // 1. to power; the mean-level check of each set; holes held flat
    const auto load = [&] (const MatchSpectrum& s, double* P, bool* has) -> bool
    {
        double meanPower = 0.0;
        int meanCount = 0, valid = 0;
        for (int i = 0; i < kMatchPoints; ++i)
        {
            const float v = s.valueDb (channel, i);
            has[i] = hasValue (v);
            P[i] = has[i] ? std::pow (10.0, (double) v / 10.0) : 0.0;
            if (has[i])
            {
                ++valid;
                if (g.hz[i] >= 20.0 && g.hz[i] <= 20000.0)
                {
                    meanPower += P[i];
                    ++meanCount;
                }
            }
        }
        if (valid == 0 || meanCount == 0 || 10.0 * std::log10 (std::max (meanPower / meanCount, 1.0e-300)) < kMinSetLevelDb)
            return false;

        int last = -1;
        for (int i = 0; i < kMatchPoints; ++i)
        {
            if (has[i])
            {
                if (last < 0)
                    for (int j = 0; j < i; ++j)
                        P[j] = P[i];   // leading hole: the first valid value
                last = i;
            }
            else if (last >= 0)
            {
                // nearest valid neighbour: the previous one, or the next one if it is closer
                int next = i + 1;
                while (next < kMatchPoints && ! has[next])
                    ++next;
                P[i] = (next < kMatchPoints && next - i < i - last) ? P[next] : P[last];
            }
        }
        return true;
    };

    if (! load (reference, R, hasR) || ! load (current, C, hasC))
        return;

    // 2. smoothing (raised cosine over log2 f, half-width in grid points), renormalised at the grid ends
    const double halfPoints = std::max (1.0, matchSmoothingOctaves (params.smoothPercent) * 48.0);
    const int reach = std::min (kMatchPoints - 1, (int) std::ceil (halfPoints) - 1);
    double kernel[kMatchPoints];
    for (int j = 0; j <= reach; ++j)
        kernel[j] = 0.5 + 0.5 * std::cos (kPi * (double) j / halfPoints);

    double Rs[kMatchPoints], Cs[kMatchPoints];
    for (int i = 0; i < kMatchPoints; ++i)
    {
        double sr = 0.0, sc = 0.0, sw = 0.0;
        const int j0 = std::max (0, i - reach), j1 = std::min (kMatchPoints - 1, i + reach);
        for (int j = j0; j <= j1; ++j)
        {
            const double w = kernel[std::abs (j - i)];
            sr += w * R[j];
            sc += w * C[j];
            sw += w;
        }
        Rs[i] = sw > 0.0 ? sr / sw : R[i];
        Cs[i] = sw > 0.0 ? sc / sw : C[i];
    }

    // 3. level normalisation (K-weighted power over 20 Hz .. 20 kHz where both have a value)
    double lr = 0.0, lc = 0.0;
    for (int i = 0; i < kMatchPoints; ++i)
    {
        if (g.hz[i] < 20.0 || g.hz[i] > 20000.0 || ! hasR[i] || ! hasC[i])
            continue;
        const double w = g.kWeight2[i] * g.cellHz[i];
        lr += w * Rs[i];
        lc += w * Cs[i];
    }
    if (! (lr > 0.0) || ! (lc > 0.0))
        return;
    const double offset = 10.0 * std::log10 (lr) - 10.0 * std::log10 (lc);

    const double lo = std::clamp ((double) params.lowHz, 1.0, 1.0e6);
    const double hi = std::max (lo, std::clamp ((double) params.highHz, 1.0, 1.0e6));
    const double amount = std::clamp ((double) params.amountPercent, 0.0, 100.0) / 100.0;
    const double maxBoost = std::clamp ((double) params.maxBoostDb, 0.0, 96.0);
    const double maxCut = std::clamp ((double) params.maxCutDb, 0.0, 96.0);

    for (int i = 0; i < kMatchPoints; ++i)
    {
        const double f = g.hz[i];
        if (f < 10.0 || ! hasR[i] || ! hasC[i])
            continue;   // 7.

        const double d = 10.0 * std::log10 (Rs[i]) - 10.0 * std::log10 (Cs[i]) - offset;

        // 4. region
        double t = 1.0;
        if (f < lo)
        {
            const double oct = std::log2 (lo / f);
            t = oct < kRegionTaperOctaves ? 0.5 + 0.5 * std::cos (kPi * oct / kRegionTaperOctaves) : 0.0;
        }
        else if (f > hi)
        {
            const double oct = std::log2 (f / hi);
            t = oct < kRegionTaperOctaves ? 0.5 + 0.5 * std::cos (kPi * oct / kRegionTaperOctaves) : 0.0;
        }

        // 5. amount, 6. soft limits
        double x = d * t * amount;
        if (x > 0.0)
            x = maxBoost > 0.0 ? maxBoost * std::tanh (x / maxBoost) : 0.0;
        else if (x < 0.0)
            x = maxCut > 0.0 ? maxCut * std::tanh (x / maxCut) : 0.0;

        outDb[i] = std::isfinite (x) ? (float) x : 0.0f;
    }
}

//==============================================================================
void setMatchSource (MatchSpectrum& s, const char* tag) noexcept
{
    s.source.fill (0);
    if (tag == nullptr)
        return;
    for (size_t i = 0; i + 1 < s.source.size() && tag[i] != 0; ++i)
        s.source[i] = tag[i];
}

int setMatchChannelFromBins (MatchSpectrum& s, MatchChannel channel, const float* hz, const float* levelDb, int numBins,
                             double binWidthHz) noexcept
{
    if (channel == MatchChannel::StereoMean)
        return 0;
    auto& out = s.psdDb[(size_t) std::clamp ((int) channel, 0, 3)];
    out.fill (kMatchNoValue);
    if (hz == nullptr || levelDb == nullptr || numBins <= 0)
        return 0;

    const double toPsd = binWidthHz > 0.0 ? -10.0 * std::log10 (binWidthHz) : 0.0;
    const auto valid = [&] (int k) { return hasValue (levelDb[k]) && std::isfinite (hz[k]) && hz[k] > 0.0f; };
    const auto& g = grid();
    const double lowEdge = std::exp2 (-1.0 / 96.0), highEdge = std::exp2 (1.0 / 96.0);

    int count = 0, k = 0;   // k: first bin with hz >= the current cell's low edge (grid ascending -> monotonic)
    for (int i = 0; i < kMatchPoints; ++i)
    {
        const double f = g.hz[i];
        if (f < hz[0] || f > hz[numBins - 1])
            continue;

        const double cellLo = f * lowEdge, cellHi = f * highEdge;
        while (k < numBins && hz[k] < cellLo)
            ++k;

        double power = 0.0;
        int inCell = 0;
        for (int j = k; j < numBins && hz[j] < cellHi; ++j)
            if (valid (j))
            {
                power += std::pow (10.0, ((double) levelDb[j] + toPsd) / 10.0);
                ++inCell;
            }

        float value = kMatchNoValue;
        if (inCell > 0)
        {
            value = dbOrNoValue (power / inCell);
        }
        else
        {
            // interpolate (log f, dB) between the bins around f
            int above = k;
            while (above < numBins && hz[above] < f)
                ++above;
            const int below = above - 1;
            if (below >= 0 && above < numBins && valid (below) && valid (above))
            {
                const double x0 = std::log2 ((double) hz[below]), x1 = std::log2 ((double) hz[above]);
                const double u = x1 > x0 ? (std::log2 (f) - x0) / (x1 - x0) : 0.0;
                value = (float) ((double) levelDb[below] + u * ((double) levelDb[above] - levelDb[below]) + toPsd);
            }
            else if (above < numBins && valid (above) && (double) hz[above] == f)
            {
                value = (float) ((double) levelDb[above] + toPsd);
            }
        }

        if (hasValue (value))
        {
            out[(size_t) i] = value;
            ++count;
        }
    }
    return count;
}

void setMatchSpectrumFromBins (MatchSpectrum& s, const float* hz, const float* levelDb, int numBins, double binWidthHz,
                               const char* source) noexcept
{
    const int count = setMatchChannelFromBins (s, MatchChannel::Left, hz, levelDb, numBins, binWidthHz);
    s.psdDb[1] = s.psdDb[0];
    s.psdDb[2] = s.psdDb[0];
    s.psdDb[3].fill (kMatchNoValue);
    s.valid = count >= 2;
    s.seconds = 0.0;
    s.sampleRate = 0.0;
    setMatchSource (s, source);
}

MatchSpectrum makeMatchTiltSpectrum (double slopeDbPerOctave, double lowShelfDb, double lowShelfHz,
                                     const char* source) noexcept
{
    MatchSpectrum s;
    const double shelfHz = std::max (1.0, lowShelfHz);
    for (int i = 0; i < kMatchPoints; ++i)
    {
        const double f = grid().hz[i];
        const double x = f / shelfHz;
        const double db = -60.0 + slopeDbPerOctave * std::log2 (f / 1000.0) + lowShelfDb / (1.0 + x * x);
        s.psdDb[0][(size_t) i] = (float) db;
    }
    s.psdDb[1] = s.psdDb[0];
    s.psdDb[2] = s.psdDb[0];
    s.psdDb[3].fill (kMatchNoValue);
    s.valid = true;
    s.seconds = 0.0;
    s.sampleRate = 0.0;
    setMatchSource (s, source);
    return s;
}
} // namespace ps
