#include "assist/OfflineChain.h"

#include "core/Loudness.h"
#include "dsp/Chain.h"
#include "dsp/Params.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace ps::assist
{
namespace
{
constexpr int kBlock = 512;

bool cancelled (const std::atomic<bool>* c) noexcept { return c != nullptr && c->load (std::memory_order_relaxed); }

float paramValue (const std::vector<float>& v, const char* id)
{
    const int i = paramIndex (id);
    return i >= 0 ? v[(size_t) i] : 0.0f;
}

void setParam (std::vector<float>& v, const char* id, float value)
{
    const int i = paramIndex (id);
    if (i >= 0)
    {
        const auto& s = paramSpecs()[(size_t) i];
        v[(size_t) i] = std::clamp (value, s.minValue, s.maxValue);
    }
}

std::vector<ModuleType> maximizerOnly() { return { ModuleType::Maximizer }; }

bool hasMaximizer (const std::vector<ModuleType>& order)
{
    return std::find (order.begin(), order.end(), ModuleType::Maximizer) != order.end();
}

// Linear interpolation of a table (positions ascending).
float interpolate (const std::array<float, Suggestion::kTablePoints>& x, const std::array<float, Suggestion::kTablePoints>& y,
                   float at) noexcept
{
    if (at <= x[0])
        return y[0];
    for (int i = 1; i < Suggestion::kTablePoints; ++i)
        if (at <= x[(size_t) i])
        {
            const float span = x[(size_t) i] - x[(size_t) i - 1];
            const float u = span > 0.0f ? (at - x[(size_t) i - 1]) / span : 0.0f;
            return y[(size_t) i - 1] + u * (y[(size_t) i] - y[(size_t) i - 1]);
        }
    return y[Suggestion::kTablePoints - 1];
}
} // namespace

Excerpt loudestExcerpt (const float* left, const float* right, long long numSamples, double sampleRate, double seconds)
{
    Excerpt e;
    e.sampleRate = sampleRate;
    if (left == nullptr || numSamples <= 0)
        return e;

    const auto want = (long long) std::max (1.0, seconds * sampleRate);
    long long start = 0, length = std::min (want, numSamples);

    if (numSamples > want)
    {
        // Energy per 100 ms block; the run of blocks covering `seconds` with the most energy wins.
        const auto block = std::max<long long> (1, (long long) (0.1 * sampleRate));
        std::vector<double> energy;
        for (long long pos = 0; pos + block <= numSamples; pos += block)
        {
            double sum = 0.0;
            for (long long i = pos; i < pos + block; ++i)
            {
                const double l = left[i], r = right != nullptr ? right[i] : left[i];
                sum += l * l + r * r;
            }
            energy.push_back (sum);
        }

        const auto span = (size_t) std::max<long long> (1, want / block);
        double run = 0.0, best = -1.0;
        for (size_t i = 0; i < energy.size(); ++i)
        {
            run += energy[i];
            if (i >= span)
                run -= energy[i - span];
            if (i + 1 >= span && run > best)
            {
                best = run;
                start = std::clamp ((long long) (i + 1 - span) * block, 0LL, numSamples - length);
            }
        }
    }

    e.left.assign (left + start, left + start + length);
    e.right.assign ((right != nullptr ? right : left) + start, (right != nullptr ? right : left) + start + length);
    return e;
}

bool renderSeries (const std::vector<ModuleType>& order, const std::vector<float>& values, const Excerpt& in,
                   Excerpt& out, bool stopAtMaximizer, const std::atomic<bool>* cancel)
{
    out.sampleRate = in.sampleRate;
    out.left = in.left;
    out.right = in.right;

    std::vector<std::unique_ptr<Module>> modules;
    for (auto t : order)
    {
        if (stopAtMaximizer && t == ModuleType::Maximizer)
            break;
        if (auto m = createModule (t))
        {
            m->prepare (in.sampleRate, kBlock);
            m->setParams (values.data());
            m->reset();
            modules.push_back (std::move (m));
        }
    }

    ProcessContext ctx;
    for (long long pos = 0; pos < out.size(); pos += kBlock)
    {
        if (cancelled (cancel))
            return false;
        const int n = (int) std::min<long long> (kBlock, out.size() - pos);
        float* ch[2] { out.left.data() + pos, out.right.data() + pos };
        for (auto& m : modules)
        {
            m->setParams (values.data());
            m->process (ch, n, ctx);
        }
    }
    return true;
}

float integratedLufs (const Excerpt& e)
{
    LoudnessMeter meter;
    meter.prepare (e.sampleRate);
    for (long long pos = 0; pos < e.size(); pos += 4096)
    {
        const int n = (int) std::min<long long> (4096, e.size() - pos);
        meter.process (e.left.data() + pos, e.right.data() + pos, n);
    }
    return meter.integratedLufs();
}

namespace
{
// The Maximizer's Gain and Trim as one line: above 0 dB the gain, below it the trim.
constexpr float kMinGain = -24.0f, kMaxGain = 20.0f;

void setGain (std::vector<float>& v, float gain)
{
    setParam (v, "max.gain", std::max (gain, 0.0f));
    setParam (v, "max.trim", std::min (gain, 0.0f));
}
} // namespace

bool refineLoudness (Suggestion& s, const Excerpt& in, const std::atomic<bool>* cancel)
{
    if (! hasMaximizer (s.order) || in.size() == 0)
        return true;

    Excerpt pre, post;
    if (! renderSeries (s.order, s.values, in, pre, true, cancel))
        return false;

    auto measure = [&] (float gain, float& lufs) {
        auto v = s.values;
        setGain (v, gain);
        if (! renderSeries (maximizerOnly(), v, pre, post, false, cancel))
            return false;
        lufs = integratedLufs (post);
        return true;
    };

    const float target = s.loudnessLufs;
    float g0 = paramValue (s.values, "max.gain") + paramValue (s.values, "max.trim"), l0 = 0.0f;
    if (! measure (g0, l0))
        return false;

    float g1 = std::clamp (g0 + 1.3f * (target - l0), kMinGain, kMaxGain), l1 = l0;
    if (std::abs (target - l0) > 0.1f && g1 != g0)
    {
        if (! measure (g1, l1))
            return false;

        for (int run = 0; run < 4 && std::abs (target - l1) > 0.1f; ++run)
        {
            const float slope = std::abs (l1 - l0) > 1.0e-3f ? (g1 - g0) / (l1 - l0) : 1.3f;
            const float g2 = std::clamp (g1 + std::clamp (slope, 0.5f, 6.0f) * (target - l1), kMinGain, kMaxGain);
            if (g2 == g1)
                break;
            g0 = g1;
            l0 = l1;
            g1 = g2;
            if (! measure (g1, l1))
                return false;
        }
    }
    else
    {
        g1 = g0;
    }

    // Slope around the result (for the Loudness macro): one more point 1 dB above when the last step was tiny.
    float gs = g1 + 1.0f, ls = 0.0f;
    if (gs <= 20.0f && measure (gs, ls) && ls - l1 > 0.05f)
        s.gainPerLufs = std::clamp (1.0f / (ls - l1), 0.8f, 6.0f);

    setGain (s.values, g1);
    s.loudnessMeasured = true;
    return ! cancelled (cancel);
}

bool buildLoudnessTables (Suggestion& s, const Excerpt& in, const Analysis& a, const std::atomic<bool>* cancel)
{
    if (in.size() == 0)
        return true;

    auto preLoudness = [&] (const MacroValues& m, float& lufs) {
        const auto applied = applyMacros (s, m, 0.0f, s.order, a);
        auto v = s.values;
        for (const auto& [i, value] : applied.values)
            v[(size_t) i] = value;
        Excerpt out;
        if (! renderSeries (applied.order, v, in, out, true, cancel))
            return false;
        lufs = integratedLufs (out);
        return true;
    };

    float base = 0.0f;
    if (! preLoudness (s.macros, base))
        return false;

    for (int k = 0; k < kNumMacros; ++k)
    {
        const auto& info = macroInfo ((Macro) k);
        if (! info.loudnessMatched)
            continue;

        for (int p = 0; p < Suggestion::kTablePoints; ++p)
        {
            const float pos = info.minValue + (info.maxValue - info.minValue) * (float) p / (Suggestion::kTablePoints - 1);
            auto m = s.macros;
            m[(size_t) k] = pos;
            float l = base;
            if (std::abs (pos - s.macros[(size_t) k]) > 1.0e-4f && ! preLoudness (m, l))
                return false;
            s.tablePositions[(size_t) k][(size_t) p] = pos;
            s.tableDeltaLu[(size_t) k][(size_t) p] = std::isfinite (l) && l > kNoValue ? l - base : 0.0f;
        }
    }

    s.tablesReady = true;
    return true;
}

float loudnessCompensation (const Suggestion& s, const MacroValues& m)
{
    if (! s.tablesReady)
        return 0.0f;

    float db = 0.0f;
    for (int k = 0; k < kNumMacros; ++k)
    {
        if (! macroInfo ((Macro) k).loudnessMatched)
            continue;
        const float now = interpolate (s.tablePositions[(size_t) k], s.tableDeltaLu[(size_t) k], m[(size_t) k]);
        const float own = interpolate (s.tablePositions[(size_t) k], s.tableDeltaLu[(size_t) k], s.macros[(size_t) k]);
        db -= now - own;
    }
    return db;
}
} // namespace ps::assist
