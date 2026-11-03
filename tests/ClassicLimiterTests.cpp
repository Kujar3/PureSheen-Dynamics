#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "core/TruePeak.h"
#include "dsp/ClassicLimiter.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Classic Limiter tests (dsp/ClassicLimiter.h). Prefix "clim.". Owner: mod-clim.
namespace
{
using namespace ps;
using pstest::kPi;
using pstest::note;

constexpr double kSr = 48000.0;

struct Values
{
    std::vector<float> v;
    Values()
    {
        for (const auto& s : paramSpecs())
            v.push_back (s.defaultValue);
    }
    void set (const std::string& id, float x)
    {
        const int i = paramIndex ("clim." + id);
        REQUIRE (i >= 0);
        v[(size_t) i] = x;
    }
    ParamValues data() const { return v.data(); }
};

// Neutral: Analog, threshold 0, ceiling 0 (ClassicLimiter.h).
Values neutral()
{
    Values v;
    v.set ("thr", 0.0f);
    v.set ("ceiling", 0.0f);
    v.set ("mode", (float) ClassicLimiterMode::Analog);
    return v;
}

struct Stereo
{
    std::vector<float> l, r;
    size_t size() const { return l.size(); }
    void append (const Stereo& o)
    {
        l.insert (l.end(), o.l.begin(), o.l.end());
        r.insert (r.end(), o.r.begin(), o.r.end());
    }
};

Stereo dual (const std::vector<float>& x) { return { x, x }; }

std::unique_ptr<ClassicLimiter> makeLimiter (double sr, const Values& vals, int maxBlock = 512)
{
    auto m = std::make_unique<ClassicLimiter>();
    m->prepare (sr, maxBlock);
    m->setParams (vals.data());
    return m;
}

using BlockHook = std::function<void (size_t, Values&, ClassicLimiter&)>;
using AfterHook = std::function<void (size_t, ClassicLimiter&)>;

Stereo run (ClassicLimiter& m, Values& vals, const Stereo& in, int block = 512, const BlockHook& onBlock = {},
            Stereo* delta = nullptr, const AfterHook& afterBlock = {})
{
    Stereo out = in;
    if (delta != nullptr)
        *delta = Stereo { std::vector<float> (in.size(), 0.0f), std::vector<float> (in.size(), 0.0f) };
    ProcessContext ctx;
    for (size_t start = 0; start < in.size(); start += (size_t) block)
    {
        if (onBlock)
            onBlock (start, vals, m);
        m.setParams (vals.data());
        const int n = (int) std::min ((size_t) block, in.size() - start);
        float* ch[2] { out.l.data() + start, out.r.data() + start };
        float* d[2] { nullptr, nullptr };
        if (delta != nullptr)
        {
            d[0] = delta->l.data() + start;
            d[1] = delta->r.data() + start;
            ctx.deltaOut = d;
        }
        m.process (ch, n, ctx);
        if (afterBlock)
            afterBlock (start + (size_t) n, m);
    }
    return out;
}

double db (double x) { return pstest::dbOf (x); }

double maxDelayedDiff (const std::vector<float>& out, const std::vector<float>& in, int latency, double gain = 1.0,
                       size_t from = 0)
{
    double m = 0.0;
    for (size_t i = std::max (from, (size_t) latency); i < out.size(); ++i)
        m = std::max (m, std::abs ((double) out[i] - gain * (double) in[i - (size_t) latency]));
    return m;
}

double maxSecondDiff (const std::vector<float>& y, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = std::max<size_t> (from, 2); i < to && i < y.size(); ++i)
        m = std::max (m, std::abs ((double) y[i] - 2.0 * (double) y[i - 1] + (double) y[i - 2]));
    return m;
}

double toneDb (const std::vector<float>& y, size_t from, size_t len, double hz, double sr)
{
    return db (std::abs (pstest::toneOf (y, from, len, hz, sr)));
}

float samplePeak (const Stereo& s, size_t from = 0)
{
    float p = 0.0f;
    for (size_t i = from; i < s.size(); ++i)
        p = std::max ({ p, std::abs (s.l[i]), std::abs (s.r[i]) });
    return p;
}

float truePeakDb (const Stereo& s, size_t from = 0)
{
    TruePeakMeter m;
    m.prepare (48000.0);
    m.process (s.l.data() + from, s.r.data() + from, (int) (s.size() - from));
    return m.truePeakDb();
}

std::string fmt (double v, int prec = 2)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision (prec) << v;
    return os.str();
}

const char* modeName (int m) { return m == 0 ? "Analog" : (m == 1 ? "Tube" : "Modern"); }

// Signals ---------------------------------------------------------------------------------------------------------
Stereo silence (double seconds, double sr)
{
    const auto n = (size_t) std::llround (seconds * sr);
    return { std::vector<float> (n, 0.0f), std::vector<float> (n, 0.0f) };
}

// Hard material: full-scale squares, onset bursts, noise, spikes, an intersample test tone, kicks, DC steps.
Stereo torture (double sr, double peak = 1.0)
{
    Stereo s;
    {
        auto x = silence (0.25, sr);
        for (size_t i = 0; i < x.size(); ++i)
        {
            x.l[i] = (float) (((i / 37) % 2) ? peak : -peak);
            x.r[i] = (float) (((i / 53) % 2) ? peak : -peak);
        }
        s.append (x);
    }
    {
        auto x = silence (0.6, sr);
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = (double) (i % (size_t) (0.15 * sr)) / sr;
            const double e = std::exp (-t / 0.03);
            x.l[i] = (float) (peak * e * std::sin (2.0 * kPi * 2500.0 * t));
            x.r[i] = (float) (peak * e * std::sin (2.0 * kPi * 70.0 * t));
        }
        s.append (x);
    }
    s.append (Stereo { pstest::whiteNoise (peak, 0.25, sr, 11), pstest::whiteNoise (peak, 0.25, sr, 12) });
    s.append (Stereo { pstest::scaled (pstest::pinkNoise (1.0, 0.25, sr, 13), (float) (0.5 * peak)),
                       pstest::scaled (pstest::pinkNoise (1.0, 0.25, sr, 14), (float) (0.5 * peak)) });
    {
        auto x = silence (0.2, sr);
        for (size_t i = 0; i < x.size(); i += 401)
            x.l[i] = x.r[i] = (float) peak;
        s.append (x);
    }
    {
        // fs / 4 at 45 degrees: samples at 0.707 of the true peak
        auto x = silence (0.25, sr);
        for (size_t i = 0; i < x.size(); ++i)
            x.l[i] = x.r[i] = (float) (peak * std::sin (0.5 * kPi * (double) i + 0.25 * kPi));
        s.append (x);
    }
    {
        auto x = silence (0.6, sr);
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = (double) (i % (size_t) (0.3 * sr)) / sr;
            x.l[i] = x.r[i] = (float) (peak * std::exp (-t / 0.08) * std::sin (2.0 * kPi * 55.0 * t));
        }
        s.append (x);
    }
    {
        auto x = silence (0.3, sr);
        for (size_t i = 0; i < x.size(); ++i)
            x.l[i] = x.r[i] = (float) (((i / (size_t) (0.05 * sr)) % 2) ? peak : -0.5 * peak);
        s.append (x);
    }
    s.append (silence (0.05, sr));
    return s;
}

