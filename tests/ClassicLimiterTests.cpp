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
// The expected numbers are the reference's, measured on its renders (private notes out/vintage: ref-measurements.md,
// fit-clim.md); the tolerances are what the fitted model reaches.
namespace
{
using namespace ps;
using pstest::kPi;
using pstest::note;

constexpr double kSr = 48000.0;
constexpr double kMakeupDb[3] { 0.35, 0.35, 0.125 };   // small-signal gain = ceiling - threshold + this (measured)

// The make-up at a gain (ceiling - threshold, dB): full from 2 dB on (measured at 2 .. 12 dB), scaled down to 0 at
// 0 dB (not measured: threshold = ceiling is the neutral setting).
double makeupDb (int mode, double gainDb) { return kMakeupDb[mode] * std::clamp (gainDb / 2.0, 0.0, 1.0); }

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

// Threshold 0, ceiling 0: unity gain (the neutral setting).
Values neutral (int mode = 0)
{
    Values v;
    v.set ("thr", 0.0f);
    v.set ("ceiling", 0.0f);
    v.set ("mode", (float) mode);
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

// Evaluate each 4x interpolation point against the ceiling at that same fractional time. Normalizing the samples
// before interpolation is useful too, but changes the waveform and cannot alone prove a moving-ceiling violation.
double truePeakAboveStampedCeiling (const Stereo& s, const std::vector<double>& ceiling, size_t latency)
{
    auto i0 = [] (double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k)
        {
            const double a = 0.5 * x / k;
            term *= a * a;
            sum += term;
            if (term < 1.0e-14 * sum)
                break;
        }
        return sum;
    };
    double taps[4][16] {};
    for (int p = 0; p < 4; ++p)
    {
        double sum = 0.0;
        for (int k = 0; k < 16; ++k)
        {
            const double d = p + 4 * k - 31.5, x = d / 4.0, r = d / 31.5;
            taps[p][k] = std::sin (kPi * x) / (kPi * x) * i0 (7.0 * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0 (7.0);
            sum += taps[p][k];
        }
        for (int k = 0; k < 16; ++k)
            taps[p][k] = (double) (float) (taps[p][k] / sum);
    }
    double worst = 0.0;
    for (size_t n = latency + 16; n < s.size(); ++n)
        for (int p = 0; p < 4; ++p)
        {
            const size_t a = n - latency - 8;
            const double fraction = 0.125 + 0.25 * p;
            const double bound = ceiling[a] + fraction * (ceiling[a + 1] - ceiling[a]);
            double l = 0.0, r = 0.0;
            for (int k = 0; k < 16; ++k)
            {
                l += taps[p][k] * s.l[n - (size_t) k];
                r += taps[p][k] * s.r[n - (size_t) k];
            }
            worst = std::max ({ worst, std::abs (l) / bound, std::abs (r) / bound });
        }
    return db (worst);
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

// Release of the limiting after a 1 kHz hold 6 dB over the ceiling (threshold -6, ceiling 0, so 6 + make-up dB over):
// -30 dBFS for 0.5 s, 0 dBFS for `holdS`, -30 dBFS for 3 s. The reduction re the small-signal gain per 20 ms, fitted
// from 30 ms after the hold (as the reference's renders were measured) by a log-linear fit while it is above 0.05 dB.
struct Release
{
    double startDb = 0.0;   // reduction 0.5 .. 1 ms after the hold (dB, positive)
    double d0 = 0.0;        // fitted depth at the end of the hold (dB, positive)
    double tauS = 0.0;      // fitted time constant (s)
};

Release measureRelease (int mode, float character, double holdS, double sr = kSr)
{
    Values v;
    v.set ("mode", (float) mode);
    v.set ("character", character);
    v.set ("thr", -6.0f);
    v.set ("ceiling", 0.0f);
    auto m = makeLimiter (sr, v);
    const auto x = pstest::sineSequence (1000.0, { { -30.0, 0.5 }, { 0.0, holdS }, { -30.0, 3.0 } }, sr);
    const auto out = run (*m, v, dual (x), 480);
    const auto lat = (size_t) m->getLatencySamples();
    const double smallDb = 6.0 + kMakeupDb[mode] - 30.0;
    const auto end = (size_t) std::llround ((0.5 + holdS) * sr) + lat;
    const auto w = (size_t) std::llround (0.020 * sr);   // 20 cycles of 1 kHz
    Release r;
    {
        const auto ms = (size_t) std::llround (0.001 * sr);
        r.startDb = smallDb - toneDb (out.l, end + ms / 2, ms, 1000.0, sr);
    }
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int n = 0;
    for (size_t i = end + (size_t) std::llround (0.030 * sr); i + w < out.size(); i += w)
    {
        const double d = smallDb - toneDb (out.l, i, w, 1000.0, sr);
        if (d < 0.05)
            break;
        const double t = ((double) (i - end) + 0.5 * (double) w) / sr;
        sx += t;
        sy += std::log (d);
        sxx += t * t;
        sxy += t * std::log (d);
        ++n;
    }
    if (n >= 3)
    {
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        r.tauS = -1.0 / slope;
        r.d0 = std::exp ((sy - slope * sx) / n);
    }
    return r;
}

// Harmonic k (dBc) of a steady sine through the limiter (threshold -6, ceiling 0), the last second of 3 s.
double harmonicDbc (int mode, float character, double hz, double peakDb, int k, double sr = kSr)
{
    Values v;
    v.set ("mode", (float) mode);
    v.set ("character", character);
    v.set ("thr", -6.0f);
    v.set ("ceiling", 0.0f);
    auto m = makeLimiter (sr, v);
    const auto x = pstest::sine (hz, peakDb, 3.0, sr);
    const auto out = run (*m, v, dual (x), 512);
    const auto from = (size_t) (2.0 * sr), len = (size_t) sr;
    return toneDb (out.l, from, len, hz * k, sr) - toneDb (out.l, from, len, hz, sr);
}
} // namespace

//==============================================================================
TEST_CASE ("clim. module type and contract registration")
{
    ClassicLimiter m;
    CHECK (m.getType() == ModuleType::ClassicLimiter);
    CHECK_EQ (std::string (moduleInfo (ModuleType::ClassicLimiter).idPrefix), std::string ("clim"));
    CHECK (m.handlesDelta());
    CHECK_EQ (ClassicLimiter::latencyFor (44100.0), 104);
    CHECK_EQ (ClassicLimiter::latencyFor (48000.0), 112);
}

TEST_CASE ("clim. latency equals the measured impulse delay at 44.1 / 48 / 96 / 192 kHz in every mode")
{
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int mode = 0; mode < 3; ++mode)
            for (int tp = 0; tp < 2; ++tp)
                for (float character : { 0.0f, 10.0f })
                {
                    Values v = neutral (mode);
                    v.set ("truePeak", (float) tp);
                    v.set ("character", character);
                    auto m = makeLimiter (sr, v);
                    const int lat = m->getLatencySamples();
                    CHECK_EQ (lat, ClassicLimiter::latencyFor (sr));
                    auto in = silence (0.05, sr);
                    in.l[100] = in.r[100] = 0.25f;   // -12 dBFS: under the ceiling
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
                    v.set ("character", 10.0f - character);
                    m->setParams (v.data());
                    CHECK_EQ (m->getLatencySamples(), lat);
                }
    note ("latency 44.1 / 48 / 96 / 192 kHz", ClassicLimiter::latencyFor (44100.0));
    note ("    48 kHz", ClassicLimiter::latencyFor (48000.0));
    note ("    96 kHz", ClassicLimiter::latencyFor (96000.0));
    note ("    192 kHz", ClassicLimiter::latencyFor (192000.0));
}

TEST_CASE ("clim. neutral (threshold = ceiling) is an exact delay, and under the ceiling the module is its gain")
{
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int mode = 0; mode < 3; ++mode)
            for (float thr : { 0.0f, -6.0f })
            {
                Values v = neutral (mode);
                v.set ("thr", thr);
                auto m = makeLimiter (sr, v);
                // pink noise and a sine, peaks at -3 dBFS after the gain (under the ceiling)
                const double inPeakDb = -3.0 + thr - makeupDb (mode, -thr);
                auto a = pstest::pinkNoise (1.0, 1.5, sr, 3);
                const float pk = (float) pstest::peakOf (a, 0, a.size());
                a = pstest::scaled (a, (float) pstest::dbfsToAmplitude (inPeakDb) / pk);
                const auto b = pstest::sine (997.0, inPeakDb, 1.5, sr);
                const Stereo in { a, b };
                Stereo delta;
                const auto out = run (*m, v, in, 333, {}, &delta);
                const int lat = m->getLatencySamples();
                const double g = std::pow (10.0, (-thr + makeupDb (mode, -thr)) / 20.0);
                const double e = std::max (maxDelayedDiff (out.l, in.l, lat, g), maxDelayedDiff (out.r, in.r, lat, g));
                note (std::string (modeName (mode)) + " threshold " + fmt (thr, 0) + " " + fmt (sr, 0)
                      + ": max error vs gain x delay", e);
                CHECK (e <= (thr == 0.0f ? 0.0 : 1.0e-6));   // neutral: bit-exact; with gain < -120 dB
                CHECK (pstest::peakOf (delta.l, 0, delta.size()) <= 1.0e-6);
                CHECK (m->getMeters().muReductionDb.load() == 0.0f);
                CHECK (m->getMeters().brickwallReductionDb.load() == 0.0f);
            }
}

