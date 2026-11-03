#include "dsp/Params.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

// The parameter table: every id, host name, range, default and choice list of PureSheen Dynamics, plus the
// precomputed index tables the realtime read*Params functions use (no string lookup, no allocation on the audio
// thread).
//
// Order (APPEND-ONLY once released):
//   1. global I/O          io.*, in.*, out.*
//   2. per Core module (the six 0.1.x types), in ModuleType order: slot params (<prefix>.on, .chMode, .a.on, .b.on,
//      .link; single-mode modules only <prefix>.on) with the channel-mode lists as released in 0.1.1, then module
//      params in the order of the struct members in Params.h; band params band by band (set A bands 1..N, then set B)
//   3. dither              dith.*                                     (end of the 0.1.1 table: 479 entries)
//   4. phase 2 appendix
//      a. Core T/S additions in ModuleType order: eq1.ts.sens/len, eq2.ts.sens/len, the Imager channel slot (img.chMode,
//         img.a.on, img.b.on, img.link, img.ts.sens/len) and its set B (img.b.b<n>.width, img.b.stz.*, img.b.rec.*),
//         then exc.ts.on, exc.ts.sens/len
//      b. each phase 2 module in ModuleType order: slot params (+ .ts.sens / .ts.len when it offers T/S), then its
//         module params in struct order (per-set params set A then set B; bands band by band)
//   5. vintage appendix (phase 3, after bass.autoGain): ccmp, peq, clim, tape in ModuleType order, built exactly like
//      4b (shared params first, then set A, then set B)
// Bands of the multiband modules exist for all kMaxBands bands and both channel sets even when a module does not use
// them (Imager width is per band only, it is Stereo-only).
//
// Log-scaled ranges ("log" in Params.h) use a skew centre at the geometric mean of the range, so the middle of the
// host's 0..1 range sits at the logarithmic middle.
//
// Crossovers: read*Params clamps them to 20..20000 Hz and makes them non-decreasing (a crossover below the one before
// it is held at that one), so the DSP always receives ascending frequencies.
namespace ps
{
namespace
{
//==============================================================================
// Index tables (filled while the specs are built).
struct IoIdx
{
    int bypass = -1, gainMatch = -1, gainMatchModern = -1, mono = -1, swap = -1;
    int inGain[2] { -1, -1 }, outGain[2] { -1, -1 };
};

struct SlotIdx
{
    int on = -1, chMode = -1, setOn[kNumChannelSets] { -1, -1 }, link = -1;
    int tsSens = -1, tsLen = -1;   // (phase 2) T/S split controls
    int tsOn = -1;                 // (phase 2) Exciter only: "exc.ts.on" overrides exc.chMode with Transient/Sustain
};

struct EqBandIdx
{
    int on = -1, shape = -1, freq = -1, gain = -1, q = -1, slope = -1, phase = -1;
};

struct EqIdx
{
    int mode = -1, amount = -1, softSat = -1, firSize = -1;
    EqBandIdx bands[kNumChannelSets][kEqBands];
};

struct XoverIdx
{
    int bands = -1, xover[kMaxCrossovers] { -1, -1, -1 }, type = -1;
};

struct CompIdx
{
    int on = -1, thr = -1, ratio = -1, att = -1, rel = -1, knee = -1;
};

struct DynBandIdx
{
    int on = -1;
    CompIdx comp, lim;
    int mix = -1, gain = -1;
};

struct DynIdx
{
    XoverIdx x;
    int detect = -1, detFilter = -1, detFreq = -1, detSlope = -1, detTilt = -1, lookahead = -1, adaptRel = -1,
        autoGain = -1, gain = -1;
    DynBandIdx bands[kNumChannelSets][kMaxBands];
};

struct ImgSetIdx
{
    int width[kMaxBands] { -1, -1, -1, -1 };
    int stzOn = -1, stzMode = -1, stzAmount = -1, recOn = -1, recGain = -1;
};

struct ImgIdx
{
    XoverIdx x;
    int width[kMaxBands] { -1, -1, -1, -1 };
    int amount = -1, stzOn = -1, stzMode = -1, stzAmount = -1, recOn = -1, recGain = -1, antiphase = -1;
    ImgSetIdx setB;   // (phase 2)
};

struct ExcBandIdx
{
    int on = -1, mode = -1, drive = -1, mix = -1;
};

struct ExcIdx
{
    XoverIdx x;
    int os = -1, postOn = -1, postFreq = -1, postGain = -1;
    ExcBandIdx bands[kNumChannelSets][kMaxBands];
};

struct MaxIdx
{
    int mode = -1, style3 = -1, style4 = -1, gain = -1, ceiling = -1, link = -1, truePeak = -1, character = -1,
        upward = -1, clipOn = -1, clipMode = -1, clipAmount = -1, transOn = -1, transAmount = -1, siTrans = -1,
        siSus = -1, learnTarget = -1;
};

struct DithIdx
{
    int on = -1, bits = -1, amount = -1, harm = -1, shape = -1, limit = -1, blank = -1, dc = -1;
};

// ---- phase 2 ----
struct DeqBandIdx
{
    int on = -1, shape = -1, freq = -1, gain = -1, q = -1, dir = -1, thr = -1, ratio = -1, knee = -1, att = -1, rel = -1,
        autoScale = -1, offset = -1, dyn = -1, sc = -1;
};

struct DeqIdx
{
    int mode = -1, firSize = -1, lookahead = -1;
    DeqBandIdx bands[kNumChannelSets][kDynEqBands];
};

struct MeqIdx
{
    int phase = -1, firSize = -1;
    struct Set { int amount = -1, smooth = -1, lo = -1, hi = -1, boost = -1, cut = -1; } sets[kNumChannelSets];
};

struct TbalIdx
{
    int target = -1, mode = -1;
    struct Set
    {
        int amount = -1, speed = -1, smooth = -1, cutSpeed = -1, sens = -1, tame = -1, low = -1, mid = -1, high = -1;
    } sets[kNumChannelSets];
};

struct DefnIdx
{
    struct Set { int amount = -1, tilt = -1, att = -1, rel = -1, low = -1, high = -1; } sets[kNumChannelSets];
};

struct SmthIdx
{
    struct Set { int amount = -1, mode = -1, att = -1, rel = -1, tone = -1, low = -1, high = -1; } sets[kNumChannelSets];
};

struct PnchIdx
{
    XoverIdx x;
    int amount = -1, env = -1, sync = -1, syncNote = -1, stLink = -1, autoGain = -1, bandLink = -1;
    struct Band { int on = -1, amount = -1; } bands[kNumChannelSets][kMaxBands];
};

struct LowcIdx
{
    int low = -1, high = -1, autoGain = -1;
    struct Set { int mode = -1, contrast = -1, gain = -1; } sets[kNumChannelSets];
};

struct BassIdx
{
    int cutoff = -1, balance = -1, punch = -1, susOn = -1, susThr = -1, susAmount = -1, susChar = -1, peakOn = -1,
        peakThr = -1, peakChar = -1, peakMix = -1, autoGain = -1;
};

// ---- vintage-style modules (phase 3) ----
struct CcmpIdx
{
    struct Set
    {
        int thr = -1, ratio = -1, attack = -1, release = -1, mode = -1, makeup = -1, autoGain = -1, scHpf = -1,
            scBellHz = -1, scBellDb = -1, scShelfHz = -1, scShelfDb = -1;
    } sets[kNumChannelSets];
};

struct PeqIdx
{
    int drive = -1;
    struct Set
    {
        int lowHz = -1, lowBoost = -1, lowCut = -1, highHz = -1, highBoost = -1, highBw = -1, highCutHz = -1,
            highCut = -1, lmHz = -1, lmBoost = -1, midHz = -1, midCut = -1, hmHz = -1, hmBoost = -1;
    } sets[kNumChannelSets];
};

struct ClimIdx
{
    int thr = -1, ceiling = -1, character = -1, mode = -1, truePeak = -1, link = -1;
};

struct TapeIdx
{
    int speed = -1, drive = -1, bias = -1, harmonics = -1, lowEmph = -1, highEmph = -1;
};

// Channel modes per module as a fixed table (the realtime side of supportedChannelModes).
struct ChannelModeTable
{
    ChannelMode modes[4] {};
    int count = 0;
};

struct Table
{
    std::vector<ParamSpec> specs;
    std::unordered_map<std::string, int> byId;

    IoIdx io;
    SlotIdx slot[kNumModuleTypes];
    ChannelModeTable channelModes[kNumModuleTypes];   // supported modes (final)
    ChannelModeTable choiceModes[kNumModuleTypes];    // modes of the "<prefix>.chMode" choice, in choice order
    EqIdx eq[2];
    DynIdx dyn;
    ImgIdx img;
    ExcIdx exc;
    MaxIdx max;
    DithIdx dith;
    DeqIdx deq;
    MeqIdx meq;
    TbalIdx tbal;
    DefnIdx defn;
    SmthIdx smth;
    PnchIdx pnch;
    LowcIdx lowc;
    BassIdx bass;
    CcmpIdx ccmp;
    PeqIdx peq;
    ClimIdx clim;
    TapeIdx tape;
};

//==============================================================================
// Choice lists (plain ASCII; index = enum value).
const std::vector<std::string>& eqShapeLabels()
{
    static const std::vector<std::string> labels {
        "Bell", "Proportional Q", "Band Shelf",
        "Low Shelf Analog", "Low Shelf Baxandall", "Low Shelf Vintage", "Low Shelf Resonant",
        "High Shelf Analog", "High Shelf Baxandall", "High Shelf Vintage", "High Shelf Resonant",
        "High Pass Flat", "High Pass Resonant", "High Pass Brickwall",
        "Low Pass Flat", "Low Pass Resonant", "Low Pass Brickwall",
        "Surgical"
    };
    return labels;
}

const char* channelModeLabel (ChannelMode m)
{
    switch (m)
    {
        case ChannelMode::Stereo:           return "Stereo";
        case ChannelMode::MidSide:          return "Mid/Side";
        case ChannelMode::LeftRight:        return "Left/Right";
        case ChannelMode::TransientSustain: return "Transient/Sustain";
    }
    return "Stereo";
}

// Supported channel modes per module (final; see supportedChannelModes in Params.h).
ChannelModeTable channelModesFor (ModuleType type)
{
    ChannelModeTable t;
    auto add = [&t] (ChannelMode m) { t.modes[t.count++] = m; };
    add (ChannelMode::Stereo);

    switch (type)
    {
        case ModuleType::Equalizer1:
        case ModuleType::Equalizer2:
        case ModuleType::DynamicEq:
        case ModuleType::MatchEq:
        case ModuleType::PassiveEq:
            add (ChannelMode::MidSide);
            add (ChannelMode::LeftRight);
            add (ChannelMode::TransientSustain);
            break;
        case ModuleType::Exciter:
        case ModuleType::ToneBalance:
        case ModuleType::Definition:
        case ModuleType::Smoother:
        case ModuleType::LowContrast:
            add (ChannelMode::MidSide);
            add (ChannelMode::TransientSustain);
            break;
        case ModuleType::Dynamics:
        case ModuleType::Punch:
        case ModuleType::ClassicComp:
            add (ChannelMode::MidSide);
            break;
        case ModuleType::Imager:
            add (ChannelMode::TransientSustain);
            break;
        case ModuleType::Maximizer:
        case ModuleType::BassShaper:
        case ModuleType::ClassicLimiter:
        case ModuleType::Tape:
        case ModuleType::Count:
        default:
            break;
    }
    return t;
}

// The channel-mode choice lists exactly as released in 0.1.1 (the Core part of the table is frozen).
ChannelModeTable releasedChannelModes (ModuleType type)
{
    ChannelModeTable t;
    auto add = [&t] (ChannelMode m) { t.modes[t.count++] = m; };
    add (ChannelMode::Stereo);

    switch (type)
    {
        case ModuleType::Equalizer1:
        case ModuleType::Equalizer2:
            add (ChannelMode::MidSide);
            add (ChannelMode::LeftRight);
            add (ChannelMode::TransientSustain);
            break;
        case ModuleType::Dynamics:
        case ModuleType::Exciter:
            add (ChannelMode::MidSide);
            break;
        default:
            break;
    }
    return t;
}

bool hasMode (const ChannelModeTable& t, ChannelMode m) noexcept
{
    for (int i = 0; i < t.count; ++i)
        if (t.modes[i] == m)
            return true;
    return false;
}

float geometricCentre (float lo, float hi) { return std::sqrt (lo * hi); }

//==============================================================================
class Builder
{
public:
    explicit Builder (Table& t) : table (t) {}

