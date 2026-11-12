#include "plugin/Bridge.h"

#include "dsp/Dynamics.h"
#include "dsp/Maximizer.h"
#include "plugin/Assistant.h"
#include "plugin/PluginProcessor.h"
#include "plugin/PluginState.h"
#include "plugin/TrackReferencing.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

// The commands Claude can send (Bridge.h), all on the message thread. They use the processor's public calls, the ones
// the editor uses, so every edit is an undo step and the editor follows. Monitoring (solo, delta, A/B) is left out on
// purpose: Claude cannot hear it, and a forgotten solo would change what the user hears.
namespace psplug
{
namespace
{
using Args = const juce::var&;

[[noreturn]] void fail (const juce::String& message) { throw message; }

juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto object = std::make_unique<juce::DynamicObject>();
    for (const auto& [name, value] : properties)
        object->setProperty (name, value);
    return juce::var (object.release());
}

// JSON has no NaN or infinity; a meter or analysis with nothing to say is null.
juce::var number (float v, double scale = 10000.0)
{
    if (! std::isfinite (v) || v <= ps::kNoValue)
        return {};
    return std::round ((double) v * scale) / scale + 0.0;   // + 0.0: never "-0.0"
}

juce::var numberD (double v)
{
    if (! std::isfinite (v))
        return {};
    return std::round (v * 10000.0) / 10000.0;
}

juce::String requireString (Args args, const char* name)
{
    const auto value = args[name].toString().trim();
    if (! args[name].isString() || value.isEmpty())
        fail (juce::String ("Missing '") + name + "'.");
    return value;
}

bool isSaneNumber (const juce::var& value)
{
    return (value.isInt() || value.isInt64() || value.isDouble()) && std::abs ((double) value) < 1.0e9;   // false for NaN too
}

float requireNumber (Args args, const char* name)
{
    if (! isSaneNumber (args[name]))
        fail (juce::String ("'") + name + "' must be a number.");
    return (float) (double) args[name];
}

int requireInt (Args args, const char* name)
{
    const auto& value = args[name];
    if (! isSaneNumber (value) || (double) value != std::floor ((double) value))
        fail (juce::String ("'") + name + "' must be a whole number.");
    return (int) (double) value;
}

ps::ModuleType requireModule (const juce::String& text)
{
    for (int i = 0; i < (int) ps::ModuleType::Count; ++i)
    {
        const auto type = (ps::ModuleType) i;
        if (text.equalsIgnoreCase (moduleIdPrefix (type)) || text.equalsIgnoreCase (moduleName (type)))
            return type;
    }
    fail ("Unknown module '" + text + "'. list_modules has the ids.");
}

// A preset scope: -1 for presets of the whole chain.
int requireScope (Args args)
{
    const auto text = args["scope"].toString().trim();
    if (text.isEmpty() || text.equalsIgnoreCase ("global"))
        return -1;
    return (int) requireModule (text);
}

juce::File requireAudioFile (Args args)
{
    const auto path = requireString (args, "path");
    if (! juce::File::isAbsolutePath (path))
        fail ("'path' must be a full path.");
    const juce::File file (path);
    if (! file.existsAsFile())
        fail ("There is no file at " + path);
    if (! TrackReferencing::isSupportedFile (file))
        fail ("Not a supported audio file. Use " + TrackReferencing::getSupportedWildcard());
    return file;
}

//==============================================================================
juce::var chainToVar (const PureSheenProcessor& p)
{
    juce::Array<juce::var> list;
    for (auto type : p.getChainOrder())
    {
        const auto prefix = moduleIdPrefix (type);
        list.add (makeObject ({ { "module", prefix }, { "name", moduleName (type) },
                                { "on", p.getParameterValue (prefix + ".on") >= 0.5f } }));
    }
    return list;
}

juce::var parameterToVar (const PureSheenProcessor& p, int index)
{
    const auto& spec = ps::paramSpecs()[(size_t) index];
    const auto id = juce::String (spec.id);
    const float value = p.getParameterValue (id);

    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty ("id", id);
    object->setProperty ("name", juce::String (spec.name));
    object->setProperty ("value", number (value));
    if (auto* parameter = p.getParameter (id))
        object->setProperty ("text", parameter->getCurrentValueAsText());
    object->setProperty ("default", number (spec.defaultValue));

    if (spec.kind == ps::ParamKind::Float)
    {
        object->setProperty ("kind", "number");
        object->setProperty ("min", number (spec.minValue));
        object->setProperty ("max", number (spec.maxValue));
        if (! spec.unit.empty())
            object->setProperty ("unit", juce::String (spec.unit));
    }
    else if (spec.kind == ps::ParamKind::Bool)
    {
        object->setProperty ("kind", "switch");
    }
    else
    {
        object->setProperty ("kind", "choice");
        juce::Array<juce::var> choices;
        for (const auto& c : spec.choices)
            choices.add (juce::String (c));
        object->setProperty ("choices", choices);
    }
    return juce::var (object.release());
}

// What was asked for, as a double: a value past float's range must still end at the top of the parameter's range.
double parameterValueFrom (const ps::ParamSpec& spec, const juce::var& value)
{
    const auto id = juce::String (spec.id);
    const bool isNumber = value.isInt() || value.isInt64() || value.isDouble();

    if (isNumber && ! std::isfinite ((double) value))
        fail ("'" + id + "' needs a finite number.");

    switch (spec.kind)
    {
        case ps::ParamKind::Bool:
            if (value.isBool())
                return (bool) value ? 1.0 : 0.0;
            if (isNumber)
                return (double) value >= 0.5 ? 1.0 : 0.0;
            if (value.toString().equalsIgnoreCase ("on"))
                return 1.0;
            if (value.toString().equalsIgnoreCase ("off"))
                return 0.0;
            fail ("'" + id + "' is a switch: use true or false.");

        case ps::ParamKind::Choice:
            if (isNumber)
            {
                const double index = (double) value;
                if (index != std::floor (index) || index < 0.0 || index >= (double) spec.choices.size())
                    fail ("'" + id + "' has " + juce::String ((int) spec.choices.size()) + " choices, from index 0.");
                return index;
            }
            for (size_t i = 0; i < spec.choices.size(); ++i)
                if (value.toString().equalsIgnoreCase (juce::String (spec.choices[i])))
                    return (double) i;
            fail ("'" + id + "' has no choice '" + value.toString() + "'. get_parameters lists its choices.");

        case ps::ParamKind::Float:
        default:
            if (isNumber)
                return (double) value;
            fail ("'" + id + "' needs a number" + (spec.unit.empty() ? juce::String() : " in " + juce::String (spec.unit)) + ".");
    }
}

//==============================================================================
juce::var cmdGetStatus (PureSheenProcessor& p, Args)
{
    const auto rate = p.getChain().getSampleRate();
    const int latency = p.getCurrentLatencySamples();

    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty ("product", p.getName());
   #ifdef PURESHEEN_VERSION
    object->setProperty ("version", PURESHEEN_VERSION);
   #endif
    object->setProperty ("host", juce::String (juce::PluginHostType().getHostDescription()));
    object->setProperty ("track", p.getTrackName());
    object->setProperty ("sample_rate", numberD (rate));
    object->setProperty ("latency_samples", latency);
    object->setProperty ("latency_ms", rate > 0.0 ? numberD (1000.0 * latency / rate) : juce::var());
    object->setProperty ("preset", p.getCurrentPresetName());
    object->setProperty ("preset_modified", p.isPresetModified());
    object->setProperty ("bypassed", p.getParameterValue ("io.bypass") >= 0.5f);
    object->setProperty ("host_playing", p.getTrackReferencing().isHostPlaying());
    object->setProperty ("chain", chainToVar (p));
    object->setProperty ("can_undo", p.canUndo());
    object->setProperty ("can_redo", p.canRedo());
    const int snapshot = p.getActiveSnapshot();
    object->setProperty ("active_snapshot", snapshot >= 0 ? juce::var (juce::String::charToString ((juce::juce_wchar) ('A' + snapshot))) : juce::var());
    return juce::var (object.release());
}

// (1.2) Multiband Dynamics, per band and channel set: what each band's compressor and limiter do right now (the latest
// audio block; the detector is the level they react to). Stereo has one set; M/S and L/R two in one instance; in
// Transient / Sustain the sustain part is the twin instance.
juce::var dynamicsBandsToVar (PureSheenProcessor& p)
{
    auto& chain = p.getChain();
    const int numBands = juce::jlimit (1, ps::kMaxBands, juce::roundToInt (p.getParameterValue ("dyn.bands")));
    const int modeIndex = juce::roundToInt (p.getParameterValue ("dyn.chMode"));
    const auto mode = (ps::ChannelMode) juce::jlimit (0, 3, modeIndex);
    const auto& modeChoices = ps::paramSpecs()[(size_t) ps::paramIndex ("dyn.chMode")].choices;
    const auto modeName = modeIndex >= 0 && modeIndex < (int) modeChoices.size() ? juce::String (modeChoices[(size_t) modeIndex])
                                                                                  : juce::String (modeIndex);

    struct Source { const ps::DynamicsMeters* meters; int set; const char* name; };
    const auto& first = static_cast<ps::Dynamics&> (chain.getModule (ps::ModuleType::Dynamics, 0)).getMeters();
    std::vector<Source> sources;
    switch (mode)
    {
        case ps::ChannelMode::Stereo:    sources = { { &first, 0, "stereo" } }; break;
        case ps::ChannelMode::MidSide:   sources = { { &first, 0, "mid" }, { &first, 1, "side" } }; break;
        case ps::ChannelMode::LeftRight: sources = { { &first, 0, "left" }, { &first, 1, "right" } }; break;
        case ps::ChannelMode::TransientSustain:
            sources = { { &first, 0, "transient" },
                        { &static_cast<ps::Dynamics&> (chain.getModule (ps::ModuleType::Dynamics, 1)).getMeters(), 0, "sustain" } };
            break;
    }

    const auto db = [] (const ps::AtomicFloat& v)
    {
        const float x = v.load();
        return x > -150.0f ? number (x, 100.0) : juce::var();
    };

    juce::Array<juce::var> bands;
    for (int b = 0; b < numBands; ++b)
    {
        const float lo = b == 0 ? 20.0f : p.getParameterValue ("dyn.xover" + juce::String (b));
        const float hi = b == numBands - 1 ? 20000.0f : p.getParameterValue ("dyn.xover" + juce::String (b + 1));
        juce::Array<juce::var> parts;
        for (const auto& s : sources)
            parts.add (makeObject ({ { "part", juce::String (s.name) },
                                     { "detector_db", db (s.meters->detectorDb[s.set][b]) },
                                     { "gain_change_db", number (s.meters->gainReductionDb[s.set][b].load(), 100.0) },
                                     { "compressor_db", number (s.meters->compGainDb[s.set][b].load(), 100.0) },
                                     { "limiter_db", number (s.meters->limGainDb[s.set][b].load(), 100.0) } }));
        bands.add (makeObject ({ { "band", b + 1 }, { "low_hz", number (lo, 1.0) }, { "high_hz", number (hi, 1.0) },
                                 { "parts", parts } }));
    }
    return makeObject ({ { "channel_mode", modeName }, { "bands", bands } });
}

// (1.3) The master's analysis bands, whatever is in the chain: level, how far the level moves, how far the loudest
// moments stand out, and width. The same analysis get_comparison reads, kept running while the connector is in use.
juce::var masterBandsToVar (PureSheenProcessor& p)
{
    const auto& refs = p.getTrackReferencing();
    const auto profile = refs.getMixProfile();
    if (! profile.valid)
        return {};

    juce::Array<juce::var> bands;
    for (int i = 0; i < ps::kNumBands; ++i)
    {
        const auto& band = profile.bands[(size_t) i];
        const auto& info = ps::bandInfo ((ps::Band) i);
        bands.add (makeObject ({ { "band", juce::String (info.name) }, { "low_hz", number (info.lowHz) }, { "high_hz", number (info.highHz) },
                                 { "level_db", number (band.levelDb, 100.0) },
                                 { "dynamic_range_db", number (band.dynamicRangeDb, 100.0) },
                                 { "peak_over_median_db", number (band.peakOverMedianDb, 100.0) },
                                 { "width_pct", number (band.widthPct, 100.0) } }));
    }
    const auto freshness = p.getAnalysisFreshness();
    return makeObject ({ { "analysed_seconds", numberD (profile.analysedSeconds) },
                         { "window", refs.getSettings().averaging == ps::Averaging::Recent ? "recent" : "since reset" },
                         { "settings_changed_during_analysis", freshness.blendsSettings },
                         { "bands", bands } });
}

juce::var cmdGetMeters (PureSheenProcessor& p, Args)
{
    auto& chain = p.getChain();
    auto& meters = chain.getIoMeters();

    // The true peak meters hold their maximum until read, and this is their only reader.
    const auto truePeakDb = [] (ps::AtomicFloat& meter)
    {
        const float linear = meter.exchange (0.0f);
        return linear > 0.0f ? number (juce::Decibels::gainToDecibels (linear, -200.0f), 100.0) : juce::var();
    };

    auto levels = [] (float momentary, float shortTerm, float integrated)
    {
        return makeObject ({ { "momentary_lufs", number (momentary, 100.0) }, { "short_term_lufs", number (shortTerm, 100.0) },
                             { "integrated_lufs", number (integrated, 100.0) } });
    };

    auto input = levels (meters.inputMomentaryLufs.load(), meters.inputShortTermLufs.load(), meters.inputIntegratedLufs.load());
    input.getDynamicObject()->setProperty ("true_peak_dbtp", truePeakDb (meters.inputTruePeak));

    auto output = levels (meters.outputMomentaryLufs.load(), meters.outputShortTermLufs.load(), meters.outputIntegratedLufs.load());
    output.getDynamicObject()->setProperty ("loudness_range_lu", number (meters.outputLra.load(), 100.0));
    output.getDynamicObject()->setProperty ("true_peak_dbtp", truePeakDb (meters.outputTruePeak));
    output.getDynamicObject()->setProperty ("gain_match_db", number (meters.gainMatchDb.load(), 100.0));
    // Options > Gain Match: with this on, the output values above leave out the match gain (what the master measures).
    output.getDynamicObject()->setProperty ("meters_ignore_gain_match", chain.getMetersIgnoreGainMatch());

    juce::Array<juce::var> modules;
    for (auto type : p.getChainOrder())
        modules.add (makeObject ({ { "module", moduleIdPrefix (type) },
                                   { "output_minus_input_db", number (meters.moduleDifferenceDb[(int) type].load(), 100.0) } }));

    const double window = p.takeSecondsSinceMeterRead();
    const auto freshness = p.getAnalysisFreshness();
    const double position = p.getTrackReferencing().getHostSeconds();
    auto object = makeObject ({ { "input", input }, { "output", output }, { "modules", modules },
                                { "host_playing", p.getTrackReferencing().isHostPlaying() },
                                { "host_position_seconds", position >= 0.0 ? numberD (position) : juce::var() },
                                { "peak_window_seconds", window >= 0.0 ? numberD (window) : juce::var() },
                                { "seconds_since_last_change", freshness.secondsSinceChange >= 0.0 ? numberD (freshness.secondsSinceChange) : juce::var() } });

    if (p.isModuleInChain (ps::ModuleType::Maximizer))
    {
        auto& m = static_cast<ps::Maximizer&> (chain.getModule (ps::ModuleType::Maximizer)).getMeters();
        object.getDynamicObject()->setProperty ("maximizer", makeObject ({
            { "ceiling_db", number (p.getParameterValue ("max.ceiling"), 100.0) },
            { "ceiling_applies_to", p.getParameterValue ("max.truePeak") >= 0.5f ? "true peak" : "sample peak" },
            { "peak_gain_reduction_db", number (m.bridgePeakGainReductionDb.exchange (0.0f), 100.0) },
            { "gain_reduction_now_left_db", number (m.gainReductionDb[0].load(), 100.0) },
            { "gain_reduction_now_right_db", number (m.gainReductionDb[1].load(), 100.0) },
            { "upward_gain_db", number (m.upwardGainDb.load(), 100.0) } }));
    }

    if (p.isModuleInChain (ps::ModuleType::Dynamics))
        object.getDynamicObject()->setProperty ("dynamics", dynamicsBandsToVar (p));

    object.getDynamicObject()->setProperty ("master_bands", masterBandsToVar (p));

    const int solo = chain.getSolo();
    object.getDynamicObject()->setProperty ("solo", solo >= 0 ? juce::var (moduleIdPrefix ((ps::ModuleType) solo)) : juce::var());
    object.getDynamicObject()->setProperty ("solo_keeps_maximizer", chain.getSoloKeepsMaximizer());
    return object;
}

juce::var cmdResetLoudness (PureSheenProcessor& p, Args)
{
    p.getChain().resetIntegratedLoudness();
    return makeObject ({ { "reset", true } });
}

juce::var cmdResetComparison (PureSheenProcessor& p, Args)
{
    p.getTrackReferencing().resetMixAnalysis();
    return makeObject ({ { "reset", true } });
}

juce::var cmdListModules (PureSheenProcessor& p, Args)
{
    juce::Array<juce::var> list;
    for (int i = 0; i < (int) ps::ModuleType::Count; ++i)
    {
        const auto type = (ps::ModuleType) i;
        const auto prefix = moduleIdPrefix (type);
        int parameters = 0;
        for (const auto& spec : ps::paramSpecs())
            parameters += spec.module == i ? 1 : 0;
        list.add (makeObject ({ { "module", prefix }, { "name", moduleName (type) }, { "in_chain", p.isModuleInChain (type) },
                                { "on", p.getParameterValue (prefix + ".on") >= 0.5f }, { "parameters", parameters } }));
    }
    return makeObject ({ { "modules", list }, { "chain", chainToVar (p) } });
}

juce::var cmdGetParameters (PureSheenProcessor& p, Args args)
{
    const auto& specs = ps::paramSpecs();

    int moduleFilter = -2;   // -2 = any, -1 = the global ones
    const auto moduleText = args["module"].toString().trim();
    if (moduleText.equalsIgnoreCase ("global"))
        moduleFilter = -1;
    else if (moduleText.isNotEmpty())
        moduleFilter = (int) requireModule (moduleText);

    juce::StringArray ids;
    if (const auto* list = args["ids"].getArray())
        for (const auto& id : *list)
            ids.add (id.toString());
    for (const auto& id : ids)
        if (ps::paramIndex (id.toStdString()) < 0)
            fail ("Unknown parameter '" + id + "'.");

    const auto query = args["query"].toString().trim();
    const bool changedOnly = (bool) args["changed_only"];
    const int offset = juce::jmax (0, args.hasProperty ("offset") ? requireInt (args, "offset") : 0);
    const int limit = juce::jlimit (1, 1000, args.hasProperty ("limit") ? requireInt (args, "limit") : 200);

    // A skewed parameter does not round trip to exactly its default, so "changed" needs a small tolerance.
    const auto isChanged = [&p] (const ps::ParamSpec& spec)
    {
        return std::abs (p.getParameterValue (juce::String (spec.id)) - spec.defaultValue) > 1.0e-4f * juce::jmax (1.0f, spec.maxValue - spec.minValue);
    };

    // "eq1.a.b4." of "eq1.a.b4.gain"; empty when the parameter is not a band's.
    const auto bandOf = [] (const juce::String& id)
    {
        for (int at = id.indexOf (".b"); at >= 0; at = id.indexOf (at + 1, ".b"))
        {
            int end = at + 2;
            while (juce::CharacterFunctions::isDigit (id[end]))
                ++end;
            if (end > at + 2 && id[end] == '.')
                return id.substring (0, end + 1);
        }
        return juce::String();
    };

    // A band's shape and power are what its other values mean, so they come along even at their defaults.
    juce::StringArray changedBands;
    if (changedOnly)
        for (const auto& spec : specs)
            if (isChanged (spec))
                changedBands.addIfNotAlreadyThere (bandOf (juce::String (spec.id)));
    changedBands.removeEmptyStrings();

    std::map<juce::String, juce::String> firstWithChoices;
    juce::Array<juce::var> list;
    int matches = 0;
    for (int i = 0; i < (int) specs.size(); ++i)
    {
        const auto& spec = specs[(size_t) i];
        const auto id = juce::String (spec.id);
        if (moduleFilter != -2 && spec.module != moduleFilter)
            continue;
        if (! ids.isEmpty() && ! ids.contains (id))
            continue;
        if (query.isNotEmpty() && ! id.containsIgnoreCase (query) && ! juce::String (spec.name).containsIgnoreCase (query))
            continue;
        const auto band = bandOf (id);
        const bool bandContext = changedBands.contains (band) && (id == band + "shape" || id == band + "on");
        if (changedOnly && ! bandContext && ! isChanged (spec))
            continue;
        if (matches >= offset && list.size() < limit)
        {
            auto entry = parameterToVar (p, i);
            // A long list of choices once per reply: the eighteen filter shapes would repeat on every band.
            if (spec.choices.size() >= 8)
            {
                juce::String key;
                for (const auto& c : spec.choices)
                    key << c << "\n";
                const auto [first, isFirst] = firstWithChoices.try_emplace (key, id);
                if (! isFirst)
                {
                    entry.getDynamicObject()->removeProperty ("choices");
                    entry.getDynamicObject()->setProperty ("choices_as", first->second);
                }
            }
            list.add (entry);
        }
        ++matches;
    }
    return makeObject ({ { "total", matches }, { "offset", offset }, { "parameters", list } });
}

// What the reply should say about channel sets: the plugin's Channel Link ties A and B together only for edits in its
// window, and set B does nothing in Stereo. Empty for a parameter of neither set.
juce::String channelSetNote (const PureSheenProcessor& p, const ps::ParamSpec& spec)
{
    if (spec.module < 0)
        return {};
    const auto id = juce::String (spec.id);
    const auto prefix = moduleIdPrefix ((ps::ModuleType) spec.module);
    const bool setA = id.startsWith (prefix + ".a."), setB = id.startsWith (prefix + ".b.");
    if ((! setA && ! setB) || ps::paramIndex ((prefix + ".chMode").toStdString()) < 0)
        return {};

    if (p.getParameterValue (prefix + ".chMode") < 0.5f)   // Stereo comes first in every module's list
        return setB ? "This module is in Stereo, which uses set A only, so this has no effect until the module is in another channel mode."
                    : juce::String();
    return juce::String ("This changed set ") + (setA ? "A" : "B") + " alone. Channel Link only ties the sets together for edits in the plugin's window.";
}

juce::var cmdSetParameters (PureSheenProcessor& p, Args args)
{
    const auto* changes = args["changes"].getArray();
    if (changes == nullptr || changes->isEmpty())
        fail ("'changes' must be a list of { id, value }.");
    if (changes->size() > 256)
        fail ("At most 256 changes at once.");

    // Everything is checked first: a bad entry changes nothing.
    const auto& specs = ps::paramSpecs();
    std::vector<std::pair<juce::String, float>> values;
    std::vector<double> asked;
    std::vector<int> indices;
    std::vector<float> before;
    for (const auto& change : *changes)
    {
        const auto id = change["id"].toString();
        const int index = ps::paramIndex (id.toStdString());
        if (index < 0)
            fail ("Unknown parameter '" + id + "'. Nothing was changed.");
        if (isMonitoringParam (index))
            fail ("'" + id + "' is a monitoring switch, which undo cannot restore, so it stays with the user. Nothing was changed.");
        if (! change.hasProperty ("value"))
            fail ("'" + id + "' has no value. Nothing was changed.");
        const auto& spec = specs[(size_t) index];
        asked.push_back (parameterValueFrom (spec, change["value"]));
        values.emplace_back (id, (float) juce::jlimit ((double) spec.minValue, (double) spec.maxValue, asked.back()));
        indices.push_back (index);
        before.push_back (p.getParameterValue (id));
    }

    auto description = args["description"].toString().replaceCharacters ("\r\n\t", "   ").trim().substring (0, 80);
    if (description.startsWithIgnoreCase ("Claude:"))   // the history adds it
        description = description.substring (7).trimStart();
    p.setParameterValues (values, description);

    juce::Array<juce::var> results;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const auto& spec = specs[(size_t) indices[i]];
        const float now = p.getParameterValue (values[i].first);
        auto result = parameterToVar (p, indices[i]);
        auto* object = result.getDynamicObject();
        object->removeProperty ("default");
        object->removeProperty ("choices");
        object->setProperty ("previous", number (before[i]));
        if (const auto channelNote = channelSetNote (p, spec); channelNote.isNotEmpty())
            object->setProperty ("channel_note", channelNote);
        if (spec.kind == ps::ParamKind::Float && (asked[i] < spec.minValue || asked[i] > spec.maxValue))
            object->setProperty ("limited", "Asked for " + juce::String (asked[i]) + ", the range ends at " + juce::String (now));
        if (spec.module >= 0 && ! p.isModuleInChain ((ps::ModuleType) spec.module))
            object->setProperty ("note", moduleName ((ps::ModuleType) spec.module) + " is not in the chain, so this has no effect yet.");
        results.add (result);
    }
    return makeObject ({ { "changes", results } });
}

