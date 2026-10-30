#include "TestFramework.h"
#include "TestSignals.h"

#include "core/Loudness.h"
#include "dsp/Chain.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// Chain host tests (dsp/Chain.cpp). Prefix "chain.". The chain's real modules are replaced by small test modules
// (Chain::setModule) so these tests only exercise the host.
namespace
{
using namespace ps;
using pstest::note;

// Per-channel gain with an exact integer latency. Records what it saw.
class TestModule : public Module
{
public:
    explicit TestModule (ModuleType t, float g = 1.0f, int latencySamples = 0) : type (t), latency (latencySamples)
    {
        gain[0] = gain[1] = g;
    }

    ModuleType getType() const noexcept override { return type; }

    void prepare (double, int) override
    {
        for (auto& b : ring)
            b.assign (1 << 17, 0.0f);
        pos = 0;
    }

    void reset() noexcept override
    {
        for (auto& b : ring)
            std::fill (b.begin(), b.end(), 0.0f);
        ++resets;
    }

    void setParams (ParamValues) noexcept override {}
    void setMonitor (const ModuleMonitor& m) noexcept override { lastMonitor = m; ++monitorCalls; }

    void process (float* const* ch, int n, const ProcessContext& ctx) noexcept override
    {
        lastCtx = ctx;
        ++processCalls;
        const int mask = (1 << 17) - 1;
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                ring[c][(size_t) pos] = ch[c][i];
                const float delayed = ring[c][(size_t) ((pos - latency) & mask)];
                ch[c][i] = (ctx.delta && deltaAware) ? delayed * (gain[c] - 1.0f) : delayed * gain[c];
            }
            pos = (pos + 1) & mask;
        }
    }

    int getLatencySamples() const noexcept override { return latency; }
    bool handlesDelta() const noexcept override { return deltaAware; }

    ModuleType type;
    float gain[2];
    int latency;
    bool deltaAware = false;
    int resets = 0, processCalls = 0, monitorCalls = 0;
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

struct Rig
{
    Chain chain;
    std::vector<float> values = defaultValues();
    TestModule* mods[kNumModuleTypes] {};
    double sr;
    int block;

    Rig (std::vector<std::pair<ModuleType, TestModule*>> ms, double sampleRate = 48000.0, int blockSize = 480)
        : sr (sampleRate), block (blockSize)
    {
        // every slot gets a test module (in or out of the chain)
        for (int t = 0; t < kNumModuleTypes; ++t)
        {
            TestModule* m = nullptr;
            for (auto& p : ms)
                if ((int) p.first == t)
                    m = p.second;
            if (m == nullptr)
                m = new TestModule ((ModuleType) t);
            mods[t] = m;
            chain.setModule ((ModuleType) t, std::unique_ptr<Module> (m));
        }
        std::vector<ModuleType> order;
        for (auto& p : ms)
            order.push_back (p.first);
        chain.setOrder (order);
        chain.prepare (sr, block);
    }

    // Processes (l, r) in place in blocks.
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

// max |a[i] - b[i - delay]| over i in [from, a.size())
double maxDiffDelayed (const std::vector<float>& a, const std::vector<float>& b, int delay, size_t from, float bGain = 1.0f)
{
    double worst = 0.0;
    for (size_t i = from; i < a.size(); ++i)
    {
        const double ref = i >= (size_t) delay ? (double) bGain * b[i - (size_t) delay] : 0.0;
        worst = std::max (worst, std::abs ((double) a[i] - ref));
    }
    return worst;
}

double toDb (double v) { return v > 1.0e-30 ? 20.0 * std::log10 (v) : -600.0; }
} // namespace

