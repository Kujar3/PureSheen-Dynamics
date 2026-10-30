#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/Chain.h"
#include "dsp/TransientSplit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Transient/Sustain channel mode of the chain host (dsp/Chain.cpp). Prefix "chain. t/s". Both instances of the
// slot (the normal one and the twin) are replaced by small test modules.
namespace
{
using namespace ps;
using pstest::note;

class TsModule : public Module
{
public:
    explicit TsModule (ModuleType t, float g = 1.0f, int latencySamples = 0) : type (t), gain (g), latency (latencySamples) {}

    ModuleType getType() const noexcept override { return type; }
    void prepare (double, int) override
    {
        for (auto& b : ring)
            b.assign (1 << 16, 0.0f);
        pos = 0;
    }
    void reset() noexcept override
    {
        for (auto& b : ring)
            std::fill (b.begin(), b.end(), 0.0f);
        ++resets;
    }
    void setParams (ParamValues) noexcept override { ++paramCalls; }
    void setMonitor (const ModuleMonitor& m) noexcept override { lastMonitor = m; }
    void process (float* const* ch, int n, const ProcessContext& ctx) noexcept override
    {
        lastCtx = ctx;
        ++processCalls;
        const int mask = (1 << 16) - 1;
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                ring[c][(size_t) pos] = ch[c][i];
                energyIn += (double) ch[c][i] * (double) ch[c][i];
                ch[c][i] = ring[c][(size_t) ((pos - latency) & mask)] * gain;
            }
            pos = (pos + 1) & mask;
        }
    }
    int getLatencySamples() const noexcept override { return latency; }

    ModuleType type;
    float gain;
    int latency;
    int resets = 0, processCalls = 0, paramCalls = 0;
    double energyIn = 0.0;
    ProcessContext lastCtx;
    ModuleMonitor lastMonitor;

private:
    std::vector<float> ring[2];
    int pos = 0;
};

std::vector<float> defaultValues()
{
    std::vector<float> v;
    for (const auto& s : paramSpecs())
        v.push_back (s.defaultValue);
    return v;
}

void setValue (std::vector<float>& v, const std::string& id, float value)
{
    const int i = paramIndex (id);
    REQUIRE (i >= 0);
    v[(size_t) i] = value;
}

// EQ 1 alone in the chain, both of its instances test modules.
struct TsRig
{
    Chain chain;
    std::vector<float> values = defaultValues();
    TsModule* a = nullptr;
    TsModule* b = nullptr;
    double sr;
    int block;

    TsRig (float gainA, float gainB, int latA = 0, int latB = 0, double sampleRate = 48000.0, int blockSize = 480,
           bool tsMode = true)
        : sr (sampleRate), block (blockSize)
    {
        a = new TsModule (ModuleType::Equalizer1, gainA, latA);
        b = new TsModule (ModuleType::Equalizer1, gainB, latB);
        chain.setModule (ModuleType::Equalizer1, std::unique_ptr<Module> (a), 0);
        chain.setModule (ModuleType::Equalizer1, std::unique_ptr<Module> (b), 1);
        chain.setOrder ({ ModuleType::Equalizer1 });
        if (tsMode)
            setValue (values, "eq1.chMode", 3.0f);   // Transient/Sustain
        chain.prepare (sr, block);
    }

    void run (std::vector<float>& l, std::vector<float>& r)
    {
        for (size_t pos = 0; pos < l.size(); pos += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, l.size() - pos);
            float* ch[2] { l.data() + pos, r.data() + pos };
            chain.process (ch, n, values.data(), HostInfo {});
        }
    }
};

// A reference splitter run over the same input (same defaults as the slot's).
struct RefParts
{
    std::vector<float> tl, tr, sl, sr;
    int latency = 0;
};

RefParts referenceSplit (const std::vector<float>& l, const std::vector<float>& r, double sr, int block,
                         float sens = 50.0f, float len = 20.0f)
{
    TransientSplitter s;
    s.prepare (sr, block);
    s.setParams (sens, len);
    RefParts p;
    p.latency = s.getLatencySamples();
    for (auto* v : { &p.tl, &p.tr, &p.sl, &p.sr })
        v->resize (l.size());
    for (size_t pos = 0; pos < l.size(); pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, l.size() - pos);
        const float* in[2] { l.data() + pos, r.data() + pos };
        float* t[2] { p.tl.data() + pos, p.tr.data() + pos };
        float* su[2] { p.sl.data() + pos, p.sr.data() + pos };
        s.process (in, t, su, n);
    }
    return p;
}

