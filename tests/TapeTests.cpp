#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/Tape.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Tape tests (dsp/Tape.h). Prefix "tape.". Owner: mod-tape.
namespace
{
using namespace ps;
using pstest::cd;
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
        const int i = paramIndex ("tape." + id);
        REQUIRE (i >= 0);
        v[(size_t) i] = x;
    }
    ParamValues data() const { return v.data(); }
};

// A quiet setting for the latency check: 15 ips, lowEmph 0, highEmph 4, bias 0.
Values flatValues()
{
    Values v;
    v.set ("speed", 1.0f);
    v.set ("lowEmph", 0.0f);
    v.set ("highEmph", 4.0f);
    v.set ("bias", 0.0f);
    return v;
}

TapeParams paramsOf (const Values& v)
{
    TapeParams p;
    readTapeParams (v.data(), p);
    return p;
}

struct Stereo
{
    std::vector<float> l, r;
    size_t size() const { return l.size(); }
};

Stereo dual (const std::vector<float>& x) { return { x, x }; }

std::unique_ptr<Tape> makeTape (double sr, const Values& vals, int maxBlock = 512)
{
    auto m = std::make_unique<Tape>();
    m->prepare (sr, maxBlock);
    m->setParams (vals.data());
    return m;
}

using BlockHook = std::function<void (size_t, Values&)>;

Stereo run (Tape& m, Values& vals, const Stereo& in, int block = 512, const BlockHook& onBlock = {})
{
    Stereo out = in;
    ProcessContext ctx;
    for (size_t start = 0; start < in.size(); start += (size_t) block)
    {
        if (onBlock)
            onBlock (start, vals);
        m.setParams (vals.data());
        const int n = (int) std::min ((size_t) block, in.size() - start);
        float* ch[2] { out.l.data() + start, out.r.data() + start };
        m.process (ch, n, ctx);
    }
    return out;
}

double db (double x) { return pstest::dbOf (x); }

// Small-signal impulse response (-40 dBFS impulse), first `len` samples after the impulse.
std::vector<double> impulseResponse (double sr, const Values& vals, size_t len, size_t at = 2048)
{
    Values v = vals;
    auto m = makeTape (sr, v);
    const double a = 0.01;
    Stereo in { std::vector<float> (at + len, 0.0f), std::vector<float> (at + len, 0.0f) };
    in.l[at] = in.r[at] = (float) a;
    const auto out = run (*m, v, in, 256);
    std::vector<double> h (len);
    for (size_t i = 0; i < len; ++i)
        h[i] = out.l[at + i] / a;
    return h;
}

// Magnitude (dB) and group delay (samples) of an impulse response at hz.
struct Response
{
    std::vector<cd> H;
    double sr;
    double binHz() const { return sr / (double) H.size(); }
    double magDb (double hz) const
    {
        const double k = hz / binHz();
        const size_t k0 = (size_t) k;
        const double f = k - (double) k0;
        return db (std::abs (H[k0]) * (1.0 - f) + std::abs (H[k0 + 1]) * f);
    }
    double groupDelay (double hz) const
    {
        const size_t k = (size_t) std::lround (hz / binHz());
        const double dphi = std::arg (H[k + 1] / H[k]);
        return -dphi / (2.0 * kPi / (double) H.size());
    }
};

Response responseOf (double sr, const Values& vals, size_t len = 65536)
{
    const auto h = impulseResponse (sr, vals, len);
    return { pstest::spectrumOf (h, len), sr };
}

// Steady-state tone amplitude (dBFS) at hz in out[start .. start + n).
double toneDb (const std::vector<float>& y, double hz, double sr, size_t start, size_t n)
{
    return db (std::abs (pstest::toneOf (y, start, n, hz, sr)));
}

// Harmonic k of a tone at f, relative to the fundamental (dBc), over whole cycles of the analysis window.
double harmonicDbc (const std::vector<float>& y, double f, int k, double sr, size_t start, size_t n)
{
    return toneDb (y, f * k, sr, start, n) - toneDb (y, f, sr, start, n);
}

double thdDb (const std::vector<float>& y, double f, double sr, size_t start, size_t n)
{
    const double a1 = std::abs (pstest::toneOf (y, start, n, f, sr));
    double s = 0.0;
    for (int k = 2; k <= 9 && k * f < 0.45 * sr; ++k)
    {
        const double a = std::abs (pstest::toneOf (y, start, n, f * k, sr));
        s += a * a;
    }
    return db (std::sqrt (s) / a1);
}

double maxSecondDiff (const std::vector<float>& y, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = std::max<size_t> (from, 2); i < std::min (to, y.size()); ++i)
        m = std::max (m, std::abs ((double) y[i] - 2.0 * y[i - 1] + y[i - 2]));
    return m;
}

double rmsDb (const std::vector<float>& y, size_t start, size_t n) { return db (pstest::rmsOf (y, start, n)); }

// Pink noise from 20 Hz up (two Butterworth high-pass sections at 20 Hz), scaled to `rmsDbfs` after the first second.
std::vector<float> bandPink (double seconds, std::uint32_t seed, double rmsDbfs)
{
    auto x = pstest::pinkNoise (1.0, seconds, kSr, seed);
    const double w = std::tan (kPi * 20.0 / kSr), k = std::sqrt (2.0), n = 1.0 / (1.0 + k * w + w * w);
    const double b0 = n, b1 = -2.0 * n, a1 = 2.0 * (w * w - 1.0) * n, a2 = (1.0 - k * w + w * w) * n;
    for (int pass = 0; pass < 2; ++pass)
    {
        double z1 = 0.0, z2 = 0.0;
        for (auto& v : x)
        {
            const double y = b0 * v + z1;
            z1 = b1 * v - a1 * y + z2;
            z2 = b0 * v - a2 * y;
            v = (float) y;
        }
    }
    const double r = pstest::rmsOf (x, (size_t) kSr, x.size() - (size_t) kSr);
    return pstest::scaled (x, (float) (pstest::dbfsToAmplitude (rmsDbfs) / r));
}
} // namespace

TEST_CASE ("tape. module type and contract registration")
{
    Tape m;
    CHECK (m.getType() == ModuleType::Tape);
    CHECK_EQ (std::string (moduleInfo (ModuleType::Tape).idPrefix), std::string ("tape"));
    CHECK_EQ (m.getLatencySamples(), kTapeLatency);
}

