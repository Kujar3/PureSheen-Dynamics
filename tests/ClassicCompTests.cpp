#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/ClassicComp.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Classic Compressor tests (dsp/ClassicComp.h). Prefix "ccmp.". Owner: mod-ccmp.
namespace
{
using namespace ps;
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
        const int i = paramIndex ("ccmp." + id);
        REQUIRE (i >= 0);
        v[(size_t) i] = x;
    }
    void setBoth (const std::string& name, float x)
    {
        set ("a." + name, x);
        set ("b." + name, x);
    }
    // ratio 1, makeup 0, auto gain off on both sets (the documented neutral settings)
    void neutral()
    {
        setBoth ("ratio", 1.0f);
        setBoth ("makeup", 0.0f);
        setBoth ("autoGain", 0.0f);
    }
    ParamValues data() const { return v.data(); }
};

struct Stereo
{
    std::vector<float> l, r;
    size_t size() const { return l.size(); }
};

Stereo dual (const std::vector<float>& x) { return { x, x }; }

std::unique_ptr<ClassicComp> makeComp (double sr, const Values& vals, int maxBlock = 512)
{
    auto m = std::make_unique<ClassicComp>();
    m->prepare (sr, maxBlock);
    m->setParams (vals.data());
    return m;
}

using BlockHook = std::function<void (size_t, Values&, ClassicComp&)>;

Stereo run (ClassicComp& m, Values& vals, const Stereo& in, int block = 512, ChannelMode mode = ChannelMode::Stereo,
            const BlockHook& onBlock = {})
{
    Stereo out = in;
    ProcessContext ctx;
    ctx.channelMode = mode;
    for (size_t start = 0; start < in.size(); start += (size_t) block)
    {
        if (onBlock)
            onBlock (start, vals, m);
        m.setParams (vals.data());
        const int n = (int) std::min ((size_t) block, in.size() - start);
        float* ch[2] { out.l.data() + start, out.r.data() + start };
        m.process (ch, n, ctx);
    }
    return out;
}

double db (double x) { return pstest::dbOf (x); }

double toneDb (const std::vector<float>& y, size_t from, size_t len, double hz, double sr)
{
    return db (std::abs (pstest::toneOf (y, from, len, hz, sr)));
}

double rmsDb (const std::vector<float>& x, size_t from, size_t len) { return db (pstest::rmsOf (x, from, len)); }

double maxSecondDiff (const std::vector<float>& y, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = std::max<size_t> (from, 2); i < to && i < y.size(); ++i)
        m = std::max (m, std::abs ((double) y[i] - 2.0 * (double) y[i - 1] + (double) y[i - 2]));
    return m;
}

ClassicCompSetParams setParamsOf (float thr, float ratio, ClassicCompMode mode)
{
    ClassicCompSetParams p;
    p.thresholdDb = thr;
    p.ratio = ratio;
    p.mode = mode;
    return p;
}

// Reduction meter of set 0 after every `block` samples while `in` runs.
std::vector<float> reductionTrace (ClassicComp& m, Values& vals, const Stereo& in, int block, int set = 0,
                                   ChannelMode mode = ChannelMode::Stereo)
{
    std::vector<float> trace;
    run (m, vals, in, block, mode, [&] (size_t start, Values&, ClassicComp& c) {
        if (start > 0)
            trace.push_back (c.getMeters().gainReductionDb[set].load());
    });
    trace.push_back (m.getMeters().gainReductionDb[set].load());
    return trace;
}

// First index from `from` on where pred (trace[i]) holds; trace.size() if never.
size_t firstWhere (const std::vector<float>& trace, size_t from, const std::function<bool (float)>& pred)
{
    for (size_t i = from; i < trace.size(); ++i)
        if (pred (trace[i]))
            return i;
    return trace.size();
}

const char* modeName (int m) { return m == 0 ? "Sharp" : (m == 1 ? "Balanced" : "Smooth"); }

// The documented knee widths (ClassicComp.h): Sharp 2, Balanced 6, Smooth 12 dB.
double kKneeDbOf (int m) { return m == 0 ? 2.0 : (m == 1 ? 6.0 : 12.0); }
} // namespace

TEST_CASE ("ccmp. module type and contract registration")
{
    ClassicComp m;
    CHECK (m.getType() == ModuleType::ClassicComp);
    CHECK_EQ (std::string (moduleInfo (ModuleType::ClassicComp).idPrefix), std::string ("ccmp"));
}

TEST_CASE ("ccmp. neutral settings are an exact passthrough (every mode, any sidechain EQ, Stereo and M/S)")
{
    const Stereo in { pstest::added (pstest::pinkNoise (0.9, 3.0, kSr, 11u), pstest::sine (90.0, -3.0, 3.0, kSr)),
                      pstest::added (pstest::pinkNoise (0.9, 3.0, kSr, 12u), pstest::sine (3000.0, -6.0, 3.0, kSr)) };
    pstest::Lcg lcg (5u);
    double worst = 0.0;
    for (int mode = 0; mode < 3; ++mode)
        for (int ms = 0; ms < 2; ++ms)
        {
            Values vals;
            vals.neutral();
            vals.setBoth ("mode", (float) mode);
            vals.setBoth ("thr", -40.0f);
            vals.setBoth ("attack", 0.1f);
            vals.setBoth ("sc.hpf", 20.0f + 240.0f * (lcg.next() + 1.0f));
            vals.setBoth ("sc.bellDb", 8.0f * lcg.next());
            vals.setBoth ("sc.shelfDb", 8.0f * lcg.next());
            auto m = makeComp (kSr, vals);
            const auto out = run (*m, vals, in, 256, ms ? ChannelMode::MidSide : ChannelMode::Stereo);
            for (size_t i = 0; i < in.size(); ++i)
                worst = std::max ({ worst, std::abs ((double) out.l[i] - in.l[i]), std::abs ((double) out.r[i] - in.r[i]) });
            CHECK (m->getMeters().gainReductionDb[0].load() == 0.0f);
        }
    note ("largest difference to the input over every mode, Stereo and M/S (dB)", db (worst));
    CHECK (worst == 0.0);

    // returning to ratio 1 after heavy compression ends in the exact passthrough again
    Values vals;
    vals.neutral();
    vals.setBoth ("ratio", 10.0f);
    vals.setBoth ("thr", -30.0f);
    vals.setBoth ("mode", 2.0f);
    vals.setBoth ("release", 30.0f);
    auto m = makeComp (kSr, vals);
    run (*m, vals, in, 512);
    vals.setBoth ("ratio", 1.0f);
    run (*m, vals, in, 512);   // ratio glides to 1, the reduction releases to 0
    const auto out = run (*m, vals, in, 512);
    double diff = 0.0;
    for (size_t i = 0; i < in.size(); ++i)
        diff = std::max (diff, std::abs ((double) out.l[i] - in.l[i]));
    note ("after a ratio 10 -> 1 move: largest difference (dB)", db (diff));
    CHECK (diff == 0.0);
}

