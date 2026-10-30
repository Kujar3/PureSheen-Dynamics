#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/Quadrature.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Quadrature envelope (dsp/Quadrature.cpp). Prefix "quad.".
namespace
{
using pstest::cd;
using pstest::kPi;

struct PairOut
{
    std::vector<double> a, b, p;
};

PairOut runPair (ps::QuadratureEnvelope& q, double hz, double amp, double seconds, double sr)
{
    PairOut o;
    const auto n = (size_t) std::llround (seconds * sr);
    o.a.resize (n);
    o.b.resize (n);
    o.p.resize (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double x = amp * std::sin (2.0 * kPi * hz * (double) i / sr);
        q.processPair (x, o.a[i], o.b[i]);
        o.p[i] = o.a[i] * o.a[i] + o.b[i] * o.b[i];
    }
    return o;
}

// Settle for max (1 s, 60 periods), then measure the pair's phases / magnitudes over a whole number of periods.
struct PhaseResult
{
    double phaseDiffDeg = 0.0, magADb = 0.0, magBDb = 0.0, rippleDb = 0.0;
};

PhaseResult measure (double hz, double sr, double lowestHz = 20.0)
{
    ps::QuadratureEnvelope q;
    q.prepare (sr, lowestHz);
    const double settle = std::max (1.5, 60.0 / hz);
    const double periods = std::max (20.0, std::ceil (0.5 * hz));
    const double meas = periods / hz;
    const auto o = runPair (q, hz, 0.5, settle + meas, sr);
    const auto start = (size_t) std::llround (settle * sr);
    const auto n = o.a.size() - start;
    // tone phases by correlation (exact whole-period windows not needed for a long window; use n samples)
    const cd ta = pstest::toneOf (o.a, start, n, hz, sr);
    const cd tb = pstest::toneOf (o.b, start, n, hz, sr);
    PhaseResult r;
    r.phaseDiffDeg = std::remainder (std::arg (ta) - std::arg (tb), 2.0 * kPi) * 180.0 / kPi;
    r.magADb = pstest::dbOf (std::abs (ta) / 0.5);
    r.magBDb = pstest::dbOf (std::abs (tb) / 0.5);
    double lo = 1e300, hi = 0.0;
    for (size_t i = start; i < o.p.size(); ++i)
    {
        lo = std::min (lo, o.p[i]);
        hi = std::max (hi, o.p[i]);
    }
    r.rippleDb = 10.0 * std::log10 (hi / lo);
    return r;
}
} // namespace

TEST_CASE ("quad. coefficient counts per sample rate")
{
    for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        ps::QuadratureEnvelope q;
        q.prepare (sr, 20.0);
        pstest::note ("coefficients at " + std::to_string ((int) sr) + " Hz", q.getNumCoefficients());
        CHECK (q.getNumCoefficients() >= 10);
        CHECK (q.getNumCoefficients() <= 24);
    }
    ps::QuadratureEnvelope q48;
    q48.prepare (48000.0, 20.0);
    CHECK (q48.getNumCoefficients() >= 14);
    CHECK (q48.getNumCoefficients() <= 20);
}

TEST_CASE ("quad. measured phase difference within 0.3 degree from 20 Hz to 0.45 fs")
{
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        double worst = 0.0, worstMag = 0.0;
        for (double hz : { 20.0, 22.0, 25.0, 30.0, 40.0, 60.0, 100.0, 250.0, 1000.0, 4000.0, 10000.0, 0.45 * sr })
        {
            const auto r = measure (hz, sr);
            worst = std::max (worst, std::abs (std::abs (r.phaseDiffDeg) - 90.0));
            worstMag = std::max ({ worstMag, std::abs (r.magADb), std::abs (r.magBDb) });
            CHECK (r.phaseDiffDeg > 0.0);   // b lags a
        }
        pstest::note ("worst |phase difference - 90| (deg) at " + std::to_string ((int) sr), worst);
        pstest::note ("worst branch magnitude error (dB)", worstMag);
        CHECK (worst < 0.3);
        CHECK (worstMag < 0.01);
    }
}