// Steady-state output level (dB re input) of the variable-mu stage for a sine of peak a (relative to the ceiling C = 1):
// the fixed point y = a / (1 + u (y)^2), u = (y - K) / (1 - K), K = 10^(-10 / 20) (ClassicLimiter.h).
double muFixedPointDb (double a)
{
    const double K = std::pow (10.0, -0.5);
    if (a <= K)
        return 0.0;
    double lo = K, hi = a;
    for (int it = 0; it < 100; ++it)
    {
        const double y = 0.5 * (lo + hi);
        const double u = (y - K) / (1.0 - K);
        if (y * (1.0 + u * u) > a)
            hi = y;
        else
            lo = y;
    }
    return 20.0 * std::log10 (0.5 * (lo + hi) / a);
}

// Mu reduction (dB, <= 0) of the last block after `seconds` of a steady sine.
double steadyMuDb (int mode, double character, double hz, double peakDb, double sr = kSr)
{
    Values v = neutral();
    v.set ("mode", (float) mode);
    v.set ("character", (float) character);
    auto m = makeLimiter (sr, v);
    const auto x = pstest::sine (hz, peakDb, 4.0, sr);
    run (*m, v, dual (x), 480);
    return m->getMeters().muReductionDb.load();
}

// Mu reduction per block (block = `block` samples) over a signal.
std::vector<float> muTrace (ClassicLimiter& m, Values& v, const Stereo& in, int block)
{
    std::vector<float> trace;
    run (m, v, in, block, {}, nullptr, [&] (size_t, ClassicLimiter& lim) { trace.push_back (lim.getMeters().muReductionDb.load()); });
    return trace;
}
} // namespace

//==============================================================================
TEST_CASE ("clim. module type and contract registration")
{
    ClassicLimiter m;
    CHECK (m.getType() == ModuleType::ClassicLimiter);
    CHECK_EQ (std::string (moduleInfo (ModuleType::ClassicLimiter).idPrefix), std::string ("clim"));
    CHECK (m.handlesDelta());
    CHECK_EQ (ClassicLimiter::latencyFor (44100.0), 109);
    CHECK_EQ (ClassicLimiter::latencyFor (48000.0), 117);
}

TEST_CASE ("clim. latency equals the measured impulse delay at 44.1 / 48 / 96 kHz in every mode")
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int mode = 0; mode < 3; ++mode)
            for (int tp = 0; tp < 2; ++tp)
                for (float character : { 0.0f, 10.0f })
                {
                    Values v = neutral();
                    v.set ("mode", (float) mode);
                    v.set ("truePeak", (float) tp);
                    v.set ("character", character);
                    auto m = makeLimiter (sr, v);
                    const int lat = m->getLatencySamples();
                    CHECK_EQ (lat, ClassicLimiter::latencyFor (sr));
                    auto in = silence (0.05, sr);
                    in.l[100] = in.r[100] = 0.25f;   // -12 dBFS: under the knee
                    const auto out = run (*m, v, in, 64);
                    size_t at = 0;
                    float best = 0.0f;
                    for (size_t i = 0; i < out.size(); ++i)
                        if (std::abs (out.l[i]) > best)
                        {
                            best = std::abs (out.l[i]);
                            at = i;
                        }
                    CHECK_EQ ((int) at - 100, lat);
                    CHECK (std::abs (best - 0.25f) < 1.0e-6f);
                    // constant whatever the parameters
                    v.set ("thr", -12.0f);
                    v.set ("ceiling", -6.0f);
                    v.set ("mode", (float) ((mode + 1) % 3));
                    v.set ("truePeak", (float) (1 - tp));
                    m->setParams (v.data());
                    CHECK_EQ (m->getLatencySamples(), lat);
                }
    note ("latency 44.1 / 48 / 96 kHz", ClassicLimiter::latencyFor (44100.0));
    note ("    48 kHz", ClassicLimiter::latencyFor (48000.0));
    note ("    96 kHz", ClassicLimiter::latencyFor (96000.0));
}

TEST_CASE ("clim. neutral settings are an exact delay in every mode (input under the knee)")
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int mode = 0; mode < 3; ++mode)
        {
            Values v = neutral();
            v.set ("mode", (float) mode);
            auto m = makeLimiter (sr, v);
            // pink noise and a sine, peaks at -12 dBFS (the knee starts at -10)
            auto a = pstest::pinkNoise (1.0, 1.5, sr, 3);
            const float pk = (float) pstest::peakOf (a, 0, a.size());
            a = pstest::scaled (a, 0.25f / pk);
            const auto b = pstest::sine (997.0, -12.0, 1.5, sr);
            const Stereo in { a, b };
            Stereo delta;
            const auto out = run (*m, v, in, 333, {}, &delta);
            const int lat = m->getLatencySamples();
            const double e = std::max (maxDelayedDiff (out.l, in.l, lat), maxDelayedDiff (out.r, in.r, lat));
            note (std::string ("neutral ") + modeName (mode) + " " + fmt (sr, 0) + ": max error vs delay", e);
            CHECK (e <= 1.0e-6);   // < -120 dB (bit-exact in practice)
            CHECK (pstest::peakOf (delta.l, 0, delta.size()) <= 1.0e-6);
            CHECK (m->getMeters().muReductionDb.load() == 0.0f);
            CHECK (m->getMeters().brickwallReductionDb.load() == 0.0f);
        }
}

