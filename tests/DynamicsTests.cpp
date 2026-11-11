#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/Crossover.h"
#include "dsp/Dynamics.h"
#include "dsp/Envelope.h"
#include "dsp/Filters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Multiband Dynamics (dsp/Dynamics.cpp). Prefix "dyn.".
namespace
{
using namespace ps;
using pstest::cd;
using pstest::kPi;

constexpr double kSr = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

// A Dynamics instance with its own flat parameter array (defaults from the table).
struct Rig
{
    explicit Rig (double sampleRate = kSr, int blockSize = 512) : sr (sampleRate), block (blockSize)
    {
        for (const auto& s : paramSpecs())
            values.push_back (s.defaultValue);
        dyn = std::make_unique<Dynamics>();
    }

    void set (const std::string& id, float v)
    {
        const int i = paramIndex ("dyn." + id);
        REQUIRE (i >= 0);
        values[(size_t) i] = v;
    }

    // every band of both sets
    void setAllBands (const std::string& name, float v)
    {
        for (const char* s : { "a", "b" })
            for (int b = 1; b <= kMaxBands; ++b)
                set (std::string (s) + ".b" + std::to_string (b) + "." + name, v);
    }

    void setBand (int set, int band, const std::string& name, float v)
    {
        this->set (std::string (set == 0 ? "a" : "b") + ".b" + std::to_string (band) + "." + name, v);
    }

    void prepare()
    {
        dyn->prepare (sr, block);
        dyn->setParams (values.data());
    }

    // Processes in blocks; onBlock (start) may change parameters before each block.
    Stereo run (const std::vector<float>& l, const std::vector<float>& r, ChannelMode mode = ChannelMode::Stereo,
                const std::function<void (size_t)>& onBlock = {})
    {
        Stereo out { l, r };
        ProcessContext ctx;
        ctx.channelMode = mode;
        for (size_t start = 0; start < l.size(); start += (size_t) block)
        {
            if (onBlock)
                onBlock (start);
            dyn->setParams (values.data());
            const int n = (int) std::min ((size_t) block, l.size() - start);
            float* ch[2] { out.l.data() + start, out.r.data() + start };
            dyn->process (ch, n, ctx);
        }
        return out;
    }

    Stereo run (const std::vector<float>& mono) { return run (mono, mono); }

    double sr;
    int block;
    std::vector<float> values;
    std::unique_ptr<Dynamics> dyn;
};

std::vector<float> dc (double dbfs, double seconds, double sr = kSr)
{
    return std::vector<float> ((size_t) std::llround (seconds * sr), (float) pstest::dbfsToAmplitude (dbfs));
}

std::vector<float> dcSteps (std::initializer_list<pstest::Segment> segs, double sr = kSr)
{
    std::vector<float> out;
    for (const auto& s : segs)
    {
        const auto v = dc (s.peakDbfs, s.seconds, sr);
        out.insert (out.end(), v.begin(), v.end());
    }
    return out;
}

double gainDbAt (const std::vector<float>& in, const std::vector<float>& out, size_t i)
{
    return 20.0 * std::log10 (std::abs ((double) out[i]) / std::abs ((double) in[i]));
}

double expectedStatic (const GainCurve& comp, bool compOn, const GainCurve& lim, bool limOn, float level)
{
    const float c = compOn ? comp.gainDb (level) : 0.0f;
    const float l = limOn ? lim.gainDb (level + c) : 0.0f;
    return (double) c + (double) l;
}

// Max |a[i] - b[i - delay]| in dBFS over [from, a.size()).
double nullDb (const std::vector<float>& a, const std::vector<float>& ref, int delay, size_t from)
{
    double m = 0.0;
    for (size_t i = from; i < a.size(); ++i)
    {
        const double r = i >= (size_t) delay ? (double) ref[i - (size_t) delay] : 0.0;
        m = std::max (m, std::abs ((double) a[i] - r));
    }
    return pstest::dbOf (m);
}

// first index where the gain (dB) trajectory crosses `level` going in direction sign(level - start)
size_t crossing (const std::vector<float>& in, const std::vector<float>& out, size_t from, double level, bool falling)
{
    for (size_t i = from; i < out.size(); ++i)
    {
        const double g = gainDbAt (in, out, i);
        if (falling ? g <= level : g >= level)
            return i;
    }
    return out.size();
}

// Interpolated crossing time (samples) for an exponential-ish trajectory.
double crossingTime (const std::vector<float>& in, const std::vector<float>& out, size_t from, double level, bool falling)
{
    const size_t i = crossing (in, out, from, level, falling);
    if (i == 0 || i >= out.size())
        return (double) i;
    const double g1 = gainDbAt (in, out, i - 1), g2 = gainDbAt (in, out, i);
    const double frac = std::abs (g2 - g1) > 1e-12 ? (level - g1) / (g2 - g1) : 0.0;
    return (double) (i - 1 - from) + frac + 1.0;   // samples after `from` (the first sample of the new level is 1)
}
} // namespace

//==============================================================================
TEST_CASE ("dyn. static transfer curve (DC, every detection mode) matches GainCurve")
{
    struct Setting
    {
        float thr, ratio, knee;
        bool compOn;
        float lThr, lRatio, lKnee;
        bool limOn;
        const char* what;
    };
    const Setting settings[] {
        { -20.0f, 4.0f, 0.0f, true, 0.0f, 10.0f, 0.0f, false, "4:1 hard knee" },
        { -20.0f, 4.0f, 12.0f, true, 0.0f, 10.0f, 0.0f, false, "4:1 12 dB knee" },
        { -30.0f, 2.0f, 6.0f, true, 0.0f, 10.0f, 0.0f, false, "2:1 6 dB knee" },
        { -10.0f, 30.0f, 0.0f, true, 0.0f, 10.0f, 0.0f, false, "30:1" },
        { -20.0f, -3.0f, 0.0f, true, 0.0f, 10.0f, 0.0f, false, "upward compression -3:1" },
        { -25.0f, -10.0f, 10.0f, true, 0.0f, 10.0f, 0.0f, false, "upward compression -10:1 10 dB knee" },
        { 0.0f, 1.0f, 0.0f, false, -20.0f, -2.0f, 0.0f, true, "limiter upward expansion -2:1" },
        { 0.0f, 1.0f, 0.0f, false, -18.0f, -2.5f, 6.0f, true, "limiter upward expansion -2.5:1 6 dB knee" },
        { -30.0f, 2.0f, 0.0f, true, -20.0f, 10.0f, 0.0f, true, "compressor 2:1 + limiter 10:1" },
    };

    double worst = 0.0;
    for (int det = 0; det < 3; ++det)
        for (const auto& s : settings)
        {
            Rig rig;
            rig.set ("detect", (float) det);
            rig.setAllBands ("comp.on", s.compOn ? 1.0f : 0.0f);
            rig.setAllBands ("comp.thr", s.thr);
            rig.setAllBands ("comp.ratio", s.ratio);
            rig.setAllBands ("comp.knee", s.knee);
            rig.setAllBands ("comp.att", 1.0f);
            rig.setAllBands ("comp.rel", 10.0f);
            rig.setAllBands ("lim.on", s.limOn ? 1.0f : 0.0f);
            rig.setAllBands ("lim.thr", s.lThr);
            rig.setAllBands ("lim.ratio", s.lRatio);
            rig.setAllBands ("lim.knee", s.lKnee);
            rig.setAllBands ("lim.att", 1.0f);
            rig.setAllBands ("lim.rel", 10.0f);
            rig.prepare();

            const GainCurve cc { s.thr, s.ratio, s.knee, false };
            const GainCurve lc { s.lThr, s.lRatio, s.lKnee, true };

            for (float level = -45.0f; level <= -1.0f; level += 4.0f)
            {
                const auto in = dc (level, 0.4);
                const auto out = rig.run (in);
                const double got = gainDbAt (in, out.l, in.size() - 1);
                const double want = expectedStatic (cc, s.compOn, lc, s.limOn, level);
                worst = std::max (worst, std::abs (got - want));
                if (std::abs (got - want) > 0.2)
                {
                    std::cout << "    " << s.what << " det " << det << " level " << level << ": got " << got
                              << " want " << want << "\n";
                    CHECK (false);
                }
                CHECK (out.l.back() == out.r.back());
            }
        }
    pstest::note ("worst static curve error (dB)", worst);
    CHECK (worst < 0.2);
}