TEST_CASE ("quad. design phase matches the measured pair and holds over the band")
{
    ps::QuadratureEnvelope q;
    q.prepare (48000.0, 20.0);
    double worstDesign = 0.0;
    for (double hz = 20.0; hz <= 0.45 * 48000.0; hz *= 1.05)
        worstDesign = std::max (worstDesign, std::abs (q.designPhaseDifferenceDegrees (hz) - 90.0));
    pstest::note ("design: worst |phase difference - 90| (deg), 20 Hz .. 21.6 kHz", worstDesign);
    CHECK (worstDesign < 0.05);

    for (double hz : { 20.0, 40.0, 1000.0 })
    {
        const auto r = measure (hz, 48000.0);
        CHECK_NEAR (r.phaseDiffDeg, q.designPhaseDifferenceDegrees (hz), 0.02);
    }
    // below the design band the error grows (documented: the band starts at lowestHz)
    pstest::note ("design phase difference at 10 Hz (deg)", q.designPhaseDifferenceDegrees (10.0));
}

TEST_CASE ("quad. narrow transitions retain the specified phase accuracy")
{
    for (double sr : { 96000.0, 192000.0, 384000.0 })
    {
        ps::QuadratureEnvelope q;
        q.prepare (sr, 5.0);
        double worst = 0.0;
        for (int i = 0; i <= 1000; ++i)
        {
            const double hz = 5.0 * std::pow (0.45 * sr / 5.0, i / 1000.0);
            worst = std::max (worst, std::abs (q.designPhaseDifferenceDegrees (hz) - 90.0));
        }
        pstest::note ("5 Hz design: worst phase error (deg) at " + std::to_string ((int) sr), worst);
        CHECK (worst < 0.3);
    }

    // Validate the recurrence as well as the coefficient-side calculation at the narrowest ordinary audio design.
    const auto measured = measure (5.0, 192000.0, 5.0);
    pstest::note ("5 Hz at 192 kHz: measured phase error (deg)", std::abs (measured.phaseDiffDeg - 90.0));
    pstest::note ("5 Hz at 192 kHz: measured power ripple (dB p-p)", measured.rippleDb);
    CHECK (std::abs (measured.phaseDiffDeg - 90.0) < 0.3);
    CHECK (measured.rippleDb < 0.05);
}

TEST_CASE ("quad. ripple-free power of low and mid sines")
{
    for (double hz : { 20.0, 30.0, 40.0, 1000.0 })
    {
        const auto r = measure (hz, 48000.0);
        pstest::note ("power ripple (dB p-p) at " + std::to_string ((int) hz) + " Hz", r.rippleDb);
        CHECK (r.rippleDb < 0.05);
        if ((int) hz == 30)
            CHECK (r.rippleDb < 0.01);
    }

    // the naive mean-square detector with the same 1 ms-class speed ripples by many dB: what this avoids
    {
        const double sr = 48000.0, hz = 30.0;
        const double c = 1.0 - std::exp (-1.0 / (0.001 * sr));
        double ms = 0.0, lo = 1e300, hi = 0.0;
        for (int i = 0; i < (int) (2.0 * sr); ++i)
        {
            const double x = std::sin (2.0 * kPi * hz * i / sr);
            ms += c * (x * x - ms);
            if (i > (int) sr)
            {
                lo = std::min (lo, ms);
                hi = std::max (hi, ms);
            }
        }
        pstest::note ("naive 1 ms mean-square ripple at 30 Hz (dB p-p)", 10.0 * std::log10 (hi / std::max (lo, 1e-30)));
    }
}

TEST_CASE ("quad. power reads A^2 and follows a level step quickly")
{
    const double sr = 48000.0;
    for (double hz : { 40.0, 1000.0 })
    {
        ps::QuadratureEnvelope q;
        q.prepare (sr, 20.0);
        const auto stepAt = (size_t) sr;
        double settledLow = 0.0, settledHigh = 0.0;
        int reached90 = -1;
        for (size_t i = 0; i < (size_t) (2.0 * sr); ++i)
        {
            const double amp = i < stepAt ? 0.1 : 0.5;
            const double p = q.process (amp * std::sin (2.0 * kPi * hz * (double) i / sr));
            if (i == stepAt - 1)
                settledLow = p;
            if (i >= stepAt && reached90 < 0 && p >= 0.9 * 0.25)
                reached90 = (int) (i - stepAt);
            settledHigh = p;
        }
        pstest::note ("A = 0.1 reads (x100)", settledLow * 100.0);
        pstest::note ("A = 0.5 reads (x4)", settledHigh * 4.0);
        pstest::note ("ms to 90 % of the new power after a step at " + std::to_string ((int) hz) + " Hz",
                      reached90 * 1000.0 / sr);
        CHECK_NEAR (settledLow * 100.0, 1.0, 0.002);
        CHECK_NEAR (settledHigh * 4.0, 1.0, 0.002);
        CHECK (reached90 >= 0);
        CHECK (reached90 * 1000.0 / sr < (hz < 100.0 ? 25.0 : 2.0));   // detector lag (group delay of the chains)
    }
}