// music-like: pink bed + decaying noise hits every 0.5 s
std::vector<float> musicLike (double sr, double seconds, std::uint32_t seed)
{
    auto x = pstest::pinkNoise (0.3, seconds, sr, seed);
    pstest::Lcg lcg (seed + 7u);
    const auto period = (size_t) (0.5 * sr);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += (float) (0.7 * std::exp (-(double) (i % period) / (0.03 * sr)) * lcg.next());
    return x;
}

double maxDiff (const std::vector<float>& a, const std::vector<float>& b, size_t from = 0)
{
    double worst = 0.0;
    for (size_t i = from; i < a.size() && i < b.size(); ++i)
    {
        const double d = std::abs ((double) a[i] - (double) b[i]);
        if (! std::isfinite (d))
            return std::numeric_limits<double>::infinity();
        worst = std::max (worst, d);
    }
    return worst;
}

std::vector<float> delayed (const std::vector<float>& x, int d)
{
    std::vector<float> y (x.size(), 0.0f);
    for (size_t i = (size_t) d; i < x.size(); ++i)
        y[i] = x[i - (size_t) d];
    return y;
}
} // namespace

TEST_CASE ("chain. t/s neutral modules: output == input delayed by the splitter latency")
{
    const double sr = 48000.0;
    const auto xl = musicLike (sr, 4.0, 1u), xr = musicLike (sr, 4.0, 2u);
    TsRig rig (1.0f, 1.0f);
    auto l = xl, r = xr;
    rig.run (l, r);
    const int lat = rig.chain.getLatencySamples();
    CHECK_EQ (lat, TransientSplitter::latencyFor (sr));
    const double e = std::max (maxDiff (l, delayed (xl, lat)), maxDiff (r, delayed (xr, lat)));
    note ("t/s null with unity twins (dBFS)", pstest::dbOf (e));
    CHECK (pstest::dbOf (e) < -120.0);
    CHECK_EQ (rig.a->lastCtx.channelMode, ChannelMode::Stereo);
    CHECK_EQ (rig.a->lastCtx.paramSet, 0);
    CHECK_EQ (rig.b->lastCtx.channelMode, ChannelMode::Stereo);
    CHECK_EQ (rig.b->lastCtx.paramSet, 1);
    CHECK_EQ (rig.b->getParamSet(), 1);
    CHECK (rig.a->processCalls > 0 && rig.b->processCalls > 0);
    CHECK (rig.b->paramCalls > 0);
}

TEST_CASE ("chain. t/s with the real Core modules at neutral settings nulls")
{
    const double sr = 48000.0;
    const auto xl = musicLike (sr, 3.0, 3u), xr = musicLike (sr, 3.0, 4u);
    for (auto type : { ModuleType::Equalizer1, ModuleType::Exciter, ModuleType::Imager })
    {
        Chain chain;
        auto values = defaultValues();
        chain.setOrder ({ type });
        for (const auto& [id, v] : channelModeParamValues (type, ChannelMode::TransientSustain))
            setValue (values, id, v);
        chain.prepare (sr, 512);
        auto l = xl, r = xr;
        for (size_t pos = 0; pos < l.size(); pos += 512)
        {
            const int n = (int) std::min<size_t> (512, l.size() - pos);
            float* ch[2] { l.data() + pos, r.data() + pos };
            chain.process (ch, n, values.data(), HostInfo {});
        }
        const int lat = chain.getLatencySamples();
        const double e = std::max (maxDiff (l, delayed (xl, lat), 4096), maxDiff (r, delayed (xr, lat), 4096));
        note (std::string (moduleInfo (type).name) + " in T/S, neutral: null (dBFS)", pstest::dbOf (e));
        CHECK (pstest::dbOf (e) < -120.0);
        CHECK (chain.hasTwin (type));
    }
}