TEST_CASE ("dyn. static curve with a sine in the middle band of 3 (RMS detection)")
{
    const double f = 1000.0, peak = -6.0;
    const float thr = -24.0f, ratio = 3.0f, knee = 6.0f;
    const float xo[3] { 200.0f, 5000.0f, 10000.0f };

    // the band levels the detector sees, from a standalone crossover
    Crossover x;
    x.prepare (kSr, 512);
    x.setup (3, xo, CrossoverType::Analog);
    const auto in = pstest::sine (f, peak, 3.0, kSr);
    std::array<std::array<std::vector<float>, 2>, 3> bands;
    for (auto& b : bands)
        for (auto& c : b)
            c.assign (in.size(), 0.0f);
    {
        std::vector<float> buf (3 * 2 * 512);
        float* chans[kMaxBands][2] {};
        float* const* ptrs[kMaxBands] {};
        for (int b = 0; b < 3; ++b)
        {
            chans[b][0] = buf.data() + (b * 2) * 512;
            chans[b][1] = buf.data() + (b * 2 + 1) * 512;
            ptrs[b] = chans[b];
        }
        for (size_t s = 0; s < in.size(); s += 512)
        {
            const int n = (int) std::min ((size_t) 512, in.size() - s);
            const float* i2[2] { in.data() + s, in.data() + s };
            x.process (i2, ptrs, n);
            for (int b = 0; b < 3; ++b)
                for (int c = 0; c < 2; ++c)
                    std::copy (chans[b][c], chans[b][c] + n, bands[(size_t) b][(size_t) c].begin() + (std::ptrdiff_t) s);
        }
    }

    const size_t from = (size_t) (2.0 * kSr), len = (size_t) kSr;
    const cd t1 = pstest::toneOf (bands[0][0], from, len, f, kSr);
    const cd t2 = pstest::toneOf (bands[1][0], from, len, f, kSr);
    const cd t3 = pstest::toneOf (bands[2][0], from, len, f, kSr);
    const double bandLevelRms = pstest::dbOf (std::abs (t2)) - 3.0103;

    Rig rig;
    rig.set ("bands", 3.0f);
    rig.set ("xover1", xo[0]);
    rig.set ("xover2", xo[1]);
    rig.set ("detect", (float) DetectionMode::Rms);
    rig.setAllBands ("comp.ratio", 1.0f);
    rig.setBand (0, 2, "comp.ratio", ratio);
    rig.setBand (0, 2, "comp.thr", thr);
    rig.setBand (0, 2, "comp.knee", knee);
    rig.setBand (0, 2, "comp.att", 50.0f);
    rig.setBand (0, 2, "comp.rel", 500.0f);
    rig.prepare();
    const auto out = rig.run (in);

    const cd o = pstest::toneOf (out.l, from, len, f, kSr);
    const cd g = (o - t1 - t3) / t2;
    const double got = pstest::dbOf (std::abs (g));
    const double want = GainCurve { thr, ratio, knee, false }.gainDb ((float) bandLevelRms);
    pstest::note ("band 2 detector level (dB RMS)", bandLevelRms);
    pstest::note ("measured band gain (dB)", got);
    pstest::note ("curve (dB)", want);
    CHECK_NEAR (got, want, 0.2);
    CHECK (std::abs (std::arg (g)) < 0.01);   // a pure gain (no phase change)
}

//==============================================================================
TEST_CASE ("dyn. attack and release time constants on level steps")
{
    struct Case
    {
        float att, rel;
        bool limiter;
    };
    for (const Case c : { Case { 10.0f, 100.0f, false }, Case { 2.0f, 400.0f, false }, Case { 1.0f, 50.0f, true },
                          Case { 0.5f, 20.0f, true } })
    {
        Rig rig;
        rig.set ("detect", (float) DetectionMode::Peak);
        if (c.limiter)
        {
            rig.setAllBands ("comp.on", 0.0f);
            rig.setAllBands ("lim.on", 1.0f);
            rig.setAllBands ("lim.thr", -20.0f);
            rig.setAllBands ("lim.ratio", 4.0f);
            rig.setAllBands ("lim.knee", 0.0f);
            rig.setAllBands ("lim.att", c.att);
            rig.setAllBands ("lim.rel", c.rel);
        }
        else
        {
            rig.setAllBands ("comp.thr", -20.0f);
            rig.setAllBands ("comp.ratio", 4.0f);
            rig.setAllBands ("comp.knee", 0.0f);
            rig.setAllBands ("comp.att", c.att);
            rig.setAllBands ("comp.rel", c.rel);
        }
        rig.prepare();

        const double up = 0.5, hold = 2.0;
        const auto in = dcSteps ({ { -30.0, up }, { -6.0, hold }, { -30.0, 0.03 * c.rel } });   // 30 time constants
        const auto out = rig.run (in).l;

        const double target = -14.0 * 0.75;   // -10.5 dB
        const size_t stepUp = (size_t) (up * kSr), stepDown = (size_t) ((up + hold) * kSr);
        CHECK_NEAR (gainDbAt (in, out, stepUp - 1), 0.0, 1e-6);
        CHECK_NEAR (gainDbAt (in, out, stepDown - 1), target, 1e-3);

        const double tAtt = crossingTime (in, out, stepUp, target * (1.0 - std::exp (-1.0)), true) / kSr * 1000.0;
        const double tRel = crossingTime (in, out, stepDown, target * std::exp (-1.0), false) / kSr * 1000.0;
        pstest::note (std::string (c.limiter ? "limiter" : "compressor") + " attack set " + std::to_string (c.att)
                          + " ms, measured (ms)",
                      tAtt);
        pstest::note (std::string (c.limiter ? "limiter" : "compressor") + " release set " + std::to_string (c.rel)
                          + " ms, measured (ms)",
                      tRel);
        CHECK (std::abs (tAtt - c.att) <= 0.1 * c.att);
        CHECK (std::abs (tRel - c.rel) <= 0.1 * c.rel);
        CHECK (out.back() == in.back());   // fully released: exact unity
    }
}

TEST_CASE ("dyn. upward stages: attack is the reaction to a rising level")
{
    // Upward compression (-3:1 at -20): a level drop raises the gain at the RELEASE time, a level rise removes the
    // boost at the ATTACK time. Limiter upward expansion (-2:1 at -20): a level rise raises the gain at the attack
    // time, a level drop lowers it at the release time.
    for (int limiter = 0; limiter < 2; ++limiter)
    {
        Rig rig;
        rig.set ("detect", (float) DetectionMode::Peak);
        const float att = limiter ? 1.0f : 5.0f, rel = limiter ? 40.0f : 200.0f;
        if (limiter)
        {
            rig.setAllBands ("comp.on", 0.0f);
            rig.setAllBands ("lim.on", 1.0f);
            rig.setAllBands ("lim.thr", -20.0f);
            rig.setAllBands ("lim.ratio", -2.0f);
            rig.setAllBands ("lim.att", att);
            rig.setAllBands ("lim.rel", rel);
        }
        else
        {
            rig.setAllBands ("comp.thr", -20.0f);
            rig.setAllBands ("comp.ratio", -3.0f);
            rig.setAllBands ("comp.knee", 0.0f);
            rig.setAllBands ("comp.att", att);
            rig.setAllBands ("comp.rel", rel);
        }
        rig.prepare();
        const double quiet = limiter ? -30.0 : -40.0, loud = limiter ? -10.0 : -10.0;
        const double boost = limiter ? 10.0 : 20.0 * (2.0 / 3.0);   // gain at the level that is boosted
        const double first = limiter ? quiet : loud, second = limiter ? loud : quiet;
        const auto in = dcSteps ({ { first, 1.0 }, { second, 2.0 }, { first, 2.0 } });
        const auto out = rig.run (in).l;
        const size_t s1 = (size_t) kSr, s2 = (size_t) (3.0 * kSr);
        CHECK_NEAR (gainDbAt (in, out, s2 - 1), boost, 1e-3);
        const double tUp = crossingTime (in, out, s1, boost * (1.0 - std::exp (-1.0)), false) / kSr * 1000.0;
        const double tDown = crossingTime (in, out, s2, boost * std::exp (-1.0), true) / kSr * 1000.0;
        const double wantUp = limiter ? att : rel, wantDown = limiter ? rel : att;
        pstest::note (std::string (limiter ? "upward expansion" : "upward compression") + ": boost rises in (ms)", tUp);
        pstest::note (std::string (limiter ? "upward expansion" : "upward compression") + ": boost falls in (ms)", tDown);
        CHECK (std::abs (tUp - wantUp) <= 0.1 * wantUp);
        CHECK (std::abs (tDown - wantDown) <= 0.1 * wantDown);
    }
}

TEST_CASE ("dyn. a long release converges exactly (double ballistics)")
{
    Rig rig;
    rig.set ("detect", (float) DetectionMode::Peak);
    rig.setAllBands ("comp.thr", -40.0f);
    rig.setAllBands ("comp.ratio", 10.0f);
    rig.setAllBands ("comp.knee", 0.0f);
    rig.setAllBands ("comp.att", 1.0f);
    rig.setAllBands ("comp.rel", 1500.0f);
    rig.prepare();
    const auto in = dcSteps ({ { -10.0, 0.5 }, { -60.0, 40.0 } });
    const auto out = rig.run (in).l;
    // 1.5 s after the step: e^-1 of the reduction left
    const double gr = -30.0 * 0.9;
    const size_t step = (size_t) (0.5 * kSr);
    CHECK_NEAR (gainDbAt (in, out, step + (size_t) (1.5 * kSr)), gr * std::exp (-1.0), 0.05 * std::abs (gr));
    CHECK (out.back() == in.back());
    size_t exactFrom = out.size();
    for (size_t i = out.size(); i-- > step;)
        if (out[i] != in[i])
        {
            exactFrom = i + 1;
            break;
        }
    pstest::note ("1500 ms release: exactly unity after (s)", (double) (exactFrom - step) / kSr);
}