TEST_CASE ("tape. latency and reproduce filter phase at 44.1 / 48 / 88.2 / 96 / 192 kHz")
{
    // The minimum-phase oversampler's convention (os. tests): the reported latency is the group delay at 2 kHz within
    // half a sample (the selftest's measure; the fit keeps the reproduce filters' own delay there under 4 us). The
    // fitted bells add their own minimum-phase group delay ripple across the mid band (phase, not latency: within 1
    // sample from 1.5 to 4 kHz).
    // At 192 kHz the analog filters' delay occupies twice as many samples as at 96 kHz. This is frequency-dependent
    // EQ phase, not an additional buffering delay; the oversampler still reports five samples.
    for (const double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        for (int setting = 0; setting < 3; ++setting)
        {
            // the selftest's linear settings (30 ips, drive -30, no head bump), a quiet 15 ips setting, the defaults
            Values vals = setting == 0 ? Values() : setting == 1 ? flatValues() : Values();
            if (setting == 0)
            {
                vals.set ("drive", -30.0f);
                vals.set ("lowEmph", 0.0f);
            }
            auto m = makeTape (sr, vals);
            const int lat = m->getLatencySamples();
            const auto h = impulseResponse (sr, vals, 16384);
            size_t arg = 0;
            for (size_t i = 0; i < h.size(); ++i)
                if (std::abs (h[i]) > std::abs (h[arg]))
                    arg = i;
            const Response r { pstest::spectrumOf (h, h.size()), sr };
            double worst = 0.0;
            for (const double f : { 1500.0, 2000.0, 3000.0, 4000.0 })
                worst = std::max (worst, std::abs (r.groupDelay (f) - lat));
            const char* names[] { "linear   ", "15 ips   ", "defaults " };
            const std::string tag = std::string (names[setting]) + "@" + std::to_string ((int) sr);
            note (tag + ": reported latency", lat);
            note ("    group delay at 2 kHz (samples)", r.groupDelay (2000.0));
            note ("    max |group delay - latency| 1.5 .. 4 kHz (samples)", worst);
            note ("    impulse peak (samples)", (double) arg);
            CHECK_EQ (lat, kTapeLatency);
            CHECK (std::abs (r.groupDelay (2000.0) - lat) < (sr > 96000.0 ? 0.9 : 0.45));
            CHECK (worst <= (sr > 96000.0 ? 2.0 : 1.0));
            CHECK ((int) arg >= lat && (int) arg <= lat + 1);   // the minimum-phase peak trails its group delay
        }

    // random settings every block: the reported latency never moves
    Values vals;
    auto m = makeTape (kSr, vals);
    pstest::Lcg lcg (5u);
    bool constant = true;
    Stereo buf { pstest::pinkNoise (0.3, 2.0, kSr, 3u), pstest::pinkNoise (0.3, 2.0, kSr, 4u) };
    run (*m, vals, buf, 256, [&] (size_t, Values& v) {
        for (const auto& s : paramSpecs())
            if (s.id.rfind ("tape.", 0) == 0)
                v.v[(size_t) paramIndex (s.id)] = s.minValue + (0.5f + 0.5f * lcg.next()) * (s.maxValue - s.minValue);
        constant = constant && m->getLatencySamples() == kTapeLatency;
    });
    CHECK (constant);
}

// The fitted tests below compare the module with numbers measured on the reference's renders (44.1 kHz; sweeps at
// -30 dBFS smoothed to 1/24 octave, staircase steps of 1 s analysed over 0.45 .. 0.95 s; out/vintage/fit-tape.md).
namespace
{
constexpr double kRefSr = 44100.0;

struct Tone
{
    double gainDb = 0.0;
    double h[6] {};   // h[k] = harmonic k re the fundamental (dBc), k = 2 .. 5 (-300 above 0.45 fs)
};

Tone toneThrough (const Values& vals, double f, double levelDb, double sr = kRefSr)
{
    Values v = vals;
    auto m = makeTape (sr, v);
    const auto x = pstest::sine (f, levelDb, 1.0, sr);
    const auto out = run (*m, v, dual (x));
    const size_t from = (size_t) (0.45 * sr), n = (size_t) (0.5 * sr);   // whole cycles of 100 Hz .. 10 kHz
    Tone t;
    const double a1 = std::abs (pstest::toneOf (out.l, from, n, f, sr));
    t.gainDb = db (a1) - levelDb;
    for (int k = 2; k <= 5; ++k)
        t.h[k] = k * f < 0.45 * sr ? db (std::abs (pstest::toneOf (out.l, from, n, f * k, sr)) / a1) : -300.0;
    return t;
}

Values settings (int speed, float drive = 0.0f, float bias = 0.0f, float harmonics = 0.0f, float lowEmph = 2.0f,
                 float highEmph = 4.0f)
{
    Values v;
    v.set ("speed", (float) speed);
    v.set ("drive", drive);
    v.set ("bias", bias);
    v.set ("harmonics", harmonics);
    v.set ("lowEmph", lowEmph);
    v.set ("highEmph", highEmph);
    return v;
}

std::string label (const char* name, double x)
{
    std::ostringstream s;
    s << name << " " << x;
    return s.str();
}
} // namespace

TEST_CASE ("tape. small-signal response matches the reference per speed, emphasis and bias")
{
    // dB re the input at 20, 30, 40, 50, 60, 80, 100, 200, 500 Hz, 1, 2, 5, 10, 16 (, 19) kHz (round 1 renders from
    // 30 Hz: their sweeps start at 20 Hz; round 2 renders up to 19 kHz, the reference's top roll-off included)
    struct Row
    {
        const char* name;
        int speed;
        float lowEmph, highEmph, bias;
        double db[15];
        bool toNineteen;
    };
    const Row rows[] {
        { "30 ips (defaults)", 2, 2, 4, 0, { -13.90, -4.92, 0.46, 2.12, 1.77, 0.41, 0.04, 0.45, 0.88, 0.66, 0.89, 0.68, 0.67, 0.65, 0.06 }, true },
        { "15 ips, low 10", 1, 10, 4, 0, { 1.85, 7.18, 3.51, 2.40, 2.17, 1.78, 1.50, 1.40, 0.97, 0.83, 0.46, -0.11, -0.29, 0.15, -0.21 }, true },
        { "7.5 ips, low 10", 0, 10, 4, 0, { 4.09, 1.85, 1.60, 1.18, 0.82, 1.07, 1.05, 0.66, 1.03, 0.48, 0.09, -0.54, -0.79, -0.37, -0.72 }, true },
        { "30 ips, low 0", 2, 0, 4, 0, { 0, -7.17, -3.21, -1.51, -1.04, -1.11, -0.84, 0.25, 0.85, 0.65, 0.88, 0.68, 0.67, 0.65, 0 }, false },
        { "30 ips, low 10", 2, 10, 4, 0, { 0, -2.47, 7.03, 8.21, 4.90, 1.56, 0.69, 0.58, 0.90, 0.66, 0.89, 0.68, 0.67, 0.65, 0 }, false },
        { "30 ips, high 0", 2, 2, 0, 0, { 0, -4.85, 0.44, 2.06, 1.69, 0.37, 0.06, 0.45, 0.88, 0.65, 0.85, 0.49, 0.00, -0.72, 0 }, false },
        { "30 ips, high 7", 2, 2, 7, 0, { -13.90, -4.92, 0.46, 2.12, 1.77, 0.41, 0.04, 0.45, 0.89, 0.67, 0.92, 0.86, 1.24, 1.75, 1.40 }, true },
        { "30 ips, high 10", 2, 2, 10, 0, { 0, -4.85, 0.44, 2.06, 1.69, 0.37, 0.06, 0.45, 0.89, 0.68, 0.96, 1.08, 1.92, 2.95, 0 }, false },
        { "30 ips, bias -10", 2, 2, 4, -10, { 0, -5.16, 0.07, 1.66, 1.26, -0.12, -0.48, -0.26, 0.12, 0.46, 1.78, 1.80, 1.79, 1.78, 0 }, false },
        { "30 ips, bias -5", 2, 2, 4, -5, { -14.03, -5.10, 0.26, 1.90, 1.54, 0.15, -0.25, 0.08, 0.48, 0.54, 1.32, 1.23, 1.21, 1.20, 0.61 }, true },
        { "30 ips, bias +5", 2, 2, 4, 5, { -13.88, -4.83, 0.60, 2.30, 1.98, 0.68, 0.36, 0.94, 1.43, 0.64, -0.25, -0.67, -0.69, -0.71, -1.30 }, true },
        { "30 ips, bias +10", 2, 2, 4, 10, { 0, -4.10, 1.28, 2.98, 2.68, 1.47, 1.26, 1.99, 2.54, 1.19, -0.80, -1.44, -1.47, -1.48, 0 }, false },
    };
    const double hz[15] { 20, 30, 40, 50, 60, 80, 100, 200, 500, 1000, 2000, 5000, 10000, 16000, 19000 };
    double overall = 0.0;
    for (const auto& row : rows)
    {
        const auto r = responseOf (kRefSr, settings (row.speed, 0.0f, row.bias, 0.0f, row.lowEmph, row.highEmph), 1 << 17);
        double worst = 0.0, at = 0.0;
        for (int i = row.toNineteen ? 0 : 1; i < (row.toNineteen ? 15 : 14); ++i)
        {
            const double e = std::abs (r.magDb (hz[i]) - row.db[i]);
            if (e > worst)
            {
                worst = e;
                at = hz[i];
            }
        }
        note (std::string (row.name) + ": max |module - reference| (dB)", worst);
        note ("    at (Hz)", at);
        overall = std::max (overall, worst);
        CHECK (worst < 0.25);
    }
    note ("worst over every setting (dB)", overall);
}

TEST_CASE ("tape. responseDb is the module's small-signal shape")
{
    struct Setting
    {
        int speed;
        float lowEmph, highEmph, bias;
    };
    const Setting cases[] { { 0, 10.0f, 0.0f, 0.0f }, { 1, 5.0f, 4.0f, 0.0f }, { 2, 10.0f, 10.0f, 0.0f },
                            { 1, 0.0f, 10.0f, -10.0f }, { 0, 3.0f, 7.0f, 10.0f }, { 2, 2.0f, 4.0f, 5.0f } };
    for (const double sr : { 44100.0, 96000.0 })
        for (const auto& s : cases)
        {
            Values v = settings (s.speed, 0.0f, s.bias, 0.0f, s.lowEmph, s.highEmph);
            const auto r = responseOf (sr, v);
            const auto p = paramsOf (v);
            // responseDb is relative to 1 kHz (the level is not part of the view)
            CHECK (std::abs (Tape::responseDb (p, 1000.0)) < 1.0e-12);
            const double offset = r.magDb (1000.0);
            double worst = 0.0;
            for (double f = 25.0; f <= 16000.0; f *= 1.03)
                worst = std::max (worst, std::abs (r.magDb (f) - offset - Tape::responseDb (p, f)));
            note ("speed " + std::to_string (s.speed) + " low " + std::to_string ((int) s.lowEmph) + " high "
                      + std::to_string ((int) s.highEmph) + " bias " + std::to_string ((int) s.bias) + " @"
                      + std::to_string ((int) sr) + ": max |measured - responseDb| 25 Hz .. 16 kHz (dB)",
                  worst);
            CHECK (worst < 0.1);
        }
}

TEST_CASE ("tape. head bump: a peak of G = 1 + a x lowEmph at the speed's bump frequency, none at 0")
{
    // fitted per speed (7.5 / 15 / 30 ips): bump frequency and a
    const double bumpHz[] { 15.075, 25.036, 44.238 }, perUnit[] { 0.2794, 0.2443, 0.2743 };
    for (int speed = 0; speed < 3; ++speed)
        for (const float lowEmph : { 0.0f, 2.0f, 5.0f, 10.0f })
        {
            const auto p0 = paramsOf (settings (speed, 0, 0, 0, 0.0f)), p = paramsOf (settings (speed, 0, 0, 0, lowEmph));
            const double lift = Tape::responseDb (p, bumpHz[speed]) - Tape::responseDb (p0, bumpHz[speed]);
            const double expected = 20.0 * std::log10 (1.0 + perUnit[speed] * lowEmph);
            note ("speed " + std::to_string (speed) + " lowEmph " + std::to_string ((int) lowEmph) + ": bump (dB)", lift);
            CHECK (std::abs (lift - expected) < 0.02);   // (responseDb is relative to 1 kHz, where the bump's tail
                                                         // still lifts by up to 0.01 dB)
            // above the bump the low emphasis leaves the response alone
            CHECK (std::abs (Tape::responseDb (p, 2000.0) - Tape::responseDb (p0, 2000.0)) < 0.02);
        }
    // 30 ips, lowEmph 10: +11.3 dB peak (reference: Low 10 minus Low 0 +10.2 dB at 40 Hz, +9.7 at 50 Hz)
    const auto p10 = paramsOf (settings (2, 0, 0, 0, 10.0f)), p0 = paramsOf (settings (2, 0, 0, 0, 0.0f));
    note ("30 ips: low 10 - low 0 at 40 Hz (dB)", Tape::responseDb (p10, 40.0) - Tape::responseDb (p0, 40.0));
    CHECK (std::abs (Tape::responseDb (p10, 40.0) - Tape::responseDb (p0, 40.0) - 10.24) < 0.25);
    CHECK (std::abs (Tape::responseDb (p10, 50.0) - Tape::responseDb (p0, 50.0) - 9.72) < 0.25);
}

TEST_CASE ("tape. high emphasis: 4 is flat; 0, 7 and 10 shelve the highs as the reference")
{
    // reference, re High 4 (30 ips): 10 kHz / 16 kHz
    struct Point
    {
        float e;
        double at10k, at16k;
    };
    const Point pts[] { { 0.0f, -0.67, -1.37 }, { 7.0f, 0.57, 1.10 }, { 10.0f, 1.25, 2.30 } };
    for (int speed = 0; speed < 3; ++speed)
    {
        const auto flat = paramsOf (settings (speed));
        double prev = -100.0;
        for (const float e : { 0.0f, 2.0f, 4.0f, 5.5f, 7.0f, 8.5f, 10.0f })
        {
            const auto p = paramsOf (settings (speed, 0, 0, 0, 2.0f, e));
            const double d16 = Tape::responseDb (p, 16000.0) - Tape::responseDb (flat, 16000.0);
            CHECK (d16 > prev);
            if (e == 4.0f)
                CHECK (std::abs (d16) < 1.0e-9);
            prev = d16;
        }
    }
    const auto flat = paramsOf (settings (2));
    for (const auto& pt : pts)
    {
        const auto p = paramsOf (settings (2, 0, 0, 0, 2.0f, pt.e));
        const double d10 = Tape::responseDb (p, 10000.0) - Tape::responseDb (flat, 10000.0);
        const double d16 = Tape::responseDb (p, 16000.0) - Tape::responseDb (flat, 16000.0);
        note (label ("highEmph", pt.e) + ": 10 kHz re 4 (dB)", d10);
        note ("    16 kHz re 4 (dB)", d16);
        CHECK (std::abs (d10 - pt.at10k) < 0.1);
        CHECK (std::abs (d16 - pt.at16k) < 0.1);
    }
}

TEST_CASE ("tape. drive: level and harmonics follow the reference's 1 kHz staircase")
{
    // reference (30 ips): gain (dB) at -42 / -12 / 0 dBFS, h3 and h5 (dBc) at 0 dBFS, h3 at -12 dBFS
    struct Row
    {
        float drive;
        double g42, g12, g0, h3at0, h5at0, h3at12;
    };
    const Row rows[] { { -10.0f, 0.52, 0.48, 0.36, -49.5, -66.6, -61.6 }, { 0.0f, 0.67, 0.54, 0.14, -39.1, -56.2, -51.5 },
                       { 10.0f, 1.16, 0.75, -0.63, -27.9, -44.9, -41.3 }, { 20.0f, 2.91, 1.55, -4.28, -14.0, -25.9, -30.3 },
                       { 30.0f, 10.32, 4.95, -6.41, -11.0, -16.2, -16.3 } };
    for (const auto& r : rows)
    {
        const auto v = settings (2, r.drive);
        const auto quiet = toneThrough (v, 1000.0, -42.0), mid = toneThrough (v, 1000.0, -12.0), loud = toneThrough (v, 1000.0, 0.0);
        note (label ("drive", r.drive) + ": gain at -42 / -12 / 0 dBFS (dB)", quiet.gainDb);
        note ("    ", mid.gainDb);
        note ("    ", loud.gainDb);
        note ("    h3 / h5 at 0 dBFS (dBc)", loud.h[3]);
        note ("    ", loud.h[5]);
        CHECK (std::abs (quiet.gainDb - r.g42) < 0.25);
        CHECK (std::abs (mid.gainDb - r.g12) < 0.25);
        CHECK (std::abs (loud.gainDb - r.g0) < 0.3);
        CHECK (std::abs (loud.h[3] - r.h3at0) < 1.5);
        CHECK (std::abs (loud.h[5] - r.h5at0) < 2.0);
        CHECK (std::abs (mid.h[3] - r.h3at12) < 1.5);
        CHECK (mid.h[2] < -100.0);   // symmetric curve: no even harmonics at harmonics 0
        CHECK (mid.h[4] < -100.0);
    }
}

TEST_CASE ("tape. pre-emphasis: the highs saturate earlier above a corner that moves with the speed")
{
    // reference at drive +20: gain (dB) at -36 / 0 dBFS per staircase frequency, h3 at -12 dBFS (100 Hz .. 5 kHz)
    struct Row
    {
        int speed;
        double f, g36, g0, h3at12;
    };
    const Row rows[] {
        { 2, 100, 2.26, -4.59, -28.6 }, { 2, 1000, 2.87, -4.28, -30.3 }, { 2, 5000, 2.89, -5.47, -27.6 }, { 2, 10000, 2.87, -5.49, 0 },
        { 1, 100, 3.57, -3.28, -29.5 }, { 1, 1000, 3.03, -5.13, -28.6 }, { 1, 5000, 2.09, -6.27, -27.4 }, { 1, 10000, 1.91, -6.45, 0 },
        { 0, 100, 3.22, -3.65, -30.3 }, { 0, 1000, 2.68, -5.67, -28.2 }, { 0, 5000, 1.66, -6.70, -27.4 }, { 0, 10000, 1.42, -6.95, 0 },
    };
    for (const auto& r : rows)
    {
        const auto v = settings (r.speed, 20.0f);
        const auto quiet = toneThrough (v, r.f, -36.0), loud = toneThrough (v, r.f, 0.0);
        note ("speed " + std::to_string (r.speed) + label (",", r.f) + " Hz: compression -36 -> 0 dBFS (dB)",
              loud.gainDb - quiet.gainDb);
        CHECK (std::abs (quiet.gainDb - r.g36) < 0.25);
        CHECK (std::abs (loud.gainDb - r.g0) < 0.3);
        if (r.h3at12 != 0.0)
        {
            const auto mid = toneThrough (v, r.f, -12.0);
            note ("    h3 at -12 dBFS (dBc)", mid.h[3]);
            CHECK (std::abs (mid.h[3] - r.h3at12) < 1.5);
        }
    }
}

TEST_CASE ("tape. harmonics: an operating-point shift (second harmonic alone at low levels)")
{
    // reference, 1 kHz, 30 ips: drive 0 / harmonics 10, drive +20 / harmonics 5 and 10. h2 (dBc) at -42 / -15 dBFS,
    // gain at -42 dBFS; h3 stays below -100 dBc until the signal exceeds the shift (drive 0: -15 dBFS)
    struct Row
    {
        float drive, harmonics;
        double h2at42, h2at15, g42, h3at6;
    };
    const Row rows[] { { 0.0f, 10.0f, -71.8, -44.8, 0.45, -48.6 }, { 20.0f, 5.0f, -51.9, -45.1, 2.84, -22.6 },
                       { 20.0f, 10.0f, -51.8, -39.1, 2.72, -22.6 } };
    for (const auto& r : rows)
    {
        const auto v = settings (2, r.drive, 0.0f, r.harmonics);
        const auto t42 = toneThrough (v, 1000.0, -42.0), t15 = toneThrough (v, 1000.0, -15.0), t6 = toneThrough (v, 1000.0, -6.0);
        note (label ("drive", r.drive) + label (", harmonics", r.harmonics) + ": h2 at -42 / -15 dBFS (dBc)", t42.h[2]);
        note ("    ", t15.h[2]);
        note ("    h3 at -15 / -6 dBFS (dBc)", t15.h[3]);
        note ("    ", t6.h[3]);
        CHECK (std::abs (t42.h[2] - r.h2at42) < 2.0);
        CHECK (std::abs (t15.h[2] - r.h2at15) < 2.0);
        CHECK (std::abs (t42.gainDb - r.g42) < 0.25);
        CHECK (std::abs (t6.h[3] - r.h3at6) < 2.0);
        if (r.drive == 0.0f)
            CHECK (t15.h[3] < -100.0);
    }
    // the even part's DC is removed by the reproduce high-pass
    const auto v = settings (2, 6.0f, 0.0f, 10.0f);
    Values vv = v;
    auto m = makeTape (kSr, vv);
    const auto out = run (*m, vv, dual (pstest::sine (200.0, -6.0, 2.0, kSr)));
    double dc = 0.0;
    const size_t from = (size_t) (1.0 * kSr), n = (size_t) (0.5 * kSr);
    for (size_t i = from; i < from + n; ++i)
        dc += out.l[i];
    dc /= (double) n;
    note ("harmonics 10, drive +6, 200 Hz at -6 dBFS: DC (dBFS)", db (std::abs (dc)));
    CHECK (db (std::abs (dc)) < -80.0);
}

TEST_CASE ("tape. bias: extra drive into the curve on both sides of 0 and the record tilt, as the reference")
{
    // reference, 30 ips, drive 0: h3 (dBc) at -30 dBFS and gain (dB) at -30 / 0 dBFS, 1 kHz and 100 Hz
    struct Row
    {
        float bias;
        double h3k, g30k, g0k, h3h, g30h, g0h;
    };
    const Row rows[] { { -10.0f, -64.0, 0.46, -0.55, -62.6, -0.50, -1.44 }, { -5.0f, -67.2, 0.54, -0.15, -65.6, -0.25, -0.90 },
                       { 0.0f, -69.7, 0.66, 0.14, -67.9, 0.04, -0.45 }, { 5.0f, -65.3, 0.64, -0.22, -63.3, 0.36, -0.50 },
                       { 10.0f, -53.1, 1.19, -3.05, -50.7, 1.24, -3.21 } };
    for (const auto& r : rows)
    {
        const auto v = settings (2, 0.0f, r.bias);
        const auto k30 = toneThrough (v, 1000.0, -30.0), k0 = toneThrough (v, 1000.0, 0.0);
        const auto h30 = toneThrough (v, 100.0, -30.0), h0 = toneThrough (v, 100.0, 0.0);
        note (label ("bias", r.bias) + ": 1 kHz h3 at -30 dBFS (dBc)", k30.h[3]);
        note ("    1 kHz gain at 0 dBFS (dB)", k0.gainDb);
        note ("    100 Hz h3 at -30 dBFS (dBc)", h30.h[3]);
        note ("    100 Hz gain at 0 dBFS (dB)", h0.gainDb);
        CHECK (std::abs (k30.h[3] - r.h3k) < 1.5);
        CHECK (std::abs (h30.h[3] - r.h3h) < 1.5);
        CHECK (std::abs (k30.gainDb - r.g30k) < 0.25);
        CHECK (std::abs (h30.gainDb - r.g30h) < 0.25);
        CHECK (std::abs (k0.gainDb - r.g0k) < 0.3);
        CHECK (std::abs (h0.gainDb - r.g0h) < 0.3);
    }
}

TEST_CASE ("tape. the curve is memoryless: the fundamental keeps its phase at any drive (no hysteresis)")
{
    // Reference: the harmonic phases are those of a memoryless curve followed by the reproduce filters. A memoryless odd
    // curve keeps the fundamental in phase with the input, so the phase does not move with the drive.
    auto phaseOf = [] (float bias, float drive) {
        Values v = settings (2, drive, bias);
        auto m = makeTape (kSr, v);
        const auto x = pstest::sine (100.0, -6.0, 1.0, kSr);
        const auto out = run (*m, v, dual (x));
        return std::arg (pstest::toneOf (out.l, (size_t) (0.5 * kSr), (size_t) (0.4 * kSr), 100.0, kSr));
    };
    for (const float bias : { -10.0f, 0.0f, 10.0f })
    {
        double d = phaseOf (bias, -30.0f) - phaseOf (bias, 14.0f);
        while (d > kPi) d -= 2.0 * kPi;
        while (d < -kPi) d += 2.0 * kPi;
        note (label ("bias", bias) + ": phase of the fundamental, drive +14 vs -30 (deg)", d * 180.0 / kPi);
        CHECK (std::abs (d * 180.0 / kPi) < 0.5);
    }
}

TEST_CASE ("tape. meters: input drive level and saturation follow the audio")
{
    const auto x = pstest::sine (100.0, -6.0, 1.0, kSr);
    for (const float drive : { -12.0f, 0.0f, 6.0f, 20.0f })
    {
        Values v = settings (2, drive);
        auto m = makeTape (kSr, v);
        run (*m, v, dual (x));
        const float in = m->getMeters().inputDriveDb.load(), sat = m->getMeters().saturationDb.load();
        note ("100 Hz at -6 dBFS, drive " + std::to_string ((int) drive) + ": input drive meter (dBFS)", in);
        note ("    saturation meter (dB)", sat);
        // 100 Hz is far below the pre-emphasis: the level into the curve = input peak + drive
        CHECK (std::abs (in - (-6.0f + drive)) < 0.5f);
        if (drive <= -12.0f)
            CHECK (sat < 0.2f);
        if (drive >= 20.0f)   // u = 1.35 at the peak: 20 log10 (u / f (u)) = 3.6 dB
            CHECK (std::abs (sat - 3.6f) < 0.3f);
    }
    // silence: the meters fall back
    Values v = settings (2);
    auto m = makeTape (kSr, v);
    run (*m, v, dual (x));
    run (*m, v, dual (std::vector<float> ((size_t) (3.0 * kSr), 0.0f)));
    note ("after 3 s of silence: input drive meter (dBFS)", m->getMeters().inputDriveDb.load());
    CHECK (m->getMeters().inputDriveDb.load() < -90.0f);
    CHECK (m->getMeters().saturationDb.load() >= 0.0f);
}

TEST_CASE ("tape. channels are processed alike and independently")
{
    Values v = flatValues();
    v.set ("drive", 12.0f);
    v.set ("harmonics", 5.0f);
    auto m = makeTape (kSr, v);
    const auto loud = pstest::pinkNoise (0.5, 1.0, kSr, 21u);
    const auto quiet = pstest::scaled (loud, 0.01f);
    const auto out = run (*m, v, { loud, quiet });
    auto m2 = makeTape (kSr, v);
    const auto ref = run (*m2, v, { quiet, quiet });
    double err = 0.0, same = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        err = std::max (err, std::abs ((double) out.r[i] - ref.r[i]));
        same = std::max (same, std::abs ((double) ref.l[i] - ref.r[i]));
    }
    note ("quiet right channel next to a loud left one vs alone: max difference", err);
    CHECK (err == 0.0);
    CHECK (same == 0.0);
}