TEST_CASE ("ccmp. latency is 0 and equals the measured impulse delay at 44.1 / 48 / 96 kHz")
{
    const double rates[] { 44100.0, 48000.0, 96000.0 };
    for (const double sr : rates)
    {
        for (int active = 0; active < 2; ++active)
        {
            Values vals;
            if (active == 0)
                vals.neutral();
            else
            {
                vals.setBoth ("thr", -30.0f);
                vals.setBoth ("ratio", 8.0f);
                vals.setBoth ("attack", 0.1f);
                vals.setBoth ("mode", 2.0f);
            }
            auto m = makeComp (sr, vals);
            const int lat = m->getLatencySamples();
            Stereo in { std::vector<float> ((size_t) sr / 2, 0.0f), std::vector<float> ((size_t) sr / 2, 0.0f) };
            in.l[1000] = in.r[1000] = 0.5f;
            const auto out = run (*m, vals, in, 256);
            size_t arg = 0;
            for (size_t i = 0; i < out.size(); ++i)
                if (std::abs (out.l[i]) > std::abs (out.l[arg]))
                    arg = i;
            const int measured = (int) arg - 1000;
            note (std::string (active ? "active  " : "neutral ") + "latency @" + std::to_string ((int) sr) + " reported", lat);
            note ("    measured impulse delay", measured);
            CHECK_EQ (lat, 0);
            CHECK_EQ (measured, 0);

            // under compression: noise in vs out correlates best at lag 0
            if (active == 1)
            {
                const auto x = pstest::pinkNoise (0.7, 1.0, sr, 3u);
                const auto y = run (*m, vals, dual (x), 256);
                int best = -99;
                double bestC = -1.0;
                for (int lag = -8; lag <= 8; ++lag)
                {
                    double c = 0.0;
                    for (size_t i = (size_t) (0.3 * sr); i < x.size() - 16; ++i)
                        c += (double) y.l[i] * (double) x[(size_t) ((long long) i - lag)];
                    if (c > bestC)
                    {
                        bestC = c;
                        best = lag;
                    }
                }
                note ("    compressed noise: best correlation lag", best);
                CHECK_EQ (best, 0);
            }
        }
    }

    // latency never moves with any parameter
    Values vals;
    auto m = makeComp (kSr, vals);
    pstest::Lcg lcg (77u);
    bool constant = true;
    for (int k = 0; k < 300; ++k)
    {
        for (const auto& s : paramSpecs())
            if (s.id.rfind ("ccmp.", 0) == 0)
                vals.v[(size_t) paramIndex (s.id)] = s.minValue + 0.5f * (lcg.next() + 1.0f) * (s.maxValue - s.minValue);
        m->setParams (vals.data());
        constant = constant && m->getLatencySamples() == 0;
    }
    CHECK (constant);
}

TEST_CASE ("ccmp. steady tones follow staticCurveDb: feedback ratio mapping, threshold, knee per mode")
{
    const double ratios[] { 1.1, 2.0, 4.0, 10.0, 20.0 };
    double worstErr = 0.0;
    for (int mode = 0; mode < 3; ++mode)
        for (const double ratio : ratios)
        {
            const float thr = -20.0f;
            double outAt[2] {};
            for (int lv = -40; lv <= 0; lv += 4)
            {
                Values vals;
                vals.neutral();
                vals.setBoth ("thr", thr);
                vals.setBoth ("ratio", (float) ratio);
                vals.setBoth ("mode", (float) mode);
                auto m = makeComp (kSr, vals);
                const auto x = pstest::sine (1000.0, (double) lv, 1.5, kSr);
                const auto out = run (*m, vals, dual (x), 512);
                const double measured = toneDb (out.l, (size_t) kSr, (size_t) (0.5 * kSr), 1000.0, kSr);
                const double expected = ClassicComp::staticCurveDb (setParamsOf (thr, (float) ratio, (ClassicCompMode) mode),
                                                                    (double) lv);
                worstErr = std::max (worstErr, std::abs (measured - expected));
                CHECK_NEAR (measured, expected, 0.3);
                if (lv == -8)
                    outAt[0] = measured;
                if (lv == 0)
                    outAt[1] = measured;
            }
            // above the knee (it ends W (1 + c / 2) - W / 2 above T on the input side): 8 dB more input -> 8 / ratio
            // dB more output
            const double kneeTop = (double) thr - 0.5 * kKneeDbOf (mode) + kKneeDbOf (mode) * (1.0 + 0.5 * (ratio - 1.0));
            if (kneeTop <= -8.0)
            {
                const double rise = outAt[1] - outAt[0];
                note (std::string (modeName (mode)) + " ratio " + std::to_string (ratio).substr (0, 4)
                          + ": output rise for 8 dB more input above the knee (dB)", rise);
                CHECK_NEAR (rise, 8.0 / ratio, 0.05);
            }
        }
    note ("worst difference to staticCurveDb over modes, ratios 1.1..20, inputs -40..0 dBFS (dB)", worstErr);

    // knee per mode at the threshold itself (ratio 4): Sharp 2, Balanced 6, Smooth 12 dB wide
    double prev = 1.0;
    for (int mode = 0; mode < 3; ++mode)
    {
        const double r = ClassicComp::staticCurveDb (setParamsOf (-20.0f, 4.0f, (ClassicCompMode) mode), -20.0) + 20.0;
        note (std::string (modeName (mode)) + ": reduction of a tone at the threshold (dB)", r);
        CHECK (r < prev);
        prev = r;
    }
    // a tone W/2 below the threshold is untouched, ratio 1 is the identity
    CHECK_EQ (ClassicComp::staticCurveDb (setParamsOf (-20.0f, 4.0f, ClassicCompMode::Smooth), -26.0), -26.0);
    CHECK_EQ (ClassicComp::staticCurveDb (setParamsOf (-20.0f, 1.0f, ClassicCompMode::Sharp), -3.0), -3.0);
    // the curve is continuous and monotonic
    bool mono = true;
    for (int mode = 0; mode < 3; ++mode)
    {
        double last = -1.0e9;
        for (double x = -60.0; x <= 0.0; x += 0.01)
        {
            const double y = ClassicComp::staticCurveDb (setParamsOf (-24.0f, 6.0f, (ClassicCompMode) mode), x);
            mono = mono && y > last && (last < -1.0e8 || y - last < 0.0101);
            last = y;
        }
    }
    CHECK (mono);
}

