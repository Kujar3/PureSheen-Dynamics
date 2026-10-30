#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/Filters.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

// Biquad designs and filters (dsp/Filters.cpp). Prefix "filters.".
namespace
{
using namespace ps;
using pstest::cd;
using pstest::kPi;

// Analog prototypes, written out independently of Filters.cpp (s = j f / f0).
cd analogPeak (double f, double f0, double q, double db)
{
    const cd s (0.0, f / f0);
    const double A = std::pow (10.0, db / 40.0);
    return (s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0);
}

cd analogLowShelf (double f, double f0, double q, double db)
{
    const cd s (0.0, f / f0);
    const double A = std::pow (10.0, db / 40.0), sA = std::sqrt (A);
    return A * (s * s + s * (sA / q) + A) / (A * s * s + s * (sA / q) + 1.0);
}

cd analogHighShelf (double f, double f0, double q, double db)
{
    const cd s (0.0, f / f0);
    const double A = std::pow (10.0, db / 40.0), sA = std::sqrt (A);
    return A * (A * s * s + s * (sA / q) + 1.0) / (s * s + s * (sA / q) + A);
}

cd analogLowPass (double f, double f0, double q)
{
    const cd s (0.0, f / f0);
    return 1.0 / (s * s + s / q + 1.0);
}

cd analogHighPass (double f, double f0, double q)
{
    const cd s (0.0, f / f0);
    return s * s / (s * s + s / q + 1.0);
}

double dbOfC (cd h) { return pstest::dbOf (std::abs (h)); }

// Frequencies to compare on: log-spaced 10 Hz .. fMax plus a dense linear grid over the top octave.
std::vector<double> gridUpTo (double fMax)
{
    std::vector<double> f;
    for (int i = 0; i <= 300; ++i)
        f.push_back (10.0 * std::pow (fMax / 10.0, i / 300.0));
    for (int i = 0; i <= 200; ++i)
        f.push_back (0.5 * fMax + 0.5 * fMax * i / 200.0);
    return f;
}

template <typename Design, typename Analog>
double maxErrorDb (Design design, Analog analog, double sr, double fMax)
{
    double worst = 0.0;
    for (double f : gridUpTo (fMax))
        worst = std::max (worst, std::abs (design.magnitudeDb (f, sr) - dbOfC (analog (f))));
    return worst;
}

// Gain of a Biquad at freq, measured with a sine (1 s at 48 kHz, last 0.5 s analysed).
double measuredGainDb (const BiquadCoeffs& c, double freq, double sr)
{
    Biquad bq;
    bq.setCoeffs (c);
    auto x = pstest::sine (freq, -6.0, 1.0, sr);
    std::vector<float> y = x;
    bq.process (y.data(), (int) y.size());
    const size_t n = (size_t) sr / 2;
    const double gx = std::abs (pstest::toneOf (x, n, n, freq, sr));
    const double gy = std::abs (pstest::toneOf (y, n, n, freq, sr));
    return 20.0 * std::log10 (gy / gx);
}

// Steep 8th-order matched Butterworth high pass (for splatter measurements).
std::vector<double> highPassed (const std::vector<double>& x, double sr, double hz)
{
    std::vector<double> y = x;
    for (int k = 0; k < 4; ++k)
    {
        Biquad hp;
        hp.setCoeffs (design::highPass (sr, hz, design::butterworthQ (8, k)));
        for (auto& v : y)
            v = hp.processSample (v);
    }
    return y;
}
} // namespace

