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

// The small-signal reference settings of the contract: 15 ips, lowEmph 0, highEmph 4, bias 0.
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

TEST_CASE ("tape. latency equals the measured impulse delay at 44.1 / 48 / 96 kHz and never changes")
{
    // The delay of the impulse response is its group delay (the minimum-phase oversampler's convention, os. tests):
    // flat over the mid band and equal to the reported latency; a delayed input then lines up with the output.
    for (const double sr : { 44100.0, 48000.0, 96000.0 })
        for (int setting = 0; setting < 2; ++setting)
        {
            Values vals = setting == 0 ? flatValues() : Values();   // the flat settings, then the defaults
            auto m = makeTape (sr, vals);
            const int lat = m->getLatencySamples();
            const auto h = impulseResponse (sr, vals, 16384);
            size_t arg = 0;
            for (size_t i = 0; i < h.size(); ++i)
                if (std::abs (h[i]) > std::abs (h[arg]))
                    arg = i;
            const Response r { pstest::spectrumOf (h, h.size()), sr };
            // (lower down the head-bump roll-off adds its own group delay, higher up the oversampler's rises: phase, not
            // latency)
            double worst = 0.0;
            for (const double f : { 1500.0, 2000.0, 3000.0, 4000.0 })
                worst = std::max (worst, std::abs (r.groupDelay (f) - lat));
            const std::string tag = std::string (setting == 0 ? "flat     " : "defaults ") + "@" + std::to_string ((int) sr);
            note (tag + ": reported latency", lat);
            note ("    group delay at 4 kHz (samples)", r.groupDelay (4000.0));
            note ("    max |group delay - latency| 1.5 .. 4 kHz (samples)", worst);
            note ("    impulse peak (samples)", (double) arg);
            CHECK_EQ (lat, kTapeLatency);
            CHECK (worst <= 0.2);
            CHECK ((int) arg >= lat && (int) arg <= lat + 1);   // the minimum-phase peak trails its group delay

            // a 4 kHz tone (-40 dBFS): its phase delay through the module is the latency
            const auto x = pstest::sine (4000.0, -40.0, 0.5, sr);
            Values v = vals;
            auto m2 = makeTape (sr, v);
            const auto out = run (*m2, v, dual (x));
            const size_t from = (size_t) (0.25 * sr), n = (size_t) (0.2 * sr);
            const cd yo = pstest::toneOf (out.l, from, n, 4000.0, sr), xo = pstest::toneOf (x, from, n, 4000.0, sr);
            const double phaseDelay = -std::arg (yo / xo) / (2.0 * kPi * 4000.0) * sr;
            note ("    4 kHz phase delay (samples)", phaseDelay);
            CHECK (std::abs (phaseDelay - lat) < 0.2);
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

TEST_CASE ("tape. small-signal response is flat at the reference settings and matches responseDb")
{
    // contract: 15 ips, lowEmph 0, highEmph 4, bias 0 -> flat within +-0.5 dB from 200 Hz to 10 kHz
    {
        const auto r = responseOf (kSr, flatValues());
        double lo = 1.0e9, hi = -1.0e9;
        for (double f = 200.0; f <= 10000.0; f *= 1.05)
        {
            lo = std::min (lo, r.magDb (f));
            hi = std::max (hi, r.magDb (f));
        }
        note ("flat settings, 200 Hz .. 10 kHz: min (dB)", lo);
        note ("    max (dB)", hi);
        note ("    at 30 Hz (roll-off)", r.magDb (30.0));
        note ("    at 20 kHz", r.magDb (20000.0));
        CHECK (lo > -0.5);
        CHECK (hi < 0.5);
    }

    // measured response == responseDb for a spread of settings, at 44.1 and 96 kHz
    struct Setting
    {
        int speed;
        float lowEmph, highEmph, bias;
    };
    const Setting settings[] { { 0, 10.0f, 0.0f, 0.0f }, { 1, 5.0f, 4.0f, 0.0f }, { 2, 10.0f, 10.0f, 0.0f },
                               { 1, 0.0f, 10.0f, -10.0f }, { 0, 3.0f, 7.0f, 10.0f }, { 2, 0.0f, 0.0f, 5.0f } };
    for (const double sr : { 44100.0, 96000.0 })
        for (const auto& s : settings)
        {
            Values v;
            v.set ("speed", (float) s.speed);
            v.set ("lowEmph", s.lowEmph);
            v.set ("highEmph", s.highEmph);
            v.set ("bias", s.bias);
            const auto r = responseOf (sr, v);
            const auto p = paramsOf (v);
            // responseDb excludes the make-up (a static gain): compare the shapes, aligned at 1 kHz
            const double offset = r.magDb (1000.0) - Tape::responseDb (p, 1000.0);
            double worst = 0.0;
            for (double f = 25.0; f <= 16000.0; f *= 1.03)
                worst = std::max (worst, std::abs (r.magDb (f) - offset - Tape::responseDb (p, f)));
            note ("speed " + std::to_string (s.speed) + " low " + std::to_string ((int) s.lowEmph) + " high "
                      + std::to_string ((int) s.highEmph) + " bias " + std::to_string ((int) s.bias) + " @"
                      + std::to_string ((int) sr) + ": max |measured - responseDb| 25 Hz .. 16 kHz (dB)",
                  worst);
            CHECK (worst < 0.25);
        }
}

TEST_CASE ("tape. head bump: peak at the speed's frequency, 0 .. +10 dB, roll-off kept at 0")
{
    const double bumpHz[] { 40.0, 60.0, 110.0 };
    for (int speed = 0; speed < 3; ++speed)
    {
        double prevPeak = -1.0;
        for (const float lowEmph : { 0.0f, 5.0f, 10.0f })
        {
            Values v = flatValues();
            v.set ("speed", (float) speed);
            v.set ("lowEmph", lowEmph);
            const auto p = paramsOf (v);
            double peak = -100.0, peakHz = 0.0;
            for (double f = 10.0; f <= 400.0; f *= 1.005)
            {
                const double g = Tape::responseDb (p, f);
                if (g > peak)
                {
                    peak = g;
                    peakHz = f;
                }
            }
            const double rollDb = Tape::responseDb (p, bumpHz[speed] * 0.25);
            note ("speed " + std::to_string (speed) + " lowEmph " + std::to_string ((int) lowEmph) + ": peak (dB)", peak);
            note ("    peak frequency (Hz)", peakHz);
            note ("    at a quarter of the bump frequency (dB)", rollDb);
            if (lowEmph == 0.0f)
            {
                CHECK (peak < 0.05);   // no peak
                CHECK (rollDb < -6.0);   // roll-off kept
            }
            else
            {
                CHECK (std::abs (peakHz / bumpHz[speed] - 1.0) < 0.1);
                CHECK (peak > prevPeak + 3.0);
            }
            if (lowEmph == 10.0f)
                CHECK (std::abs (peak - 10.0) < 0.2);
            prevPeak = peak;
        }
    }

    // the measured module agrees (48 kHz, 15 ips, lowEmph 10)
    Values v = flatValues();
    v.set ("lowEmph", 10.0f);
    const auto r = responseOf (kSr, v, 1 << 17);
    note ("measured at 60 Hz, lowEmph 10 (dB)", r.magDb (60.0));
    CHECK (std::abs (r.magDb (60.0) - 10.0) < 0.3);
}

TEST_CASE ("tape. high emphasis: 4 is flat, below rolls off, above lifts; slower speeds lose more")
{
    for (int speed = 0; speed < 3; ++speed)
    {
        double prev = -100.0;
        for (const float e : { 0.0f, 2.0f, 4.0f, 7.0f, 10.0f })
        {
            Values v = flatValues();
            v.set ("speed", (float) speed);
            v.set ("highEmph", e);
            const auto p = paramsOf (v);
            const double at16k = Tape::responseDb (p, 16000.0), at1k = Tape::responseDb (p, 1000.0);
            note ("speed " + std::to_string (speed) + " highEmph " + std::to_string ((int) e) + ": 16 kHz (dB)", at16k);
            CHECK (at16k > prev);
            CHECK (std::abs (at1k) < 0.6);
            if (e == 4.0f)
                CHECK (std::abs (at16k) < 0.01);
            if (e == 0.0f)
                CHECK (at16k < -3.0);
            if (e == 10.0f)
                CHECK (at16k > 3.0);
            prev = at16k;
        }
    }
    // highEmph 0: 7.5 ips loses more at 8 kHz than 30 ips
    Values v7 = flatValues(), v30 = flatValues();
    v7.set ("highEmph", 0.0f);
    v7.set ("speed", 0.0f);
    v30.set ("highEmph", 0.0f);
    v30.set ("speed", 2.0f);
    const double l7 = Tape::responseDb (paramsOf (v7), 8000.0), l30 = Tape::responseDb (paramsOf (v30), 8000.0);
    note ("highEmph 0 at 8 kHz: 7.5 ips (dB)", l7);
    note ("    30 ips (dB)", l30);
    CHECK (l7 < l30 - 1.5);
}

TEST_CASE ("tape. make-up: -18 dBFS pink noise comes out at -18 dBFS at every drive, speed and bias")
{
    const auto x = bandPink (6.0, 11u, -18.0), x2 = bandPink (6.0, 12u, -18.0);
    const size_t from = (size_t) kSr, n = x.size() - from;
    const double inDb = rmsDb (x, from - kTapeLatency, n);
    double worst = 0.0;
    for (int speed = 0; speed < 3; ++speed)
        for (const float bias : { -10.0f, 0.0f, 10.0f })
            for (const float harm : { 0.0f, 10.0f })
                for (const float drive : { -30.0f, -10.0f, 0.0f, 10.0f, 20.0f })
                {
                    Values v = flatValues();
                    v.set ("speed", (float) speed);
                    v.set ("bias", bias);
                    v.set ("harmonics", harm);
                    v.set ("drive", drive);
                    auto m = makeTape (kSr, v);
                    const auto out = run (*m, v, { x, x2 });
                    const double d = rmsDb (out.l, from, n) - inDb;
                    worst = std::max (worst, std::abs (d));
                    if (drive >= 10.0f || drive == -30.0f || std::abs (d) > 0.5)
                        note ("speed " + std::to_string (speed) + " bias " + std::to_string ((int) bias) + " harmonics "
                                  + std::to_string ((int) harm) + " drive " + std::to_string ((int) drive)
                                  + ": out - in (dB)",
                              d);
                }
    note ("worst |out - in| over 90 settings (dB)", worst);
    CHECK (worst <= 0.5);
}

TEST_CASE ("tape. drive saturates: THD rises with drive, odd harmonics only at harmonics 0")
{
    const double f = 200.0;
    const auto x = pstest::sine (f, -6.0, 1.0, kSr);
    const size_t from = (size_t) (0.5 * kSr), n = (size_t) (0.4 * kSr);   // 80 whole cycles
    double prev = -300.0;
    for (const float drive : { -30.0f, -10.0f, 0.0f, 10.0f, 20.0f })
    {
        Values v = flatValues();
        v.set ("drive", drive);
        auto m = makeTape (kSr, v);
        const auto out = run (*m, v, dual (x));
        const double thd = thdDb (out.l, f, kSr, from, n);
        const double h2 = harmonicDbc (out.l, f, 2, kSr, from, n), h3 = harmonicDbc (out.l, f, 3, kSr, from, n);
        note ("200 Hz at -6 dBFS, drive " + std::to_string ((int) drive) + ": THD (dB)", thd);
        note ("    H2 (dBc)", h2);
        note ("    H3 (dBc)", h3);
        CHECK (thd > prev + 3.0);
        CHECK (h2 < -90.0);   // symmetric curve: no even harmonics
        prev = thd;
        if (drive == -30.0f)
            CHECK (thd < -70.0);
        if (drive == 20.0f)
            CHECK (thd > -20.0);
    }
}

TEST_CASE ("tape. harmonics adds even-order distortion, growing with the control")
{
    const double f = 200.0;
    const auto x = pstest::sine (f, -6.0, 1.0, kSr);
    const size_t from = (size_t) (0.5 * kSr), n = (size_t) (0.4 * kSr);
    double prev = -300.0;
    for (const float harm : { 0.0f, 2.5f, 5.0f, 10.0f })
    {
        Values v = flatValues();
        v.set ("harmonics", harm);
        auto m = makeTape (kSr, v);
        const auto out = run (*m, v, dual (x));
        const double h2 = harmonicDbc (out.l, f, 2, kSr, from, n), h3 = harmonicDbc (out.l, f, 3, kSr, from, n);
        double dc = 0.0;
        for (size_t i = from; i < from + n; ++i)
            dc += out.l[i];
        dc /= (double) n;
        note ("drive 0, harmonics " + std::to_string (harm) + ": H2 (dBc)", h2);
        note ("    H3 (dBc)", h3);
        note ("    DC (dBFS)", db (std::abs (dc)));
        CHECK (h2 > prev + (harm == 0.0f ? 0.0 : 3.0));
        CHECK (db (std::abs (dc)) < -80.0);   // the even part's DC is removed
        if (harm == 10.0f)
        {
            CHECK (h2 > -30.0);
            CHECK (h2 > h3);
        }
        prev = h2;
    }
}

TEST_CASE ("tape. pre-emphasis: highs saturate earlier, equal distortion at 500 Hz / 1 kHz / 2 kHz per speed")
{
    auto h3At = [] (int speed, double f, float drive, float bias) {
        Values v = flatValues();
        v.set ("speed", (float) speed);
        v.set ("drive", drive);
        v.set ("bias", bias);
        auto m = makeTape (kSr, v);
        const auto x = pstest::sine (f, -12.0, 1.0, kSr);
        const auto out = run (*m, v, dual (x));
        // a whole number of cycles of f over ~0.4 s
        const auto cycles = (size_t) std::floor (0.4 * f);
        const auto n = (size_t) std::lround ((double) cycles * kSr / f);
        return harmonicDbc (out.l, f, 3, kSr, (size_t) (0.5 * kSr), n);
    };
    const double h500 = h3At (0, 500.0, 6.0f, 0.0f), h1k = h3At (1, 1000.0, 6.0f, 0.0f), h2k = h3At (2, 2000.0, 6.0f, 0.0f);
    note ("H3 of the corner tone at -12 dBFS, drive +6: 7.5 ips / 500 Hz (dBc)", h500);
    note ("    15 ips / 1 kHz (dBc)", h1k);
    note ("    30 ips / 2 kHz (dBc)", h2k);
    CHECK (std::abs (h500 - h1k) < 1.0);
    CHECK (std::abs (h2k - h1k) < 1.0);

    const double lowF = h3At (1, 100.0, 6.0f, 0.0f), highF = h3At (1, 4000.0, 6.0f, 0.0f);
    note ("15 ips, drive +6: H3 at 100 Hz (dBc)", lowF);
    note ("    H3 at 4 kHz (dBc)", highF);
    CHECK (highF > lowF + 6.0);

    // negative bias saturates the highs earlier still (relative to the lows)
    const double lowN = h3At (1, 100.0, 6.0f, -10.0f), highN = h3At (1, 4000.0, 6.0f, -10.0f);
    note ("bias -10: H3 at 4 kHz minus H3 at 100 Hz (dB)", highN - lowN);
    note ("bias 0:   H3 at 4 kHz minus H3 at 100 Hz (dB)", highF - lowF);
    CHECK (highN - lowN > highF - lowF + 2.0);
}

TEST_CASE ("tape. bias: positive compresses the dynamics, negative lifts the highs")
{
    // level difference of a -30 dBFS and a -6 dBFS 200 Hz tone (24 dB in) at drive +6
    auto outLevel = [] (float bias, double levelDb) {
        Values v = flatValues();
        v.set ("drive", 6.0f);
        v.set ("bias", bias);
        auto m = makeTape (kSr, v);
        const auto x = pstest::sine (200.0, levelDb, 1.0, kSr);
        const auto out = run (*m, v, dual (x));
        return toneDb (out.l, 200.0, kSr, (size_t) (0.5 * kSr), (size_t) (0.4 * kSr));
    };
    double prev = 100.0;
    for (const float bias : { -10.0f, 0.0f, 5.0f, 10.0f })
    {
        const double range = outLevel (bias, -6.0) - outLevel (bias, -30.0);
        note ("bias " + std::to_string ((int) bias) + ": output range of a 24 dB input step (dB)", range);
        CHECK (range < prev);
        if (bias == 10.0f)
            CHECK (range < 18.0);
        prev = range;
    }

    Values neg = flatValues(), pos = flatValues();
    neg.set ("bias", -10.0f);
    pos.set ("bias", 10.0f);
    const double n16 = Tape::responseDb (paramsOf (neg), 16000.0), p16 = Tape::responseDb (paramsOf (pos), 16000.0);
    note ("small-signal 16 kHz: bias -10 (dB)", n16);
    note ("    bias +10 (dB)", p16);
    CHECK (n16 > 0.7);
    CHECK (p16 < -0.7);
}

TEST_CASE ("tape. hysteresis: the curve depends on the slope's sign, wider at negative bias, none at small levels")
{
    // A memoryless odd curve keeps the fundamental in phase; the hysteresis loop delays it (the output lags the input
    // as magnetisation does). Phase of a 100 Hz fundamental against the same settings at drive -30 (linear).
    auto phaseOf = [] (float bias, float drive) {
        Values v = flatValues();
        v.set ("drive", drive);
        v.set ("bias", bias);
        auto m = makeTape (kSr, v);
        const auto x = pstest::sine (100.0, -6.0, 1.0, kSr);
        const auto out = run (*m, v, dual (x));
        return std::arg (pstest::toneOf (out.l, (size_t) (0.5 * kSr), (size_t) (0.4 * kSr), 100.0, kSr));
    };
    auto lagDeg = [&] (float bias, float drive) {
        double d = phaseOf (bias, -30.0f) - phaseOf (bias, drive);
        while (d > kPi) d -= 2.0 * kPi;
        while (d < -kPi) d += 2.0 * kPi;
        return d * 180.0 / kPi;
    };
    const double lNeg = lagDeg (-10.0f, 14.0f), lZero = lagDeg (0.0f, 14.0f), lPos = lagDeg (10.0f, 14.0f);
    const double lQuiet = lagDeg (0.0f, -12.0f);
    note ("phase lag of the fundamental at drive +14 (deg): bias -10", lNeg);
    note ("    bias 0", lZero);
    note ("    bias +10", lPos);
    note ("    drive -12, bias 0 (small level)", lQuiet);
    CHECK (lNeg > lZero);
    CHECK (lZero > lPos);
    CHECK (lPos > 0.0);
    CHECK (lZero > 1.0);
    CHECK (std::abs (lQuiet) < 0.05);
}

TEST_CASE ("tape. meters: input drive level and saturation follow the audio")
{
    const auto x = pstest::sine (100.0, -6.0, 1.0, kSr);
    for (const float drive : { -12.0f, 0.0f, 6.0f, 20.0f })
    {
        Values v = flatValues();
        v.set ("drive", drive);
        auto m = makeTape (kSr, v);
        run (*m, v, dual (x));
        const float in = m->getMeters().inputDriveDb.load(), sat = m->getMeters().saturationDb.load();
        note ("100 Hz at -6 dBFS, drive " + std::to_string ((int) drive) + ": input drive meter (dBFS)", in);
        note ("    saturation meter (dB)", sat);
        // 100 Hz is far below the pre-emphasis: the level into the curve = input peak + drive (a little pre-emphasis)
        CHECK (std::abs (in - (-6.0f + drive)) < 0.5f);
        if (drive <= -12.0f)
            CHECK (sat < 0.2f);
        if (drive >= 20.0f)
            CHECK (sat > 6.0f);
    }
    // silence: the meters fall back
    Values v = flatValues();
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
    for (const double sr : { 44100.0, 96000.0 })
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
        hot.set ("drive", 20.0f);
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
    for (const double sr : { 44100.0, 48000.0, 96000.0 })
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
            v.set ("speed", 2.0f);
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