TEST_CASE ("tape. output is independent of the host block size")
{
    const Stereo in { pstest::pinkNoise (0.4, 1.5, kSr, 31u), pstest::pinkNoise (0.4, 1.5, kSr, 32u) };
    auto render = [&] (int block) {
        Values v = Values();
        v.set ("drive", 8.0f);
        auto m = makeTape (kSr, v, block);
        Stereo out = in;
        ProcessContext ctx;
        for (size_t start = 0; start < out.size();)
        {
            // automation at fixed sample positions
            v.set ("drive", start < (size_t) (0.5 * kSr) ? 8.0f : -4.0f);
            v.set ("speed", start < (size_t) (0.8 * kSr) ? 1.0f : 2.0f);
            v.set ("highEmph", start < (size_t) (1.0 * kSr) ? 4.0f : 9.0f);
            m->setParams (v.data());
            size_t end = std::min (start + (size_t) block, out.size());
            for (const double t : { 0.5, 0.8, 1.0 })
                if (start < (size_t) (t * kSr))
                    end = std::min (end, (size_t) (t * kSr));
            float* ch[2] { out.l.data() + start, out.r.data() + start };
            m->process (ch, (int) (end - start), ctx);
            start = end;
        }
        return out;
    };
    const auto reference = render (1);
    for (const int block : { 7, 64, 333, 512 })
    {
        const auto out = render (block);
        double err = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
            err = std::max (err, std::abs ((double) out.l[i] - reference.l[i]));
        note ("block " + std::to_string (block) + ": max difference to block 1", err);
        CHECK (err < 1.0e-6);
    }
}

