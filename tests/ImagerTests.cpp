#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/Crossover.h"
#include "dsp/Imager.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

// Imager (dsp/Imager.cpp). Prefix "img.".
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
        const int i = paramIndex (id);
        REQUIRE (i >= 0);
        v[(size_t) i] = x;
    }
    ParamValues data() const { return v.data(); }
};

struct Stereo
{
    std::vector<float> l, r;
};

// Runs the imager over the input in blocks; onBlock (start, values) may change parameters before each block.
Stereo run (Imager& m, Values& vals, const Stereo& in, int block,
            const std::function<void (size_t, Values&)>& onBlock = {})
{
    Stereo out = in;
    for (size_t start = 0; start < in.l.size(); start += (size_t) block)
    {
        if (onBlock)
            onBlock (start, vals);
        m.setParams (vals.data());
        const int n = (int) std::min ((size_t) block, in.l.size() - start);
        float* ch[2] { out.l.data() + start, out.r.data() + start };
        m.process (ch, n, ProcessContext {});
    }
    return out;
}

Stereo noisePair (double seconds, float amp, std::uint32_t seedL, std::uint32_t seedR)
{
    return { pstest::whiteNoise (amp, seconds, kSr, seedL), pstest::whiteNoise (amp, seconds, kSr, seedR) };
}

double maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t offsetB = 0)
{
    double m = 0.0;
    for (size_t i = from; i < a.size() && i >= offsetB; ++i)
        m = std::max (m, std::abs ((double) a[i] - (double) b[i - offsetB]));
    return m;
}

// correlation of two tones from their complex amplitudes
double toneCorrelation (cd a, cd b) { return std::real (a * std::conj (b)) / std::max (1e-30, std::abs (a) * std::abs (b)); }

double power (const std::vector<float>& x, size_t from)
{
    double s = 0.0;
    for (size_t i = from; i < x.size(); ++i)
        s += (double) x[i] * (double) x[i];
    return s / (double) std::max<size_t> (1, x.size() - from);
}
} // namespace

//==============================================================================
TEST_CASE ("img. width -100 gives exact mono (1 and 3 bands)")
{
    for (int bands : { 1, 3 })
    {
        Imager m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("img.bands", (float) bands);
        for (int b = 1; b <= 4; ++b)
            v.set ("img.b" + std::to_string (b) + ".width", -100.0f);
        const auto in = noisePair (0.5, 0.5f, 1, 2);
        const auto out = run (m, v, in, 512);

        double lr = 0.0, mid = 0.0;
        for (size_t i = 0; i < out.l.size(); ++i)
        {
            lr = std::max (lr, std::abs ((double) out.l[i] - (double) out.r[i]));
            if (bands == 1)
                mid = std::max (mid, std::abs ((double) out.l[i] - 0.5 * ((double) in.l[i] + (double) in.r[i])));
        }
        note ("img. width -100, bands " + std::to_string (bands) + ": max |L - R|", lr);
        CHECK (lr <= 1.0e-6);
        if (bands == 1)
        {
            note ("img. width -100, 1 band: max |L - (Lin + Rin) / 2|", mid);
            CHECK (mid <= 1.0e-6);
        }
    }
}

TEST_CASE ("img. width 0 is crossover-only / neutral (null)")
{
    struct Case { int bands; CrossoverType type; };
    for (const Case c : { Case { 1, CrossoverType::Analog }, Case { 3, CrossoverType::Analog },
                          Case { 4, CrossoverType::Hybrid }, Case { 3, CrossoverType::Digital } })
    {
        Imager m;
        m.prepare (kSr, 256);
        Values v;
        v.set ("img.bands", (float) c.bands);
        v.set ("img.xoverType", (float) c.type);
        v.set ("img.stz.on", 1.0f);      // only acts on bands with width > 0
        v.set ("img.stz.amount", 80.0f);
        v.set ("img.rec.on", 1.0f);      // only acts on bands with width < 0
        const auto in = noisePair (0.5, 0.3f, 3, 4);
        const auto out = run (m, v, in, 256);
        const int lat = m.getLatencySamples();
        const double d = std::max (maxAbsDiff (out.l, in.l, (size_t) lat, (size_t) lat),
                                   maxAbsDiff (out.r, in.r, (size_t) lat, (size_t) lat));
        note ("img. neutral null, bands " + std::to_string (c.bands) + " type " + std::to_string ((int) c.type)
                  + ", latency " + std::to_string (lat) + ": max diff",
              d);
        CHECK (d == 0.0);   // bit-exact
    }
}

