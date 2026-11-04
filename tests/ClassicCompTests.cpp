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

// Classic Compressor tests (dsp/ClassicComp.h). Prefix "ccmp.". Owner: mod-ccmp. The "fitted:" cases hold the
// module to numbers measured on the reference's renders (44.1 kHz, the same signals and measurements).
namespace
{
using namespace ps;
using pstest::note;

constexpr double kSr = 48000.0;
constexpr double kRefSr = 44100.0;   // the reference renders' rate (the fitted numbers below)

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

// The reference test file's level steps: a sine whose amplitude moves linearly over 1 ms centred on each step.
std::vector<float> rampedSequence (double hz, std::initializer_list<pstest::Segment> segments, double sr)
{
    std::vector<double> amp, edge;
    double t = 0.0;
    for (const auto& seg : segments)
    {
        amp.push_back (pstest::dbfsToAmplitude (seg.peakDbfs));
        t += seg.seconds;
        edge.push_back (t * sr);
    }
    const double half = 0.5e-3 * sr;
    std::vector<float> out ((size_t) std::llround (t * sr));
    size_t k = 0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        const double n = (double) i;
        while (k + 1 < amp.size() && n >= edge[k])
            ++k;
        double a = amp[k];
        if (k > 0 && n < edge[k - 1] + half)
            a = amp[k - 1] + (amp[k] - amp[k - 1]) * (n - (edge[k - 1] - half)) / (2.0 * half);
        else if (k + 1 < amp.size() && n >= edge[k] - half)
            a = amp[k] + (amp[k + 1] - amp[k]) * (n - (edge[k] - half)) / (2.0 * half);
        out[i] = (float) (a * std::sin (2.0 * 3.14159265358979323846 * hz * n / sr));
    }
    return out;
}

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

TEST_CASE ("ccmp. latency is 0 and equals the measured impulse delay at 44.1 / 48 / 96 / 192 kHz")
{
    const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
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

TEST_CASE ("ccmp. steady tones follow staticCurveDb: feedback ratio mapping, detector reading, knee per mode")
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
                const auto x = pstest::sine (1000.0, (double) lv, 3.0, kSr);
                const auto out = run (*m, vals, dual (x), 512);
                const double measured = toneDb (out.l, (size_t) (2.5 * kSr), (size_t) (0.5 * kSr), 1000.0, kSr);
                const double expected = ClassicComp::staticCurveDb (setParamsOf (thr, (float) ratio, (ClassicCompMode) mode),
                                                                    (double) lv);
                worstErr = std::max (worstErr, std::abs (measured - expected));
                CHECK_NEAR (measured, expected, 0.15);
                if (lv == -8)
                    outAt[0] = measured;
                if (lv == 0)
                    outAt[1] = measured;
            }
            // Sharp / Balanced above their (narrow) knee: 8 dB more input -> 8 / ratio dB more output
            if (mode != 2 && ratio <= 4.0)
            {
                const double rise = outAt[1] - outAt[0];
                note (std::string (modeName (mode)) + " ratio " + std::to_string (ratio).substr (0, 4)
                          + ": output rise for 8 dB more input above the knee (dB)", rise);
                CHECK_NEAR (rise, 8.0 / ratio, 0.03);
            }
        }
    note ("worst difference to staticCurveDb over modes, ratios 1.1..20, inputs -40..0 dBFS (dB)", worstErr);

    // the detector reads a steady sine below its peak (charge / discharge balance), so the knee sits above the knob in
    // sine-peak terms by the measured offset: 0.4 / 2.6 / 4.6 dB at attack 1 / 20 / 50 ms (release 50)
    auto kneeShift = [] (float attack) {
        ClassicCompSetParams p = setParamsOf (-20.0f, 4.0f, ClassicCompMode::Sharp);
        p.attackMs = attack;
        double lo = -40.0, hi = 0.0;   // input where the reduction reaches 1 dB
        for (int i = 0; i < 60; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            (ClassicComp::staticCurveDb (p, mid) - mid < -1.0 ? hi : lo) = mid;
        }
        return 0.5 * (lo + hi);
    };
    const double k1 = kneeShift (1.0f), k20 = kneeShift (20.0f), k50 = kneeShift (50.0f);
    note ("input at 1 dB of reduction (thr -20, ratio 4, Sharp): attack 1 (dBFS)", k1);
    note ("    attack 20", k20);
    note ("    attack 50", k50);
    CHECK_NEAR (k20 - k1, 2.62 - 0.37, 0.15);
    CHECK_NEAR (k50 - k20, 4.59 - 2.62, 0.2);
    // Sharp is the narrowest knee, Smooth starts 10 dB higher and softly
    const double atT[3] { ClassicComp::staticCurveDb (setParamsOf (-20.0f, 4.0f, ClassicCompMode::Sharp), -17.4) + 17.4,
                          ClassicComp::staticCurveDb (setParamsOf (-20.0f, 4.0f, ClassicCompMode::Balanced), -17.4) + 17.4,
                          ClassicComp::staticCurveDb (setParamsOf (-20.0f, 4.0f, ClassicCompMode::Smooth), -17.4) + 17.4 };
    note ("reduction of a tone where the detector reads the threshold: Sharp / Balanced / Smooth (dB)", atT[0]);
    note ("    Balanced", atT[1]);
    note ("    Smooth", atT[2]);
    CHECK (atT[0] > atT[1]);
    CHECK (atT[2] > -0.05 && atT[2] < 0.0);
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