TEST_CASE ("chain. neutral chain is bit-exact")
{
    Rig rig ({ { ModuleType::Equalizer1, new TestModule (ModuleType::Equalizer1) },
               { ModuleType::Dynamics, new TestModule (ModuleType::Dynamics) },
               { ModuleType::Maximizer, new TestModule (ModuleType::Maximizer) } });
    auto l = pstest::pinkNoise (0.5, 2.0, 48000.0, 1u);
    auto r = pstest::pinkNoise (0.5, 2.0, 48000.0, 2u);
    const auto l0 = l, r0 = r;
    rig.run (l, r);
    CHECK (l == l0);
    CHECK (r == r0);
    CHECK_EQ (rig.chain.getLatencySamples(), 0);

    // M/S mode on a passthrough module: round trip within float rounding
    setValue (rig.values, "eq1.chMode", 1.0f);
    l = l0; r = r0;
    rig.run (l, r);
    const double err = std::max (maxDiffDelayed (l, l0, 0, 0), maxDiffDelayed (r, r0, 0, 0));
    note ("M/S encode -> decode worst error (dBFS)", toDb (err));
    CHECK (toDb (err) < -130.0);

    // empty chain
    Rig empty ({});
    l = l0; r = r0;
    empty.run (l, r);
    CHECK (l == l0);
}

TEST_CASE ("chain. latency is the sum of the modules and every dry path is aligned")
{
    auto* a = new TestModule (ModuleType::Equalizer1, 2.0f, 100);
    auto* b = new TestModule (ModuleType::Dynamics, 1.0f, 37);
    Rig rig ({ { ModuleType::Equalizer1, a }, { ModuleType::Dynamics, b } });
    const auto x = pstest::pinkNoise (0.3, 1.0, 48000.0, 5u);

    auto l = x, r = x;
    rig.run (l, r);
    CHECK_EQ (rig.chain.getLatencySamples(), 137);
    CHECK (maxDiffDelayed (l, x, 137, 0, 2.0f) < 1.0e-7);

    // power off: same latency, unity, after the 20 ms fade
    setValue (rig.values, "eq1.on", 0.0f);
    l = x; r = x;
    rig.run (l, r);
    CHECK_EQ (rig.chain.getLatencySamples(), 137);
    CHECK (maxDiffDelayed (l, x, 137, 4800) == 0.0);
    const int callsWhenOff = a->processCalls;
    l = x; r = x;
    rig.run (l, r);
    CHECK_EQ (a->processCalls, callsWhenOff);   // a powered-off module stops processing

    // power on again: reset, silent warm-up for its latency, fade in, processed again
    const int resetsBefore = a->resets;
    setValue (rig.values, "eq1.on", 1.0f);
    l = x; r = x;
    rig.run (l, r);
    CHECK_EQ (a->resets, resetsBefore + 1);
    CHECK (maxDiffDelayed (l, x, 137, 4800, 2.0f) < 1.0e-7);

    // global bypass: the input delayed by the chain latency
    setValue (rig.values, "io.bypass", 1.0f);
    l = x; r = x;
    rig.run (l, r);
    CHECK (maxDiffDelayed (l, x, 137, 4800) == 0.0);
}

TEST_CASE ("chain. large latencies stay aligned (Digital EQ at 192 kHz)")
{
    auto* a = new TestModule (ModuleType::Equalizer1, 1.0f, 70000);
    auto* m = new TestModule (ModuleType::Maximizer, 1.0f, 300);
    Rig rig ({ { ModuleType::Equalizer1, a }, { ModuleType::Maximizer, m } }, 192000.0, 1024);
    const auto x = pstest::pinkNoise (0.3, 1.0, 192000.0, 6u);
    setValue (rig.values, "eq1.on", 0.0f);
    auto l = x, r = x;
    rig.run (l, r);
    CHECK_EQ (rig.chain.getLatencySamples(), 70300);
    CHECK (maxDiffDelayed (l, x, 70300, 0) == 0.0);   // powered-off slot: dry delayed by 70000

    setValue (rig.values, "io.bypass", 1.0f);
    l = x; r = x;
    rig.run (l, r);
    CHECK (maxDiffDelayed (l, x, 70300, 70300) == 0.0);   // (the first 70300 samples are the previous run's tail)
}