TEST_CASE ("dyn. adaptive release is longer on sustained material than after transients")
{
    auto releaseTime = [] (bool adaptive, double burstSeconds) {
        Rig rig;
        rig.set ("detect", (float) DetectionMode::Envelope);
        rig.set ("adaptRel", adaptive ? 1.0f : 0.0f);
        rig.setAllBands ("comp.thr", -30.0f);
        rig.setAllBands ("comp.ratio", 4.0f);
        rig.setAllBands ("comp.knee", 0.0f);
        rig.setAllBands ("comp.att", 0.5f);
        rig.setAllBands ("comp.rel", 100.0f);
        rig.prepare();
        const auto in = pstest::sineSequence (1000.0, { { -40.0, 1.0 }, { -6.0, burstSeconds }, { -40.0, 2.0 } }, kSr);
        const auto out = rig.run (in).l;
        // gain from the envelope of the output vs input over 1 ms windows
        const size_t drop = (size_t) ((1.0 + burstSeconds) * kSr);
        const size_t w = 48;
        auto gainAt = [&] (size_t i) {
            return 20.0 * std::log10 (pstest::peakOf (out, i, w) / pstest::peakOf (in, i, w));
        };
        const double grStart = gainAt (drop - w);
        for (size_t i = drop; i + w < out.size(); i += 8)
            if (gainAt (i) >= grStart * 0.5)
                return std::make_pair (grStart, (double) (i - drop) / kSr * 1000.0);
        return std::make_pair (grStart, 1.0e9);
    };

    const auto fixedSustained = releaseTime (false, 1.0);
    const auto sustained = releaseTime (true, 1.0);
    const auto transient = releaseTime (true, 0.004);
    pstest::note ("fixed release: half-recovery time (ms)", fixedSustained.second);
    pstest::note ("adaptive, after 1 s sustained: GR (dB)", sustained.first);
    pstest::note ("adaptive, after 1 s sustained: half-recovery time (ms)", sustained.second);
    pstest::note ("adaptive, after a 4 ms transient: GR (dB)", transient.first);
    pstest::note ("adaptive, after a 4 ms transient: half-recovery time (ms)", transient.second);
    CHECK (sustained.first < -15.0 && transient.first < -10.0);
    CHECK (sustained.second > 1.5 * fixedSustained.second);   // ~2x the set release
    CHECK (transient.second < 0.5 * fixedSustained.second);   // faster than the set release
    CHECK (sustained.second > 3.0 * transient.second);
}

//==============================================================================
TEST_CASE ("dyn. neutral settings = crossover only")
{
    // 1 band, lookahead 0: bit-exact (also with the default curve on material below its threshold)
    for (int neutralRatio = 0; neutralRatio < 2; ++neutralRatio)
    {
        Rig rig;
        if (neutralRatio == 1)
            rig.setAllBands ("comp.ratio", 1.0f);
        rig.prepare();
        const auto l = pstest::pinkNoise (neutralRatio == 1 ? 0.9 : 0.03, 2.0, kSr, 7);
        const auto r = pstest::pinkNoise (neutralRatio == 1 ? 0.9 : 0.03, 2.0, kSr, 8);
        const auto out = rig.run (l, r);
        CHECK (out.l == l);
        CHECK (out.r == r);
        CHECK_EQ (rig.dyn->getLatencySamples(), 0);
    }

    // Analog / Hybrid 4 bands: sum = allpass of the input -> flat magnitude
    for (auto type : { CrossoverType::Analog, CrossoverType::Hybrid })
    {
        Rig rig;
        rig.set ("bands", 4.0f);
        rig.set ("xoverType", (float) type);
        rig.setAllBands ("comp.ratio", 1.0f);
        rig.prepare();
        std::vector<float> imp ((size_t) 65536, 0.0f);
        imp[0] = 1.0f;
        const auto out = rig.run (imp, imp);
        const auto spec = pstest::spectrumOf (out.l, 65536);
        double worst = 0.0;
        for (size_t k = 1; k < 32768; ++k)
        {
            const double hz = (double) k * kSr / 65536.0;
            if (hz >= 10.0 && hz <= 22000.0)
                worst = std::max (worst, std::abs (pstest::dbOf (std::abs (spec[k]))));
        }
        pstest::note (std::string (type == CrossoverType::Analog ? "Analog" : "Hybrid")
                          + " 4 bands neutral: max |magnitude| (dB)",
                      worst);
        CHECK (worst < 0.01);
        CHECK_EQ (rig.dyn->getLatencySamples(), 0);
    }

    // Digital 4 bands: delayed input
    {
        Rig rig;
        rig.set ("bands", 4.0f);
        rig.set ("xoverType", (float) CrossoverType::Digital);
        rig.setAllBands ("comp.ratio", 1.0f);
        rig.prepare();
        const auto l = pstest::pinkNoise (0.5, 2.0, kSr, 3);
        const auto out = rig.run (l, l);
        const int lat = rig.dyn->getLatencySamples();
        CHECK_EQ (lat, 3327);
        const double d = nullDb (out.l, l, lat, (size_t) lat + 8192);
        pstest::note ("Digital 4 bands neutral: null vs delayed input (dBFS)", d);
        CHECK (d < -90.0);
    }
}

//==============================================================================
TEST_CASE ("dyn. lookahead: impulse at the reported latency, limiter catches a fast transient")
{
    struct Case
    {
        int bands;
        CrossoverType type;
        float lookMs;
    };
    for (const Case c : { Case { 1, CrossoverType::Analog, 5.0f }, Case { 1, CrossoverType::Analog, 10.0f },
                          Case { 4, CrossoverType::Digital, 3.0f }, Case { 1, CrossoverType::Digital, 0.0f } })
    {
        Rig rig;
        rig.set ("bands", (float) c.bands);
        rig.set ("xoverType", (float) c.type);
        rig.set ("lookahead", c.lookMs);
        rig.setAllBands ("comp.ratio", 1.0f);
        rig.prepare();
        std::vector<float> imp ((size_t) 16384, 0.0f);
        imp[100] = 1.0f;
        const auto out = rig.run (imp, imp).l;
        size_t at = 0;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::abs (out[i]) > std::abs (out[at]))
                at = i;
        const int expectedLat = (int) std::lround (c.lookMs * 0.001 * kSr) + (c.type == CrossoverType::Digital ? 3327 : 0);
        CHECK_EQ (rig.dyn->getLatencySamples(), expectedLat);
        CHECK_EQ ((int) at - 100, expectedLat);
        if (c.bands == 1 && c.type == CrossoverType::Analog)
            CHECK (out[at] == 1.0f);
    }

    auto firstPeak = [] (float lookMs, double& staticOut) {
        Rig rig;
        rig.set ("detect", (float) DetectionMode::Peak);
        rig.set ("lookahead", lookMs);
        rig.setAllBands ("comp.on", 0.0f);
        rig.setAllBands ("lim.on", 1.0f);
        rig.setAllBands ("lim.thr", -12.0f);
        rig.setAllBands ("lim.ratio", 30.0f);
        rig.setAllBands ("lim.knee", 0.0f);
        rig.setAllBands ("lim.att", 0.3f);
        rig.setAllBands ("lim.rel", 50.0f);
        rig.prepare();
        auto in = std::vector<float> ((size_t) (0.2 * kSr), 0.0f);
        const auto burst = pstest::sine (2000.0, 0.0, 0.05, kSr);
        in.insert (in.end(), burst.begin(), burst.end());
        in.resize (in.size() + (size_t) (0.1 * kSr), 0.0f);
        const auto out = rig.run (in, in).l;
        const int lat = rig.dyn->getLatencySamples();
        const size_t onset = (size_t) (0.2 * kSr) + (size_t) lat;
        staticOut = pstest::dbOf (pstest::peakOf (out, onset + 960, 480));
        return pstest::dbOf (pstest::peakOf (out, onset, 48));   // first millisecond
    };
    // Peak detection on a 2 kHz burst, attack 0.3 ms: the first cycles overshoot by ~9 dB without lookahead
    double s0 = 0.0, s2 = 0.0, s5 = 0.0;
    const double noLook = firstPeak (0.0f, s0);
    const double look2 = firstPeak (2.0f, s2);
    const double look5 = firstPeak (5.0f, s5);
    const double staticCurve = -12.0 + 12.0 / 30.0;
    pstest::note ("limiter, no lookahead: first-ms peak (dBFS)", noLook);
    pstest::note ("limiter, 2 ms lookahead: first-ms peak (dBFS)", look2);
    pstest::note ("limiter, 5 ms lookahead: first-ms peak (dBFS)", look5);
    pstest::note ("limiter steady state (dBFS)", s5);
    CHECK_NEAR (s5, staticCurve, 0.5);
    CHECK (look2 < staticCurve + 1.5);
    CHECK (look5 < staticCurve + 1.0);
    CHECK (noLook > look2 + 6.0);
}

//==============================================================================
namespace
{
double documentedMakeup (const DynamicsBandParams& bp, double lo, double hi, DetectionMode det)
{
    const double phi = std::max (0.01, std::log2 (hi / lo) / std::log2 (1000.0));
    const double lref = -18.0 + 10.0 * std::log10 (phi) + (det == DetectionMode::Rms ? 0.0 : 3.0);
    const GainCurve cc { bp.comp.thresholdDb, bp.comp.ratio, bp.comp.kneeDb, false };
    const GainCurve lc { bp.lim.thresholdDb, bp.lim.ratio, bp.lim.kneeDb, true };
    double acc = 0.0, wsum = 0.0;
    for (double d = -12.0; d <= 12.0001; d += 0.5)
    {
        const double w = std::exp (-d * d / 32.0);
        const float level = (float) (lref + d);
        const float c = bp.comp.on ? cc.gainDb (level) : 0.0f;
        const float l = bp.lim.on ? lc.gainDb (level + c) : 0.0f;
        acc += w * (c + l);
        wsum += w;
    }
    return std::clamp (-acc / wsum, -24.0, 24.0);
}
} // namespace

