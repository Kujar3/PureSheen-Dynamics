#include "plugin/FactoryPresets.h"

// Factory presets. Values are musically conservative starting points: the Maximizer always keeps True Peak on with
// the ceiling at -1 dBTP (-0.5 for the loud club styles), low bands are narrowed rather than widened, and the
// compressor settings aim at 1-3 dB of gain reduction on typical mixes. The Streaming preset aims at roughly
// -14 LUFS integrated on a typical -20 LUFS premaster (and sets the Learn target to -14), the loud ones at -9..-7 LUFS.
namespace psplug
{
namespace
{
using ps::EqShape;
using ps::ModuleType;

constexpr float kMidSide = (float) ps::ChannelMode::MidSide;
constexpr float kSlope12 = (float) ps::EqSlope::Db12, kSlope24 = (float) ps::EqSlope::Db24;
constexpr float kAnalog = (float) ps::CrossoverType::Analog, kHybrid = (float) ps::CrossoverType::Hybrid;

float shape (EqShape s) { return (float) s; }
float mode (ps::MaximizerMode m) { return (float) m; }
float exc (ps::ExciterMode m) { return (float) m; }

struct Builder
{
    std::vector<std::pair<std::string, float>> values;

    Builder& set (const std::string& id, float v)
    {
        values.emplace_back (id, v);
        return *this;
    }

    // EQ band ("eq." placeholder prefix unless a real prefix is given).
    Builder& eq (char channelSet, int band, EqShape s, float freq, float gain = 0.0f, float q = 0.707f,
                 float slope = kSlope12, const std::string& prefix = "eq")
    {
        const auto p = prefix + "." + channelSet + ".b" + std::to_string (band) + ".";
        set (p + "on", 1.0f);
        set (p + "shape", shape (s));
        set (p + "freq", freq);
        set (p + "gain", gain);
        set (p + "q", q);
        set (p + "slope", slope);
        return *this;
    }

    Builder& eq1 (char channelSet, int band, EqShape s, float freq, float gain = 0.0f, float q = 0.707f,
                  float slope = kSlope12)
    {
        return eq (channelSet, band, s, freq, gain, q, slope, "eq1");
    }

    Builder& comp (int band, float thr, float ratio, float att, float rel, float knee = 6.0f, float mix = 100.0f)
    {
        const auto p = "dyn.a.b" + std::to_string (band) + ".";
        set (p + "comp.on", 1.0f);
        set (p + "comp.thr", thr);
        set (p + "comp.ratio", ratio);
        set (p + "comp.att", att);
        set (p + "comp.rel", rel);
        set (p + "comp.knee", knee);
        set (p + "mix", mix);
        return *this;
    }

    Builder& bands (const std::string& prefix, int n, float x1 = 120.0f, float x2 = 1000.0f, float x3 = 6000.0f)
    {
        set (prefix + ".bands", (float) n);
        set (prefix + ".xover1", x1);
        set (prefix + ".xover2", x2);
        set (prefix + ".xover3", x3);
        return *this;
    }

    Builder& width (int band, float w)
    {
        return set ("img.b" + std::to_string (band) + ".width", w);
    }

    Builder& excite (int band, ps::ExciterMode m, float drive, float mix)
    {
        const auto p = "exc.a.b" + std::to_string (band) + ".";
        set (p + "on", 1.0f);
        set (p + "mode", exc (m));
        set (p + "drive", drive);
        set (p + "mix", mix);
        return *this;
    }

    Builder& maximizer (ps::MaximizerMode m, float gain, float ceiling, float character)
    {
        set ("max.mode", mode (m));
        set ("max.gain", gain);
        set ("max.ceiling", ceiling);
        set ("max.truePeak", 1.0f);
        set ("max.character", character);
        return *this;
    }
};

std::vector<ModuleType> defaultOrder() { return { ps::kDefaultChain.begin(), ps::kDefaultChain.end() }; }

FactoryPreset global (const char* name, const char* category, Builder b, std::vector<ModuleType> order = defaultOrder())
{
    FactoryPreset p;
    p.name = name;
    p.category = category;
    p.scope = -1;
    p.order = std::move (order);
    p.values = std::move (b.values);
    return p;
}

FactoryPreset modulePreset (const char* name, const char* category, ModuleType scope, Builder b)
{
    FactoryPreset p;
    p.name = name;
    p.category = category;
    p.scope = (int) scope;
    p.values = std::move (b.values);
    return p;
}

std::vector<FactoryPreset> build()
{
    using MM = ps::MaximizerMode;
    using EM = ps::ExciterMode;
    std::vector<FactoryPreset> all;

    //==============================================================================
    // Global presets
    all.push_back (global ("Default", "Start Here", Builder()));

    all.push_back (global ("Soft Sheen", "Start Here",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 25.0f, 0.0f, 0.707f, kSlope12)
            .eq1 ('a', 2, EqShape::ProportionalQ, 280.0f, -1.0f, 0.9f)
            .eq1 ('a', 3, EqShape::HighShelfBaxandall, 10000.0f, 1.5f)
            .comp (1, -18.0f, 1.5f, 30.0f, 150.0f, 10.0f)
            .bands ("img", 2, 200.0f).width (1, -15.0f).width (2, 8.0f)
            .excite (1, EM::Warm, 10.0f, 50.0f)
            .maximizer (MM::Transparent, 3.0f, -1.0f, 4.0f)));