TEST_CASE ("clim. threshold and ceiling set the gain (ceiling - threshold) under the knee")
{
    struct Case { float thr, ceiling, expectDb; };
    for (const Case c : { Case { -6.0f, 0.0f, 6.0f }, Case { -12.0f, 0.0f, 12.0f }, Case { -3.0f, -3.0f, 0.0f },
                          Case { 0.0f, -6.0f, -6.0f }, Case { -12.0f, -12.0f, 0.0f }, Case { -8.0f, -2.0f, 6.0f } })
    {
        Values v = neutral();
        v.set ("thr", c.thr);
        v.set ("ceiling", c.ceiling);
        auto m = makeLimiter (kSr, v);
        // 1 kHz at -30 dBFS: after the gain still >= 8 dB under the knee
        const auto x = pstest::sine (1000.0, -30.0, 1.0, kSr);
        const auto out = run (*m, v, dual (x), 256);
        const int lat = m->getLatencySamples();
        const double g = toneDb (out.l, 24000, 24000, 1000.0, kSr) - toneDb (x, 24000 - (size_t) lat, 24000, 1000.0, kSr);
        note ("thr " + fmt (c.thr, 0) + " ceiling " + fmt (c.ceiling, 0) + ": gain dB", g);
        CHECK (std::abs (g - c.expectDb) < 0.01);
        const double e = maxDelayedDiff (out.l, x, lat, std::pow (10.0, c.expectDb / 20.0), 4800);
        CHECK (e < 1.0e-6);   // a pure gain: no limiting, no colour
    }
}

TEST_CASE ("clim. variable-mu static curve: exactly 1 under the knee, the documented fixed point above, ratio rising")
{
    // Analog, character 10 (slow release: env ~ the peak), 1 kHz (passes the 60 Hz detector high-pass), ceiling 0.
    struct Point { double inDb, muDb, theoryDb; };
    std::vector<Point> pts;
    for (double inDb : { -12.0, -10.5, -7.0, -4.0, -2.0, 0.0, 2.0, 4.0, 6.0 })
    {
        const double mu = steadyMuDb (0, 10.0, 1000.0, inDb);
        const double th = muFixedPointDb (std::pow (10.0, inDb / 20.0));
        pts.push_back ({ inDb, mu, th });
        note ("mu reduction at " + fmt (inDb, 1) + " dBFS (theory " + fmt (th, 2) + ")", mu);
        if (inDb < -10.0)
            CHECK (mu == 0.0);
        else
            CHECK (std::abs (mu - th) < 0.35);
    }
    // documented points (ClassicLimiter.cpp): 1.0 dB at 6 dB over the knee start (-4 dBFS), 2.7 dB at the ceiling,
    // 6.0 dB at +6 dBFS, 9.7 dB at +12 dBFS
    CHECK (std::abs (muFixedPointDb (std::pow (10.0, -4.0 / 20.0)) + 1.04) < 0.01);
    CHECK (std::abs (muFixedPointDb (1.0) + 2.72) < 0.01);
    CHECK (std::abs (muFixedPointDb (std::pow (10.0, 6.0 / 20.0)) + 6.01) < 0.01);
    CHECK (std::abs (muFixedPointDb (std::pow (10.0, 12.0 / 20.0)) + 9.73) < 0.01);
    // effective ratio (input dB per output dB) rises with level above the knee
    double lastRatio = 1.0;
    for (size_t i = 3; i < pts.size(); ++i)
    {
        const double dIn = pts[i].inDb - pts[i - 1].inDb;
        const double dOut = dIn + pts[i].muDb - pts[i - 1].muDb;
        const double ratio = dIn / dOut;
        note ("    effective ratio " + fmt (pts[i - 1].inDb, 0) + " .. " + fmt (pts[i].inDb, 0) + " dBFS", ratio);
        CHECK (ratio > lastRatio);
        lastRatio = ratio;
    }
}

TEST_CASE ("clim. character sets attack and release per mode (fast 0 .. slow 10), Analog the fastest attack")
{
    // 1 kHz: -20 dBFS (under the knee) for 0.5 s, +6 dBFS for 1.5 s, -20 dBFS for 4 s. Ceiling 0, threshold 0.
    const double sr = kSr;
    const int block = 4;
    const auto quiet = pstest::dbfsToAmplitude (-20.0), loud = pstest::dbfsToAmplitude (6.0);
    std::vector<float> x ((size_t) (6.0 * sr));
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / sr;
        const double a = (t >= 0.5 && t < 2.0) ? loud : quiet;
        x[i] = (float) (a * std::sin (2.0 * kPi * 1000.0 * t));
    }
    double attack[3][2] {}, release[3][2] {};
    for (int mode = 0; mode < 3; ++mode)
        for (int ci = 0; ci < 2; ++ci)
        {
            Values v = neutral();
            v.set ("mode", (float) mode);
            v.set ("character", ci == 0 ? 0.0f : 10.0f);
            auto m = makeLimiter (sr, v);
            const auto tr = muTrace (*m, v, dual (x), block);
            const auto onBlock = (size_t) (0.5 * sr) / block, offBlock = (size_t) (2.0 * sr) / block;
            const double fin = tr[offBlock - 1];
            size_t a = onBlock;
            while (a < offBlock && tr[a] > 0.632 * fin)
                ++a;
            size_t r = offBlock;
            while (r < tr.size() && tr[r] < 0.368 * fin)
                ++r;
            attack[mode][ci] = 1000.0 * (double) ((a - onBlock) * block) / sr;
            release[mode][ci] = 1000.0 * (double) ((r - offBlock) * block) / sr;
            note (std::string (modeName (mode)) + " character " + (ci ? "10" : "0") + ": steady mu dB", fin);
            note ("    attack to 63 % (ms)", attack[mode][ci]);
            note ("    release to 37 % (ms)", release[mode][ci]);
        }
    for (int mode = 0; mode < 3; ++mode)
    {
        CHECK (attack[mode][1] > 5.0 * std::max (attack[mode][0], 0.05));
        CHECK (release[mode][1] > 5.0 * release[mode][0]);
        CHECK (release[mode][0] > 10.0);
    }
    for (int ci = 0; ci < 2; ++ci)
    {
        CHECK (attack[0][ci] < attack[2][ci]);
        CHECK (attack[2][ci] < attack[1][ci]);
    }
}