TEST_CASE ("img. stereoize keeps L + R identical (Mode I and II)")
{
    for (int mode : { 0, 1 })
    {
        for (int bands : { 1, 3 })
        {
            Values v;
            v.set ("img.bands", (float) bands);
            for (int b = 1; b <= 4; ++b)
                v.set ("img.b" + std::to_string (b) + ".width", b == 2 ? -30.0f : 100.0f);
            v.set ("img.stz.mode", (float) mode);
            v.set ("img.stz.amount", 100.0f);

            // partly correlated stereo material (mono noise + a little side)
            const auto mono = pstest::whiteNoise (0.4, 1.0, kSr, 7);
            const auto side = pstest::whiteNoise (0.05, 1.0, kSr, 8);
            Stereo in { mono, mono };
            for (size_t i = 0; i < mono.size(); ++i)
            {
                in.l[i] += side[i];
                in.r[i] -= side[i];
            }

            Imager off, on;
            off.prepare (kSr, 512);
            on.prepare (kSr, 512);
            const auto refOut = run (off, v, in, 512);
            v.set ("img.stz.on", 1.0f);
            const auto out = run (on, v, in, 512);

            double dSum = 0.0, dInput = 0.0;
            for (size_t i = 0; i < out.l.size(); ++i)
            {
                const double s = (double) out.l[i] + (double) out.r[i];
                dSum = std::max (dSum, std::abs (s - ((double) refOut.l[i] + (double) refOut.r[i])));
                dInput = std::max (dInput, std::abs (s - ((double) in.l[i] + (double) in.r[i])));
            }

            // how much side the stereoize added
            std::vector<float> sOn (out.l.size()), sOff (out.l.size());
            for (size_t i = 0; i < out.l.size(); ++i)
            {
                sOn[i] = 0.5f * (out.l[i] - out.r[i]);
                sOff[i] = 0.5f * (refOut.l[i] - refOut.r[i]);
            }
            const double added = 10.0 * std::log10 (power (sOn, 9600) / power (sOff, 9600));

            const std::string tag = "img. stereoize mode " + std::string (mode == 0 ? "I" : "II") + ", bands "
                                    + std::to_string (bands);
            note (tag + ": max |(L+R) - (L+R without stereoize)| dB", pstest::dbOf (dSum));
            CHECK (pstest::dbOf (dSum) < -100.0);
            if (bands == 1)
            {
                note (tag + ": max |(L+R) - input L+R| dB", pstest::dbOf (dInput));
                CHECK (pstest::dbOf (dInput) < -100.0);
            }
            note (tag + ": side level added (dB)", added);
            CHECK (added > 3.0);
        }
    }
}

