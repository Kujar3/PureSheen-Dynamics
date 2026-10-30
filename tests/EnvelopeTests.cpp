#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/Envelope.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

// Envelope building blocks (dsp/Envelope.cpp). Prefix "env.".
namespace
{
using namespace ps;

// samples until x first reaches the level `frac` of the way from `from` to `to`
int samplesToReach (const std::vector<float>& y, float from, float to, float frac)
{
    const float target = from + (to - from) * frac;
    for (size_t i = 0; i < y.size(); ++i)
        if ((to < from && y[i] <= target) || (to >= from && y[i] >= target))
            return (int) i + 1;
    return -1;
}
} // namespace

TEST_CASE ("env. GainCurve: downward compression, ratio, knee")
{
    GainCurve c;
    c.thresholdDb = -20.0f;
    c.ratio = 4.0f;
    c.kneeDb = 0.0f;
    CHECK_NEAR (c.gainDb (-30.0f), 0.0, 1e-6);
    CHECK_NEAR (c.gainDb (-20.0f), 0.0, 1e-6);
    CHECK_NEAR (c.gainDb (-10.0f), -7.5, 1e-5);   // 10 dB over at 4:1 -> 2.5 dB over
    CHECK_NEAR (c.gainDb (0.0f), -15.0, 1e-5);

    c.ratio = 30.0f;
    CHECK_NEAR (c.gainDb (-10.0f), 10.0 / 30.0 - 10.0, 1e-5);

    c.ratio = 0.5f;   // |ratio| < 1 is 1:1
    CHECK_NEAR (c.gainDb (0.0f), 0.0, 1e-9);
    c.ratio = -0.9f;
    CHECK_NEAR (c.gainDb (-60.0f), 0.0, 1e-9);

    // soft knee: quadratic, continuous in value and slope, -W/8 (1 - 1/R) at the threshold
    c.ratio = 4.0f;
    c.kneeDb = 10.0f;
    CHECK_NEAR (c.gainDb (-20.0f), -0.75 * 10.0 / 8.0, 1e-5);
    CHECK_NEAR (c.gainDb (-25.0f), 0.0, 1e-5);
    CHECK_NEAR (c.gainDb (-15.0f), -3.75, 1e-5);
    double worstJump = 0.0, worstSlopeJump = 0.0;
    float prevG = c.gainDb (-60.0f), prevSlope = 0.0f;
    for (float x = -60.0f + 0.01f; x < 10.0f; x += 0.01f)
    {
        const float g = c.gainDb (x);
        const float slope = (g - prevG) / 0.01f;
        worstJump = std::max (worstJump, (double) std::abs (g - prevG));
        if (x > -59.9f)
            worstSlopeJump = std::max (worstSlopeJump, (double) std::abs (slope - prevSlope));
        prevG = g;
        prevSlope = slope;
    }
    CHECK (worstJump < 0.01);
    CHECK (worstSlopeJump < 0.02);
}

TEST_CASE ("env. GainCurve: negative ratios (upward compression and upward expansion)")
{
    GainCurve up;
    up.thresholdDb = -30.0f;
    up.ratio = -2.0f;
    CHECK_NEAR (up.gainDb (-20.0f), 0.0, 1e-6);          // above the threshold: untouched
    CHECK_NEAR (up.gainDb (-40.0f), 5.0, 1e-5);          // 10 dB below at 2:1 -> raised to 5 dB below
    CHECK_NEAR (up.gainDb (-70.0f), 20.0, 1e-5);
    CHECK_NEAR (up.gainDb (-78.0f), 24.0, 1e-5);         // 48 dB below: 24 dB lift = the cap
    CHECK_NEAR (up.gainDb (-90.0f), 12.0, 1e-5);         // beyond 48 dB the lift falls back 1 dB per dB
    CHECK_NEAR (up.gainDb (-110.0f), 0.0, 1e-5);
    CHECK_NEAR (up.gainDb (-200.0f), 0.0, 1e-5);         // silence is never raised

    up.ratio = -10.0f;
    CHECK_NEAR (up.gainDb (-60.0f), 24.0, 1e-5);         // capped at +24 dB

    // upward compression output level never decreases with the input (monotonic transfer curve)
    up.ratio = -3.0f;
    up.kneeDb = 6.0f;
    float prevOut = -300.0f;
    bool monotonic = true;
    for (float x = -150.0f; x < 0.0f; x += 0.05f)
    {
        const float out = x + up.gainDb (x);
        monotonic = monotonic && out >= prevOut - 1e-4f;
        prevOut = out;
    }
    CHECK (monotonic);

    GainCurve ex;
    ex.thresholdDb = -10.0f;
    ex.ratio = -2.0f;
    ex.upwardExpands = true;
    CHECK_NEAR (ex.gainDb (-20.0f), 0.0, 1e-6);
    CHECK_NEAR (ex.gainDb (-5.0f), 5.0, 1e-5);           // 5 dB over at 1:2 -> 10 dB over
    CHECK_NEAR (ex.gainDb (40.0f), 24.0, 1e-5);          // capped
    ex.kneeDb = 4.0f;
    CHECK_NEAR (ex.gainDb (-10.0f), 1.0 * 4.0 / 8.0, 1e-5);
}