TEST_CASE ("dyn. auto gain equals the documented static makeup")
{
    // single band, DC well below the thresholds: the output gain is the makeup alone
    struct Case
    {
        float thr, ratio, knee;
        bool limOn;
        float lThr, lRatio;
        DetectionMode det;
    };
    for (const Case c : { Case { -30.0f, 4.0f, 6.0f, false, 0.0f, 10.0f, DetectionMode::Envelope },
                          Case { -20.0f, 2.0f, 0.0f, true, -15.0f, 20.0f, DetectionMode::Rms },
                          Case { -24.0f, 10.0f, 12.0f, false, 0.0f, 10.0f, DetectionMode::Peak },
                          Case { -10.0f, -3.0f, 0.0f, false, 0.0f, 10.0f, DetectionMode::Envelope } })
    {
        Rig rig;
        rig.set ("autoGain", 1.0f);
        rig.set ("detect", (float) c.det);
        rig.setAllBands ("comp.thr", c.thr);
        rig.setAllBands ("comp.ratio", c.ratio);
        rig.setAllBands ("comp.knee", c.knee);
        rig.setAllBands ("lim.on", c.limOn ? 1.0f : 0.0f);
        rig.setAllBands ("lim.thr", c.lThr);
        rig.setAllBands ("lim.ratio", c.lRatio);
        rig.prepare();

        DynamicsParams p;
        readDynamicsParams (rig.values.data(), p);
        const double want = documentedMakeup (p.bands[0][0], 20.0, 20000.0, c.det);

        // a level where the curves do nothing: upward compression lifts quiet signals, so measure it above its
        // threshold
        const double level = c.ratio < 0.0f ? -3.0 : -70.0;
        const auto in = dc (level, 0.5);
        const auto out = rig.run (in).l;
        const double got = gainDbAt (in, out, in.size() - 1);
        pstest::note ("auto gain makeup (dB) measured", got);
        pstest::note ("auto gain makeup (dB) documented", want);
        CHECK_NEAR (got, want, 1.0);
        CHECK (std::abs (want) > 0.5);

        // static: independent of the material level (a louder DC below the threshold gets the same makeup)
        if (c.ratio > 0.0f)
        {
            const auto in2 = dc (level + 20.0, 0.5);
            const auto out2 = rig.run (in2).l;
            CHECK_NEAR (gainDbAt (in2, out2, in2.size() - 1), got, 1e-3);
        }
    }

    // three bands, one tone per band at -70 dBFS: band makeups differ by their pink-noise share
    {
        Rig rig;
        rig.set ("autoGain", 1.0f);
        rig.set ("bands", 3.0f);
        rig.set ("xover1", 200.0f);
        rig.set ("xover2", 5000.0f);
        rig.set ("detect", (float) DetectionMode::Rms);
        const float thr[3] { -40.0f, -25.0f, -30.0f }, ratio[3] { 3.0f, 2.0f, 6.0f };
        for (int b = 0; b < 3; ++b)
        {
            rig.setBand (0, b + 1, "comp.thr", thr[b]);
            rig.setBand (0, b + 1, "comp.ratio", ratio[b]);
            rig.setBand (0, b + 1, "comp.knee", 6.0f);
        }
        rig.prepare();
        DynamicsParams p;
        readDynamicsParams (rig.values.data(), p);
        const double edges[4] { 20.0, 200.0, 5000.0, 20000.0 };
        const double freqs[3] { 60.0, 1000.0, 15000.0 };
        for (int b = 0; b < 3; ++b)
        {
            const auto in = pstest::sine (freqs[b], -70.0, 1.0, kSr);
            const auto out = rig.run (in).l;
            const size_t from = (size_t) (0.5 * kSr), len = (size_t) (0.5 * kSr);
            const double got = pstest::dbOf (std::abs (pstest::toneOf (out, from, len, freqs[b], kSr)))
                               - pstest::dbOf (std::abs (pstest::toneOf (in, from, len, freqs[b], kSr)));
            const double want = documentedMakeup (p.bands[0][b], edges[b], edges[b + 1], DetectionMode::Rms);
            pstest::note ("band " + std::to_string (b + 1) + " makeup measured (dB)", got);
            pstest::note ("band " + std::to_string (b + 1) + " makeup documented (dB)", want);
            CHECK_NEAR (got, want, 1.0);
        }
    }
}

//==============================================================================
TEST_CASE ("dyn. parallel mix: 0 % is the aligned dry signal, 50 % blends linearly")
{
    Rig rig;
    rig.set ("lookahead", 3.0f);
    rig.set ("detect", (float) DetectionMode::Peak);
    rig.setAllBands ("comp.thr", -40.0f);
    rig.setAllBands ("comp.ratio", 10.0f);
    rig.setAllBands ("comp.knee", 0.0f);
    rig.setAllBands ("lim.on", 1.0f);
    rig.setAllBands ("lim.thr", -30.0f);
    rig.setAllBands ("mix", 0.0f);
    rig.prepare();
    const auto l = pstest::pinkNoise (0.5, 2.0, kSr, 11);
    const auto r = pstest::pinkNoise (0.5, 2.0, kSr, 12);
    const auto out = rig.run (l, r);
    const int lat = rig.dyn->getLatencySamples();
    CHECK_EQ (lat, 144);
    const double d = std::max (nullDb (out.l, l, lat, 0), nullDb (out.r, r, lat, 0));
    pstest::note ("mix 0 %: null vs delayed input (dBFS)", d);
    CHECK (d < -140.0);

    // 50 % on DC: g = 0.5 + 0.5 * wet
    Rig half;
    half.set ("detect", (float) DetectionMode::Peak);
    half.setAllBands ("comp.thr", -30.0f);
    half.setAllBands ("comp.ratio", 4.0f);
    half.setAllBands ("comp.knee", 0.0f);
    half.setAllBands ("mix", 50.0f);
    half.setAllBands ("gain", 3.0f);
    half.prepare();
    const auto in = dc (-10.0, 0.5);
    const auto o = half.run (in).l;
    const double wet = std::pow (10.0, (-20.0 * 0.75) / 20.0);
    const double want = 20.0 * std::log10 ((0.5 + 0.5 * wet) * std::pow (10.0, 3.0 / 20.0));
    CHECK_NEAR (gainDbAt (in, o, in.size() - 1), want, 0.01);
}

//==============================================================================
TEST_CASE ("dyn. band solo outputs only that band, detector solo outputs the detector signal")
{
    const float xo[3] { 300.0f, 3000.0f, 10000.0f };
    const auto l = pstest::pinkNoise (0.3, 1.5, kSr, 21);
    const auto r = pstest::pinkNoise (0.3, 1.5, kSr, 22);

    // reference bands
    Crossover x;
    x.prepare (kSr, 512);
    x.setup (3, xo, CrossoverType::Analog);
    std::vector<float> band2l (l.size()), band2r (l.size());
    {
        std::vector<float> buf (3 * 2 * 512);
        float* chans[kMaxBands][2] {};
        float* const* ptrs[kMaxBands] {};
        for (int b = 0; b < 3; ++b)
        {
            chans[b][0] = buf.data() + (b * 2) * 512;
            chans[b][1] = buf.data() + (b * 2 + 1) * 512;
            ptrs[b] = chans[b];
        }
        for (size_t s = 0; s < l.size(); s += 512)
        {
            const int n = (int) std::min ((size_t) 512, l.size() - s);
            const float* in[2] { l.data() + s, r.data() + s };
            x.process (in, ptrs, n);
            std::copy (chans[1][0], chans[1][0] + n, band2l.begin() + (std::ptrdiff_t) s);
            std::copy (chans[1][1], chans[1][1] + n, band2r.begin() + (std::ptrdiff_t) s);
        }
    }

    Rig rig;
    rig.set ("bands", 3.0f);
    rig.set ("xover1", xo[0]);
    rig.set ("xover2", xo[1]);
    rig.setAllBands ("comp.ratio", 1.0f);
    rig.setBand (0, 1, "comp.ratio", 8.0f);   // the other bands are processed hard (must not leak)
    rig.setBand (0, 1, "comp.thr", -40.0f);
    rig.setBand (0, 3, "gain", 12.0f);
    rig.prepare();
    ModuleMonitor m;
    m.soloBand = 1;
    rig.dyn->setMonitor (m);
    const auto out = rig.run (l, r);
    const size_t from = (size_t) (0.1 * kSr);
    const double d = std::max (nullDb (out.l, band2l, 0, from), nullDb (out.r, band2r, 0, from));
    pstest::note ("band solo vs reference band 2 (dBFS)", d);
    CHECK (d < -100.0);

    // detector solo: 1 band, HP 24 dB/oct at 150 Hz, lookahead 2 ms
    Rig det;
    det.set ("detFilter", (float) DetectionFilter::HighPass);
    det.set ("detFreq", 150.0f);
    det.set ("detSlope", 2.0f);
    det.set ("lookahead", 2.0f);
    det.prepare();
    ModuleMonitor dm;
    dm.soloDetectionFilter = true;
    det.dyn->setMonitor (dm);
    const auto dout = det.run (l, r);
    Biquad hp[2][2];
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < 2; ++k)
            hp[c][k].setCoeffs (design::highPass (kSr, 150.0, design::butterworthQ (4, k)));
    std::vector<float> refL (l.size()), refR (l.size());
    for (size_t i = 0; i < l.size(); ++i)
    {
        refL[i] = (float) hp[0][1].processSample (hp[0][0].processSample ((double) l[i]));
        refR[i] = (float) hp[1][1].processSample (hp[1][0].processSample ((double) r[i]));
    }
    const int lat = det.dyn->getLatencySamples();
    const double dd = std::max (nullDb (dout.l, refL, lat, from), nullDb (dout.r, refR, lat, from));
    pstest::note ("detector solo vs reference HP 24 dB/oct (dBFS)", dd);
    CHECK (dd < -100.0);
}