TEST_CASE ("clim. program-dependent release: a long stretch of reduction recovers slower than a short hit")
{
    for (int mode = 0; mode < 3; ++mode)
    {
        double rel[2] {};
        for (int longHit = 0; longHit < 2; ++longHit)
        {
            const double hitS = longHit ? 3.0 : 0.05;
            std::vector<float> x ((size_t) ((1.0 + hitS + 3.0) * kSr));
            for (size_t i = 0; i < x.size(); ++i)
            {
                const double t = (double) i / kSr;
                const double a = (t >= 1.0 && t < 1.0 + hitS) ? 2.0 : 0.1;
                x[i] = (float) (a * std::sin (2.0 * kPi * 1000.0 * t));
            }
            Values v = neutral();
            v.set ("mode", (float) mode);
            v.set ("character", 3.0f);
            auto m = makeLimiter (kSr, v);
            const auto tr = muTrace (*m, v, dual (x), 16);
            const auto off = (size_t) ((1.0 + hitS) * kSr) / 16;
            const double from = tr[off - 1];
            size_t r = off;
            while (r < tr.size() && tr[r] < 0.368 * from)
                ++r;
            rel[longHit] = 1000.0 * (double) ((r - off) * 16) / kSr;
            note (std::string (modeName (mode)) + (longHit ? " 3 s hit" : " 50 ms hit") + ": reduction before release", from);
            note ("    release to 37 % (ms)", rel[longHit]);
        }
        CHECK (rel[1] > 2.0 * rel[0]);
        CHECK (rel[1] < 4.5 * rel[0]);
    }
}

TEST_CASE ("clim. modes: Analog is clean with tighter lows, Tube adds mostly 2nd harmonic, Modern half of it")
{
    // 1 kHz at +3 dBFS, ceiling 0: about 4.3 dB of mu reduction, the stage output under the ceiling (brickwall idle).
    const size_t from = (size_t) (2.0 * kSr), len = (size_t) kSr;
    double h2[3] {}, h3[3] {}, worstOther[3] {};
    for (int mode = 0; mode < 3; ++mode)
    {
        Values v = neutral();
        v.set ("mode", (float) mode);
        v.set ("character", 10.0f);
        auto m = makeLimiter (kSr, v);
        const auto x = pstest::sine (1000.0, 3.0, 3.0, kSr);
        const auto out = run (*m, v, dual (x), 512);
        CHECK (m->getMeters().brickwallReductionDb.load() > -0.01f);
        const double f = toneDb (out.l, from, len, 1000.0, kSr);
        h2[mode] = toneDb (out.l, from, len, 2000.0, kSr) - f;
        h3[mode] = toneDb (out.l, from, len, 3000.0, kSr) - f;
        worstOther[mode] = -300.0;
        for (int k = 4; k <= 20; ++k)
            worstOther[mode] = std::max (worstOther[mode], toneDb (out.l, from, len, 1000.0 * k, kSr) - f);
        note (std::string (modeName (mode)) + ": H2 dBc", h2[mode]);
        note ("    H3 dBc", h3[mode]);
        note ("    worst H4..H20 dBc", worstOther[mode]);
    }
    CHECK (h2[0] < -80.0 && h3[0] < -80.0);
    CHECK (h2[1] > -50.0 && h2[1] < -25.0);
    CHECK (h2[1] > h3[1] + 3.0);
    CHECK (std::abs ((h2[2] - h2[1]) + 6.02) < 1.0);   // kModernBlend 0.5
    CHECK (worstOther[1] < h3[1]);

    // tighter lows: Analog's detector high-pass leaves a 35 Hz tone far less reduced than a 1 kHz one; Tube does not
    const double a35 = steadyMuDb (0, 10.0, 35.0, 3.0), a1k = steadyMuDb (0, 10.0, 1000.0, 3.0);
    const double t35 = steadyMuDb (1, 10.0, 35.0, 3.0), t1k = steadyMuDb (1, 10.0, 1000.0, 3.0);
    note ("Analog mu at 35 Hz / 1 kHz", a35);
    note ("    1 kHz", a1k);
    note ("Tube mu at 35 Hz / 1 kHz", t35);
    note ("    1 kHz", t1k);
    CHECK (a35 > 0.5 * a1k);   // values are negative: less than half the reduction
    CHECK (std::abs (t35 - t1k) < 1.0);
}

TEST_CASE ("clim. Tube saturation is oversampled: aliases stay far below the harmonics")
{
    // 7.3 kHz at +3 dBFS: H2..H3 lie above 14 kHz, everything else must be tiny
    const double hz = 7300.0;
    Values v = neutral();
    v.set ("mode", (float) ClassicLimiterMode::Tube);
    v.set ("character", 10.0f);
    auto m = makeLimiter (kSr, v);
    const auto x = pstest::sine (hz, 3.0, 2.0, kSr);
    const auto out = run (*m, v, dual (x), 512);
    const size_t n = 32768, from = (size_t) kSr - n / 2;
    std::vector<pstest::cd> a (n);
    for (size_t i = 0; i < n; ++i)
        a[i] = (double) out.l[from + i] * (0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) n));
    pstest::fftInPlace (a);
    std::vector<double> mag (n / 2);
    double fund = 0.0;
    for (size_t k = 0; k < n / 2; ++k)
    {
        mag[k] = 4.0 * std::abs (a[k]) / (double) n;
        fund = std::max (fund, mag[k]);
    }
    double worst = -300.0, worstHz = 0.0;
    for (size_t k = 20; k < n / 2; ++k)
    {
        const double f = (double) k * kSr / (double) n;
        bool harmonic = false;
        for (int h = 1; h <= 3; ++h)
            harmonic |= std::abs (f - h * hz) < 60.0;
        if (! harmonic && db (mag[k] / fund) > worst)
        {
            worst = db (mag[k] / fund);
            worstHz = f;
        }
    }
    note ("Tube 7.3 kHz +3 dBFS: worst non-harmonic component dBc", worst);
    note ("    at Hz", worstHz);
    CHECK (worst < -70.0);
}

