#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"

#include "dsp/DeLimiter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// De-limiter tests (dsp/DeLimiter.h). Prefix "dlim.". Owner: mod-dlim. Synthetic signals only.
namespace
{
using namespace ps;

constexpr double kRate = 44100.0;
constexpr double kPi = 3.14159265358979323846;

struct Rig
{
    explicit Rig (double rate = kRate, int block = 512) : blockSize (block)
    {
        values.resize (paramSpecs().size());
        for (size_t i = 0; i < values.size(); ++i)
            values[i] = paramSpecs()[i].defaultValue;
        module.prepare (rate, block);
        module.setParams (values.data());
    }

    void set (const char* id, float v)
    {
        values[(size_t) paramIndex (id)] = v;
        module.setParams (values.data());
    }

    // Runs the signal through in blocks, returns the output
    void run (std::vector<float>& l, std::vector<float>& r)
    {
        float* ch[2];
        for (size_t at = 0; at < l.size(); at += (size_t) blockSize)
        {
            const int n = (int) std::min (l.size() - at, (size_t) blockSize);
            ch[0] = l.data() + at;
            ch[1] = r.data() + at;
            module.setParams (values.data());
            module.process (ch, n, ctx);
        }
    }

    DeLimiter module;
    ProcessContext ctx;
    std::vector<float> values;
    int blockSize;
};

// Hits over a steady bed: enough for the model to lift, limited like a finished master.
void hits (std::vector<float>& l, std::vector<float>& r, double seconds, float peak, double rate = kRate)
{
    const size_t n = (size_t) (seconds * rate);
    l.assign (n, 0.0f);
    r.assign (n, 0.0f);
    std::uint32_t seed = 77u;
    for (size_t i = 0; i < n; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        const double noise = (double) (seed >> 8) / 8388608.0 - 1.0;
        const double t = std::fmod ((double) i / rate, 0.25);
        const double env = 0.35 + std::exp (-t * 40.0);
        const double v = std::clamp (noise * env, -1.0, 1.0) * peak;
        l[i] = r[i] = (float) v;
    }
}

float peakOf (const std::vector<float>& x, size_t from = 0)
{
    float p = 0.0f;
    for (size_t i = from; i < x.size(); ++i)
        p = std::max (p, std::abs (x[i]));
    return p;
}
} // namespace

TEST_CASE ("dlim. defaults, ranges and latency")
{
    DeLimiterParams d;
    CHECK_EQ (d.amountPercent, 0.0f);
    CHECK_EQ (d.thresholdDb, 0.0f);
    const auto& amount = paramSpecs()[(size_t) paramIndex ("dlim.amount")];
    CHECK_EQ (amount.maxValue, 200.0f);
    const auto& thr = paramSpecs()[(size_t) paramIndex ("dlim.thr")];
    CHECK_EQ (thr.minValue, -12.0f);
    CHECK_EQ (thr.maxValue, 0.0f);
    CHECK_EQ (DeLimiter::latencyFor (44100.0), 80);
    CHECK_EQ (DeLimiter::latencyFor (48000.0), 85);
    CHECK_EQ (DeLimiter::latencyFor (96000.0), 175);
    CHECK_EQ (DeLimiter::latencyFor (192000.0), 350);
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        DeLimiter m;
        m.prepare (rate, 256);
        CHECK_EQ (m.getLatencySamples(), DeLimiter::latencyFor (rate));
    }
}

TEST_CASE ("dlim. Amount 0 is the input delayed, bit-exact, on a hot signal")
{
    std::vector<float> l, r;
    hits (l, r, 2.0, 1.0f);
    const auto inL = l, inR = r;
    Rig rig;
    rig.run (l, r);
    const int d = rig.module.getLatencySamples();
    bool exact = true;
    for (size_t i = (size_t) d; i < l.size(); ++i)
        exact = exact && l[i] == inL[i - (size_t) d] && r[i] == inR[i - (size_t) d];
    CHECK (exact);
}

