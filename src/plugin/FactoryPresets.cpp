#include "plugin/FactoryPresets.h"

// Factory presets. Values are musically conservative starting points: the Maximizer always keeps True Peak on with
// the ceiling at -1 dBTP (-0.5 for the loud club styles), low bands are narrowed rather than widened, EQ moves stay
// within +-2 dB and the compressor settings aim at 1-3 dB of gain reduction on typical mixes.
// The Maximizer gains are tuned on a -20 LUFS premaster (peaks at -4 dBFS, LRA 5 LU; the integration test mix):
// "Streaming Ready" lands at about -14 LUFS, "Loud and Clear" about -9.5, "Club Loud" about -8 (-0.5 dBTP), the
// Pop / Rock / Hip-Hop / EDM styles at -11..-15, and the Gentle presets stay within 1-2 LU of the input. Every preset that
// pushes loudness sets the Learn target to its intended level, so Learn Input Gain adapts it to other mixes.
//
// Phase 2:
// - Module presets of the eight phase 2 modules (proposed by their DSP owners, checked against ParamTable.cpp). In
//   Stereo presets set B gets the same values as set A, so a later switch to M/S or T/S starts linked; presets whose
//   sets differ turn the channel Link off.
// - The Match EQ presets "Pink Balance", "Warm Tilt" and "Bright Tilt" carry a synthesised reference: the preset
//   format holds no spectrum, MatchEqState::beginPresetLoad attaches it by name (factoryMatchReference), so these
//   names must stay exactly as they are. "Low End Only" carries none and leaves the References alone.
// - Transient/Sustain presets of the Core modules ("Transient/Sustain" category): set A = Transient, set B = Sustain.
//   The channel-mode values come from ps::channelModeParamValues (the EQ's chMode index, the Exciter's exc.ts.on).
// - Global presets with phase 2 modules in their chains, tuned like the Core ones on the -20 LUFS premaster:
//   "Modern Loud Balance" about -10 LUFS, "Clean Vocal Pop" -11, "Heavy Low End Master" -12 (all -1 dBTP),
//   "Gentle Acoustic Master" within 1.5 LU of the input.
// - Module preset names are unique across all scopes (the DSP owners' "Soft Sheen" / "Air Lift" proposals became
//   "Gentle Balance" / "Airy Detail", since a global and an EQ preset already carry those names).
//
// Phase 3 (vintage-style modules): 4-5 presets per module, built on the module defaults (which are the reference's,
// not neutral: e.g. the Classic Compressor's auto gain stays on). Stereo presets set both channel sets as above; the
// Mid-Side ones turn Link off. Every preset sets each control it relies on, so the confirmed defaults changed
// nothing they use; the Classic Compressor releases moved into the confirmed 30..150 ms range in the same order, and
// the Passive EQ high-boost widths became High Q values (0 = broad). Amounts stay moderate.
namespace psplug
{
namespace
{
using ps::EqShape;
using ps::ModuleType;
using ps::ChannelMode;
using ps::DynEqShape;

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

    // (phase 2) The values that select a channel mode (from the parameter table). `placeholder` replaces the module
    // prefix (the shared EQ presets are written with "eq.").
    Builder& channelMode (ModuleType type, ChannelMode m, const std::string& placeholder = {})
    {
        const auto modeValues = ps::channelModeParamValues (type, m);
        jassert (! modeValues.empty());   // the module does not offer this mode
        for (const auto& [id, v] : modeValues)
            set (placeholder.empty() ? id : placeholder + id.substr (id.find ('.')), v);
        return *this;
    }

    // "<prefix>.a.<name>" and "<prefix>.b.<name>" (both channel sets).
    Builder& both (const std::string& prefix, const std::string& name, float v)
    {
        set (prefix + ".a." + name, v);
        return set (prefix + ".b." + name, v);
    }

    // Dynamic EQ band on the channel sets listed in `sets` ("a", "b" or "ab"); knee 6 dB.
    Builder& dynEq (const std::string& sets, int band, DynEqShape s, float freq, float gain, float q, float thr,
                    float ratio, float att, float rel, bool autoScale = true,
                    ps::DynEqDirection dir = ps::DynEqDirection::Down)
    {
        for (const char c : sets)
        {
            const auto p = std::string ("deq.") + c + ".b" + std::to_string (band) + ".";
            set (p + "on", 1.0f);
            set (p + "shape", (float) s);
            set (p + "freq", freq);
            set (p + "gain", gain);
            set (p + "q", q);
            set (p + "dir", (float) dir);
            set (p + "thr", thr);
            set (p + "ratio", ratio);
            set (p + "knee", 6.0f);
            set (p + "auto", autoScale ? 1.0f : 0.0f);
            set (p + "att", att);
            set (p + "rel", rel);
        }
        return *this;
    }