TEST_CASE ("dyn. detection filter changes what the detector hears (HP and tilt)")
{
    // a 60 Hz tone at -6 dBFS against a threshold of -20: High Pass at 500 Hz hides it from the detector
    auto gainWith = [] (DetectionFilter f, float tilt) {
        Rig rig;
        rig.set ("detect", (float) DetectionMode::Rms);
        rig.set ("detFilter", (float) f);
        rig.set ("detFreq", 500.0f);
        rig.set ("detSlope", 3.0f);
        rig.set ("detTilt", tilt);
        rig.setAllBands ("comp.thr", -40.0f);
        rig.setAllBands ("comp.ratio", 2.0f);
        rig.setAllBands ("comp.knee", 0.0f);
        rig.prepare();
        const auto in = pstest::sine (62.5, -6.0, 2.0, kSr);
        const auto out = rig.run (in).l;
        const size_t from = (size_t) kSr, len = (size_t) (0.96 * kSr);
        return pstest::dbOf (std::abs (pstest::toneOf (out, from, len, 62.5, kSr)))
               - pstest::dbOf (std::abs (pstest::toneOf (in, from, len, 62.5, kSr)));
    };
    const double none = gainWith (DetectionFilter::None, 0.0f);
    const double hp = gainWith (DetectionFilter::HighPass, 0.0f);
    const double tiltUp = gainWith (DetectionFilter::Tilt, 6.0f);
    const double tiltDown = gainWith (DetectionFilter::Tilt, -3.0f);
    pstest::note ("62.5 Hz tone gain, no filter (dB)", none);
    pstest::note ("62.5 Hz tone gain, HP 500 Hz 48 dB/oct (dB)", hp);
    pstest::note ("62.5 Hz tone gain, tilt +6 dB/oct around 500 Hz (dB)", tiltUp);
    pstest::note ("62.5 Hz tone gain, tilt -3 dB/oct around 500 Hz (dB)", tiltDown);
    // no filter: level -9 dB RMS, 31 dB over -> -15.5 dB
    CHECK_NEAR (none, -15.5, 0.2);
    CHECK (hp > -0.01);
    // +6 dB/oct: the tone is 3 octaves below the pivot -> detector -18 dB -> 13 dB over -> -6.5 dB
    CHECK_NEAR (tiltUp, -0.5 * (31.0 - 18.0), 0.6);
    // -3 dB/oct: +9 dB at 62.5 Hz -> 40 dB over -> -20 dB
    CHECK_NEAR (tiltDown, -0.5 * (31.0 + 9.0), 0.6);
}

//==============================================================================
TEST_CASE ("dyn. M/S: the channels are independent; Stereo is linked")
{
    const auto loud = pstest::sine (440.0, -3.0, 1.5, kSr);
    const auto quiet = pstest::sine (550.0, -40.0, 1.5, kSr);
    const size_t from = (size_t) kSr, len = (size_t) (0.5 * kSr);
    auto toneGain = [&] (const std::vector<float>& in, const std::vector<float>& out, double f) {
        return pstest::dbOf (std::abs (pstest::toneOf (out, from, len, f, kSr)))
               - pstest::dbOf (std::abs (pstest::toneOf (in, from, len, f, kSr)));
    };

    // M/S: A (mid) compresses, B (side) neutral
    {
        Rig rig;
        rig.set ("detect", (float) DetectionMode::Rms);
        rig.setAllBands ("comp.ratio", 1.0f);
        rig.setBand (0, 1, "comp.ratio", 4.0f);
        rig.setBand (0, 1, "comp.thr", -30.0f);
        rig.setBand (0, 1, "comp.knee", 0.0f);
        rig.prepare();
        const auto out = rig.run (loud, loud, ChannelMode::MidSide);
        CHECK (out.r == loud);   // side untouched although it is loud
        CHECK_NEAR (toneGain (loud, out.l, 440.0), -0.75 * (30.0 - 6.0103), 0.2);
    }
    // M/S: B compresses on a loud side while the mid is quiet and neutral
    {
        Rig rig;
        rig.set ("detect", (float) DetectionMode::Rms);
        rig.setAllBands ("comp.ratio", 1.0f);
        rig.setBand (1, 1, "comp.ratio", 4.0f);
        rig.setBand (1, 1, "comp.thr", -30.0f);
        rig.setBand (1, 1, "comp.knee", 0.0f);
        rig.prepare();
        const auto out = rig.run (quiet, loud, ChannelMode::MidSide);
        CHECK (out.l == quiet);
        CHECK_NEAR (toneGain (loud, out.r, 440.0), -0.75 * (30.0 - 6.0103), 0.2);
        const auto& mt = rig.dyn->getMeters();
        CHECK_NEAR (mt.compGainDb[1][0].load(), -0.75 * (30.0 - 6.0103), 0.3);
        CHECK_NEAR (mt.compGainDb[0][0].load(), 0.0, 1e-6);
    }
    // Stereo: one gain for both channels from max (|L|, |R|)
    {
        Rig rig;
        rig.set ("detect", (float) DetectionMode::Rms);
        rig.setAllBands ("comp.thr", -30.0f);
        rig.setAllBands ("comp.ratio", 4.0f);
        rig.setAllBands ("comp.knee", 0.0f);
        rig.prepare();
        const auto out = rig.run (quiet, loud, ChannelMode::Stereo);
        const double gq = toneGain (quiet, out.l, 550.0), gl = toneGain (loud, out.r, 440.0);
        pstest::note ("stereo linked: quiet channel gain (dB)", gq);
        pstest::note ("stereo linked: loud channel gain (dB)", gl);
        CHECK_NEAR (gq, gl, 0.1);
        CHECK (gl < -10.0);
    }
}

//==============================================================================
TEST_CASE ("dyn. parameter moves are smooth (no zipper steps)")
{
    // DC at -10 dBFS, fast attack: threshold, ratio, band gain, mix and output gain jump between blocks
    Rig rig;
    rig.set ("detect", (float) DetectionMode::Peak);
    rig.setAllBands ("comp.att", 0.1f);
    rig.setAllBands ("comp.rel", 5.0f);
    rig.setAllBands ("comp.knee", 0.0f);
    rig.prepare();
    const auto in = dc (-10.0, 3.0);
    int k = 0;
    const auto out = rig.run (in, in, ChannelMode::Stereo, [&] (size_t start) {
        if ((start / 512) % 20 != 0)
            return;
        ++k;
        rig.setAllBands ("comp.thr", k % 2 == 0 ? 0.0f : -40.0f);
        rig.setAllBands ("comp.ratio", k % 3 == 0 ? 2.0f : 20.0f);
        rig.setAllBands ("gain", k % 4 == 0 ? 0.0f : -12.0f);
        rig.setAllBands ("mix", k % 5 == 0 ? 30.0f : 100.0f);
        rig.set ("gain", k % 2 == 0 ? 6.0f : -6.0f);
        rig.setAllBands ("comp.on", k % 7 == 0 ? 0.0f : 1.0f);
    }).l;
    // linear gain per sample: the largest move here is the threshold ramp (28.5 dB of gain reduction in 20 ms at an
    // output gain of x2: ~0.007 per sample at its steepest); an unsmoothed jump would move by ~1
    double maxStep = 0.0, totalMove = 0.0;
    for (size_t i = 1; i < out.size(); ++i)
    {
        const double step = std::abs ((double) out[i] - (double) out[i - 1]) / (double) in[i];
        maxStep = std::max (maxStep, step);
        totalMove += step;
    }
    pstest::note ("largest per-sample linear gain step", maxStep);
    pstest::note ("total linear gain movement", totalMove);
    CHECK (totalMove > 10.0);   // the parameters really moved the gain
    CHECK (maxStep < 0.01);
}