//==============================================================================
juce::var cmdAddModule (PureSheenProcessor& p, Args args)
{
    const auto type = requireModule (requireString (args, "module"));
    if (p.isModuleInChain (type))
        fail (moduleName (type) + " is already in the chain.");
    p.addModule (type, args.hasProperty ("position") ? requireInt (args, "position") : -1);
    return makeObject ({ { "chain", chainToVar (p) } });
}

juce::var cmdRemoveModule (PureSheenProcessor& p, Args args)
{
    const auto type = requireModule (requireString (args, "module"));
    if (! p.isModuleInChain (type))
        fail (moduleName (type) + " is not in the chain.");
    p.removeModule (type);
    return makeObject ({ { "chain", chainToVar (p) } });
}

juce::var cmdSetChain (PureSheenProcessor& p, Args args)
{
    const auto* list = args["order"].getArray();
    if (list == nullptr)
        fail ("'order' must be a list of module ids.");

    std::vector<ps::ModuleType> order;
    for (const auto& item : *list)
    {
        const auto type = requireModule (item.toString());
        if (std::find (order.begin(), order.end(), type) != order.end())
            fail (moduleName (type) + " is in the list twice. A chain has one of each module.");
        order.push_back (type);
    }
    p.setChainOrder (order);
    return makeObject ({ { "chain", chainToVar (p) } });
}