TEST_CASE ("quad. silence flushes the state to exact zero (denormal safe)")
{
    ps::QuadratureEnvelope q;
    q.prepare (48000.0, 20.0);
    for (int i = 0; i < 48000; ++i)
        q.process (0.7 * std::sin (2.0 * kPi * 37.0 * i / 48000.0));
    double last = 1.0;
    int firstZero = -1;
    for (int i = 0; i < 48000 * 60; ++i)
    {
        last = q.process (0.0);
        if (last == 0.0 && firstZero < 0)
            firstZero = i;
        if (last != 0.0 && firstZero >= 0)
            firstZero = -2;   // must stay at zero once flushed
        if (last != 0.0 && std::fpclassify (last) == FP_SUBNORMAL)
        {
            CHECK (false);
            break;
        }
    }
    pstest::note ("seconds of silence until exact zero", firstZero / 48000.0);
    CHECK (firstZero > 0);
    CHECK_EQ (last, 0.0);
}

#if PS_QUAD_SSE2
TEST_CASE ("quad. individual stages and stereo lanes never process subnormal tails")
{
    int configurations = 0;
    double smallestCoefficient = 1.0;
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0, 384000.0 })
        for (double low : { 5.0, 20.0, 60.0, 500.0, 0.1 * sr })
            for (int mode = 0; mode < 3; ++mode)
            {
                ps::QuadratureEnvelope mono;
                ps::QuadratureEnvelope2 pair;
                mono.prepare (sr, low);
                pair.prepare (sr, low);
                for (double c : mono.getCoefficients())
                    smallestCoefficient = std::min (smallestCoefficient, c);
                const unsigned int originalCsr = _mm_getcsr();
                // Clear status and disable host FTZ/DAZ: the detector must prevent its own denormal arithmetic.
                _mm_setcsr (originalCsr & ~0x807fu);
                int firstDenormal = -1;
                double lastA = 0.0, lastB = 0.0;
                for (int i = 0; i < 48000; ++i)
                {
                    const double x = i == 0 ? 1.0 : 0.0;
                    if (mode == 2)
                    {
                        double activeA = 0.0, activeB = 0.0;
                        // One decaying channel must not depend on silence in the other channel.
                        pair.processPair (x, 0.25, lastA, lastB, activeA, activeB);
                    }
                    else if (mode == 1)
                        lastA = mono.process (x);   // include the output squaring, not only the all-pass recurrence
                    else
                        mono.processPair (x, lastA, lastB);
                    // Underflow also detects creation of a subnormal before it can become an operand.
                    if (firstDenormal < 0 && (_mm_getcsr() & 0x12u) != 0)
                        firstDenormal = i;
                }
                _mm_setcsr (originalCsr);
                if (firstDenormal >= 0)
                {
                    pstest::note ("denormal configuration sample rate", sr);
                    pstest::note ("denormal configuration lowestHz", low);
                    pstest::note ("denormal configuration mode (0 pair, 1 power, 2 stereo)", mode);
                    pstest::note ("first denormal or underflow sample", firstDenormal);
                }
                CHECK_EQ (firstDenormal, -1);
                CHECK (std::isfinite (lastA) && std::isfinite (lastB));
                ++configurations;
            }
    pstest::note ("denormal-free coefficient / sample-rate / API configurations", configurations);
    pstest::note ("smallest coefficient exercised", smallestCoefficient);
}

TEST_CASE ("quad. complete fastest and slowest tails remain normal through output squaring")
{
    for (double low : { 5.0, 38400.0 })
    {
        constexpr double sr = 384000.0;
        ps::QuadratureEnvelope mono, power;
        ps::QuadratureEnvelope2 pair;
        mono.prepare (sr, low);
        power.prepare (sr, low);
        pair.prepare (sr, low);
        const unsigned int originalCsr = _mm_getcsr();
        _mm_setcsr (originalCsr & ~0x807fu);
        int firstDenormal = -1, zeros = 0, i = 0;
        bool same = true;
        for (; i < (int) (60.0 * sr) && zeros < 1024; ++i)
        {
            const double x = i == 0 ? 1.0 : 0.0;
            double a = 0.0, b = 0.0, a0 = 0.0, b0 = 0.0, a1 = 0.0, b1 = 0.0;
            mono.processPair (x, a, b);
            const double p = power.process (x);
            pair.processPair (x, 0.25, a0, b0, a1, b1);
            same = same && a == a0 && b == b0;
            zeros = a == 0.0 && b == 0.0 && p == 0.0 ? zeros + 1 : 0;
            if (firstDenormal < 0 && (_mm_getcsr() & 0x12u) != 0)
                firstDenormal = i;
        }
        _mm_setcsr (originalCsr);
        pstest::note ("tail lowestHz", low);
        pstest::note ("tail seconds through 1024 consecutive zeros", i / sr);
        pstest::note ("tail first denormal / underflow sample (-1 = none)", firstDenormal);
        CHECK_EQ (firstDenormal, -1);
        CHECK_EQ (zeros, 1024);
        CHECK (same);   // independent lane flushing must also preserve the bit-identical mono contract
    }
}
#endif