TEST_CASE ("dlim. never below the input; lifts peaks, leaves quiet material exactly alone")
{
    std::vector<float> l, r;
    hits (l, r, 3.0, 0.5f);
    const auto in = l;
    std::vector<float> rr = r;
    Rig rig;
    rig.set ("dlim.amount", 100.0f);
    rig.set ("dlim.thr", -6.0f);
    rig.run (l, rr);
    const int d = rig.module.getLatencySamples();
    double worst = 1.0, best = 1.0;
    for (size_t i = (size_t) d; i < l.size(); ++i)
        if (std::abs (in[i - (size_t) d]) > 1.0e-3f)
        {
            const double g = (double) l[i] / (double) in[i - (size_t) d];
            worst = std::min (worst, g);
            best = std::max (best, g);
        }
    CHECK (worst >= 1.0 - 1.0e-5);
    CHECK (best > 1.3);
    CHECK (best < std::pow (10.0, 11.5 / 20.0));

    // Far under the threshold: exact
    std::vector<float> ql, qr;
    hits (ql, qr, 2.0, 0.05f);
    const auto qin = ql;
    Rig quiet;
    quiet.set ("dlim.amount", 200.0f);
    quiet.run (ql, qr);
    bool exact = true;
    for (size_t i = 80; i < ql.size(); ++i)
        exact = exact && ql[i] == qin[i - 80];
    CHECK (exact);
}

TEST_CASE ("dlim. one curve for both channels; side-only material gets no lift")
{
    std::vector<float> l, r;
    hits (l, r, 2.0, 0.5f);
    const auto a = l, b = r;
    std::vector<float> rs (r.size());
    for (size_t i = 0; i < r.size(); ++i)
        rs[i] = 0.25f * r[i];
    const auto bs = rs;
    Rig rig;
    rig.set ("dlim.amount", 100.0f);
    rig.set ("dlim.thr", -6.0f);
    rig.run (l, rs);
    bool same = true;
    for (size_t i = 80; i < l.size(); ++i)
        if (std::abs (a[i - 80]) > 1.0e-3f && std::abs (bs[i - 80]) > 1.0e-3f)
            same = same && std::abs (l[i] / a[i - 80] - rs[i] / bs[i - 80]) < 1.0e-3f;
    CHECK (same);

    std::vector<float> sl, sr;
    hits (sl, sr, 2.0, 0.9f);
    for (auto& v : sr)
        v = -v;
    const auto s0 = sl;
    Rig side;
    side.set ("dlim.amount", 200.0f);
    side.set ("dlim.thr", -12.0f);
    side.run (sl, sr);
    bool untouched = true;
    for (size_t i = 80; i < sl.size(); ++i)
        untouched = untouched && sl[i] == s0[i - 80];
    CHECK (untouched);
}

TEST_CASE ("dlim. Amount scales the linear lift exactly; Output Gain is a plain gain afterwards")
{
    std::vector<float> l, r, base;
    hits (l, r, 2.0, 0.5f);
    const auto in = l;
    auto render = [&] (float amount, float out)
    {
        auto a = in, b = in;
        Rig rig;
        rig.set ("dlim.amount", amount);
        rig.set ("dlim.output", out);
        rig.set ("dlim.thr", -6.0f);
        rig.run (a, b);
        return a;
    };
    const auto a100 = render (100.0f, 0.0f), a200 = render (200.0f, 0.0f), a50 = render (50.0f, 0.0f), o6 = render (100.0f, 6.0f);
    const double outGain = std::pow (10.0, 6.0 / 20.0);
    bool law = true, output = true;
    for (size_t i = 44100; i < a100.size(); ++i)
    {
        const double x = in[i - 80];
        if (std::abs (x) < 1.0e-2)
            continue;
        const double g = a100[i] / x;
        law = law && std::abs (a200[i] / x - (1.0 + 2.0 * (g - 1.0))) < 1.0e-3 && std::abs (a50[i] / x - (1.0 + 0.5 * (g - 1.0))) < 1.0e-3;
        output = output && std::abs (o6[i] / outGain - a100[i]) < 1.0e-4;
    }
    CHECK (law);
    CHECK (output);
}