TEST_CASE ("tape. meter release is independent of host block boundaries")
{
    const auto input = dual (pstest::sine (317.0, -6.0, 0.013, kSr));
    auto render = [&] (int block) {
        Values v = settings (2, 20.0f);
        auto m = makeTape (kSr, v);
        run (*m, v, input, block);
        return std::pair<float, float> { m->getMeters().inputDriveDb.load(), m->getMeters().saturationDb.load() };
    };
    const auto expected = render (1);
    for (const int block : { 7, 8, 333, 512 })
    {
        const auto actual = render (block);
        note ("meter difference, block " + std::to_string (block) + " (dB)", actual.first - expected.first);
        CHECK_EQ (actual.first, expected.first);
        CHECK_EQ (actual.second, expected.second);
    }
}

TEST_CASE ("tape. speed fades ending inside a control period are block independent at every rate")
{
    for (const double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        const Stereo input { pstest::sine (43.0, -9.0, 0.12, sr), pstest::sine (711.0, -18.0, 0.12, sr) };
        // Unaligned events include a new target during a fade, then reversal after it completes.
        const size_t events[] { 13, 113, (size_t) (0.05 * sr) + 3, (size_t) (0.08 * sr) + 1 };
        auto render = [&] (int block) {
            Values v = settings (0, 12.0f, -10.0f, 10.0f, 10.0f, 0.0f);
            auto m = makeTape (sr, v);
            Stereo out = input;
            ProcessContext ctx;
            size_t event = 0;
            for (size_t start = 0; start < out.size();)
            {
                if (event < 4 && start == events[event])
                {
                    v.set ("speed", event % 2 == 0 ? 2.0f : 1.0f);
                    v.set ("bias", event % 2 == 0 ? 10.0f : -10.0f);
                    v.set ("lowEmph", event % 2 == 0 ? 0.0f : 10.0f);
                    v.set ("highEmph", event % 2 == 0 ? 10.0f : 0.0f);
                    m->setParams (v.data());
                    ++event;
                }
                size_t end = std::min (start + (size_t) block, out.size());
                if (event < 4)
                    end = std::min (end, events[event]);
                float* ch[] { out.l.data() + start, out.r.data() + start };
                m->process (ch, (int) (end - start), ctx);
                start = end;
            }
            return out;
        };
        const auto expected = render (1);
        for (const int block : { 7, 8, 17, 333, 512 })
        {
            const auto actual = render (block);
            CHECK (actual.l == expected.l);
            CHECK (actual.r == expected.r);
        }
    }
}