TEST_CASE ("clim. threshold and ceiling set the gain: ceiling - threshold + the make-up (+0.35 / +0.125 dB from 2 dB)")
{
    struct Case { float thr, ceiling, expectDb; };
    for (int mode = 0; mode < 3; ++mode)
        for (const Case c : { Case { -6.0f, 0.0f, 6.0f }, Case { -12.0f, 0.0f, 12.0f }, Case { -3.0f, -3.0f, 0.0f },
                              Case { 0.0f, -6.0f, -6.0f }, Case { -12.0f, -12.0f, 0.0f }, Case { -8.0f, -2.0f, 6.0f },
                              Case { -1.0f, 0.0f, 1.0f }, Case { -2.0f, 0.0f, 2.0f } })
        {
            Values v = neutral (mode);
            v.set ("thr", c.thr);
            v.set ("ceiling", c.ceiling);
            auto m = makeLimiter (kSr, v);
            // 1 kHz at -30 dBFS: after the gain still >= 5 dB under the ceiling
            const auto x = pstest::sine (1000.0, -30.0, 1.0, kSr);
            const auto out = run (*m, v, dual (x), 256);
            const int lat = m->getLatencySamples();
            const double g = toneDb (out.l, 24000, 24000, 1000.0, kSr) - toneDb (x, 24000 - (size_t) lat, 24000, 1000.0, kSr);
            const double expect = c.expectDb + makeupDb (mode, c.expectDb);
            note (std::string (modeName (mode)) + " thr " + fmt (c.thr, 0) + " ceiling " + fmt (c.ceiling, 0) + ": gain dB", g);
            CHECK (std::abs (g - expect) < 0.005);
            const double e = maxDelayedDiff (out.l, x, lat, std::pow (10.0, expect / 20.0), 4800);
            CHECK (e < 1.0e-6);   // a pure gain: no limiting, no colour
        }
}