    int addFloat (const std::string& id, const std::string& name, float lo, float hi, float def,
                  const std::string& unit, int module, float skewCentre = 0.0f, float interval = 0.0f)
    {
        ParamSpec s;
        s.id = id;
        s.name = name;
        s.kind = ParamKind::Float;
        s.minValue = lo;
        s.maxValue = hi;
        s.defaultValue = def;
        s.skewCentre = skewCentre;
        s.interval = interval;
        s.unit = unit;
        s.module = module;
        return push (std::move (s));
    }

    int addLog (const std::string& id, const std::string& name, float lo, float hi, float def,
                const std::string& unit, int module)
    {
        return addFloat (id, name, lo, hi, def, unit, module, geometricCentre (lo, hi));
    }

    int addBool (const std::string& id, const std::string& name, bool def, int module)
    {
        ParamSpec s;
        s.id = id;
        s.name = name;
        s.kind = ParamKind::Bool;
        s.minValue = 0.0f;
        s.maxValue = 1.0f;
        s.defaultValue = def ? 1.0f : 0.0f;
        s.interval = 1.0f;
        s.module = module;
        return push (std::move (s));
    }

    int addChoice (const std::string& id, const std::string& name, std::vector<std::string> choices, int def,
                   int module)
    {
        ParamSpec s;
        s.id = id;
        s.name = name;
        s.kind = ParamKind::Choice;
        s.minValue = 0.0f;
        s.maxValue = (float) (choices.size() - 1);
        s.defaultValue = (float) def;
        s.interval = 1.0f;
        s.choices = std::move (choices);
        s.module = module;
        return push (std::move (s));
    }

private:
    int push (ParamSpec&& s)
    {
        const int index = (int) table.specs.size();
        table.byId.emplace (s.id, index);
        table.specs.push_back (std::move (s));
        return index;
    }