    // Tone Balance on both sets: target, mode and the set controls.
    Builder& tone (ps::ToneTarget target, ps::ToneBalanceMode m, float amount, float speed, float smooth,
                   float cutSpeed = 50.0f, float sens = 50.0f, bool tame = false, float low = 100.0f,
                   float mid = 100.0f, float high = 100.0f)
    {
        set ("tbal.target", (float) target);
        set ("tbal.mode", (float) m);
        both ("tbal", "amount", amount);
        both ("tbal", "speed", speed);
        both ("tbal", "smooth", smooth);
        both ("tbal", "cutSpeed", cutSpeed);
        both ("tbal", "sens", sens);
        both ("tbal", "tame", tame ? 1.0f : 0.0f);
        both ("tbal", "low", low);
        both ("tbal", "mid", mid);
        return both ("tbal", "high", high);
    }

    // Definition set ("a" / "b" / "ab").
    Builder& definition (const std::string& sets, float amount, float tilt, float att, float rel, float low, float high)
    {
        for (const char c : sets)
        {
            const auto p = std::string ("defn.") + c + ".";
            set (p + "amount", amount);
            set (p + "tilt", tilt);
            set (p + "att", att);
            set (p + "rel", rel);
            set (p + "low", low);
            set (p + "high", high);
        }
        return *this;
    }

    // Smoother set ("a" / "b" / "ab").
    Builder& smoother (const std::string& sets, float amount, ps::SmootherMode m, float att, float rel, float tone,
                       float low, float high)
    {
        for (const char c : sets)
        {
            const auto p = std::string ("smth.") + c + ".";
            set (p + "amount", amount);
            set (p + "mode", (float) m);
            set (p + "att", att);
            set (p + "rel", rel);
            set (p + "tone", tone);
            set (p + "low", low);
            set (p + "high", high);
        }
        return *this;
    }

    // Punch band amount on both sets.
    Builder& punch (int band, float amount)
    {
        return both ("pnch", "b" + std::to_string (band) + ".amount", amount);
    }

    // Low Contrast set ("a" / "b" / "ab").
    Builder& lowContrast (const std::string& sets, ps::LowContrastMode m, float contrast, float gain)
    {
        for (const char c : sets)
        {
            const auto p = std::string ("lowc.") + c + ".";
            set (p + "mode", (float) m);
            set (p + "contrast", contrast);
            set (p + "gain", gain);
        }
        return *this;
    }

    // Match EQ set controls on both sets.
    Builder& match (float amount, float smooth, float boost, float cut)
    {
        both ("meq", "amount", amount);
        both ("meq", "smooth", smooth);
        both ("meq", "boost", boost);
        return both ("meq", "cut", cut);
    }

    // (phase 3) Classic Compressor core controls on the channel sets listed in `sets` ("a", "b" or "ab"); makeup 0,
    // auto gain on (the module defaults).
    Builder& classicComp (const std::string& sets, float thr, float ratio, float attackMs, float releaseMs,
                          ps::ClassicCompMode m)
    {
        for (char s : sets)
        {
            const std::string p = std::string ("ccmp.") + s + ".";
            set (p + "thr", thr);
            set (p + "ratio", ratio);
            set (p + "attack", attackMs);
            set (p + "release", releaseMs);
            set (p + "mode", (float) m);
        }
        return *this;
    }

    // De-limiter controls (Learn usually sets the threshold afterwards).
    Builder& deLimiter (float amountPercent, float thresholdDb)
    {
        set ("dlim.amount", amountPercent);
        return set ("dlim.thr", thresholdDb);
    }