TEST_CASE ("chain. t/s latency: splitter + module latency, kept when powered off")
{
    const double sr = 48000.0;
    TsRig rig (1.0f, 1.0f, 100, 100);
    std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
    l[1000] = 0.5f;
    r[1000] = 0.5f;
    auto yl = l, yr = r;
    rig.run (yl, yr);
    const int lat = rig.chain.getLatencySamples();
    CHECK_EQ (lat, TransientSplitter::latencyFor (sr) + 100);
    CHECK (std::abs (yl[1000 + (size_t) lat] - 0.5f) < 1.0e-6f);
    CHECK (maxDiff (yl, delayed (l, lat)) < 1.0e-6);

    // instances with different latencies are padded to the larger one
    TsRig pad (1.0f, 1.0f, 30, 100);
    auto pl = l, pr = r;
    pad.run (pl, pr);
    CHECK_EQ (pad.chain.getLatencySamples(), TransientSplitter::latencyFor (sr) + 100);
    CHECK (maxDiff (pl, delayed (l, pad.chain.getLatencySamples())) < 1.0e-6);

    // powered off: same latency, exact delayed copy
    setValue (rig.values, "eq1.on", 0.0f);
    const auto x = musicLike (sr, 1.0, 9u);
    auto ol = x, orr = x;
    rig.run (ol, orr);
    rig.run (ol = x, orr = x);
    CHECK_EQ (rig.chain.getLatencySamples(), lat);
    CHECK_EQ (maxDiff (ol, delayed (x, lat), (size_t) lat), 0.0);   // (the first lat samples hold the previous run)
}

TEST_CASE ("chain. t/s routes set A to the transients and set B to the sustain")
{
    const double sr = 48000.0;
    const auto xl = musicLike (sr, 3.0, 11u), xr = musicLike (sr, 3.0, 12u);
    const auto ref = referenceSplit (xl, xr, sr, 480);
    for (int swap = 0; swap < 2; ++swap)
    {
        TsRig rig (swap == 0 ? 2.0f : 1.0f, swap == 0 ? 1.0f : 2.0f);
        auto l = xl, r = xr;
        rig.run (l, r);
        std::vector<float> el (l.size()), er (l.size());
        for (size_t i = 0; i < l.size(); ++i)
        {
            el[i] = swap == 0 ? 2.0f * ref.tl[i] + ref.sl[i] : ref.tl[i] + 2.0f * ref.sl[i];
            er[i] = swap == 0 ? 2.0f * ref.tr[i] + ref.sr[i] : ref.tr[i] + 2.0f * ref.sr[i];
        }
        const double e = std::max (maxDiff (l, el), maxDiff (r, er));
        note (swap == 0 ? "2 T + S: error" : "T + 2 S: error", e);
        CHECK (e < 1.0e-6);
    }
    // the hits really are in T: the transient part carries a meaningful share
    double eT = 0.0, eX = 0.0;
    for (size_t i = 0; i < xl.size(); ++i)
    {
        eT += (double) ref.tl[i] * ref.tl[i];
        eX += (double) xl[i] * xl[i];
    }
    note ("transient energy share of the test signal (dB)", 10.0 * std::log10 (eT / eX));
    CHECK (10.0 * std::log10 (eT / eX) > -15.0);
}