TEST_CASE ("clim. static curve: linear up to the ceiling, then the output peak held at ceiling - 0.01 dB")
{
    // Reference (1 kHz staircase, threshold -6, ceiling 0, every mode): out = in + 6.35 up to -9 dBFS in, then the
    // output sample peak at -0.01 dBFS (< 0.05 dB of knee); ceiling -3 peaks at -3.01.
    for (int mode = 0; mode < 3; ++mode)
        for (float ceilingDb : { 0.0f, -3.0f })
        {
            double worst = 0.0;
            for (double inDb : { -18.0, -12.0, -9.0, -7.0, -6.0, -4.0, -2.0, 0.0, 3.0 })
            {
                Values v;
                v.set ("mode", (float) mode);
                v.set ("character", 5.0f);
                v.set ("thr", ceilingDb - 6.0f);
                v.set ("ceiling", ceilingDb);
                auto m = makeLimiter (kSr, v);
                const auto x = pstest::sine (1000.0, inDb, 2.0, kSr);
                const auto out = run (*m, v, dual (x), 512);
                const double peak = db (pstest::peakOf (out.l, (size_t) (1.5 * kSr), (size_t) (0.5 * kSr)));
                const double expect = std::min (inDb + 6.0 + kMakeupDb[mode], ceilingDb - 0.01);
                worst = std::max (worst, std::abs (peak - expect));
                if (inDb + 6.0 + kMakeupDb[mode] > ceilingDb)
                    CHECK (peak <= ceilingDb - 0.01 + 1.0e-4);
            }
            note (std::string (modeName (mode)) + " ceiling " + fmt (ceilingDb, 0) + ": worst static error dB", worst);
            CHECK (worst < 0.05);
        }
}