TEST_CASE ("filters. butterworth Q values")
{
    CHECK_NEAR (design::butterworthQ (2, 0), 0.7071067811865476, 1e-12);
    CHECK_NEAR (design::butterworthQ (3, 0), 1.0, 1e-12);
    CHECK_NEAR (design::butterworthQ (4, 0), 0.5411961001461970, 1e-12);
    CHECK_NEAR (design::butterworthQ (4, 1), 1.3065629648763766, 1e-12);
    CHECK_NEAR (design::butterworthQ (5, 0), 0.6180339887498949, 1e-12);
    CHECK_NEAR (design::butterworthQ (5, 1), 1.6180339887498949, 1e-12);
    CHECK_NEAR (design::butterworthQ (6, 0), 0.5176380902050415, 1e-12);
    CHECK_NEAR (design::butterworthQ (6, 1), 0.7071067811865476, 1e-12);
    CHECK_NEAR (design::butterworthQ (6, 2), 1.9318516525781366, 1e-12);
    CHECK_NEAR (design::butterworthQ (8, 0), 0.5097955791041592, 1e-12);
    CHECK_NEAR (design::butterworthQ (8, 3), 2.5629154477415055, 1e-12);

    // A cascade of the sections is maximally flat: -3.01 dB at the corner and -6 N dB/oct far above (here N = 4, 6).
    for (int order : { 4, 6 })
    {
        const double sr = 48000.0, fc = 1000.0;
        double atFc = 0.0, at4Fc = 0.0, at100 = 0.0;
        for (int k = 0; k < order / 2; ++k)
        {
            const auto c = design::lowPass (sr, fc, design::butterworthQ (order, k));
            atFc += c.magnitudeDb (fc, sr);
            at4Fc += c.magnitudeDb (4.0 * fc, sr);
            at100 += c.magnitudeDb (100.0, sr);
        }
        CHECK_NEAR (atFc, -3.0103, 0.01);
        CHECK_NEAR (at4Fc, -10.0 * std::log10 (1.0 + std::pow (4.0, 2.0 * order)), 0.05);
        CHECK_NEAR (at100, 0.0, 0.001);
    }
}