TEST_CASE ("env. GainSmoother attack and release times")
{
    const double sr = 48000.0;
    GainSmoother s;
    s.prepare (sr);
    s.setTimes (10.0f, 100.0f, false);
    s.reset (0.0f);

    std::vector<float> y;
    for (int i = 0; i < 48000; ++i)
        y.push_back (s.process (-10.0f, 0.0f));
    const int att = samplesToReach (y, 0.0f, -10.0f, 1.0f - (float) std::exp (-1.0));
    pstest::note ("attack: samples to 63 % (10 ms = 480)", att);
    CHECK (std::abs (att - 480) <= 2);

    y.clear();
    for (int i = 0; i < 96000; ++i)
        y.push_back (s.process (0.0f, 0.0f));
    const int rel = samplesToReach (y, -10.0f, 0.0f, 1.0f - (float) std::exp (-1.0));
    pstest::note ("release: samples to 63 % (100 ms = 4800)", rel);
    CHECK (std::abs (rel - 4800) <= 5);

    // upward gain: rising is release, falling is attack
    s.reset (0.0f);
    y.clear();
    for (int i = 0; i < 48000; ++i)
        y.push_back (s.process (6.0f, 0.0f));
    CHECK (std::abs (samplesToReach (y, 0.0f, 6.0f, 1.0f - (float) std::exp (-1.0)) - 4800) <= 5);
}

TEST_CASE ("env. adaptive release: 2x on sustained material, 0.2x on transients")
{
    const double sr = 48000.0;

    auto effectiveRelease = [&] (const std::vector<float>& detectorDb)
    {
        GainSmoother s;
        s.prepare (sr);
        s.setTimes (1.0f, 100.0f, true);
        s.reset (0.0f);
        // settle the crest estimate, holding -10 dB of gain reduction
        const size_t settle = 48000;
        for (size_t i = 0; i < settle; ++i)
            s.process (-10.0f, detectorDb[i]);
        std::vector<float> y;
        for (size_t i = settle; i < detectorDb.size(); ++i)
            y.push_back (s.process (0.0f, detectorDb[i]));
        return 1000.0 * samplesToReach (y, -10.0f, 0.0f, 1.0f - (float) std::exp (-1.0)) / sr;
    };

    // sustained: 80 Hz sine at -6 dBFS (detector = |x| in dB, i.e. Peak detection)
    std::vector<float> sustained;
    {
        const auto x = pstest::sine (80.0, -6.0, 2.0, sr);
        for (float v : x)
            sustained.push_back (gainToDbF (std::abs (v)));
    }
    // transient: 2 ms clicks every 150 ms, decaying 60 dB in 40 ms, on a -70 dB floor
    std::vector<float> transient;
    for (int i = 0; i < 96000; ++i)
    {
        const double t = std::fmod ((double) i / sr, 0.150);
        transient.push_back ((float) std::max (-70.0, -1.5 * 1000.0 * t));
    }

    std::vector<float> bass40;
    {
        const auto x = pstest::sine (40.0, -6.0, 2.0, sr);
        for (float v : x)
            bass40.push_back (gainToDbF (std::abs (v)));
    }
    pstest::note ("effective release, sustained 40 Hz sine (ms, set 100)", effectiveRelease (bass40));
    const double sus = effectiveRelease (sustained);
    const double tra = effectiveRelease (transient);
    pstest::note ("effective release, sustained 80 Hz sine (ms, set 100)", sus);
    pstest::note ("effective release, transient material (ms, set 100)", tra);
    CHECK (sus > 180.0 && sus < 210.0);
    CHECK (tra > 15.0 && tra < 30.0);
}