TEST_CASE ("chain. delta plays only what the module changes, aligned, not re-processed downstream")
{
    auto* a = new TestModule (ModuleType::Equalizer1, 1.5f, 100);
    auto* b = new TestModule (ModuleType::Maximizer, 4.0f, 37);
    Rig rig ({ { ModuleType::Equalizer1, a }, { ModuleType::Maximizer, b } });
    rig.chain.setDelta (ModuleType::Equalizer1, true);
    CHECK (rig.chain.getDelta (ModuleType::Equalizer1));
    const auto x = pstest::pinkNoise (0.3, 1.0, 48000.0, 8u);
    auto l = x, r = x;
    rig.run (l, r);
    // delta of A = 0.5 x (aligned to the chain output), not 4 * 0.5 x
    CHECK (maxDiffDelayed (l, x, 137, 0, 0.5f) < 1.0e-6);
    CHECK_EQ (b->lastCtx.delta, false);

    // delta of a module that computes its own delta (Maximizer style)
    rig.chain.setDelta (ModuleType::Equalizer1, false);
    b->deltaAware = true;
    rig.chain.setDelta (ModuleType::Maximizer, true);
    l = x; r = x;
    rig.run (l, r);
    CHECK (b->lastCtx.delta);
    // module's own delta = (4 - 1) * input of B = 3 * 1.5 x
    CHECK (maxDiffDelayed (l, x, 137, 4800, 4.5f) < 1.0e-5);

    // delta off again: back to the processed signal (1.5 * 4)
    rig.chain.setDelta (ModuleType::Maximizer, false);
    l = x; r = x;
    rig.run (l, r);
    CHECK (maxDiffDelayed (l, x, 137, 4800, 6.0f) < 1.0e-5);
}

TEST_CASE ("chain. solo processes only the soloed module")
{
    auto* a = new TestModule (ModuleType::Equalizer1, 2.0f, 10);
    auto* b = new TestModule (ModuleType::Dynamics, 3.0f, 20);
    Rig rig ({ { ModuleType::Equalizer1, a }, { ModuleType::Dynamics, b } });
    rig.chain.setSolo ((int) ModuleType::Dynamics);
    CHECK_EQ (rig.chain.getSolo(), (int) ModuleType::Dynamics);
    const auto x = pstest::pinkNoise (0.3, 1.0, 48000.0, 9u);
    auto l = x, r = x;
    rig.run (l, r);
    CHECK (maxDiffDelayed (l, x, 30, 0, 3.0f) < 1.0e-6);

    // soloing a powered-off module plays the aligned dry signal
    setValue (rig.values, "dyn.on", 0.0f);
    l = x; r = x;
    rig.run (l, r);
    CHECK (maxDiffDelayed (l, x, 30, 4800) == 0.0);

    rig.chain.setSolo (-1);
    setValue (rig.values, "dyn.on", 1.0f);
    l = x; r = x;
    rig.run (l, r);
    CHECK (maxDiffDelayed (l, x, 30, 4800, 6.0f) < 1.0e-5);
}

TEST_CASE ("chain. channel modes: M/S encode, channel on/off and channel solo")
{
    auto* a = new TestModule (ModuleType::Equalizer1);
    Rig rig ({ { ModuleType::Equalizer1, a } });
    const auto x = pstest::pinkNoise (0.3, 0.5, 48000.0, 10u);
    const std::vector<float> zero (x.size(), 0.0f);

    // M/S: the module sees Mid on ch0 and Side on ch1; killing Side gives mono (L + R) / 2 on both sides
    setValue (rig.values, "eq1.chMode", 1.0f);
    a->gain[1] = 0.0f;
    auto l = x, r = zero;
    rig.run (l, r);
    CHECK (maxDiffDelayed (l, x, 0, 0, 0.5f) < 1.0e-7);
    CHECK (maxDiffDelayed (r, x, 0, 0, 0.5f) < 1.0e-7);

    // channel B (Side) off: Side is the dry Side again -> unity
    setValue (rig.values, "eq1.b.on", 0.0f);
    l = x; r = zero;
    rig.run (l, r);
    rig.run (l = x, r = zero);
    CHECK (maxDiffDelayed (l, x, 0, 0) < 1.0e-7);
    CHECK (maxDiffDelayed (r, zero, 0, 0) < 1.0e-7);

    // channel solo Side on a unity module: L = S, R = -S
    setValue (rig.values, "eq1.b.on", 1.0f);
    a->gain[1] = 1.0f;
    rig.chain.setChannelSolo (ModuleType::Equalizer1, 1);
    rig.run (l = x, r = zero);
    rig.run (l = x, r = zero);
    CHECK (maxDiffDelayed (l, x, 0, 0, 0.5f) < 1.0e-7);
    CHECK (maxDiffDelayed (r, x, 0, 0, -0.5f) < 1.0e-7);

    // L/R mode: set A (Left) gain 2, channel solo off; Left doubled, Right untouched
    rig.chain.setChannelSolo (ModuleType::Equalizer1, -1);
    setValue (rig.values, "eq1.chMode", 2.0f);
    a->gain[0] = 2.0f;
    rig.run (l = x, r = x);
    rig.run (l = x, r = x);
    CHECK (maxDiffDelayed (l, x, 0, 0, 2.0f) < 1.0e-7);
    CHECK (maxDiffDelayed (r, x, 0, 0) == 0.0);
    CHECK_EQ (a->lastCtx.channelMode, ChannelMode::LeftRight);

    // Stereo ignores channel on/off
    setValue (rig.values, "eq1.chMode", 0.0f);
    setValue (rig.values, "eq1.a.on", 0.0f);
    a->gain[1] = 2.0f;
    rig.run (l = x, r = x);
    rig.run (l = x, r = x);
    CHECK (maxDiffDelayed (l, x, 0, 0, 2.0f) < 1.0e-7);
}