TEST_CASE ("dlim. only the level against the threshold matters")
{
    std::vector<float> l, r;
    hits (l, r, 2.0, 0.4f);
    const auto in = l;
    auto a = in, b = in, c = in, e = in;
    Rig t12;
    t12.set ("dlim.amount", 100.0f);
    t12.set ("dlim.thr", -12.0f);
    t12.run (a, b);
    Rig in6;
    in6.set ("dlim.amount", 100.0f);
    in6.set ("dlim.thr", -6.0f);
    in6.set ("dlim.input", 6.0f);
    in6.run (c, e);
    const float back = std::pow (10.0f, -6.0f / 20.0f);
    double worst = 0.0;
    for (size_t i = 44100; i < a.size(); ++i)
        worst = std::max (worst, (double) std::abs (a[i] - c[i] * back));
    CHECK (worst < 2.0e-3);
}

TEST_CASE ("dlim. the same audio in any block size, deterministic")
{
    std::vector<float> l, r;
    hits (l, r, 1.5, 0.5f);
    const auto in = l;
    auto render = [&] (int block)
    {
        auto a = in, b = in;
        Rig rig (kRate, block);
        rig.set ("dlim.amount", 130.0f);
        rig.set ("dlim.thr", -6.0f);
        rig.run (a, b);
        return a;
    };
    const auto ref = render (512);
    for (int block : { 1, 17, 64, 511, 4096 })
        CHECK (render (block) == ref);
    CHECK (render (512) == ref);
}

TEST_CASE ("dlim. survives corrupt input and stays finite")
{
    std::vector<float> l, r;
    hits (l, r, 1.0, 0.5f);
    l[1000] = std::numeric_limits<float>::quiet_NaN();
    r[2000] = std::numeric_limits<float>::infinity();
    l[3000] = 1.0e20f;
    Rig rig;
    rig.set ("dlim.amount", 200.0f);
    rig.set ("dlim.thr", -12.0f);
    rig.set ("dlim.input", 9.0f);
    rig.run (l, r);
    bool finite = true;
    for (size_t i = 0; i < l.size(); ++i)
        finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]) && std::abs (l[i]) < 100.0f;
    CHECK (finite);
}

TEST_CASE ("dlim. allocation free")
{
    std::vector<float> l, r;
    hits (l, r, 1.0, 0.5f);
    Rig rig;
    rig.set ("dlim.amount", 100.0f);
    rig.module.startLearn();
    float* ch[2];
    pstest::AllocationScope scope;
    for (size_t at = 0; at + 256 <= l.size(); at += 256)
    {
        ch[0] = l.data() + at;
        ch[1] = r.data() + at;
        rig.module.setParams (rig.values.data());
        rig.module.process (ch, 256, rig.ctx);
    }
    CHECK_EQ (scope.count(), 0);
}

//==============================================================================
// Learn Threshold: the behaviour measured on the reference
namespace
{
std::vector<float> sine (double seconds, float level, double hz = 1000.0, double rate = kRate)
{
    std::vector<float> s ((size_t) (seconds * rate));
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = level * (float) std::sin (2.0 * kPi * hz * (double) i / rate);
    return s;
}

float db (double a) { return (float) (20.0 * std::log10 (a)); }
float level (float dbValue) { return std::pow (10.0f, dbValue / 20.0f); }

bool learnOf (Rig& rig, std::vector<float> l, std::vector<float> r, float& result)
{
    rig.run (l, r);
    return rig.module.takeLearnedThreshold (result);
}
} // namespace

TEST_CASE ("dlim. learn: waits for audio without limit, then takes the peak over exactly 6 s")
{
    Rig rig;
    rig.module.startLearn();
    CHECK (rig.module.isLearning());
    CHECK (rig.module.isWaitingForAudio());

    // A minute of silence changes nothing.
    std::vector<float> zl ((size_t) kRate * 60), zr ((size_t) kRate * 60);
    rig.run (zl, zr);
    CHECK (rig.module.isWaitingForAudio());
    CHECK_EQ (rig.module.getLearnProgress(), 0.0f);

    // -8 dB for 5.9 s, -5 dB for 0.2 s, then -1: the window ends at 6.0 s, so it reads -5.
    auto l = sine (5.9, level (-8.0f));
    const auto mid = sine (0.2, level (-5.0f));
    const auto late = sine (2.0, level (-1.0f));
    l.insert (l.end(), mid.begin(), mid.end());
    l.insert (l.end(), late.begin(), late.end());
    float t = 0.0f;
    CHECK (learnOf (rig, l, l, t));
    CHECK (std::abs (t - (-5.0f)) < 0.01f);
    CHECK (! (rig.module.isLearning()));
    CHECK (! (rig.module.takeLearnedThreshold (t)));   // once
}