    all.push_back (global ("Streaming Ready", "Start Here",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 28.0f, 0.0f, 0.707f, kSlope12)
            .eq1 ('a', 2, EqShape::ProportionalQ, 3500.0f, 0.8f, 0.7f)
            .comp (1, -20.0f, 1.8f, 25.0f, 180.0f, 8.0f)
            .set ("dyn.adaptRel", 1.0f)
            .bands ("img", 2, 150.0f).width (1, -25.0f)
            .maximizer (MM::Transparent, 4.0f, -1.0f, 4.0f)
            .set ("max.learnTarget", -14.0f)));

    all.push_back (global ("Loud and Clear", "Start Here",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 30.0f, 0.0f, 0.707f, kSlope24)
            .eq1 ('a', 2, EqShape::ProportionalQ, 320.0f, -1.5f, 1.0f)
            .eq1 ('a', 3, EqShape::HighShelfBaxandall, 9000.0f, 2.0f)
            .bands ("dyn", 3, 150.0f, 2500.0f)
            .comp (1, -22.0f, 2.5f, 30.0f, 120.0f)
            .comp (2, -20.0f, 2.0f, 20.0f, 100.0f)
            .comp (3, -24.0f, 2.0f, 10.0f, 80.0f)
            .bands ("exc", 2, 4000.0f).excite (2, EM::Tape, 15.0f, 50.0f)
            .bands ("img", 2, 180.0f).width (1, -30.0f).width (2, 10.0f)
            .maximizer (MM::Transparent, 8.0f, -1.0f, 2.5f)
            .set ("max.upward", 1.5f)
            .set ("max.learnTarget", -9.0f)));

    all.push_back (global ("Club Loud", "EDM",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 28.0f, 0.0f, 0.707f, kSlope24)
            .eq1 ('a', 2, EqShape::LowShelfAnalog, 60.0f, 1.5f)
            .eq1 ('a', 3, EqShape::HighShelfBaxandall, 12000.0f, 2.0f)
            .bands ("dyn", 3, 120.0f, 2500.0f).set ("dyn.xoverType", kHybrid)
            .comp (1, -20.0f, 3.0f, 20.0f, 100.0f)
            .comp (2, -18.0f, 2.0f, 15.0f, 80.0f)
            .comp (3, -22.0f, 2.0f, 5.0f, 60.0f)
            .bands ("img", 2, 150.0f).width (1, -100.0f).width (2, 15.0f)
            .bands ("exc", 2, 5000.0f).excite (2, EM::Analog, 15.0f, 40.0f)
            .maximizer (MM::Aggressive, 10.0f, -0.5f, 1.5f)
            .set ("max.style3", (float) ps::AggressiveStyle::Balanced)
            .set ("max.clip.on", 1.0f)
            .set ("max.clip.mode", (float) ps::SoftClipMode::Moderate)
            .set ("max.clip.amount", 50.0f)
            .set ("max.learnTarget", -7.0f)));

    all.push_back (global ("Wide Supersaw Master", "EDM",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 25.0f, 0.0f, 0.707f, kSlope24)
            .eq1 ('a', 2, EqShape::ProportionalQ, 450.0f, -1.0f, 0.8f)
            .eq1 ('a', 3, EqShape::HighShelfBaxandall, 11000.0f, 1.5f)
            .comp (1, -20.0f, 2.0f, 30.0f, 120.0f)
            .bands ("img", 3, 200.0f, 2500.0f).width (1, -50.0f).width (2, 15.0f).width (3, 35.0f)
            .set ("img.stz.on", 1.0f).set ("img.stz.mode", (float) ps::StereoizeMode::ModeII).set ("img.stz.amount", 20.0f)
            .set ("img.antiphase", 1.0f)
            .bands ("exc", 2, 6000.0f).excite (2, EM::Analog, 20.0f, 60.0f)
            .maximizer (MM::Transparent, 7.0f, -0.5f, 3.0f)
            .set ("max.si.trans", 20.0f)
            .set ("max.learnTarget", -8.0f)));

    all.push_back (global ("Deep Bass Anchor", "EDM",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 22.0f, 0.0f, 0.707f, kSlope24)
            .eq1 ('a', 2, EqShape::LowShelfVintage, 50.0f, 2.0f)
            .bands ("dyn", 2, 140.0f)
            .comp (1, -22.0f, 3.0f, 25.0f, 120.0f)
            .comp (2, -20.0f, 1.5f, 15.0f, 100.0f, 10.0f)
            .bands ("img", 2, 140.0f).width (1, -100.0f)
            .maximizer (MM::Smooth, 6.0f, -1.0f, 5.0f)));

    all.push_back (global ("Acoustic Air", "Gentle",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 30.0f, 0.0f, 0.707f, kSlope12)
            .eq1 ('a', 2, EqShape::ProportionalQ, 3500.0f, 0.8f, 0.7f)
            .eq1 ('a', 3, EqShape::HighShelfBaxandall, 12000.0f, 2.0f)
            .comp (1, -24.0f, 1.3f, 50.0f, 300.0f, 15.0f)
            .bands ("exc", 2, 6000.0f).excite (2, EM::Tube, 12.0f, 40.0f)
            .width (1, 5.0f)
            .maximizer (MM::Transparent, 2.0f, -1.0f, 6.0f)));

    all.push_back (global ("Warm Glue", "Gentle",
        Builder()
            .eq1 ('a', 1, EqShape::LowShelfAnalog, 100.0f, 1.0f, 0.7f)
            .eq1 ('a', 2, EqShape::HighShelfAnalog, 12000.0f, -0.5f, 0.7f)
            .comp (1, -20.0f, 2.0f, 30.0f, 200.0f, 12.0f)
            .set ("dyn.adaptRel", 1.0f)
            .excite (1, EM::Tape, 15.0f, 50.0f)
            .maximizer (MM::Smooth, 3.0f, -1.0f, 5.0f)));

    all.push_back (global ("Limit Only", "Gentle",
        Builder().maximizer (MM::Transparent, 3.0f, -1.0f, 4.0f),
        { ModuleType::Maximizer }));

    all.push_back (global ("Radio Pop", "Pop and Rock",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 30.0f, 0.0f, 0.707f, kSlope24)
            .eq1 ('a', 2, EqShape::ProportionalQ, 220.0f, -1.0f, 0.9f)
            .eq1 ('a', 3, EqShape::ProportionalQ, 4000.0f, 1.0f, 0.8f)
            .eq1 ('a', 4, EqShape::HighShelfBaxandall, 12000.0f, 1.5f)
            .bands ("dyn", 3, 150.0f, 3000.0f)
            .comp (1, -20.0f, 2.5f, 30.0f, 120.0f)
            .comp (2, -20.0f, 2.0f, 15.0f, 100.0f)
            .comp (3, -24.0f, 2.5f, 5.0f, 60.0f)
            .bands ("img", 2, 200.0f).width (1, -20.0f).width (2, 10.0f)
            .maximizer (MM::Aggressive, 7.0f, -1.0f, 3.0f)
            .set ("max.style3", (float) ps::AggressiveStyle::Crisp)
            .set ("max.upward", 2.0f)
            .set ("max.learnTarget", -9.0f)));

    all.push_back (global ("Rock Drive", "Pop and Rock",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 35.0f, 0.0f, 0.707f, kSlope12)
            .eq1 ('a', 2, EqShape::ProportionalQ, 400.0f, -1.5f, 1.0f)
            .eq1 ('a', 3, EqShape::ProportionalQ, 2500.0f, 1.0f, 0.8f)
            .comp (1, -18.0f, 2.0f, 20.0f, 120.0f)
            .bands ("exc", 3, 200.0f, 3000.0f).excite (2, EM::Triode, 20.0f, 60.0f).excite (3, EM::Tape, 10.0f, 50.0f)
            .maximizer (MM::Aggressive, 8.0f, -1.0f, 2.0f)
            .set ("max.style3", (float) ps::AggressiveStyle::Clipping)));

    all.push_back (global ("Hip-Hop Knock", "Hip-Hop",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 25.0f, 0.0f, 0.707f, kSlope24)
            .eq1 ('a', 2, EqShape::LowShelfBaxandall, 80.0f, 1.5f)
            .eq1 ('a', 3, EqShape::ProportionalQ, 300.0f, -1.5f, 1.0f)
            .bands ("dyn", 2, 120.0f)
            .comp (1, -18.0f, 2.5f, 40.0f, 150.0f)
            .comp (2, -20.0f, 2.0f, 20.0f, 120.0f)
            .bands ("img", 2, 120.0f).width (1, -100.0f).width (2, 10.0f)
            .maximizer (MM::Transparent, 7.0f, -1.0f, 3.0f)
            .set ("max.trans.on", 1.0f)
            .set ("max.trans.amount", 30.0f)));

    all.push_back (global ("Mid-Side Clarity", "Mastering",
        Builder()
            .set ("eq1.chMode", kMidSide)
            .set ("eq1.link", 0.0f)
            .eq1 ('a', 1, EqShape::HighPassFlat, 25.0f, 0.0f, 0.707f, kSlope12)
            .eq1 ('a', 2, EqShape::ProportionalQ, 300.0f, -1.5f, 1.0f)
            .eq1 ('a', 3, EqShape::ProportionalQ, 2500.0f, 0.8f, 0.8f)
            .eq1 ('b', 1, EqShape::HighPassFlat, 120.0f, 0.0f, 0.707f, kSlope12)
            .eq1 ('b', 2, EqShape::HighShelfBaxandall, 9000.0f, 2.0f)
            .comp (1, -20.0f, 1.5f, 30.0f, 150.0f, 10.0f)
            .bands ("img", 2, 150.0f).width (1, -40.0f).width (2, 10.0f)
            .maximizer (MM::Transparent, 4.0f, -1.0f, 4.0f)));

    //==============================================================================
    // Equalizer presets (shared by EQ 1 and EQ 2)
    all.push_back (modulePreset ("Low Cut 30 Hz", "Cleanup", ModuleType::Equalizer1,
        Builder().eq ('a', 1, EqShape::HighPassFlat, 30.0f, 0.0f, 0.707f, kSlope24)));
    all.push_back (modulePreset ("Mud Cleanup", "Cleanup", ModuleType::Equalizer1,
        Builder()
            .eq ('a', 1, EqShape::HighPassFlat, 25.0f, 0.0f, 0.707f, kSlope12)
            .eq ('a', 2, EqShape::ProportionalQ, 300.0f, -2.0f, 1.2f)));
    all.push_back (modulePreset ("Air Lift", "Tone", ModuleType::Equalizer1,
        Builder().eq ('a', 1, EqShape::HighShelfBaxandall, 12000.0f, 2.5f)));
    all.push_back (modulePreset ("Mid-Side Width", "Mid-Side", ModuleType::Equalizer1,
        Builder()
            .set ("eq.chMode", kMidSide)
            .set ("eq.link", 0.0f)
            .eq ('a', 1, EqShape::HighPassFlat, 25.0f, 0.0f, 0.707f, kSlope12)
            .eq ('b', 1, EqShape::HighPassFlat, 150.0f, 0.0f, 0.707f, kSlope12)
            .eq ('b', 2, EqShape::HighShelfBaxandall, 8000.0f, 2.0f)));

    //==============================================================================
    // Dynamics presets
    all.push_back (modulePreset ("Gentle Glue", "Glue", ModuleType::Dynamics,
        Builder().comp (1, -20.0f, 1.5f, 30.0f, 200.0f, 12.0f)));
    all.push_back (modulePreset ("Multiband Control", "Multiband", ModuleType::Dynamics,
        Builder()
            .bands ("dyn", 3, 150.0f, 2500.0f)
            .comp (1, -22.0f, 2.5f, 30.0f, 120.0f)
            .comp (2, -20.0f, 2.0f, 20.0f, 100.0f)
            .comp (3, -24.0f, 2.0f, 10.0f, 80.0f)));
    all.push_back (modulePreset ("Tame Low End", "Multiband", ModuleType::Dynamics,
        Builder()
            .bands ("dyn", 2, 150.0f)
            .set ("dyn.xoverType", kAnalog)
            .comp (1, -24.0f, 3.0f, 25.0f, 120.0f)));
    all.push_back (modulePreset ("Upward Detail", "Detail", ModuleType::Dynamics,
        Builder().comp (1, -40.0f, -2.0f, 10.0f, 200.0f, 10.0f, 50.0f)));

    //==============================================================================
    // Imager presets
    all.push_back (modulePreset ("Mono Bass", "Width", ModuleType::Imager,
        Builder().bands ("img", 2, 120.0f).width (1, -100.0f)));
    all.push_back (modulePreset ("Wide Top", "Width", ModuleType::Imager,
        Builder().bands ("img", 3, 300.0f, 4000.0f).width (2, 10.0f).width (3, 30.0f)));
    all.push_back (modulePreset ("Stereoize Lift", "Width", ModuleType::Imager,
        Builder()
            .width (1, 15.0f)
            .set ("img.stz.on", 1.0f).set ("img.stz.mode", (float) ps::StereoizeMode::ModeII).set ("img.stz.amount", 25.0f)
            .set ("img.antiphase", 1.0f)));
    all.push_back (modulePreset ("Safe Narrow", "Correction", ModuleType::Imager,
        Builder().width (1, -20.0f).set ("img.rec.on", 1.0f).set ("img.rec.gain", 0.0f)));

    //==============================================================================
    // Exciter presets
    all.push_back (modulePreset ("Warm Tape", "Saturation", ModuleType::Exciter,
        Builder().excite (1, EM::Tape, 20.0f, 60.0f)));
    all.push_back (modulePreset ("Analog Glow", "Saturation", ModuleType::Exciter,
        Builder().excite (1, EM::Analog, 15.0f, 70.0f)));
    all.push_back (modulePreset ("Tube Sparkle", "Highs", ModuleType::Exciter,
        Builder()
            .bands ("exc", 2, 5000.0f)
            .excite (2, EM::Tube, 25.0f, 50.0f)
            .set ("exc.post.on", 1.0f).set ("exc.post.freq", 10000.0f).set ("exc.post.gain", -1.5f)));

    //==============================================================================
    // Maximizer presets
    all.push_back (modulePreset ("Transparent Safe", "Safe", ModuleType::Maximizer,
        Builder().maximizer (MM::Transparent, 3.0f, -1.0f, 4.0f)));
    all.push_back (modulePreset ("Low Latency Safety", "Safe", ModuleType::Maximizer,
        Builder().maximizer (MM::LowLatency, 0.0f, -0.3f, 3.0f)));
    all.push_back (modulePreset ("Smooth Bass Heavy", "Warm", ModuleType::Maximizer,
        Builder().maximizer (MM::Smooth, 5.0f, -1.0f, 6.0f)));
    all.push_back (modulePreset ("Loud Modern", "Loud", ModuleType::Maximizer,
        Builder()
            .maximizer (MM::Aggressive, 9.0f, -0.5f, 2.0f)
            .set ("max.style3", (float) ps::AggressiveStyle::Balanced)
            .set ("max.clip.on", 1.0f)
            .set ("max.clip.mode", (float) ps::SoftClipMode::Moderate)
            .set ("max.clip.amount", 50.0f)));

    return all;
}

