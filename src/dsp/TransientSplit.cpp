#include "dsp/TransientSplit.h"

#include <algorithm>
#include <cmath>

// Transient / sustain splitter (see TransientSplit.h). Choices beyond the header: the envelopes run per sample; the
// band states, the combination and the mask run once per control tick (8 samples at 44.1 / 48 kHz, scaled with the
// rate family so the time resolution stays ~0.17 ms) and the mask is interpolated linearly between ticks. The detector
// constants were fitted (black-box) on measured mask traces of tone bursts, level steps, noise, drums and music.
namespace ps
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kCrossoverHz[TransientSplitter::kNumBands - 1] { 120.0, 800.0, 5000.0 };
constexpr double kFastAttackMs = 4.88, kFastReleaseMs = 0.994;   // F: band power, short smoothing
constexpr double kSlowAttackMs = 157.4, kSlowReleaseMs = 61.5;   // W: the steady level an onset is measured against
constexpr double kFloor = 4.78e-10;                              // e: -93 dB of power
constexpr double kSpanDb = 8.04;                                 // dB of F over W for a full transient (sensitivity 50)
constexpr double kFallWeight = 0.132;                            // a falling band (F < W) pulls its state down
constexpr double kHoldMs = 121.6;                                // release of the transient state (length 20)
constexpr double kWeightPower = 0.323;                           // band weight (F + e)^a
constexpr double kMeanPower = 0.646;                             // power mean over the bands
constexpr double kLookaheadMs = 0.206;
constexpr double kLn10Over10 = 0.23025850929940458;             // dB = ln (ratio) / kLn10Over10

double coeffFor (double ms, double sampleRate) noexcept
{
    return 1.0 - std::exp (-1.0 / std::max (1.0e-9, ms * 0.001 * sampleRate));
}

int tickFor (double sampleRate) noexcept { return 8 * std::max (1, (int) std::lround (sampleRate / 48000.0)); }

inline double flushTiny (double v) noexcept { return std::abs (v) < 1.0e-30 ? 0.0 : v; }
} // namespace

//==============================================================================
void TransientSplitter::Svf::design (double hz, double sampleRate) noexcept
{
    const double g = std::tan (kPi * hz / sampleRate);
    const double k = 1.4142135623730951;   // 1 / Q, Q = 0.7071 (Butterworth)
    a1 = 1.0 / (1.0 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
}

double TransientSplitter::Svf::lowPass (int ch, double x) noexcept
{
    const double v3 = x - ic2[ch];
    const double v1 = a1 * ic1[ch] + a2 * v3;
    const double v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3;
    ic1[ch] = 2.0 * v1 - ic1[ch];
    ic2[ch] = 2.0 * v2 - ic2[ch];
    return v2;
}

void TransientSplitter::Svf::clear() noexcept
{
    ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0;
}

void TransientSplitter::Svf::flush() noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        ic1[c] = flushTiny (ic1[c]);
        ic2[c] = flushTiny (ic2[c]);
    }
}

//==============================================================================
TransientSplitter::TransientSplitter (bool lookahead) noexcept : useLookahead (lookahead) {}

int TransientSplitter::latencyFor (double sampleRate) noexcept
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    return (int) std::lround (kLookaheadMs * 0.001 * rate);
}

void TransientSplitter::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    latency = useLookahead ? latencyFor (sr) : 0;
    tick = tickFor (sr);

    int size = 2;
    while (size < latency + 2)
        size <<= 1;
    for (auto& r : ring)
        r.assign ((size_t) size, 0.0f);
    ringMask = size - 1;

    for (int i = 0; i < kNumBands - 1; ++i)
        detect[i].design (std::min (kCrossoverHz[i], 0.45 * sr), sr);

    fastAtt = coeffFor (kFastAttackMs, sr);
    fastRel = coeffFor (kFastReleaseMs, sr);
    slowAtt = coeffFor (kSlowAttackMs, sr);
    slowRel = coeffFor (kSlowReleaseMs, sr);
    paramGlide = 1.0 - std::exp (-(double) tick / (0.05 * sr / 3.0));   // ~95 % after 50 ms

    prepared = true;
    reset();
}

void TransientSplitter::reset() noexcept
{
    for (auto& r : ring)
        std::fill (r.begin(), r.end(), 0.0f);
    ringPos = 0;
    for (auto& d : detect)
        d.clear();
    for (auto& b : bands)
        b = Band {};
    for (int c = 0; c < 2; ++c)
        mask[c] = maskStep[c] = 0.0;
    tickPos = 0;
    activity = 0.0f;
    energyT = energyX = 0.0;
    fresh = true;
    sens = sensTarget;
    len = lenTarget;
    updateCoefficients (true);
}