TEST_CASE ("ccmp. fitted: 1 kHz staircase gains per mode, ratio and threshold match the reference (44.1 kHz)")
{
    // The reference's renders (1 kHz, 1 s steps from -42 to 0 dBFS, gain over 0.45..0.95 s of each step, auto gain
    // off, attack 20 / release 50) at the steps -15 .. 0 dBFS
    struct Row
    {
        const char* name;
        float thr, ratio;
        int mode;
        double gain[6];
    };
    const Row rows[] {
        { "Sharp thr -20 ratio 4", -20.0f, 4.0f, 0, { -1.77, -4.04, -6.29, -8.54, -10.79, -13.04 } },
        { "Balanced thr -20 ratio 4", -20.0f, 4.0f, 1, { -1.78, -4.04, -6.29, -8.54, -10.79, -13.04 } },
        { "Smooth thr -20 ratio 4", -20.0f, 4.0f, 2, { -0.07, -0.31, -1.04, -2.38, -4.10, -6.05 } },
        { "Balanced thr -20 ratio 2", -20.0f, 2.0f, 1, { -1.16, -2.67, -4.17, -5.67, -7.17, -8.67 } },
        { "Balanced thr -20 ratio 20", -20.0f, 20.0f, 1, { -2.47, -5.22, -8.03, -10.86, -13.70, -16.55 } },
        { "Balanced thr -30 ratio 4", -30.0f, 4.0f, 1, { -9.29, -11.54, -13.79, -16.04, -18.29, -20.54 } },
    };
    // one phase-continuous staircase, 1 s per step
    std::vector<float> stair ((size_t) (15.0 * kRefSr));
    for (size_t i = 0; i < stair.size(); ++i)
        stair[i] = (float) (pstest::dbfsToAmplitude (-42.0 + 3.0 * (double) (i / (size_t) kRefSr))
                            * std::sin (2.0 * 3.14159265358979323846 * 1000.0 * (double) i / kRefSr));
    double worst = 0.0;
    for (const auto& r : rows)
    {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", r.thr);
        vals.setBoth ("ratio", r.ratio);
        vals.setBoth ("mode", (float) r.mode);
        auto m = makeComp (kRefSr, vals);
        const auto out = run (*m, vals, dual (stair), 512);
        double rowWorst = 0.0;
        for (int k = 0; k < 15; ++k)
        {
            const size_t from = (size_t) k * (size_t) kRefSr + (size_t) (0.45 * kRefSr), len = (size_t) (0.5 * kRefSr);
            const double g = toneDb (out.l, from, len, 1000.0, kRefSr) - toneDb (stair, from, len, 1000.0, kRefSr);
            const double expected = k >= 9 ? r.gain[k - 9] : (-42.0 + 3.0 * k < r.thr ? 0.0 : g);
            rowWorst = std::max (rowWorst, std::abs (g - expected));
        }
        note (std::string (r.name) + ": worst gain difference to the reference over the staircase (dB)", rowWorst);
        CHECK (rowWorst < 0.12);
        worst = std::max (worst, rowWorst);
    }
    note ("worst over every row (dB)", worst);
}