    // (phase 3) Tape controls.
    Builder& tape (ps::TapeSpeed speed, float driveDb, float bias, float harmonics, float lowEmph, float highEmph)
    {
        set ("tape.speed", (float) speed);
        set ("tape.drive", driveDb);
        set ("tape.bias", bias);
        set ("tape.harmonics", harmonics);
        set ("tape.lowEmph", lowEmph);
        return set ("tape.highEmph", highEmph);
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
            .maximizer (MM::Transparent, 9.5f, -1.0f, 4.0f)
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
            .maximizer (MM::Transparent, 15.0f, -1.0f, 2.5f)
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
            .maximizer (MM::Aggressive, 15.0f, -0.5f, 1.5f)
            .set ("max.style3", (float) ps::AggressiveStyle::Balanced)
            .set ("max.clip.on", 1.0f)
            .set ("max.clip.mode", (float) ps::SoftClipMode::Moderate)
            .set ("max.clip.amount", 50.0f)
            .set ("max.learnTarget", -7.5f)));

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
            .maximizer (MM::Transparent, 13.0f, -0.5f, 3.0f)
            .set ("max.si.trans", 20.0f)
            .set ("max.learnTarget", -11.5f)));

    all.push_back (global ("Deep Bass Anchor", "EDM",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 22.0f, 0.0f, 0.707f, kSlope24)
            .eq1 ('a', 2, EqShape::LowShelfVintage, 50.0f, 2.0f)
            .bands ("dyn", 2, 140.0f)
            .comp (1, -22.0f, 3.0f, 25.0f, 120.0f)
            .comp (2, -20.0f, 1.5f, 15.0f, 100.0f, 10.0f)
            .bands ("img", 2, 140.0f).width (1, -100.0f)
            .maximizer (MM::Smooth, 8.0f, -1.0f, 5.0f)
            .set ("max.learnTarget", -14.5f)));

    all.push_back (global ("Acoustic Air", "Gentle",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 30.0f, 0.0f, 0.707f, kSlope12)
            .eq1 ('a', 2, EqShape::ProportionalQ, 3500.0f, 0.8f, 0.7f)
            .eq1 ('a', 3, EqShape::HighShelfBaxandall, 12000.0f, 2.0f)
            .comp (1, -24.0f, 1.3f, 50.0f, 300.0f, 15.0f)
            .bands ("exc", 2, 6000.0f).excite (2, EM::Tube, 12.0f, 40.0f)
            .width (1, 5.0f)
            .maximizer (MM::Transparent, 3.0f, -1.0f, 6.0f)));

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
            .maximizer (MM::Aggressive, 12.0f, -1.0f, 3.0f)
            .set ("max.style3", (float) ps::AggressiveStyle::Crisp)
            .set ("max.upward", 2.0f)
            .set ("max.learnTarget", -11.0f)));

    all.push_back (global ("Rock Drive", "Pop and Rock",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 35.0f, 0.0f, 0.707f, kSlope12)
            .eq1 ('a', 2, EqShape::ProportionalQ, 400.0f, -1.5f, 1.0f)
            .eq1 ('a', 3, EqShape::ProportionalQ, 2500.0f, 1.0f, 0.8f)
            .comp (1, -18.0f, 2.0f, 20.0f, 120.0f)
            .bands ("exc", 3, 200.0f, 3000.0f).excite (2, EM::Triode, 20.0f, 60.0f).excite (3, EM::Tape, 10.0f, 50.0f)
            .maximizer (MM::Aggressive, 11.0f, -1.0f, 2.0f)
            .set ("max.style3", (float) ps::AggressiveStyle::Clipping)
            .set ("max.learnTarget", -12.0f)));

    all.push_back (global ("Hip-Hop Knock", "Hip-Hop",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 25.0f, 0.0f, 0.707f, kSlope24)
            .eq1 ('a', 2, EqShape::LowShelfBaxandall, 80.0f, 1.5f)
            .eq1 ('a', 3, EqShape::ProportionalQ, 300.0f, -1.5f, 1.0f)
            .bands ("dyn", 2, 120.0f)
            .comp (1, -18.0f, 2.5f, 40.0f, 150.0f)
            .comp (2, -20.0f, 2.0f, 20.0f, 120.0f)
            .bands ("img", 2, 120.0f).width (1, -100.0f).width (2, 10.0f)
            .maximizer (MM::Transparent, 10.0f, -1.0f, 3.0f)
            .set ("max.trans.on", 1.0f)
            .set ("max.trans.amount", 30.0f)
            .set ("max.learnTarget", -11.0f)));

    all.push_back (global ("Mid-Side Detail", "Mastering",
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
            .maximizer (MM::Transparent, 7.0f, -1.0f, 4.0f)
            .set ("max.learnTarget", -15.5f)));

    // (phase 2) Global presets with the phase 2 modules.
    using TT = ps::ToneTarget;
    using TM = ps::ToneBalanceMode;
    using SM = ps::SmootherMode;
    using LM = ps::LowContrastMode;

    all.push_back (global ("Modern Loud Balance", "Start Here",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 28.0f, 0.0f, 0.707f, kSlope24)
            .tone (TT::Pop, TM::Shape, 30.0f, 40.0f, 70.0f)
            .comp (1, -20.0f, 1.8f, 30.0f, 150.0f, 10.0f)
            .bands ("pnch", 2, 200.0f).punch (1, 15.0f).punch (2, 20.0f)
            .set ("pnch.env", 60.0f).set ("pnch.stLink", 1.0f).set ("pnch.autoGain", 1.0f)
            .bands ("img", 2, 150.0f).width (1, -30.0f).width (2, 8.0f)
            .maximizer (MM::Transparent, 13.5f, -1.0f, 3.0f)
            .set ("max.learnTarget", -10.0f),
        { ModuleType::Equalizer1, ModuleType::ToneBalance, ModuleType::Dynamics, ModuleType::Punch, ModuleType::Imager,
          ModuleType::Maximizer }));

    all.push_back (global ("Gentle Acoustic Master", "Gentle",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 30.0f, 0.0f, 0.707f, kSlope12)
            .eq1 ('a', 2, EqShape::HighShelfBaxandall, 12000.0f, 1.0f)
            .definition ("ab", 25.0f, 0.0f, 150.0f, 150.0f, 300.0f, 20000.0f)
            .smoother ("ab", 25.0f, SM::Gentle, 10.0f, 50.0f, 0.0f, 2500.0f, 10000.0f)
            .comp (1, -24.0f, 1.3f, 50.0f, 300.0f, 15.0f)
            .maximizer (MM::Transparent, 4.0f, -1.0f, 6.0f),
        { ModuleType::Equalizer1, ModuleType::Definition, ModuleType::Smoother, ModuleType::Dynamics,
          ModuleType::Maximizer }));

    all.push_back (global ("Heavy Low End Master", "Hip-Hop",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 22.0f, 0.0f, 0.707f, kSlope24)
            .set ("lowc.low", 30.0f).set ("lowc.high", 120.0f)
            .lowContrast ("ab", LM::Punchy, 30.0f, 0.0f)
            .set ("bass.punch", 2.0f)
            .set ("bass.peak.on", 1.0f).set ("bass.peak.thr", -8.0f).set ("bass.peak.char", 100.0f)
            .comp (1, -20.0f, 1.8f, 30.0f, 150.0f, 10.0f)
            .bands ("img", 2, 140.0f).width (1, -100.0f).width (2, 5.0f)
            .maximizer (MM::Smooth, 10.5f, -1.0f, 5.0f)
            .set ("max.learnTarget", -12.0f),
        { ModuleType::Equalizer1, ModuleType::LowContrast, ModuleType::BassShaper, ModuleType::Dynamics,
          ModuleType::Imager, ModuleType::Maximizer }));

    all.push_back (global ("Clean Vocal Pop", "Pop and Rock",
        Builder()
            .eq1 ('a', 1, EqShape::HighPassFlat, 30.0f, 0.0f, 0.707f, kSlope24)
            .dynEq ("ab", 3, DynEqShape::Bell, 280.0f, -2.5f, 1.2f, -32.0f, 2.0f, 8.0f, 120.0f)
            .dynEq ("ab", 5, DynEqShape::ProportionalQ, 6500.0f, -4.0f, 2.5f, -38.0f, 3.0f, 1.0f, 45.0f, false)
            .tone (TT::Pop, TM::Shape, 25.0f, 40.0f, 70.0f)
            .comp (1, -20.0f, 2.0f, 20.0f, 120.0f, 8.0f)
            .bands ("exc", 2, 6000.0f).excite (2, EM::Tube, 10.0f, 40.0f)
            .bands ("img", 2, 180.0f).width (1, -25.0f).width (2, 8.0f)
            .maximizer (MM::Transparent, 12.5f, -1.0f, 3.0f)
            .set ("max.learnTarget", -11.0f),
        { ModuleType::Equalizer1, ModuleType::DynamicEq, ModuleType::ToneBalance, ModuleType::Dynamics,
          ModuleType::Exciter, ModuleType::Imager, ModuleType::Maximizer }));

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
    // Transient/Sustain (set A = Transient, set B = Sustain)
    all.push_back (modulePreset ("Air on the Hits", "Transient/Sustain", ModuleType::Equalizer1,
        Builder()
            .channelMode (ModuleType::Equalizer1, ChannelMode::TransientSustain, "eq")
            .set ("eq.link", 0.0f)
            .set ("eq.ts.sens", 55.0f).set ("eq.ts.len", 15.0f)
            .eq ('a', 1, EqShape::HighShelfBaxandall, 9000.0f, 2.0f)         // Transient: air on the attacks
            .eq ('a', 2, EqShape::ProportionalQ, 4000.0f, 0.8f, 0.8f)
            .eq ('b', 1, EqShape::HighShelfBaxandall, 9000.0f, -0.5f)));      // Sustain: tails a touch softer
    all.push_back (modulePreset ("Body for the Tails", "Transient/Sustain", ModuleType::Equalizer1,
        Builder()
            .channelMode (ModuleType::Equalizer1, ChannelMode::TransientSustain, "eq")
            .set ("eq.link", 0.0f)
            .set ("eq.ts.sens", 50.0f).set ("eq.ts.len", 25.0f)
            .eq ('b', 1, EqShape::LowShelfBaxandall, 90.0f, 0.8f)
            .eq ('b', 2, EqShape::ProportionalQ, 220.0f, 1.2f, 0.8f)));

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
    all.push_back (modulePreset ("Wide Tails, Focused Hits", "Transient/Sustain", ModuleType::Imager,
        Builder()
            .channelMode (ModuleType::Imager, ChannelMode::TransientSustain)
            .set ("img.link", 0.0f)
            .set ("img.ts.sens", 50.0f).set ("img.ts.len", 20.0f)
            .bands ("img", 2, 250.0f)
            .width (2, -20.0f)                                   // Transient: the hits a little more centred
            .set ("img.b.b2.width", 25.0f)                      // Sustain: wider tails
            .set ("img.b.stz.on", 1.0f).set ("img.b.stz.mode", (float) ps::StereoizeMode::ModeII)
            .set ("img.b.stz.amount", 15.0f)
            .set ("img.antiphase", 1.0f)));

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
    all.push_back (modulePreset ("Sparkle the Transients", "Transient/Sustain", ModuleType::Exciter,
        Builder()
            .channelMode (ModuleType::Exciter, ChannelMode::TransientSustain)
            .set ("exc.link", 0.0f)
            .set ("exc.ts.sens", 60.0f).set ("exc.ts.len", 12.0f)
            .bands ("exc", 2, 5000.0f)
            .excite (2, EM::Tube, 25.0f, 60.0f)));               // Transient top band only; Sustain stays clean

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

    //==============================================================================
    // (phase 2) Dynamic EQ presets (Stereo; both sets carry the band so M/S or T/S starts linked)
    all.push_back (modulePreset ("Soft De-ess", "Control", ModuleType::DynamicEq,
        Builder().dynEq ("ab", 5, DynEqShape::ProportionalQ, 6500.0f, -5.0f, 2.5f, -38.0f, 3.0f, 1.0f, 45.0f, false)));
    all.push_back (modulePreset ("Low-Mid Bloom Control", "Control", ModuleType::DynamicEq,
        Builder().dynEq ("ab", 3, DynEqShape::Bell, 280.0f, -3.5f, 1.2f, -32.0f, 2.0f, 8.0f, 120.0f)));
    all.push_back (modulePreset ("Kick Tamer", "Control", ModuleType::DynamicEq,
        Builder()
            .dynEq ("ab", 1, DynEqShape::LowShelfBaxandall, 70.0f, -2.5f, 0.707f, -22.0f, 2.5f, 12.0f, 150.0f)
            .set ("deq.lookahead", 2.0f)));
    all.push_back (modulePreset ("Air When Quiet", "Tone", ModuleType::DynamicEq,
        Builder().dynEq ("ab", 6, DynEqShape::HighShelfBaxandall, 11000.0f, 2.5f, 0.707f, -42.0f, 2.0f, 5.0f, 80.0f)));

    //==============================================================================
    // (phase 2) Match EQ presets (the first three get their synthesised reference by name, see the top comment)
    all.push_back (modulePreset ("Pink Balance", "Tonal", ModuleType::MatchEq,
        Builder().match (30.0f, 70.0f, 6.0f, 6.0f)));
    all.push_back (modulePreset ("Warm Tilt", "Tonal", ModuleType::MatchEq,
        Builder().match (25.0f, 75.0f, 4.0f, 4.0f)));
    all.push_back (modulePreset ("Bright Tilt", "Tonal", ModuleType::MatchEq,
        Builder().match (25.0f, 75.0f, 4.0f, 4.0f)));
    all.push_back (modulePreset ("Low End Only", "Region", ModuleType::MatchEq,
        Builder()
            .match (50.0f, 60.0f, 12.0f, 12.0f)
            .both ("meq", "lo", 20.0f)
            .both ("meq", "hi", 250.0f)));

    //==============================================================================
    // (phase 2) Tone Balance presets
    all.push_back (modulePreset ("Gentle Balance", "Shape", ModuleType::ToneBalance,
        Builder().tone (TT::Balanced, TM::Shape, 35.0f, 40.0f, 70.0f)));
    all.push_back (modulePreset ("Steady Low End", "Shape", ModuleType::ToneBalance,
        Builder().tone (TT::HipHop, TM::Shape, 50.0f, 55.0f, 60.0f, 50.0f, 50.0f, false, 100.0f, 30.0f, 0.0f)));
    all.push_back (modulePreset ("Resonance Sweep", "Cut", ModuleType::ToneBalance,
        Builder().tone (TT::Balanced, TM::Cut, 60.0f, 50.0f, 50.0f, 70.0f, 55.0f)));
    all.push_back (modulePreset ("Calm the Hits", "Cut", ModuleType::ToneBalance,
        Builder().tone (TT::Balanced, TM::Cut, 30.0f, 50.0f, 50.0f, 85.0f, 40.0f, true, 0.0f, 100.0f, 100.0f)));

    //==============================================================================
    // (phase 2) Definition presets
    all.push_back (modulePreset ("Open Up", "Detail", ModuleType::Definition,
        Builder().definition ("ab", 30.0f, 0.0f, 120.0f, 200.0f, 300.0f, 20000.0f)));
    all.push_back (modulePreset ("Airy Detail", "Detail", ModuleType::Definition,
        Builder().definition ("ab", 45.0f, 1.5f, 150.0f, 120.0f, 5000.0f, 20000.0f)));
    all.push_back (modulePreset ("Warm Detail", "Detail", ModuleType::Definition,
        Builder().definition ("ab", 35.0f, -2.0f, 250.0f, 250.0f, 300.0f, 4000.0f)));
    all.push_back (modulePreset ("Center Presence", "Mid-Side", ModuleType::Definition,
        Builder()
            .channelMode (ModuleType::Definition, ChannelMode::MidSide)
            .set ("defn.link", 0.0f)
            .definition ("a", 40.0f, 1.0f, 150.0f, 120.0f, 1000.0f, 6000.0f)));   // Side (set B) stays at 0

    //==============================================================================
    // (phase 2) Smoother presets
    all.push_back (modulePreset ("De-Harsh Master", "Harshness", ModuleType::Smoother,
        Builder().smoother ("ab", 35.0f, SM::Medium, 8.0f, 40.0f, 0.0f, 2500.0f, 10000.0f)));
    all.push_back (modulePreset ("Cymbal Taming", "Harshness", ModuleType::Smoother,
        Builder().smoother ("ab", 50.0f, SM::Strong, 3.0f, 25.0f, -20.0f, 6000.0f, 16000.0f)));
    all.push_back (modulePreset ("Soft Vocal Edge", "Harshness", ModuleType::Smoother,
        Builder().smoother ("ab", 40.0f, SM::Gentle, 10.0f, 60.0f, 0.0f, 1800.0f, 6000.0f)));
    all.push_back (modulePreset ("Side Shine Control", "Mid-Side", ModuleType::Smoother,
        Builder()
            .channelMode (ModuleType::Smoother, ChannelMode::MidSide)
            .set ("smth.link", 0.0f)
            .smoother ("b", 45.0f, SM::Medium, 5.0f, 30.0f, 0.0f, 4000.0f, 14000.0f)));   // Mid (set A) stays at 0

    //==============================================================================
    // (phase 2) Punch presets (Stereo, set B = set A, Analog crossover: no latency)
    all.push_back (modulePreset ("Gentle Snap", "Punch", ModuleType::Punch,
        Builder()
            .bands ("pnch", 2, 200.0f).punch (1, 15.0f).punch (2, 25.0f)
            .set ("pnch.env", 60.0f).set ("pnch.stLink", 1.0f).set ("pnch.autoGain", 1.0f)));
    all.push_back (modulePreset ("Glue the Groove", "Density", ModuleType::Punch,
        Builder()
            .bands ("pnch", 3, 150.0f, 3000.0f).punch (1, -20.0f).punch (2, -15.0f).punch (3, -10.0f)
            .set ("pnch.env", 120.0f).set ("pnch.stLink", 1.0f).set ("pnch.autoGain", 1.0f)));
    all.push_back (modulePreset ("Drum Lift", "Punch", ModuleType::Punch,
        Builder()
            .bands ("pnch", 4, 120.0f, 1000.0f, 6000.0f)
            .punch (1, 30.0f).punch (2, 20.0f).punch (3, 25.0f).punch (4, 15.0f)
            .set ("pnch.env", 40.0f).set ("pnch.autoGain", 1.0f)));
    all.push_back (modulePreset ("Tempo Bounce", "Punch", ModuleType::Punch,
        Builder()
            .bands ("pnch", 3, 180.0f, 2500.0f).punch (1, 20.0f).punch (2, 20.0f).punch (3, 15.0f)
            .set ("pnch.sync", 1.0f).set ("pnch.syncNote", 10.0f)   // 1/8
            .set ("pnch.stLink", 1.0f).set ("pnch.autoGain", 1.0f)));

    //==============================================================================
    // (phase 2) Low Contrast presets
    all.push_back (modulePreset ("Tight Kick", "Contrast", ModuleType::LowContrast,
        Builder()
            .set ("lowc.low", 30.0f).set ("lowc.high", 120.0f)
            .lowContrast ("ab", LM::Punchy, 40.0f, 0.5f)));
    all.push_back (modulePreset ("Round Sub", "Contrast", ModuleType::LowContrast,
        Builder()
            .set ("lowc.low", 20.0f).set ("lowc.high", 80.0f)
            .lowContrast ("ab", LM::Smooth, -35.0f, 1.0f)));
    all.push_back (modulePreset ("Punchy Center, Calm Sides", "Mid-Side", ModuleType::LowContrast,
        Builder()
            .channelMode (ModuleType::LowContrast, ChannelMode::MidSide)
            .set ("lowc.link", 0.0f)
            .set ("lowc.low", 40.0f).set ("lowc.high", 180.0f)
            .lowContrast ("a", LM::Punchy, 35.0f, 0.0f)
            .lowContrast ("b", LM::Smooth, -50.0f, -1.0f)));
    all.push_back (modulePreset ("Hit and Hold", "Transient/Sustain", ModuleType::LowContrast,
        Builder()
            .channelMode (ModuleType::LowContrast, ChannelMode::TransientSustain)
            .set ("lowc.link", 0.0f)
            .set ("lowc.ts.sens", 50.0f).set ("lowc.ts.len", 20.0f)
            .set ("lowc.low", 25.0f).set ("lowc.high", 200.0f)
            .lowContrast ("a", LM::Punchy, 45.0f, 0.0f)
            .lowContrast ("b", LM::Smooth, -20.0f, 0.5f)));

    //==============================================================================
    // (phase 2) Bass Shaper presets (thresholds are absolute dBFS of the low band)
    all.push_back (modulePreset ("Kick Forward", "Bass", ModuleType::BassShaper,
        Builder()
            .set ("bass.punch", 3.0f)
            .set ("bass.peak.on", 1.0f).set ("bass.peak.thr", -6.0f).set ("bass.peak.char", 100.0f)
            .set ("bass.peak.mix", 100.0f)));
    all.push_back (modulePreset ("Warm Sustain", "Bass", ModuleType::BassShaper,
        Builder()
            .set ("bass.sus.on", 1.0f).set ("bass.sus.thr", -20.0f).set ("bass.sus.amount", 40.0f)
            .set ("bass.sus.char", 60.0f)));
    all.push_back (modulePreset ("Small Speaker Drive", "Translation", ModuleType::BassShaper,
        Builder()
            .set ("bass.cutoff", 100.0f)
            .set ("bass.sus.on", 1.0f).set ("bass.sus.thr", -18.0f).set ("bass.sus.amount", 50.0f)
            .set ("bass.sus.char", 20.0f)
            .set ("bass.peak.on", 1.0f).set ("bass.peak.thr", -8.0f).set ("bass.peak.char", 30.0f)
            .set ("bass.autoGain", 1.0f)));
    all.push_back (modulePreset ("Tidy Low End", "Bass", ModuleType::BassShaper,
        Builder()
            .set ("bass.balance", -1.5f).set ("bass.punch", 1.0f)
            .set ("bass.peak.on", 1.0f).set ("bass.peak.thr", -8.0f).set ("bass.peak.char", 100.0f)));

    //==============================================================================
    // (phase 3) Classic Compressor presets (thresholds in sine-peak dBFS of the detector; 1-3 dB of reduction on a
    // -20 LUFS premaster; the release knob is the transient release, 30..150 ms: sustained reduction recovers slower on
    // its own)
    using CM = ps::ClassicCompMode;
    all.push_back (modulePreset ("Bus Glue", "Glue", ModuleType::ClassicComp,
        Builder()
            .classicComp ("ab", -18.0f, 1.5f, 30.0f, 120.0f, CM::Smooth)
            .both ("ccmp", "sc.hpf", 90.0f)));
    all.push_back (modulePreset ("Transient Snap", "Punch", ModuleType::ClassicComp,
        Builder()
            .classicComp ("ab", -16.0f, 3.0f, 25.0f, 60.0f, CM::Sharp)
            .both ("ccmp", "sc.hpf", 120.0f)));
    all.push_back (modulePreset ("Slow Leveler", "Glue", ModuleType::ClassicComp,
        Builder()
            .classicComp ("ab", -22.0f, 2.0f, 50.0f, 150.0f, CM::Smooth)
            .both ("ccmp", "sc.hpf", 60.0f)));
    all.push_back (modulePreset ("Soft on Harshness", "Tone", ModuleType::ClassicComp,
        Builder()
            .classicComp ("ab", -20.0f, 2.5f, 5.0f, 120.0f, CM::Balanced)
            .both ("ccmp", "sc.hpf", 150.0f)
            .both ("ccmp", "sc.bellHz", 3500.0f).both ("ccmp", "sc.bellDb", 4.0f)
            .both ("ccmp", "sc.shelfHz", 8000.0f).both ("ccmp", "sc.shelfDb", 3.0f)));
    all.push_back (modulePreset ("Mid Glue, Free Sides", "Mid-Side", ModuleType::ClassicComp,
        Builder()
            .channelMode (ModuleType::ClassicComp, ChannelMode::MidSide)
            .set ("ccmp.link", 0.0f)
            .classicComp ("a", -18.0f, 2.0f, 20.0f, 100.0f, CM::Balanced)
            .classicComp ("b", -24.0f, 1.3f, 40.0f, 150.0f, CM::Smooth)
            .both ("ccmp", "sc.hpf", 100.0f)));

    //==============================================================================
    // (phase 3) Passive EQ presets (amounts in the classic dB markings, 0..10; High Q 0 = broad .. 10 = narrow)
    all.push_back (modulePreset ("Low Boost and Dip", "Bass", ModuleType::PassiveEq,
        Builder()
            .both ("peq", "lowHz", 3.0f)          // 60 Hz
            .both ("peq", "lowBoost", 3.0f).both ("peq", "lowCut", 2.0f)));
    all.push_back (modulePreset ("Silky Top", "Air", ModuleType::PassiveEq,
        Builder()
            .both ("peq", "highHz", 5.0f)         // 12 kHz
            .both ("peq", "highBoost", 2.5f).both ("peq", "highQ", 3.0f)
            .both ("peq", "highCutHz", 2.0f)      // 20 kHz
            .both ("peq", "highCut", 1.0f)));
    all.push_back (modulePreset ("Midrange Clean-Up", "Tone", ModuleType::PassiveEq,
        Builder()
            .both ("peq", "midHz", 2.0f)          // 500 Hz
            .both ("peq", "midCut", 2.5f)
            .both ("peq", "hmHz", 2.0f)           // 3 kHz
            .both ("peq", "hmBoost", 1.5f)));
    all.push_back (modulePreset ("Tube Warmth", "Color", ModuleType::PassiveEq,
        Builder()
            .set ("peq.drive", 3.0f)
            .both ("peq", "lowHz", 4.0f)          // 100 Hz
            .both ("peq", "lowBoost", 1.5f)
            .both ("peq", "highCutHz", 2.0f)      // 20 kHz
            .both ("peq", "highCut", 1.5f)));
    all.push_back (modulePreset ("Solid Center, Airy Sides", "Mid-Side", ModuleType::PassiveEq,
        Builder()
            .channelMode (ModuleType::PassiveEq, ChannelMode::MidSide)
            .set ("peq.link", 0.0f)
            .set ("peq.a.lowHz", 2.0f)            // Mid: 45 Hz
            .set ("peq.a.lowBoost", 2.0f)
            .set ("peq.b.highHz", 4.0f)           // Side: 10 kHz
            .set ("peq.b.highBoost", 3.0f).set ("peq.b.highQ", 4.0f)));

    //==============================================================================
    // (phase 3) Classic Limiter presets (Threshold below the Ceiling = gain into the limiter)
    using LMode = ps::ClassicLimiterMode;
    all.push_back (modulePreset ("Tube Glue Limit", "Limiting", ModuleType::ClassicLimiter,
        Builder()
            .set ("clim.mode", (float) LMode::Tube).set ("clim.thr", -3.0f).set ("clim.ceiling", -1.0f)
            .set ("clim.character", 6.0f).set ("clim.truePeak", 1.0f)));
    all.push_back (modulePreset ("Fast Analog Catch", "Limiting", ModuleType::ClassicLimiter,
        Builder()
            .set ("clim.mode", (float) LMode::Analog).set ("clim.thr", -2.0f).set ("clim.ceiling", -0.5f)
            .set ("clim.character", 1.0f).set ("clim.truePeak", 1.0f)));
    all.push_back (modulePreset ("Modern Tube Push", "Loudness", ModuleType::ClassicLimiter,
        Builder()
            .set ("clim.mode", (float) LMode::Modern).set ("clim.thr", -5.0f).set ("clim.ceiling", -1.0f)
            .set ("clim.character", 3.0f).set ("clim.truePeak", 1.0f)));
    all.push_back (modulePreset ("Slow Tube Leveling", "Limiting", ModuleType::ClassicLimiter,
        Builder()
            .set ("clim.mode", (float) LMode::Tube).set ("clim.thr", -4.0f).set ("clim.ceiling", -1.0f)
            .set ("clim.character", 9.0f).set ("clim.truePeak", 1.0f)));

    //==============================================================================
    // (phase 3) Tape presets (the drive is level-compensated, less so at high drive; High Emphasis 4 = flat highs)
    using TS = ps::TapeSpeed;
    all.push_back (modulePreset ("Clean 30 ips", "Tape", ModuleType::Tape,
        Builder()
            .tape (TS::Ips30, -6.0f, 2.0f, 1.0f, 3.0f, 4.0f)));
    all.push_back (modulePreset ("Warm 15 ips", "Tape", ModuleType::Tape,
        Builder()
            .tape (TS::Ips15, 4.0f, 0.0f, 3.0f, 6.0f, 3.5f)));
    all.push_back (modulePreset ("Dusty 7.5 ips", "Color", ModuleType::Tape,
        Builder()
            .tape (TS::Ips7_5, 8.0f, -3.0f, 5.0f, 7.0f, 2.5f)));
    all.push_back (modulePreset ("Tape Glue", "Glue", ModuleType::Tape,
        Builder()
            .tape (TS::Ips30, 2.0f, 4.0f, 2.0f, 4.0f, 4.5f)));

    //==============================================================================
    // De-limiter presets. The thresholds suit a master peaking near full scale; Learn Threshold fits them to a song.
    all.push_back (modulePreset ("Gentle Restore", "Restore", ModuleType::DeLimiter,
        Builder().deLimiter (50.0f, -1.0f)));
    all.push_back (modulePreset ("Full Restore", "Restore", ModuleType::DeLimiter,
        Builder().deLimiter (100.0f, -1.0f)));
    all.push_back (modulePreset ("Hot Master", "Restore", ModuleType::DeLimiter,
        Builder().deLimiter (100.0f, -0.3f)));
    all.push_back (modulePreset ("Punch Restore", "Enhance", ModuleType::DeLimiter,
        Builder().deLimiter (150.0f, -1.0f)));

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