TEST_CASE ("clim. slow stage: depth and release time per mode and character (fitted to the reference)")
{
    // Expected = the fitted model; the reference's measured values in the comments. Release-start depth after 1 s at
    // 6.35 dB over (Modern 6.125), fitted from 30 ms on, and the release time constant. d0 < 0: not checked.
    struct Case { int mode; float character; double d0, tau; };
    const Case cases[] {
        { 0, 0.0f, 4.28, 0.37 }, { 0, 2.5f, 4.28, 0.81 }, { 0, 5.0f, 4.29, 1.22 },   // reference 4.28 .. 4.29 dB and
        { 0, 7.5f, 4.28, 1.64 }, { 0, 10.0f, 4.28, 1.99 },                           //   0.37 / 0.81 / 1.22 / 1.64 / 1.99 s
        { 1, 0.0f, 4.27, 0.30 }, { 1, 2.0f, 4.27, 0.99 },                            // reference 4.27 dB, 0.30 / 0.99 s
        { 1, 5.0f, -1.0, 1.96 },      // reference 2.23 s (round 1; its depth was not measured: 4.2 .. 5.2 dB)
        { 2, 0.0f, 1.51, 0.135 },     // reference 1.33 dB, 0.061 s: the fitted medium part releases slower (open)
        { 2, 10.0f, 1.36, 0.87 },     // reference 1.33 dB, 0.87 s
    };
    for (const auto& c : cases)
    {
        const auto r = measureRelease (c.mode, c.character, 1.0);
        const std::string what = std::string (modeName (c.mode)) + " C" + fmt (c.character, 1);
        note (what + ": release depth dB (expected " + fmt (c.d0, 2) + ")", r.d0);
        note ("    release tau s (expected " + fmt (c.tau, 3) + ")", r.tauS);
        if (c.d0 > 0.0)
            CHECK (std::abs (r.d0 - c.d0) < 0.1);
        CHECK (std::abs (r.tauS / c.tau - 1.0) < 0.08);
    }
}