TEST_CASE ("filters. matched peak and shelves follow the analog prototype up to 20 kHz at 44.1 kHz")
{
    // Worst |digital - analog| (dB) from 10 Hz to 20 kHz over the whole EQ range (gain -30..+15 dB, bell Q 0.1..24,
    // shelf Q 0.1..2), grouped by centre frequency. A single biquad cannot follow every analog shape up to Nyquist;
    // the numbers below are what the matched + refined designs achieve.
    const double sr = 44100.0;
    const double groups[] { 2000.0, 5000.0, 10000.0, 16000.0, 20000.0 };
    double peakAll[5] {}, peakMod[5] {}, peakCore[5] {}, shelfAll[5] {}, shelfMod[5] {};

    auto groupOf = [&] (double f0) { int g = 0; while (f0 > groups[g]) ++g; return g; };

    for (double f0 : { 20.0, 60.0, 200.0, 600.0, 1000.0, 2000.0, 3000.0, 5000.0, 7000.0, 10000.0, 12000.0, 14000.0,
                       16000.0, 18000.0, 19000.0, 20000.0 })
        for (double db : { -30.0, -12.0, -6.0, -1.0, 1.0, 6.0, 12.0, 15.0 })
        {
            const int g = groupOf (f0);
            const bool moderate = std::abs (db) <= 12.0;
            for (double q : { 0.1, 0.3, 0.7071, 1.5, 4.0, 10.0, 24.0 })
            {
                const double e = maxErrorDb (design::peak (sr, f0, q, db),
                                             [&] (double f) { return analogPeak (f, f0, q, db); }, sr, 20000.0);
                peakAll[g] = std::max (peakAll[g], e);
                if (moderate && q >= 0.3 && q <= 4.0)
                    peakMod[g] = std::max (peakMod[g], e);
                if (moderate && q >= 0.7 && q <= 4.0)
                    peakCore[g] = std::max (peakCore[g], e);
            }
            for (double q : { 0.1, 0.3, 0.5, 0.7071, 1.0, 2.0 })
            {
                const double el = maxErrorDb (design::lowShelf (sr, f0, q, db),
                                              [&] (double f) { return analogLowShelf (f, f0, q, db); }, sr, 20000.0);
                const double eh = maxErrorDb (design::highShelf (sr, f0, q, db),
                                              [&] (double f) { return analogHighShelf (f, f0, q, db); }, sr, 20000.0);
                shelfAll[g] = std::max (shelfAll[g], std::max (el, eh));
                if (moderate && q >= 0.3 && q <= 1.0)
                    shelfMod[g] = std::max (shelfMod[g], std::max (el, eh));
            }
        }

    const char* names[] { "f0 <= 2 kHz", "f0 <= 5 kHz", "f0 <= 10 kHz", "f0 <= 16 kHz", "f0 <= 20 kHz" };
    for (int g = 0; g < 5; ++g)
    {
        pstest::note (std::string ("bell, full range, ") + names[g] + " (dB)", peakAll[g]);
        pstest::note (std::string ("bell, |gain| <= 12, Q 0.3..4, ") + names[g] + " (dB)", peakMod[g]);
        pstest::note (std::string ("bell, |gain| <= 12, Q 0.7..4, ") + names[g] + " (dB)", peakCore[g]);
        pstest::note (std::string ("shelf, full range, ") + names[g] + " (dB)", shelfAll[g]);
        pstest::note (std::string ("shelf, |gain| <= 12, Q 0.3..1, ") + names[g] + " (dB)", shelfMod[g]);
    }
    for (int g = 0; g < 5; ++g)
    {
        CHECK (peakCore[g] < (g == 0 ? 0.1 : 0.35));
        CHECK (peakMod[g] < 0.35);
        CHECK (shelfMod[g] < (g < 3 ? 0.1 : 0.2));
        CHECK (peakAll[g] < 2.0);
        CHECK (shelfAll[g] < 0.6);
    }

    // exact at the matching points where the closed form is used
    const auto pk = design::peak (sr, 1000.0, 2.0, 12.0);
    CHECK_NEAR (pk.magnitudeDb (1000.0, sr), 12.0, 1e-6);
    CHECK_NEAR (pk.magnitudeDb (0.001, sr), 0.0, 1e-6);
    const auto ls = design::lowShelf (sr, 100.0, 0.7071, -9.0);
    CHECK_NEAR (ls.magnitudeDb (100.0, sr), -4.5, 1e-6);
    CHECK_NEAR (ls.magnitudeDb (0.001, sr), -9.0, 1e-6);
    const auto hs = design::highShelf (sr, 15000.0, 0.7071, 9.0);
    CHECK_NEAR (hs.magnitudeDb (0.001, sr), 0.0, 1e-6);   // DC stays exact after refinement
    CHECK_NEAR (hs.magnitudeDb (15000.0, sr), 4.5, 0.1);

    // gain 0 is the exact identity
    for (const auto& c : { design::peak (sr, 3000.0, 1.0, 0.0), design::lowShelf (sr, 100.0, 0.7, 0.0),
                           design::highShelf (sr, 9000.0, 0.7, 0.0) })
        CHECK (c.b0 == 1.0 && c.b1 == 0.0 && c.b2 == 0.0 && c.a1 == 0.0 && c.a2 == 0.0);

    // boost and cut are mirror images
    for (double f0 : { 500.0, 12000.0 })
    {
        const auto up = design::peak (sr, f0, 1.0, 9.0), dn = design::peak (sr, f0, 1.0, -9.0);
        const auto su = design::highShelf (sr, f0, 0.7, 9.0), sd = design::highShelf (sr, f0, 0.7, -9.0);
        double worst = 0.0;
        for (double f : gridUpTo (20000.0))
        {
            worst = std::max (worst, std::abs (up.magnitudeDb (f, sr) + dn.magnitudeDb (f, sr)));
            worst = std::max (worst, std::abs (su.magnitudeDb (f, sr) + sd.magnitudeDb (f, sr)));
        }
        pstest::note ("boost + cut mirror error (dB)", worst);
        CHECK (worst < 0.05);
    }
}

