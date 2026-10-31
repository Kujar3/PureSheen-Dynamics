#include "dsp/MaximizerStages.h"

#include <algorithm>
#include <cstring>

// Maximizer building blocks (dsp-maximizer). See MaximizerStages.h for the gain construction they share.
namespace ps::maxi
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double halfX = 0.5 * x;
    for (int k = 1; k < 64; ++k)
    {
        const double f = halfX / k;
        term *= f * f;
        sum += term;
        if (term < 1.0e-14 * sum)
            break;
    }
    return sum;
}

double sinc (double x) noexcept { return std::abs (x) < 1.0e-12 ? 1.0 : std::sin (kPi * x) / (kPi * x); }

double kaiser (double r, double beta) noexcept
{
    return besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (beta);
}
} // namespace

//==============================================================================
void MaxDeque::prepare (int capacity)
{
    const int size = nextPow2 (std::max (4, capacity));
    vals.assign ((size_t) size, 0.0);
    stamps.assign ((size_t) size, 0);
    mask = (unsigned) size - 1u;
    reset();
}

//==============================================================================
void BoxAverage::prepare (int maxLength)
{
    maxLen = std::max (1, maxLength);
    const int size = nextPow2 (maxLen + 2);
    ring.assign ((size_t) size, 1.0);
    mask = size - 1;
    len = std::min (len, maxLen);
    reset();
}

void BoxAverage::reset() noexcept
{
    std::fill (ring.begin(), ring.end(), 1.0);
    pos = 0;
    sinceResync = 0;
    sum = (double) len;
    invLen = 1.0 / (double) len;
}

void BoxAverage::setLength (int newLength) noexcept
{
    newLength = std::clamp (newLength, 1, maxLen);
    if (newLength == len)
        return;
    len = newLength;
    invLen = 1.0 / (double) len;
    resync();
}

void BoxAverage::resync() noexcept
{
    double s = 0.0;
    for (int i = 0; i < len; ++i)
        s += ring[(size_t) ((pos - i) & mask)];
    sum = s;
    sinceResync = 0;
}

//==============================================================================
void LimiterCore::prepare (double sampleRate, double maxLookaheadMs)
{
    sr = sampleRate;
    controlStep = 1.0 / std::max (1.0, msToSamples (20.0, sr));
    maxLookahead = std::max (2, (int) std::lround (msToSamples (maxLookaheadMs, sr)));
    maxHold = (int) std::lround (msToSamples (24.0, sr));
    const int sigSize = nextPow2 (maxLookahead + kMidDetectorDelay + 8);
    const int redSize = nextPow2 (maxLookahead + 4);
    sigMask = sigSize - 1;
    redMask = redSize - 1;
    for (int c = 0; c < 2; ++c)
    {
        sig[(size_t) c].assign ((size_t) sigSize, 0.0f);
        red[(size_t) c].assign ((size_t) redSize, 0.0);
        windowMax[(size_t) c].prepare (maxLookahead + maxHold + 8);
        box[(size_t) c].prepare (maxLookahead + 1);
    }

    // half-sample interpolator (8 taps, Kaiser-windowed sinc, unity DC): estimates x (m + 0.5) from x[m - 3 .. m + 4]
    double s = 0.0;
    for (int j = 0; j < 8; ++j)
    {
        const double d = (double) j - 3.5;
        midTaps[(size_t) j] = sinc (d) * kaiser (d / 4.0, 5.0);
        s += midTaps[(size_t) j];
    }
    for (auto& v : midTaps)
        v /= s;

    kEnvFastRel = decayCoeff (msToSamples (8.0, sr));
    kEnvSlowAtt = chargeCoeff (msToSamples (6.0, sr));
    kEnvSlowRel = chargeCoeff (msToSamples (120.0, sr));
    kLf = chargeCoeff (sr / (2.0 * kPi * 150.0));
    kPower = chargeCoeff (msToSamples (150.0, sr));

    lookahead = std::min (lookahead, maxLookahead);
    settingsValid = false;
    setSettings (settings);
    reset();
}

void LimiterCore::setLookahead (int samples) noexcept
{
    lookahead = std::clamp (samples, 1, maxLookahead);
    settingsValid = false;
    setSettings (settings);
}