    Table& table;
};

//==============================================================================
// Naming helpers
const char* setLetter (int set) { return set == 0 ? "a" : "b"; }
const char* setSuffix (int set) { return set == 0 ? " (A)" : " (B)"; }

std::string bandIdPrefix (const std::string& prefix, int set, int band)   // "eq1.a.b3."
{
    return prefix + "." + setLetter (set) + ".b" + std::to_string (band + 1) + ".";
}

std::string bandName (const std::string& shortName, int band, const std::string& label, int set)
{
    return shortName + " Band " + std::to_string (band + 1) + " " + label + (set >= 0 ? setSuffix (set) : "");
}

//==============================================================================
void addGlobalIo (Builder& b, IoIdx& io)
{
    io.bypass          = b.addBool ("io.bypass", "Bypass", false, -1);
    io.gainMatch       = b.addBool ("io.gainMatch", "Gain Match", false, -1);
    io.gainMatchModern = b.addBool ("io.gainMatchModern", "Gain Match Modern", false, -1);
    io.mono            = b.addBool ("io.mono", "Sum to Mono", false, -1);
    io.swap            = b.addBool ("io.swap", "Swap Channels", false, -1);
    io.inGain[0]       = b.addFloat ("in.gainL", "Input Gain L", -24.0f, 24.0f, 0.0f, "dB", -1);
    io.inGain[1]       = b.addFloat ("in.gainR", "Input Gain R", -24.0f, 24.0f, 0.0f, "dB", -1);
    io.outGain[0]      = b.addFloat ("out.gainL", "Output Gain L", -24.0f, 24.0f, 0.0f, "dB", -1);
    io.outGain[1]      = b.addFloat ("out.gainR", "Output Gain R", -24.0f, 24.0f, 0.0f, "dB", -1);
}

// The channel parameters of a slot (chMode with the given choice list, a.on, b.on, link).
void addChannelSlot (Builder& b, ModuleType type, SlotIdx& slot, const ChannelModeTable& modes)
{
    const auto& info = moduleInfo (type);
    const std::string p = info.idPrefix;
    const std::string n = info.shortName;
    const int m = (int) type;

    std::vector<std::string> labels;
    for (int i = 0; i < modes.count; ++i)
        labels.emplace_back (channelModeLabel (modes.modes[i]));

    slot.chMode   = b.addChoice (p + ".chMode", n + " Channel Mode", std::move (labels), 0, m);
    slot.setOn[0] = b.addBool (p + ".a.on", n + " Channel On (A)", true, m);
    slot.setOn[1] = b.addBool (p + ".b.on", n + " Channel On (B)", true, m);
    slot.link     = b.addBool (p + ".link", n + " Channel Link", true, m);
}

void addSlot (Builder& b, ModuleType type, SlotIdx& slot, const ChannelModeTable& choiceModes)
{
    const auto& info = moduleInfo (type);
    slot.on = b.addBool (std::string (info.idPrefix) + ".on", std::string (info.shortName) + " Power", true, (int) type);

    // Single-mode modules: no channel parameters (see SlotParams in Params.h).
    if (choiceModes.count >= 2)
        addChannelSlot (b, type, slot, choiceModes);
}

// (phase 2) The T/S split controls of a module that offers Transient/Sustain.
void addTsParams (Builder& b, ModuleType type, SlotIdx& slot)
{
    const auto& info = moduleInfo (type);
    const std::string p = info.idPrefix;
    const std::string n = info.shortName;
    const SlotParams d;
    slot.tsSens = b.addFloat (p + ".ts.sens", n + " T/S Sensitivity", 0.0f, 100.0f, d.tsSensitivity, "%", (int) type);
    slot.tsLen  = b.addLog (p + ".ts.len", n + " T/S Length", 5.0f, 100.0f, d.tsLengthMs, "ms", (int) type);
}

void addCrossovers (Builder& b, const std::string& p, const std::string& n, int m, CrossoverType defType,
                    XoverIdx& x)
{
    static const float defaults[kMaxCrossovers] { 120.0f, 1000.0f, 6000.0f };
    x.bands = b.addFloat (p + ".bands", n + " Bands", 1.0f, (float) kMaxBands, 1.0f, "", m, 0.0f, 1.0f);
    for (int i = 0; i < kMaxCrossovers; ++i)
        x.xover[i] = b.addLog (p + ".xover" + std::to_string (i + 1), n + " Crossover " + std::to_string (i + 1),
                               20.0f, 20000.0f, defaults[i], "Hz", m);
    x.type = b.addChoice (p + ".xoverType", n + " Crossover Type", { "Analog", "Digital", "Hybrid" }, (int) defType, m);
}

void addEq (Builder& b, ModuleType type, EqIdx& eq)
{
    const auto& info = moduleInfo (type);
    const std::string p = info.idPrefix;
    const std::string n = info.shortName;
    const int m = (int) type;
    const EqParams d;

    eq.mode    = b.addChoice (p + ".mode", n + " Filter Mode", { "Analog", "Digital" }, (int) d.filterMode, m);
    eq.amount  = b.addFloat (p + ".amount", n + " Amount", 0.0f, 200.0f, d.amountPercent, "%", m);
    eq.softSat = b.addBool (p + ".softSat", n + " Soft Saturation", d.softSaturation, m);
    eq.firSize = b.addChoice (p + ".firSize", n + " FIR Size", { "4096", "8192", "16384", "32768" }, d.firSizeIndex, m);

    std::vector<std::string> slopes;
    for (int db : kEqSlopeDb)
        slopes.push_back (std::to_string (db) + " dB/oct");

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        for (int band = 0; band < kEqBands; ++band)
        {
            const auto& bd = d.bands[set][band];
            const auto id = bandIdPrefix (p, set, band);
            auto& bi = eq.bands[set][band];
            bi.on    = b.addBool (id + "on", bandName (n, band, "On", set), bd.on, m);
            bi.shape = b.addChoice (id + "shape", bandName (n, band, "Shape", set), eqShapeLabels(), (int) bd.shape, m);
            bi.freq  = b.addLog (id + "freq", bandName (n, band, "Freq", set), 20.0f, 20000.0f, bd.freqHz, "Hz", m);
            bi.gain  = b.addFloat (id + "gain", bandName (n, band, "Gain", set), -30.0f, 15.0f, bd.gainDb, "dB", m);
            bi.q     = b.addLog (id + "q", bandName (n, band, "Q", set), 0.1f, 24.0f, bd.q, "", m);
            bi.slope = b.addChoice (id + "slope", bandName (n, band, "Slope", set), slopes, (int) bd.slope, m);
            bi.phase = b.addFloat (id + "phase", bandName (n, band, "Phase", set), 0.0f, 100.0f, bd.phasePercent, "%", m);
        }
    }
}

void addDynamics (Builder& b, DynIdx& dyn)
{
    const auto& info = moduleInfo (ModuleType::Dynamics);
    const std::string p = info.idPrefix;
    const std::string n = info.shortName;
    const int m = (int) ModuleType::Dynamics;
    const DynamicsParams d;

    addCrossovers (b, p, n, m, d.crossoverType, dyn.x);
    dyn.detect    = b.addChoice (p + ".detect", n + " Detection", { "Peak", "Envelope", "RMS" }, (int) d.detection, m);
    dyn.detFilter = b.addChoice (p + ".detFilter", n + " Detection Filter", { "None", "High Pass", "Tilt" },
                                 (int) d.detectionFilter, m);
    dyn.detFreq   = b.addLog (p + ".detFreq", n + " Detection Filter Freq", 20.0f, 2000.0f, d.detectionFilterHz, "Hz", m);
    dyn.detSlope  = b.addChoice (p + ".detSlope", n + " Detection Filter Slope",
                                 { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" }, d.detectionFilterSlopeIndex, m);
    dyn.detTilt   = b.addFloat (p + ".detTilt", n + " Detection Tilt", -6.0f, 6.0f, d.detectionTiltDb, "dB/oct", m);
    dyn.lookahead = b.addFloat (p + ".lookahead", n + " Lookahead", 0.0f, 10.0f, d.lookaheadMs, "ms", m);
    dyn.adaptRel  = b.addBool (p + ".adaptRel", n + " Adaptive Release", d.adaptiveRelease, m);
    dyn.autoGain  = b.addBool (p + ".autoGain", n + " Auto Gain", d.autoGain, m);
    dyn.gain      = b.addFloat (p + ".gain", n + " Output Gain", -24.0f, 24.0f, d.outputGainDb, "dB", m);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        for (int band = 0; band < kMaxBands; ++band)
        {
            const auto& bd = d.bands[set][band];
            const auto id = bandIdPrefix (p, set, band);
            auto& bi = dyn.bands[set][band];
            bi.on         = b.addBool (id + "on", bandName (n, band, "On", set), bd.on, m);
            bi.comp.on    = b.addBool (id + "comp.on", bandName (n, band, "Comp On", set), bd.comp.on, m);
            bi.comp.thr   = b.addFloat (id + "comp.thr", bandName (n, band, "Comp Threshold", set), -60.0f, 0.0f,
                                        bd.comp.thresholdDb, "dB", m);
            bi.comp.ratio = b.addFloat (id + "comp.ratio", bandName (n, band, "Comp Ratio", set), -10.0f, 30.0f,
                                        bd.comp.ratio, ":1", m);
            bi.comp.att   = b.addLog (id + "comp.att", bandName (n, band, "Comp Attack", set), 0.1f, 500.0f,
                                      bd.comp.attackMs, "ms", m);
            bi.comp.rel   = b.addLog (id + "comp.rel", bandName (n, band, "Comp Release", set), 1.0f, 5000.0f,
                                      bd.comp.releaseMs, "ms", m);
            bi.comp.knee  = b.addFloat (id + "comp.knee", bandName (n, band, "Comp Knee", set), 0.0f, 30.0f,
                                        bd.comp.kneeDb, "dB", m);
            bi.lim.on     = b.addBool (id + "lim.on", bandName (n, band, "Limiter On", set), bd.lim.on, m);
            bi.lim.thr    = b.addFloat (id + "lim.thr", bandName (n, band, "Limiter Threshold", set), -60.0f, 0.0f,
                                        bd.lim.thresholdDb, "dB", m);
            bi.lim.ratio  = b.addFloat (id + "lim.ratio", bandName (n, band, "Limiter Ratio", set), -2.5f, 30.0f,
                                        bd.lim.ratio, ":1", m);
            bi.lim.att    = b.addLog (id + "lim.att", bandName (n, band, "Limiter Attack", set), 0.1f, 100.0f,
                                      bd.lim.attackMs, "ms", m);
            bi.lim.rel    = b.addLog (id + "lim.rel", bandName (n, band, "Limiter Release", set), 1.0f, 2000.0f,
                                      bd.lim.releaseMs, "ms", m);
            bi.lim.knee   = b.addFloat (id + "lim.knee", bandName (n, band, "Limiter Knee", set), 0.0f, 30.0f,
                                        bd.lim.kneeDb, "dB", m);
            bi.mix        = b.addFloat (id + "mix", bandName (n, band, "Mix", set), 0.0f, 100.0f, bd.parallelPercent,
                                        "%", m);
            bi.gain       = b.addFloat (id + "gain", bandName (n, band, "Gain", set), -24.0f, 24.0f, bd.gainDb, "dB", m);
        }
    }
}

void addImager (Builder& b, ImgIdx& img)
{
    const auto& info = moduleInfo (ModuleType::Imager);
    const std::string p = info.idPrefix;
    const std::string n = info.shortName;
    const int m = (int) ModuleType::Imager;
    const ImagerParams d;

    addCrossovers (b, p, n, m, d.crossoverType, img.x);
    for (int band = 0; band < kMaxBands; ++band)
        img.width[band] = b.addFloat (p + ".b" + std::to_string (band + 1) + ".width", bandName (n, band, "Width", -1),
                                      -100.0f, 100.0f, d.widthPercent[band], "%", m);
    img.amount    = b.addFloat (p + ".amount", n + " Amount", 0.0f, 100.0f, d.amountPercent, "%", m);
    img.stzOn     = b.addBool (p + ".stz.on", n + " Stereoize On", d.stereoizeOn, m);
    img.stzMode   = b.addChoice (p + ".stz.mode", n + " Stereoize Mode", { "Mode I", "Mode II" }, (int) d.stereoizeMode, m);
    img.stzAmount = b.addFloat (p + ".stz.amount", n + " Stereoize Amount", 0.0f, 100.0f, d.stereoizeAmount, "%", m);
    img.recOn     = b.addBool (p + ".rec.on", n + " Recover Sides On", d.recoverSidesOn, m);
    img.recGain   = b.addFloat (p + ".rec.gain", n + " Recover Sides Gain", -12.0f, 12.0f, d.recoverSidesGainDb, "dB", m);
    img.antiphase = b.addBool (p + ".antiphase", n + " Prevent Antiphase", d.preventAntiphase, m);
}

void addExciter (Builder& b, ExcIdx& exc)
{
    const auto& info = moduleInfo (ModuleType::Exciter);
    const std::string p = info.idPrefix;
    const std::string n = info.shortName;
    const int m = (int) ModuleType::Exciter;
    const ExciterParams d;

    addCrossovers (b, p, n, m, d.crossoverType, exc.x);
    exc.os       = b.addBool (p + ".os", n + " Oversampling", d.oversampling, m);
    exc.postOn   = b.addBool (p + ".post.on", n + " Post Filter On", d.postFilterOn, m);
    exc.postFreq = b.addLog (p + ".post.freq", n + " Post Filter Freq", 1000.0f, 20000.0f, d.postFilterHz, "Hz", m);
    exc.postGain = b.addFloat (p + ".post.gain", n + " Post Filter Gain", -12.0f, 12.0f, d.postFilterGainDb, "dB", m);

    const std::vector<std::string> modes { "Analog", "Retro", "Tape", "Tube", "Warm", "Triode", "Dual Triode" };

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        for (int band = 0; band < kMaxBands; ++band)
        {
            const auto& bd = d.bands[set][band];
            const auto id = bandIdPrefix (p, set, band);
            auto& bi = exc.bands[set][band];
            bi.on    = b.addBool (id + "on", bandName (n, band, "On", set), bd.on, m);
            bi.mode  = b.addChoice (id + "mode", bandName (n, band, "Mode", set), modes, (int) bd.mode, m);
            bi.drive = b.addFloat (id + "drive", bandName (n, band, "Drive", set), 0.0f, 100.0f, bd.drivePercent, "%", m);
            bi.mix   = b.addFloat (id + "mix", bandName (n, band, "Mix", set), 0.0f, 100.0f, bd.mixPercent, "%", m);
        }
    }
}

void addMaximizer (Builder& b, MaxIdx& mx)
{
    const auto& info = moduleInfo (ModuleType::Maximizer);
    const std::string p = info.idPrefix;
    const std::string n = info.shortName;
    const int m = (int) ModuleType::Maximizer;
    const MaximizerParams d;

    mx.mode        = b.addChoice (p + ".mode", n + " Mode",
                                  { "Low Latency", "Smooth", "Transparent", "Aggressive", "Spectral", "Multiband" },
                                  (int) d.mode, m);
    mx.style3      = b.addChoice (p + ".style3", n + " Aggressive Style", { "Clipping", "Crisp", "Balanced", "Pumping" },
                                  (int) d.aggressiveStyle, m);
    mx.style4      = b.addChoice (p + ".style4", n + " Spectral Style", { "Classic", "Modern", "Transient" },
                                  (int) d.spectralStyle, m);
    mx.gain        = b.addFloat (p + ".gain", n + " Gain", 0.0f, 20.0f, d.gainDb, "dB", m);
    mx.ceiling     = b.addFloat (p + ".ceiling", n + " Output Level", -20.0f, 0.0f, d.outputLevelDb, "dB", m);
    mx.link        = b.addBool (p + ".link", n + " Link Gain and Output", d.linkGainOutput, m);
    mx.truePeak    = b.addBool (p + ".truePeak", n + " True Peak", d.truePeak, m);
    mx.character   = b.addFloat (p + ".character", n + " Character", 0.0f, 10.0f, d.character, "", m);
    mx.upward      = b.addFloat (p + ".upward", n + " Upward Compress", 0.0f, 12.0f, d.upwardCompressDb, "dB", m);
    mx.clipOn      = b.addBool (p + ".clip.on", n + " Soft Clip On", d.softClipOn, m);
    mx.clipMode    = b.addChoice (p + ".clip.mode", n + " Soft Clip Mode", { "Light", "Moderate", "Heavy" },
                                  (int) d.softClipMode, m);
    mx.clipAmount  = b.addFloat (p + ".clip.amount", n + " Soft Clip Amount", 0.0f, 100.0f, d.softClipAmount, "%", m);
    mx.transOn     = b.addBool (p + ".trans.on", n + " Transient Emphasis On", d.transientEmphasisOn, m);
    mx.transAmount = b.addFloat (p + ".trans.amount", n + " Transient Emphasis", 0.0f, 100.0f, d.transientEmphasis,
                                 "%", m);
    mx.siTrans     = b.addFloat (p + ".si.trans", n + " Stereo Independence Transient", 0.0f, 100.0f,
                                 d.stereoIndependenceTransient, "%", m);
    mx.siSus       = b.addFloat (p + ".si.sus", n + " Stereo Independence Sustain", 0.0f, 100.0f,
                                 d.stereoIndependenceSustain, "%", m);
    mx.learnTarget = b.addFloat (p + ".learnTarget", n + " Learn Target", -24.0f, -5.0f, d.learnTargetLufs, "LUFS", m);
}

void addDither (Builder& b, DithIdx& di)
{
    const DitherParams d;
    di.on     = b.addBool ("dith.on", "Dither On", d.on, -1);
    di.bits   = b.addChoice ("dith.bits", "Dither Bit Depth", { "24 bit", "20 bit", "16 bit", "12 bit", "8 bit" },
                             (int) d.bits, -1);
    di.amount = b.addChoice ("dith.amount", "Dither Amount", { "Strong", "Medium", "Low", "Off" }, (int) d.amount, -1);
    di.harm   = b.addBool ("dith.harm", "Dither Harmonic Suppression", d.harmonicSuppression, -1);
    di.shape  = b.addFloat ("dith.shape", "Dither Noise Shaping", 0.0f, 100.0f, d.noiseShaping, "%", -1);
    di.limit  = b.addBool ("dith.limit", "Dither Limit Peaks", d.limitPeaks, -1);
    di.blank  = b.addBool ("dith.blank", "Dither Auto Blank", d.autoBlank, -1);
    di.dc     = b.addBool ("dith.dc", "DC Filter", d.dcFilter, -1);
}

//==============================================================================
// Phase 2 appendix (after dith.*)
void addCoreTransientSustain (Builder& b, Table& t)
{
    for (auto type : { ModuleType::Equalizer1, ModuleType::Equalizer2 })
        addTsParams (b, type, t.slot[(int) type]);

    // The Imager was single-mode in 0.1.x: its channel slot is appended here, then its set B.
    {
        const auto type = ModuleType::Imager;
        const int m = (int) type;
        const std::string p = moduleInfo (type).idPrefix;
        const std::string n = moduleInfo (type).shortName;
        auto& slot = t.slot[m];
        t.choiceModes[m] = channelModesFor (type);
        addChannelSlot (b, type, slot, t.choiceModes[m]);
        addTsParams (b, type, slot);

        const ImagerSetParams d;
        auto& sb = t.img.setB;
        for (int band = 0; band < kMaxBands; ++band)
            sb.width[band] = b.addFloat (bandIdPrefix (p, 1, band) + "width", bandName (n, band, "Width", 1), -100.0f,
                                         100.0f, d.widthPercent[band], "%", m);
        sb.stzOn     = b.addBool (p + ".b.stz.on", n + " Stereoize On (B)", d.stereoizeOn, m);
        sb.stzMode   = b.addChoice (p + ".b.stz.mode", n + " Stereoize Mode (B)", { "Mode I", "Mode II" },
                                    (int) d.stereoizeMode, m);
        sb.stzAmount = b.addFloat (p + ".b.stz.amount", n + " Stereoize Amount (B)", 0.0f, 100.0f, d.stereoizeAmount, "%", m);
        sb.recOn     = b.addBool (p + ".b.rec.on", n + " Recover Sides On (B)", d.recoverSidesOn, m);
        sb.recGain   = b.addFloat (p + ".b.rec.gain", n + " Recover Sides Gain (B)", -12.0f, 12.0f, d.recoverSidesGainDb,
                                   "dB", m);
    }

    {
        const int m = (int) ModuleType::Exciter;
        auto& slot = t.slot[m];
        slot.tsOn = b.addBool ("exc.ts.on", "Exciter Transient/Sustain Mode", false, m);
        addTsParams (b, ModuleType::Exciter, slot);
    }
}

void addDynamicEq (Builder& b, DeqIdx& x)
{
    const auto type = ModuleType::DynamicEq;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const DynamicEqParams d;
    static const float freqs[kDynEqBands] { 60.0f, 200.0f, 500.0f, 1500.0f, 4000.0f, 10000.0f };

    x.mode      = b.addChoice (p + ".mode", n + " Filter Mode", { "Analog", "Digital" }, (int) d.filterMode, m);
    x.firSize   = b.addChoice (p + ".firSize", n + " FIR Size", { "4096", "8192", "16384", "32768" }, d.firSizeIndex, m);
    x.lookahead = b.addFloat (p + ".lookahead", n + " Lookahead", 0.0f, 10.0f, d.lookaheadMs, "ms", m);

    const std::vector<std::string> shapes { "Bell", "Proportional Q", "Band Shelf", "Low Shelf Baxandall",
                                            "High Shelf Baxandall", "Low Shelf Analog", "High Shelf Analog" };
    const std::vector<std::string> sidechains { "Own band", "Band 1", "Band 2", "Band 3", "Band 4", "Band 5", "Band 6",
                                                "Other channel" };

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        for (int band = 0; band < kDynEqBands; ++band)
        {
            const auto& bd = d.bands[set][band];
            const auto id = bandIdPrefix (p, set, band);
            auto& bi = x.bands[set][band];
            bi.on        = b.addBool (id + "on", bandName (n, band, "On", set), bd.on, m);
            bi.shape     = b.addChoice (id + "shape", bandName (n, band, "Shape", set), shapes, (int) bd.shape, m);
            bi.freq      = b.addLog (id + "freq", bandName (n, band, "Freq", set), 20.0f, 20000.0f, freqs[band], "Hz", m);
            bi.gain      = b.addFloat (id + "gain", bandName (n, band, "Gain", set), -30.0f, 15.0f, bd.gainDb, "dB", m);
            bi.q         = b.addLog (id + "q", bandName (n, band, "Q", set), 0.1f, 24.0f, bd.q, "", m);
            bi.dir       = b.addChoice (id + "dir", bandName (n, band, "Direction", set), { "Down", "Up" },
                                        (int) bd.direction, m);
            bi.thr       = b.addFloat (id + "thr", bandName (n, band, "Threshold", set), -130.0f, 0.0f, bd.thresholdDb,
                                       "dB", m, -30.0f);
            bi.ratio     = b.addLog (id + "ratio", bandName (n, band, "Ratio", set), 1.0f, 20.0f, bd.ratio, ":1", m);
            bi.knee      = b.addFloat (id + "knee", bandName (n, band, "Knee", set), 0.0f, 24.0f, bd.kneeDb, "dB", m);
            bi.att       = b.addLog (id + "att", bandName (n, band, "Attack", set), 0.1f, 500.0f, bd.attackMs, "ms", m);
            bi.rel       = b.addLog (id + "rel", bandName (n, band, "Release", set), 1.0f, 5000.0f, bd.releaseMs, "ms", m);
            bi.autoScale = b.addBool (id + "auto", bandName (n, band, "Auto Scale", set), bd.autoScale, m);
            bi.offset    = b.addFloat (id + "offset", bandName (n, band, "Offset", set), -15.0f, 15.0f, bd.offsetDb, "dB", m);
            bi.dyn       = b.addBool (id + "dyn", bandName (n, band, "Dynamic", set), bd.dynamic, m);
            bi.sc        = b.addChoice (id + "sc", bandName (n, band, "Sidechain", set), sidechains, (int) bd.sidechain, m);
        }
    }
}