TEST_CASE ("ccmp. attack: closed-loop time follows the knob, scaled per mode")
{
    // 1 kHz: 0.3 s at -40 dBFS, then a step to -6 dBFS (threshold -30, ratio 4)
    const auto x = pstest::sineSequence (1000.0, { { -40.0, 0.3 }, { -6.0, 0.7 } }, kSr);
    const int block = 16;
    const size_t stepAt = (size_t) (0.3 * kSr) / block;
    auto t63 = [&] (float attackMs, int mode) {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -30.0f);
        vals.setBoth ("ratio", 4.0f);
        vals.setBoth ("attack", attackMs);
        vals.setBoth ("release", 1000.0f);
        vals.setBoth ("mode", (float) mode);
        auto m = makeComp (kSr, vals);
        const auto tr = reductionTrace (*m, vals, dual (x), block);
        const float final = tr.back();
        const size_t i = firstWhere (tr, stepAt, [&] (float r) { return r <= 0.632f * final; });
        return (double) ((i - stepAt) * (size_t) block) / kSr * 1000.0;
    };
    const double a1 = t63 (1.0f, 1), a10 = t63 (10.0f, 1), a50 = t63 (50.0f, 1);
    const double s10 = t63 (10.0f, 0), m10 = t63 (10.0f, 2);
    note ("Balanced: 63 % of the reduction after (ms), attack 1", a1);
    note ("    attack 10", a10);
    note ("    attack 50", a50);
    note ("attack 10: Sharp (ms)", s10);
    note ("    Smooth (ms)", m10);
    CHECK (a1 < a10 && a10 < a50);
    CHECK (a10 > 5.0 && a10 < 20.0);
    CHECK (a50 > 25.0 && a50 < 90.0);
    CHECK (s10 > a10 * 1.2);
    CHECK (m10 < a10 * 0.85);

    // the closed-loop compensation keeps the attack time nearly independent of the ratio
    auto t63r = [&] (float ratio) {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -30.0f);
        vals.setBoth ("ratio", ratio);
        vals.setBoth ("attack", 10.0f);
        vals.setBoth ("release", 1000.0f);
        vals.setBoth ("mode", 0.0f);
        auto m = makeComp (kSr, vals);
        const auto tr = reductionTrace (*m, vals, dual (x), block);
        const size_t i = firstWhere (tr, stepAt, [&] (float r) { return r <= 0.632f * tr.back(); });
        return (double) ((i - stepAt) * (size_t) block) / kSr * 1000.0;
    };
    const double r2 = t63r (2.0f), r20 = t63r (20.0f);
    note ("Sharp attack 10: ratio 2 (ms)", r2);
    note ("    ratio 20 (ms)", r20);
    CHECK (r20 / r2 > 0.5 && r20 / r2 < 2.0);
}

TEST_CASE ("ccmp. release: a short hit recovers at the knob, sustained reduction releases slower")
{
    const int block = 16;
    auto releaseMs = [&] (double burstSeconds, float releaseKnob) {
        const auto x = pstest::sineSequence (1000.0, { { -6.0, burstSeconds }, { -200.0, 3.0 } }, kSr);
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -30.0f);
        vals.setBoth ("ratio", 4.0f);
        vals.setBoth ("attack", 1.0f);
        vals.setBoth ("release", releaseKnob);
        vals.setBoth ("mode", 0.0f);
        auto m = makeComp (kSr, vals);
        const auto tr = reductionTrace (*m, vals, dual (x), block);
        const size_t endAt = (size_t) (burstSeconds * kSr) / block - 1;
        const float r0 = tr[endAt];
        const size_t i = firstWhere (tr, endAt, [&] (float r) { return r >= r0 / 2.718281828f; });
        return (double) ((i - endAt) * (size_t) block) / kSr * 1000.0;
    };
    const double short200 = releaseMs (0.03, 200.0f), long200 = releaseMs (3.0, 200.0f);
    const double short100 = releaseMs (0.03, 100.0f), short800 = releaseMs (0.03, 800.0f);
    note ("release 200: after a 30 ms hit, time to 1/e of the reduction (ms)", short200);
    note ("    after 3 s of compression (ms)", long200);
    note ("release 100 after a hit (ms)", short100);
    note ("release 800 after a hit (ms)", short800);
    CHECK (short200 > 180.0 && short200 < 400.0);
    CHECK (long200 > 2.0 * short200);
    CHECK (long200 < 4.5 * 200.0 + 100.0);
    CHECK (short100 < short200 && short200 < short800);
    CHECK (short800 / short200 > 3.0);
}

