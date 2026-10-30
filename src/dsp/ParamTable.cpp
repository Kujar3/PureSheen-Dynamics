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
//   2. per module, in ModuleType order: slot params (<prefix>.on, .chMode, .a.on, .b.on, .link; Stereo-only modules
//      only <prefix>.on), then module params in the order of the struct members in Params.h; band params band by band
//      (set A bands 1..N, then set B)
//   3. dither              dith.*
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

struct ImgIdx
{
    XoverIdx x;
    int width[kMaxBands] { -1, -1, -1, -1 };
    int amount = -1, stzOn = -1, stzMode = -1, stzAmount = -1, recOn = -1, recGain = -1, antiphase = -1;
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
    ChannelModeTable channelModes[kNumModuleTypes];
    EqIdx eq[2];
    DynIdx dyn;
    ImgIdx img;
    ExcIdx exc;
    MaxIdx max;
    DithIdx dith;
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

ChannelModeTable channelModesFor (ModuleType type)
{
    ChannelModeTable t;
    auto add = [&t] (ChannelMode m) { t.modes[t.count++] = m; };

    switch (type)
    {
        case ModuleType::Equalizer1:
        case ModuleType::Equalizer2:
            add (ChannelMode::Stereo);
            add (ChannelMode::MidSide);
            add (ChannelMode::LeftRight);
            add (ChannelMode::TransientSustain);
            break;
        case ModuleType::Dynamics:
        case ModuleType::Exciter:
            add (ChannelMode::Stereo);
            add (ChannelMode::MidSide);
            break;
        case ModuleType::Imager:
        case ModuleType::Maximizer:
        case ModuleType::Count:
        default:
            add (ChannelMode::Stereo);
            break;
    }
    return t;
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

void addSlot (Builder& b, ModuleType type, SlotIdx& slot, ChannelModeTable& modes)
{
    const auto& info = moduleInfo (type);
    const std::string p = info.idPrefix;
    const std::string n = info.shortName;
    const int m = (int) type;

    modes = channelModesFor (type);
    std::vector<std::string> labels;
    for (int i = 0; i < modes.count; ++i)
        labels.emplace_back (channelModeLabel (modes.modes[i]));

    slot.on = b.addBool (p + ".on", n + " Power", true, m);

    // Stereo-only modules: no channel parameters (see SlotParams in Params.h).
    if (modes.count < 2)
        return;

    slot.chMode   = b.addChoice (p + ".chMode", n + " Channel Mode", std::move (labels), 0, m);
    slot.setOn[0] = b.addBool (p + ".a.on", n + " Channel On (A)", true, m);
    slot.setOn[1] = b.addBool (p + ".b.on", n + " Channel On (B)", true, m);
    slot.link     = b.addBool (p + ".link", n + " Channel Link", true, m);
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

Table buildTable()
{
    Table t;
    t.specs.reserve (512);
    Builder b (t);

    addGlobalIo (b, t.io);

    for (int i = 0; i < kNumModuleTypes; ++i)
    {
        const auto type = (ModuleType) i;
        addSlot (b, type, t.slot[i], t.channelModes[i]);

        switch (type)
        {
            case ModuleType::Equalizer1: addEq (b, type, t.eq[0]); break;
            case ModuleType::Equalizer2: addEq (b, type, t.eq[1]); break;
            case ModuleType::Dynamics:   addDynamics (b, t.dyn); break;
            case ModuleType::Imager:     addImager (b, t.img); break;
            case ModuleType::Exciter:    addExciter (b, t.exc); break;
            case ModuleType::Maximizer:  addMaximizer (b, t.max); break;
            case ModuleType::Count:
            default: break;
        }
    }

    addDither (b, t.dith);
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
    const auto& modes = t.channelModes[i];
    p.power = readBool (v, s.on);
    if (s.chMode < 0)
    {
        p.channelMode = ChannelMode::Stereo;
        p.channelOn[0] = p.channelOn[1] = true;
        p.channelLink = true;
        return;
    }
    p.channelMode = modes.modes[readInt (v, s.chMode, 0, modes.count - 1)];
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

} // namespace ps