juce::var cmdResetModule (PureSheenProcessor& p, Args args)
{
    p.resetModule (requireModule (requireString (args, "module")));
    return makeObject ({ { "reset", true } });
}

juce::var historyToVar (const PureSheenProcessor& p, int limit)
{
    const auto history = p.getHistory();
    juce::Array<juce::var> list;
    for (int i = juce::jmax (0, (int) history.size() - limit); i < (int) history.size(); ++i)
    {
        auto step = makeObject ({ { "index", i }, { "description", history[(size_t) i].description } });
        if (history[(size_t) i].request > 0)
            step.getDynamicObject()->setProperty ("request", history[(size_t) i].request);
        list.add (step);
    }
    return makeObject ({ { "position", p.getHistoryPosition() }, { "can_undo", p.canUndo() }, { "can_redo", p.canRedo() }, { "steps", list } });
}

juce::var cmdGetHistory (PureSheenProcessor& p, Args args)
{
    return historyToVar (p, juce::jlimit (1, 1000, args.hasProperty ("limit") ? requireInt (args, "limit") : 30));
}

juce::var cmdUndo (PureSheenProcessor& p, Args)
{
    if (! p.canUndo())
        fail ("Nothing to undo.");
    p.undo();
    return historyToVar (p, 5);
}

juce::var cmdRedo (PureSheenProcessor& p, Args)
{
    if (! p.canRedo())
        fail ("Nothing to redo.");
    p.redo();
    return historyToVar (p, 5);
}

