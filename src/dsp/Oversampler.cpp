#include "dsp/Oversampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// 2x / 4x / 8x oversampler, cascaded half-band stages (dsp-common).
//
// LinearPhase: Kaiser-windowed half-band FIRs (beta for 106 dB, so >= 100 dB is met with margin). Stage 1 (base -> 2x) passes 0 .. 0.45 fs and stops
//   from 0.55 fs (139 taps); later stages only have to protect 0 .. 0.55 fs (base) from the images of their own rate,
//   so they are much shorter (stage 2: 0.1375 .. 0.3625 of its rate, 39 taps; stage 3: 27 taps). Polyphase: one phase
//   of a half-band is a pure delay, the other a dense FIR with symmetric taps. Each stage delays by c = (N - 1) / 2
//   samples of its own rate on the way up and again on the way down; the total is padded at the top rate to a whole
//   number of base-rate samples, so getLatencySamples() is exact (2x: 69, 4x: 79, 8x: 82). Double accumulation.
// MinimumPhase: polyphase IIR half-bands (two parallel chains of first-order all-passes in z^2, elliptic design after
//   the classic Valenzuela / Constantinides structure), designed for >= 100 dB from 0.55 fs for stage 1. Latency
//   convention: getLatencySamples() = the round trip's low-frequency group delay (-dphase/dw at DC, base-rate
//   samples) rounded to the nearest integer, so low frequencies line up with a dry path delayed by it within half a
//   sample (the phase is not linear, so no integer is exact at every frequency). Per stage: interpolator gd, decimator
//   gd - 1 (it keeps the odd-indexed filtered sample), in that stage's high-rate samples. 2x / 4x / 8x: 3 / 5 / 6
//   (group delay at DC 3.49 / 4.94 / 5.60).
namespace ps
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kAttenuationDb = 106.0;   // design target (spec: >= 100 dB)

    double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double q = 0.25 * x * x;
        for (int k = 1; k < 300; ++k)
        {
            term *= q / ((double) k * (double) k);
            sum += term;
            if (term < 1.0e-17 * sum)
                break;
        }
        return sum;
    }

    // Kaiser-windowed half-band low pass (cutoff fs/4, DC gain 1), length 2c + 1 with c odd.
    std::vector<double> halfBand (int c)
    {
        const double beta = 0.1102 * (kAttenuationDb - 8.7);
        const double i0b = besselI0 (beta);
        std::vector<double> h ((size_t) (2 * c + 1), 0.0);

        for (int n = 0; n <= 2 * c; ++n)
        {
            const int t = n - c;
            if (t == 0)
            {
                h[(size_t) n] = 0.5;
                continue;
            }
            if ((t & 1) == 0)
                continue;   // exact zeros of the half band

            const double u = (double) t / (double) c;
            const double w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - u * u))) / i0b;
            h[(size_t) n] = w * std::sin (0.5 * kPi * t) / (kPi * t);
        }

        // exact DC gain: the dense phase sums to 0.5
        double sum = 0.0;
        for (int n = 0; n <= 2 * c; n += 2)
            sum += h[(size_t) n];
        for (int n = 0; n <= 2 * c; n += 2)
            h[(size_t) n] *= 0.5 / sum;
        return h;
    }

    // Number of half-band taps (c) per stage: stage 1 at 0.05 transition, then the relaxed later stages.
    int halfBandCentre (int stage)
    {
        return stage == 0 ? 69 : (stage == 1 ? 19 : 13);
    }

    //--------------------------------------------------------------------------
    // Elliptic polyphase IIR half-band design (coefficients of the first-order all-passes in z^2).
    void transitionParams (double transition, double& k, double& q)
    {
        k = std::tan ((1.0 - transition * 2.0) * kPi / 4.0);
        k *= k;
        const double kksqrt = std::pow (1.0 - k * k, 0.25);
        const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
        const double e2 = e * e;
        const double e4 = e2 * e2;
        q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    }

    int iirOrderFor (double attenuationDb, double q)
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
        double acc = 0.0, term = 0.0;
        int sign = 1;
        for (int i = 0; i < 100; ++i)
        {
            term = std::pow (q, (double) i * (i + 1)) * std::sin ((i * 2 + 1) * c * kPi / order) * sign;
            acc += term;
            sign = -sign;
            if (std::abs (term) < 1.0e-100)
                break;
        }
        return acc;
    }

    double accDen (double q, int order, int c)
    {
        double acc = 0.0, term = 0.0;
        int sign = -1;
        for (int i = 1; i < 100; ++i)
        {
            term = std::pow (q, (double) i * i) * std::cos (i * 2 * c * kPi / order) * sign;
            acc += term;
            sign = -sign;
            if (std::abs (term) < 1.0e-100)
                break;
        }
        return acc;
    }

    // transition: half width of the transition band relative to the high rate (pass < 0.25 - t, stop > 0.25 + t).
    std::vector<double> iirHalfBand (double transition, double attenuationDb)
    {
        double k = 0.0, q = 0.0;
        transitionParams (transition, k, q);
        const int order = iirOrderFor (attenuationDb, q);
        const int numCoefs = (order - 1) / 2;

        std::vector<double> coefs ((size_t) numCoefs);
        for (int i = 0; i < numCoefs; ++i)
        {
            const int c = i + 1;
            const double num = accNum (q, order, c) * std::pow (q, 0.25);
            const double den = accDen (q, order, c) + 0.5;
            const double ww = num / den;
            const double wwsq = ww * ww;
            const double x = std::sqrt ((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
            coefs[(size_t) i] = (1.0 - x) / (1.0 + x);
        }
        return coefs;
    }

    // Response of the IIR half band H(z) = 0.5 (A0(z^2) + z^-1 A1(z^2)) at w (rad/sample of the high rate);
    // used for the latency estimate.
    double iirPhase (const std::vector<double>& coefs, double w)
    {
        auto apPhase = [] (double c, double ww)
        {
            // (c + z^-2) / (1 + c z^-2)
            const double re = c + std::cos (2.0 * ww), im = -std::sin (2.0 * ww);
            const double dr = 1.0 + c * std::cos (2.0 * ww), di = -c * std::sin (2.0 * ww);
            return std::atan2 (im, re) - std::atan2 (di, dr);
        };
        double p0 = 0.0, p1 = -w;
        for (size_t i = 0; i < coefs.size(); ++i)
            ((i & 1) == 0 ? p0 : p1) += apPhase (coefs[i], w);

        const double re = 0.5 * (std::cos (p0) + std::cos (p1)), im = 0.5 * (std::sin (p0) + std::sin (p1));
        return std::atan2 (im, re);
    }

    //--------------------------------------------------------------------------
    struct FirStage
    {
        int c = 0;                     // centre (odd)
        int numDense = 0;              // dense taps h[0], h[2], ..., h[2c]
        std::vector<double> dense;     // dense[j] = h[2j] (sum 0.5)
        // per channel histories
        std::vector<std::vector<float>> upHist, downHist;
        int upLen = 0, downLen = 0;    // history lengths in input samples

        void prepare (int centre, int maxInput, int numChannels)
        {
            c = centre;
            const auto h = halfBand (c);
            numDense = c + 1;
            dense.resize ((size_t) numDense);
            for (int j = 0; j < numDense; ++j)
                dense[(size_t) j] = h[(size_t) (2 * j)];

            upLen = numDense - 1;          // taps reach back numDense - 1 base samples
            downLen = 2 * c;               // reach back 2c high-rate samples
            upHist.assign ((size_t) numChannels, std::vector<float> ((size_t) (upLen + maxInput), 0.0f));
            downHist.assign ((size_t) numChannels, std::vector<float> ((size_t) (downLen + 2 * maxInput), 0.0f));
        }

        void reset()
        {
            for (auto& v : upHist) std::fill (v.begin(), v.end(), 0.0f);
            for (auto& v : downHist) std::fill (v.begin(), v.end(), 0.0f);
        }

        // y[2n] = 2 sum_j dense[j] x[n - j], y[2n + 1] = x[n - (c - 1) / 2]
        void up (int ch, const float* in, float* out, int n) noexcept
        {
            float* buf = upHist[(size_t) ch].data();
            std::memcpy (buf + upLen, in, sizeof (float) * (size_t) n);
            const int d = (c - 1) / 2;

            for (int i = 0; i < n; ++i)
            {
                const float* x = buf + upLen + i;   // x[0] = current, x[-j] = past
                double acc = 0.0;
                for (int j = 0; j < numDense; ++j)
                    acc += (double) dense[(size_t) j] * (double) x[-j];
                out[2 * i] = (float) (2.0 * acc);
                out[2 * i + 1] = x[-d];
            }
            std::memmove (buf, buf + n, sizeof (float) * (size_t) upLen);
        }

        // y[n] = sum_j dense[j] v[2n - 2j] + 0.5 v[2n - c]
        void down (int ch, const float* in, float* out, int n) noexcept
        {
            float* buf = downHist[(size_t) ch].data();
            std::memcpy (buf + downLen, in, sizeof (float) * (size_t) (2 * n));

            for (int i = 0; i < n; ++i)
            {
                const float* v = buf + downLen + 2 * i;   // v[0] = sample 2n
                double acc = 0.5 * (double) v[-c];
                for (int j = 0; j < numDense; ++j)
                    acc += (double) dense[(size_t) j] * (double) v[-2 * j];
                out[i] = (float) acc;
            }
            std::memmove (buf, buf + 2 * n, sizeof (float) * (size_t) downLen);
        }
    };

    //--------------------------------------------------------------------------
    struct IirStage
    {
        std::vector<double> coefs;
        int numCoefs = 0;
        struct Chain { double x[16] {}, y[16] {}; };
        std::vector<Chain> upA, upB, downA, downB;   // per channel; A = even coefficients, B = odd

        void prepare (double transition, int numChannels)
        {
            coefs = iirHalfBand (transition, kAttenuationDb);
            numCoefs = std::min ((int) coefs.size(), 32);
            upA.assign ((size_t) numChannels, {});
            upB.assign ((size_t) numChannels, {});
            downA.assign ((size_t) numChannels, {});
            downB.assign ((size_t) numChannels, {});
        }

        void reset()
        {
            for (auto* v : { &upA, &upB, &downA, &downB })
                for (auto& c : *v)
                    c = Chain {};
        }

        // y = c (x - y1) + x1, per coefficient of one chain (parity 0 or 1)
        inline double runChain (Chain& ch, int parity, double s) const noexcept
        {
            int slot = 0;
            for (int i = parity; i < numCoefs; i += 2, ++slot)
            {
                const double y = coefs[(size_t) i] * (s - ch.y[slot]) + ch.x[slot];
                ch.x[slot] = s;
                ch.y[slot] = y;
                s = y;
            }
            return s;
        }

        static void flushChain (Chain& c) noexcept
        {
            for (int i = 0; i < 16; ++i)
            {
                if (std::abs (c.x[i]) < 1.0e-30) c.x[i] = 0.0;
                if (std::abs (c.y[i]) < 1.0e-30) c.y[i] = 0.0;
            }
        }

        void up (int ch, const float* in, float* out, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const double x = in[i];
                out[2 * i] = (float) runChain (upA[(size_t) ch], 0, x);
                out[2 * i + 1] = (float) runChain (upB[(size_t) ch], 1, x);
            }
            flushChain (upA[(size_t) ch]);
            flushChain (upB[(size_t) ch]);
        }

        void down (int ch, const float* in, float* out, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const double a = runChain (downA[(size_t) ch], 0, in[2 * i + 1]);
                const double b = runChain (downB[(size_t) ch], 1, in[2 * i]);
                out[i] = (float) (0.5 * (a + b));
            }
            flushChain (downA[(size_t) ch]);
            flushChain (downB[(size_t) ch]);
        }
    };
}