TEST_CASE ("img. recover sides restores a hard-panned source when narrowing")
{
    // hard-left 500 Hz sine, width -100
    const auto s = pstest::sine (500.0, -6.0, 1.0, kSr);
    Stereo in { s, std::vector<float> (s.size(), 0.0f) };
    const double inPower = power (in.l, 24000) + power (in.r, 24000);

    double resultDb[3] {};
    const float gains[3] { -100.0f, 0.0f, -6.0f };   // -100 = Recover off
    for (int k = 0; k < 3; ++k)
    {
        Imager m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("img.b1.width", -100.0f);
        if (gains[k] > -50.0f)
        {
            v.set ("img.rec.on", 1.0f);
            v.set ("img.rec.gain", gains[k]);
        }
        const auto out = run (m, v, in, 512);
        resultDb[k] = 10.0 * std::log10 ((power (out.l, 24000) + power (out.r, 24000)) / inPower);
        CHECK (maxAbsDiff (out.l, out.r, 24000) < 1.0e-6);   // still mono
    }
    note ("img. hard-left source at width -100, total power change, Recover off (dB)", resultDb[0]);
    note ("img. hard-left source at width -100, total power change, Recover 0 dB (dB)", resultDb[1]);
    note ("img. hard-left source at width -100, total power change, Recover -6 dB (dB)", resultDb[2]);
    CHECK (std::abs (resultDb[0] + 3.01) < 0.05);
    CHECK (std::abs (resultDb[1]) < 0.1);
    CHECK (resultDb[2] > resultDb[0] + 0.5 && resultDb[2] < resultDb[1] - 0.5);

    // a centred source is not touched by Recover Sides
    {
        Imager m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("img.b1.width", -100.0f);
        v.set ("img.rec.on", 1.0f);
        const Stereo c { s, s };
        const auto out = run (m, v, c, 512);
        const double d = maxAbsDiff (out.l, c.l, 24000);
        note ("img. centred source with Recover Sides: max diff", d);
        CHECK (d < 1.0e-6);
    }
}

TEST_CASE ("img. prevent antiphase makes a polarity-inverted band's correlation >= 0")
{
    // low band: mono 150 Hz; high band (above 1 kHz): 5 kHz, right channel inverted and a bit lower
    const auto low = pstest::sine (150.0, -12.0, 1.0, kSr);
    const auto high = pstest::sine (5000.0, -12.0, 1.0, kSr);
    Stereo in { low, low };
    for (size_t i = 0; i < low.size(); ++i)
    {
        in.l[i] += high[i];
        in.r[i] -= 0.8f * high[i];
    }

    for (bool anti : { false, true })
    {
        Imager m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("img.bands", 2.0f);
        v.set ("img.xover1", 1000.0f);
        v.set ("img.antiphase", anti ? 1.0f : 0.0f);
        v.set ("img.b2.width", anti ? 0.0f : 1.0f);   // force processing in the reference too
        const auto out = run (m, v, in, 512);

        const size_t start = 24000, n = 24000;
        const cd hl = pstest::toneOf (out.l, start, n, 5000.0, kSr);
        const cd hr = pstest::toneOf (out.r, start, n, 5000.0, kSr);
        const cd ll = pstest::toneOf (out.l, start, n, 150.0, kSr);
        const cd lr = pstest::toneOf (out.r, start, n, 150.0, kSr);
        const double corrHigh = toneCorrelation (hl, hr);
        const double lowGainDb = pstest::dbOf (std::abs (ll) / pstest::dbfsToAmplitude (-12.0));
        const double meter = m.getMeters().correlation[1].load();
        const std::string tag = std::string ("img. antiphase ") + (anti ? "on" : "off");
        note (tag + ": 5 kHz band correlation", corrHigh);
        note (tag + ": band 2 correlation meter", meter);
        note (tag + ": 150 Hz (band 1) gain dB", lowGainDb);
        CHECK (std::abs (toneCorrelation (ll, lr) - 1.0) < 1.0e-6);
        CHECK (std::abs (lowGainDb) < 0.05);
        if (anti)
        {
            CHECK (corrHigh >= 0.0);
            CHECK (meter >= -0.01f);
        }
        else
        {
            CHECK (corrHigh < -0.9);
            CHECK (meter < -0.9f);
        }
    }
}