TEST_CASE ("clim. output never above the ceiling (sample peak; true peak + 0.05 dB with True Peak on)")
{
    double worstSp = -100.0, worstTp = -100.0;
    std::string worstSpWhat, worstTpWhat;
    int runs = 0;
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const auto in = torture (sr, 2.0);   // +6 dBFS material, +12 dB more gain at threshold -12
        for (int mode = 0; mode < 3; ++mode)
            for (float character : { 0.0f, 5.0f, 10.0f })
                for (int tp = 0; tp < 2; ++tp)
                    for (float ceilingDb : { 0.0f, -0.3f, -6.0f, -12.0f })
                    {
                        Values v;
                        v.set ("mode", (float) mode);
                        v.set ("character", character);
                        v.set ("truePeak", (float) tp);
                        v.set ("ceiling", ceilingDb);
                        v.set ("thr", -12.0f);
                        auto m = makeLimiter (sr, v);
                        const auto out = run (*m, v, in, 500);
                        ++runs;
                        const std::string what = std::string (modeName (mode)) + " ch " + fmt (character, 0) + " ceil "
                                                 + fmt (ceilingDb, 1) + " sr " + fmt (sr, 0);
                        const double lim = std::pow (10.0, ceilingDb / 20.0);
                        const double sp = db (samplePeak (out)) - ceilingDb;
                        if (sp > worstSp)
                        {
                            worstSp = sp;
                            worstSpWhat = what;
                        }
                        CHECK ((double) samplePeak (out) <= lim);
                        if (tp)
                        {
                            const double over = truePeakDb (out) - ceilingDb;
                            if (over > worstTp)
                            {
                                worstTp = over;
                                worstTpWhat = what;
                            }
                            CHECK (over <= 0.05);
                        }
                        const float peakMeter = m->getMeters().outputPeakDb.load();
                        CHECK (peakMeter <= ceilingDb + 1.0e-4f);
                    }
    }
    std::cout << "    " << runs << " runs; worst sample peak re ceiling " << fmt (worstSp, 5) << " dB (" << worstSpWhat
              << "), worst true peak " << fmt (worstTp, 4) << " dB (" << worstTpWhat << ")\n";
}

TEST_CASE ("clim. ceiling steps never exceed the ceiling, also while they travel through the lookahead")
{
    // ceiling steps every 130 ms while limiting hard (threshold -12): every output sample is bounded by the ceiling
    // stamped on its input sample, which glides 20 ms from the old block value to the new one
    const auto in = torture (kSr, 1.0);
    const float steps[] { 0.0f, -6.0f, -1.0f, -12.0f, -0.1f, -3.0f, -9.0f };
    for (int mode = 0; mode < 3; ++mode)
    {
        Values v;
        v.set ("mode", (float) mode);
        v.set ("thr", -12.0f);
        auto m = makeLimiter (kSr, v);
        const int lat = m->getLatencySamples();
        std::vector<double> ceilAt (in.size(), 1.0);
        double current = std::pow (10.0, v.v[(size_t) paramIndex ("clim.ceiling")] / 20.0);
        double target = current, step = 0.0;
        int remaining = 0;
        const auto out = run (*m, v, in, 64, [&] (size_t start, Values& vals, ClassicLimiter&) {
            const float c = steps[(start / (size_t) (0.13 * kSr)) % 7];
            vals.set ("ceiling", c);
            const double next = std::pow (10.0, c / 20.0);
            if (next != target)
            {
                target = next;
                remaining = (int) std::lround (0.020 * kSr);
                step = (target - current) / remaining;
            }
            for (size_t i = start; i < std::min (in.size(), start + 64); ++i)
            {
                if (remaining > 0)
                    current = --remaining == 0 ? target : current + step;
                ceilAt[i] = current;
            }
        });
        double worst = -100.0;
        for (size_t i = (size_t) lat; i < out.size(); ++i)
        {
            const double bound = ceilAt[i - (size_t) lat];
            worst = std::max (worst, db (std::max (std::abs (out.l[i]), std::abs (out.r[i]))) - db (bound));
        }
        note (std::string (modeName (mode)) + ": worst peak re the stamped ceiling bound dB", worst);
        CHECK (worst <= 1.0e-4);
    }
}

TEST_CASE ("clim. Delta is what the limiting stages changed (output - gain x input, delayed)")
{
    for (int mode = 0; mode < 3; ++mode)
    {
        Values v;
        v.set ("mode", (float) mode);
        v.set ("thr", -6.0f);
        v.set ("ceiling", -1.0f);
        auto m = makeLimiter (kSr, v);
        const auto in = torture (kSr, 0.7);
        Stereo delta;
        const auto out = run (*m, v, in, 256, {}, &delta);
        const int lat = m->getLatencySamples();
        const double g = std::pow (10.0, 5.0 / 20.0);
        double e = 0.0;
        for (size_t i = (size_t) lat; i < out.size(); ++i)
            e = std::max (e, std::abs ((double) out.l[i] - (double) delta.l[i] - g * (double) in.l[i - (size_t) lat]));
        note (std::string (modeName (mode)) + ": max |out - delta - G x in|", e);
        CHECK (e < 1.0e-5);
        CHECK (pstest::peakOf (delta.l, 0, delta.size()) > 0.05);   // it limits

        // under the knee the Threshold / Ceiling gain is not heard in Delta
        Values q = neutral();
        q.set ("mode", (float) mode);
        q.set ("thr", -6.0f);
        auto mq = makeLimiter (kSr, q);
        const auto quiet = pstest::sine (300.0, -24.0, 0.5, kSr);
        Stereo dq;
        run (*mq, q, dual (quiet), 256, {}, &dq);
        CHECK (pstest::peakOf (dq.l, 0, dq.size()) < 1.0e-6);
    }

    // a host without deltaOut gets the delta in place of the output
    Values v;
    v.set ("thr", -6.0f);
    auto a = makeLimiter (kSr, v), b = makeLimiter (kSr, v);
    const auto in = torture (kSr, 0.7);
    Stereo delta;
    run (*a, v, in, 256, {}, &delta);
    Stereo inPlace = in;
    ProcessContext ctx;
    ctx.delta = true;
    for (size_t start = 0; start < in.size(); start += 256)
    {
        b->setParams (v.data());
        float* ch[2] { inPlace.l.data() + start, inPlace.r.data() + start };
        b->process (ch, (int) std::min ((size_t) 256, in.size() - start), ctx);
    }
    CHECK (maxDelayedDiff (inPlace.l, delta.l, 0) == 0.0);
}