void addMatchEq (Builder& b, MeqIdx& x)
{
    const auto type = ModuleType::MatchEq;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const MatchEqParams d;

    x.phase   = b.addChoice (p + ".phase", n + " Phase", { "Linear", "Minimum" }, (int) d.phase, m);
    x.firSize = b.addChoice (p + ".firSize", n + " FIR Size", { "4096", "8192", "16384", "32768" }, d.firSizeIndex, m);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& sd = d.sets[set];
        const std::string id = p + "." + setLetter (set) + ".";
        const std::string sx = setSuffix (set);
        auto& si = x.sets[set];
        si.amount = b.addFloat (id + "amount", n + " Amount" + sx, 0.0f, 100.0f, sd.amountPercent, "%", m);
        si.smooth = b.addFloat (id + "smooth", n + " Smoothing" + sx, 0.0f, 100.0f, sd.smoothPercent, "%", m);
        si.lo     = b.addLog (id + "lo", n + " Low Cutoff" + sx, 20.0f, 20000.0f, sd.lowHz, "Hz", m);
        si.hi     = b.addLog (id + "hi", n + " High Cutoff" + sx, 20.0f, 20000.0f, sd.highHz, "Hz", m);
        si.boost  = b.addFloat (id + "boost", n + " Limit Boost" + sx, 0.0f, 24.0f, sd.maxBoostDb, "dB", m);
        si.cut    = b.addFloat (id + "cut", n + " Limit Cut" + sx, 0.0f, 24.0f, sd.maxCutDb, "dB", m);
    }
}

const std::vector<std::string>& toneTargetLabels()
{
    static const std::vector<std::string> labels {
        "Balanced", "Cinematic", "Country", "EDM", "Folk", "Hip-Hop", "Jazz", "Pop", "Reggae", "RnB and Soul", "Rock",
        "Alt Rock", "Ambient", "Classic Hip-Hop", "Classic Rock", "Dance Pop", "Drum and Bass", "Dubstep", "Electropop",
        "Future Bass", "House", "Hyperpop", "Indie Pop", "Indie Rock", "K-Pop and J-Pop", "Latin Pop", "Lo-Fi", "Metal",
        "Pop Country", "Post Rock", "Punk Rock", "Reggaeton", "Techno", "Trap", "Vocal Jazz", "Flat (pink)"
    };
    return labels;
}

void addToneBalance (Builder& b, TbalIdx& x)
{
    const auto type = ModuleType::ToneBalance;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const ToneBalanceParams d;

    x.target = b.addChoice (p + ".target", n + " Target", toneTargetLabels(), (int) d.target, m);
    x.mode   = b.addChoice (p + ".mode", n + " Mode", { "Shape", "Cut", "Boost" }, (int) d.mode, m);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& sd = d.sets[set];
        const std::string id = p + "." + setLetter (set) + ".";
        const std::string sx = setSuffix (set);
        auto& si = x.sets[set];
        si.amount   = b.addFloat (id + "amount", n + " Amount" + sx, 0.0f, 100.0f, sd.amountPercent, "%", m);
        si.speed    = b.addFloat (id + "speed", n + " Speed" + sx, 0.0f, 100.0f, sd.speed, "%", m);
        si.smooth   = b.addFloat (id + "smooth", n + " Smoothing" + sx, 0.0f, 100.0f, sd.smoothing, "%", m);
        si.cutSpeed = b.addFloat (id + "cutSpeed", n + " Cut Speed" + sx, 0.0f, 100.0f, sd.cutSpeed, "%", m);
        si.sens     = b.addFloat (id + "sens", n + " Sensitivity" + sx, 0.0f, 100.0f, sd.sensitivity, "%", m);
        si.tame     = b.addBool (id + "tame", n + " Tame Transients" + sx, sd.tameTransients, m);
        si.low      = b.addFloat (id + "low", n + " Low Amount" + sx, 0.0f, 100.0f, sd.lowPercent, "%", m);
        si.mid      = b.addFloat (id + "mid", n + " Mid Amount" + sx, 0.0f, 100.0f, sd.midPercent, "%", m);
        si.high     = b.addFloat (id + "high", n + " High Amount" + sx, 0.0f, 100.0f, sd.highPercent, "%", m);
    }
}

