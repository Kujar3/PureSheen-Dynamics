#include "AllocationCounter.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/Chain.h"
#include "dsp/DynamicEq.h"
#include "dsp/MatchEq.h"
#include "dsp/Punch.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace
{
using namespace ps;

std::vector<float> latencyValues()
{
    std::vector<float> values;
    for (const auto& spec : paramSpecs())
        values.push_back (spec.defaultValue);
    return values;
}

void put (std::vector<float>& values, const char* id, float value)
{
    const int index = paramIndex (id);
    REQUIRE (index >= 0);
    values[(size_t) index] = value;
}

class LatencyGain : public Module
{
public:
    LatencyGain (ModuleType type, int latency, float gain) : type (type), latency (latency), gain (gain) {}
    ModuleType getType() const noexcept override { return type; }
    void prepare (double, int) override
    {
        for (auto& channel : ring)
            channel.assign ((size_t) latency + 1, 0.0f);
        reset();
    }
    void reset() noexcept override
    {
        for (auto& channel : ring)
            std::fill (channel.begin(), channel.end(), 0.0f);
        position = 0;
    }
    void setParams (ParamValues) noexcept override {}
    int getLatencySamples() const noexcept override { return latency; }
    void process (float* const* channels, int count, const ProcessContext&) noexcept override
    {
        for (int i = 0; i < count; ++i)
        {
            const size_t read = (position + 1) % ring[0].size();
            for (int c = 0; c < 2; ++c)
            {
                ring[c][position] = channels[c][i];
                channels[c][i] = ring[c][read] * gain;
            }
            position = read;
        }
    }
private:
    ModuleType type;
    int latency;
    float gain;
    std::vector<float> ring[2];
    size_t position = 0;
};
}

TEST_CASE ("chain. phase 2 latency prediction is pure and matches committed parameters")
{
    for (const double rate : { 48000.0, 96000.0, 192000.0 })
    {
        MatchEq match;
        DynamicEq dynamic;
        Punch punch;
        auto values = latencyValues();
        for (auto* module : { static_cast<Module*> (&match), static_cast<Module*> (&dynamic), static_cast<Module*> (&punch) })
            module->prepare (rate, 256);
        for (int mode = 0; mode < 2; ++mode)
            for (int size = 0; size < 4; ++size)
            {
                put (values, "meq.phase", (float) mode);
                put (values, "meq.firSize", (float) size);
                put (values, "deq.mode", (float) mode);
                put (values, "deq.firSize", (float) size);
                put (values, "deq.lookahead", (float) (size * 3));
                put (values, "pnch.xoverType", (float) mode);
                for (auto* module : { static_cast<Module*> (&match), static_cast<Module*> (&dynamic), static_cast<Module*> (&punch) })
                {
                    const int before = module->getLatencySamples();
                    const int predicted = module->getLatencySamplesForParams (values.data());
                    CHECK_EQ (module->getLatencySamples(), before);
                    module->setParams (values.data());
                    CHECK_EQ (module->getLatencySamples(), predicted);
                }
            }
    }
}

TEST_CASE ("chain. phase 2 latency changes keep neutral delta silent in stereo and t/s")
{
    for (const bool ts : { false, true })
        for (const auto type : { ModuleType::MatchEq, ModuleType::DynamicEq, ModuleType::Punch })
        {
            Chain chain;
            auto values = latencyValues();
            put (values, "meq.phase", 1.0f);
            put (values, "meq.firSize", 0.0f);
            put (values, "deq.firSize", 0.0f);
            if (ts && type != ModuleType::Punch)
                put (values, type == ModuleType::MatchEq ? "meq.chMode" : "deq.chMode", 3.0f);
            chain.setOrder ({ type });
            chain.setDelta (type, true);
            chain.prepare (48000.0, 128);
            // Blocks larger than the prepared block size exercise chunking across every fade boundary.
            std::vector<float> l (2048), r (2048);
            double peak = 0.0;
            long long allocations = 0;
            for (int block = 0; block < 80; ++block)
            {
                if (block == 16 || block == 48)
                {
                    const float phase = block == 16 ? 0.0f : 1.0f;
                    put (values, "meq.phase", phase);
                    put (values, "deq.mode", 1.0f - phase);
                    put (values, "deq.lookahead", block == 16 ? 7.0f : 0.0f);
                    put (values, "pnch.xoverType", 1.0f - phase);
                }
                for (size_t i = 0; i < l.size(); ++i)
                    l[i] = r[i] = 0.25f * (float) std::sin (0.019 * ((double) block * l.size() + i));
                float* channels[2] { l.data(), r.data() };
                {
                    pstest::AllocationScope scope;
                    chain.process (channels, (int) l.size(), values.data(), {});
                    allocations += scope.count();
                }
                for (float sample : l)
                {
                    CHECK (std::isfinite (sample));
                    peak = std::max (peak, std::abs ((double) sample));
                }
            }
            CHECK (peak < 2.0e-6);
            CHECK_EQ (allocations, 0LL);
        }
}