TEST_CASE ("ccmp. fitted: 1 kHz step ballistics match the reference (attack 1..50, release 30..150, Smooth, ratio 20)")
{
    // -36 / -6 dBFS steps with 1 ms ramps (1 s up at 1 s, 4 s, 7 s; a 20 ms burst at 10 s), thr -20, auto gain off,
    // 44.1 kHz; gain of the reference over 2 ms windows at these times after the first rise (up), its fall (down) and
    // the burst's end. "Moving": the first 20 ms after a rise and every release point, where the gain changes by up
    // to 1 dB per ms.
    struct Row
    {
        const char* name;
        float attack, release, ratio;
        int mode;
        double up[8], down[6], burst[4];
    };
    const double tUp[8] { 0, 3, 5, 10, 20, 100, 500, 980 }, tDown[6] { 2, 5, 10, 20, 30, 50 }, tBurst[4] { 1, 3, 5, 8 };
    const Row rows[] {
        { "attack 20 release 50", 20, 50, 4, 1, { -0.00, -2.12, -5.16, -6.57, -6.79, -7.15, -8.13, -8.47 },
          { -7.42, -6.58, -5.32, -3.28, -1.79, -0.36 }, { -4.88, -3.25, -1.71, -0.25 } },
        { "attack 1", 1, 50, 4, 1, { -8.00, -9.92, -9.91, -9.92, -9.93, -9.99, -10.15, -10.21 },
          { -9.16, -8.34, -7.11, -5.11, -3.60, -1.66 }, { -7.81, -6.17, -4.56, -2.24 } },
        { "attack 5", 5, 50, 4, 1, { -2.85, -8.64, -8.84, -8.89, -8.92, -9.08, -9.52, -9.66 },
          { -8.60, -7.77, -6.53, -4.51, -2.99, -1.12 }, { -6.83, -5.19, -3.58, -1.35 } },
        { "attack 50", 50, 50, 4, 1, { -0.00, 0.00, -0.00, -1.59, -4.11, -4.81, -6.36, -6.93 },
          { -5.91, -5.06, -3.79, -1.77, -0.58, -0.05 }, { -2.29, -0.90, -0.18, -0.01 } },
        { "release 30", 20, 30, 4, 1, { -0.00, -1.60, -4.36, -5.46, -5.64, -6.39, -8.03, -8.37 },
          { -7.29, -6.49, -5.40, -3.90, -2.98, -1.96 }, { -2.75, -0.69, -0.05, -0.00 } },
        { "release 100", 20, 100, 4, 1, { -0.00, -2.57, -5.82, -7.58, -7.97, -8.11, -8.51, -8.71 },
          { -7.83, -7.08, -5.89, -3.68, -1.75, -0.07 }, { -6.91, -6.03, -5.15, -3.85 } },
        { "release 150", 20, 150, 4, 1, { -0.00, -2.74, -6.06, -7.98, -8.48, -8.59, -8.80, -8.92 },
          { -8.18, -7.56, -6.54, -4.57, -2.71, -0.22 }, { -7.72, -7.12, -6.52, -5.62 } },
        { "Smooth", 20, 50, 4, 2, { -0.00, -0.05, -0.15, -0.57, -1.14, -1.50, -2.10, -2.34 },
          { -1.99, -1.74, -1.41, -1.00, -0.76, -0.52 }, { -0.82, -0.61, -0.46, -0.30 } },
        { "ratio 20", 20, 50, 20, 1, { -0.00, -4.99, -8.47, -8.61, -8.67, -9.11, -10.36, -10.77 },
          { -5.44, -2.52, -0.57, -0.04, -0.00, -0.00 }, { -1.74, -0.23, -0.03, -0.00 } },
    };
    const auto x = rampedSequence (1000.0, { { -36.0, 1.0 }, { -6.0, 1.0 }, { -36.0, 2.0 }, { -6.0, 1.0 }, { -36.0, 2.0 },
                                             { -6.0, 1.0 }, { -36.0, 2.0 }, { -6.0, 0.02 }, { -36.0, 1.0 } }, kRefSr);
    auto gainAt = [&] (const std::vector<float>& y, double atSeconds, double ms) {
        const size_t a = (size_t) std::llround (atSeconds * kRefSr) + (size_t) std::llround (ms * 1.0e-3 * kRefSr);
        return db (std::abs (pstest::toneOf (y, a, 88, 1000.0, kRefSr) / pstest::toneOf (x, a, 88, 1000.0, kRefSr)));
    };
    double worstSteady = 0.0, worstEdge = 0.0;
    for (const auto& r : rows)
    {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -20.0f);
        vals.setBoth ("ratio", r.ratio);
        vals.setBoth ("attack", r.attack);
        vals.setBoth ("release", r.release);
        vals.setBoth ("mode", (float) r.mode);
        auto m = makeComp (kRefSr, vals);
        const auto y = run (*m, vals, dual (x), 256);
        double steady = 0.0, edge = 0.0;
        auto acc = [&] (double got, double want, bool fast) { (fast ? edge : steady) = std::max (fast ? edge : steady, std::abs (got - want)); };
        for (int i = 0; i < 8; ++i)
            acc (gainAt (y.l, 1.0, tUp[i]), r.up[i], tUp[i] <= 20.0);
        for (int i = 0; i < 6; ++i)
            acc (gainAt (y.l, 2.0, tDown[i]), r.down[i], true);
        for (int i = 0; i < 4; ++i)
            acc (gainAt (y.l, 10.02, tBurst[i]), r.burst[i], true);
        note (std::string (r.name) + ": worst difference to the reference, held / moving gain (dB)", steady);
        note ("    moving (within ms of a step)", edge);
        CHECK (steady < 0.1);
        CHECK (edge < 0.35);
        worstSteady = std::max (worstSteady, steady);
        worstEdge = std::max (worstEdge, edge);
    }
    note ("worst over every row: held / moving (dB)", worstSteady);
    note ("    moving", worstEdge);
}