TEST_CASE ("ccmp. mode colour: Sharp clean, Balanced a little, Smooth more; linear at no reduction and below -30 dBFS")
{
    const auto x = pstest::sine (1000.0, 0.0, 2.0, kSr);
    const size_t from = (size_t) kSr, len = (size_t) kSr;
    double h2[3] {}, h3[3] {};
    for (int mode = 0; mode < 3; ++mode)
    {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -6.0f);
        vals.setBoth ("ratio", 4.0f);
        vals.setBoth ("mode", (float) mode);
        vals.setBoth ("attack", 30.0f);
        vals.setBoth ("release", 300.0f);
        auto m = makeComp (kSr, vals);
        const auto out = run (*m, vals, dual (x), 512);
        const double f = toneDb (out.l, from, len, 1000.0, kSr);
        h2[mode] = toneDb (out.l, from, len, 2000.0, kSr) - f;
        h3[mode] = toneDb (out.l, from, len, 3000.0, kSr) - f;
        note (std::string (modeName (mode)) + " (0 dBFS in, thr -6, ratio 4): reduction (dB)", m->getMeters().gainReductionDb[0].load());
        note ("    H2 (dBc)", h2[mode]);
        note ("    H3 (dBc)", h3[mode]);
    }
    CHECK (h2[0] < -90.0 && h3[0] < -90.0);
    CHECK (h2[1] > -80.0 && h2[1] < -50.0 && h3[1] > -80.0 && h3[1] < -50.0);
    CHECK (h2[2] > -60.0 && h2[2] < -40.0 && h3[2] > -60.0 && h3[2] < -40.0);
    CHECK (h2[2] > h2[1] + 6.0 && h3[2] > h3[1] + 6.0);

    // no reduction (threshold 0, -8 dBFS sine): Smooth is exactly linear
    {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", 0.0f);
        vals.setBoth ("ratio", 20.0f);
        vals.setBoth ("mode", 2.0f);
        auto m = makeComp (kSr, vals);
        const auto xin = pstest::sine (1000.0, -8.0, 1.0, kSr);
        const auto out = run (*m, vals, dual (xin), 512);
        double diff = 0.0;
        for (size_t i = 0; i < xin.size(); ++i)
            diff = std::max (diff, std::abs ((double) out.l[i] - xin[i]));
        note ("Smooth, no reduction: largest difference to the input", diff);
        CHECK (diff == 0.0);
    }
    // 14 dB of reduction, but the output stays under -30 dBFS: no saturation
    {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -40.0f);
        vals.setBoth ("ratio", 4.0f);
        vals.setBoth ("mode", 2.0f);
        vals.setBoth ("attack", 30.0f);
        vals.setBoth ("release", 300.0f);
        auto m = makeComp (kSr, vals);
        const auto out = run (*m, vals, dual (pstest::sine (1000.0, -20.0, 2.0, kSr)), 512);
        const double f = toneDb (out.l, from, len, 1000.0, kSr);
        const double s2 = toneDb (out.l, from, len, 2000.0, kSr) - f, s3 = toneDb (out.l, from, len, 3000.0, kSr) - f;
        note ("Smooth, output at (dBFS)", f);
        note ("    reduction (dB)", m->getMeters().gainReductionDb[0].load());
        note ("    H2 / H3 (dBc)", std::max (s2, s3));
        CHECK (f < -30.0);
        CHECK (s2 < -90.0 && s3 < -90.0);
    }
}

TEST_CASE ("ccmp. Stereo links both channels; M/S compresses each set on its own")
{
    // Stereo: a loud left channel turns the quiet right channel down by the same gain
    const auto loud = pstest::sine (1000.0, -6.0, 1.5, kSr);
    const auto quiet = pstest::sine (300.0, -40.0, 1.5, kSr);
    const size_t from = (size_t) kSr, len = (size_t) (0.5 * kSr);
    {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -30.0f);
        vals.setBoth ("ratio", 4.0f);
        auto m = makeComp (kSr, vals);
        const auto out = run (*m, vals, { loud, quiet });
        const double gl = toneDb (out.l, from, len, 1000.0, kSr) - toneDb (loud, from, len, 1000.0, kSr);
        const double gr = toneDb (out.r, from, len, 300.0, kSr) - toneDb (quiet, from, len, 300.0, kSr);
        note ("Stereo: left (loud) gain (dB)", gl);
        note ("    right (quiet) gain (dB)", gr);
        const double expected = ClassicComp::staticCurveDb (setParamsOf (-30.0f, 4.0f, ClassicCompMode::Balanced), -6.0) + 6.0;
        note ("    expected from staticCurveDb (dB)", expected);
        CHECK_NEAR (gl, expected, 0.3);
        CHECK_NEAR (gl, gr, 0.05);
        CHECK (m->getMeters().gainReductionDb[1].load() == 0.0f);
        CHECK (m->getMeters().sidechainDb[1].load() <= -99.0f);
    }
    // M/S: set A on ch0 (Mid), set B on ch1 (Side); the quiet side is untouched by the loud mid
    {
        Values vals;
        vals.neutral();
        vals.set ("a.thr", -30.0f);
        vals.set ("a.ratio", 4.0f);
        vals.set ("b.thr", -20.0f);
        vals.set ("b.ratio", 4.0f);
        auto m = makeComp (kSr, vals);
        const auto out = run (*m, vals, { loud, quiet }, 512, ChannelMode::MidSide);
        double diff = 0.0;
        for (size_t i = 0; i < quiet.size(); ++i)
            diff = std::max (diff, std::abs ((double) out.r[i] - quiet[i]));
        const double gm = toneDb (out.l, from, len, 1000.0, kSr) - toneDb (loud, from, len, 1000.0, kSr);
        note ("M/S: mid gain (dB)", gm);
        note ("    side: largest difference to its input", diff);
        CHECK (gm < -10.0);
        CHECK (diff == 0.0);
        CHECK (m->getMeters().gainReductionDb[0].load() < -10.0f);
        CHECK (m->getMeters().gainReductionDb[1].load() == 0.0f);

        // set B with a low threshold compresses the side alone, the mid keeps set A's amount
        vals.set ("b.thr", -50.0f);
        vals.set ("b.ratio", 10.0f);
        auto m2 = makeComp (kSr, vals);
        const auto out2 = run (*m2, vals, { loud, quiet }, 512, ChannelMode::MidSide);
        const double gs = toneDb (out2.r, from, len, 300.0, kSr) - toneDb (quiet, from, len, 300.0, kSr);
        const double gm2 = toneDb (out2.l, from, len, 1000.0, kSr) - toneDb (loud, from, len, 1000.0, kSr);
        const double expectedSide = ClassicComp::staticCurveDb (setParamsOf (-50.0f, 10.0f, ClassicCompMode::Balanced), -40.0) + 40.0;
        note ("M/S set B thr -50 ratio 10: side gain (dB)", gs);
        note ("    expected from staticCurveDb (dB)", expectedSide);
        note ("    mid gain (dB)", gm2);
        CHECK_NEAR (gs, expectedSide, 0.4);
        CHECK_NEAR (gm2, gm, 0.01);
        CHECK (m2->getMeters().gainReductionDb[1].load() < -5.0f);
    }
}