TEST_CASE ("clim. deep reductions release slower than the exponential (fitted: threshold -12, 8.8 dB)")
{
    // reference (Analog C5, threshold -12, 1 s at 0 dBFS): 8.8 dB at the release start, 5.5 dB left after 1 s and
    // 4.2 dB after 1.4 s (an exponential with the 1.22 s of the shallow releases would leave 3.9 / 2.8 dB)
    Values v;
    v.set ("mode", 0.0f);
    v.set ("character", 5.0f);
    v.set ("thr", -12.0f);
    v.set ("ceiling", 0.0f);
    auto m = makeLimiter (kSr, v);
    const auto x = pstest::sineSequence (1000.0, { { -30.0, 0.5 }, { 0.0, 1.0 }, { -30.0, 3.0 } }, kSr);
    const auto out = run (*m, v, dual (x), 480);
    const auto lat = (size_t) m->getLatencySamples();
    const double smallDb = 12.0 + kMakeupDb[0] - 30.0;
    const auto end = (size_t) std::llround (1.5 * kSr) + lat, w = (size_t) std::llround (0.020 * kSr);
    auto at = [&] (double t) { return smallDb - toneDb (out.l, end + (size_t) std::llround (t * kSr) - w / 2, w, 1000.0, kSr); };
    const double d0 = at (0.02), d1 = at (1.0), d14 = at (1.4);
    note ("depth at 20 ms / 1 s / 1.4 s dB", d0);
    note ("    1 s", d1);
    note ("    1.4 s", d14);
    CHECK (std::abs (d0 - 8.44) < 0.15);   // fitted (reference 8.65 at 20 ms)
    CHECK (std::abs (d1 - 5.94) < 0.15);   // fitted (reference 5.5)
    CHECK (std::abs (d14 - 4.42) < 0.15);  // fitted (reference 4.2)
    CHECK (d1 > 8.7 * std::exp (-1.0 / 1.22) + 1.0);   // clearly slower than the exponential
}

TEST_CASE ("clim. slow stage charges within 20 ms: the release starts from the same depth after 20 ms as after 1 s")
{
    // reference: Analog C5 4.265 / 4.285 dB, Tube C0 4.256 / 4.267, Modern C10 1.303 / 1.328
    const float character[3] { 5.0f, 0.0f, 10.0f };
    for (int mode = 0; mode < 3; ++mode)
    {
        const auto brief = measureRelease (mode, character[mode], 0.02), longHold = measureRelease (mode, character[mode], 1.0);
        note (std::string (modeName (mode)) + ": release depth after 20 ms / 1 s", brief.d0);
        note ("    1 s", longHold.d0);
        CHECK (std::abs (brief.d0 - longHold.d0) < 0.15);
    }
}

TEST_CASE ("clim. harmonics when limiting (fitted): Analog and Modern follow a 100 Hz wave, Tube holds the gain")
{
    // threshold -6, -3 dBFS in = 3.35 dB over (Modern 3.125). Reference h3: Analog -25.0 (C0 / C5 / C10 alike),
    // Tube C5 -48.6, Modern C5 -20.8 dBc; at 1 kHz Analog -77, Tube C5 -98, Modern C5 -58 dBc.
    // Ours at 1 kHz: Analog and Tube hold the whole cycle (cleaner than the reference, both far under -60 dBc),
    // Modern ripples about 7 dB less than the reference (open).
    struct Case { int mode; float character; double hz, lo, hi; };
    const Case cases[] {
        { 0, 0.0f, 100.0, -27.0, -23.0 }, { 0, 5.0f, 100.0, -27.0, -23.0 }, { 0, 10.0f, 100.0, -27.0, -23.0 },
        { 1, 5.0f, 100.0, -50.6, -46.6 }, { 2, 5.0f, 100.0, -22.8, -18.8 },
        { 0, 5.0f, 1000.0, -300.0, -70.0 }, { 1, 5.0f, 1000.0, -300.0, -90.0 }, { 2, 5.0f, 1000.0, -68.0, -55.0 },
    };
    for (const auto& c : cases)
    {
        const double h3 = harmonicDbc (c.mode, c.character, c.hz, -3.0, 3);
        note (std::string (modeName (c.mode)) + " C" + fmt (c.character, 0) + " " + fmt (c.hz, 0) + " Hz: h3 dBc", h3);
        CHECK (h3 > c.lo && h3 < c.hi);
    }
    // under the ceiling: no colour at all (reference: h3 < -120 dBc at -9 dBFS in)
    for (int mode = 0; mode < 3; ++mode)
        CHECK (harmonicDbc (mode, 5.0f, 1000.0, -9.0, 3) < -120.0);
}