TEST_CASE ("clim. stereo link: one gain for both channels")
{
    Values v = neutral();
    v.set ("mode", (float) ClassicLimiterMode::Tube);
    auto m = makeLimiter (kSr, v);
    const auto loud = pstest::sine (1000.0, 4.0, 2.0, kSr);
    const auto quiet = pstest::sine (300.0, -30.0, 2.0, kSr);
    const auto out = run (*m, v, Stereo { loud, quiet }, 512);
    const int lat = m->getLatencySamples();
    std::vector<float> qd (quiet.size(), 0.0f);
    for (size_t i = (size_t) lat; i < quiet.size(); ++i)
        qd[i] = quiet[i - (size_t) lat];
    const double gR = toneDb (out.r, 48000, 48000, 300.0, kSr) - toneDb (qd, 48000, 48000, 300.0, kSr);
    const double mu = m->getMeters().muReductionDb.load();
    note ("quiet right channel change dB (mu reduction of the loud left)", gR);
    note ("    mu dB", mu);
    CHECK (gR < -1.0);
    CHECK (std::abs (gR - mu) < 0.3);
}

TEST_CASE ("clim. meters and the gain-reduction history")
{
    Values v;
    v.set ("thr", -9.0f);
    v.set ("ceiling", -1.0f);
    v.set ("mode", (float) ClassicLimiterMode::Tube);
    auto m = makeLimiter (kSr, v);
    m->getMeters().peakReductionDb.exchange (0.0f);
    const auto in = torture (kSr, 0.5);
    double worstOrder = 0.0;
    int pairs = 0;
    std::vector<float> l (8192), r (8192);
    const auto out = run (*m, v, in, 100, {}, nullptr, [&] (size_t, ClassicLimiter& lim) {
        const auto& mt = lim.getMeters();
        CHECK (mt.muReductionDb.load() <= 0.0f);
        CHECK (mt.brickwallReductionDb.load() <= 0.0f);
        const int got = lim.getHistoryFifo().pull (l.data(), r.data(), (int) l.size());
        for (int i = 0; i < got; ++i)
        {
            CHECK (l[(size_t) i] <= 0.0f);
            worstOrder = std::max (worstOrder, (double) r[(size_t) i] - (double) l[(size_t) i]);   // total <= mu
        }
        pairs += got;
    });
    note ("history pairs (one per 64 samples)", pairs);
    CHECK_EQ (pairs, (int) (in.size() / 64));
    CHECK (worstOrder <= 1.0e-5);
    const float peakRed = m->getMeters().peakReductionDb.exchange (0.0f);
    const float peakOut = m->getMeters().outputPeakDb.exchange (-200.0f);
    note ("peak reduction (positive dB)", peakRed);
    note ("output peak dBFS", peakOut);
    CHECK (peakRed > 3.0f);
    CHECK (peakOut <= -1.0f + 1.0e-4f);
    CHECK (peakOut > -1.5f);
    CHECK (std::abs (peakOut - (float) db (samplePeak (out))) < 1.0e-3f);
}

TEST_CASE ("clim. parameter changes are click-free")
{
    struct Move
    {
        const char* name;
        std::function<void (Values&)> before, after;
        double inputDb;
    };
    const std::vector<Move> moves {
        { "threshold 0 -> -12", [] (Values&) {}, [] (Values& v) { v.set ("thr", -12.0f); }, -6.0 },
        { "threshold -12 -> 0", [] (Values& v) { v.set ("thr", -12.0f); }, [] (Values&) {}, -6.0 },
        { "ceiling 0 -> -12 (threshold -12)", [] (Values& v) { v.set ("thr", -12.0f); },
          [] (Values& v) { v.set ("thr", -12.0f); v.set ("ceiling", -12.0f); }, -9.0 },
        { "ceiling and threshold -6 -> 0 (linked)", [] (Values& v) { v.set ("thr", -9.0f); v.set ("ceiling", -6.0f); },
          [] (Values& v) { v.set ("thr", -3.0f); v.set ("ceiling", 0.0f); }, -6.0 },
        { "mode Analog -> Tube", [] (Values& v) { v.set ("thr", -6.0f); },
          [] (Values& v) { v.set ("thr", -6.0f); v.set ("mode", 1.0f); }, -6.0 },
        { "mode Tube -> Modern", [] (Values& v) { v.set ("thr", -6.0f); v.set ("mode", 1.0f); },
          [] (Values& v) { v.set ("thr", -6.0f); v.set ("mode", 2.0f); }, -6.0 },
        { "mode Modern -> Analog", [] (Values& v) { v.set ("thr", -6.0f); v.set ("mode", 2.0f); },
          [] (Values& v) { v.set ("thr", -6.0f); v.set ("mode", 0.0f); }, -6.0 },
        { "character 10 -> 0", [] (Values& v) { v.set ("thr", -6.0f); v.set ("character", 10.0f); },
          [] (Values& v) { v.set ("thr", -6.0f); v.set ("character", 0.0f); }, -6.0 },
        { "character 0 -> 10", [] (Values& v) { v.set ("thr", -6.0f); v.set ("character", 0.0f); },
          [] (Values& v) { v.set ("thr", -6.0f); v.set ("character", 10.0f); }, -6.0 },
        { "true peak off -> on", [] (Values& v) { v.set ("thr", -6.0f); },
          [] (Values& v) { v.set ("thr", -6.0f); v.set ("truePeak", 1.0f); }, -6.0 },
        { "link on (no DSP effect)", [] (Values& v) { v.set ("thr", -6.0f); },
          [] (Values& v) { v.set ("thr", -6.0f); v.set ("link", 1.0f); }, -6.0 },
    };

    const auto tSwitch = (size_t) (3.0 * kSr);
    for (const auto& mv : moves)
    {
        // 150 Hz: a small second difference, so a step in gain or state shows up at once
        const auto x = pstest::sine (150.0, mv.inputDb, 6.0, kSr);
        Values vals;
        mv.before (vals);
        auto m = makeLimiter (kSr, vals);
        const auto out = run (*m, vals, dual (x), 64, [&] (size_t start, Values& v, ClassicLimiter&) {
            if (start < tSwitch)
                return;
            v = Values();
            mv.after (v);
        });
        const auto lat = (size_t) m->getLatencySamples();
        const double steadyBefore = maxSecondDiff (out.l, (size_t) (2.0 * kSr), tSwitch);
        const double steadyAfter = maxSecondDiff (out.l, (size_t) (5.0 * kSr), out.size());
        const double during = maxSecondDiff (out.l, tSwitch, tSwitch + lat + (size_t) (1.0 * kSr));
        const double ratio = during / std::max (steadyBefore, steadyAfter);
        note (std::string (mv.name) + ": transition / steady second difference", ratio);
        CHECK (ratio <= 4.0);
    }
}