TEST_CASE ("ccmp. sidechain EQ: the listen output follows detectorResponseDb and the EQ steers the reduction")
{
    ClassicCompSetParams sc;
    sc.scHighPassHz = 200.0f;
    sc.scBellHz = 2000.0f;
    sc.scBellDb = 6.0f;
    sc.scShelfHz = 8000.0f;
    sc.scShelfDb = -6.0f;
    const double freqs[] { 40.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 8000.0, 12000.0, 16000.0 };
    for (const double sr : { 44100.0, 96000.0 })
    {
        Values vals;
        vals.setBoth ("sc.hpf", sc.scHighPassHz);
        vals.setBoth ("sc.bellHz", sc.scBellHz);
        vals.setBoth ("sc.bellDb", sc.scBellDb);
        vals.setBoth ("sc.shelfHz", sc.scShelfHz);
        vals.setBoth ("sc.shelfDb", sc.scShelfDb);
        double worst = 0.0;
        for (const double hz : freqs)
        {
            auto m = makeComp (sr, vals);
            ModuleMonitor mon;
            mon.soloDetectionFilter = true;
            m->setMonitor (mon);
            const auto x = pstest::sine (hz, -20.0, 1.0, sr);
            const auto out = run (*m, vals, dual (x));
            const size_t from = (size_t) (0.5 * sr), len = (size_t) (0.5 * sr);
            const double measured = toneDb (out.l, from, len, hz, sr) - toneDb (x, from, len, hz, sr);
            const double expected = ClassicComp::detectorResponseDb (sc, hz);
            worst = std::max (worst, std::abs (measured - expected));
            CHECK_NEAR (measured, expected, 0.25);
        }
        note ("listen output vs detectorResponseDb, 40 Hz .. 16 kHz @" + std::to_string ((int) sr) + ": worst (dB)", worst);
    }
    // defaults: flat in the band, the 20 Hz high-pass -3 dB at its corner
    const ClassicCompSetParams def;
    CHECK_NEAR (ClassicComp::detectorResponseDb (def, 1000.0), 0.0, 0.01);
    CHECK_NEAR (ClassicComp::detectorResponseDb (def, 20.0), -3.01, 0.02);

    // the high-pass keeps a loud 60 Hz tone from driving the compressor
    auto reductionAt = [&] (double hz, const std::function<void (Values&)>& setup) {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -30.0f);
        vals.setBoth ("ratio", 4.0f);
        setup (vals);
        auto m = makeComp (kSr, vals);
        run (*m, vals, dual (pstest::sine (hz, -10.0, 1.5, kSr)));
        return (double) m->getMeters().gainReductionDb[0].load();
    };
    const double hp20 = reductionAt (60.0, [] (Values&) {});
    const double hp500 = reductionAt (60.0, [] (Values& v) { v.setBoth ("sc.hpf", 500.0f); });
    note ("60 Hz at -10 dBFS: reduction with sc.hpf 20 Hz (dB)", hp20);
    note ("    with sc.hpf 500 Hz (dB)", hp500);
    CHECK (hp500 > hp20 + 8.0);
    // a bell boost on the detector at 1 kHz compresses a 1 kHz tone harder, a cut lighter
    const double flat = reductionAt (1000.0, [] (Values&) {});
    const double up = reductionAt (1000.0, [] (Values& v) { v.setBoth ("sc.bellDb", 8.0f); });
    const double down = reductionAt (1000.0, [] (Values& v) { v.setBoth ("sc.bellDb", -8.0f); });
    note ("1 kHz: reduction flat / bell +8 / bell -8 (dB)", flat);
    note ("    bell +8", up);
    note ("    bell -8", down);
    CHECK_NEAR (up - flat, -8.0 * 3.0 / 4.0, 0.5);   // above the knee the reduction moves by (ratio - 1) / ratio
    CHECK_NEAR (down - flat, 8.0 * 3.0 / 4.0, 0.5);
    const double shelf = reductionAt (10000.0, [] (Values& v) { v.setBoth ("sc.shelfHz", 4000.0f); v.setBoth ("sc.shelfDb", 8.0f); });
    const double shelfFlat = reductionAt (10000.0, [] (Values&) {});
    note ("10 kHz: reduction with a +8 dB shelf at 4 kHz (dB)", shelf);
    CHECK (shelf < shelfFlat - 5.0);
}

TEST_CASE ("ccmp. auto gain levels the output to the input; makeup stays on top; frozen in silence; clamped")
{
    const auto noise = pstest::pinkNoise (0.5, 10.0, kSr, 21u);
    const size_t from = (size_t) (8.0 * kSr), len = (size_t) (2.0 * kSr);
    const double inDb = rmsDb (noise, from, len);
    auto runWith = [&] (float makeup, bool autoGain, std::unique_ptr<ClassicComp>& keep, Values& vals) {
        vals = Values();
        vals.setBoth ("thr", -30.0f);
        vals.setBoth ("ratio", 4.0f);
        vals.setBoth ("makeup", makeup);
        vals.setBoth ("autoGain", autoGain ? 1.0f : 0.0f);
        keep = makeComp (kSr, vals);
        return run (*keep, vals, dual (noise));
    };
    std::unique_ptr<ClassicComp> m;
    Values vals;
    const auto off = runWith (0.0f, false, m, vals);
    const auto on = runWith (0.0f, true, m, vals);
    const float agMeter = m->getMeters().autoGainDb[0].load();
    const auto onMakeup = runWith (6.0f, true, m, vals);
    note ("pink noise in (dBFS RMS)", inDb);
    note ("    compressed, auto gain off (dB vs input)", rmsDb (off.l, from, len) - inDb);
    note ("    auto gain on (dB vs input)", rmsDb (on.l, from, len) - inDb);
    note ("    auto gain meter (dB)", agMeter);
    note ("    auto gain on, makeup +6 (dB vs input)", rmsDb (onMakeup.l, from, len) - inDb);
    CHECK (rmsDb (off.l, from, len) - inDb < -6.0);
    CHECK_NEAR (rmsDb (on.l, from, len), inDb, 0.5);
    CHECK_NEAR (rmsDb (onMakeup.l, from, len) - inDb, 6.0, 0.5);
    CHECK (agMeter > 6.0f && agMeter < 12.0f);

    // silence: the correction is held, not chased toward +12
    const auto held = m->getMeters().autoGainDb[0].load();
    run (*m, vals, dual (std::vector<float> ((size_t) (3.0 * kSr), 0.0f)));
    note ("    after 3 s of silence (dB)", m->getMeters().autoGainDb[0].load());
    CHECK_NEAR (m->getMeters().autoGainDb[0].load(), held, 0.01);

    // clamped at +12 dB
    Values hard;
    hard.setBoth ("thr", -40.0f);
    hard.setBoth ("ratio", 20.0f);
    hard.setBoth ("autoGain", 1.0f);
    auto mh = makeComp (kSr, hard);
    run (*mh, hard, dual (noise));
    note ("thr -40, ratio 20: auto gain (dB)", mh->getMeters().autoGainDb[0].load());
    CHECK_NEAR (mh->getMeters().autoGainDb[0].load(), 12.0, 0.01);

    // switching auto gain off returns to exactly 0 dB
    hard.setBoth ("autoGain", 0.0f);
    run (*mh, hard, dual (std::vector<float> ((size_t) (3.0 * kSr), 0.0f)));
    CHECK (mh->getMeters().autoGainDb[0].load() == 0.0f);
}