TEST_CASE ("filters. matched low and high pass track the analog prototype")
{
    const double sr = 44100.0;
    double worstLp = 0.0, worstHp = 0.0;
    double worstLpToFc = 0.0;   // up to the corner and beyond, where the curve is still within 30 dB
    for (double f0 : { 20.0, 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 19000.0 })
        for (double q : { 0.5, 0.7071, 1.3, 3.0 })
        {
            const auto lp = design::lowPass (sr, f0, q);
            const auto hp = design::highPass (sr, f0, q);
            for (double f : gridUpTo (20000.0))
            {
                const double a = dbOfC (analogLowPass (f, f0, q));
                const double d = std::abs (lp.magnitudeDb (f, sr) - a);
                if (a > -30.0) worstLpToFc = std::max (worstLpToFc, d);
                worstLp = std::max (worstLp, d);
                const double ah = dbOfC (analogHighPass (f, f0, q));
                if (ah > -40.0)
                    worstHp = std::max (worstHp, std::abs (hp.magnitudeDb (f, sr) - ah));
            }
            CHECK_NEAR (lp.magnitudeDb (f0, sr), 20.0 * std::log10 (q), f0 <= 1000.0 ? 1e-6 : 0.6);   // corner
            CHECK_NEAR (hp.magnitudeDb (f0, sr), 20.0 * std::log10 (q), f0 <= 1000.0 ? 1e-6 : 0.6);
            CHECK_NEAR (lp.magnitudeDb (0.01, sr), 0.0, 1e-5);
        }
    pstest::note ("low pass: worst error where the curve is above -30 dB (dB)", worstLpToFc);
    pstest::note ("low pass: worst error anywhere (dB)", worstLp);
    pstest::note ("high pass: worst error where the curve is above -40 dB (dB)", worstHp);
    CHECK (worstLpToFc < 0.6);
    CHECK (worstHp < 0.9);

    // HP has a true double zero at DC: 12 dB / octave far below the corner
    const auto hp = design::highPass (sr, 1000.0, 0.7071);
    CHECK_NEAR (hp.magnitudeDb (20.0, sr) - hp.magnitudeDb (40.0, sr), -12.04, 0.02);
}

TEST_CASE ("filters. first-order designs")
{
    const double sr = 48000.0;
    for (double f0 : { 30.0, 300.0, 3000.0, 12000.0 })
    {
        double worst = 0.0;
        const auto lp = design::lowPass1 (sr, f0);
        const auto hp = design::highPass1 (sr, f0);
        CHECK (lp.b2 == 0.0 && lp.a2 == 0.0 && hp.b2 == 0.0 && hp.a2 == 0.0);
        for (double f : gridUpTo (20000.0))
        {
            const double r = f / f0;
            const double al = 10.0 * std::log10 (1.0 / (1.0 + r * r));
            if (al > -30.0)
                worst = std::max (worst, std::abs (lp.magnitudeDb (f, sr) - al));
            const double ah = 10.0 * std::log10 (r * r / (1.0 + r * r));
            if (ah > -30.0)
                worst = std::max (worst, std::abs (hp.magnitudeDb (f, sr) - ah));
        }
        for (double g : { -12.0, 6.0 })
        {
            const auto ls = design::lowShelf1 (sr, f0, g);
            const auto hs = design::highShelf1 (sr, f0, g);
            CHECK_NEAR (ls.magnitudeDb (0.01, sr), g, 1e-5);
            CHECK_NEAR (hs.magnitudeDb (0.01, sr), 0.0, 1e-5);
            CHECK_NEAR (ls.magnitudeDb (20000.0, sr), 20.0 * std::log10 (std::abs (
                            (cd (0.0, 20000.0 / f0) + std::pow (10.0, g / 40.0)) /
                            (cd (0.0, 20000.0 / f0) + std::pow (10.0, -g / 40.0)))), f0 < 5000.0 ? 0.1 : 1.2);
            if (f0 < 5000.0)
            {
                CHECK_NEAR (ls.magnitudeDb (f0, sr), 0.5 * g, 0.05);
                CHECK_NEAR (hs.magnitudeDb (f0, sr), 0.5 * g, 0.05);
                const double sG = std::pow (10.0, g / 40.0);
                const cd s (0.0, 20000.0 / f0);
                CHECK_NEAR (hs.magnitudeDb (20000.0, sr), dbOfC (sG * sG * (s + 1.0 / sG) / (s + sG)), 0.1);
            }
        }
        pstest::note ("first-order LP / HP: worst error (dB) at corner " + std::to_string ((int) f0) + " Hz", worst);
        CHECK (worst < 0.2);
    }
}