//==============================================================================
int requireSlot (Args args)
{
    const auto text = requireString (args, "slot").toUpperCase();
    if (text.length() != 1 || text[0] < 'A' || text[0] >= 'A' + PureSheenProcessor::kNumSnapshots)
        fail ("'slot' is A, B, C or D.");
    return (int) (text[0] - 'A');
}

juce::var cmdStoreSnapshot (PureSheenProcessor& p, Args args)
{
    const int slot = requireSlot (args);
    if (p.hasSnapshot (slot) && ! (bool) args["overwrite"])
        fail ("Snapshot " + args["slot"].toString() + " is in use. Pass overwrite: true only if the user agreed to replace it.");
    p.storeSnapshot (slot);
    return makeObject ({ { "stored", true } });
}

juce::var cmdRecallSnapshot (PureSheenProcessor& p, Args args)
{
    const int slot = requireSlot (args);
    if (! p.hasSnapshot (slot))
        fail ("Snapshot " + args["slot"].toString() + " is empty.");
    p.recallSnapshot (slot);
    return makeObject ({ { "recalled", true } });
}

//==============================================================================
juce::var cmdListPresets (PureSheenProcessor& p, Args args)
{
    juce::Array<juce::var> list;
    for (const auto& info : p.getPresets (requireScope (args)))
        list.add (makeObject ({ { "name", info.name }, { "category", info.category }, { "factory", info.factory } }));
    return makeObject ({ { "presets", list } });
}