//==============================================================================
struct Oversampler::Impl
{
    int stages = 1, factor = 2, numChannels = 2, maxBlock = 0, latency = 0;
    Quality quality = Quality::LinearPhase;
    FirStage fir[3];
    IirStage iir[3];

    // buffers[s][ch]: output of stage s (length maxBlock << (s + 1))
    std::vector<std::vector<float>> buffers[3];
    std::vector<float*> topPointers;

    // top-rate padding delay (linear phase)
    int pad = 0;
    std::vector<std::vector<float>> padHist;
};

Oversampler::Oversampler() : impl (std::make_unique<Impl>()) {}
Oversampler::~Oversampler() = default;

void Oversampler::prepare (int factorLog2, Quality quality, int maxBlockSize, int numChannels)
{
    auto& m = *impl;
    m.stages = std::clamp (factorLog2, 1, 3);
    m.factor = 1 << m.stages;
    m.quality = quality;
    m.numChannels = std::max (1, numChannels);
    m.maxBlock = std::max (1, maxBlockSize);

    for (int s = 0; s < m.stages; ++s)
    {
        const int inLen = m.maxBlock << s;
        if (quality == Quality::LinearPhase)
            m.fir[s].prepare (halfBandCentre (s), inLen, m.numChannels);
        else
            m.iir[s].prepare (s == 0 ? 0.025 : (s == 1 ? 0.1125 : 0.18), m.numChannels);

        m.buffers[s].assign ((size_t) m.numChannels, std::vector<float> ((size_t) (inLen * 2), 0.0f));
    }

    m.topPointers.resize ((size_t) m.numChannels);
    for (int ch = 0; ch < m.numChannels; ++ch)
        m.topPointers[(size_t) ch] = m.buffers[m.stages - 1][(size_t) ch].data();

    if (quality == Quality::LinearPhase)
    {
        // delay of each stage, up + down, in top-rate samples
        int total = 0;
        for (int s = 0; s < m.stages; ++s)
            total += 2 * m.fir[s].c * (1 << (m.stages - 1 - s));
        const int rounded = (total + m.factor - 1) / m.factor * m.factor;
        m.pad = rounded - total;
        m.latency = rounded / m.factor;
    }
    else
    {
        // Low-frequency group delay of the round trip, in base-rate samples. Stage s runs at 2^(s+1) x the base rate;
        // its interpolator is H(z) (gd high-rate samples), its decimator keeps the filtered sample at odd index 2n + 1
        // (A0 on the odd inputs, A1 on the even ones), i.e. it is one high-rate sample early: gd - 1.
        double delay = 0.0;
        for (int s = 0; s < m.stages; ++s)
        {
            const double w = 1.0e-3;   // rad/sample at the stage's high rate
            const double gd = -(iirPhase (m.iir[s].coefs, 2.0 * w) - iirPhase (m.iir[s].coefs, w)) / w;
            delay += (2.0 * gd - 1.0) / (double) (2 << s);
        }
        m.pad = 0;
        m.latency = (int) std::lround (delay);
    }

    m.padHist.assign ((size_t) m.numChannels, std::vector<float> ((size_t) (m.pad + m.maxBlock * m.factor), 0.0f));
    reset();
}