TEST_CASE ("ccmp. fitted: the harmonics are the detector ripple (h3 per setting as the reference, no h2)")
{
    // h3 (dBc) of a 0 dBFS tone at the end of a 1 s step (thr -20, auto gain off, 44.1 kHz), the reference's values
    struct Row
    {
        const char* name;
        float attack, ratio;
        int mode;
        double h3k, h3c;   // 1 kHz, 100 Hz
    };
    const Row rows[] {
        { "Balanced ratio 4", 20, 4, 1, -51.4, -31.8 },  { "Sharp ratio 4", 20, 4, 0, -51.4, -31.8 },
        { "Smooth ratio 4", 20, 4, 2, -54.7, -35.5 },    { "Balanced ratio 2", 20, 2, 1, -60.8, -41.1 },
        { "Balanced ratio 20", 20, 20, 1, -35.7, -17.2 }, { "attack 1", 1, 4, 1, -48.4, -30.1 },
        { "attack 50", 50, 4, 1, -53.5, -33.6 },
    };
    double worst = 0.0;
    for (const auto& r : rows)
        for (const double hz : { 1000.0, 100.0 })
        {
            Values vals;
            vals.neutral();
            vals.setBoth ("thr", -20.0f);
            vals.setBoth ("ratio", r.ratio);
            vals.setBoth ("attack", r.attack);
            vals.setBoth ("mode", (float) r.mode);
            auto m = makeComp (kRefSr, vals);
            const auto x = pstest::sineSequence (hz, { { -6.0, 1.0 }, { 0.0, 1.0 } }, kRefSr);
            const auto out = run (*m, vals, dual (x), 512);
            const size_t from = (size_t) (1.45 * kRefSr), len = (size_t) (0.5 * kRefSr);
            const double f = toneDb (out.l, from, len, hz, kRefSr);
            const double h2 = toneDb (out.l, from, len, 2.0 * hz, kRefSr) - f;
            const double h3 = toneDb (out.l, from, len, 3.0 * hz, kRefSr) - f;
            const double want = hz > 500.0 ? r.h3k : r.h3c;
            note (std::string (r.name) + (hz > 500.0 ? " 1 kHz" : " 100 Hz") + ": h3 (dBc)", h3);
            CHECK_NEAR (h3, want, 1.0);
            note ("    h2 (dBc)", h2);
            CHECK (h2 < h3 - 40.0);
            worst = std::max (worst, std::abs (h3 - want));
        }
    note ("worst h3 difference to the reference (dB)", worst);

    // no reduction: Sharp / Balanced are exactly linear; Smooth's soft knee leaves only a far-below-audible tail
    for (int mode = 0; mode < 3; ++mode)
    {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -20.0f);
        vals.setBoth ("ratio", 20.0f);
        vals.setBoth ("mode", (float) mode);
        auto m = makeComp (kSr, vals);
        const auto xin = pstest::sine (1000.0, -40.0, 1.0, kSr);
        const auto out = run (*m, vals, dual (xin), 512);
        double diff = 0.0;
        for (size_t i = 0; i < xin.size(); ++i)
            diff = std::max (diff, std::abs ((double) out.l[i] - xin[i]));
        note (std::string (modeName (mode)) + ", 20 dB under the threshold: largest difference to the input (dB re the input)",
              db (diff) + 40.0);
        if (mode == 2)
            CHECK (db (diff) + 40.0 < -100.0);
        else
            CHECK (diff == 0.0);
    }
}