void LimiterCore::reset() noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        std::fill (sig[(size_t) c].begin(), sig[(size_t) c].end(), 0.0f);
        std::fill (red[(size_t) c].begin(), red[(size_t) c].end(), 0.0);
        windowMax[(size_t) c].reset();
        minGain[c] = 1.0;
    }
    slow.fill (0.0);
    fast.fill (0.0);
    siTransNow = settings.siTrans;
    siSusNow = settings.siSus;
    preserveNow = settings.transientPreserve;
    envFast = envSlow = 0.0;
    lfState = lfPower = allPower = lfRatio = 0.0;
    attack = attackTarget;
    hold = holdTarget;
    for (auto& b : box)
    {
        b.setLength (attack);
        b.reset();
    }
    t = -1;
    k = (t - kMidDetectorDelay) - (lookahead - attack + 1);
    updateCoefficients();
}

void LimiterCore::setSettings (const CoreSettings& s) noexcept
{
    const bool same = settingsValid && s.attackMs == settings.attackMs && s.holdMs == settings.holdMs
                      && s.fastReleaseMs == settings.fastReleaseMs && s.slowReleaseMs == settings.slowReleaseMs
                      && s.slowChargeMs == settings.slowChargeMs && s.lfAdapt == settings.lfAdapt
                      && s.transientPreserve == settings.transientPreserve && s.siTrans == settings.siTrans
                      && s.siSus == settings.siSus && s.midDetect == settings.midDetect;
    settings = s;
    if (same)
        return;
    settingsValid = true;
    attackTarget = std::clamp ((int) std::lround (msToSamples (s.attackMs, sr)), 1, lookahead);
    kSlowCharge = s.slowChargeMs > 0.0 ? chargeCoeff (msToSamples (s.slowChargeMs, sr)) : 0.0;   // 0: no slow stage
    kSlowRelease = chargeCoeff (msToSamples (s.slowReleaseMs, sr));
    kFastBase = msToSamples (s.fastReleaseMs, sr);
    updateCoefficients();
}

void LimiterCore::updateCoefficients() noexcept
{
    const double lf = settings.lfAdapt * std::clamp ((lfRatio - 0.3) / 0.5, 0.0, 1.0);
    kFast = decayCoeff (kFastBase * (1.0 + 3.0 * lf));
    holdTarget = std::clamp ((int) std::lround (msToSamples (settings.holdMs + 8.0 * lf, sr)), 0, maxHold);
}

void LimiterCore::process (const float* const* in, float* const* out, const double* thr, int n) noexcept
{
    const bool mid = settings.midDetect;
    float* s0 = sig[0].data();
    float* s1 = sig[1].data();
    double* r0 = red[0].data();
    double* r1 = red[1].data();

    for (int i = 0; i < n; ++i)
    {
        const double preserve = rampControl (preserveNow, settings.transientPreserve, controlStep);
        const double siT = rampControl (siTransNow, settings.siTrans, controlStep);
        const double siS = rampControl (siSusNow, settings.siSus, controlStep);
        ++t;
        s0[t & sigMask] = in[0][i];
        s1[t & sigMask] = in[1][i];
        const long long m = t - kMidDetectorDelay;
        const double x0 = s0[m & sigMask], x1 = s1[m & sigMask];
        double a0 = std::abs (x0), a1 = std::abs (x1);
        if (mid)
        {
            double y0 = 0.0, y1 = 0.0;
            for (int j = 0; j < 8; ++j)
            {
                const long long idx = (m - 3 + j) & sigMask;
                y0 += midTaps[(size_t) j] * (double) s0[idx];
                y1 += midTaps[(size_t) j] * (double) s1[idx];
            }
            a0 = std::max (a0, std::abs (y0));
            a1 = std::max (a1, std::abs (y1));
        }

        const double la = std::max (a0, a1);
        envFast = std::max (la, envFast * kEnvFastRel);
        envSlow += (la - envSlow) * (la > envSlow ? kEnvSlowAtt : kEnvSlowRel);
        const double mono = 0.5 * (x0 + x1);
        lfState += (mono - lfState) * kLf;
        lfPower += (lfState * lfState - lfPower) * kPower;
        allPower += (mono * mono - allPower) * kPower;

        const double th = thr[i];
        double d0 = a0 > th ? std::log (a0 / th) : 0.0;
        double d1 = a1 > th ? std::log (a1 / th) : 0.0;
        if (preserve > 0.0 && (d0 > 0.0 || d1 > 0.0))
        {
            // transient: the fast envelope 3 dB (x1.4) .. 9 dB (x2.8) above the slow one
            const double ratio = envFast / (envSlow + 1.0e-12);
            const double tau = std::clamp ((ratio - 1.4) / 1.4, 0.0, 1.0);
            const double scale = 1.0 - preserve * tau;
            d0 *= scale;
            d1 *= scale;
        }
        r0[m & redMask] = d0;
        r1[m & redMask] = d1;

        if (attack != attackTarget)
        {
            attack += attack < attackTarget ? 1 : -1;
            box[0].setLength (attack);
            box[1].setLength (attack);
        }
        if (hold != holdTarget)
            hold += hold < holdTarget ? 1 : -1;

        const long long targetK = m - (lookahead - attack + 1);
        while (k < targetK)
        {
            ++k;
            windowMax[0].push (k, r0[k & redMask]);
            windowMax[1].push (k, r1[k & redMask]);
        }
        const long long oldest = k - (attack + hold) + 1;
        windowMax[0].dropOlderThan (oldest);
        windowMax[1].dropOlderThan (oldest);
        const double dm0 = windowMax[0].max(), dm1 = windowMax[1].max();
        const double dl = std::max (dm0, dm1);

        const double dsrc[3] { dm0, dm1, dl };
        for (int s = 0; s < 3; ++s)
        {
            const double d = dsrc[s];
            slow[(size_t) s] += (d - slow[(size_t) s]) * (d > slow[(size_t) s] ? kSlowCharge : kSlowRelease);
            fast[(size_t) s] = std::max (d - slow[(size_t) s], fast[(size_t) s] * kFast);
        }

        const long long sIdx = m - lookahead;
        const double dm[2] { dm0, dm1 };
        const float* sp[2] { s0, s1 };
        for (int c = 0; c < 2; ++c)
        {
            double g = (1.0 - siS) * slow[2] + siS * slow[(size_t) c] + (1.0 - siT) * fast[2] + siT * fast[(size_t) c];
            g = std::max (g, dm[c]);
            const double rr = g > 0.0 ? std::exp (-g) : 1.0;
            const double gain = box[(size_t) c].process (rr);
            minGain[c] = std::min (minGain[c], gain);
            out[c][i] = (float) ((double) sp[c][sIdx & sigMask] * gain);
        }
    }

    lfRatio = lfPower / (allPower + 1.0e-20);
    updateCoefficients();
}