TEST_CASE ("chain. t/s channel power, channel solo and delta per part")
{
    const double sr = 48000.0;
    const auto xl = musicLike (sr, 2.0, 21u), xr = musicLike (sr, 2.0, 22u);
    const auto ref = referenceSplit (xl, xr, sr, 480);
    const int L = ref.latency;
    const size_t settle = (size_t) (0.1 * sr);

    // channel power: Transient off -> T passes unprocessed; the output is the input delayed
    {
        TsRig rig (2.0f, 1.0f);
        setValue (rig.values, "eq1.a.on", 0.0f);
        auto l = xl, r = xr;
        rig.run (l, r);
        const double e = std::max (maxDiff (l, delayed (xl, L), settle), maxDiff (r, delayed (xr, L), settle));
        note ("Transient channel off (gain 2 on T): null (dBFS)", pstest::dbOf (e));
        CHECK (pstest::dbOf (e) < -120.0);
    }
    {
        TsRig rig (1.0f, 2.0f);
        setValue (rig.values, "eq1.b.on", 0.0f);
        auto l = xl, r = xr;
        rig.run (l, r);
        CHECK (pstest::dbOf (std::max (maxDiff (l, delayed (xl, L), settle), maxDiff (r, delayed (xr, L), settle))) < -120.0);
    }
    // the power ramp is smooth (no step larger than 0.01 on a 100 Hz sine)
    {
        TsRig rig (2.0f, 1.0f);
        const auto s = pstest::sine (100.0, -6.0, 1.0, sr);
        auto l = s, r = s;
        for (size_t pos = 0; pos < l.size(); pos += 480)
        {
            if (pos == 24000)
                setValue (rig.values, "eq1.a.on", 0.0f);
            float* ch[2] { l.data() + pos, r.data() + pos };
            rig.chain.process (ch, 480, rig.values.data(), HostInfo {});
        }
        double step = 0.0;
        for (size_t i = 1000; i < l.size(); ++i)
            step = std::max (step, std::abs ((double) l[i] - l[i - 1]));
        const double sineStep = 2.0 * pstest::kPi * 100.0 / sr * pstest::dbfsToAmplitude (-6.0);
        note ("largest sample step around a channel power switch (x sine step)", step / sineStep);
        CHECK (step < 0.01 + sineStep);
    }

    // channel solo: Transient alone == processed T, Sustain alone == processed S; the two renders sum to the input
    std::vector<float> soloT, soloS;
    for (int part = 0; part < 2; ++part)
    {
        TsRig rig (1.0f, 1.0f);
        rig.chain.setChannelSolo (ModuleType::Equalizer1, part);
        auto l = xl, r = xr;
        rig.run (l, r);
        const auto& wantL = part == 0 ? ref.tl : ref.sl;
        const auto& wantR = part == 0 ? ref.tr : ref.sr;
        const double e = std::max (maxDiff (l, wantL, settle), maxDiff (r, wantR, settle));
        note (part == 0 ? "Transient solo vs reference T (dBFS)" : "Sustain solo vs reference S (dBFS)", pstest::dbOf (e));
        CHECK (pstest::dbOf (e) < -120.0);
        (part == 0 ? soloT : soloS) = l;
    }
    {
        std::vector<float> sum (soloT.size());
        for (size_t i = 0; i < sum.size(); ++i)
            sum[i] = soloT[i] + soloS[i];
        CHECK (pstest::dbOf (maxDiff (sum, delayed (xl, L), settle)) < -120.0);
    }

    // delta: gain 2 on T -> what the module changes is T itself; with the Sustain solo the delta is 0
    {
        TsRig rig (2.0f, 1.0f);
        rig.chain.setDelta (ModuleType::Equalizer1, true);
        auto l = xl, r = xr;
        rig.run (l, r);
        const double e = std::max (maxDiff (l, ref.tl, settle), maxDiff (r, ref.tr, settle));
        note ("Delta (gain 2 on T) vs reference T (dBFS)", pstest::dbOf (e));
        CHECK (pstest::dbOf (e) < -120.0);

        rig.chain.setChannelSolo (ModuleType::Equalizer1, 1);
        rig.run (l = xl, r = xr);
        rig.run (l = xl, r = xr);
        double peak = 0.0;
        for (size_t i = settle; i < l.size(); ++i)
            peak = std::max (peak, (double) std::abs (l[i]));
        note ("Delta with the Sustain solo (dBFS)", pstest::dbOf (peak));
        CHECK (pstest::dbOf (peak) < -140.0);
    }
}