void Oversampler::reset() noexcept
{
    auto& m = *impl;
    for (int s = 0; s < 3; ++s)
    {
        m.fir[s].reset();
        m.iir[s].reset();
    }
    for (auto& v : m.padHist)
        std::fill (v.begin(), v.end(), 0.0f);
}

int Oversampler::getFactor() const noexcept { return impl->factor; }
int Oversampler::getLatencySamples() const noexcept { return impl->latency; }

float* const* Oversampler::upsample (const float* const* input, int numSamples) noexcept
{
    auto& m = *impl;
    numSamples = std::clamp (numSamples, 0, m.maxBlock);

    for (int ch = 0; ch < m.numChannels; ++ch)
    {
        const float* src = input[ch];
        int n = numSamples;
        for (int s = 0; s < m.stages; ++s)
        {
            float* dst = m.buffers[s][(size_t) ch].data();
            if (m.quality == Quality::LinearPhase)
                m.fir[s].up (ch, src, dst, n);
            else
                m.iir[s].up (ch, src, dst, n);
            src = dst;
            n *= 2;
        }

        if (m.pad > 0)
        {
            float* hist = m.padHist[(size_t) ch].data();
            float* top = m.buffers[m.stages - 1][(size_t) ch].data();
            std::memcpy (hist + m.pad, top, sizeof (float) * (size_t) n);
            std::memcpy (top, hist, sizeof (float) * (size_t) n);
            std::memmove (hist, hist + n, sizeof (float) * (size_t) m.pad);
        }
    }

    return m.topPointers.data();
}

void Oversampler::downsample (float* const* output, int numSamples) noexcept
{
    auto& m = *impl;
    numSamples = std::clamp (numSamples, 0, m.maxBlock);

    for (int ch = 0; ch < m.numChannels; ++ch)
    {
        // top stage first; intermediate results go into the lower stage buffers (no longer needed)
        const float* src = m.buffers[m.stages - 1][(size_t) ch].data();
        for (int s = m.stages - 1; s >= 0; --s)
        {
            const int nOut = numSamples << s;
            float* dst = s == 0 ? output[ch] : m.buffers[s - 1][(size_t) ch].data();

            if (m.quality == Quality::LinearPhase)
                m.fir[s].down (ch, src, dst, nOut);
            else
                m.iir[s].down (ch, src, dst, nOut);
            src = dst;
        }
    }
}
} // namespace ps