TEST_CASE ("chain. phase 2 latency changes fade bypass and upstream delta before re-tapping")
{
    for (const bool bypass : { false, true })
    {
        Chain chain;
        auto values = latencyValues();
        put (values, "meq.phase", 1.0f);
        put (values, "meq.firSize", 0.0f);
        put (values, "deq.firSize", 0.0f);
        put (values, "io.bypass", bypass ? 1.0f : 0.0f);
        chain.setModule (ModuleType::Equalizer1, std::make_unique<LatencyGain> (ModuleType::Equalizer1, 0, 2.0f));
        chain.setOrder ({ ModuleType::Equalizer1, ModuleType::MatchEq, ModuleType::DynamicEq });
        chain.setDelta (ModuleType::Equalizer1, ! bypass);
        chain.prepare (48000.0, 128);
        std::vector<float> l (256), r (256);
        double last = 0.0, step = 0.0;
        for (int block = 0; block < 240; ++block)
        {
            if (block == 80 || block == 160)
            {
                put (values, "meq.phase", block == 80 ? 0.0f : 1.0f);
                put (values, "deq.mode", block == 80 ? 1.0f : 0.0f);
                put (values, "deq.lookahead", block == 80 ? 7.0f : 0.0f);
            }
            for (size_t i = 0; i < l.size(); ++i)
                l[i] = r[i] = 0.25f * (float) std::sin (0.013 * ((double) block * l.size() + i));
            float* channels[2] { l.data(), r.data() };
            chain.process (channels, (int) l.size(), values.data(), {});
            for (float sample : l)
            {
                CHECK (std::isfinite (sample));
                if (block > 20)
                    step = std::max (step, std::abs ((double) sample - last));
                last = sample;
            }
        }
        CHECK (step < 0.004);
    }
}

TEST_CASE ("chain. phase 2 full-length downstream delta and bypass remain aligned")
{
    // Four longest FIRs plus spectral modules exceed the Core chain's old delta capacity. The rate immediately
    // above a FIR-family boundary also exceeds a capacity based only on seconds rather than FIR family.
    for (const double rate : { 8000.0, 48000.0, 50001.0, 100001.0 })
    {
        const int family = rate > 100000.0 ? 4 : (rate > 50000.0 ? 2 : 1);
        const int slotLatency = 18432 * family;
        const int total = slotLatency * 4;
        Chain chain;
        auto values = latencyValues();
        chain.setModule (ModuleType::Punch, std::make_unique<LatencyGain> (ModuleType::Punch, 0, 2.0f));
        for (auto type : { ModuleType::Equalizer1, ModuleType::Equalizer2, ModuleType::MatchEq, ModuleType::DynamicEq })
            chain.setModule (type, std::make_unique<LatencyGain> (type, slotLatency, 1.0f));
        chain.setOrder ({ ModuleType::Punch, ModuleType::Equalizer1, ModuleType::Equalizer2, ModuleType::MatchEq, ModuleType::DynamicEq });
        chain.setDelta (ModuleType::Punch, true);
        chain.prepare (rate, 4096);
        std::vector<float> l ((size_t) total + 4096, 0.0f), r = l;
        for (const bool bypass : { false, true })
        {
            chain.reset();
            put (values, "io.bypass", bypass ? 1.0f : 0.0f);
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            l[0] = r[0] = 0.25f;
            float* channels[2] { l.data(), r.data() };
            chain.process (channels, (int) l.size(), values.data(), {});
            CHECK_EQ (chain.getLatencySamples(), total);
            CHECK_EQ (l[(size_t) total], 0.25f);
            double other = 0.0;
            for (size_t i = 0; i < l.size(); ++i)
            {
                CHECK (std::isfinite (l[i]));
                if (i != (size_t) total)
                    other = std::max (other, std::abs ((double) l[i]));
            }
            CHECK_EQ (other, 0.0);
        }
    }
}