TEST_CASE ("chain. t/s monitors: band solo to both instances, audition only to its set")
{
    TsRig rig (1.0f, 1.0f);
    ModuleMonitor m;
    m.soloBand = 2;
    rig.chain.setMonitor (ModuleType::Equalizer1, m);
    std::vector<float> l (2048, 0.1f), r (2048, 0.1f);
    rig.run (l, r);
    CHECK_EQ (rig.a->lastMonitor.soloBand, 2);
    CHECK_EQ (rig.b->lastMonitor.soloBand, 2);

    m = {};
    m.auditionOn = true;
    m.auditionChannelSet = 1;
    rig.chain.setMonitor (ModuleType::Equalizer1, m);
    rig.run (l, r);
    CHECK (! rig.a->lastMonitor.auditionOn);
    CHECK (rig.b->lastMonitor.auditionOn);

    // while set B auditions, only the sustain part is heard
    const double sr = 48000.0;
    const auto xl = musicLike (sr, 1.0, 31u);
    const auto ref = referenceSplit (xl, xl, sr, 480);
    TsRig fresh (1.0f, 1.0f);
    fresh.chain.setMonitor (ModuleType::Equalizer1, m);
    auto al = xl, ar = xl;
    fresh.run (al, ar);
    CHECK (pstest::dbOf (maxDiff (al, ref.sl, (size_t) (0.1 * sr))) < -120.0);
}

TEST_CASE ("chain. t/s monitor handoff preserves region solo and learn requests")
{
    TsRig rig (1.0f, 1.0f);
    ModuleMonitor m;
    m.soloRegion = true;
    m.learnRegion = true;
    rig.chain.setMonitor (ModuleType::Equalizer1, m);
    std::vector<float> l (2048, 0.1f), r = l;
    rig.run (l, r);
    CHECK (rig.a->lastMonitor.soloRegion);
    CHECK (rig.a->lastMonitor.learnRegion);
    CHECK (rig.b->lastMonitor.soloRegion);
    CHECK (rig.b->lastMonitor.learnRegion);

    rig.chain.setMonitor (ModuleType::Equalizer1, {});
    rig.run (l, r);
    CHECK (! rig.a->lastMonitor.soloRegion && ! rig.a->lastMonitor.learnRegion);
    CHECK (! rig.b->lastMonitor.soloRegion && ! rig.b->lastMonitor.learnRegion);
}

TEST_CASE ("chain. t/s selected band solo routes and isolates its part")
{
    const auto x = musicLike (48000.0, 0.1, 33u);
    const auto ref = referenceSplit (x, x, 48000.0, 480);
    for (int part = 0; part < 2; ++part)
    {
        TsRig rig (1.0f, 1.0f);
        ModuleMonitor m;
        m.soloBand = 2;
        m.soloChannelSet = part;
        rig.chain.setMonitor (ModuleType::Equalizer1, m);
        auto l = x, r = x;
        rig.run (l, r);
        CHECK_EQ (rig.a->lastMonitor.soloBand, part == 0 ? 2 : -1);
        CHECK_EQ (rig.b->lastMonitor.soloBand, part == 1 ? 2 : -1);
        CHECK (maxDiff (l, part == 0 ? ref.tl : ref.sl) < 1.0e-6);
        CHECK (maxDiff (r, part == 0 ? ref.tr : ref.sr) < 1.0e-6);
    }
}

TEST_CASE ("chain. t/s audition routing follows the active mode without a new monitor request")
{
    TsRig rig (1.0f, 1.0f, 0, 0, 48000.0, 480, false);
    ModuleMonitor m;
    m.auditionOn = true;
    m.auditionChannelSet = 1;
    rig.chain.setMonitor (ModuleType::Equalizer1, m);
    std::vector<float> l (4800, 0.1f), r = l;
    for (int mode : { 1, 2, 3, 1 })
    {
        setValue (rig.values, "eq1.chMode", (float) mode);
        rig.run (l, r);
        CHECK_EQ (rig.a->lastMonitor.auditionOn, mode != 3);
        if (mode != 3)
            CHECK_EQ (rig.a->lastMonitor.auditionChannelSet, 1);
        else
            CHECK (rig.b->lastMonitor.auditionOn);
    }
}