TEST_CASE ("clim. output does not depend on the host block size")
{
    const auto in = torture (kSr, 0.8);
    auto render = [&] (int block) {
        Values v;
        v.set ("thr", -8.0f);
        auto m = makeLimiter (kSr, v, block);
        Stereo out = in;
        ProcessContext ctx;
        const size_t switches[] { (size_t) (0.4 * kSr), (size_t) (0.9 * kSr), (size_t) (1.7 * kSr), (size_t) (2.2 * kSr) };
        for (size_t start = 0; start < out.size();)
        {
            v.set ("mode", start < switches[0] ? 0.0f : (start < switches[1] ? 1.0f : 2.0f));
            v.set ("character", start < switches[2] ? 2.0f : 8.0f);
            v.set ("ceiling", start < switches[3] ? -0.1f : -3.0f);
            m->setParams (v.data());
            size_t end = std::min (start + (size_t) block, out.size());
            for (size_t s : switches)
                if (start < s)
                    end = std::min (end, s);
            float* ch[2] { out.l.data() + start, out.r.data() + start };
            m->process (ch, (int) (end - start), ctx);
            start = end;
        }
        return out;
    };
    const auto reference = render (64);
    for (int block : { 1, 17, 500 })
    {
        const auto out = render (block);
        const double e = std::max (maxDelayedDiff (out.l, reference.l, 0), maxDelayedDiff (out.r, reference.r, 0));
        note ("block " + std::to_string (block) + ": max difference vs block 64", e);
        CHECK (e == 0.0);
    }
}

TEST_CASE ("clim. stable, finite and denormal-free at the extremes")
{
    for (int mode = 0; mode < 3; ++mode)
        for (float character : { 0.0f, 10.0f })
        {
            Values v;
            v.set ("mode", (float) mode);
            v.set ("character", character);
            v.set ("thr", -12.0f);
            v.set ("ceiling", 0.0f);
            auto m = makeLimiter (kSr, v);
            Stereo in = torture (kSr, 8.0);   // +18 dBFS into +12 dB of gain
            {
                // DC, alternating full scale, then tiny values and silence
                auto dc = silence (0.3, kSr);
                std::fill (dc.l.begin(), dc.l.end(), 4.0f);
                std::fill (dc.r.begin(), dc.r.end(), -4.0f);
                in.append (dc);
                auto alt = silence (0.2, kSr);
                for (size_t i = 0; i < alt.size(); ++i)
                    alt.l[i] = alt.r[i] = (i & 1) ? 8.0f : -8.0f;
                in.append (alt);
                auto tiny = silence (0.5, kSr);
                for (size_t i = 0; i < tiny.size(); ++i)
                    tiny.l[i] = tiny.r[i] = (float) (1.0e-30 * ((i & 1) ? 1.0 : -1.0));
                in.append (tiny);
                in.append (silence (3.0, kSr));
            }
            Values automated = v;
            const auto out = run (*m, automated, in, 128, [&] (size_t start, Values& vals, ClassicLimiter&) {
                // jumps every 0.1 s across the whole ranges
                const int k = (int) (start / (size_t) (0.1 * kSr));
                vals.set ("thr", (k % 3) == 0 ? -12.0f : ((k % 3) == 1 ? 0.0f : -6.0f));
                vals.set ("ceiling", (k % 4) == 0 ? -12.0f : 0.0f);
                vals.set ("character", (k % 5) < 2 ? 0.0f : 10.0f);
                vals.set ("mode", (float) ((k / 2) % 3));
                vals.set ("truePeak", (float) (k % 2));
            });
            bool finite = true, denormal = false;
            for (size_t i = 0; i < out.size(); ++i)
                for (float y : { out.l[i], out.r[i] })
                {
                    finite &= std::isfinite (y) && std::abs (y) <= 1.0f;
                    denormal |= std::fpclassify (y) == FP_SUBNORMAL;
                }
            CHECK (finite);
            CHECK (! denormal);
            // after 3 s of silence the output is exactly silent again
            CHECK (pstest::peakOf (out.l, out.size() - (size_t) kSr, (size_t) kSr) == 0.0);
            const auto& mt = m->getMeters();
            CHECK (std::isfinite (mt.muReductionDb.load()) && std::isfinite (mt.brickwallReductionDb.load()));
        }
    // non-finite parameter values fall back to the defaults
    Values bad;
    bad.set ("thr", std::nanf (""));
    bad.set ("ceiling", INFINITY);
    bad.set ("character", -INFINITY);
    auto m = makeLimiter (kSr, bad);
    const auto out = run (*m, bad, dual (pstest::sine (440.0, 0.0, 0.5, kSr)), 256);
    bool finite = true;
    for (float y : out.l)
        finite &= std::isfinite (y);
    CHECK (finite);
}

TEST_CASE ("clim. interrupted three-mode fades preserve the attack-time bounds")
{
    auto v = neutral();
    v.set ("character", 0.0f);
    auto m = makeLimiter (kSr, v, 1);
    double previousEnv = 0.0, previousLevel = 0.0;
    double fastestMs = 1.0, slowestMs = 0.0;
    int measured = 0;
    const double knee = std::pow (10.0, -10.0 / 20.0);
    for (int i = 0; i < 24000; ++i)
    {
        // Interrupt Analog -> Tube with Modern, then keep interrupting the two
        // non-Analog modes. Once Analog has faded out their detector is plain peak.
        v.set ("mode", i < 240 ? 0.0f : (float) (1 + ((i / 240 - 1) % 2)));
        m->setParams (v.data());
        const float input = (i / 400) % 2 == 0 ? 0.5f : 2.0f;
        float l = input, r = input;
        float* ch[2] { &l, &r };
        m->process (ch, 1, {});
        const double gain = std::pow (10.0, m->getMeters().muReductionDb.load() / 20.0);
        const double env = knee + (1.0 - knee) * std::sqrt (std::max (0.0, 1.0 / gain - 1.0));
        if (i > 12000 && previousLevel - previousEnv > 0.01 && env > knee + 0.05)
        {
            const double alpha = (env - previousEnv) / (previousLevel - previousEnv);
            const double ms = -1000.0 / (kSr * std::log1p (-alpha));
            fastestMs = std::min (fastestMs, ms);
            slowestMs = std::max (slowestMs, ms);
            ++measured;
        }
        previousEnv = env;
        previousLevel = (double) input * gain;
    }
    note ("interrupted fades: fastest inferred attack ms", fastestMs);
    note ("interrupted fades: slowest inferred attack ms", slowestMs);
    note ("interrupted fades: measured attacks", measured);
    CHECK (measured > 100);
    CHECK (fastestMs >= 0.199);
    CHECK (slowestMs <= 0.401);
}