//==============================================================================
Guard::Guard()
{
    // Phases 0..3: exactly the ps::TruePeakMeter interpolator (64-tap Kaiser beta 7 sinc, centre 31.5, per-phase unity
    // DC, rounded to float like the meter) -> points m + 1/8, 3/8, 5/8, 7/8. Phases 4..6: points m + 1/4, 1/2, 3/4
    // from 16-tap Kaiser-windowed sincs. Tap k multiplies x[m + 8 - k].
    constexpr int numTaps = 4 * kTpTaps;
    const double centre = 0.5 * (numTaps - 1);
    std::array<double, numTaps> h {};
    for (int n = 0; n < numTaps; ++n)
    {
        const double x = (n - centre) / 4.0;
        h[(size_t) n] = sinc (x) * kaiser ((n - centre) / centre, 7.0);
    }
    for (int p = 0; p < 4; ++p)
    {
        double s = 0.0;
        for (int k = 0; k < kTpTaps; ++k)
            s += h[(size_t) (p + 4 * k)];
        for (int k = 0; k < kTpTaps; ++k)
            phases[(size_t) p][(size_t) k] = (double) (float) (h[(size_t) (p + 4 * k)] / s);
    }
    const double offsets[3] { 0.25, 0.5, 0.75 };
    for (int q = 0; q < 3; ++q)
    {
        double s = 0.0;
        auto& ph = phases[(size_t) (4 + q)];
        for (int k = 0; k < kTpTaps; ++k)
        {
            const double d = 8.0 - (double) k - offsets[q];
            ph[(size_t) k] = sinc (d) * kaiser (d / 8.5, 7.0);
            s += ph[(size_t) k];
        }
        for (auto& v : ph)
            v /= s;
    }
}

void Guard::prepare (double sampleRate, int maxAttackSamples)
{
    sr = sampleRate;
    controlStep = 1.0 / std::max (1.0, msToSamples (20.0, sr));
    kSustainCharge = chargeCoeff (msToSamples (20.0, sr));
    prepMaxAttack = std::max (1, maxAttackSamples);
    maxAttack = std::min (std::max (1, maxAttack), prepMaxAttack);
    const int size = nextPow2 (prepMaxAttack + kTpDetectorDelay + 16);
    sigMask = size - 1;
    for (int c = 0; c < 2; ++c)
    {
        sig[(size_t) c].assign ((size_t) size, 0.0f);
        windowMax[(size_t) c].prepare (prepMaxAttack + 24);
        box[(size_t) c].prepare (prepMaxAttack);
    }
    reset();
}