void addDefinition (Builder& b, DefnIdx& x)
{
    const auto type = ModuleType::Definition;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const DefinitionParams d;

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& sd = d.sets[set];
        const std::string id = p + "." + setLetter (set) + ".";
        const std::string sx = setSuffix (set);
        auto& si = x.sets[set];
        si.amount = b.addFloat (id + "amount", n + " Amount" + sx, 0.0f, 100.0f, sd.amountPercent, "%", m);
        si.tilt   = b.addFloat (id + "tilt", n + " Tilt" + sx, -6.0f, 6.0f, sd.tiltDbPerOct, "dB/oct", m);
        si.att    = b.addLog (id + "att", n + " Attack" + sx, 5.0f, 1000.0f, sd.attackMs, "ms", m);
        si.rel    = b.addLog (id + "rel", n + " Release" + sx, 10.0f, 1000.0f, sd.releaseMs, "ms", m);
        si.low    = b.addLog (id + "low", n + " Low Cutoff" + sx, 300.0f, 20000.0f, sd.lowHz, "Hz", m);
        si.high   = b.addLog (id + "high", n + " High Cutoff" + sx, 300.0f, 20000.0f, sd.highHz, "Hz", m);
    }
}

void addSmoother (Builder& b, SmthIdx& x)
{
    const auto type = ModuleType::Smoother;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const SmootherParams d;

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& sd = d.sets[set];
        const std::string id = p + "." + setLetter (set) + ".";
        const std::string sx = setSuffix (set);
        auto& si = x.sets[set];
        si.amount = b.addFloat (id + "amount", n + " Amount" + sx, 0.0f, 100.0f, sd.amountPercent, "%", m);
        si.mode   = b.addChoice (id + "mode", n + " Mode" + sx, { "Gentle", "Medium", "Strong" }, (int) sd.mode, m);
        si.att    = b.addLog (id + "att", n + " Attack" + sx, 1.0f, 100.0f, sd.attackMs, "ms", m);
        si.rel    = b.addLog (id + "rel", n + " Release" + sx, 2.0f, 500.0f, sd.releaseMs, "ms", m);
        si.tone   = b.addFloat (id + "tone", n + " Tone" + sx, -100.0f, 100.0f, sd.tonePercent, "%", m);
        si.low    = b.addLog (id + "low", n + " Low Cutoff" + sx, 20.0f, 20000.0f, sd.lowHz, "Hz", m);
        si.high   = b.addLog (id + "high", n + " High Cutoff" + sx, 20.0f, 20000.0f, sd.highHz, "Hz", m);
    }
}

void addPunch (Builder& b, PnchIdx& x)
{
    const auto type = ModuleType::Punch;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const PunchParams d;

    addCrossovers (b, p, n, m, d.crossoverType, x.x);
    x.amount   = b.addFloat (p + ".amount", n + " Amount", 0.0f, 100.0f, d.amountPercent, "%", m);
    x.env      = b.addFloat (p + ".env", n + " Envelope", 10.0f, 1000.0f, d.envelopeMs, "ms", m, 100.0f);
    x.sync     = b.addBool (p + ".sync", n + " Sync", d.sync, m);
    x.syncNote = b.addChoice (p + ".syncNote", n + " Sync Note",
                              { "1/64T", "1/64", "1/64D", "1/32T", "1/32", "1/32D", "1/16T", "1/16", "1/16D",
                                "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2T", "1/2", "1/2D" }, d.syncNote, m);
    x.stLink   = b.addBool (p + ".stLink", n + " Stereo Link", d.stereoLink, m);
    x.autoGain = b.addBool (p + ".autoGain", n + " Auto Gain", d.autoGain, m);
    x.bandLink = b.addBool (p + ".bandLink", n + " Link Bands", d.linkBands, m);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        for (int band = 0; band < kMaxBands; ++band)
        {
            const auto& bd = d.bands[set][band];
            const auto id = bandIdPrefix (p, set, band);
            auto& bi = x.bands[set][band];
            bi.on     = b.addBool (id + "on", bandName (n, band, "On", set), bd.on, m);
            bi.amount = b.addFloat (id + "amount", bandName (n, band, "Amount", set), -100.0f, 100.0f, bd.amountPercent,
                                    "%", m);
        }
    }
}

void addLowContrast (Builder& b, LowcIdx& x)
{
    const auto type = ModuleType::LowContrast;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const LowContrastParams d;

    x.low      = b.addLog (p + ".low", n + " Low Cutoff", 20.0f, 300.0f, d.lowHz, "Hz", m);
    x.high     = b.addLog (p + ".high", n + " High Cutoff", 20.0f, 300.0f, d.highHz, "Hz", m);
    x.autoGain = b.addBool (p + ".autoGain", n + " Auto Gain", d.autoGain, m);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& sd = d.sets[set];
        const std::string id = p + "." + setLetter (set) + ".";
        const std::string sx = setSuffix (set);
        auto& si = x.sets[set];
        si.mode     = b.addChoice (id + "mode", n + " Mode" + sx, { "Punchy", "Smooth" }, (int) sd.mode, m);
        si.contrast = b.addFloat (id + "contrast", n + " Contrast" + sx, -100.0f, 100.0f, sd.contrastPercent, "%", m);
        si.gain     = b.addFloat (id + "gain", n + " Gain" + sx, -12.0f, 12.0f, sd.gainDb, "dB", m);
    }
}

void addBassShaper (Builder& b, BassIdx& x)
{
    const auto type = ModuleType::BassShaper;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const BassShaperParams d;

    x.cutoff    = b.addLog (p + ".cutoff", n + " Cutoff", 65.0f, 300.0f, d.cutoffHz, "Hz", m);
    x.balance   = b.addFloat (p + ".balance", n + " Balance", -12.0f, 12.0f, d.balanceDb, "dB", m);
    x.punch     = b.addFloat (p + ".punch", n + " Punch", -12.0f, 12.0f, d.punchDb, "dB", m);
    x.susOn     = b.addBool (p + ".sus.on", n + " Sustain On", d.sustainOn, m);
    x.susThr    = b.addFloat (p + ".sus.thr", n + " Sustain Threshold", -40.0f, 0.0f, d.sustainThresholdDb, "dB", m);
    x.susAmount = b.addFloat (p + ".sus.amount", n + " Sustain Amount", 0.0f, 100.0f, d.sustainAmountPercent, "%", m);
    x.susChar   = b.addFloat (p + ".sus.char", n + " Sustain Character", 0.0f, 100.0f, d.sustainCharacterPercent, "%", m);
    x.peakOn    = b.addBool (p + ".peak.on", n + " Peak On", d.peakOn, m);
    x.peakThr   = b.addFloat (p + ".peak.thr", n + " Peak Threshold", -30.0f, 0.0f, d.peakThresholdDb, "dB", m);
    x.peakChar  = b.addFloat (p + ".peak.char", n + " Peak Character", 0.0f, 100.0f, d.peakCharacterPercent, "%", m);
    x.peakMix   = b.addFloat (p + ".peak.mix", n + " Peak Mix", 0.0f, 100.0f, d.peakMixPercent, "%", m);
    x.autoGain  = b.addBool (p + ".autoGain", n + " Auto Gain", d.autoGain, m);
}

//==============================================================================
// Vintage appendix (phase 3, after bass.*)

// "20 Hz", "1.5 kHz", "10 kHz" (ASCII choice labels of the stepped frequency selectors).
std::string hzLabel (double hz)
{
    if (hz < 1000.0)
        return std::to_string ((int) std::lround (hz)) + " Hz";
    const long tenths = std::lround (hz / 100.0);
    return std::to_string (tenths / 10) + (tenths % 10 != 0 ? "." + std::to_string (tenths % 10) : std::string()) + " kHz";
}

template <size_t N>
std::vector<std::string> hzLabels (const std::array<double, N>& hz)
{
    std::vector<std::string> labels;
    for (double f : hz)
        labels.push_back (hzLabel (f));
    return labels;
}

void addClassicComp (Builder& b, CcmpIdx& x)
{
    const auto type = ModuleType::ClassicComp;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const ClassicCompParams d;

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& sd = d.sets[set];
        const std::string id = p + "." + setLetter (set) + ".";
        const std::string sx = setSuffix (set);
        auto& si = x.sets[set];
        si.thr       = b.addFloat (id + "thr", n + " Threshold" + sx, -40.0f, 0.0f, sd.thresholdDb, "dB", m);
        si.ratio     = b.addFloat (id + "ratio", n + " Ratio" + sx, 1.0f, 20.0f, sd.ratio, ":1", m, 4.0f);
        si.attack    = b.addLog (id + "attack", n + " Attack" + sx, 0.1f, 100.0f, sd.attackMs, "ms", m);
        si.release   = b.addLog (id + "release", n + " Release" + sx, 30.0f, 3000.0f, sd.releaseMs, "ms", m);
        si.mode      = b.addChoice (id + "mode", n + " Mode" + sx, { "Sharp", "Balanced", "Smooth" }, (int) sd.mode, m);
        si.makeup    = b.addFloat (id + "makeup", n + " Makeup" + sx, -12.0f, 24.0f, sd.makeupDb, "dB", m);
        si.autoGain  = b.addBool (id + "autoGain", n + " Auto Gain" + sx, sd.autoGain, m);
        si.scHpf     = b.addLog (id + "sc.hpf", n + " Sidechain High Pass" + sx, 20.0f, 500.0f, sd.scHighPassHz, "Hz", m);
        si.scBellHz  = b.addLog (id + "sc.bellHz", n + " Sidechain Bell Freq" + sx, 200.0f, 8000.0f, sd.scBellHz, "Hz", m);
        si.scBellDb  = b.addFloat (id + "sc.bellDb", n + " Sidechain Bell Gain" + sx, -8.0f, 8.0f, sd.scBellDb, "dB", m);
        si.scShelfHz = b.addLog (id + "sc.shelfHz", n + " Sidechain Shelf Freq" + sx, 2000.0f, 16000.0f, sd.scShelfHz,
                                 "Hz", m);
        si.scShelfDb = b.addFloat (id + "sc.shelfDb", n + " Sidechain Shelf Gain" + sx, -8.0f, 8.0f, sd.scShelfDb, "dB", m);
    }
}