TEST_CASE ("clim. the channels are limited independently")
{
    // reference: a loud left channel leaves the right one untouched (stereo uncorrelated noise renders null 20 dB
    // deeper unlinked)
    for (int mode = 0; mode < 3; ++mode)
    {
        Values v = neutral (mode);
        auto m = makeLimiter (kSr, v);
        const auto loud = pstest::sine (1000.0, 4.0, 2.0, kSr);
        const auto quiet = pstest::sine (300.0, -30.0, 2.0, kSr);
        const auto out = run (*m, v, Stereo { loud, quiet }, 512);
        const int lat = m->getLatencySamples();
        const double e = maxDelayedDiff (out.r, quiet, lat, 1.0, (size_t) lat);
        const double mu = m->getMeters().muReductionDb.load();
        note (std::string (modeName (mode)) + ": quiet right channel max error vs gain x delay", e);
        note ("    slow reduction of the loud left dB", mu);
        CHECK (mu < -0.5);
        CHECK (e < 1.0e-6);
    }
}

TEST_CASE ("clim. output never above the ceiling (sample peak; true peak with True Peak on)")
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
                            CHECK (over <= 0.0);
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
        const double g = std::pow (10.0, (5.0 + kMakeupDb[mode]) / 20.0);
        double e = 0.0;
        for (size_t i = (size_t) lat; i < out.size(); ++i)
            e = std::max (e, std::abs ((double) out.l[i] - (double) delta.l[i] - g * (double) in.l[i - (size_t) lat]));
        note (std::string (modeName (mode)) + ": max |out - delta - G x in|", e);
        CHECK (e < 1.0e-5);
        CHECK (pstest::peakOf (delta.l, 0, delta.size()) > 0.05);   // it limits

        // under the ceiling the Threshold / Ceiling gain (and the make-up) is not heard in Delta
        Values q = neutral (mode);
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

TEST_CASE ("clim. interrupted three-mode fades keep the gain a blend of the modes' make-ups")
{
    // under the ceiling (0.25 + 6 dB of gain) the output is the input times the gain and the weighted make-up of the
    // three modes (settled on Tube at the end): interrupting fades
    // never loses or adds weight (the gain stays between the smallest and the largest make-up, and settles exactly)
    auto v = neutral (0);
    v.set ("thr", -6.0f);
    auto m = makeLimiter (kSr, v, 1);
    const double lo = std::pow (10.0, kMakeupDb[2] / 20.0), hi = std::pow (10.0, kMakeupDb[0] / 20.0);
    const int lat = m->getLatencySamples();
    double worstOutside = 0.0;
    float last = 0.0f;
    for (int i = 0; i < 24000; ++i)
    {
        v.set ("mode", i < 240 ? 0.0f : (i < 20000 ? (float) (1 + ((i / 240 - 1) % 2)) : 1.0f));
        m->setParams (v.data());
        float l = 0.25f, r = 0.25f;
        float* ch[2] { &l, &r };
        m->process (ch, 1, {});
        if (i > lat)
        {
            const double g = (double) l / (0.25 * std::pow (10.0, 6.0 / 20.0));
            worstOutside = std::max ({ worstOutside, lo - g, g - hi });
        }
        last = l;
    }
    note ("interrupted fades: worst gain outside the make-up range", worstOutside);
    CHECK (worstOutside <= 1.0e-6);
    CHECK (std::abs ((double) last / (0.25 * std::pow (10.0, 6.0 / 20.0)) - std::pow (10.0, kMakeupDb[1] / 20.0)) < 1.0e-6);
}