TEST_CASE ("dyn. adding a band keeps the gain reduction (no level jump)")
{
    // a compressed 2 kHz tone; the band count goes 1 -> 2 (split at 500 Hz): the tone moves to the new band 2, which
    // inherits the gain state of the band it was split from. Compared with the same switch at ratio 1 (the crossover's
    // own 1 -> 2 band crossfade changes the level of the sum a little while it runs).
    auto levels = [] (float ratio, double& before, double& lo, double& hi) {
        Rig rig;
        rig.set ("detect", (float) DetectionMode::Rms);
        rig.set ("xover1", 500.0f);
        rig.setAllBands ("comp.thr", -30.0f);
        rig.setAllBands ("comp.ratio", ratio);
        rig.setAllBands ("comp.knee", 0.0f);
        rig.setAllBands ("comp.att", 20.0f);
        rig.setAllBands ("comp.rel", 2000.0f);
        rig.prepare();
        const auto in = pstest::sine (2000.0, -6.0, 2.0, kSr);
        const size_t switchAt = (size_t) (1.5 * kSr) / 512 * 512;
        const auto out = rig.run (in, in, ChannelMode::Stereo, [&] (size_t start) {
            if (start == switchAt)
                rig.set ("bands", 2.0f);
        }).l;
        before = pstest::dbOf (pstest::peakOf (out, switchAt - 4800, 4800));
        lo = 0.0;
        hi = -300.0;
        for (size_t i = switchAt; i + 48 < switchAt + 9600; i += 24)
        {
            const double p = pstest::dbOf (pstest::peakOf (out, i, 48)) - before;
            lo = std::min (lo, p);
            hi = std::max (hi, p);
        }
    };
    double b1, lo1, hi1, b4, lo4, hi4;
    levels (1.0f, b1, lo1, hi1);
    levels (4.0f, b4, lo4, hi4);
    pstest::note ("ratio 1: level change during the split, min (dB)", lo1);
    pstest::note ("ratio 1: level change during the split, max (dB)", hi1);
    pstest::note ("ratio 4: level before the split (dBFS)", b4);
    pstest::note ("ratio 4: level change during the split, min (dB)", lo4);
    pstest::note ("ratio 4: level change during the split, max (dB)", hi4);
    CHECK (b4 < -20.0);
    CHECK (hi4 < hi1 + 0.3);
    CHECK (lo4 > lo1 - 0.3);
}

//==============================================================================
TEST_CASE ("dyn. detector solo follows the audio lookahead crossfade")
{
    // With no detection filter and neutral curves, these are the same signal even while the tap changes.
    auto render = [] (bool detectorSolo) {
        Rig rig (kSr, 256);
        rig.setAllBands ("comp.ratio", 1.0f);
        rig.prepare();
        ModuleMonitor monitor;
        monitor.soloDetectionFilter = detectorSolo;
        rig.dyn->setMonitor (monitor);
        const auto l = pstest::pinkNoise (0.4, 1.0, kSr, 147);
        const auto r = pstest::pinkNoise (0.4, 1.0, kSr, 148);
        return rig.run (l, r, ChannelMode::Stereo, [&] (size_t start) {
            constexpr float delays[] { 0.0f, 5.0f, 2.0f, 10.0f, 0.0f };
            rig.set ("lookahead", delays[(start / 4096) % 5]);
        });
    };
    const auto audio = render (false), detector = render (true);
    const double error = std::max (nullDb (audio.l, detector.l, 0, 0), nullDb (audio.r, detector.r, 0, 0));
    pstest::note ("detector solo vs audio during delay changes, maximum error (dBFS)", error);
    CHECK (error < -120.0);
}

TEST_CASE ("dyn. adding a band and lookahead together does not leave the band muted")
{
    Rig rig;
    rig.set ("xoverType", (float) CrossoverType::Digital);
    rig.set ("xover1", 500.0f);
    rig.setAllBands ("comp.ratio", 1.0f);
    rig.prepare();
    const auto in = pstest::sine (2000.0, -6.0, 2.0, kSr);
    const size_t switchAt = 49152;
    const auto out = rig.run (in, in, ChannelMode::Stereo, [&] (size_t start) {
        if (start == switchAt)
        {
            rig.set ("bands", 2.0f);
            rig.set ("lookahead", 5.0f);
        }
    }).l;
    const size_t from = 72000, length = 24000;
    const double gain = pstest::dbOf (std::abs (pstest::toneOf (out, from, length, 2000.0, kSr)))
                        - pstest::dbOf (std::abs (pstest::toneOf (in, from, length, 2000.0, kSr)));
    pstest::note ("neutral high tone after simultaneous band/delay addition, gain (dB)", gain);
    CHECK_NEAR (gain, 0.0, 0.05);
    CHECK_EQ (rig.dyn->getLatencySamples(), 3327 + 240);
}

TEST_CASE ("dyn. band layout changes retain queued lookahead audio")
{
    Rig rig;
    rig.set ("xoverType", (float) CrossoverType::Digital);
    rig.set ("xover1", 500.0f);
    rig.set ("lookahead", 5.0f);
    rig.setAllBands ("comp.ratio", 1.0f);
    rig.prepare();
    const auto in = pstest::pinkNoise (0.4, 3.0, kSr, 179);
    const auto out = rig.run (in, in, ChannelMode::Stereo, [&] (size_t start) {
        // Exercise adding, merging, and reopening bands while every delay contains live audio.
        if (start >= 32768 && start % 16384 == 0)
        {
            constexpr float counts[] { 2.0f, 4.0f, 2.0f, 1.0f, 4.0f, 2.0f, 1.0f };
            const size_t index = (start - 32768) / 16384;
            if (index < 7)
                rig.set ("bands", counts[index]);
        }
    }).l;
    const int latency = rig.dyn->getLatencySamples();
    double error = 0.0;
    for (size_t i = 32768 - 512; i < out.size(); ++i)
        error = std::max (error, std::abs ((double) out[i] - (double) in[i - (size_t) latency]));
    pstest::note ("neutral Digital 1/2/4 band edits with lookahead, transition null (dBFS)", pstest::dbOf (error));
    CHECK (error < 1.0e-4);
}

TEST_CASE ("dyn. automated detection filter is applied before the crossover")
{
    for (int block : { 128, 512 })
    {
        const auto input = pstest::pinkNoise (0.3, 2.0, kSr, 181);
        auto render = [&] (bool digital) {
            Rig rig (kSr, block);
            rig.set ("bands", digital ? 3.0f : 1.0f);
            rig.set ("xoverType", (float) (digital ? CrossoverType::Digital : CrossoverType::Analog));
            rig.setAllBands ("comp.ratio", 1.0f);
            rig.prepare();
            ModuleMonitor monitor;
            monitor.soloDetectionFilter = true;
            rig.dyn->setMonitor (monitor);
            return rig.run (input, input, ChannelMode::Stereo, [&] (size_t start) {
                if (start % 8192 != 0)
                    return;
                switch (start / 8192)
                {
                    case 1:
                        rig.set ("detFilter", (float) DetectionFilter::HighPass);
                        rig.set ("detFreq", 400.0f);
                        rig.set ("detSlope", 2.0f);
                        break;
                    case 2: rig.set ("detSlope", 3.0f); break;
                    case 3: rig.set ("detFreq", 1500.0f); break;
                    case 4:
                        rig.set ("detFilter", (float) DetectionFilter::Tilt);
                        rig.set ("detTilt", 3.0f);
                        rig.set ("detFreq", 1000.0f);
                        break;
                    case 5:
                        rig.set ("detTilt", -3.0f);
                        rig.set ("detFreq", 300.0f);
                        break;
                    case 6: rig.set ("detFilter", (float) DetectionFilter::None); break;
                    case 7:
                        rig.set ("detFilter", (float) DetectionFilter::HighPass);
                        rig.set ("detSlope", 1.0f);
                        rig.set ("detFreq", 100.0f);
                        break;
                    case 8: rig.set ("detFreq", 5000.0f); break;
                    default: break;
                }
            }).l;
        };
        const auto filteredInput = render (false), multibandDetector = render (true);
        const double error = nullDb (multibandDetector, filteredInput, 3327, 4096);
        pstest::note ("filter before Digital crossover, automated HP/Tilt, block " + std::to_string (block)
                          + ": maximum null error (dBFS)",
                      error);
        CHECK (error < -80.0);
    }
}

TEST_CASE ("dyn. band layout changes crossfade distinct band gains")
{
    for (float lookahead : { 0.0f, 5.0f })
    {
        Rig rig;
        rig.set ("xoverType", (float) CrossoverType::Digital);
        rig.set ("xover1", 250.0f);
        rig.set ("xover2", 500.0f);
        rig.set ("xover3", 750.0f);
        rig.set ("lookahead", lookahead);
        rig.setAllBands ("comp.ratio", 1.0f);
        rig.setBand (0, 1, "gain", 0.0f);
        rig.setBand (0, 2, "gain", -24.0f);
        rig.setBand (0, 3, "gain", 6.0f);
        rig.setBand (0, 4, "gain", -12.0f);
        rig.prepare();
        // This tone moves through the last band of each layout. Digital bands have the same phase, so dividing
        // by the delayed input removes the sine's natural sample changes and exposes any instantaneous gain jump.
        auto input = pstest::sine (4000.0, 0.0, 3.0, kSr);
        for (float& value : input)
            value *= 0.25f;
        constexpr size_t interval = 24576;
        const auto output = rig.run (input, input, ChannelMode::Stereo, [&] (size_t start) {
            if (start > 0 && start % interval == 0)
            {
                constexpr float counts[] { 2.0f, 4.0f, 2.0f, 1.0f };
                const size_t index = start / interval - 1;
                if (index < 4)
                    rig.set ("bands", counts[index]);
            }
        }).l;
        const size_t latency = (size_t) rig.dyn->getLatencySamples();
        double largestStep = 0.0;
        for (size_t i = 8192 + latency; i < output.size(); ++i)
        {
            const double ref = input[i - latency], previousRef = input[i - latency - 1];
            if (std::abs (ref) >= 0.125 && std::abs (previousRef) >= 0.125)
            {
                const double gain = (double) output[i] / ref;
                const double previousGain = (double) output[i - 1] / previousRef;
                largestStep = std::max (largestStep, std::abs (gain - previousGain));
            }
        }
        pstest::note ("distinct band gains, lookahead " + std::to_string (lookahead)
                          + " ms: largest per-sample linear gain step",
                      largestStep);
        CHECK (largestStep < 0.008);

        constexpr double expected[] { 0.0, -24.0, -12.0, -24.0, 0.0 };
        for (size_t segment = 0; segment < 5; ++segment)
        {
            const size_t end = segment < 4 ? (segment + 1) * interval : output.size();
            const size_t start = end - 4800;
            const double gain = pstest::dbOf (std::abs (pstest::toneOf (output, start, 4800, 4000.0, kSr)))
                                - pstest::dbOf (std::abs (pstest::toneOf (input, start, 4800, 4000.0, kSr)));
            CHECK_NEAR (gain, expected[segment], 0.08);
        }
    }
}

