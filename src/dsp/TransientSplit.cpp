#include "dsp/TransientSplit.h"

#include <algorithm>
#include <cmath>

// Transient / sustain splitter (see TransientSplit.h). Choices beyond the header: the control rate is one tick per 8
// samples at every sample rate (the envelopes run per sample, so only the mask target is sampled); the baseline
// tracker rises with a 2 s and falls with a 0.2 s time constant and is clamped to 0..12 dB; the silence gate is a
// slow-envelope power below 1e-9 (-90 dBFS).
namespace ps
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kCrossoverHz[3] { 120.0, 800.0, 5000.0 };
constexpr double kBandScale[TransientSplitter::kNumBands] { 2.0, 1.25, 0.75, 0.5 };
constexpr double kFastAttackMs[TransientSplitter::kNumBands] { 1.0, 0.5, 0.25, 0.15 };
constexpr double kMaskRiseMs = 0.3, kMaskFallMs = 8.0;
constexpr double kEps = 1.0e-12, kSilence = 1.0e-9;
constexpr int kTick = 8;
constexpr double kSlowAttackScale = 3.0;   // slow envelope attack = 3 x length x band scale

double coeffFor (double ms, double sampleRate) noexcept
{
    return 1.0 - std::exp (-1.0 / std::max (1.0e-9, ms * 0.001 * sampleRate));
}

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
    return 64 * std::max (1, (int) std::lround (rate / 48000.0));
}

void TransientSplitter::prepare (double sampleRate, int /*maxBlockSize*/)
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    latency = useLookahead ? latencyFor (sr) : 0;

    int size = 2;
    while (size < latency + 2)
        size <<= 1;
    for (auto& r : ring)
        r.assign ((size_t) size, 0.0f);
    ringMask = size - 1;

    for (int i = 0; i < 3; ++i)
    {
        const double hz = std::min (kCrossoverHz[i], 0.45 * sr);
        detect[i].design (hz, sr);
        audio[i].design (hz, sr);
    }

    rise = coeffFor (kMaskRiseMs, sr);
    baselineUp = 1.0 - std::exp (-(double) kTick / (2.0 * sr));
    baselineDown = 1.0 - std::exp (-(double) kTick / (0.2 * sr));
    paramGlide = 1.0 - std::exp (-(double) kTick / (0.05 * sr / 3.0));   // ~95 % after 50 ms
    for (int b = 0; b < kNumBands; ++b)
    {
        bands[b].fastAtt = coeffFor (kFastAttackMs[b], sr);
        bands[b].fall = coeffFor (kMaskFallMs * kBandScale[b], sr);
    }

    prepared = true;
    reset();
}

void TransientSplitter::reset() noexcept
{
    for (auto& r : ring)
        std::fill (r.begin(), r.end(), 0.0f);
    ringPos = 0;
    for (int i = 0; i < 3; ++i)
    {
        detect[i].clear();
        audio[i].clear();
    }
    for (auto& b : bands)
    {
        b.fast = b.slow = b.baseline = 0.0;
        b.gain = b.target = b.targetStep = b.interp = 0.0;
    }
    tickPos = 0;
    activity = 0.0f;
    energyT = energyX = 0.0;
    fresh = true;
    lenUsed = -1.0;
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
    const double s01 = sens / 100.0;
    t0 = 9.0 - 7.5 * s01;
    t1 = t0 + 9.0 - 6.0 * s01;

    if (! force && std::abs (len - lenUsed) <= 1.0e-4 * len)
        return;
    lenUsed = len;
    for (int b = 0; b < kNumBands; ++b)
    {
        bands[b].slowAtt = coeffFor (kSlowAttackScale * len * kBandScale[b], sr);
        bands[b].release = coeffFor (2.5 * len * kBandScale[b], sr);
    }
}

void TransientSplitter::controlTick() noexcept
{
    sens += paramGlide * (sensTarget - sens);
    len += paramGlide * (lenTarget - len);
    if (std::abs (sens - sensTarget) < 1.0e-6) sens = sensTarget;
    if (std::abs (len - lenTarget) < 1.0e-6) len = lenTarget;
    updateCoefficients (false);

    const double span = std::max (1.0e-6, t1 - t0);
    for (auto& b : bands)
    {
        const double delta = 10.0 * std::log10 ((b.fast + kEps) / (b.slow + kEps));
        b.baseline += (delta > b.baseline ? baselineUp : baselineDown) * (delta - b.baseline);
        b.baseline = std::clamp (b.baseline, 0.0, 12.0);
        double d = std::max (0.0, delta - b.baseline);
        if (b.slow < kSilence)
            d = 0.0;
        const double u = std::clamp ((d - t0) / span, 0.0, 1.0);
        const double target = u * u * (3.0 - 2.0 * u);
        b.target = target;
        b.targetStep = (target - b.interp) / (double) kTick;
    }
}