TEST_CASE ("img. width sweep and jumps are click-free")
{
    const auto a = pstest::sine (440.0, -6.0, 2.0, kSr);
    const auto b = pstest::sine (550.0, -6.0, 2.0, kSr, 1.0);
    const Stereo in { a, b };

    auto maxSecondDiff = [] (const Stereo& s, size_t from)
    {
        double m = 0.0;
        for (const auto* x : { &s.l, &s.r })
            for (size_t i = from + 2; i < x->size(); ++i)
                m = std::max (m, std::abs ((double) (*x)[i] - 2.0 * (double) (*x)[i - 1] + (double) (*x)[i - 2]));
        return m;
    };

    double staticMax = 0.0;
    for (float w : { -100.0f, 100.0f })
    {
        Imager m;
        m.prepare (kSr, 64);
        Values v;
        v.set ("img.b1.width", w);
        staticMax = std::max (staticMax, maxSecondDiff (run (m, v, in, 64), 4800));
    }

    for (int variant = 0; variant < 3; ++variant)
    {
        Imager m;
        m.prepare (kSr, 64);
        Values v;
        v.set ("img.bands", 2.0f);
        v.set ("img.xover1", 500.0f);
        if (variant == 2)
        {
            v.set ("img.stz.on", 1.0f);
            v.set ("img.stz.amount", 60.0f);
        }
        const auto out = run (m, v, in, 64, [&] (size_t start, Values& vals)
        {
            const double t = (double) start / kSr;
            float w;
            if (variant == 1)
                w = ((start / 4800) % 2) == 0 ? -100.0f : 100.0f;   // instant jumps every 100 ms
            else
                w = (float) (-100.0 + 100.0 * t);                    // linear sweep -100 .. +100 over 2 s
            vals.set ("img.b1.width", w);
            vals.set ("img.b2.width", -w);
        });
        const double d = maxSecondDiff (out, 2400);   // skip the crossover's start-up transient
        note ("img. width " + std::string (variant == 1 ? "jumps" : (variant == 2 ? "sweep + stereoize" : "sweep"))
                  + ": max |2nd difference| / static max",
              d / staticMax);
        CHECK (d < 1.3 * staticMax);
    }
}

TEST_CASE ("img. latency is reported exactly and bands stay aligned")
{
    Crossover ref;
    ref.prepare (kSr, 512);
    for (CrossoverType t : { CrossoverType::Analog, CrossoverType::Digital })
    {
        Imager m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("img.bands", 3.0f);
        v.set ("img.xoverType", (float) t);
        v.set ("img.b1.width", 50.0f);
        v.set ("img.b2.width", 50.0f);
        v.set ("img.b3.width", 50.0f);
        m.setParams (v.data());
        const int lat = m.getLatencySamples();
        CHECK_EQ (lat, ref.getLatencySamples (t));

        // mono impulse (S = 0): the width cannot change it, so the output must be the crossover sum (Digital: the
        // delayed impulse)
        const size_t len = 16384, at = 2000;
        Stereo in { std::vector<float> (len, 0.0f), std::vector<float> (len, 0.0f) };
        in.l[at] = in.r[at] = 0.5f;
        // warm up the ramps on silence first
        Stereo silence { std::vector<float> (4800, 0.0f), std::vector<float> (4800, 0.0f) };
        (void) run (m, v, silence, 512);
        const auto out = run (m, v, in, 512);
        size_t peak = 0;
        for (size_t i = 0; i < len; ++i)
            if (std::abs (out.l[i]) > std::abs (out.l[peak]))
                peak = i;
        note ("img. latency " + std::string (t == CrossoverType::Digital ? "Digital" : "Analog") + ": reported", lat);
        if (t == CrossoverType::Digital)
        {
            CHECK_EQ ((int) (peak - at), lat);
            CHECK (std::abs (out.l[peak] - 0.5f) < 1.0e-4f);
        }
        else
        {
            CHECK_EQ (lat, 0);
        }
        CHECK (maxAbsDiff (out.l, out.r, 0) < 1.0e-7);
    }
}