juce::var cmdLoadPreset (PureSheenProcessor& p, Args args)
{
    const int scope = requireScope (args);
    const auto name = requireString (args, "name");
    const auto category = args["category"].toString().trim();
    for (const auto& info : p.getPresets (scope))
        if (info.name.equalsIgnoreCase (name) && (category.isEmpty() || info.category.equalsIgnoreCase (category)))
        {
            if (! p.loadPreset (scope, info))
                fail ("That preset could not be loaded.");
            return makeObject ({ { "loaded", info.name }, { "chain", chainToVar (p) } });
        }
    fail ("No preset named '" + name + "'. list_presets has the names.");
}

juce::var cmdSavePreset (PureSheenProcessor& p, Args args)
{
    const int scope = requireScope (args);
    const auto name = requireString (args, "name");
    const auto clean = juce::File::createLegalFileName (name).trim();
    if (args["category"].toString().contains ("..") || clean.containsOnly (". "))
        fail ("That name or category cannot be used for a preset.");
    if (! (bool) args["overwrite"])
        for (const auto& info : p.getPresets (scope))
            if (info.name.equalsIgnoreCase (clean))
                fail ("A preset named '" + clean + "' exists. Pass overwrite: true only if the user agreed to replace it.");
    if (! p.savePreset (scope, name, args["category"].toString()))
        fail ("That preset could not be saved.");
    return makeObject ({ { "saved", clean } });
}

//==============================================================================
juce::var cmdLearn (PureSheenProcessor& p, Args args)
{
    const auto target = requireString (args, "target");
    const auto action = args["action"].toString().trim();
    const bool start = action.isEmpty() || action == "start", cancel = action == "cancel", status = action == "status";
    if (! start && ! cancel && ! status)
        fail ("'action' is start, cancel or status.");

    if (target == "maximizer")
    {
        if (start)  p.startMaximizerLearn();
        if (cancel) p.cancelMaximizerLearn();
        return makeObject ({ { "learning", p.isMaximizerLearning() }, { "progress", number (p.getMaximizerLearnProgress()) },
                             { "gain_db", number (p.getParameterValue ("max.gain")) },
                             { "trim_db", number (p.getParameterValue ("max.trim")) } });
    }
    if (target == "de_limiter")
    {
        if (start)  p.startDeLimiterLearn();
        if (cancel) p.cancelDeLimiterLearn();
        return makeObject ({ { "learning", p.isDeLimiterLearning() }, { "waiting_for_audio", p.isDeLimiterWaitingForAudio() },
                             { "progress", number (p.getDeLimiterLearnProgress()) },
                             { "threshold_db", number (p.getParameterValue ("dlim.thr")) } });
    }
    fail ("'target' is maximizer or de_limiter.");
}

//==============================================================================
const char* phaseName (Assistant::Phase phase)
{
    switch (phase)
    {
        case Assistant::Phase::Listening: return "listening";
        case Assistant::Phase::Analysing: return "analysing";
        case Assistant::Phase::Ready:     return "ready";
        case Assistant::Phase::Failed:    return "failed";
        case Assistant::Phase::Idle:
        default:                          return "idle";
    }
}

const char* intensityName (ps::assist::Intensity intensity)
{
    switch (intensity)
    {
        case ps::assist::Intensity::Gentle: return "gentle";
        case ps::assist::Intensity::Strong: return "strong";
        case ps::assist::Intensity::Balanced:
        default:                            return "balanced";
    }
}