TEST_CASE ("dyn. unlinking preserves set B compressor and limiter attack times")
{
    for (bool limiter : { false, true })
    {
        Rig rig;
        rig.set ("detect", (float) DetectionMode::Peak);
        rig.setAllBands ("comp.on", limiter ? 0.0f : 1.0f);
        rig.setAllBands ("lim.on", limiter ? 1.0f : 0.0f);
        const std::string stage = limiter ? "lim." : "comp.";
        rig.setAllBands (stage + "thr", -30.0f);
        rig.setAllBands (stage + "ratio", 4.0f);
        rig.setAllBands (stage + "knee", 0.0f);
        rig.setBand (0, 1, stage + "att", 0.1f);
        rig.setBand (1, 1, stage + "att", 100.0f);
        rig.prepare();
        rig.run (dc (-60.0, 0.1));

        // Inspect the independent gain computer, unaffected by the audible channel-mode crossfade.
        std::vector<float> left (512, (float) pstest::dbfsToAmplitude (-60.0));
        std::vector<float> right (512, (float) pstest::dbfsToAmplitude (-3.0));
        float* channels[2] { left.data(), right.data() };
        ProcessContext ctx;
        ctx.channelMode = ChannelMode::LeftRight;
        rig.dyn->setParams (rig.values.data());
        rig.dyn->process (channels, 512, ctx);
        const auto& meters = rig.dyn->getMeters();
        const double actual = limiter ? meters.limGainDb[1][0].load() : meters.compGainDb[1][0].load();
        const double expected = -20.25 * (1.0 - std::exp (-512.0 / (0.100 * kSr)));
        pstest::note (stage + "set B gain after first unlinked block (dB)", actual);
        CHECK_NEAR (actual, expected, 0.002);
    }
}

TEST_CASE ("dyn. channel linking and unlinking crossfade different set gains")
{
    Rig rig (kSr, 256);
    rig.setAllBands ("comp.ratio", 1.0f);
    rig.setBand (1, 1, "gain", -24.0f);
    rig.prepare();
    std::vector<float> left (48000, 0.25f), right (48000, 0.25f);
    const size_t linkAt = 16384, unlinkAt = 32768;
    for (size_t start = 0; start < left.size(); start += 256)
    {
        ProcessContext ctx;
        ctx.channelMode = start >= linkAt && start < unlinkAt ? ChannelMode::Stereo : ChannelMode::LeftRight;
        rig.dyn->setParams (rig.values.data());
        float* channels[2] { left.data() + start, right.data() + start };
        rig.dyn->process (channels, (int) std::min ((size_t) 256, left.size() - start), ctx);
    }
    double largestGainStep = 0.0;
    for (size_t i = 1024; i < right.size(); ++i)
        largestGainStep = std::max (largestGainStep, std::abs ((double) right[i] - (double) right[i - 1]) / 0.25);
    const double independent = 0.25 * std::pow (10.0, -24.0 / 20.0);
    pstest::note ("channel mode change: largest per-sample linear gain step", largestGainStep);
    CHECK (largestGainStep < 0.005);
    CHECK_NEAR (right[linkAt - 1], independent, 1.0e-6);
    CHECK_NEAR (right[unlinkAt - 1], 0.25, 1.0e-6);
    CHECK_NEAR (right.back(), independent, 1.0e-6);
}

TEST_CASE ("dyn. channel linking crossfades the left detector under compression")
{
    Rig rig (kSr, 256);
    rig.set ("detect", (float) DetectionMode::Peak);
    rig.setAllBands ("comp.thr", -20.0f);
    rig.setAllBands ("comp.ratio", 30.0f);
    rig.setAllBands ("comp.knee", 0.0f);
    rig.setAllBands ("comp.att", 0.1f);
    rig.setAllBands ("comp.rel", 1.0f);
    rig.prepare();
    const std::vector<float> quiet (12000, 0.05f), loud (12000, 0.5f);
    const auto independent = rig.run (quiet, loud, ChannelMode::LeftRight);
    const double linkedGain = std::pow (5.0, 1.0 / 30.0 - 1.0);
    CHECK_NEAR (independent.l.back(), 0.05, 1.0e-6);
    CHECK_NEAR (independent.r.back(), 0.5 * linkedGain, 1.0e-6);

    const auto linked = rig.run (quiet, loud, ChannelMode::Stereo);
    const auto unlinked = rig.run (quiet, loud, ChannelMode::LeftRight);
    const double firstStep = std::abs ((double) linked.l.front() - (double) independent.l.back());
    double largestStep = firstStep;
    double rightDeviation = 0.0, rightPeak = 0.0;
    auto measureSteps = [&] (const Stereo& block, float previousLeft, float previousRight) {
        for (size_t i = 0; i < block.l.size(); ++i)
        {
            largestStep = std::max (largestStep, std::abs ((double) block.l[i] - (double) previousLeft));
            largestStep = std::max (largestStep, std::abs ((double) block.r[i] - (double) previousRight));
            rightDeviation = std::max (rightDeviation, std::abs ((double) block.r[i] - 0.5 * linkedGain));
            rightPeak = std::max (rightPeak, (double) block.r[i]);
            previousLeft = block.l[i];
            previousRight = block.r[i];
        }
    };
    measureSteps (linked, independent.l.back(), independent.r.back());
    measureSteps (unlinked, linked.l.back(), linked.r.back());
    pstest::note ("linking differently driven detectors: first left sample step", firstStep);
    pstest::note ("link/unlink under compression: largest DC sample step", largestStep);
    pstest::note ("dominant right channel: gain hump during link/unlink (dB)",
                  20.0 * std::log10 (rightPeak / (0.5 * linkedGain)));
    CHECK (firstStep < 0.001);
    CHECK (largestStep < 0.002);
    // With identical A/B settings the dominant right channel has the same detector level in either mode.
    // It must retain its gain throughout the fade, not merely arrive at the same settled endpoints.
    CHECK (rightDeviation < 1.0e-6);
    CHECK_NEAR (linked.l.back(), 0.05 * linkedGain, 1.0e-6);
    CHECK_NEAR (linked.r.back(), 0.5 * linkedGain, 1.0e-6);
    CHECK_NEAR (unlinked.l.back(), 0.05, 1.0e-6);
    CHECK_NEAR (unlinked.r.back(), 0.5 * linkedGain, 1.0e-6);
}

TEST_CASE ("dyn. reset clears adaptive release history")
{
    auto configure = [] (Rig& rig) {
        rig.set ("detect", (float) DetectionMode::Peak);
        rig.set ("adaptRel", 1.0f);
        rig.setAllBands ("comp.thr", -20.0f);
        rig.setAllBands ("comp.ratio", -3.0f);
        rig.setAllBands ("comp.knee", 0.0f);
        rig.setAllBands ("comp.rel", 100.0f);
        rig.prepare();
    };
    Rig used, fresh;
    configure (used);
    configure (fresh);
    used.run (dc (-6.0, 3.0));   // sustained material selects the slow adaptive release
    used.dyn->reset();
    const auto input = dc (-30.0, 0.02);
    const auto afterReset = used.run (input).l, firstRun = fresh.run (input).l;
    pstest::note ("adaptive reset vs fresh output, maximum error (dBFS)", nullDb (afterReset, firstRun, 0, 0));
    CHECK (afterReset == firstRun);
}

TEST_CASE ("dyn. changing detection modes keeps a settled DC level")
{
    Rig rig (kSr, 256);
    rig.set ("detect", (float) DetectionMode::Peak);
    rig.setAllBands ("comp.thr", -40.0f);
    rig.setAllBands ("comp.ratio", 30.0f);
    rig.setAllBands ("comp.knee", 0.0f);
    rig.setAllBands ("comp.att", 0.1f);
    rig.setAllBands ("comp.rel", 1.0f);
    rig.prepare();
    rig.run (dc (-6.0, 0.5));
    const auto input = dc (-6.0, 0.5);
    const auto out = rig.run (input, input, ChannelMode::Stereo, [&] (size_t start) {
        constexpr DetectionMode modes[] { DetectionMode::Rms, DetectionMode::Envelope, DetectionMode::Peak };
        rig.set ("detect", (float) modes[(start / 4096) % 3]);
    }).l;
    const double expected = -34.0 * (1.0 - 1.0 / 30.0);
    double worst = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
        worst = std::max (worst, std::abs (gainDbAt (input, out, i) - expected));
    pstest::note ("constant DC: gain excursion while switching detection modes (dB)", worst);
    CHECK (worst < 0.01);
}