TEST_CASE ("img. meters report correlation and width")
{
    const auto s = pstest::sine (700.0, -10.0, 1.0, kSr);
    auto measure = [&] (const Stereo& in, float width)
    {
        Imager m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("img.b1.width", width);
        (void) run (m, v, in, 512);
        return std::make_pair (m.getMeters().correlation[0].load(), m.getMeters().widthRatio[0].load());
    };
    const auto mono = measure ({ s, s }, 0.0f);
    std::vector<float> inv (s.size());
    for (size_t i = 0; i < s.size(); ++i)
        inv[i] = -s[i];
    const auto anti = measure ({ s, inv }, 30.0f);
    const auto left = measure ({ s, std::vector<float> (s.size(), 0.0f) }, 0.0f);
    note ("img. meter: mono correlation", mono.first);
    note ("img. meter: antiphase correlation", anti.first);
    note ("img. meter: hard-left S/M ratio", left.second);
    CHECK (mono.first > 0.999f);
    CHECK (mono.second < 0.001f);
    CHECK (anti.first < -0.999f);
    CHECK (std::abs (left.second - 1.0f) < 0.01f);
}

TEST_CASE ("img. no allocation in setParams / process")
{
    Imager m;
    m.prepare (kSr, 512);
    Values v;
    const auto in = noisePair (0.1, 0.3f, 11, 12);
    std::vector<float> l = in.l, r = in.r;
    float* ch[2] { l.data(), r.data() };

    pstest::AllocationScope scope;
    for (int pass = 0; pass < 24; ++pass)
    {
        v.set ("img.bands", (float) (1 + pass % 4));
        v.set ("img.xoverType", (float) ((pass / 4) % 3));
        v.set ("img.b1.width", pass % 2 ? -100.0f : 60.0f);
        v.set ("img.b2.width", 40.0f);
        v.set ("img.b3.width", -50.0f);
        v.set ("img.b4.width", 100.0f);
        v.set ("img.stz.on", (float) (pass % 3 != 0));
        v.set ("img.stz.mode", (float) (pass % 2));
        v.set ("img.stz.amount", 70.0f);
        v.set ("img.rec.on", 1.0f);
        v.set ("img.antiphase", (float) (pass % 2));
        ModuleMonitor mon;
        mon.soloBand = pass % 5 == 4 ? 1 : -1;
        m.setMonitor (mon);
        m.setParams (v.data());
        m.process (ch, 512, ProcessContext {});
        if (pass == 12)
            m.reset();
    }
    CHECK_EQ (scope.count(), 0LL);
}

TEST_CASE ("img. cpu benchmark")
{
    for (int config = 0; config < 2; ++config)
    {
        Imager m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("img.bands", config == 0 ? 1.0f : 4.0f);
        v.set ("img.b1.width", -60.0f);
        v.set ("img.b2.width", 20.0f);
        v.set ("img.b3.width", 50.0f);
        v.set ("img.b4.width", 80.0f);
        v.set ("img.stz.on", 1.0f);
        v.set ("img.stz.mode", 1.0f);
        v.set ("img.stz.amount", 50.0f);
        v.set ("img.rec.on", 1.0f);
        v.set ("img.antiphase", 1.0f);
        const auto in = noisePair (10.0, 0.3f, 5, 6);
        pstest::Stopwatch sw;
        const auto out = run (m, v, in, 512);
        const double secs = sw.seconds();
        CHECK (std::isfinite (out.l.back()));
        note (std::string ("img. cpu: ") + (config == 0 ? "1 band" : "4 bands Analog")
                  + ", stereoize II + recover + antiphase, x realtime",
              10.0 / secs);
        CHECK_SPEED (10.0 / secs > 20.0);
    }
}