juce::var assistantToVar (const Assistant& a)
{
    juce::Array<juce::var> cards;
    const auto suggestions = a.getSuggestions();
    for (int i = 0; i < (int) suggestions.size(); ++i)
    {
        const auto& s = suggestions[(size_t) i];
        cards.add (makeObject ({ { "index", i }, { "name", juce::String (s.name) }, { "description", juce::String (s.description) },
                                 { "target", juce::String (ps::assist::targetName (s.target)) },
                                 { "loudness_lufs", number (s.loudnessLufs, 100.0) }, { "default", s.isDefault } }));
    }

    juce::Array<juce::var> macros;
    const auto values = a.getMacros();
    for (int i = 0; i < ps::assist::kNumMacros; ++i)
    {
        const auto& info = ps::assist::macroInfo ((ps::assist::Macro) i);
        macros.add (makeObject ({ { "macro", juce::String (info.id) }, { "name", juce::String (info.name) },
                                  { "value", number (values[(size_t) i], 100.0) }, { "min", number (info.minValue) },
                                  { "max", number (info.maxValue) }, { "unit", juce::String (info.unit) } }));
    }

    const auto settings = a.getSettings();
    const auto reference = a.getReference();
    return makeObject ({
        { "phase", juce::String (phaseName (a.getPhase())) },
        { "progress", number (a.getProgress(), 1000.0) },
        { "status", a.getStatusText() },
        { "heard_seconds", a.getPhase() == Assistant::Phase::Listening ? numberD (a.getHeardSeconds()) : juce::var() },
        { "working", a.isWorking() },
        { "has_audio", a.hasAnalysis() },
        { "detected_target", juce::String (ps::assist::targetName (a.getDetectedTarget())) },
        { "input_lufs", number (a.getInputLufs(), 100.0) },
        { "suggestions", cards },
        { "applied", a.getAppliedIndex() },
        { "macros", macros },
        { "loudness_matched", a.isLoudnessMatched() },
        { "reference", reference.valid ? juce::var (juce::String (reference.name)) : juce::var() },
        { "settings", makeObject ({
            { "profile", settings.engine.profile < 0 ? juce::var ("detect")
                                                     : juce::var (juce::String (ps::assist::targetName ((ps::ToneTarget) settings.engine.profile))) },
            { "auto_loudness", settings.engine.autoLoudness },
            { "loudness_lufs", number (settings.engine.loudnessLufs, 100.0) },
            { "intensity", juce::String (intensityName (settings.engine.intensity)) },
            { "use_maximizer", settings.engine.useMaximizer },
            { "ceiling_db", number (settings.engine.ceilingDb, 100.0) },
            { "true_peak", settings.engine.truePeak },
            { "analysis_seconds", number (settings.analysisSeconds, 10.0) },
            { "whole_song", settings.wholeSong } }) } });
}

juce::var cmdGetAssistant (PureSheenProcessor& p, Args) { return assistantToVar (p.getAssistant()); }

juce::var cmdAssistantListen (PureSheenProcessor& p, Args args)
{
    auto& a = p.getAssistant();
    const auto action = args["action"].toString();
    if (action == "cancel")
        a.cancel();
    else if (action == "finish")
    {
        if (! a.finishListening())
            fail ("There is no whole-song listen to finish. A normal listen ends by itself.");
    }
    else
        a.startListening();
    return assistantToVar (a);
}

juce::var cmdAssistantAnalyseFile (PureSheenProcessor& p, Args args)
{
    if (! p.getAssistant().analyseFile (requireAudioFile (args)))
        fail ("That file could not be read.");
    return assistantToVar (p.getAssistant());
}

juce::var cmdAssistantApply (PureSheenProcessor& p, Args args)
{
    if (! p.getAssistant().apply (requireInt (args, "index")))
        fail ("There is no such suggestion. get_assistant lists them.");
    return makeObject ({ { "applied", p.getAssistant().getAppliedIndex() }, { "chain", chainToVar (p) } });
}

juce::var cmdAssistantSetMacro (PureSheenProcessor& p, Args args)
{
    const auto id = requireString (args, "macro");
    if (p.getAssistant().getAppliedIndex() < 0)
        fail ("Macros work on an applied suggestion, and none is applied. See get_assistant.");
    for (int i = 0; i < ps::assist::kNumMacros; ++i)
    {
        const auto macro = (ps::assist::Macro) i;
        const auto& info = ps::assist::macroInfo (macro);
        if (id.equalsIgnoreCase (info.id) || id.equalsIgnoreCase (info.name))
        {
            const float gainBefore = p.getParameterValue ("max.gain"), trimBefore = p.getParameterValue ("max.trim");
            const double asked = requireNumber (args, "value");
            p.getAssistant().setMacro (macro, (float) juce::jlimit ((double) info.minValue, (double) info.maxValue, asked), true);
            const float gainAfter = p.getParameterValue ("max.gain"), trimAfter = p.getParameterValue ("max.trim");

            auto result = makeObject ({ { "macro", juce::String (info.id) }, { "value", number (p.getAssistant().getMacros()[(size_t) i], 100.0) },
                                        { "maximizer_gain_db", number (gainAfter, 100.0) }, { "maximizer_trim_db", number (trimAfter, 100.0) } });
            if (asked < info.minValue || asked > info.maxValue)
                result.getDynamicObject()->setProperty ("limited", "Asked for " + juce::String (asked) + ", the range ends at "
                                                                       + juce::String (asked < info.minValue ? info.minValue : info.maxValue));

            juce::StringArray moved;
            if (std::abs (gainAfter - gainBefore) > 0.005f)
                moved.add ("the Maximizer Gain from " + juce::String (gainBefore, 2) + " to " + juce::String (gainAfter, 2) + " dB");
            if (std::abs (trimAfter - trimBefore) > 0.005f)
                moved.add ("the Maximizer Trim from " + juce::String (trimBefore, 2) + " to " + juce::String (trimAfter, 2) + " dB");
            if (! moved.isEmpty())
                result.getDynamicObject()->setProperty ("note", "This also moved " + moved.joinIntoString (" and ")
                    + (macro == ps::assist::Macro::Loudness ? "." : ", to keep the loudness steady. loudness_matched in assistant_settings turns that off."));
            return result;
        }
    }
    fail ("Unknown macro '" + id + "'. get_assistant lists them.");
}