TEST_CASE ("tape. parameter changes are click-free")
{
    struct Move
    {
        const char* name;
        std::function<void (Values&)> before, after;
    };
    const std::vector<Move> moves {
        { "drive -30 -> +20", [] (Values& v) { v.set ("drive", -30.0f); }, [] (Values& v) { v.set ("drive", 20.0f); } },
        { "drive +20 -> -30", [] (Values& v) { v.set ("drive", 20.0f); }, [] (Values& v) { v.set ("drive", -30.0f); } },
        { "bias -10 -> +10 (drive +10)", [] (Values& v) { v.set ("drive", 10.0f); v.set ("bias", -10.0f); },
          [] (Values& v) { v.set ("drive", 10.0f); v.set ("bias", 10.0f); } },
        { "harmonics 0 -> 10 (drive +6)", [] (Values& v) { v.set ("drive", 6.0f); },
          [] (Values& v) { v.set ("drive", 6.0f); v.set ("harmonics", 10.0f); } },
        { "harmonics 10 -> 0 (drive +6)", [] (Values& v) { v.set ("drive", 6.0f); v.set ("harmonics", 10.0f); },
          [] (Values& v) { v.set ("drive", 6.0f); } },
        { "low emphasis 0 -> 10", [] (Values& v) { v.set ("lowEmph", 0.0f); }, [] (Values& v) { v.set ("lowEmph", 10.0f); } },
        { "high emphasis 0 -> 10", [] (Values& v) { v.set ("highEmph", 0.0f); }, [] (Values& v) { v.set ("highEmph", 10.0f); } },
        { "speed 7.5 -> 30 ips (lowEmph 10)", [] (Values& v) { v.set ("speed", 0.0f); v.set ("lowEmph", 10.0f); },
          [] (Values& v) { v.set ("speed", 2.0f); v.set ("lowEmph", 10.0f); } },
        { "speed 30 -> 7.5 ips (drive +10)", [] (Values& v) { v.set ("speed", 2.0f); v.set ("drive", 10.0f); },
          [] (Values& v) { v.set ("speed", 0.0f); v.set ("drive", 10.0f); } },
    };
    const auto tSwitch = (size_t) (2.0 * kSr);
    for (const auto& mv : moves)
    {
        const auto x = pstest::sine (80.0, -12.0, 4.0, kSr);
        Values vals;
        mv.before (vals);
        auto m = makeTape (kSr, vals);
        const auto out = run (*m, vals, dual (x), 64, [&] (size_t start, Values& v) {
            if (start < tSwitch)
                return;
            v = Values();
            mv.after (v);
        });
        const double steadyBefore = maxSecondDiff (out.l, (size_t) (1.0 * kSr), tSwitch);
        const double steadyAfter = maxSecondDiff (out.l, (size_t) (3.0 * kSr), out.size());
        const double during = maxSecondDiff (out.l, tSwitch, tSwitch + (size_t) (0.5 * kSr));
        const double ratio = during / std::max (steadyBefore, steadyAfter);
        note (std::string (mv.name) + ": transition / steady second difference", ratio);
        CHECK (ratio <= 2.0);
    }
}