TEST_CASE ("quad. reset restores the initial state and nothing allocates")
{
    ps::QuadratureEnvelope q;
    q.prepare (44100.0, 30.0);
    std::vector<double> first, second;
    pstest::Lcg lcg (7);
    std::vector<double> in (5000);
    for (auto& x : in)
        x = lcg.next();
    first.reserve (in.size());
    second.reserve (in.size());
    {
        pstest::AllocationScope scope;
        for (double x : in)
            first.push_back (q.process (x));
        q.reset();
        for (double x : in)
            second.push_back (q.process (x));
        double a = 0.0, b = 0.0;
        q.processPair (0.1, a, b);
        CHECK_EQ (scope.count(), 0LL);
    }
    CHECK (first == second);
}

TEST_CASE ("quad. unprepared and extreme lowestHz stay finite")
{
    ps::QuadratureEnvelope q;
    CHECK_NEAR (q.process (0.5), 0.25, 1e-12);   // unprepared: passthrough power
    for (double low : { 0.0, 1.0, 5.0, 100.0, 5000.0, 1.0e9, std::nan ("") })
    {
        q.prepare (48000.0, low);
        double p = 0.0;
        for (int i = 0; i < 20000; ++i)
            p = q.process (std::sin (2.0 * kPi * 10000.0 * i / 48000.0));   // inside every clamped band
        CHECK (std::isfinite (p));
        CHECK_NEAR (p, 1.0, 0.01);
    }
    q.prepare (48000.0, 60.0);
    const auto r = measure (60.0, 48000.0, 60.0);
    pstest::note ("lowestHz 60: coefficients", q.getNumCoefficients());
    pstest::note ("lowestHz 60: |phase - 90| at 60 Hz", std::abs (r.phaseDiffDeg - 90.0));
    CHECK (std::abs (r.phaseDiffDeg - 90.0) < 0.3);
}

TEST_CASE ("quad. cpu")
{
    ps::QuadratureEnvelope q;
    q.prepare (48000.0, 20.0);
    const int n = 48000 * 10;
    pstest::Lcg lcg (3);
    double acc = 0.0;
    pstest::Stopwatch sw;
    for (int i = 0; i < n; ++i)
        acc += q.process (lcg.next());
    const double s = sw.seconds();
    pstest::note ("x realtime (one mono detector, 48 kHz)", 10.0 / std::max (s, 1e-9));
    CHECK (std::isfinite (acc));
}

TEST_CASE ("quad. two-channel pair equals two mono instances sample for sample")
{
    for (double sr : { 44100.0, 48000.0, 192000.0 })
    {
        ps::QuadratureEnvelope m0, m1;
        ps::QuadratureEnvelope2 st;
        m0.prepare (sr, 20.0);
        m1.prepare (sr, 20.0);
        st.prepare (sr, 20.0);
        CHECK_EQ (st.getNumCoefficients(), m0.getNumCoefficients());
        pstest::Lcg lcg (5);
        bool same = true;
        pstest::AllocationScope scope;
        for (int i = 0; i < 100000; ++i)
        {
            const double x0 = lcg.next(), x1 = 0.3 * std::sin (2.0 * kPi * 31.0 * i / sr);
            double a0, b0, a1, b1, ra0, rb0, ra1, rb1;
            st.processPair (x0, x1, a0, b0, a1, b1);
            m0.processPair (x0, ra0, rb0);
            m1.processPair (x1, ra1, rb1);
            same = same && a0 == ra0 && b0 == rb0 && a1 == ra1 && b1 == rb1;
            if (i == 50000)
            {
                st.reset();
                m0.reset();
                m1.reset();
            }
        }
        CHECK_EQ (scope.count(), 0LL);
        CHECK (same);
    }
}