TEST_CASE ("clim. history is block independent and measures reduction at the output sample")
{
    const auto in = torture (kSr, 2.0);
    auto render = [&] (int block, bool checkOutput) {
        auto v = neutral(); // clean mode: output/input directly measures total gain
        v.set ("thr", -12.0f);
        v.set ("character", 0.0f);
        auto m = makeLimiter (kSr, v, block);
        Stereo history;
        float mu[64] {}, total[64] {};
        const auto out = run (*m, v, in, block, {}, nullptr, [&] (size_t, ClassicLimiter& lim) {
            const int got = lim.getHistoryFifo().pull (mu, total, 64);
            history.l.insert (history.l.end(), mu, mu + got);
            history.r.insert (history.r.end(), total, total + got);
        });
        if (checkOutput)
        {
            const int latency = m->getLatencySamples();
            const double inputGain = std::pow (10.0, 12.0 / 20.0);
            double worst = 0.0;
            int checked = 0;
            for (size_t h = 0; h < history.size(); ++h)
            {
                double minimum = 0.0;
                bool allNonzero = true;
                for (size_t i = h * 64; i < (h + 1) * 64; ++i)
                {
                    if (i < (size_t) latency || std::abs (in.l[i - (size_t) latency]) < 0.01f)
                    {
                        allNonzero = false;
                        break;
                    }
                    minimum = std::min (minimum, db (std::abs ((double) out.l[i]
                        / ((double) in.l[i - (size_t) latency] * inputGain))));
                }
                if (allNonzero)
                {
                    worst = std::max (worst, std::abs (minimum - history.r[h]));
                    ++checked;
                }
            }
            note ("history vs actual output gain: max error dB", worst);
            CHECK (checked > 100);
            CHECK (worst < 2.0e-5);
        }
        return history;
    };
    const auto reference = render (64, true);
    for (int block : { 1, 17, 500 })
    {
        const auto history = render (block, false);
        REQUIRE (history.size() == reference.size());
        const double error = std::max (maxDelayedDiff (history.l, reference.l, 0),
                                       maxDelayedDiff (history.r, reference.r, 0));
        note ("history block " + std::to_string (block) + ": max difference dB", error);
        CHECK (error == 0.0);
    }
}

TEST_CASE ("clim. process and setParams do not allocate")
{
    Values v;
    v.set ("thr", -10.0f);
    auto m = makeLimiter (kSr, v, 512);
    Stereo s = torture (kSr, 0.5);
    ProcessContext ctx;
    std::vector<float> dl (512), dr (512);
    float* d[2] { dl.data(), dr.data() };
    ctx.deltaOut = d;
    long long count = 0;
    {
        pstest::AllocationScope scope;
        for (size_t start = 0; start + 512 <= s.size(); start += 512)
        {
            v.set ("mode", (float) ((start / 4096) % 3));
            v.set ("character", (float) ((start / 2048) % 11));
            v.set ("truePeak", (float) ((start / 8192) % 2));
            m->setParams (v.data());
            float* ch[2] { s.l.data() + start, s.r.data() + start };
            m->process (ch, 512, ctx);
        }
        m->reset();
        count = scope.count();
    }
    CHECK_EQ (count, 0LL);
}

TEST_CASE ("clim. cpu: real-time factor at 48 kHz stereo (Tube, True Peak on; >= 40x)")
{
    Values v;
    v.set ("thr", -8.0f);
    v.set ("mode", (float) ClassicLimiterMode::Tube);
    v.set ("truePeak", 1.0f);
    auto m = makeLimiter (kSr, v);
    Stereo s = torture (kSr, 0.8);
    while (s.size() < (size_t) (10.0 * kSr))
        s.append (torture (kSr, 0.8));
    pstest::Stopwatch sw;
    run (*m, v, s, 512);
    const double factor = ((double) s.size() / kSr) / std::max (1.0e-9, sw.seconds());
    note ("real-time factor", factor);
    CHECK (factor > 40.0);
}

TEST_CASE ("clim. corrupt samples are replaced without poisoning audio histories")
{
    for (double rate : { 44100.0, 96000.0 })
        for (int variant = 0; variant < 3; ++variant)
            for (int block : { 1, 17, 512 })
            {
                Values v;
                v.set ("mode", (float) variant);
                v.set ("thr", -6.0f);
                auto m = makeLimiter (rate, v);
                auto reference = makeLimiter (rate, v);
                Stereo damaged = dual (pstest::sine (997.0, -14.0, 4096.0 / rate, rate));
                auto clean = damaged;
                const float bad[] { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max() };
                for (size_t i = 0; i < 4; ++i)
                {
                    damaged.l[63 + i * 512] = bad[i];
                    clean.l[63 + i * 512] = 0.0f;
                }

                Stereo delta, referenceDelta;
                const auto out = run (*m, v, damaged, block, {}, &delta);
                const auto expected = run (*reference, v, clean, block, {}, &referenceDelta);
                bool finite = true;
                double error = 0.0;
                for (size_t i = 0; i < out.size(); ++i)
                {
                    finite = finite && std::isfinite (out.l[i]) && std::isfinite (out.r[i])
                             && std::isfinite (delta.l[i]) && std::isfinite (delta.r[i]);
                    error = std::max (error, std::abs ((double) out.l[i] - expected.l[i]));
                    error = std::max (error, std::abs ((double) out.r[i] - expected.r[i]));
                    error = std::max (error, std::abs ((double) delta.l[i] - referenceDelta.l[i]));
                    error = std::max (error, std::abs ((double) delta.r[i] - referenceDelta.r[i]));
                }
                CHECK (finite);
                CHECK (error < 1.0e-7);
            }
}