TEST_CASE ("chain. difference meter, taps and monitors")
{
    auto* a = new TestModule (ModuleType::Equalizer1, 1.0f, 5);
    auto* b = new TestModule (ModuleType::Dynamics, 2.0f, 0);
    Rig rig ({ { ModuleType::Equalizer1, a }, { ModuleType::Dynamics, b } });
    rig.chain.setTapModule ((int) ModuleType::Dynamics);
    ModuleMonitor mon;
    mon.soloBand = 2;
    rig.chain.setMonitor (ModuleType::Dynamics, mon);

    auto l = pstest::pinkNoise (0.2, 2.0, 48000.0, 11u), r = l;
    rig.run (l, r);
    const float diff = rig.chain.getIoMeters().moduleDifferenceDb[(int) ModuleType::Dynamics].load();
    note ("difference meter of a +6.02 dB module (dB)", diff);
    CHECK_NEAR (diff, 6.0206, 0.05);
    CHECK_NEAR (rig.chain.getIoMeters().moduleDifferenceDb[(int) ModuleType::Equalizer1].load(), 0.0, 0.01);
    CHECK_EQ (b->monitorCalls, 1);
    CHECK_EQ (b->lastMonitor.soloBand, 2);

    std::vector<float> pl (4096), pr (4096), ql (4096), qr (4096);
    const int np = rig.chain.getModulePreFifo().pull (pl.data(), pr.data(), 4096);
    const int nq = rig.chain.getModulePostFifo().pull (ql.data(), qr.data(), 4096);
    CHECK_EQ (np, 4096);
    CHECK_EQ (nq, 4096);
    double worst = 0.0;
    for (int i = 0; i < 4096; ++i)
        worst = std::max (worst, std::abs ((double) ql[(size_t) i] - 2.0 * pl[(size_t) i]));
    CHECK (worst < 1.0e-7);

    std::vector<float> ol (4096), orr (4096);
    CHECK_EQ (rig.chain.getOutputFifo().pull (ol.data(), orr.data(), 4096), 4096);
    CHECK_EQ (rig.chain.getInputFifo().pull (ol.data(), orr.data(), 4096), 4096);
}