TEST_CASE ("dlim. learn: silence or a pause does not stop the clock")
{
    Rig rig;
    rig.module.startLearn();
    auto l = sine (2.0, level (-10.0f));
    l.resize ((size_t) (kRate * 10.0));                  // 8 s of silence
    const auto loud = sine (2.0, level (-3.0f));
    l.insert (l.end(), loud.begin(), loud.end());
    float t = 0.0f;
    CHECK (learnOf (rig, l, l, t));
    CHECK (std::abs (t - (-10.0f)) < 0.01f);
}

TEST_CASE ("dlim. learn: louder channel, sample peak, after Input Gain, clamped to -12 .. 0")
{
    {
        Rig rig;
        rig.module.startLearn();
        float t = 0.0f;
        CHECK (learnOf (rig, sine (6.5, level (-6.0f)), sine (6.5, level (-12.0f)), t));
        CHECK (std::abs (t - (-6.0f)) < 0.01f);
    }
    {
        // A tone at a quarter of the rate, sampled off its crests: 3 dB below its true peak
        Rig rig;
        std::vector<float> s ((size_t) (kRate * 6.5));
        for (size_t i = 0; i < s.size(); ++i)
            s[i] = level (-6.0f) * (float) std::sin (kPi * 0.5 * (double) i + kPi * 0.25);
        rig.module.startLearn();
        float t = 0.0f;
        CHECK (learnOf (rig, s, s, t));
        CHECK (std::abs (t - (-6.0f - (float) db (std::sqrt (2.0)))) < 0.01f);
    }
    {
        Rig rig;
        rig.set ("dlim.input", 6.0f);
        std::vector<float> settle (1 << 16);
        auto settleR = settle;
        rig.run (settle, settleR);
        rig.module.startLearn();
        float t = 0.0f;
        CHECK (learnOf (rig, sine (6.5, level (-9.0f)), sine (6.5, level (-9.0f)), t));
        CHECK (std::abs (t - (-3.0f)) < 0.01f);
    }
    {
        Rig rig;
        rig.module.startLearn();
        float t = 1.0f;
        CHECK (learnOf (rig, sine (6.5, level (-20.0f)), sine (6.5, level (-20.0f)), t));
        CHECK_EQ (t, -12.0f);
    }
    {
        Rig rig;
        rig.module.startLearn();
        float t = -1.0f;
        CHECK (learnOf (rig, sine (6.5, level (3.0f)), sine (6.5, level (3.0f)), t));
        CHECK_EQ (t, 0.0f);
    }
}

TEST_CASE ("dlim. learn: the window starts at the press; cancel and restart; the audio is not touched")
{
    std::vector<float> first = sine (3.0, level (-1.0f));
    auto firstR = first;
    Rig rig;
    rig.run (first, firstR);       // playing before the press: not counted
    rig.module.startLearn();
    float t = 0.0f;
    CHECK (learnOf (rig, sine (6.5, level (-7.0f)), sine (6.5, level (-7.0f)), t));
    CHECK (std::abs (t - (-7.0f)) < 0.01f);

    // A cancel leaves nothing behind; a restart counts from zero.
    Rig again;
    again.module.startLearn();
    auto a = sine (5.0, level (-2.0f));
    auto b = a;
    again.run (a, b);
    CHECK (again.module.getLearnProgress() > 0.5f);
    again.module.cancelLearn();
    CHECK (! (again.module.isLearning()));
    again.module.startLearn();
    CHECK_EQ (again.module.getLearnProgress(), 0.0f);
    float t2 = 0.0f;
    CHECK (learnOf (again, sine (6.5, level (-9.0f)), sine (6.5, level (-9.0f)), t2));
    CHECK (std::abs (t2 - (-9.0f)) < 0.01f);

    // Audio is the same with and without a learn running.
    auto x = sine (2.0, level (-3.0f)), xr = x, y = x, yr = x;
    Rig p, q;
    p.set ("dlim.amount", 100.0f);
    q.set ("dlim.amount", 100.0f);
    q.module.startLearn();
    p.run (x, xr);
    q.run (y, yr);
    CHECK (x == y);
}