TEST_CASE ("tape. stable, finite and denormal-safe at the extremes")
{
    for (const double sr : { 44100.0, 96000.0, 192000.0 })
    {
        Values vals;
        auto m = makeTape (sr, vals);
        pstest::Lcg lcg (99u);
        // loud noise and a full-scale square, extreme values every 64 samples
        auto sq = pstest::sine (60.0, 6.0, 4.0, sr);
        for (auto& s : sq)
            s = s >= 0.0f ? 2.0f : -2.0f;
        const Stereo in { pstest::added (pstest::pinkNoise (2.0, 4.0, sr, 41u), sq), pstest::pinkNoise (2.0, 4.0, sr, 42u) };
        const auto out = run (*m, vals, in, 64, [&] (size_t, Values& v) {
            for (const auto& s : paramSpecs())
            {
                if (s.id.rfind ("tape.", 0) != 0)
                    continue;
                const float u = lcg.next() > 0.0f ? 1.0f : 0.0f;
                v.v[(size_t) paramIndex (s.id)] = s.minValue + u * (s.maxValue - s.minValue);
            }
        });
        bool finite = true;
        double pk = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            finite = finite && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
            pk = std::max (pk, (double) std::max (std::abs (out.l[i]), std::abs (out.r[i])));
        }
        note ("@" + std::to_string ((int) sr) + ": random extremes on +6 dBFS material: output peak (dBFS)", db (pk));
        CHECK (finite);
        CHECK (pk < pstest::dbfsToAmplitude (24.0));

        // every control at its maximum on a full-scale sine: bounded
        Values hot;
        hot.set ("drive", 30.0f);
        hot.set ("bias", 10.0f);
        hot.set ("harmonics", 10.0f);
        hot.set ("lowEmph", 10.0f);
        hot.set ("highEmph", 10.0f);
        auto m3 = makeTape (sr, hot);
        const auto hotOut = run (*m3, hot, dual (pstest::sine (1000.0, 0.0, 1.0, sr)));
        double hotPk = 0.0;
        for (const float s : hotOut.l)
            hotPk = std::max (hotPk, (double) std::abs (s));
        note ("    every control at its maximum, 0 dBFS 1 kHz: output peak (dBFS)", db (hotPk));
        CHECK (std::isfinite (hotPk));
        CHECK (hotPk < pstest::dbfsToAmplitude (20.0));

        // silence after loud material decays to (practically) zero, without denormals
        Values v2;
        v2.set ("drive", 12.0f);
        v2.set ("harmonics", 10.0f);
        v2.set ("lowEmph", 10.0f);
        auto m2 = makeTape (sr, v2);
        run (*m2, v2, dual (pstest::pinkNoise (0.8, 1.0, sr, 7u)));
        const Stereo silence { std::vector<float> ((size_t) (20.0 * sr), 0.0f), std::vector<float> ((size_t) (20.0 * sr), 0.0f) };
        const auto quiet = run (*m2, v2, silence);
        double tail = 0.0;
        bool subnormal = false;
        for (size_t i = 0; i < quiet.size(); ++i)
        {
            subnormal = subnormal || std::fpclassify (quiet.l[i]) == FP_SUBNORMAL;
            if (i > (size_t) (15.0 * sr))
                tail = std::max (tail, (double) std::abs (quiet.l[i]));
        }
        note ("    20 s of silence after noise: largest sample after 15 s", tail);
        CHECK (! subnormal);
        CHECK (tail < 1.0e-20);
    }
}