juce::var cmdAssistantSettings (PureSheenProcessor& p, Args args)
{
    auto& a = p.getAssistant();
    auto settings = a.getSettings();

    if (args.hasProperty ("profile"))
    {
        const auto text = args["profile"].toString().trim();
        int profile = -2;
        if (text.equalsIgnoreCase ("detect"))
            profile = -1;
        for (int i = 0; i < (int) ps::ToneTarget::Count && profile == -2; ++i)
            if (text.equalsIgnoreCase (ps::assist::targetName ((ps::ToneTarget) i)))
                profile = i;
        if (profile == -2)
            fail ("Unknown profile '" + text + "'.");
        settings.engine.profile = profile;
    }
    if (args.hasProperty ("auto_loudness"))
        settings.engine.autoLoudness = (bool) args["auto_loudness"];
    if (args.hasProperty ("loudness_lufs"))
    {
        settings.engine.loudnessLufs = juce::jlimit (ps::assist::kMinTargetLufs, ps::assist::kMaxTargetLufs, requireNumber (args, "loudness_lufs"));
        if (! args.hasProperty ("auto_loudness"))
            settings.engine.autoLoudness = false;
    }
    if (args.hasProperty ("intensity"))
    {
        const auto text = args["intensity"].toString();
        if (text.equalsIgnoreCase ("gentle"))        settings.engine.intensity = ps::assist::Intensity::Gentle;
        else if (text.equalsIgnoreCase ("balanced")) settings.engine.intensity = ps::assist::Intensity::Balanced;
        else if (text.equalsIgnoreCase ("strong"))   settings.engine.intensity = ps::assist::Intensity::Strong;
        else fail ("'intensity' is gentle, balanced or strong.");
    }
    if (args.hasProperty ("use_maximizer"))
        settings.engine.useMaximizer = (bool) args["use_maximizer"];
    if (args.hasProperty ("ceiling_db"))
        settings.engine.ceilingDb = juce::jlimit (-3.0f, 0.0f, requireNumber (args, "ceiling_db"));
    if (args.hasProperty ("true_peak"))
        settings.engine.truePeak = (bool) args["true_peak"];
    if (args.hasProperty ("analysis_seconds"))
        settings.analysisSeconds = juce::jlimit (Assistant::kMinSeconds, Assistant::kMaxSeconds, requireNumber (args, "analysis_seconds"));
    if (args.hasProperty ("whole_song"))
        settings.wholeSong = (bool) args["whole_song"];

    // A rebuild applies a new suggestion over the current settings, so it only happens when asked for.
    const bool rebuild = (bool) args["rebuild"] && a.hasAnalysis();
    a.setSettings (settings, rebuild);
    if (args.hasProperty ("loudness_matched"))
        a.setLoudnessMatched ((bool) args["loudness_matched"]);

    auto result = assistantToVar (a);
    result.getDynamicObject()->setProperty ("rebuilt", rebuild);
    return result;
}

//==============================================================================
const char* referenceStateName (TrackReferencing::State state)
{
    switch (state)
    {
        case TrackReferencing::State::Loading: return "loading";
        case TrackReferencing::State::Ready:   return "ready";
        case TrackReferencing::State::Missing: return "missing";
        case TrackReferencing::State::Error:
        default:                               return "error";
    }
}

juce::var referencesToVar (const TrackReferencing& refs)
{
    const auto info = refs.getInfo();
    juce::Array<juce::var> list;
    for (int i = 0; i < (int) info.entries.size(); ++i)
    {
        const auto& e = info.entries[(size_t) i];
        juce::Array<juce::var> loops;
        for (int l = 0; l < TrackReferencing::kNumLoops; ++l)
            if (e.loops[(size_t) l].set)
                loops.add (makeObject ({ { "letter", juce::String::charToString ((juce::juce_wchar) ('A' + l)) },
                                         { "start_seconds", numberD (e.loops[(size_t) l].start) },
                                         { "end_seconds", numberD (e.loops[(size_t) l].end) } }));
        list.add (makeObject ({ { "id", e.id }, { "name", e.name }, { "path", e.file.getFullPathName() },
                                { "state", juce::String (referenceStateName (e.state)) }, { "error", e.error },
                                { "progress", number (e.progress, 1000.0) }, { "selected", i == info.selected },
                                { "length_seconds", numberD (e.lengthSeconds) },
                                { "integrated_lufs", number (e.integratedLufs, 100.0) },
                                { "true_peak_dbtp", number (e.truePeakDb, 100.0) }, { "loops", loops },
                                { "active_loop", e.activeLoop >= 0 ? juce::var (juce::String::charToString ((juce::juce_wchar) ('A' + e.activeLoop))) : juce::var() } }));
    }
    return makeObject ({ { "references", list } });
}

int requireReference (const TrackReferencing& refs, Args args)
{
    const int id = requireInt (args, "id");
    for (const auto& e : refs.getInfo().entries)
        if (e.id == id)
            return id;
    fail ("No reference with id " + juce::String (id) + ". get_references lists them.");
}

juce::var cmdGetReferences (PureSheenProcessor& p, Args) { return referencesToVar (p.getTrackReferencing()); }

juce::var cmdAddReference (PureSheenProcessor& p, Args args)
{
    auto& refs = p.getTrackReferencing();
    const int id = refs.addReference (requireAudioFile (args), ! args.hasProperty ("select") || (bool) args["select"]);
    if (id == 0)
        fail ("The list is full (" + juce::String (TrackReferencing::kMaxReferences) + " references).");
    auto result = referencesToVar (refs);
    result.getDynamicObject()->setProperty ("added", id);
    return result;
}

juce::var cmdRemoveReference (PureSheenProcessor& p, Args args)
{
    auto& refs = p.getTrackReferencing();
    refs.removeReference (requireReference (refs, args));
    return referencesToVar (refs);
}

juce::var cmdSelectReference (PureSheenProcessor& p, Args args)
{
    auto& refs = p.getTrackReferencing();
    refs.select (requireReference (refs, args));
    return referencesToVar (refs);
}

juce::var cmdSetReferenceLoop (PureSheenProcessor& p, Args args)
{
    auto& refs = p.getTrackReferencing();
    const int id = requireReference (refs, args);

    if (args.hasProperty ("letter"))
    {
        const auto text = args["letter"].toString().toUpperCase();
        if (text.length() != 1 || text[0] < 'A' || text[0] >= 'A' + TrackReferencing::kNumLoops)
            fail ("'letter' is A to E.");
        const int letter = (int) (text[0] - 'A');
        if ((bool) args["clear"])
            refs.clearLoop (id, letter);
        else if (args.hasProperty ("start_seconds") || args.hasProperty ("end_seconds"))
            refs.setLoop (id, letter, requireNumber (args, "start_seconds"), requireNumber (args, "end_seconds"));
        else
            refs.setActiveLoop (id, letter);
    }
    else
    {
        refs.setActiveLoop (id, -1);   // the whole track
    }
    return referencesToVar (refs);
}

juce::var profileToVar (const ps::TrackProfile& profile, bool isReference)
{
    if (! profile.valid)   // its stereo fields hold placeholders until there is audio
        return makeObject ({ { "valid", false } });

    return makeObject ({ { "valid", profile.valid }, { "analysed_seconds", numberD (profile.analysedSeconds) },
                         { "integrated_lufs", number (profile.integratedLufs, 100.0) },
                         // Of a whole reference file this would be the file's last 3 seconds, so it says nothing.
                         { "short_term_lufs", isReference ? juce::var() : number (profile.shortTermLufs, 100.0) },
                         { "loudness_range_lu", number (profile.loudnessRangeLu, 100.0) },
                         { "true_peak_dbtp", number (profile.truePeakDb, 100.0) },
                         { "sample_peak_db", number (profile.samplePeakDb, 100.0) },
                         { "peak_to_loudness_db", number (profile.plrDb, 100.0) },
                         { "crest_db", number (profile.crestDb, 100.0) }, { "correlation", number (profile.correlation, 1000.0) },
                         { "width_pct", number (profile.widthPct, 100.0) },
                         { "low_end_correlation", number (profile.lowEndCorrelation, 1000.0) } });
}