TEST_CASE ("ccmp. Stereo detects the mid and links both channels; M/S compresses each set on its own")
{
    const size_t from = (size_t) kSr, len = (size_t) (0.5 * kSr);
    Values vals;
    vals.neutral();
    vals.setBoth ("thr", -30.0f);
    vals.setBoth ("ratio", 4.0f);
    // left only at -6 dBFS is detected as a -12 dBFS mid: the same gain as a dual-mono -12 dBFS tone
    {
        const auto loud = pstest::sine (1000.0, -6.0, 1.5, kSr);
        auto m = makeComp (kSr, vals);
        const auto out = run (*m, vals, { loud, std::vector<float> (loud.size(), 0.0f) });
        const double gl = toneDb (out.l, from, len, 1000.0, kSr) - toneDb (loud, from, len, 1000.0, kSr);
        const auto half = pstest::sine (1000.0, -12.0, 1.5, kSr);
        auto m2 = makeComp (kSr, vals);
        const auto out2 = run (*m2, vals, dual (half));
        const double gm = toneDb (out2.l, from, len, 1000.0, kSr) - toneDb (half, from, len, 1000.0, kSr);
        note ("Stereo: left-only -6 dBFS gain (dB)", gl);
        note ("    dual-mono -12 dBFS gain (dB)", gm);
        CHECK_NEAR (gl, gm, 0.02);
        CHECK (gm < -5.0);
    }
    // side only (L = -R): the mid is silent, nothing is compressed
    {
        const auto s = pstest::sine (1000.0, -6.0, 1.5, kSr);
        auto neg = s;
        for (auto& v : neg)
            v = -v;
        auto m = makeComp (kSr, vals);
        const auto out = run (*m, vals, { s, neg });
        double diff = 0.0;
        for (size_t i = 0; i < s.size(); ++i)
            diff = std::max ({ diff, std::abs ((double) out.l[i] - s[i]), std::abs ((double) out.r[i] - neg[i]) });
        note ("Stereo: side-only -6 dBFS: largest difference to the input", diff);
        CHECK (diff == 0.0);
    }
    // linked: a loud left channel turns a quiet right channel down by the same gain
    const auto loud = pstest::sine (1000.0, -6.0, 1.5, kSr);
    const auto quiet = pstest::sine (300.0, -40.0, 1.5, kSr);
    {
        auto m = makeComp (kSr, vals);
        const auto out = run (*m, vals, { loud, quiet });
        const double gl = toneDb (out.l, from, len, 1000.0, kSr) - toneDb (loud, from, len, 1000.0, kSr);
        const double gr = toneDb (out.r, from, len, 300.0, kSr) - toneDb (quiet, from, len, 300.0, kSr);
        note ("Stereo: left (loud) gain (dB)", gl);
        note ("    right (quiet) gain (dB)", gr);
        CHECK_NEAR (gl, gr, 0.05);
        CHECK (m->getMeters().gainReductionDb[1].load() == 0.0f);
        CHECK (m->getMeters().sidechainDb[1].load() <= -99.0f);
    }
    // M/S: set A on ch0 (Mid), set B on ch1 (Side); the quiet side is untouched by the loud mid
    {
        Values ms;
        ms.neutral();
        ms.set ("a.thr", -30.0f);
        ms.set ("a.ratio", 4.0f);
        ms.set ("b.thr", -20.0f);
        ms.set ("b.ratio", 4.0f);
        auto m = makeComp (kSr, ms);
        const auto out = run (*m, ms, { loud, quiet }, 512, ChannelMode::MidSide);
        double diff = 0.0;
        for (size_t i = 0; i < quiet.size(); ++i)
            diff = std::max (diff, std::abs ((double) out.r[i] - quiet[i]));
        const double gm = toneDb (out.l, from, len, 1000.0, kSr) - toneDb (loud, from, len, 1000.0, kSr);
        const double expected = ClassicComp::staticCurveDb (setParamsOf (-30.0f, 4.0f, ClassicCompMode::Balanced), -6.0) + 6.0;
        note ("M/S: mid gain (dB)", gm);
        note ("    expected from staticCurveDb (dB)", expected);
        note ("    side: largest difference to its input", diff);
        CHECK_NEAR (gm, expected, 0.3);
        CHECK (diff == 0.0);
        CHECK (m->getMeters().gainReductionDb[0].load() < -10.0f);
        CHECK (m->getMeters().gainReductionDb[1].load() == 0.0f);

        // set B with a low threshold compresses the side alone, the mid keeps set A's amount
        ms.set ("b.thr", -50.0f);
        ms.set ("b.ratio", 10.0f);
        auto m2 = makeComp (kSr, ms);
        const auto out2 = run (*m2, ms, { loud, quiet }, 512, ChannelMode::MidSide);
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

TEST_CASE ("ccmp. sidechain EQ: the listen output follows detectorResponseDb and the reference's measured shapes")
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
            if (hz > 0.1 * sr)   // the bilinear designs cramp toward Nyquist; the prototype does not
                continue;
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
        note ("listen output vs detectorResponseDb up to 0.1 x the rate @" + std::to_string ((int) sr) + ": worst (dB)", worst);
    }
    // defaults: flat in the band, the high-pass -3 dB at its corner (1st order)
    const ClassicCompSetParams def;
    CHECK_NEAR (ClassicComp::detectorResponseDb (def, 1000.0), 0.0, 0.01);
    CHECK_NEAR (ClassicComp::detectorResponseDb (def, (double) def.scHighPassHz), -3.01, 0.02);
    CHECK_NEAR (ClassicComp::detectorResponseDb (def, 0.5 * (double) def.scHighPassHz), -6.99, 0.02);

    // the reference's measured detector gains at 44.1 kHz: high-pass 400 Hz -12.3 dB at 100 Hz (exactly 1st order);
    // bell +8 at 1 kHz with shelf -8 at 6 kHz: +0.6 / +3.6 / -5.1 / -11.4 dB at 100 Hz / 1 / 5 / 10 kHz
    auto listenDb = [&] (double hz, const std::function<void (Values&)>& setup) {
        Values vals;
        setup (vals);
        auto m = makeComp (kRefSr, vals);
        ModuleMonitor mon;
        mon.soloDetectionFilter = true;
        m->setMonitor (mon);
        const auto x = pstest::sine (hz, -20.0, 1.0, kRefSr);
        const auto out = run (*m, vals, dual (x));
        const size_t from = (size_t) (0.5 * kRefSr), len = (size_t) (0.5 * kRefSr);
        return toneDb (out.l, from, len, hz, kRefSr) - toneDb (x, from, len, hz, kRefSr);
    };
    const double hp400 = listenDb (100.0, [] (Values& v) { v.setBoth ("sc.hpf", 400.0f); })
                         - listenDb (100.0, [] (Values& v) { v.setBoth ("sc.hpf", 20.0f); });
    note ("high-pass 400 Hz at 100 Hz (dB re 20 Hz)", hp400);
    CHECK_NEAR (hp400, -12.3 + 0.08, 0.1);   // (the 20 Hz high-pass itself takes 0.08 dB at 100 Hz)
    const double hzs[] { 100.0, 1000.0, 5000.0, 10000.0 }, want[] { 0.6, 3.6, -5.1, -11.4 };
    for (int i = 0; i < 4; ++i)
    {
        const double g = listenDb (hzs[i], [] (Values& v) { v.setBoth ("sc.bellHz", 1000.0f); v.setBoth ("sc.bellDb", 8.0f); v.setBoth ("sc.shelfDb", -8.0f); })
                         - listenDb (hzs[i], [] (Values&) {});
        note ("bell +8 @ 1 kHz, shelf -8 @ 6 kHz: detector gain at " + std::to_string ((int) hzs[i]) + " Hz (dB)", g);
        CHECK_NEAR (g, want[i], 0.3);
    }

    // the EQ steers the reduction (above the knee by (ratio - 1) / ratio of the detector gain)
    auto reductionAt = [&] (double hz, const std::function<void (Values&)>& setup) {
        Values vals;
        vals.neutral();
        vals.setBoth ("thr", -30.0f);
        vals.setBoth ("ratio", 4.0f);
        setup (vals);
        auto m = makeComp (kSr, vals);
        run (*m, vals, dual (pstest::sine (hz, -10.0, 3.0, kSr)));
        return (double) m->getMeters().gainReductionDb[0].load();
    };
    const double hp20 = reductionAt (60.0, [] (Values& v) { v.setBoth ("sc.hpf", 20.0f); });
    const double hpHi = reductionAt (60.0, [] (Values& v) { v.setBoth ("sc.hpf", 400.0f); });
    note ("60 Hz at -10 dBFS: reduction with sc.hpf 20 Hz (dB)", hp20);
    note ("    with sc.hpf 400 Hz (dB)", hpHi);
    CHECK (hpHi > hp20 + 8.0);
    const double flat = reductionAt (1000.0, [] (Values&) {});
    const double up = reductionAt (1000.0, [] (Values& v) { v.setBoth ("sc.bellHz", 1000.0f); v.setBoth ("sc.bellDb", 8.0f); });
    const double down = reductionAt (1000.0, [] (Values& v) { v.setBoth ("sc.bellHz", 1000.0f); v.setBoth ("sc.bellDb", -8.0f); });
    note ("1 kHz: reduction flat / bell +8 / bell -8 (dB)", flat);
    note ("    bell +8", up);
    note ("    bell -8", down);
    CHECK_NEAR (up - flat, -4.0 * 3.0 / 4.0, 0.15);   // the bell gives half the knob at its centre
    CHECK_NEAR (down - flat, 4.0 * 3.0 / 4.0, 0.15);
    const double shelf = reductionAt (10000.0, [] (Values& v) { v.setBoth ("sc.shelfHz", 4000.0f); v.setBoth ("sc.shelfDb", 8.0f); });
    const double shelfFlat = reductionAt (10000.0, [] (Values&) {});
    note ("10 kHz: reduction with a +8 dB shelf at 4 kHz (dB)", shelf);
    CHECK (shelf < shelfFlat - 10.0);   // the shelf gives twice the knob
}