TEST_CASE ("env. LevelDetector modes")
{
    const double sr = 48000.0;
    const auto x = pstest::sine (1000.0, -6.0, 0.5, sr);
    for (auto mode : { DetectionMode::Peak, DetectionMode::Envelope, DetectionMode::Rms })
    {
        LevelDetector d;
        d.prepare (sr);
        d.setMode (mode);
        float maxDb = -200.0f, minDb = 0.0f;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const float db = d.processDb (x[i]);
            if (i > x.size() / 2)
            {
                maxDb = std::max (maxDb, db);
                minDb = std::min (minDb, db);
            }
        }
        if (mode == DetectionMode::Peak)
            CHECK_NEAR (maxDb, -6.0, 0.01);
        if (mode == DetectionMode::Envelope)
        {
            CHECK_NEAR (maxDb, -6.0, 0.3);   // the 0.1 ms attack reads a 1 kHz peak ~0.2 dB low
            CHECK (maxDb - minDb < 0.5);     // smooth: ripple < 0.5 dB on a 1 kHz tone
        }
        if (mode == DetectionMode::Rms)
        {
            CHECK_NEAR (0.5 * (maxDb + minDb), -9.03, 0.1);
            CHECK (maxDb - minDb < 0.5);
        }
    }

    // envelope release ~10 ms: 63 % of the way down (in amplitude) after 480 samples of silence
    LevelDetector d;
    d.prepare (sr);
    d.setMode (DetectionMode::Envelope);
    for (int i = 0; i < 4800; ++i)
        d.processDb (0.5f);
    int n = 0;
    while (dbToGainF (d.processDb (0.0f)) > 0.5f * (float) std::exp (-1.0) && n < 48000)
        ++n;
    CHECK (std::abs (n + 1 - 480) <= 2);
}

TEST_CASE ("env. SlidingMax equals the brute-force maximum")
{
    const auto x = pstest::whiteNoise (1.0, 2.0, 48000.0, 77u);
    for (int window : { 1, 2, 7, 64, 480 })
    {
        SlidingMax m;
        m.prepare (512);
        m.setWindow (window);
        bool same = true;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const float got = m.process (x[i]);
            float ref = x[i];
            for (int k = 1; k < window && (size_t) k <= i; ++k)
                ref = std::max (ref, x[i - (size_t) k]);
            same = same && got == ref;
        }
        CHECK (same);
    }

    // window shrinks mid-stream: the result is the brute force over the new window immediately
    SlidingMax m;
    m.prepare (1000);
    m.setWindow (1000);
    bool same = true;
    for (size_t i = 0; i < 20000; ++i)
    {
        if (i == 10000)
            m.setWindow (100);
        const int w = i < 10000 ? 1000 : 100;
        const float got = m.process (x[i]);
        float ref = x[i];
        for (int k = 1; k < w && (size_t) k <= i; ++k)
            ref = std::max (ref, x[i - (size_t) k]);
        same = same && got == ref;
    }
    CHECK (same);
}

TEST_CASE ("env. DelayLine and LinearSmoother are exact")
{
    const auto x = pstest::whiteNoise (1.0, 0.2, 48000.0, 13u);
    for (int delay : { 0, 1, 17, 4096 })
    {
        DelayLine a, b;
        a.prepare (4096);
        b.prepare (4096);
        a.setDelay (delay);
        b.setDelay (delay);
        CHECK_EQ (a.getDelay(), delay);
        std::vector<float> blockOut = x;
        for (size_t pos = 0; pos < blockOut.size(); pos += 333)
            b.process (blockOut.data() + pos, (int) std::min<size_t> (333, blockOut.size() - pos));
        bool exact = true;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const float y = a.process (x[i]);
            const float want = i >= (size_t) delay ? x[i - (size_t) delay] : 0.0f;
            exact = exact && y == want && blockOut[i] == want;
        }
        CHECK (exact);
    }

    LinearSmoother s;
    s.prepare (48000.0, 10.0);   // 480 samples
    s.snap (0.0f);
    s.setTarget (1.0f);
    float v = 0.0f;
    int steps = 0;
    while (s.isSmoothing())
    {
        v = s.next();
        ++steps;
    }
    CHECK_EQ (steps, 480);
    CHECK (v == 1.0f);
    CHECK (s.getCurrent() == 1.0f);
    CHECK (s.next() == 1.0f);

    CHECK_NEAR (onePoleCoeff (10.0, 48000.0), std::exp (-1.0 / 480.0), 1e-7);
}

TEST_CASE ("env. realtime blocks do not allocate")
{
    LevelDetector d;
    GainSmoother g;
    SlidingMax m;
    DelayLine dl;
    LinearSmoother ls;
    d.prepare (48000.0);
    g.prepare (48000.0);
    m.prepare (480);
    dl.prepare (480);
    ls.prepare (48000.0, 5.0);
    const auto x = pstest::whiteNoise (0.5, 0.5, 48000.0, 1u);

    pstest::AllocationScope scope;
    GainCurve c;
    c.thresholdDb = -20.0f;
    c.ratio = 4.0f;
    float acc = 0.0f;
    for (size_t i = 0; i < x.size(); ++i)
    {
        if (i % 512 == 0)
        {
            g.setTimes (5.0f, 80.0f, (i / 512) % 2 == 0);
            m.setWindow (100 + (int) (i % 300));
            dl.setDelay ((int) (i % 400));
            ls.setTarget ((float) (i % 7));
        }
        const float lvl = d.processDb (x[i]);
        acc += g.process (c.gainDb (lvl), lvl) + m.process (x[i]) + dl.process (x[i]) + ls.next();
    }
    CHECK_EQ (scope.count(), 0LL);
    CHECK (std::isfinite (acc));
}