void addPassiveEq (Builder& b, PeqIdx& x)
{
    const auto type = ModuleType::PassiveEq;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const PassiveEqParams d;

    x.drive = b.addFloat (p + ".drive", n + " Drive", 0.0f, 10.0f, d.drive, "", m);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& sd = d.sets[set];
        const std::string id = p + "." + setLetter (set) + ".";
        const std::string sx = setSuffix (set);
        auto& si = x.sets[set];
        si.lowHz     = b.addChoice (id + "lowHz", n + " Low Freq" + sx, hzLabels (kPassiveEqLowHz), sd.lowHzIndex, m);
        si.lowBoost  = b.addFloat (id + "lowBoost", n + " Low Boost" + sx, 0.0f, 10.0f, sd.lowBoost, "", m);
        si.lowCut    = b.addFloat (id + "lowCut", n + " Low Cut" + sx, 0.0f, 10.0f, sd.lowCut, "", m);
        si.highHz    = b.addChoice (id + "highHz", n + " High Boost Freq" + sx, hzLabels (kPassiveEqHighHz),
                                    sd.highHzIndex, m);
        si.highBoost = b.addFloat (id + "highBoost", n + " High Boost" + sx, 0.0f, 10.0f, sd.highBoost, "", m);
        si.highBw    = b.addFloat (id + "highBw", n + " High Bandwidth" + sx, 0.0f, 10.0f, sd.highBandwidth, "", m);
        si.highCutHz = b.addChoice (id + "highCutHz", n + " High Cut Freq" + sx, hzLabels (kPassiveEqHighCutHz),
                                    sd.highCutHzIndex, m);
        si.highCut   = b.addFloat (id + "highCut", n + " High Cut" + sx, 0.0f, 10.0f, sd.highCut, "", m);
        si.lmHz      = b.addChoice (id + "lmHz", n + " Low-Mid Freq" + sx, hzLabels (kPassiveEqLowMidHz),
                                    sd.lowMidHzIndex, m);
        si.lmBoost   = b.addFloat (id + "lmBoost", n + " Low-Mid Boost" + sx, 0.0f, 10.0f, sd.lowMidBoost, "", m);
        si.midHz     = b.addChoice (id + "midHz", n + " Mid Freq" + sx, hzLabels (kPassiveEqMidHz), sd.midHzIndex, m);
        si.midCut    = b.addFloat (id + "midCut", n + " Mid Cut" + sx, 0.0f, 10.0f, sd.midCut, "", m);
        si.hmHz      = b.addChoice (id + "hmHz", n + " High-Mid Freq" + sx, hzLabels (kPassiveEqHighMidHz),
                                    sd.highMidHzIndex, m);
        si.hmBoost   = b.addFloat (id + "hmBoost", n + " High-Mid Boost" + sx, 0.0f, 10.0f, sd.highMidBoost, "", m);
    }
}

void addClassicLimiter (Builder& b, ClimIdx& x)
{
    const auto type = ModuleType::ClassicLimiter;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const ClassicLimiterParams d;

    x.thr       = b.addFloat (p + ".thr", n + " Threshold", -12.0f, 0.0f, d.thresholdDb, "dB", m);
    x.ceiling   = b.addFloat (p + ".ceiling", n + " Ceiling", -12.0f, 0.0f, d.ceilingDb, "dB", m);
    x.character = b.addFloat (p + ".character", n + " Character", 0.0f, 10.0f, d.character, "", m);
    x.mode      = b.addChoice (p + ".mode", n + " Mode", { "Analog", "Tube", "Modern" }, (int) d.mode, m);
    x.truePeak  = b.addBool (p + ".truePeak", n + " True Peak", d.truePeak, m);
    x.link      = b.addBool (p + ".link", n + " Link Threshold and Ceiling", d.linkThresholdCeiling, m);
}

void addTape (Builder& b, TapeIdx& x)
{
    const auto type = ModuleType::Tape;
    const std::string p = moduleInfo (type).idPrefix;
    const std::string n = moduleInfo (type).shortName;
    const int m = (int) type;
    const TapeParams d;

    x.speed     = b.addChoice (p + ".speed", n + " Speed", { "7.5 ips", "15 ips", "30 ips" }, (int) d.speed, m);
    x.drive     = b.addFloat (p + ".drive", n + " Drive", -30.0f, 20.0f, d.driveDb, "dB", m);
    x.bias      = b.addFloat (p + ".bias", n + " Bias", -10.0f, 10.0f, d.bias, "", m);
    x.harmonics = b.addFloat (p + ".harmonics", n + " Harmonics", 0.0f, 10.0f, d.harmonics, "", m);
    x.lowEmph   = b.addFloat (p + ".lowEmph", n + " Low Emphasis", 0.0f, 10.0f, d.lowEmphasis, "", m);
    x.highEmph  = b.addFloat (p + ".highEmph", n + " High Emphasis", 0.0f, 10.0f, d.highEmphasis, "", m);
}

Table buildTable()
{
    Table t;
    t.specs.reserve (1024);
    Builder b (t);

    addGlobalIo (b, t.io);

    // The 0.1.1 table: the six Core modules with their released channel-mode lists, then Dither.
    for (int i = 0; i < kNumCoreModuleTypes; ++i)
    {
        const auto type = (ModuleType) i;
        t.choiceModes[i] = releasedChannelModes (type);
        addSlot (b, type, t.slot[i], t.choiceModes[i]);

        switch (type)
        {
            case ModuleType::Equalizer1: addEq (b, type, t.eq[0]); break;
            case ModuleType::Equalizer2: addEq (b, type, t.eq[1]); break;
            case ModuleType::Dynamics:   addDynamics (b, t.dyn); break;
            case ModuleType::Imager:     addImager (b, t.img); break;
            case ModuleType::Exciter:    addExciter (b, t.exc); break;
            case ModuleType::Maximizer:  addMaximizer (b, t.max); break;
            default: break;
        }
    }

    addDither (b, t.dith);

    // ---- phase 2 appendix, then the vintage appendix (phase 3): every module after the Core six in ModuleType order
    addCoreTransientSustain (b, t);

    for (int i = kNumCoreModuleTypes; i < kNumModuleTypes; ++i)
    {
        const auto type = (ModuleType) i;
        const auto modes = channelModesFor (type);
        t.choiceModes[i] = modes;
        addSlot (b, type, t.slot[i], modes);
        if (hasMode (modes, ChannelMode::TransientSustain))
            addTsParams (b, type, t.slot[i]);

        switch (type)
        {
            case ModuleType::DynamicEq:   addDynamicEq (b, t.deq); break;
            case ModuleType::MatchEq:     addMatchEq (b, t.meq); break;
            case ModuleType::ToneBalance: addToneBalance (b, t.tbal); break;
            case ModuleType::Definition:  addDefinition (b, t.defn); break;
            case ModuleType::Smoother:    addSmoother (b, t.smth); break;
            case ModuleType::Punch:       addPunch (b, t.pnch); break;
            case ModuleType::LowContrast: addLowContrast (b, t.lowc); break;
            case ModuleType::BassShaper:  addBassShaper (b, t.bass); break;
            case ModuleType::ClassicComp:    addClassicComp (b, t.ccmp); break;
            case ModuleType::PassiveEq:      addPassiveEq (b, t.peq); break;
            case ModuleType::ClassicLimiter: addClassicLimiter (b, t.clim); break;
            case ModuleType::Tape:           addTape (b, t.tape); break;
            default: break;
        }
    }

    for (int i = 0; i < kNumModuleTypes; ++i)
        t.channelModes[i] = channelModesFor ((ModuleType) i);

    return t;
}

const Table& table()
{
    static const Table t = buildTable();
    return t;
}

// Builds the table during static initialisation, so the first read*Params on the audio thread never constructs it.
[[maybe_unused]] const Table& kTableAtStaticInit = table();

//==============================================================================
// Realtime readers
inline bool readBool (ParamValues v, int index) noexcept { return v[index] >= 0.5f; }

inline int readInt (ParamValues v, int index, int lo, int hi) noexcept
{
    const int i = (int) std::lround (v[index]);
    return i < lo ? lo : (i > hi ? hi : i);
}

template <typename Enum>
inline Enum readEnum (ParamValues v, int index, int count) noexcept
{
    return (Enum) readInt (v, index, 0, count - 1);
}

inline float readFloat (ParamValues v, int index) noexcept { return v[index]; }

// Keeps a region ordered: both edges inside [minHz, maxHz] and hi >= lo * ratio (hi is raised; at the top of the range
// lo is lowered instead).
void orderRegion (float& lo, float& hi, float minHz, float maxHz, float ratio) noexcept
{
    lo = lo < minHz ? minHz : (lo > maxHz ? maxHz : lo);
    hi = hi < minHz ? minHz : (hi > maxHz ? maxHz : hi);
    if (hi < lo * ratio)
    {
        hi = lo * ratio;
        if (hi > maxHz)
        {
            hi = maxHz;
            lo = maxHz / ratio;
        }
    }
}

void readCrossovers (ParamValues v, const XoverIdx& x, int& numBands, float* hz, CrossoverType& type) noexcept
{
    numBands = readInt (v, x.bands, 1, kMaxBands);
    float prev = 20.0f;
    for (int i = 0; i < kMaxCrossovers; ++i)
    {
        float f = v[x.xover[i]];
        f = f < 20.0f ? 20.0f : (f > 20000.0f ? 20000.0f : f);
        if (f < prev) f = prev;
        hz[i] = f;
        prev = f;
    }
    type = readEnum<CrossoverType> (v, x.type, 3);
}
} // namespace

//==============================================================================
const std::vector<ParamSpec>& paramSpecs()
{
    return table().specs;
}

int paramIndex (const std::string& id) noexcept
{
    const auto& byId = table().byId;
    const auto it = byId.find (id);
    return it != byId.end() ? it->second : -1;
}

std::vector<ChannelMode> supportedChannelModes (ModuleType type)
{
    const int i = (int) type;
    if (i < 0 || i >= kNumModuleTypes)
        return { ChannelMode::Stereo };

    const auto& t = table().channelModes[i];
    return std::vector<ChannelMode> (t.modes, t.modes + t.count);
}

std::vector<ChannelMode> channelModeChoices (ModuleType type)
{
    const int i = (int) type;
    if (i < 0 || i >= kNumModuleTypes || table().slot[i].chMode < 0)
        return {};

    const auto& t = table().choiceModes[i];
    return std::vector<ChannelMode> (t.modes, t.modes + t.count);
}