TEST_CASE ("ccmp. auto gain: ratio of 0.5 s |input| / |output| averages; makeup on top; held in silence; clamped 0..20")
{
    // steady compression: the correction equals the reduction, the output returns to the input level
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
    CHECK_NEAR (rmsDb (on.l, from, len), inDb, 0.25);
    CHECK_NEAR (rmsDb (onMakeup.l, from, len) - inDb, 6.0, 0.25);
    CHECK (agMeter > 5.0f && agMeter < 12.0f);

    // silence: the correction is held (both averages decay alike)
    const auto held = m->getMeters().autoGainDb[0].load();
    run (*m, vals, dual (std::vector<float> ((size_t) (3.0 * kSr), 0.0f)));
    note ("    after 3 s of silence (dB)", m->getMeters().autoGainDb[0].load());
    CHECK_NEAR (m->getMeters().autoGainDb[0].load(), held, 0.01);

    // the 0.5 s averages: a steady compressed tone, then the same tone 30 dB lower (no reduction): the correction
    // follows (e^(-t/0.5) + a (1 - e^(-t/0.5))) / (g e^(-t/0.5) + a (1 - e^(-t/0.5))), a = -30 dB, g = the gain before
    {
        Values v;
        v.setBoth ("thr", -20.0f);
        v.setBoth ("ratio", 4.0f);
        v.setBoth ("autoGain", 1.0f);
        auto mc = makeComp (kSr, v);
        run (*mc, v, dual (pstest::sine (1000.0, -6.0, 4.0, kSr)));
        const double gBefore = pstest::dbfsToAmplitude ((double) mc->getMeters().gainReductionDb[0].load());
        const double agBefore = mc->getMeters().autoGainDb[0].load();
        note ("compressed tone: reduction / auto gain (dB)", mc->getMeters().gainReductionDb[0].load());
        note ("    auto gain", agBefore);
        CHECK_NEAR (agBefore, -mc->getMeters().gainReductionDb[0].load(), 0.1);
        const double a = pstest::dbfsToAmplitude (-30.0);
        double worst = 0.0;
        std::vector<float> trace;
        const auto quietTone = pstest::sine (1000.0, -36.0, 1.5, kSr);
        run (*mc, v, dual (quietTone), 480, ChannelMode::Stereo,
             [&] (size_t start, Values&, ClassicComp& c) { if (start > 0) trace.push_back (c.getMeters().autoGainDb[0].load()); });
        for (const double t : { 0.25, 0.5, 1.0 })
        {
            const double e = std::exp (-t / 0.5);
            const double want = db ((e + a * (1.0 - e)) / (gBefore * e + a * (1.0 - e)));
            const double got = trace[(size_t) std::llround (t * 100.0) - 1];
            note ("    " + std::to_string (t).substr (0, 4) + " s after the drop: auto gain (dB)", got);
            note ("        expected", want);
            worst = std::max (worst, std::abs (got - want));
        }
        CHECK (worst < 0.25);
    }

    // clamped at +20 dB
    Values hard;
    hard.setBoth ("thr", -60.0f);
    hard.setBoth ("ratio", 20.0f);
    hard.setBoth ("autoGain", 1.0f);
    auto mh = makeComp (kSr, hard);
    run (*mh, hard, dual (noise));
    note ("thr -60, ratio 20: auto gain (dB)", mh->getMeters().autoGainDb[0].load());
    CHECK_NEAR (mh->getMeters().autoGainDb[0].load(), 20.0, 0.01);

    // switching auto gain off returns to exactly 0 dB
    hard.setBoth ("autoGain", 0.0f);
    run (*mh, hard, dual (std::vector<float> ((size_t) (3.0 * kSr), 0.0f)));
    CHECK (mh->getMeters().autoGainDb[0].load() == 0.0f);
}