TEST_CASE ("tape. invalid audio cannot poison the oversampler or the other channel")
{
    for (const double sr : { 44100.0, 96000.0 })
    {
        Values v;
        v.set ("drive", 14.0f);
        v.set ("harmonics", 10.0f);
        const Stereo input { pstest::sine (173.0, -12.0, 0.5, sr), pstest::sine (431.0, -18.0, 0.5, sr) };
        for (const float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max(),
                                 -std::numeric_limits<float>::max() })
        {
            auto damaged = input, repaired = input;
            // A brief corrupt burst, after the recursive stages have warmed up; leave the right channel intact.
            const size_t at = (size_t) (0.1 * sr) + 3;
            for (size_t i = at; i < at + 16; ++i)
            {
                damaged.l[i] = bad;
                repaired.l[i] = 0.0f;
            }
            auto m = makeTape (sr, v), reference = makeTape (sr, v);
            const auto out = run (*m, v, damaged, 7), expected = run (*reference, v, repaired, 7);
            size_t nonFinite = 0;
            double error = 0.0, otherError = 0.0;
            for (size_t i = 0; i < out.size(); ++i)
            {
                if (! std::isfinite (out.l[i]))
                    ++nonFinite;
                else
                    error = std::max (error, std::abs ((double) out.l[i] - expected.l[i]));
                if (! std::isfinite (out.r[i]))
                    ++nonFinite;
                else
                    otherError = std::max (otherError, std::abs ((double) out.r[i] - expected.r[i]));
            }
            note ("corrupt audio @" + std::to_string ((int) sr) + ": non-finite output samples", (double) nonFinite);
            CHECK_EQ (nonFinite, (size_t) 0);
            CHECK_EQ (error, 0.0);
            CHECK_EQ (otherError, 0.0);
            CHECK (std::isfinite (m->getMeters().inputDriveDb.load()));
            CHECK (std::isfinite (m->getMeters().saturationDb.load()));
        }
    }
}