std::vector<std::pair<std::string, float>> channelModeParamValues (ModuleType type, ChannelMode mode)
{
    std::vector<std::pair<std::string, float>> result;
    const int i = (int) type;
    if (i < 0 || i >= kNumModuleTypes || ! hasMode (table().channelModes[i], mode))
        return result;

    const auto& slot = table().slot[i];
    const auto& choices = table().choiceModes[i];
    const auto& specs = table().specs;
    if (slot.chMode < 0)
        return result;   // single-mode module (Stereo only)

    if (slot.tsOn >= 0)
        result.emplace_back (specs[(size_t) slot.tsOn].id, mode == ChannelMode::TransientSustain ? 1.0f : 0.0f);

    for (int k = 0; k < choices.count; ++k)
        if (choices.modes[k] == mode)
            result.emplace_back (specs[(size_t) slot.chMode].id, (float) k);

    return result;
}

bool supportsTransientSustain (ModuleType type) noexcept
{
    const int i = (int) type;
    return i >= 0 && i < kNumModuleTypes && hasMode (table().channelModes[i], ChannelMode::TransientSustain);
}

void readIoParams (ParamValues v, IoParams& p) noexcept
{
    const auto& io = table().io;
    for (int c = 0; c < 2; ++c)
    {
        p.inputGainDb[c] = readFloat (v, io.inGain[c]);
        p.outputGainDb[c] = readFloat (v, io.outGain[c]);
    }
    p.bypass = readBool (v, io.bypass);
    p.gainMatch = readBool (v, io.gainMatch);
    p.gainMatchModern = readBool (v, io.gainMatchModern);
    p.sumToMono = readBool (v, io.mono);
    p.swapChannels = readBool (v, io.swap);
}

void readSlotParams (ParamValues v, ModuleType type, SlotParams& p) noexcept
{
    const int i = (int) type;
    if (i < 0 || i >= kNumModuleTypes)
        return;

    const auto& t = table();
    const auto& s = t.slot[i];
    const auto& modes = t.choiceModes[i];
    p.power = readBool (v, s.on);
    p.tsSensitivity = s.tsSens >= 0 ? readFloat (v, s.tsSens) : 50.0f;
    p.tsLengthMs = s.tsLen >= 0 ? readFloat (v, s.tsLen) : 20.0f;
    if (s.chMode < 0)
    {
        p.channelMode = ChannelMode::Stereo;
        p.channelOn[0] = p.channelOn[1] = true;
        p.channelLink = true;
        return;
    }
    p.channelMode = modes.modes[readInt (v, s.chMode, 0, modes.count - 1)];
    if (s.tsOn >= 0 && readBool (v, s.tsOn))
        p.channelMode = ChannelMode::TransientSustain;
    p.channelOn[0] = readBool (v, s.setOn[0]);
    p.channelOn[1] = readBool (v, s.setOn[1]);
    p.channelLink = readBool (v, s.link);
}

void readEqParams (ParamValues v, ModuleType eq1or2, EqParams& p) noexcept
{
    const auto& e = table().eq[eq1or2 == ModuleType::Equalizer2 ? 1 : 0];
    p.filterMode = readEnum<EqFilterMode> (v, e.mode, 2);
    p.amountPercent = readFloat (v, e.amount);
    p.softSaturation = readBool (v, e.softSat);
    p.firSizeIndex = readInt (v, e.firSize, 0, 3);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        for (int band = 0; band < kEqBands; ++band)
        {
            const auto& bi = e.bands[set][band];
            auto& b = p.bands[set][band];
            b.on = readBool (v, bi.on);
            b.shape = readEnum<EqShape> (v, bi.shape, (int) EqShape::Count);
            b.freqHz = readFloat (v, bi.freq);
            b.gainDb = readFloat (v, bi.gain);
            b.q = readFloat (v, bi.q);
            b.slope = readEnum<EqSlope> (v, bi.slope, (int) EqSlope::Count);
            b.phasePercent = readFloat (v, bi.phase);
        }
    }
}

void readDynamicsParams (ParamValues v, DynamicsParams& p) noexcept
{
    const auto& d = table().dyn;
    readCrossovers (v, d.x, p.numBands, p.crossoverHz, p.crossoverType);
    p.detection = readEnum<DetectionMode> (v, d.detect, 3);
    p.detectionFilter = readEnum<DetectionFilter> (v, d.detFilter, 3);
    p.detectionFilterHz = readFloat (v, d.detFreq);
    p.detectionFilterSlopeIndex = readInt (v, d.detSlope, 0, 3);
    p.detectionTiltDb = readFloat (v, d.detTilt);
    p.lookaheadMs = readFloat (v, d.lookahead);
    p.adaptiveRelease = readBool (v, d.adaptRel);
    p.autoGain = readBool (v, d.autoGain);
    p.outputGainDb = readFloat (v, d.gain);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        for (int band = 0; band < kMaxBands; ++band)
        {
            const auto& bi = d.bands[set][band];
            auto& b = p.bands[set][band];
            b.on = readBool (v, bi.on);
            b.comp.on = readBool (v, bi.comp.on);
            b.comp.thresholdDb = readFloat (v, bi.comp.thr);
            b.comp.ratio = readFloat (v, bi.comp.ratio);
            b.comp.attackMs = readFloat (v, bi.comp.att);
            b.comp.releaseMs = readFloat (v, bi.comp.rel);
            b.comp.kneeDb = readFloat (v, bi.comp.knee);
            b.lim.on = readBool (v, bi.lim.on);
            b.lim.thresholdDb = readFloat (v, bi.lim.thr);
            b.lim.ratio = readFloat (v, bi.lim.ratio);
            b.lim.attackMs = readFloat (v, bi.lim.att);
            b.lim.releaseMs = readFloat (v, bi.lim.rel);
            b.lim.kneeDb = readFloat (v, bi.lim.knee);
            b.parallelPercent = readFloat (v, bi.mix);
            b.gainDb = readFloat (v, bi.gain);
        }
    }
}

void readImagerParams (ParamValues v, ImagerParams& p) noexcept
{
    const auto& d = table().img;
    readCrossovers (v, d.x, p.numBands, p.crossoverHz, p.crossoverType);
    for (int band = 0; band < kMaxBands; ++band)
        p.widthPercent[band] = readFloat (v, d.width[band]);
    p.amountPercent = readFloat (v, d.amount);
    p.stereoizeOn = readBool (v, d.stzOn);
    p.stereoizeMode = readEnum<StereoizeMode> (v, d.stzMode, 2);
    p.stereoizeAmount = readFloat (v, d.stzAmount);
    p.recoverSidesOn = readBool (v, d.recOn);
    p.recoverSidesGainDb = readFloat (v, d.recGain);
    p.preventAntiphase = readBool (v, d.antiphase);

    auto& b = p.setB;
    for (int band = 0; band < kMaxBands; ++band)
        b.widthPercent[band] = readFloat (v, d.setB.width[band]);
    b.stereoizeOn = readBool (v, d.setB.stzOn);
    b.stereoizeMode = readEnum<StereoizeMode> (v, d.setB.stzMode, 2);
    b.stereoizeAmount = readFloat (v, d.setB.stzAmount);
    b.recoverSidesOn = readBool (v, d.setB.recOn);
    b.recoverSidesGainDb = readFloat (v, d.setB.recGain);
}

ImagerSetParams imagerSetA (const ImagerParams& p) noexcept
{
    ImagerSetParams s;
    for (int band = 0; band < kMaxBands; ++band)
        s.widthPercent[band] = p.widthPercent[band];
    s.stereoizeOn = p.stereoizeOn;
    s.stereoizeMode = p.stereoizeMode;
    s.stereoizeAmount = p.stereoizeAmount;
    s.recoverSidesOn = p.recoverSidesOn;
    s.recoverSidesGainDb = p.recoverSidesGainDb;
    return s;
}

void useImagerSet (ImagerParams& p, const ImagerSetParams& s) noexcept
{
    for (int band = 0; band < kMaxBands; ++band)
        p.widthPercent[band] = s.widthPercent[band];
    p.stereoizeOn = s.stereoizeOn;
    p.stereoizeMode = s.stereoizeMode;
    p.stereoizeAmount = s.stereoizeAmount;
    p.recoverSidesOn = s.recoverSidesOn;
    p.recoverSidesGainDb = s.recoverSidesGainDb;
}

void readExciterParams (ParamValues v, ExciterParams& p) noexcept
{
    const auto& d = table().exc;
    readCrossovers (v, d.x, p.numBands, p.crossoverHz, p.crossoverType);
    p.oversampling = readBool (v, d.os);
    p.postFilterOn = readBool (v, d.postOn);
    p.postFilterHz = readFloat (v, d.postFreq);
    p.postFilterGainDb = readFloat (v, d.postGain);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        for (int band = 0; band < kMaxBands; ++band)
        {
            const auto& bi = d.bands[set][band];
            auto& b = p.bands[set][band];
            b.on = readBool (v, bi.on);
            b.mode = readEnum<ExciterMode> (v, bi.mode, (int) ExciterMode::Count);
            b.drivePercent = readFloat (v, bi.drive);
            b.mixPercent = readFloat (v, bi.mix);
        }
    }
}

void readMaximizerParams (ParamValues v, MaximizerParams& p) noexcept
{
    const auto& d = table().max;
    p.mode = readEnum<MaximizerMode> (v, d.mode, (int) MaximizerMode::Count);
    p.aggressiveStyle = readEnum<AggressiveStyle> (v, d.style3, 4);
    p.spectralStyle = readEnum<SpectralStyle> (v, d.style4, 3);
    p.gainDb = readFloat (v, d.gain);
    p.outputLevelDb = readFloat (v, d.ceiling);
    p.linkGainOutput = readBool (v, d.link);
    p.truePeak = readBool (v, d.truePeak);
    p.character = readFloat (v, d.character);
    p.upwardCompressDb = readFloat (v, d.upward);
    p.softClipOn = readBool (v, d.clipOn);
    p.softClipMode = readEnum<SoftClipMode> (v, d.clipMode, 3);
    p.softClipAmount = readFloat (v, d.clipAmount);
    p.transientEmphasisOn = readBool (v, d.transOn);
    p.transientEmphasis = readFloat (v, d.transAmount);
    p.stereoIndependenceTransient = readFloat (v, d.siTrans);
    p.stereoIndependenceSustain = readFloat (v, d.siSus);
    p.learnTargetLufs = readFloat (v, d.learnTarget);
}

void readDitherParams (ParamValues v, DitherParams& p) noexcept
{
    const auto& d = table().dith;
    p.on = readBool (v, d.on);
    p.bits = readEnum<DitherBits> (v, d.bits, (int) kDitherBitDepth.size());
    p.amount = readEnum<DitherAmount> (v, d.amount, 4);
    p.harmonicSuppression = readBool (v, d.harm);
    p.noiseShaping = readFloat (v, d.shape);
    p.limitPeaks = readBool (v, d.limit);
    p.autoBlank = readBool (v, d.blank);
    p.dcFilter = readBool (v, d.dc);
}