TEST_CASE ("img. neutral crossover latency changes crossfade warm delay taps")
{
    Imager m;
    m.prepare (kSr, 64);
    Values v;
    const auto tone = pstest::sine (173.0, -6.0, 0.7, kSr, 0.3);
    const Stereo in { tone, tone };
    constexpr size_t first = 8192, second = 16384;
    const auto out = run (m, v, in, 64, [] (size_t start, Values& values)
    {
        if (start == first)
            values.set ("img.xoverType", (float) CrossoverType::Digital);
        if (start == second)
            values.set ("img.xoverType", (float) CrossoverType::Analog);
    });
    double secondDifference = 0.0;
    for (size_t at : { first, second })
        for (size_t i = at; i < at + 2048; ++i)
            secondDifference = std::max (secondDifference,
                std::abs ((double) out.l[i] - 2.0 * out.l[i - 1] + out.l[i - 2]));
    note ("img. neutral type changes: maximum second difference", secondDifference);
    CHECK (secondDifference < 0.002);
    Crossover ref;
    ref.prepare (kSr, 64);
    const size_t latency = (size_t) ref.getLatencySamples (CrossoverType::Digital);
    for (size_t i = first + 2048; i < second; ++i)
        CHECK_EQ (out.l[i], in.l[i - latency]);
    for (size_t i = second + 2048; i < out.l.size(); ++i)
        CHECK_EQ (out.l[i], in.l[i]);
    CHECK_EQ (m.getLatencySamples(), 0);
}

TEST_CASE ("img. band count edits crossfade unequal width assignments")
{
    Imager m;
    m.prepare (kSr, 64);
    Values v;
    v.set ("img.bands", 2.0f);
    v.set ("img.xover1", 100.0f);
    v.set ("img.b1.width", -100.0f);
    v.set ("img.b2.width", 100.0f);
    const auto tone = pstest::sine (330.0, -12.0, 0.6, kSr, 0.3);
    Stereo in { tone, tone };
    for (auto& r : in.r)
        r = -r;
    constexpr size_t first = 8192, second = 16384;
    const auto out = run (m, v, in, 64, [] (size_t start, Values& values)
    {
        if (start == first)
            values.set ("img.bands", 1.0f);
        if (start == second)
            values.set ("img.bands", 2.0f);
    });
    double secondDifference = 0.0;
    for (size_t at : { first, second })
        for (size_t i = at; i < at + 2048; ++i)
            secondDifference = std::max (secondDifference,
                std::abs ((double) out.l[i] - 2.0 * out.l[i - 1] + out.l[i - 2]));
    note ("img. band-count edits: maximum second difference", secondDifference);
    CHECK (secondDifference < 0.01);
    CHECK (power (out.l, second + 4096) > 0.01);
    for (size_t i = first + 2048; i < second; ++i)
        CHECK (std::abs (out.l[i]) < 1.0e-7f);
}

TEST_CASE ("img. removed bands discard stereoize history before reactivation")
{
    for (int mode : { 0, 1 })
    {
        Imager m;
        m.prepare (kSr, 64);
        Values v;
        v.set ("img.bands", 2.0f);
        v.set ("img.xover1", 1000.0f);
        v.set ("img.b2.width", 100.0f);
        v.set ("img.stz.on", 1.0f);
        v.set ("img.stz.amount", 100.0f);
        v.set ("img.stz.mode", (float) mode);
        const auto tone = pstest::sine (5000.0, -6.0, 0.2, kSr, 0.4);
        (void) run (m, v, { tone, tone }, 64);
        v.set ("img.bands", 1.0f);
        const Stereo silence { std::vector<float> (24000, 0.0f), std::vector<float> (24000, 0.0f) };
        (void) run (m, v, silence, 64);
        v.set ("img.bands", 2.0f);
        const auto out = run (m, v, silence, 64);
        double peak = 0.0;
        for (size_t i = 0; i < out.l.size(); ++i)
            peak = std::max ({ peak, std::abs ((double) out.l[i]), std::abs ((double) out.r[i]) });
        note ("img. re-added stereoize band peak, mode " + std::to_string (mode), peak);
        CHECK (peak < 1.0e-8);
    }
}