TEST_CASE ("clim. history is block independent and measures reduction at the output sample")
{
    const auto in = torture (kSr, 2.0);
    auto render = [&] (int block, bool checkOutput) {
        auto v = neutral(); // output / input directly measures the total gain
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
            const double inputGain = std::pow (10.0, (12.0 + kMakeupDb[0]) / 20.0);
            double worst = 0.0;
            int checked = 0;
            for (size_t h = 0; h < history.size(); ++h)
            {
                double minimum = 0.0;
                bool allNonzero = true;
                // the history holds the deeper channel (the channels are limited independently)
                for (size_t i = h * 64; i < (h + 1) * 64; ++i)
                {
                    if (i < (size_t) latency || std::abs (in.l[i - (size_t) latency]) < 0.01f
                        || std::abs (in.r[i - (size_t) latency]) < 0.01f)
                    {
                        allNonzero = false;
                        break;
                    }
                    minimum = std::min ({ minimum,
                                          db (std::abs ((double) out.l[i] / ((double) in.l[i - (size_t) latency] * inputGain))),
                                          db (std::abs ((double) out.r[i] / ((double) in.r[i - (size_t) latency] * inputGain))) });
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
    CHECK_SPEED (factor > 40.0);
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

TEST_CASE ("clim. fading modes meter the deepest blended channel")
{
    Values v;
    v.set ("thr", -6.0f);
    v.set ("character", 0.0f);
    auto stereo = makeLimiter (kSr, v, 1);
    auto left = makeLimiter (kSr, v, 1);
    auto right = makeLimiter (kSr, v, 1);
    double worst = 0.0, wallError = 0.0, historyError = 0.0, audioError = 0.0;
    double expectedHistory = 0.0;
    for (int i = 0; i < 24000; ++i)
    {
        v.set ("mode", (float) ((i / 317) % 3));
        const float l = (float) (((i / 1901) % 2 ? 0.7 : 0.15) * std::sin (2.0 * kPi * 71.0 * i / kSr));
        const float r = (float) (((i / 887) % 3 ? 0.1 : 1.0) * std::sin (2.0 * kPi * 331.0 * i / kSr));
        float s[2] { l, r }, a[2] { l, l }, b[2] { r, r };
        float* sc[2] { &s[0], &s[1] };
        float* ac[2] { &a[0], &a[1] };
        float* bc[2] { &b[0], &b[1] };
        stereo->setParams (v.data()); left->setParams (v.data()); right->setParams (v.data());
        stereo->process (sc, 1, {}); left->process (ac, 1, {}); right->process (bc, 1, {});
        audioError = std::max ({ audioError, std::abs ((double) s[0] - a[0]), std::abs ((double) s[1] - b[0]) });
        const double expected = std::min (left->getMeters().muReductionDb.load(), right->getMeters().muReductionDb.load());
        worst = std::max (worst, std::abs (stereo->getMeters().muReductionDb.load() - expected));
        const double expectedWall = std::min (left->getMeters().brickwallReductionDb.load(),
                                             right->getMeters().brickwallReductionDb.load());
        wallError = std::max (wallError, std::abs (stereo->getMeters().brickwallReductionDb.load() - expectedWall));
        expectedHistory = std::min (expectedHistory, expected);
        if ((i + 1) % 64 == 0)
        {
            float slow = 0.0f, total = 0.0f;
            REQUIRE (stereo->getHistoryFifo().pull (&slow, &total, 1) == 1);
            historyError = std::max (historyError, std::abs (slow - expectedHistory));
            CHECK (total <= slow && slow <= 0.0f);
            expectedHistory = 0.0;
        }
    }
    note ("stereo slow meter vs deepest mono blend: maximum error dB", worst);
    note ("rest-stage meter maximum error dB", wallError);
    note ("slow history maximum error dB", historyError);
    CHECK (audioError == 0.0);
    CHECK (worst < 2.0e-5);
    CHECK (wallError < 2.0e-5);
    CHECK (historyError < 2.0e-5);
}

TEST_CASE ("clim. automated windows and mode fades respect true peak")
{
    double worst = -200.0, worstInterpolated = -200.0;
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (int variant = 0; variant < 4; ++variant)
        {
            Values v;
            v.set ("thr", -12.0f);
            v.set ("truePeak", 1.0f);
            auto m = makeLimiter (rate, v, 1);
            auto in = torture (rate, 2.0);
            const auto out = run (*m, v, in, 17, [variant] (size_t start, Values& p, ClassicLimiter&) {
                p.set ("character", (start / 221) % 2 ? 10.0f : 0.0f);
                p.set ("mode", variant < 3 ? (float) variant : (float) ((start / 391) % 3));
            });
            worst = std::max (worst, (double) truePeakDb (out));

            // Repeat the same window and mode moves while the ceiling glides. Measure against the ceiling stamped
            // on each source sample: a global peak would miss excursions at the lower ceiling.
            v.set ("ceiling", 0.0f);
            m = makeLimiter (rate, v, 1);
            double ceiling = 1.0, target = 1.0, step = 0.0;
            int remaining = 0;
            const int glide = (int) std::lround (0.020 * rate);
            std::vector<double> stamps (in.size(), 1.0);
            auto moving = run (*m, v, in, 17, [&] (size_t start, Values& p, ClassicLimiter&) {
                p.set ("character", (start / 221) % 2 ? 10.0f : 0.0f);
                p.set ("mode", variant < 3 ? (float) variant : (float) ((start / 391) % 3));
                const float ceilingDb = (start / 1700) % 2 ? -12.0f : 0.0f;
                p.set ("ceiling", ceilingDb);
                const double next = std::pow (10.0, ceilingDb / 20.0);
                if (next != target)
                {
                    target = next;
                    remaining = glide;
                    step = (target - ceiling) / glide;
                }
                for (size_t i = start; i < std::min (start + 17, in.size()); ++i)
                {
                    if (remaining > 0)
                        ceiling = --remaining == 0 ? target : ceiling + step;
                    stamps[i] = ceiling;
                }
            });
            const auto latency = (size_t) m->getLatencySamples();
            worstInterpolated = std::max (worstInterpolated, truePeakAboveStampedCeiling (moving, stamps, latency));
            for (size_t i = latency; i < moving.size(); ++i)
            {
                const double bound = stamps[i - latency];
                CHECK (std::abs ((double) moving.l[i]) <= bound && std::abs ((double) moving.r[i]) <= bound);
            }
        }
    note ("automated windows / fades: maximum true peak dBFS", worst);
    note ("automated windows / fades / ceiling: maximum interpolated true peak above stamped ceiling dB", worstInterpolated);
    CHECK (worst <= 0.0);
    CHECK (worstInterpolated <= 0.0);
}

TEST_CASE ("clim. Character moves under deep limiting avoid gain steps")
{
    double worst = 0.0;
    for (int mode : { 1, 2 })
        for (double hz : { 40.0, 80.0, 150.0 })
            for (int direction = 0; direction < 2; ++direction)
            {
                Values v;
                v.set ("thr", -12.0f);
                v.set ("mode", (float) mode);
                v.set ("character", direction ? 10.0f : 0.0f);
                auto m = makeLimiter (kSr, v);
                const auto in = dual (pstest::sine (hz, -3.0, 4.0, kSr));
                const auto out = run (*m, v, in, 1, [direction] (size_t start, Values& p, ClassicLimiter&) {
                    if (start == 96017)
                        p.set ("character", direction ? 0.0f : 10.0f);
                });
                const double steady = std::max (maxSecondDiff (out.l, 48000, 95000), maxSecondDiff (out.l, 150000, out.size()));
                const double ratio = maxSecondDiff (out.l, 96017, 100000) / steady;
                note (std::string (modeName (mode)) + " " + fmt (hz, 0) + " Hz direction " + std::to_string (direction), ratio);
                worst = std::max (worst, ratio);
            }
    CHECK (worst <= 2.0);
}