TEST_CASE ("tape. reset and repeated prepare clear both lanes and pending control ramps")
{
    for (const double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        for (const bool reprepare : { false, true })
        {
            Values v;
            const double initialRate = reprepare ? 48000.0 : sr;
            auto m = makeTape (initialRate, v);
            const auto warm = dual (pstest::sine (83.0, -6.0, 0.1, initialRate));
            run (*m, v, warm, 7);
            v.set ("drive", 20.0f);
            v.set ("bias", 10.0f);
            v.set ("harmonics", 10.0f);
            v.set ("speed", 0.0f);
            run (*m, v, dual (std::vector<float> (103, 0.2f)), 7);
            const double rate = sr;
            if (reprepare)
                m->prepare (rate, 333);
            else
                m->reset();
            CHECK_EQ (m->getMeters().inputDriveDb.load(), -100.0f);
            CHECK_EQ (m->getMeters().saturationDb.load(), 0.0f);
            auto reference = makeTape (rate, v);
            const auto input = dual (pstest::sine (157.0, -12.0, 0.1, rate));
            const auto out = run (*m, v, input, 333), expected = run (*reference, v, input, 333);
            CHECK (out.l == expected.l);
            CHECK (out.r == expected.r);
        }
}

TEST_CASE ("tape. no allocation in setParams / process / reset")
{
    Values vals;
    auto m = makeTape (kSr, vals);
    Stereo buf { pstest::pinkNoise (0.6, 2.0, kSr, 51u), pstest::pinkNoise (0.6, 2.0, kSr, 52u) };
    long long count = 0;
    {
        pstest::AllocationScope scope;
        ProcessContext ctx;
        int k = 0;
        for (size_t start = 0; start + 512 <= buf.size(); start += 512, ++k)
        {
            vals.set ("drive", (float) ((k % 11) * 5 - 30));
            vals.set ("bias", (float) ((k % 5) * 5 - 10));
            vals.set ("harmonics", (float) (k % 3) * 5.0f);
            vals.set ("speed", (float) ((k / 4) % 3));
            vals.set ("lowEmph", (float) (k % 10));
            vals.set ("highEmph", (float) ((k * 3) % 10));
            m->setParams (vals.data());
            // Exercise the corrupt-input guard inside the allocation scope as well.
            if (k % 17 == 0)
                buf.l[start] = std::numeric_limits<float>::quiet_NaN();
            float* ch[2] { buf.l.data() + start, buf.r.data() + start };
            m->process (ch, 512, ctx);
            if (k == 40)
                m->reset();
        }
        count = scope.count();
    }
    note ("allocations", (double) count);
    CHECK_EQ (count, 0LL);
}

TEST_CASE ("tape. cpu benchmark (48 kHz stereo)")
{
    const Stereo in { pstest::pinkNoise (0.6, 10.0, kSr, 61u), pstest::pinkNoise (0.6, 10.0, kSr, 62u) };
    Values typical;
    typical.set ("drive", 6.0f);
    typical.set ("harmonics", 4.0f);
    auto m = makeTape (kSr, typical);
    pstest::Stopwatch sw;
    run (*m, typical, in, 512);
    const double x = 10.0 / sw.seconds();
    note ("drive +6, harmonics 4: x realtime", x);
    CHECK (x > 40.0);

    // every control moving all the time (glides, make-up re-evaluated every control period, speed crossfades)
    Values moving;
    auto m2 = makeTape (kSr, moving);
    int k = 0;
    pstest::Stopwatch sw2;
    run (*m2, moving, in, 512, [&] (size_t, Values& v) {
        ++k;
        v.set ("drive", (float) ((k * 7) % 50 - 30));
        v.set ("bias", (float) ((k * 3) % 21 - 10));
        v.set ("harmonics", (float) (k % 11));
        v.set ("lowEmph", (float) ((k * 5) % 11));
        v.set ("highEmph", (float) ((k * 2) % 11));
        v.set ("speed", (float) ((k / 40) % 3));
    });
    const double x2 = 10.0 / sw2.seconds();
    note ("every control moving every block: x realtime", x2);
    CHECK (x2 > 10.0);
}