//==============================================================================
// Phase 2 readers
void readDynamicEqParams (ParamValues v, DynamicEqParams& p) noexcept
{
    const auto& d = table().deq;
    p.filterMode = readEnum<EqFilterMode> (v, d.mode, 2);
    p.firSizeIndex = readInt (v, d.firSize, 0, 3);
    p.lookaheadMs = readFloat (v, d.lookahead);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        for (int band = 0; band < kDynEqBands; ++band)
        {
            const auto& bi = d.bands[set][band];
            auto& b = p.bands[set][band];
            b.on = readBool (v, bi.on);
            b.shape = readEnum<DynEqShape> (v, bi.shape, (int) DynEqShape::Count);
            b.freqHz = readFloat (v, bi.freq);
            b.gainDb = readFloat (v, bi.gain);
            b.q = readFloat (v, bi.q);
            b.direction = readEnum<DynEqDirection> (v, bi.dir, 2);
            b.thresholdDb = readFloat (v, bi.thr);
            b.ratio = readFloat (v, bi.ratio);
            b.kneeDb = readFloat (v, bi.knee);
            b.attackMs = readFloat (v, bi.att);
            b.releaseMs = readFloat (v, bi.rel);
            b.autoScale = readBool (v, bi.autoScale);
            b.offsetDb = readFloat (v, bi.offset);
            b.dynamic = readBool (v, bi.dyn);
            b.sidechain = readEnum<DynEqSidechain> (v, bi.sc, (int) DynEqSidechain::Count);
        }
    }
}

void readMatchEqParams (ParamValues v, MatchEqParams& p) noexcept
{
    const auto& d = table().meq;
    p.phase = readEnum<MatchPhase> (v, d.phase, 2);
    p.firSizeIndex = readInt (v, d.firSize, 0, 3);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& si = d.sets[set];
        auto& s = p.sets[set];
        s.amountPercent = readFloat (v, si.amount);
        s.smoothPercent = readFloat (v, si.smooth);
        s.lowHz = readFloat (v, si.lo);
        s.highHz = readFloat (v, si.hi);
        orderRegion (s.lowHz, s.highHz, 20.0f, 20000.0f, 1.122462048f);   // 2^(1/6)
        s.maxBoostDb = readFloat (v, si.boost);
        s.maxCutDb = readFloat (v, si.cut);
    }
}

void readToneBalanceParams (ParamValues v, ToneBalanceParams& p) noexcept
{
    const auto& d = table().tbal;
    p.target = readEnum<ToneTarget> (v, d.target, (int) ToneTarget::Count);
    p.mode = readEnum<ToneBalanceMode> (v, d.mode, 3);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& si = d.sets[set];
        auto& s = p.sets[set];
        s.amountPercent = readFloat (v, si.amount);
        s.speed = readFloat (v, si.speed);
        s.smoothing = readFloat (v, si.smooth);
        s.cutSpeed = readFloat (v, si.cutSpeed);
        s.sensitivity = readFloat (v, si.sens);
        s.tameTransients = readBool (v, si.tame);
        s.lowPercent = readFloat (v, si.low);
        s.midPercent = readFloat (v, si.mid);
        s.highPercent = readFloat (v, si.high);
    }
}

void readDefinitionParams (ParamValues v, DefinitionParams& p) noexcept
{
    const auto& d = table().defn;
    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& si = d.sets[set];
        auto& s = p.sets[set];
        s.amountPercent = readFloat (v, si.amount);
        s.tiltDbPerOct = readFloat (v, si.tilt);
        s.attackMs = readFloat (v, si.att);
        s.releaseMs = readFloat (v, si.rel);
        s.lowHz = readFloat (v, si.low);
        s.highHz = readFloat (v, si.high);
        orderRegion (s.lowHz, s.highHz, 300.0f, 20000.0f, 1.259921050f);   // 2^(1/3)
    }
}

void readSmootherParams (ParamValues v, SmootherParams& p) noexcept
{
    const auto& d = table().smth;
    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& si = d.sets[set];
        auto& s = p.sets[set];
        s.amountPercent = readFloat (v, si.amount);
        s.mode = readEnum<SmootherMode> (v, si.mode, 3);
        s.attackMs = readFloat (v, si.att);
        s.releaseMs = readFloat (v, si.rel);
        s.tonePercent = readFloat (v, si.tone);
        s.lowHz = readFloat (v, si.low);
        s.highHz = readFloat (v, si.high);
        orderRegion (s.lowHz, s.highHz, 20.0f, 20000.0f, 1.122462048f);   // 2^(1/6)
    }
}

void readPunchParams (ParamValues v, PunchParams& p) noexcept
{
    const auto& d = table().pnch;
    readCrossovers (v, d.x, p.numBands, p.crossoverHz, p.crossoverType);
    p.amountPercent = readFloat (v, d.amount);
    p.envelopeMs = readFloat (v, d.env);
    p.sync = readBool (v, d.sync);
    p.syncNote = readInt (v, d.syncNote, 0, kPunchSyncNotes - 1);
    p.stereoLink = readBool (v, d.stLink);
    p.autoGain = readBool (v, d.autoGain);
    p.linkBands = readBool (v, d.bandLink);

    for (int set = 0; set < kNumChannelSets; ++set)
        for (int band = 0; band < kMaxBands; ++band)
        {
            p.bands[set][band].on = readBool (v, d.bands[set][band].on);
            p.bands[set][band].amountPercent = readFloat (v, d.bands[set][band].amount);
        }
}

void readLowContrastParams (ParamValues v, LowContrastParams& p) noexcept
{
    const auto& d = table().lowc;
    p.lowHz = readFloat (v, d.low);
    p.highHz = readFloat (v, d.high);
    orderRegion (p.lowHz, p.highHz, 20.0f, 300.0f, 1.259921050f);   // 2^(1/3)
    p.autoGain = readBool (v, d.autoGain);

    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& si = d.sets[set];
        auto& s = p.sets[set];
        s.mode = readEnum<LowContrastMode> (v, si.mode, 2);
        s.contrastPercent = readFloat (v, si.contrast);
        s.gainDb = readFloat (v, si.gain);
    }
}

void readBassShaperParams (ParamValues v, BassShaperParams& p) noexcept
{
    const auto& d = table().bass;
    p.cutoffHz = readFloat (v, d.cutoff);
    p.balanceDb = readFloat (v, d.balance);
    p.punchDb = readFloat (v, d.punch);
    p.sustainOn = readBool (v, d.susOn);
    p.sustainThresholdDb = readFloat (v, d.susThr);
    p.sustainAmountPercent = readFloat (v, d.susAmount);
    p.sustainCharacterPercent = readFloat (v, d.susChar);
    p.peakOn = readBool (v, d.peakOn);
    p.peakThresholdDb = readFloat (v, d.peakThr);
    p.peakCharacterPercent = readFloat (v, d.peakChar);
    p.peakMixPercent = readFloat (v, d.peakMix);
    p.autoGain = readBool (v, d.autoGain);
}

//==============================================================================
// Vintage readers (phase 3)
void readClassicCompParams (ParamValues v, ClassicCompParams& p) noexcept
{
    const auto& d = table().ccmp;
    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& si = d.sets[set];
        auto& s = p.sets[set];
        s.thresholdDb = readFloat (v, si.thr);
        s.ratio = readFloat (v, si.ratio);
        s.attackMs = readFloat (v, si.attack);
        s.releaseMs = readFloat (v, si.release);
        s.mode = readEnum<ClassicCompMode> (v, si.mode, (int) ClassicCompMode::Count);
        s.makeupDb = readFloat (v, si.makeup);
        s.autoGain = readBool (v, si.autoGain);
        s.scHighPassHz = readFloat (v, si.scHpf);
        s.scBellHz = readFloat (v, si.scBellHz);
        s.scBellDb = readFloat (v, si.scBellDb);
        s.scShelfHz = readFloat (v, si.scShelfHz);
        s.scShelfDb = readFloat (v, si.scShelfDb);
    }
}

void readPassiveEqParams (ParamValues v, PassiveEqParams& p) noexcept
{
    const auto& d = table().peq;
    p.drive = readFloat (v, d.drive);
    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const auto& si = d.sets[set];
        auto& s = p.sets[set];
        s.lowHzIndex = readInt (v, si.lowHz, 0, (int) kPassiveEqLowHz.size() - 1);
        s.lowBoost = readFloat (v, si.lowBoost);
        s.lowCut = readFloat (v, si.lowCut);
        s.highHzIndex = readInt (v, si.highHz, 0, (int) kPassiveEqHighHz.size() - 1);
        s.highBoost = readFloat (v, si.highBoost);
        s.highBandwidth = readFloat (v, si.highBw);
        s.highCutHzIndex = readInt (v, si.highCutHz, 0, (int) kPassiveEqHighCutHz.size() - 1);
        s.highCut = readFloat (v, si.highCut);
        s.lowMidHzIndex = readInt (v, si.lmHz, 0, (int) kPassiveEqLowMidHz.size() - 1);
        s.lowMidBoost = readFloat (v, si.lmBoost);
        s.midHzIndex = readInt (v, si.midHz, 0, (int) kPassiveEqMidHz.size() - 1);
        s.midCut = readFloat (v, si.midCut);
        s.highMidHzIndex = readInt (v, si.hmHz, 0, (int) kPassiveEqHighMidHz.size() - 1);
        s.highMidBoost = readFloat (v, si.hmBoost);
    }
}

void readClassicLimiterParams (ParamValues v, ClassicLimiterParams& p) noexcept
{
    const auto& d = table().clim;
    p.thresholdDb = readFloat (v, d.thr);
    p.ceilingDb = readFloat (v, d.ceiling);
    p.character = readFloat (v, d.character);
    p.mode = readEnum<ClassicLimiterMode> (v, d.mode, (int) ClassicLimiterMode::Count);
    p.truePeak = readBool (v, d.truePeak);
    p.linkThresholdCeiling = readBool (v, d.link);
}

void readTapeParams (ParamValues v, TapeParams& p) noexcept
{
    const auto& d = table().tape;
    p.speed = readEnum<TapeSpeed> (v, d.speed, (int) TapeSpeed::Count);
    p.driveDb = readFloat (v, d.drive);
    p.bias = readFloat (v, d.bias);
    p.harmonics = readFloat (v, d.harmonics);
    p.lowEmphasis = readFloat (v, d.lowEmph);
    p.highEmphasis = readFloat (v, d.highEmph);
}

} // namespace ps