bool isEqScope (int scope) noexcept
{
    return scope == (int) ModuleType::Equalizer1 || scope == (int) ModuleType::Equalizer2;
}
} // namespace

const std::vector<FactoryPreset>& factoryPresets()
{
    static const std::vector<FactoryPreset> all = build();
    return all;
}

std::vector<const FactoryPreset*> factoryPresetsFor (int scope)
{
    std::vector<const FactoryPreset*> result;
    for (const auto& p : factoryPresets())
        if (p.scope == scope || (isEqScope (scope) && p.scope == (int) ModuleType::Equalizer1))
            result.push_back (&p);
    return result;
}

PresetData toPresetData (const FactoryPreset& preset, int scope)
{
    PresetData d;
    d.scope = preset.scope < 0 ? -1 : scope;
    d.name = preset.name;
    d.category = preset.category;
    d.values = defaultValues();
    d.hasChain = d.scope < 0;
    d.order = preset.order;

    const std::string eqPrefix = isEqScope (scope) ? moduleIdPrefix ((ModuleType) scope).toStdString() : std::string();

    for (const auto& [rawId, value] : preset.values)
    {
        auto id = rawId;
        if (! eqPrefix.empty() && id.rfind ("eq.", 0) == 0)
            id = eqPrefix + id.substr (2);

        const int i = ps::paramIndex (id);
        jassert (i >= 0);   // factory preset refers to an unknown parameter
        if (i >= 0 && isInPresetScope (i, d.scope))
            d.values[(size_t) i] = sanitiseValue (i, value);
    }
    return d;
}
} // namespace psplug