TEST_CASE ("chain. gain match converges (classic and modern)")
{
    const double sr = 48000.0;
    const auto x = pstest::pinkNoise (0.25, 10.0, sr, 12u);
    const float inputLufs = measureIntegratedLufs (x.data(), x.data(), (long long) x.size(), sr);

    // modern: the processed output is levelled to the input
    {
        Rig rig ({ { ModuleType::Equalizer1, new TestModule (ModuleType::Equalizer1, 2.0f, 64) } });
        setValue (rig.values, "io.gainMatch", 1.0f);
        setValue (rig.values, "io.gainMatchModern", 1.0f);
        auto l = x, r = x;
        rig.run (l, r);
        const size_t tail = (size_t) (3.0 * sr);
        const float outLufs = measureIntegratedLufs (l.data() + l.size() - tail, r.data() + r.size() - tail, (long long) tail, sr);
        note ("modern: output - input loudness over the last 3 s (LU)", outLufs - inputLufs);
        note ("modern: applied match gain (dB)", rig.chain.getIoMeters().gainMatchDb.load());
        CHECK (std::abs (outLufs - inputLufs) < 0.5);
        CHECK_NEAR (rig.chain.getIoMeters().gainMatchDb.load(), -6.02, 0.5);
    }

    // classic: the bypassed signal is levelled to the processed loudness
    {
        Rig rig ({ { ModuleType::Equalizer1, new TestModule (ModuleType::Equalizer1, 2.0f, 64) } });
        setValue (rig.values, "io.gainMatch", 1.0f);
        auto l = x, r = x;
        rig.run (l, r);
        setValue (rig.values, "io.bypass", 1.0f);
        l = x; r = x;
        rig.run (l, r);
        const size_t tail = (size_t) (3.0 * sr);
        const float outLufs = measureIntegratedLufs (l.data() + l.size() - tail, r.data() + r.size() - tail, (long long) tail, sr);
        note ("classic: bypassed output - input loudness (LU, expect +6.02)", outLufs - inputLufs);
        CHECK (std::abs (outLufs - inputLufs - 6.02) < 0.5);

        // without gain match the bypass is unity
        setValue (rig.values, "io.gainMatch", 0.0f);
        l = x; r = x;
        rig.run (l, r);
        CHECK (maxDiffDelayed (l, x, 64, 4800) < 1.0e-7);
    }

    // frozen while the input is silent: no runaway
    {
        Rig rig ({ { ModuleType::Equalizer1, new TestModule (ModuleType::Equalizer1, 2.0f) } });
        setValue (rig.values, "io.gainMatch", 1.0f);
        setValue (rig.values, "io.gainMatchModern", 1.0f);
        std::vector<float> l (x.size(), 0.0f), r (x.size(), 0.0f);
        rig.run (l, r);
        CHECK_NEAR (rig.chain.getIoMeters().gainMatchDb.load(), 0.0, 1.0e-6);
    }
}