TEST_CASE ("ccmp. meters: reduction, 1 s hold, sidechain level")
{
    // sidechain level reads a sine's peak level
    {
        Values vals;
        vals.neutral();
        auto m = makeComp (kSr, vals);
        run (*m, vals, dual (pstest::sine (1000.0, -18.0, 0.5, kSr)));
        note ("sidechain meter of a -18 dBFS sine (dBFS)", m->getMeters().sidechainDb[0].load());
        CHECK_NEAR (m->getMeters().sidechainDb[0].load(), -18.0, 0.3);
    }
    // a 100 ms hit: the hold keeps its depth for ~1 s while the current reduction releases
    Values vals;
    vals.neutral();
    vals.setBoth ("thr", -30.0f);
    vals.setBoth ("ratio", 4.0f);
    vals.setBoth ("attack", 1.0f);
    vals.setBoth ("release", 50.0f);
    auto m = makeComp (kSr, vals);
    const auto x = pstest::sineSequence (1000.0, { { -6.0, 0.1 }, { -200.0, 2.0 } }, kSr);
    std::vector<float> cur, hold;
    run (*m, vals, dual (x), 480, ChannelMode::Stereo, [&] (size_t start, Values&, ClassicComp& c) {
        if (start > 0)
        {
            cur.push_back (c.getMeters().gainReductionDb[0].load());
            hold.push_back (c.getMeters().gainReductionHoldDb[0].load());
        }
    });
    // blocks of 10 ms: the hit ends at block 10
    note ("reduction at the end of the hit (dB)", cur[9]);
    note ("    0.5 s later: current / held (dB)", cur[59]);
    note ("    held", hold[59]);
    note ("    1.5 s later: held (dB)", hold[159]);
    CHECK (cur[9] < -15.0f);
    CHECK (cur[59] > -0.5f);
    CHECK_NEAR (hold[59], cur[9], 1.5);
    CHECK (hold[159] > -0.5f);
}

TEST_CASE ("ccmp. parameter changes are click-free")
{
    struct Move
    {
        const char* name;
        std::function<void (Values&)> before, after;
        double inputDb = -6.0;
        int solo = -1;   // 0 / 1: set the detector solo to this at the switch
    };
    auto base = [] (Values& v) {
        v.neutral();
        v.setBoth ("thr", -24.0f);
        v.setBoth ("ratio", 4.0f);
    };
    const std::vector<Move> moves {
        { "threshold 0 -> -40 jump", [&] (Values& v) { base (v); v.setBoth ("thr", 0.0f); },
          [&] (Values& v) { base (v); v.setBoth ("thr", -40.0f); } },
        { "threshold -40 -> 0 jump", [&] (Values& v) { base (v); v.setBoth ("thr", -40.0f); },
          [&] (Values& v) { base (v); v.setBoth ("thr", 0.0f); } },
        { "ratio 1 -> 20 jump", [&] (Values& v) { base (v); v.setBoth ("ratio", 1.0f); },
          [&] (Values& v) { base (v); v.setBoth ("ratio", 20.0f); } },
        { "mode Sharp -> Smooth", [&] (Values& v) { base (v); v.setBoth ("mode", 0.0f); },
          [&] (Values& v) { base (v); v.setBoth ("mode", 2.0f); } },
        { "mode Smooth -> Sharp", [&] (Values& v) { base (v); v.setBoth ("mode", 2.0f); },
          [&] (Values& v) { base (v); v.setBoth ("mode", 0.0f); } },
        { "makeup -12 -> +24 jump", [&] (Values& v) { base (v); v.setBoth ("makeup", -12.0f); },
          [&] (Values& v) { base (v); v.setBoth ("makeup", 24.0f); }, -24.0 },
        { "attack 100 -> 0.1 jump", [&] (Values& v) { base (v); v.setBoth ("attack", 100.0f); },
          [&] (Values& v) { base (v); v.setBoth ("attack", 0.1f); } },
        { "release 3000 -> 30 jump", [&] (Values& v) { base (v); v.setBoth ("release", 3000.0f); },
          [&] (Values& v) { base (v); v.setBoth ("release", 30.0f); } },
        { "sidechain high-pass 20 -> 500 Hz", [&] (Values& v) { base (v); },
          [&] (Values& v) { base (v); v.setBoth ("sc.hpf", 500.0f); } },
        { "sidechain bell -8 -> +8 dB", [&] (Values& v) { base (v); v.setBoth ("sc.bellHz", 200.0f); v.setBoth ("sc.bellDb", -8.0f); },
          [&] (Values& v) { base (v); v.setBoth ("sc.bellHz", 200.0f); v.setBoth ("sc.bellDb", 8.0f); } },
        { "sidechain shelf 2 kHz -8 -> 16 kHz +8", [&] (Values& v) { base (v); v.setBoth ("sc.shelfHz", 2000.0f); v.setBoth ("sc.shelfDb", -8.0f); },
          [&] (Values& v) { base (v); v.setBoth ("sc.shelfHz", 16000.0f); v.setBoth ("sc.shelfDb", 8.0f); } },
        { "auto gain on", [&] (Values& v) { base (v); },
          [&] (Values& v) { base (v); v.setBoth ("autoGain", 1.0f); } },
        { "auto gain off", [&] (Values& v) { base (v); v.setBoth ("autoGain", 1.0f); },
          [&] (Values& v) { base (v); } },
        { "detector solo on", [&] (Values& v) { base (v); }, [&] (Values& v) { base (v); }, -6.0, 1 },
    };

    const auto tSwitch = (size_t) (3.0 * kSr);
    for (const auto& mv : moves)
    {
        const auto x = pstest::sine (100.0, mv.inputDb, 6.0, kSr);
        Values vals;
        mv.before (vals);
        auto m = makeComp (kSr, vals);
        bool switched = false;
        const auto out = run (*m, vals, dual (x), 64, ChannelMode::Stereo, [&] (size_t start, Values& v, ClassicComp& c) {
            if (start < tSwitch || switched)
                return;
            switched = true;
            v = Values();
            mv.after (v);
            if (mv.solo >= 0)
            {
                ModuleMonitor mon;
                mon.soloDetectionFilter = mv.solo == 1;
                c.setMonitor (mon);
            }
        });
        const double steadyBefore = maxSecondDiff (out.l, (size_t) (2.0 * kSr), tSwitch);
        const double steadyAfter = maxSecondDiff (out.l, (size_t) (5.0 * kSr), out.size());
        const double during = maxSecondDiff (out.l, tSwitch, tSwitch + (size_t) (1.0 * kSr));
        const double ratio = during / std::max (steadyBefore, steadyAfter);
        note (std::string (mv.name) + ": transition / steady second difference", ratio);
        CHECK (ratio <= 2.0);
    }
}