void Guard::setMaxAttack (int samples) noexcept
{
    maxAttack = std::clamp (samples, 1, prepMaxAttack);
    attack = std::min (attack, maxAttack);
}

void Guard::setTimes (int attackSamples, double releaseMs, double indep, double sustainIndep) noexcept
{
    attackSamples = std::clamp (attackSamples, 1, maxAttack);
    attack = attackSamples;
    attackLogTarget = std::log ((double) attack);
    kRelease = decayCoeff (msToSamples (releaseMs, sr));
    independence = std::clamp (indep, 0.0, 1.0);
    sustainIndependence = sustainIndep < 0.0 ? -1.0 : std::clamp (sustainIndep, 0.0, 1.0);
}

void Guard::reset() noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        std::fill (sig[(size_t) c].begin(), sig[(size_t) c].end(), 0.0f);
        hist[(size_t) c].fill (0.0);
        windowMax[(size_t) c].reset();
        box[(size_t) c].setLength (attack);
        box[(size_t) c].reset();
        minGain[c] = 1.0;
    }
    release.fill (0.0);
    sustained.fill (0.0);
    independenceNow = independence;
    sustainIndependenceNow = sustainIndependence < 0.0 ? independence : sustainIndependence;
    attackLogNow = attackLogTarget = std::log ((double) attack);
    histPos = 0;
    t = -1;
}

void Guard::process (const float* const* in, float* const* out, const double* thr, const std::uint8_t* truePeak,
                     int n) noexcept
{
    const int window = maxAttack + 15;
    const int outDelay = maxAttack + 6;

    for (int i = 0; i < n; ++i)
    {
        ++t;
        histPos = (histPos + kTpTaps - 1) & (kTpTaps - 1);
        const long long m = t - kTpDetectorDelay;
        double dm[2];
        for (int c = 0; c < 2; ++c)
        {
            const float x = in[c][i];
            sig[(size_t) c][(size_t) (t & sigMask)] = x;
            auto& hc = hist[(size_t) c];
            hc[(size_t) histPos] = hc[(size_t) (histPos + kTpTaps)] = (double) x;
            const double* w = hc.data() + histPos;
            double e = std::abs (w[kTpDetectorDelay]);
            if (truePeak[i] != 0)
            {
                for (const auto& ph : phases)
                {
                    double y = 0.0;
                    for (int k = 0; k < kTpTaps; ++k)
                        y += ph[(size_t) k] * w[k];
                    e = std::max (e, std::abs (y));
                }
            }
            const double th = thr[i];
            auto& wm = windowMax[(size_t) c];
            wm.push (m, e > th ? std::log (e / th) : 0.0);
            wm.dropOlderThan (m - window + 1);
            dm[c] = wm.max();
        }

        release[0] = std::max (dm[0], release[0] * kRelease);
        release[1] = std::max (dm[1], release[1] * kRelease);
        release[2] = std::max (std::max (dm[0], dm[1]), release[2] * kRelease);

        if (sustainIndependence >= 0.0)
            for (int c = 0; c < 3; ++c)
            {
                auto& sus = sustained[(size_t) c];
                sus += (release[(size_t) c] - sus) * kSustainCharge;
                sus = std::min (sus, release[(size_t) c]);
            }

        const long long sIdx = m - outDelay;
        const double si = rampControl (independenceNow, independence, controlStep);
        const double siSus = rampControl (sustainIndependenceNow,
                                          sustainIndependence < 0.0 ? independence : sustainIndependence, controlStep);
        double fractionalAttack = (double) attack;
        if (attackLogNow != attackLogTarget)
        {
            // A log ramp spends more time near the short windows, where adding
            // even one historical sample has the largest effect on their mean.
            rampControl (attackLogNow, attackLogTarget, std::log ((double) maxAttack) * controlStep);
            fractionalAttack = attackLogNow == attackLogTarget ? (double) attack : std::exp (attackLogNow);
        }
        for (int c = 0; c < 2; ++c)
        {
            double g = si * release[(size_t) c] + (1.0 - si) * release[2];
            if (sustainIndependence >= 0.0 && si != siSus)
            {
                g = si * (release[(size_t) c] - sustained[(size_t) c])
                    + (1.0 - si) * (release[2] - sustained[2])
                    + siSus * sustained[(size_t) c] + (1.0 - siSus) * sustained[2];
                g = std::max (g, dm[c]);
            }
            const double rr = g > 0.0 ? std::exp (-g) : 1.0;
            const double gain = box[(size_t) c].process (rr, fractionalAttack);
            minGain[c] = std::min (minGain[c], gain);
            out[c][i] = (float) ((double) sig[(size_t) c][(size_t) (sIdx & sigMask)] * gain);
        }
    }
}

