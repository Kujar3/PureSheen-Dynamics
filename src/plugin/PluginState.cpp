#include "plugin/PluginState.h"

#include <algorithm>
#include <cmath>

namespace psplug
{
namespace
{
const juce::Identifier kP ("P"), kId ("id"), kV ("v"), kParam ("PARAM"), kValue ("value");
const juce::Identifier kChain ("Chain"), kModule ("Module"), kType ("type"), kParams ("Params");

struct MonitoringTable
{
    std::vector<char> flags;

    MonitoringTable()
    {
        flags.assign (ps::paramSpecs().size(), 0);
        for (const char* id : { "io.bypass", "io.gainMatch", "io.gainMatchModern" })
        {
            const int i = ps::paramIndex (id);
            if (i >= 0)
                flags[(size_t) i] = 1;
        }
    }
};

const MonitoringTable& monitoring()
{
    static const MonitoringTable table;
    return table;
}

bool validIndex (int index) noexcept
{
    return index >= 0 && index < (int) ps::paramSpecs().size();
}

juce::String trimZeros (juce::String s)
{
    if (s.containsChar ('.'))
    {
        while (s.endsWithChar ('0'))
            s = s.dropLastCharacters (1);
        if (s.endsWithChar ('.'))
            s = s.dropLastCharacters (1);
    }
    return s;
}
} // namespace

//==============================================================================
bool isMonitoringParam (int index) noexcept
{
    const auto& f = monitoring().flags;
    return index >= 0 && index < (int) f.size() && f[(size_t) index] != 0;
}

bool isInPresetScope (int index, int moduleOrMinus1) noexcept
{
    if (! validIndex (index) || isMonitoringParam (index))
        return false;

    const auto& s = ps::paramSpecs()[(size_t) index];
    if (moduleOrMinus1 < 0)
        return true;
    if (s.module != moduleOrMinus1)
        return false;

    // The power switch is the first slot parameter of every module ("<prefix>.on").
    const auto& prefix = ps::moduleInfo ((ps::ModuleType) moduleOrMinus1).idPrefix;
    return s.id != std::string (prefix) + ".on";
}

bool sameValue (int index, float a, float b) noexcept
{
    if (a == b)
        return true;
    if (! validIndex (index))
        return false;

    const auto& s = ps::paramSpecs()[(size_t) index];
    if (s.kind != ps::ParamKind::Float)
        return std::lround (a) == std::lround (b);
    return std::abs (a - b) <= 1.0e-5f * (s.maxValue - s.minValue);
}

bool sameState (const FullState& a, const FullState& b, bool compareOrder) noexcept
{
    if (compareOrder && a.order != b.order)
        return false;
    if (a.values.size() != b.values.size())
        return false;
    for (size_t i = 0; i < a.values.size(); ++i)
        if (! isMonitoringParam ((int) i) && ! sameValue ((int) i, a.values[i], b.values[i]))
            return false;
    return true;
}

std::vector<int> changedParams (const FullState& from, const FullState& to)
{
    std::vector<int> changed;
    const auto n = std::min (from.values.size(), to.values.size());
    for (size_t i = 0; i < n; ++i)
        if (! isMonitoringParam ((int) i) && ! sameValue ((int) i, from.values[i], to.values[i]))
            changed.push_back ((int) i);
    return changed;
}

//==============================================================================
std::vector<float> defaultValues()
{
    std::vector<float> v;
    v.reserve (ps::paramSpecs().size());
    for (const auto& s : ps::paramSpecs())
        v.push_back (s.defaultValue);
    return v;
}

std::vector<ps::ModuleType> defaultChain()
{
    return { ps::kDefaultChain.begin(), ps::kDefaultChain.end() };
}

float sanitiseValue (int index, float v) noexcept
{
    if (! validIndex (index))
        return 0.0f;

    const auto& s = ps::paramSpecs()[(size_t) index];
    if (! std::isfinite (v))
        return s.defaultValue;

    switch (s.kind)
    {
        case ps::ParamKind::Bool:
            return v >= 0.5f ? 1.0f : 0.0f;

        case ps::ParamKind::Choice:
        {
            const auto n = (long) s.choices.size();
            return (float) juce::jlimit (0L, juce::jmax (0L, n - 1), (long) std::lround (v));
        }

        case ps::ParamKind::Float:
        default:
        {
            v = juce::jlimit (s.minValue, s.maxValue, v);
            if (s.interval > 0.0f)
                v = juce::jlimit (s.minValue, s.maxValue,
                                  s.minValue + s.interval * std::round ((v - s.minValue) / s.interval));
            return v;
        }
    }
}

juce::String formatParamValue (int index, float v)
{
    if (! validIndex (index))
        return {};

    const auto& s = ps::paramSpecs()[(size_t) index];

    if (s.kind == ps::ParamKind::Bool)
        return v >= 0.5f ? "On" : "Off";

    if (s.kind == ps::ParamKind::Choice)
    {
        const auto i = (size_t) juce::jlimit (0L, (long) s.choices.size() - 1, (long) std::lround (v));
        return s.choices.empty() ? juce::String() : juce::String (s.choices[i]);
    }

    const juce::String unit (s.unit);

    if (s.interval >= 1.0f)
        return juce::String ((int) std::lround (v)) + (unit.isEmpty() ? "" : " " + unit);

    if (unit == "dB" || unit == "dB/oct")
    {
        const auto r = std::round (v * 10.0f) / 10.0f;
        return (r > 0.0f ? "+" : "") + juce::String (r == 0.0f ? 0.0f : r, 1) + " " + unit;
    }
    if (unit == "Hz")
    {
        if (v >= 1000.0f)
            return trimZeros (juce::String (v / 1000.0f, 2)) + " kHz";
        return juce::String (v, v < 100.0f ? 1 : 0) + " Hz";
    }
    if (unit == "ms")
        return trimZeros (juce::String (v, v < 10.0f ? 2 : (v < 100.0f ? 1 : 0))) + " ms";
    if (unit == "%")
        return juce::String (v, 0) + " %";
    if (unit == ":1")
        return trimZeros (juce::String (v, 1)) + ":1";
    if (unit == "LUFS")
        return juce::String (v, 1) + " LUFS";

    return trimZeros (juce::String (v, 2)) + (unit.isEmpty() ? "" : " " + unit);
}

juce::String describeParam (int index, float v)
{
    if (! validIndex (index))
        return "Changed a parameter";
    return juce::String (ps::paramSpecs()[(size_t) index].name) + ": " + formatParamValue (index, v);
}

//==============================================================================
juce::String moduleIdPrefix (ps::ModuleType t)
{
    return ps::moduleInfo (t).idPrefix;
}

int moduleFromIdPrefix (const juce::String& id) noexcept
{
    for (int i = 0; i < ps::kNumModuleTypes; ++i)
        if (id == ps::moduleInfo ((ps::ModuleType) i).idPrefix)
            return i;
    return -1;
}

juce::String moduleName (ps::ModuleType t)
{
    return ps::moduleInfo (t).name;
}

juce::ValueTree chainToTree (const std::vector<ps::ModuleType>& order)
{
    juce::ValueTree tree (kChain);
    for (auto t : order)
    {
        juce::ValueTree m (kModule);
        m.setProperty (kType, moduleIdPrefix (t), nullptr);
        tree.appendChild (m, nullptr);
    }
    return tree;
}

std::vector<ps::ModuleType> chainFromTree (const juce::ValueTree& tree)
{
    std::vector<ps::ModuleType> order;
    for (const auto& m : tree)
    {
        if (! m.hasType (kModule))
            continue;
        const int t = moduleFromIdPrefix (m.getProperty (kType).toString());
        if (t >= 0 && std::find (order.begin(), order.end(), (ps::ModuleType) t) == order.end())
            order.push_back ((ps::ModuleType) t);
    }
    return order;
}

juce::ValueTree valuesToTree (const juce::Identifier& type, const std::vector<float>& values, int moduleOrMinus1,
                              bool includeMonitoring)
{
    juce::ValueTree tree (type);
    const auto& specs = ps::paramSpecs();
    for (size_t i = 0; i < specs.size() && i < values.size(); ++i)
    {
        const bool inScope = isInPresetScope ((int) i, moduleOrMinus1)
                             || (includeMonitoring && moduleOrMinus1 < 0 && isMonitoringParam ((int) i));
        if (! inScope)
            continue;
        juce::ValueTree p (kP);
        p.setProperty (kId, juce::String (specs[i].id), nullptr);
        p.setProperty (kV, values[i], nullptr);
        tree.appendChild (p, nullptr);
    }
    return tree;
}

int valuesFromTree (const juce::ValueTree& tree, std::vector<float>& values, int moduleOrMinus1, bool missingToDefault,
                    bool includeMonitoring)
{
    const auto& specs = ps::paramSpecs();
    if (values.size() != specs.size())
        values = defaultValues();

    auto inScope = [&] (int i)
    {
        return isInPresetScope (i, moduleOrMinus1) || (includeMonitoring && moduleOrMinus1 < 0 && isMonitoringParam (i));
    };

    std::vector<char> seen (specs.size(), 0);
    int read = 0;

    for (const auto& p : tree)
    {
        const bool ours = p.hasType (kP), apvts = p.hasType (kParam);
        if (! ours && ! apvts)
            continue;

        const int i = ps::paramIndex (p.getProperty (kId).toString().toStdString());
        if (i < 0 || ! inScope (i))
            continue;

        const auto& var = p.getProperty (ours ? kV : kValue);
        if (var.isVoid())
            continue;

        values[(size_t) i] = sanitiseValue (i, (float) (double) var);
        seen[(size_t) i] = 1;
        ++read;
    }

    if (missingToDefault)
        for (size_t i = 0; i < specs.size(); ++i)
            if (seen[i] == 0 && inScope ((int) i))
                values[i] = specs[i].defaultValue;

    return read;
}

juce::ValueTree fullStateToTree (const juce::Identifier& type, const FullState& s)
{
    juce::ValueTree tree (type);
    tree.appendChild (chainToTree (s.order), nullptr);
    tree.appendChild (valuesToTree (kParams, s.values, -1), nullptr);
    return tree;
}

bool fullStateFromTree (const juce::ValueTree& tree, FullState& s)
{
    const auto params = tree.getChildWithName (kParams);
    const auto chain = tree.getChildWithName (kChain);
    if (! params.isValid() || ! chain.isValid())
        return false;

    s.values = defaultValues();
    valuesFromTree (params, s.values, -1, true);
    s.order = chainFromTree (chain);
    return true;
}
} // namespace psplug