TEST_CASE ("chain. t/s powered-off selected part keeps publishing aligned dry taps")
{
    const auto x = musicLike (48000.0, 0.1, 34u);
    const auto ref = referenceSplit (x, x, 48000.0, 480);
    for (int part = 0; part < 2; ++part)
    {
        TsRig rig (2.0f, 3.0f);
        setValue (rig.values, "eq1.on", 0.0f);
        rig.chain.setTapModule ((int) ModuleType::Equalizer1);
        rig.chain.setTapPart (part);
        auto l = x, r = x;
        rig.run (l, r);
        std::vector<float> pre (x.size()), post (x.size());
        CHECK_EQ (rig.chain.getModulePreFifo().pull (pre.data(), nullptr, (int) pre.size()), (int) x.size());
        CHECK_EQ (rig.chain.getModulePostFifo().pull (post.data(), nullptr, (int) post.size()), (int) x.size());
        CHECK (maxDiff (pre, part == 0 ? ref.tl : ref.sl) < 1.0e-6);
        CHECK_EQ (maxDiff (pre, post), 0.0);
        CHECK_EQ (maxDiff (l, delayed (x, ref.latency)), 0.0);
        CHECK_EQ (rig.a->processCalls, 0);
        CHECK_EQ (rig.b->processCalls, 0);
    }
}

TEST_CASE ("chain. t/s mode switch: no click, latency changes at the switch, both instances reset")
{
    const double sr = 48000.0;
    TsRig rig (1.0f, 1.0f, 0, 0, sr, 480, false);
    const auto s = pstest::sine (200.0, -6.0, 3.0, sr);
    auto l = s, r = s;
    const int resetsA = rig.a->resets, resetsB = rig.b->resets;
    std::vector<int> latencies;
    for (size_t pos = 0; pos < l.size(); pos += 480)
    {
        if (pos == 48000)
            setValue (rig.values, "eq1.chMode", 3.0f);
        if (pos == 96000)
            setValue (rig.values, "eq1.chMode", 0.0f);
        float* ch[2] { l.data() + pos, r.data() + pos };
        rig.chain.process (ch, 480, rig.values.data(), HostInfo {});
        latencies.push_back (rig.chain.getLatencySamples());
    }
    double step = 0.0;
    for (size_t i = 1; i < l.size(); ++i)
        step = std::max (step, std::abs ((double) l[i] - l[i - 1]));
    const double sineStep = 2.0 * std::sin (pstest::kPi * 200.0 / sr) * pstest::dbfsToAmplitude (-6.0);
    note ("largest sample step through Stereo -> T/S -> Stereo (x sine step)", step / sineStep);
    CHECK (step <= sineStep * 1.05);

    // the output dips while switching, for at most ~60 ms + the latency
    auto dipLength = [&] (size_t from)
    {
        size_t quiet = 0;
        for (size_t i = from; i < from + 24000 && i < l.size(); ++i)
            if (std::abs (l[i]) < 0.5 * pstest::dbfsToAmplitude (-6.0) * 0.1)
                ++quiet;
        return quiet;
    };
    note ("samples near zero after the switch into T/S", (double) dipLength (48000));
    CHECK (dipLength (48000) < (size_t) (0.06 * sr) + 64);
    CHECK (std::find (latencies.begin(), latencies.end(), TransientSplitter::latencyFor (sr)) != latencies.end());
    CHECK_EQ (latencies.back(), 0);
    CHECK_EQ (rig.a->resets, resetsA + 2);
    CHECK_EQ (rig.b->resets, resetsB + 2);
}

TEST_CASE ("chain. t/s mode switch also fades global bypass and upstream delta")
{
    const double sr = 48000.0;
    const auto x = pstest::sine (200.0, -6.0, 2.0, sr);
    const double sineStep = 2.0 * std::sin (pstest::kPi * 200.0 / sr) * pstest::dbfsToAmplitude (-6.0);
    for (bool globalBypass : { true, false })
    {
        Chain chain;
        chain.setModule (ModuleType::Equalizer1, std::make_unique<TsModule> (ModuleType::Equalizer1, 2.0f));
        chain.setModule (ModuleType::Equalizer2, std::make_unique<TsModule> (ModuleType::Equalizer2), 0);
        chain.setModule (ModuleType::Equalizer2, std::make_unique<TsModule> (ModuleType::Equalizer2), 1);
        chain.setOrder ({ ModuleType::Equalizer1, ModuleType::Equalizer2 });
        auto values = defaultValues();
        setValue (values, "io.bypass", globalBypass ? 1.0f : 0.0f);
        chain.setDelta (ModuleType::Equalizer1, ! globalBypass);
        chain.prepare (sr, 480);
        auto l = x, r = x;
        for (size_t pos = 0; pos < l.size(); pos += 480)
        {
            if (pos == 24000) setValue (values, "eq2.chMode", 3.0f);
            if (pos == 72000) setValue (values, "eq2.chMode", 0.0f);
            float* ch[2] { l.data() + pos, r.data() + pos };
            chain.process (ch, 480, values.data(), HostInfo {});
        }
        double step = 0.0;
        for (size_t i = 1; i < l.size(); ++i)
            step = std::max (step, std::abs ((double) l[i] - l[i - 1]));
        note (globalBypass ? "bypass latency switch: largest step / sine step"
                           : "upstream delta latency switch: largest step / sine step", step / sineStep);
        CHECK (step <= sineStep * 1.05);
    }
}