TEST_CASE ("filters. all-passes have unit magnitude and the textbook phase")
{
    const double sr = 48000.0;
    for (double f0 : { 50.0, 1000.0, 15000.0 })
    {
        const auto ap = design::allPass (sr, f0, 0.7071);
        const auto ap1 = design::allPass1 (sr, f0);
        for (double f : gridUpTo (23000.0))
        {
            CHECK_NEAR (ap.magnitudeDb (f, sr), 0.0, 1e-9);
            CHECK_NEAR (ap1.magnitudeDb (f, sr), 0.0, 1e-9);
        }
        CHECK_NEAR (std::abs (std::arg (ap.response (f0, sr))), kPi, 1e-9);        // -180 degrees at the corner
        CHECK_NEAR (std::arg (ap1.response (f0, sr)), -0.5 * kPi, 1e-9);           // -90 degrees
    }
}

TEST_CASE ("filters. Biquad processing matches the designed response")
{
    const double sr = 48000.0;
    for (double f : { 100.0, 1000.0, 7000.0 })
    {
        const auto c = design::peak (sr, 1000.0, 1.0, 9.0);
        CHECK_NEAR (measuredGainDb (c, f, sr), c.magnitudeDb (f, sr), 0.001);
        const auto h = design::highShelf (sr, 3000.0, 0.7071, -6.0);
        CHECK_NEAR (measuredGainDb (h, f, sr), h.magnitudeDb (f, sr), 0.001);
    }

    // decays to exact zeros after the input stops (no denormal tail)
    Biquad bq;
    bq.setCoeffs (design::lowPass (sr, 30.0, 0.7071));
    std::vector<float> buf (48000, 0.0f);
    buf[0] = 1.0f;
    for (int block = 0; block < 400; ++block)
    {
        bq.process (buf.data(), (int) buf.size());
        std::fill (buf.begin(), buf.end(), 0.0f);
    }
    bq.process (buf.data(), (int) buf.size());
    bool allZero = true;
    for (float v : buf)
        allZero = allZero && v == 0.0f;
    CHECK (allZero);
}