juce::var cmdGetComparison (PureSheenProcessor& p, Args)
{
    const auto& refs = p.getTrackReferencing();
    const auto comparison = refs.getComparison();
    // Band dynamics come straight from the two profiles (the comparison's band levels are relative, these are not).
    const auto master = refs.getMixProfile();
    const auto reference = refs.getReferenceProfile();

    juce::Array<juce::var> bands;
    for (int i = 0; i < ps::kNumBands; ++i)
    {
        const auto& band = comparison.bands[(size_t) i];
        const auto& info = ps::bandInfo ((ps::Band) i);
        bands.add (makeObject ({ { "band", juce::String (info.name) }, { "low_hz", number (info.lowHz) }, { "high_hz", number (info.highHz) },
                                 { "valid", band.valid }, { "master_minus_reference_db", number (band.levelDiffDb, 100.0) },
                                 { "master_width_pct", number (band.mixWidthPct, 100.0) },
                                 { "reference_width_pct", number (band.refWidthPct, 100.0) },
                                 { "master_dynamic_range_db", number (master.bands[(size_t) i].dynamicRangeDb, 100.0) },
                                 { "reference_dynamic_range_db", number (reference.bands[(size_t) i].dynamicRangeDb, 100.0) },
                                 { "master_peak_over_median_db", number (master.bands[(size_t) i].peakOverMedianDb, 100.0) },
                                 { "reference_peak_over_median_db", number (reference.bands[(size_t) i].peakOverMedianDb, 100.0) } }));
    }

    juce::Array<juce::var> hints;
    for (const auto& hint : comparison.hints)
        hints.add (makeObject ({ { "title", juce::String (hint.title) }, { "detail", juce::String (hint.detail) } }));

    const auto freshness = p.getAnalysisFreshness();
    const double position = refs.getHostSeconds();
    return makeObject ({ { "ready", comparison.valid() }, { "master", profileToVar (refs.getMixProfile(), false) },
                         { "reference", profileToVar (refs.getReferenceProfile(), true) },
                         { "settings_changed_during_analysis", freshness.blendsSettings },
                         { "seconds_since_last_change", freshness.secondsSinceChange >= 0.0 ? numberD (freshness.secondsSinceChange) : juce::var() },
                         { "analysed_seconds_since_last_change", freshness.analysedSecondsSinceChange >= 0.0 ? numberD (freshness.analysedSecondsSinceChange) : juce::var() },
                         { "host_playing", refs.isHostPlaying() },
                         { "host_position_seconds", position >= 0.0 ? numberD (position) : juce::var() },
                         { "loudness_difference_lu", number (comparison.loudnessDiffLu, 100.0) },
                         { "true_peak_difference_db", number (comparison.truePeakDiffDb, 100.0) },
                         { "peak_to_loudness_difference_db", number (comparison.plrDiffDb, 100.0) },
                         { "loudness_range_difference_lu", number (comparison.lraDiffLu, 100.0) },
                         { "bands", bands }, { "hints", hints } });
}

juce::var cmdAssistantUseReference (PureSheenProcessor& p, Args args)
{
    auto& a = p.getAssistant();
    auto& refs = p.getTrackReferencing();
    const int id = requireInt (args, "id");
    if (id == 0)
    {
        a.clearReference();
        return assistantToVar (a);
    }

    requireReference (refs, args);
    for (const auto& e : refs.getInfo().entries)
        if (e.id == id)
        {
            if (e.state != TrackReferencing::State::Ready)
            {
                if (e.state != TrackReferencing::State::Loading)
                    fail ("That reference could not be read: " + e.error);
                a.awaitReference (e.file.getFullPathName(), e.name);
            }
            else
            {
                a.setReference (refs.getAssistantTarget (id));
            }
        }
    return assistantToVar (a);
}

//==============================================================================
struct Command
{
    std::function<juce::var (PureSheenProcessor&, Args)> run;
    bool changesState;
};

const std::map<juce::String, Command>& commands()
{
    static const std::map<juce::String, Command> table {
        { "get_status",               { cmdGetStatus, false } },
        { "get_meters",               { cmdGetMeters, false } },
        { "reset_loudness",           { cmdResetLoudness, false } },
        { "reset_comparison",         { cmdResetComparison, false } },
        { "list_modules",             { cmdListModules, false } },
        { "get_parameters",           { cmdGetParameters, false } },
        { "set_parameters",           { cmdSetParameters, true } },
        { "add_module",               { cmdAddModule, true } },
        { "remove_module",            { cmdRemoveModule, true } },
        { "set_chain",                { cmdSetChain, true } },
        { "reset_module",             { cmdResetModule, true } },
        { "get_history",              { cmdGetHistory, false } },
        { "undo",                     { cmdUndo, true } },
        { "redo",                     { cmdRedo, true } },
        { "store_snapshot",           { cmdStoreSnapshot, true } },
        { "recall_snapshot",          { cmdRecallSnapshot, true } },
        { "list_presets",             { cmdListPresets, false } },
        { "load_preset",              { cmdLoadPreset, true } },
        { "save_preset",              { cmdSavePreset, false } },
        { "learn",                    { cmdLearn, true } },
        { "get_assistant",            { cmdGetAssistant, false } },
        { "assistant_listen",         { cmdAssistantListen, true } },
        { "assistant_analyse_file",   { cmdAssistantAnalyseFile, true } },
        { "assistant_apply",          { cmdAssistantApply, true } },
        { "assistant_set_macro",      { cmdAssistantSetMacro, true } },
        { "assistant_settings",       { cmdAssistantSettings, true } },
        { "assistant_use_reference",  { cmdAssistantUseReference, true } },
        { "get_references",           { cmdGetReferences, false } },
        { "add_reference",            { cmdAddReference, true } },
        { "remove_reference",         { cmdRemoveReference, true } },
        { "select_reference",         { cmdSelectReference, true } },
        { "set_reference_loop",       { cmdSetReferenceLoop, true } },
        { "get_comparison",           { cmdGetComparison, false } },
    };
    return table;
}
} // namespace

juce::var runBridgeCommand (PureSheenProcessor& processor, const juce::String& command, const juce::var& args)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const auto found = commands().find (command);
    if (found == commands().end())
        fail ("Unknown command '" + command + "'.");
    if (! processor.getProduct().isSuite())
        fail ("Only PureSheen Dynamics takes commands.");
    processor.noteConnectorActivity();   // keeps the master's analysis running for get_meters' bands
    return found->second.run (processor, args);
}

bool isBridgeCommand (const juce::String& command) { return commands().count (command) > 0; }

bool bridgeCommandChangesState (const juce::String& command)
{
    const auto found = commands().find (command);
    return found != commands().end() && found->second.changesState;
}

juce::StringArray bridgeCommandNames()
{
    juce::StringArray names;
    for (const auto& [name, command] : commands())
        names.add (name);
    return names;
}
} // namespace psplug