void TransientSplitter::setParams (float sensitivityPercent, float lengthMs) noexcept
{
    sensTarget = std::clamp (std::isfinite (sensitivityPercent) ? (double) sensitivityPercent : 50.0, 0.0, 100.0);
    lenTarget = std::clamp (std::isfinite (lengthMs) ? (double) lengthMs : 20.0, 5.0, 100.0);
    if (fresh)
    {
        sens = sensTarget;
        len = lenTarget;
        updateCoefficients (true);
        fresh = false;
    }
}

void TransientSplitter::updateCoefficients (bool force) noexcept
{
    if (! force && sens == sensUsed && len == lenUsed)
        return;
    sensUsed = sens;
    lenUsed = len;
    invSpan = 1.0 / (kSpanDb * std::exp2 ((50.0 - sens) / 50.0));
    holdDecay = std::exp (-(double) tick / (kHoldMs * (len / 20.0) * 0.001 * sr));
}

float TransientSplitter::getMask (int channel) const noexcept
{
    return channel >= 0 && channel < 2 ? (float) mask[channel] : 0.0f;
}

void TransientSplitter::controlTick() noexcept
{
    sens += paramGlide * (sensTarget - sens);
    len += paramGlide * (lenTarget - len);
    if (std::abs (sens - sensTarget) < 1.0e-6) sens = sensTarget;
    if (std::abs (len - lenTarget) < 1.0e-6) len = lenTarget;
    updateCoefficients (false);

    for (int c = 0; c < 2; ++c)
    {
        double num = 0.0, den = 0.0;
        for (auto& b : bands)
        {
            const double r = std::log ((b.fast[c] + kFloor) / (b.slow[c] + kFloor)) / kLn10Over10 * invSpan;
            b.held[c] = std::max (r, b.held[c] * holdDecay);
            if (b.held[c] < 1.0e-9)
                b.held[c] = 0.0;
            const double cb = std::clamp (b.held[c] + kFallWeight * std::min (r, 0.0), 0.0, 1.0);
            const double w = std::pow (b.fast[c] + kFloor, kWeightPower);
            num += w * std::pow (cb, kMeanPower);
            den += w;
        }
        const double m = num > 0.0 ? std::pow (num / den, 1.0 / kMeanPower) : 0.0;
        target[c] = std::clamp (m, 0.0, 1.0);
        maskStep[c] = (target[c] - mask[c]) / (double) tick;
    }
}

void TransientSplitter::process (const float* const* in, float* const* outT, float* const* outS, int n) noexcept
{
    if (! prepared || n <= 0)
        return;

    double peakMask = 0.0, eT = 0.0, eX = 0.0;
    float* rl = ring[0].data();
    float* rr = ring[1].data();

    for (int i = 0; i < n; ++i)
    {
        const float xs[2] { in[0][i], in[1][i] };

        // detection bank (undelayed input), per channel
        for (int c = 0; c < 2; ++c)
        {
            double r = (double) xs[c];
            for (int b = 0; b < kNumBands; ++b)
            {
                double y = r;
                if (b < kNumBands - 1)
                {
                    y = detect[b].lowPass (c, r);
                    r -= y;
                }
                auto& s = bands[b];
                const double p = y * y;
                s.fast[c] += (p > s.fast[c] ? fastAtt : fastRel) * (p - s.fast[c]);
                s.slow[c] += (p > s.slow[c] ? slowAtt : slowRel) * (p - s.slow[c]);
            }
        }

        if (++tickPos >= tick)
        {
            tickPos = 0;
            controlTick();
        }

        // lookahead delay of the audio
        rl[ringPos] = xs[0];
        rr[ringPos] = xs[1];
        const int rp = (ringPos - latency) & ringMask;
        const float xd[2] { rl[rp], rr[rp] };
        ringPos = (ringPos + 1) & ringMask;

        for (int c = 0; c < 2; ++c)
        {
            if (maskStep[c] != 0.0)
            {
                mask[c] += maskStep[c];
                if ((maskStep[c] > 0.0 && mask[c] >= target[c]) || (maskStep[c] < 0.0 && mask[c] <= target[c]))
                {
                    mask[c] = target[c];
                    maskStep[c] = 0.0;
                }
            }
            const float t = mask[c] > 0.0 ? (float) (mask[c] * (double) xd[c]) : 0.0f;
            outT[c][i] = t;
            outS[c][i] = xd[c] - t;
            eT += (double) t * (double) t;
            eX += (double) xd[c] * (double) xd[c];
            peakMask = std::max (peakMask, mask[c]);
        }
    }

    for (auto& d : detect)
        d.flush();
    for (auto& b : bands)
        for (int c = 0; c < 2; ++c)
        {
            b.fast[c] = flushTiny (b.fast[c]);
            b.slow[c] = flushTiny (b.slow[c]);
        }

    activity = (float) peakMask;
    energyT = eT;
    energyX = eX;
}
} // namespace ps