TEST_CASE ("img. stereoize follows the sign of the smoothed width")
{
    Imager m;
    m.prepare (kSr, 64);
    Values v;
    v.set ("img.b1.width", 100.0f);
    v.set ("img.stz.on", 1.0f);
    v.set ("img.stz.amount", 100.0f);
    const auto tone = pstest::sine (431.0, -12.0, 0.4, kSr);
    const Stereo in { tone, tone };
    constexpr size_t down = 4096, up = 12288;
    const auto out = run (m, v, in, 64, [] (size_t start, Values& values)
    {
        if (start == down)
            values.set ("img.b1.width", -100.0f);
        if (start == up)
            values.set ("img.b1.width", 100.0f);
    });
    double negativeWidthSide = 0.0;
    for (size_t i = down + 736; i < up + 704; ++i)
        negativeWidthSide = std::max (negativeWidthSide, std::abs ((double) out.l[i] - out.r[i]));
    note ("img. Stereoize side while smoothed width is negative", negativeWidthSide);
    CHECK (negativeWidthSide == 0.0);
}

TEST_CASE ("img. recover sides preserves pure Side energy when Mid is zero")
{
    const auto tone = pstest::sine (500.0, -12.0, 1.0, kSr);
    Stereo in { tone, tone };
    for (auto& r : in.r)
        r = -r;
    for (float recoverDb : { -6.0f, 0.0f, 6.0f })
    {
        Imager m;
        m.prepare (kSr, 64);
        Values v;
        v.set ("img.b1.width", -100.0f);
        v.set ("img.rec.on", 1.0f);
        v.set ("img.rec.gain", recoverDb);
        const auto out = run (m, v, in, 64);
        CHECK (maxAbsDiff (out.l, out.r, 0) == 0.0);
        const double gain = 10.0 * std::log10 (power (out.l, 24000) / power (in.l, 24000));
        note ("img. pure-S recovery gain, requested " + std::to_string (recoverDb), gain);
        CHECK (std::abs (gain - recoverDb) < 0.01);
    }

    Imager m;
    m.prepare (kSr, 64);
    Values v;
    v.set ("img.b1.width", -100.0f);
    const auto out = run (m, v, in, 64, [] (size_t start, Values& values)
    {
        if (start == 8192)
            values.set ("img.rec.on", 1.0f);
        if (start == 32768)
            values.set ("img.rec.on", 0.0f);
    });
    double secondDifference = 0.0;
    for (size_t i = 2; i < out.l.size(); ++i)
        secondDifference = std::max (secondDifference,
            std::abs ((double) out.l[i] - 2.0 * out.l[i - 1] + out.l[i - 2]));
    CHECK (secondDifference < 0.002);
}

TEST_CASE ("img. meters are sample weighted across arbitrary host block sizes")
{
    auto render = [] (bool variable)
    {
        Imager m;
        m.prepare (kSr, 512);
        Values v;
        const auto tone = pstest::sine (700.0, -12.0, 0.4, kSr);
        Stereo audio { tone, tone };
        for (size_t i = 9600; i < audio.r.size(); ++i)
            audio.r[i] = -audio.r[i];
        for (size_t offset = 0; offset < audio.l.size();)
        {
            const int block = variable ? (offset < 9600 ? 1 : 512) : 64;
            const int n = (int) std::min ((size_t) block, audio.l.size() - offset);
            m.setParams (v.data());
            float* ch[2] { audio.l.data() + offset, audio.r.data() + offset };
            m.process (ch, n, ProcessContext {});
            offset += (size_t) n;
        }
        return std::make_pair (m.getMeters().correlation[0].load(), m.getMeters().widthRatio[0].load());
    };
    const auto fixed = render (false), varied = render (true);
    CHECK (std::abs (fixed.first - varied.first) < 1.0e-7f);
    CHECK (std::abs (fixed.second - varied.second) < 1.0e-7f);
}