TEST_CASE ("ccmp. stable, finite and denormal-safe under extreme automation (Stereo and M/S)")
{
    for (int ms = 0; ms < 2; ++ms)
    {
        Values vals;
        auto m = makeComp (kSr, vals);
        pstest::Lcg lcg (99u + (unsigned) ms);
        const Stereo in { pstest::added (pstest::pinkNoise (0.6, 8.0, kSr, 41u), pstest::sine (50.0, -3.0, 8.0, kSr)),
                          pstest::pinkNoise (0.6, 8.0, kSr, 42u) };
        const auto out = run (*m, vals, in, 64, ms ? ChannelMode::MidSide : ChannelMode::Stereo,
                              [&] (size_t, Values& v, ClassicComp& c) {
                                  for (const auto& s : paramSpecs())
                                  {
                                      if (s.id.rfind ("ccmp.", 0) != 0)
                                          continue;
                                      const float u = lcg.next() > 0.0f ? 1.0f : 0.0f;   // extremes only
                                      v.v[(size_t) paramIndex (s.id)] = s.minValue + u * (s.maxValue - s.minValue);
                                  }
                                  ModuleMonitor mon;
                                  mon.soloDetectionFilter = lcg.next() > 0.8f;
                                  c.setMonitor (mon);
                              });
        bool finite = true;
        double pk = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            finite = finite && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
            pk = std::max (pk, (double) std::max (std::abs (out.l[i]), std::abs (out.r[i])));
        }
        note (std::string (ms ? "M/S" : "Stereo") + ": random extremes every 64 samples, output peak (dBFS)", db (pk));
        CHECK (finite);
        CHECK (pk < pstest::dbfsToAmplitude (40.0));
    }

    // a loud +12 dBFS input at threshold -40 / ratio 20 / attack 0.1 (hardest loop): bounded, no oscillation
    {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -40.0f);
        vals.setBoth ("ratio", 20.0f);
        vals.setBoth ("attack", 0.1f);
        vals.setBoth ("release", 30.0f);
        vals.setBoth ("mode", 2.0f);
        auto m = makeComp (kSr, vals);
        const auto x = pstest::scaled (pstest::pinkNoise (1.0, 3.0, kSr, 8u), 4.0f);
        const auto out = run (*m, vals, dual (x), 128);
        double pk = 0.0;
        for (size_t i = (size_t) kSr; i < out.size(); ++i)
            pk = std::max (pk, (double) std::abs (out.l[i]));
        note ("+12 dBFS noise, thr -40, ratio 20, attack 0.1: output peak (dBFS)", db (pk));
        CHECK (pk < pstest::dbfsToAmplitude (0.0));
        // a steady tone settles to the static curve (no limit cycle)
        const auto tone = pstest::sine (1000.0, 6.0, 1.5, kSr);
        const auto y = run (*m, vals, dual (tone), 128);
        const double a = toneDb (y.l, (size_t) kSr, (size_t) (0.25 * kSr), 1000.0, kSr);
        const double b = toneDb (y.l, (size_t) (1.25 * kSr), (size_t) (0.25 * kSr), 1000.0, kSr);
        note ("    +6 dBFS tone: output (dBFS)", b);
        CHECK_NEAR (a, b, 0.05);
        CHECK_NEAR (b, ClassicComp::staticCurveDb (setParamsOf (-40.0f, 20.0f, ClassicCompMode::Smooth), 6.0), 0.5);
    }

    // silence after loud material: decays to exact zeros, with no subnormal output samples
    Values v2;
    v2.setBoth ("thr", -30.0f);
    v2.setBoth ("ratio", 6.0f);
    v2.setBoth ("mode", 2.0f);
    v2.setBoth ("autoGain", 1.0f);
    auto m2 = makeComp (kSr, v2);
    run (*m2, v2, dual (pstest::pinkNoise (0.8, 2.0, kSr, 7u)));
    const Stereo silence { std::vector<float> ((size_t) (20.0 * kSr), 0.0f), std::vector<float> ((size_t) (20.0 * kSr), 0.0f) };
    const auto quiet = run (*m2, v2, silence);
    double tail = 0.0;
    bool subnormal = false;
    for (size_t i = 0; i < quiet.size(); ++i)
    {
        subnormal = subnormal || std::fpclassify (quiet.l[i]) == FP_SUBNORMAL;
        if (i >= (size_t) (10.0 * kSr))
            tail = std::max (tail, (double) std::abs (quiet.l[i]));
    }
    note ("20 s of silence after noise: largest output sample after 10 s", tail);
    CHECK (tail == 0.0);
    CHECK (! subnormal);
}