TEST_CASE ("chain. I/O gain, mono, swap, DC filter and meters")
{
    const double sr = 48000.0;
    auto* mx = new TestModule (ModuleType::Maximizer);
    Rig rig ({ { ModuleType::Equalizer1, new TestModule (ModuleType::Equalizer1) }, { ModuleType::Maximizer, mx } });
    const auto x = pstest::sine (997.0, -20.0, 1.0, sr);
    const auto y = pstest::sine (300.0, -20.0, 1.0, sr);

    setValue (rig.values, "in.gainL", 6.0f);
    setValue (rig.values, "out.gainR", -6.0f);
    auto l = x, r = y;
    rig.run (l, r);
    const float g6 = (float) std::pow (10.0, 6.0 / 20.0), gm6 = (float) std::pow (10.0, -6.0 / 20.0);
    CHECK (maxDiffDelayed (l, x, 0, 4800, g6) < 1.0e-6);
    CHECK (maxDiffDelayed (r, y, 0, 4800, gm6) < 1.0e-6);
    setValue (rig.values, "in.gainL", 0.0f);
    setValue (rig.values, "out.gainR", 0.0f);

    setValue (rig.values, "io.swap", 1.0f);
    rig.run (l = x, r = y);
    CHECK (maxDiffDelayed (l, y, 0, 4800) < 1.0e-7);
    CHECK (maxDiffDelayed (r, x, 0, 4800) < 1.0e-7);
    setValue (rig.values, "io.swap", 0.0f);

    setValue (rig.values, "io.mono", 1.0f);
    rig.run (l = x, r = y);
    rig.run (l = x, r = y);
    CHECK (maxDiffDelayed (l, r, 0, 0) < 1.0e-7);
    CHECK (maxDiffDelayed (l, pstest::added (pstest::scaled (x, 0.5f), pstest::scaled (y, 0.5f)), 0, 0) < 1.0e-7);
    setValue (rig.values, "io.mono", 0.0f);

    // meters at unity: input and output agree; peaks; clip flags
    rig.chain.resetIntegratedLoudness();
    auto& meters = rig.chain.getIoMeters();
    for (auto* f : { &meters.outputPeak[0], &meters.outputPeak[1], &meters.outputTruePeak })
        f->exchange (0.0f);
    rig.run (l = x, r = x);
    rig.run (l = x, r = x);
    const auto& m = meters;
    note ("output momentary LUFS of a -20 dBFS sine in both channels", m.outputMomentaryLufs.load());
    CHECK_NEAR (m.outputMomentaryLufs.load(), m.inputMomentaryLufs.load(), 0.01);
    CHECK_NEAR (m.outputMomentaryLufs.load(), -20.0, 0.1);
    CHECK_NEAR (m.outputIntegratedLufs.load(), -20.0, 0.1);
    CHECK_NEAR (m.outputPeak[0].load(), 0.1, 0.001);
    CHECK_NEAR (m.outputTruePeak.load(), 0.1, 0.002);
    CHECK_EQ (m.outputClip[0].load(), 0.0f);
    setValue (rig.values, "in.gainL", 24.0f);
    rig.run (l = x, r = x);
    CHECK_EQ (m.inputClip[0].load(), 1.0f);
    CHECK_EQ (m.outputClip[0].load(), 1.0f);
    CHECK_EQ (m.inputClip[1].load(), 0.0f);
    setValue (rig.values, "in.gainL", 0.0f);

    // DC filter: 1 Hz high-pass ahead of the Maximizer
    std::vector<float> dc ((size_t) (6.0 * sr));
    for (size_t i = 0; i < dc.size(); ++i)
        dc[i] = 0.2f + x[i % x.size()];
    auto meanOfTail = [&] (const std::vector<float>& v)
    {
        double acc = 0.0;
        const size_t n = (size_t) sr;
        for (size_t i = v.size() - n; i < v.size(); ++i)
            acc += v[i];
        return acc / (double) n;
    };
    setValue (rig.values, "dith.dc", 1.0f);
    rig.run (l = dc, r = dc);
    note ("DC left after 6 s with the DC filter (from 0.2)", meanOfTail (l));
    CHECK (std::abs (meanOfTail (l)) < 0.002);

    rig.chain.setOrder ({ ModuleType::Equalizer1 });   // no Maximizer: the filter is not applied
    rig.run (l = dc, r = dc);
    CHECK_NEAR (meanOfTail (l), 0.2, 0.001);
}

TEST_CASE ("chain. order edits are published, new modules reset, removed modules stop")
{
    auto* a = new TestModule (ModuleType::Equalizer1, 1.0f, 10);
    Rig rig ({ { ModuleType::Equalizer1, a } });
    TestModule* b = rig.mods[(int) ModuleType::Imager];
    b->latency = 20;
    auto l = pstest::pinkNoise (0.2, 0.2, 48000.0, 13u), r = l;
    rig.run (l, r);
    CHECK_EQ (rig.chain.getLatencySamples(), 10);
    CHECK_EQ (b->processCalls, 0);

    rig.chain.setOrder ({ ModuleType::Imager, ModuleType::Equalizer1, ModuleType::Imager, (ModuleType) 99 });
    CHECK_EQ ((int) rig.chain.getOrder().size(), 2);
    CHECK (rig.chain.isInChain (ModuleType::Imager));
    const int resetsBefore = b->resets;
    rig.run (l, r);
    CHECK_EQ (b->resets, resetsBefore + 1);
    CHECK (b->processCalls > 0);
    CHECK_EQ (rig.chain.getLatencySamples(), 30);

    rig.chain.setOrder ({ ModuleType::Imager });
    rig.run (l, r);
    const int callsA = a->processCalls;
    rig.run (l, r);
    CHECK_EQ (a->processCalls, callsA);
    CHECK_EQ (rig.chain.getLatencySamples(), 20);
    CHECK (! rig.chain.isInChain (ModuleType::Equalizer1));

    // bigger blocks than prepared are processed in chunks
    std::vector<float> big ((size_t) 5000, 0.25f), bigR = big;
    float* ch[2] { big.data(), bigR.data() };
    rig.chain.process (ch, 5000, rig.values.data(), HostInfo {});
    CHECK_EQ (big[4999], 0.25f);
}