//==============================================================================
void SoftClipStage::prepare (double sampleRate)
{
    linear.prepare (2, Oversampler::Quality::LinearPhase, kChunk, 2);
    minimum.prepare (2, Oversampler::Quality::MinimumPhase, kChunk, 2);
    const int maxLat = std::max (linear.getLatencySamples(), minimum.getLatencySamples());
    for (auto& d : dry)
    {
        d.prepare (maxLat + 1);
        d.setDelay (getLatency());
    }
    for (auto& r : residue)
        r.assign ((size_t) kChunk, 0.0f);
    amountStep = 1.0 / std::max (1.0, msToSamples (20.0, sampleRate));
    reset();
}

void SoftClipStage::setMinimumPhase (bool m) noexcept
{
    useMinimum = m;
    for (auto& d : dry)
        d.setDelay (getLatency());
}

int SoftClipStage::getLatency() const noexcept
{
    return useMinimum ? minimum.getLatencySamples() : linear.getLatencySamples();
}

void SoftClipStage::reset() noexcept
{
    for (auto& d : dry)
        d.reset();
    linear.reset();
    minimum.reset();
    amountNow = amountTarget;
    onsetNow = onset;
    ceilingValid = false;
    stale = false;
}

void SoftClipStage::setParams (bool on, int mode, double amount01) noexcept
{
    static constexpr double onsets[3] { 0.70794578438, 0.35481338923, 0.03162277660 };   // -3, -9, -30 dB
    onset = onsets[std::clamp (mode, 0, 2)];
    amountTarget = on ? std::clamp (amount01, 0.0, 1.0) : 0.0;
}

void SoftClipStage::process (const float* const* in, float* const* out, const double* ceilingLin, int n) noexcept
{
    if (n <= 0)
        return;
    const double ceilingBefore = ceilingValid ? previousCeiling : std::max (ceilingLin[0], 1.0e-6);
    previousCeiling = std::max (ceilingLin[n - 1], 1.0e-6);
    ceilingValid = true;
    auto& os = useMinimum ? minimum : linear;
    const bool active = amountNow > 0.0 || amountTarget > 0.0;

    if (active && stale)
    {
        os.reset();   // the oversampler did not run while off; the amount ramps up from 0 over 20 ms
        stale = false;
    }

    float* const* up = active ? os.upsample (in, n) : nullptr;

    for (int c = 0; c < 2; ++c)
    {
        if (out[c] != in[c])
            std::memcpy (out[c], in[c], sizeof (float) * (size_t) n);
        dry[(size_t) c].process (out[c], n);
    }

    if (! active)
    {
        stale = true;
        return;
    }

    const int factor = os.getFactor();
    const int on = n * factor;
    const double step = amountStep / (double) factor;
    double amountEnd = amountNow;
    double onsetEnd = onsetNow;
    for (int ch = 0; ch < 2; ++ch)
    {
        double amt = amountNow;
        double o = onsetNow;
        float* u = up[ch];
        for (int j = 0; j < on; ++j)
        {
            const int at = j / factor;
            const double before = at == 0 ? ceilingBefore : std::max (ceilingLin[at - 1], 1.0e-6);
            const double after = std::max (ceilingLin[at], 1.0e-6);
            const double fraction = (double) (j % factor + 1) / factor;
            const double c = before + fraction * (after - before), invC = 1.0 / c;
            rampControl (o, onset, step);
            const double range = 1.0 - o;
            if (amt < amountTarget)
                amt = std::min (amountTarget, amt + step);
            else if (amt > amountTarget)
                amt = std::max (amountTarget, amt - step);
            const double v = (double) u[j];
            const double a = std::abs (v) * invC;
            double r = 0.0;
            if (a > o)
            {
                const double y = o + range * std::tanh ((a - o) / range);
                r = (v < 0.0 ? -(y - a) : (y - a)) * c;
            }
            u[j] = (float) (r * amt);
        }
        amountEnd = amt;
        onsetEnd = o;
    }
    amountNow = amountEnd;
    onsetNow = onsetEnd;

    float* res[2] { residue[0].data(), residue[1].data() };
    os.downsample (res, n);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i)
            out[ch][i] += res[ch][i];
}

} // namespace ps::maxi