float TransientSplitter::getBandMask (int band) const noexcept
{
    return band >= 0 && band < kNumBands ? (float) bands[band].gain : 0.0f;
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
        const float xl = in[0][i], xr = in[1][i];

        // detection bank (undelayed input)
        double band[2][kNumBands];
        const double x[2] { (double) xl, (double) xr };
        for (int c = 0; c < 2; ++c)
        {
            const double lo1 = detect[0].lowPass (c, x[c]);
            const double r1 = x[c] - lo1;
            const double lo2 = detect[1].lowPass (c, r1);
            const double r2 = r1 - lo2;
            const double lo3 = detect[2].lowPass (c, r2);
            band[c][0] = lo1;
            band[c][1] = lo2;
            band[c][2] = lo3;
            band[c][3] = r2 - lo3;
        }

        bool anyMask = false;
        for (int b = 0; b < kNumBands; ++b)
        {
            auto& s = bands[b];
            const double p = 0.5 * (band[0][b] * band[0][b] + band[1][b] * band[1][b]);
            s.fast += (p > s.fast ? s.fastAtt : s.release) * (p - s.fast);
            s.slow += (p > s.slow ? s.slowAtt : s.release) * (p - s.slow);
        }

        if (++tickPos >= kTick)
        {
            tickPos = 0;
            controlTick();
        }

        for (int b = 0; b < kNumBands; ++b)
        {
            auto& s = bands[b];
            if (s.targetStep != 0.0)
            {
                s.interp += s.targetStep;
                if ((s.targetStep > 0.0 && s.interp >= s.target) || (s.targetStep < 0.0 && s.interp <= s.target))
                {
                    s.interp = s.target;
                    s.targetStep = 0.0;
                }
            }
            if (s.gain != s.interp)
            {
                s.gain += (s.interp > s.gain ? rise : s.fall) * (s.interp - s.gain);
                if (s.gain < 1.0e-6 && s.interp <= 0.0)
                    s.gain = 0.0;
                else if (s.gain > 1.0 - 1.0e-6 && s.interp >= 1.0)
                    s.gain = 1.0;
            }
            anyMask = anyMask || s.gain != 0.0;
            peakMask = std::max (peakMask, s.gain);
        }

        // lookahead delay of the audio
        rl[ringPos] = xl;
        rr[ringPos] = xr;
        const int rp = (ringPos - latency) & ringMask;
        const float dl = rl[rp], dr = rr[rp];
        ringPos = (ringPos + 1) & ringMask;

        // audio bank (always run, so its state is valid when a mask rises)
        const double xd[2] { (double) dl, (double) dr };
        float t[2] { 0.0f, 0.0f };
        for (int c = 0; c < 2; ++c)
        {
            const double lo1 = audio[0].lowPass (c, xd[c]);
            const double r1 = xd[c] - lo1;
            const double lo2 = audio[1].lowPass (c, r1);
            const double r2 = r1 - lo2;
            const double lo3 = audio[2].lowPass (c, r2);
            if (anyMask)
                t[c] = (float) (bands[0].gain * lo1 + bands[1].gain * lo2 + bands[2].gain * lo3 + bands[3].gain * (r2 - lo3));
        }

        outT[0][i] = t[0];
        outT[1][i] = t[1];
        outS[0][i] = anyMask ? dl - t[0] : dl;
        outS[1][i] = anyMask ? dr - t[1] : dr;
        eT += (double) t[0] * (double) t[0] + (double) t[1] * (double) t[1];
        eX += (double) dl * (double) dl + (double) dr * (double) dr;
    }

    for (int i = 0; i < 3; ++i)
    {
        detect[i].flush();
        audio[i].flush();
    }
    for (auto& b : bands)
    {
        b.fast = flushTiny (b.fast);
        b.slow = flushTiny (b.slow);
    }

    activity = (float) peakMask;
    energyT = eT;
    energyX = eX;
}
} // namespace ps