TEST_CASE ("filters. SmoothedBiquad is click-free and lands exactly on the target")
{
    const double sr = 48000.0;
    const auto x = pstest::sine (1000.0, -6.0, 0.5, sr);
    const size_t change = 12000;

    const auto flat = design::peak (sr, 1000.0, 1.0, 0.0);
    const auto boost = design::peak (sr, 1000.0, 1.0, 12.0);

    SmoothedBiquad sm;
    sm.setRampLength (64);
    sm.setTarget (flat);
    sm.snapToTarget();
    Biquad abrupt;
    abrupt.setCoeffs (flat);

    // ideal reference: two static filters, their outputs crossfaded linearly over the same 64 samples
    Biquad refA, refB;
    refA.setCoeffs (flat);
    refB.setCoeffs (boost);

    std::vector<double> ys (x.size()), ya (x.size()), yr (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        if (i == change)
        {
            sm.setTarget (boost);
            abrupt.setCoeffs (boost);
        }
        ys[i] = sm.processSample (x[i]);
        ya[i] = abrupt.processSample (x[i]);
        const double r = std::clamp (((double) i - (double) change + 1.0) / 64.0, 0.0, 1.0);
        yr[i] = (1.0 - r) * refA.processSample (x[i]) + r * refB.processSample (x[i]);
    }
    CHECK (! sm.isRamping());

    // splatter above 6 kHz around the change (the 1 kHz tone itself is > 120 dB down after the high pass)
    const auto hs = highPassed (ys, sr, 6000.0);
    const auto ha = highPassed (ya, sr, 6000.0);
    const double before = pstest::dbOf (pstest::peakOf (hs, 6000, 5000));
    const double smooth = pstest::dbOf (pstest::peakOf (hs, change - 100, 2000));
    const double jump = pstest::dbOf (pstest::peakOf (ha, change - 100, 2000));
    const double ideal = pstest::dbOf (pstest::peakOf (highPassed (yr, sr, 6000.0), change - 100, 2000));
    pstest::note ("HF floor before the change (dBFS)", before);
    pstest::note ("HF splatter, smoothed 64-sample ramp (dBFS)", smooth);
    pstest::note ("HF splatter, ideal 64-sample output crossfade (dBFS)", ideal);
    pstest::note ("HF splatter, abrupt coefficient switch (dBFS)", jump);
    CHECK (smooth < -60.0);
    CHECK (smooth < ideal + 3.0);    // no worse than crossfading two clean outputs over the same time
    CHECK (smooth < jump - 10.0);

    // steady state equals the target filter exactly
    Biquad ref;
    ref.setCoeffs (boost);
    SmoothedBiquad s2;
    s2.setRampLength (32);
    s2.setTarget (flat);
    s2.snapToTarget();
    s2.setTarget (boost);
    for (int i = 0; i < 32; ++i)
        s2.processSample (0.0);
    CHECK (! s2.isRamping());
    double maxDiff = 0.0;
    for (size_t i = 0; i < 2000; ++i)
        maxDiff = std::max (maxDiff, std::abs (s2.processSample (x[i]) - ref.processSample (x[i])));
    CHECK (maxDiff < 1e-12);
}

TEST_CASE ("filters. SmoothedBiquad stays stable under fast high-Q sweeps")
{
    const double sr = 44100.0;
    const auto noise = pstest::whiteNoise (0.5, 2.0, sr, 99u);
    SmoothedBiquad sm;
    sm.setRampLength (32);
    double peak = 0.0;
    bool finite = true;
    for (size_t i = 0; i < noise.size(); ++i)
    {
        if (i % 32 == 0)
        {
            const double f = 30.0 * std::pow (2.0, 9.0 * std::abs (std::sin ((double) i / 9000.0)));
            sm.setTarget (design::peak (sr, f, 12.0, (i / 32) % 2 == 0 ? 15.0 : -30.0));
        }
        const double y = sm.processSample (noise[i]);
        finite = finite && std::isfinite (y);
        peak = std::max (peak, std::abs (y));
    }
    pstest::note ("peak output under a +15/-30 dB, Q 12 sweep (input 0.5)", peak);
    CHECK (finite);
    CHECK (peak < 50.0);
}

TEST_CASE ("filters. design cost")
{
    // closed form (low / narrow) vs refined (wide, high) designs; the EQ may redesign per block while automating
    const double sr = 48000.0;
    double sink = 0.0;
    pstest::Stopwatch a;
    for (int i = 0; i < 20000; ++i)
        sink += design::peak (sr, 100.0 + (i % 50), 2.0, 6.0).b0;
    const double closedUs = a.seconds() / 20000.0 * 1.0e6;
    pstest::Stopwatch b;
    for (int i = 0; i < 20000; ++i)
        sink += design::peak (sr, 9000.0 + (i % 50), 0.5, 12.0).b0 + design::highShelf (sr, 12000.0 + (i % 50), 0.7, 9.0).b0;
    const double refinedUs = b.seconds() / 40000.0 * 1.0e6;
    pstest::note ("design time, closed form (us)", closedUs);
    pstest::note ("design time, refined (us)", refinedUs);
    CHECK (std::isfinite (sink));
    CHECK (refinedUs < 50.0);
}