TEST_CASE ("img. neutral processing clears meters of removed bands")
{
    Imager m;
    m.prepare (kSr, 64);
    Values v;
    v.set ("img.bands", 4.0f);
    const auto noise = noisePair (0.1, 0.2f, 21, 22);
    (void) run (m, v, noise, 64);
    CHECK (m.getMeters().widthRatio[3].load() > 0.1f);
    v.set ("img.bands", 1.0f);
    (void) run (m, v, noise, 64);
    for (int b = 1; b < kMaxBands; ++b)
    {
        CHECK_EQ (m.getMeters().widthRatio[b].load(), 0.0f);
        CHECK_EQ (m.getMeters().correlation[b].load(), 0.0f);
    }
}

TEST_CASE ("img. topology fade consumes the last request without repeated setParams")
{
    for (bool finishDigital : { false, true })
    {
        Imager m;
        m.prepare (kSr, 64);
        Values v;
        const CrossoverType initial = finishDigital ? CrossoverType::Digital : CrossoverType::Analog;
        v.set ("img.xoverType", (float) initial);
        m.setParams (v.data());
        const auto tone = pstest::sine (173.0, -12.0, 0.5, kSr, 0.3);
        Stereo out { tone, tone };
        auto process = [&] (size_t start, int count)
        {
            float* ch[2] { out.l.data() + start, out.r.data() + start };
            m.process (ch, count, ProcessContext {});
        };
        process (0, 8192);
        v.set ("img.xoverType", (float) (finishDigital ? CrossoverType::Analog : CrossoverType::Digital));
        v.set ("img.bands", 2.0f);
        m.setParams (v.data());
        process (8192, 64);
        v.set ("img.xoverType", (float) CrossoverType::Hybrid);
        v.set ("img.bands", 3.0f);
        m.setParams (v.data());
        process (8256, 64);
        v.set ("img.xoverType", (float) initial);
        v.set ("img.bands", 1.0f);
        m.setParams (v.data());
        const int latency = m.getLatencySamples();
        CHECK_EQ (latency == 0, ! finishDigital);
        // No more parameter writes: process must finish both fades and honour the last request itself.
        process (8320, (int) out.l.size() - 8320);
        for (size_t i = 15000; i < out.l.size(); ++i)
        {
            CHECK_EQ (out.l[i], tone[i - (size_t) latency]);
            CHECK_EQ (out.r[i], tone[i - (size_t) latency]);
        }
    }
}

TEST_CASE ("img. reset retains the latest topology without another parameter write")
{
    for (bool pending : { false, true })
    {
        Imager m;
        m.prepare (kSr, 64);
        Values v;
        m.setParams (v.data());
        Stereo silence { std::vector<float> (4096, 0.0f), std::vector<float> (4096, 0.0f) };
        float* warm[2] { silence.l.data(), silence.r.data() };
        m.process (warm, 4096, ProcessContext {});
        v.set ("img.bands", 3.0f);
        v.set ("img.xoverType", (float) CrossoverType::Digital);
        m.setParams (v.data());
        m.process (warm, pending ? 64 : 1600, ProcessContext {});
        if (pending)
        {
            v.set ("img.xoverType", (float) CrossoverType::Hybrid);
            m.setParams (v.data());
        }
        const int latency = m.getLatencySamples();
        m.reset();
        CHECK_EQ (m.getLatencySamples(), latency);
        Stereo impulse { std::vector<float> (5000, 0.0f), std::vector<float> (5000, 0.0f) };
        impulse.l[17] = impulse.r[17] = 0.25f;
        float* ch[2] { impulse.l.data(), impulse.r.data() };
        m.process (ch, (int) impulse.l.size(), ProcessContext {});
        for (size_t i = 0; i < impulse.l.size(); ++i)
        {
            const float expected = i == 17 + (size_t) latency ? 0.25f : 0.0f;
            CHECK_EQ (impulse.l[i], expected);
            CHECK_EQ (impulse.r[i], expected);
        }
    }
}