TEST_CASE ("chain. t/s processing is allocation free")
{
    TsRig rig (1.5f, 0.5f, 10, 10);
    const auto x = musicLike (48000.0, 2.0, 41u);
    auto l = x, r = x;
    rig.run (l, r);   // warm
    long long count = 0;
    {
        pstest::AllocationScope scope;
        for (int k = 0; k < 8; ++k)
        {
            setValue (rig.values, "eq1.ts.sens", (float) (k * 12));
            setValue (rig.values, "eq1.ts.len", 5.0f + (float) (k * 10));
            setValue (rig.values, "eq1.a.on", (float) (k % 2));
            rig.chain.setChannelSolo (ModuleType::Equalizer1, k % 3 - 1);
            rig.chain.setDelta (ModuleType::Equalizer1, k % 2 == 0);
            rig.chain.setTapModule ((int) ModuleType::Equalizer1);
            rig.chain.setTapPart (k % 3 - 1);
            setValue (rig.values, "eq1.chMode", k == 5 ? 1.0f : 3.0f);
            rig.run (l = x, r = x);
        }
        count = scope.count();
    }
    CHECK_EQ (count, 0LL);
    const auto& meters = rig.chain.getIoMeters();
    note ("transient share meter (dB)", meters.moduleTsShareDb[(int) ModuleType::Equalizer1].load());
}

TEST_CASE ("chain. t/s real Equalizer: set A shapes the hits, set B the steady sound")
{
    const double sr = 48000.0;
    // steady 1 kHz sine at -20 dBFS: it belongs to the sustain part
    const auto x = pstest::sine (1000.0, -20.0, 3.0, sr);
    for (int set = 0; set < 2; ++set)
    {
        Chain chain;
        auto values = defaultValues();
        chain.setOrder ({ ModuleType::Equalizer1 });
        setValue (values, "eq1.chMode", 3.0f);
        const std::string b = set == 0 ? "eq1.a.b1." : "eq1.b.b1.";
        setValue (values, b + "on", 1.0f);
        setValue (values, b + "shape", (float) EqShape::Bell);
        setValue (values, b + "freq", 1000.0f);
        setValue (values, b + "gain", 6.0f);
        setValue (values, b + "q", 1.0f);
        chain.prepare (sr, 512);
        auto l = x, r = x;
        for (size_t pos = 0; pos < l.size(); pos += 512)
        {
            const int n = (int) std::min<size_t> (512, l.size() - pos);
            float* ch[2] { l.data() + pos, r.data() + pos };
            chain.process (ch, n, values.data(), HostInfo {});
        }
        const size_t from = (size_t) (2.0 * sr), len = (size_t) (0.5 * sr);
        const double gainDb = pstest::dbOf (std::abs (pstest::toneOf (l, from, len, 1000.0, sr)))
                              - pstest::dbOf (std::abs (pstest::toneOf (x, from - (size_t) chain.getLatencySamples(), len, 1000.0, sr)));
        note (set == 0 ? "steady sine, +6 dB bell on the Transient set: gain (dB)"
                       : "steady sine, +6 dB bell on the Sustain set: gain (dB)", gainDb);
        if (set == 0)
            CHECK (std::abs (gainDb) < 0.2);
        else
            CHECK (std::abs (gainDb - 6.0) < 0.2);
    }
}