TEST_CASE ("dyn. bypassed band meters and reset meters show no gain change")
{
    Rig rig;
    rig.set ("detect", (float) DetectionMode::Peak);
    rig.setAllBands ("comp.thr", -30.0f);
    rig.setAllBands ("comp.ratio", 2.0f);
    rig.setAllBands ("comp.knee", 0.0f);
    rig.setAllBands ("lim.on", 1.0f);
    rig.setAllBands ("lim.thr", -24.0f);
    rig.setAllBands ("lim.ratio", 4.0f);
    rig.setAllBands ("lim.knee", 0.0f);
    rig.prepare();
    const auto input = dc (-6.0, 0.5);
    rig.run (input);
    const auto& meters = rig.dyn->getMeters();
    CHECK (meters.compGainDb[0][0].load() < -10.0f);
    CHECK (meters.limGainDb[0][0].load() < -3.0f);
    rig.setAllBands ("on", 0.0f);
    const auto bypass = rig.run (input).l;
    CHECK_EQ (bypass.back(), input.back());
    for (int set = 0; set < kNumChannelSets; ++set)
    {
        CHECK_EQ (meters.compGainDb[set][0].load(), 0.0f);
        CHECK_EQ (meters.limGainDb[set][0].load(), 0.0f);
        CHECK_EQ (meters.gainReductionDb[set][0].load(), 0.0f);
    }
    rig.setAllBands ("on", 1.0f);
    rig.run (input);
    rig.dyn->reset();
    for (int set = 0; set < kNumChannelSets; ++set)
        for (int band = 0; band < kMaxBands; ++band)
        {
            CHECK_EQ (meters.compGainDb[set][band].load(), 0.0f);
            CHECK_EQ (meters.limGainDb[set][band].load(), 0.0f);
            CHECK_EQ (meters.gainReductionDb[set][band].load(), 0.0f);
            CHECK (meters.detectorDb[set][band].load() <= -120.0f);
        }
}

TEST_CASE ("dyn. setParams / process / setMonitor do not allocate")
{
    Rig rig (kSr, 256);
    rig.prepare();
    const auto l = pstest::pinkNoise (0.5, 0.2, kSr, 5);
    const auto r = pstest::pinkNoise (0.5, 0.2, kSr, 6);
    std::vector<float> bl (256), br (256);
    long long allocations = 0;
    int configs = 0;
    for (int bands = 1; bands <= 4; ++bands)
        for (int xo = 0; xo < 3; ++xo)
            for (int det = 0; det < 3; ++det)
            {
                ++configs;
                rig.set ("bands", (float) bands);
                rig.set ("xoverType", (float) xo);
                rig.set ("detect", (float) det);
                rig.set ("detFilter", (float) ((det + bands) % 3));
                rig.set ("detSlope", (float) (bands - 1));
                rig.set ("detTilt", det == 1 ? 4.0f : -2.0f);
                rig.set ("lookahead", (float) (bands * 2));
                rig.set ("adaptRel", (float) (det % 2));
                rig.set ("autoGain", (float) ((xo + bands) % 2));
                rig.set ("gain", (float) det);
                rig.setAllBands ("comp.thr", -30.0f + (float) bands);
                rig.setAllBands ("comp.ratio", det == 2 ? -4.0f : 6.0f);
                rig.setAllBands ("lim.on", (float) (bands % 2));
                rig.setAllBands ("lim.ratio", xo == 1 ? -2.0f : 20.0f);
                rig.setAllBands ("mix", 70.0f);
                ModuleMonitor m;
                m.soloBand = bands == 3 ? 1 : -1;
                m.soloDetectionFilter = det == 1;
                ProcessContext ctx;
                ctx.channelMode = (bands + det) % 2 == 0 ? ChannelMode::Stereo : ChannelMode::MidSide;
                pstest::AllocationScope scope;
                rig.dyn->setMonitor (m);
                for (size_t s = 0; s + 256 <= l.size(); s += 256)
                {
                    std::copy (l.begin() + (std::ptrdiff_t) s, l.begin() + (std::ptrdiff_t) s + 256, bl.begin());
                    std::copy (r.begin() + (std::ptrdiff_t) s, r.begin() + (std::ptrdiff_t) s + 256, br.begin());
                    float* ch[2] { bl.data(), br.data() };
                    rig.dyn->setParams (rig.values.data());
                    rig.dyn->process (ch, 256, ctx);
                }
                rig.dyn->reset();   // the chain calls reset on the audio thread
                allocations += scope.count();
            }
    pstest::note ("configurations", configs);
    CHECK_EQ (allocations, 0LL);
}

TEST_CASE ("dyn. output stays finite and bounded on extreme settings")
{
    Rig rig;
    rig.set ("bands", 4.0f);
    rig.set ("detFilter", (float) DetectionFilter::Tilt);
    rig.set ("detTilt", 6.0f);
    rig.set ("gain", 24.0f);
    rig.setAllBands ("comp.thr", -60.0f);
    rig.setAllBands ("comp.ratio", -10.0f);
    rig.setAllBands ("lim.on", 1.0f);
    rig.setAllBands ("lim.ratio", -2.5f);
    rig.setAllBands ("lim.thr", -60.0f);
    rig.setAllBands ("gain", 24.0f);
    rig.prepare();
    auto in = pstest::pinkNoise (0.9, 1.0, kSr, 9);
    in.resize (in.size() + (size_t) kSr, 0.0f);   // then silence (upward stages must not raise the noise floor)
    const auto out = rig.run (in, in);
    bool finite = true;
    for (float v : out.l)
        finite = finite && std::isfinite (v);
    CHECK (finite);

    // Each upward stage is capped at +24 dB. With band/output gains also +24 dB and auto gain disabled,
    // the absolute output cannot exceed the sum of absolute crossover bands multiplied by +96 dB.
    DynamicsParams params;
    readDynamicsParams (rig.values.data(), params);
    Crossover reference;
    reference.prepare (kSr, 512);
    reference.setup (4, params.crossoverHz, params.crossoverType);
    std::array<std::array<std::array<float, 512>, 2>, kMaxBands> buffers {};
    float* bandChannels[kMaxBands][2] {};
    float* const* bandOutputs[kMaxBands] {};
    for (int band = 0; band < kMaxBands; ++band)
    {
        for (int channel = 0; channel < 2; ++channel)
            bandChannels[band][channel] = buffers[(size_t) band][(size_t) channel].data();
        bandOutputs[band] = bandChannels[band];
    }
    double largestBandSum = 0.0;
    for (size_t start = 0; start < in.size(); start += 512)
    {
        const int count = (int) std::min ((size_t) 512, in.size() - start);
        const float* channels[2] { in.data() + start, in.data() + start };
        reference.process (channels, bandOutputs, count);
        for (int i = 0; i < count; ++i)
        {
            double sum = 0.0;
            for (int band = 0; band < kMaxBands; ++band)
                sum += std::abs ((double) bandChannels[band][0][i]);
            largestBandSum = std::max (largestBandSum, sum);
        }
    }
    const double bound = largestBandSum * std::pow (10.0, 96.0 / 20.0);
    const double peak = pstest::peakOf (out.l, 0, out.l.size());
    pstest::note ("extreme settings output peak", peak);
    pstest::note ("bound from crossover bands and the four +24 dB caps", bound);
    CHECK (peak <= bound * 1.00001);
    CHECK (pstest::peakOf (out.l, out.l.size() - 4800, 4800) == 0.0);
}

//==============================================================================
TEST_CASE ("dyn. CPU: 4 bands Digital at 48 kHz")
{
    Rig rig (kSr, 512);
    rig.set ("bands", 4.0f);
    rig.set ("xoverType", (float) CrossoverType::Digital);
    rig.set ("detFilter", (float) DetectionFilter::Tilt);
    rig.set ("detTilt", 3.0f);
    rig.set ("lookahead", 5.0f);
    rig.set ("adaptRel", 1.0f);
    rig.set ("autoGain", 1.0f);
    rig.setAllBands ("comp.thr", -30.0f);
    rig.setAllBands ("comp.ratio", 3.0f);
    rig.setAllBands ("lim.on", 1.0f);
    rig.setAllBands ("lim.thr", -12.0f);
    rig.prepare();
    const double seconds = 10.0;
    const auto l = pstest::pinkNoise (0.5, seconds, kSr, 31);
    const auto r = pstest::pinkNoise (0.5, seconds, kSr, 32);
    for (auto mode : { ChannelMode::Stereo, ChannelMode::MidSide })
    {
        pstest::Stopwatch sw;
        rig.run (l, r, mode);
        const double t = sw.seconds();
        pstest::note (std::string (mode == ChannelMode::Stereo ? "Stereo" : "M/S")
                          + ": % of one core for 4 bands Digital, tilt detection, limiters",
                      100.0 * t / seconds);
        CHECK_SPEED (t < 0.25 * seconds);
    }

    // the Dynamics part alone (Analog, 4 bands)
    rig.set ("xoverType", (float) CrossoverType::Analog);
    rig.prepare();
    pstest::Stopwatch sw;
    rig.run (l, r, ChannelMode::MidSide);
    pstest::note ("M/S: % of one core for 4 bands Analog", 100.0 * sw.seconds() / seconds);
}