TEST_CASE ("ccmp. no allocation in setParams / setMonitor / process / reset")
{
    Values vals;
    auto m = makeComp (kSr, vals);
    const Stereo in { pstest::pinkNoise (0.6, 2.0, kSr, 51u), pstest::pinkNoise (0.6, 2.0, kSr, 52u) };
    Stereo buf = in;
    // indices outside the scope (building the id strings would allocate)
    const char* names[] { "thr", "ratio", "mode", "sc.hpf", "sc.bellDb", "autoGain" };
    int idx[6][2] {};
    for (int n = 0; n < 6; ++n)
        for (int s = 0; s < 2; ++s)
            idx[n][s] = paramIndex (std::string ("ccmp.") + (s == 0 ? "a." : "b.") + names[n]);
    auto setBoth = [&] (int n, float x) { vals.v[(size_t) idx[n][0]] = vals.v[(size_t) idx[n][1]] = x; };
    long long count = 0;
    {
        pstest::AllocationScope scope;
        ProcessContext ctx;
        int k = 0;
        for (size_t start = 0; start + 512 <= buf.size(); start += 512, ++k)
        {
            setBoth (0, -40.0f + (float) (k % 9) * 5.0f);
            setBoth (1, 1.0f + (float) (k % 5) * 4.0f);
            setBoth (2, (float) (k % 3));
            setBoth (3, 20.0f + (float) (k % 7) * 60.0f);
            setBoth (4, (float) ((k % 9) - 4));
            setBoth (5, (float) ((k / 4) % 2));
            ctx.channelMode = (k / 6) % 2 ? ChannelMode::MidSide : ChannelMode::Stereo;
            ModuleMonitor mon;
            mon.soloDetectionFilter = (k / 5) % 2 == 1;
            m->setMonitor (mon);
            m->setParams (vals.data());
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

TEST_CASE ("ccmp. attack and release automation glides use elapsed samples in short blocks")
{
    for (const bool release : { false, true })
    {
        Values vals;
        vals.neutral();
        vals.setBoth ("mode", 0.0f);
        vals.setBoth ("thr", -24.0f);
        vals.setBoth ("ratio", 4.0f);
        vals.setBoth ("attack", 100.0f);
        vals.setBoth ("release", 3000.0f);
        const auto warm = dual (pstest::sine (1000.0, release ? -3.0 : -36.0, 1.0, kSr));
        const auto input = dual (pstest::sine (1000.0, release ? -36.0 : -3.0, 0.1, kSr));
        auto render = [&] (int block)
        {
            Values v = vals;
            auto m = makeComp (kSr, v);
            run (*m, v, warm);
            v.setBoth (release ? "release" : "attack", release ? 30.0f : 0.1f);
            return run (*m, v, input, block);
        };
        const auto reference = render (64);
        double worst = 0.0;
        for (const int block : { 1, 7, 31, 65, 127 })
        {
            const auto out = render (block);
            for (size_t i = 0; i < input.size(); ++i)
                if (std::abs (input.l[i]) > 0.005f)
                    worst = std::max (worst, std::abs (db (std::abs (out.l[i] / reference.l[i]))));
        }
        note (release ? "release glide: block-size gain difference (dB)" : "attack glide: block-size gain difference (dB)", worst);
        CHECK (worst < 1.0e-6);
    }
}

TEST_CASE ("ccmp. cpu benchmark (48 kHz stereo)")
{
    const Stereo in { pstest::pinkNoise (0.6, 20.0, kSr, 61u), pstest::pinkNoise (0.6, 20.0, kSr, 62u) };
    auto bench = [&] (Values& vals, ChannelMode mode, const char* name) {
        auto m = makeComp (kSr, vals);
        pstest::Stopwatch sw;
        run (*m, vals, in, 512, mode);
        const double x = 20.0 / sw.seconds();
        note (std::string (name) + ": x realtime", x);
        return x;
    };
    Values defaults;
    Values ms;
    ms.setBoth ("thr", -30.0f);
    ms.setBoth ("mode", 2.0f);
    bench (defaults, ChannelMode::Stereo, "defaults, Stereo");
    const double w = bench (ms, ChannelMode::MidSide, "Smooth, compressing, M/S");
    CHECK (w > 30.0);
}

TEST_CASE ("ccmp. corrupt samples are replaced without poisoning audio histories")
{
    for (double rate : { 44100.0, 96000.0 })
        for (int variant = 0; variant < 2; ++variant)
            for (int block : { 1, 17, 512 })
            {
                Values v;
                v.setBoth ("ratio", 4.0f);
                v.setBoth ("autoGain", 1.0f);
                auto m = makeComp (rate, v);
                auto reference = makeComp (rate, v);
                Stereo damaged = dual (pstest::sine (997.0, -14.0, 4096.0 / rate, rate));
                auto clean = damaged;
                const float bad[] { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max() };
                for (size_t i = 0; i < 4; ++i)
                {
                    damaged.l[63 + i * 512] = bad[i];
                    clean.l[63 + i * 512] = 0.0f;
                }
                ModuleMonitor monitor;
                monitor.soloDetectionFilter = variant != 0;
                m->setMonitor (monitor);
                reference->setMonitor (monitor);
                const auto out = run (*m, v, damaged, block, ChannelMode::Stereo);
                const auto expected = run (*reference, v, clean, block, ChannelMode::Stereo);
                bool finite = true;
                double error = 0.0;
                for (size_t i = 0; i < out.size(); ++i)
                {
                    finite = finite && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
                    error = std::max (error, std::abs ((double) out.l[i] - expected.l[i]));
                    error = std::max (error, std::abs ((double) out.r[i] - expected.r[i]));
                }
                CHECK (finite);
                CHECK (error < 1.0e-7);
            }
}

TEST_CASE ("ccmp. auto gain follows elapsed samples independently of host blocks")
{
    Values v;
    v.setBoth ("ratio", 4.0f);
    v.setBoth ("thr", -24.0f);
    v.setBoth ("autoGain", 1.0f);
    const auto input = dual (pstest::sine (997.0, -6.0, 2.0, kSr));
    auto reference = makeComp (kSr, v);
    const auto expected = run (*reference, v, input, 64);
    for (int block : { 1, 17, 511 })
    {
        auto m = makeComp (kSr, v);
        const auto out = run (*m, v, input, block);
        double error = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
            error = std::max (error, std::abs ((double) out.l[i] - expected.l[i]));
        note ("auto gain block " + std::to_string (block) + ": maximum sample error", error);
        CHECK (error < 1.0e-7);
    }
}