TEST_CASE ("ccmp. meters: reduction, 1 s hold, detector level")
{
    // the detector meter reads a steady sine 2.6 dB under its peak at attack 20 / release 50 (charge / discharge)
    {
        Values vals;
        vals.neutral();
        auto m = makeComp (kSr, vals);
        run (*m, vals, dual (pstest::sine (1000.0, -18.0, 3.0, kSr)));
        note ("detector meter of a -18 dBFS sine (dBFS)", m->getMeters().sidechainDb[0].load());
        CHECK_NEAR (m->getMeters().sidechainDb[0].load(), -18.0 - 2.6, 0.15);
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
        { "threshold 0 -> -60 jump", [&] (Values& v) { base (v); v.setBoth ("thr", 0.0f); },
          [&] (Values& v) { base (v); v.setBoth ("thr", -60.0f); } },
        { "threshold -60 -> 0 jump", [&] (Values& v) { base (v); v.setBoth ("thr", -60.0f); },
          [&] (Values& v) { base (v); v.setBoth ("thr", 0.0f); } },
        { "ratio 1 -> 20 jump", [&] (Values& v) { base (v); v.setBoth ("ratio", 1.0f); },
          [&] (Values& v) { base (v); v.setBoth ("ratio", 20.0f); } },
        { "mode Sharp -> Smooth", [&] (Values& v) { base (v); v.setBoth ("mode", 0.0f); },
          [&] (Values& v) { base (v); v.setBoth ("mode", 2.0f); } },
        { "mode Smooth -> Sharp", [&] (Values& v) { base (v); v.setBoth ("mode", 2.0f); },
          [&] (Values& v) { base (v); v.setBoth ("mode", 0.0f); } },
        { "makeup -30 -> +30 jump", [&] (Values& v) { base (v); v.setBoth ("makeup", -30.0f); },
          [&] (Values& v) { base (v); v.setBoth ("makeup", 30.0f); }, -30.0 },
        { "attack 100 -> 0.1 jump", [&] (Values& v) { base (v); v.setBoth ("attack", 100.0f); },
          [&] (Values& v) { base (v); v.setBoth ("attack", 0.1f); } },
        { "release 150 -> 30 jump", [&] (Values& v) { base (v); v.setBoth ("release", 150.0f); },
          [&] (Values& v) { base (v); v.setBoth ("release", 30.0f); } },
        { "sidechain high-pass 20 -> 400 Hz", [&] (Values& v) { base (v); v.setBoth ("sc.hpf", 20.0f); },
          [&] (Values& v) { base (v); v.setBoth ("sc.hpf", 400.0f); } },
        { "sidechain bell -8 -> +8 dB", [&] (Values& v) { base (v); v.setBoth ("sc.bellHz", 800.0f); v.setBoth ("sc.bellDb", -8.0f); },
          [&] (Values& v) { base (v); v.setBoth ("sc.bellHz", 800.0f); v.setBoth ("sc.bellDb", 8.0f); } },
        { "sidechain shelf 4 kHz -8 -> 12 kHz +8", [&] (Values& v) { base (v); v.setBoth ("sc.shelfHz", 4000.0f); v.setBoth ("sc.shelfDb", -8.0f); },
          [&] (Values& v) { base (v); v.setBoth ("sc.shelfHz", 12000.0f); v.setBoth ("sc.shelfDb", 8.0f); } },
        { "auto gain on", [&] (Values& v) { base (v); },
          [&] (Values& v) { base (v); v.setBoth ("autoGain", 1.0f); } },
        { "auto gain off", [&] (Values& v) { base (v); v.setBoth ("autoGain", 1.0f); },
          [&] (Values& v) { base (v); } },
        // (the 20 Hz high-pass keeps the detector signal close to the 100 Hz input: the switch itself is measured)
        { "detector solo on", [&] (Values& v) { base (v); v.setBoth ("sc.hpf", 20.0f); },
          [&] (Values& v) { base (v); v.setBoth ("sc.hpf", 20.0f); }, -6.0, 1 },
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
        CHECK (pk < pstest::dbfsToAmplitude (52.0));   // input peak + makeup 30 + auto gain 20 dB at most
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
        CHECK (b < -20.0);
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
        vals.setBoth ("release", 150.0f);
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

TEST_CASE ("ccmp. fastest feedback loop: periodic above the high-pass corner, bounded everywhere, 44.1..192 kHz")
{
    // Attack 0.1 ms, +12 dBFS tones 72 dB over the threshold, every mode / release / ratio and extreme detection
    // filters. A tone at least twice the high-pass corner settles to a repeating cycle (residual under -70 dB; with a
    // +8 dB detection boost near the tone under -30 dB: a small irregular ripple); below that the
    // detection filter's memory of the gain modulation (the reference's structure, ClassicComp.cpp) may keep the
    // gain moving non-periodically at high ratios, but the output stays finite and under 0 dBFS.
    for (const double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        double worstResidual = -300.0, largestPeak = -300.0;
        int periodicCases = 0;
        for (int mode = 0; mode < 3; ++mode)
            for (const float release : { 30.0f, 150.0f })
                for (const float ratio : { 4.0f, 20.0f })
                    for (const double hz : { 20.0, 100.0, 1000.0 })
                        for (int filter = 0; filter < 6; ++filter)
                        {
                            Values v;
                            v.neutral();
                            v.setBoth ("thr", -60.0f);
                            v.setBoth ("ratio", ratio);
                            v.setBoth ("attack", 0.1f);
                            v.setBoth ("release", release);
                            v.setBoth ("mode", (float) mode);
                            v.setBoth ("sc.hpf", filter == 0 ? 45.0f : 400.0f);
                            v.setBoth ("sc.bellHz", filter == 2 ? 9000.0f : 800.0f);
                            v.setBoth ("sc.shelfHz", filter == 2 ? 12000.0f : 4000.0f);
                            v.setBoth ("sc.bellDb", filter == 0 || filter == 3 ? 0.0f : (filter == 1 || filter == 4 ? 8.0f : -8.0f));
                            v.setBoth ("sc.shelfDb", filter == 0 || filter == 3 ? 0.0f : (filter == 1 || filter == 5 ? 8.0f : -8.0f));
                            // Exactly repeated cycles: compare samples, not just two fundamental amplitudes.
                            // At 44.1 kHz the nearest integer period to 1 kHz is used.
                            const size_t period = (size_t) std::llround (sr / hz);
                            std::vector<float> x ((size_t) (8.0 * sr));
                            for (size_t i = 0; i < x.size(); ++i)
                                x[i] = (float) (4.0 * std::sin (2.0 * 3.14159265358979323846 * (double) (i % period) / (double) period));
                            auto m = makeComp (sr, v);
                            const auto y = run (*m, v, dual (x), 127);
                            double residual = 0.0, power = 0.0, peak = 0.0;
                            for (size_t i = (size_t) (7.5 * sr); i < y.size(); ++i)
                            {
                                const double sample = y.l[i];
                                const double d = sample - (double) y.l[i - period];
                                residual += d * d;
                                power += sample * sample;
                                peak = std::max (peak, std::abs (sample));
                            }
                            const double error = db (std::sqrt (residual / power));
                            worstResidual = std::max (worstResidual, error);
                            largestPeak = std::max (largestPeak, db (peak));
                            CHECK (std::isfinite (error));
                            CHECK (peak < 1.0);
                            if (hz < 2.0 * (filter == 0 ? 45.0 : 400.0))
                                continue;
                            ++periodicCases;
                            const bool boosted = filter == 1 || filter == 4 || filter == 5;
                            if (error >= (boosted ? -30.0 : -70.0))
                                note (std::string (modeName (mode)) + " release " + std::to_string ((int) release)
                                      + " filter " + std::to_string (filter) + " ratio " + std::to_string ((int) ratio)
                                      + " Hz " + std::to_string ((int) hz) + ": cycle residual (dB)", error);
                            CHECK (error < (boosted ? -30.0 : -70.0));
                        }
        note ("fast loop @" + std::to_string ((int) sr) + ": cases above the corner", periodicCases);
        note ("    worst cycle residual over every case (dB)", worstResidual);
        note ("    largest settled peak (dBFS)", largestPeak);
    }
}

TEST_CASE ("ccmp. auto gain holds through long silence and recovers on fresh quiet audio")
{
    for (const double sr : { 44100.0, 192000.0 })
    {
        Values v;
        v.neutral();
        v.setBoth ("thr", -60.0f);
        v.setBoth ("ratio", 20.0f);
        v.setBoth ("autoGain", 1.0f);
        auto m = makeComp (sr, v);
        run (*m, v, dual (pstest::sine (1000.0, -6.0, 3.0, sr)));
        const float held = m->getMeters().autoGainDb[0].load();
        CHECK_NEAR (held, 20.0, 0.001);
        // More than enough for both 500 ms averages to pass the floor and flush to zero.
        const auto silence = run (*m, v, dual (std::vector<float> ((size_t) (40.0 * sr), 0.0f)), 511);
        CHECK (std::all_of (silence.l.begin(), silence.l.end(), [] (float x) { return x == 0.0f; }));
        CHECK_NEAR (m->getMeters().autoGainDb[0].load(), held, 0.001);
        const auto x = pstest::sine (1000.0, -90.0, 0.25, sr);
        const auto y = run (*m, v, dual (x), 17);
        double error = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
            error = std::max (error, std::abs ((double) y.l[i] - x[i]));
        note ("fresh quiet audio after 40 s silence @" + std::to_string ((int) sr) + ": sample error", error);
        CHECK (error < 1.0e-10);
        CHECK_NEAR (m->getMeters().autoGainDb[0].load(), 0.0, 0.001);
    }
}
