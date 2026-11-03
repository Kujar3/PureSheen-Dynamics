#include "TestFramework.h"

#include "dsp/Params.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Parameter table tests (dsp/ParamTable.cpp). Prefix "params.".
namespace
{
using namespace ps;

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

// Every id documented in the comments of Params.h, expanded for all prefixes, channel sets and bands.
std::vector<std::string> documentedIds()
{
    std::vector<std::string> ids {
        "in.gainL", "in.gainR", "out.gainL", "out.gainR",
        "io.bypass", "io.gainMatch", "io.gainMatchModern", "io.mono", "io.swap",
        "dith.on", "dith.bits", "dith.amount", "dith.harm", "dith.shape", "dith.limit", "dith.blank", "dith.dc"
    };

    auto add = [&ids] (const std::string& prefix, std::initializer_list<const char*> names)
    {
        for (auto* n : names)
            ids.push_back (prefix + "." + n);
    };
    auto addBands = [&ids] (const std::string& prefix, int numBands, bool sets, std::initializer_list<const char*> names)
    {
        for (int set = 0; set < (sets ? 2 : 1); ++set)
            for (int b = 1; b <= numBands; ++b)
                for (auto* n : names)
                    ids.push_back (prefix + (sets ? (set == 0 ? ".a" : ".b") : "") + ".b" + std::to_string (b) + "." + n);
    };

    for (int m = 0; m < kNumModuleTypes; ++m)
    {
        const std::string p = moduleInfo ((ModuleType) m).idPrefix;
        if (supportedChannelModes ((ModuleType) m).size() > 1)
            add (p, { "on", "chMode", "a.on", "b.on", "link" });
        else
            add (p, { "on" });   // single-mode modules
        if (supportsTransientSustain ((ModuleType) m))
            add (p, { "ts.sens", "ts.len" });
    }
    add ("exc", { "ts.on" });

    // phase 2
    add ("img", { "b.stz.on", "b.stz.mode", "b.stz.amount", "b.rec.on", "b.rec.gain" });
    for (int b = 1; b <= kMaxBands; ++b)
        ids.push_back ("img.b.b" + std::to_string (b) + ".width");

    add ("deq", { "mode", "firSize", "lookahead" });
    addBands ("deq", kDynEqBands, true, { "on", "shape", "freq", "gain", "q", "dir", "thr", "ratio", "knee", "att", "rel",
                                          "auto", "offset", "dyn", "sc" });
    add ("meq", { "phase", "firSize" });
    for (const char* set : { "a.", "b." })
    {
        const std::string sp = set;
        add ("meq", { (sp + "amount").c_str(), (sp + "smooth").c_str(), (sp + "lo").c_str(), (sp + "hi").c_str(),
                      (sp + "boost").c_str(), (sp + "cut").c_str() });
        add ("tbal", { (sp + "amount").c_str(), (sp + "speed").c_str(), (sp + "smooth").c_str(), (sp + "cutSpeed").c_str(),
                       (sp + "sens").c_str(), (sp + "tame").c_str(), (sp + "low").c_str(), (sp + "mid").c_str(),
                       (sp + "high").c_str() });
        add ("defn", { (sp + "amount").c_str(), (sp + "tilt").c_str(), (sp + "att").c_str(), (sp + "rel").c_str(),
                       (sp + "low").c_str(), (sp + "high").c_str() });
        add ("smth", { (sp + "amount").c_str(), (sp + "mode").c_str(), (sp + "att").c_str(), (sp + "rel").c_str(),
                       (sp + "tone").c_str(), (sp + "low").c_str(), (sp + "high").c_str() });
        add ("lowc", { (sp + "mode").c_str(), (sp + "contrast").c_str(), (sp + "gain").c_str() });
    }
    add ("tbal", { "target", "mode" });
    add ("pnch", { "bands", "xover1", "xover2", "xover3", "xoverType", "amount", "env", "sync", "syncNote", "stLink",
                   "autoGain", "bandLink" });
    addBands ("pnch", kMaxBands, true, { "on", "amount" });
    add ("lowc", { "low", "high", "autoGain" });
    add ("bass", { "cutoff", "balance", "punch", "sus.on", "sus.thr", "sus.amount", "sus.char", "peak.on", "peak.thr",
                   "peak.char", "peak.mix", "autoGain" });

    // vintage-style modules (phase 3)
    for (const char* set : { "a.", "b." })
    {
        const std::string sp = set;
        for (const char* n : { "thr", "ratio", "attack", "release", "mode", "makeup", "autoGain", "sc.hpf", "sc.bellHz",
                               "sc.bellDb", "sc.shelfHz", "sc.shelfDb" })
            ids.push_back ("ccmp." + sp + n);
        for (const char* n : { "lowHz", "lowBoost", "lowCut", "highHz", "highBoost", "highQ", "highCutHz", "highCut",
                               "lmHz", "lmBoost", "midHz", "midCut", "hmHz", "hmBoost" })
            ids.push_back ("peq." + sp + n);
    }
    add ("peq", { "drive" });
    add ("clim", { "thr", "ceiling", "character", "mode", "truePeak", "link" });
    add ("tape", { "speed", "drive", "bias", "harmonics", "lowEmph", "highEmph" });

    for (const char* eq : { "eq1", "eq2" })
    {
        add (eq, { "mode", "amount", "softSat", "firSize" });
        addBands (eq, kEqBands, true, { "on", "shape", "freq", "gain", "q", "slope", "phase" });
    }

    add ("dyn", { "bands", "xover1", "xover2", "xover3", "xoverType", "detect", "detFilter", "detFreq", "detSlope",
                  "detTilt", "lookahead", "adaptRel", "autoGain", "gain" });
    addBands ("dyn", kMaxBands, true, { "on", "comp.on", "comp.thr", "comp.ratio", "comp.att", "comp.rel", "comp.knee",
                                        "lim.on", "lim.thr", "lim.ratio", "lim.att", "lim.rel", "lim.knee", "mix", "gain" });

    add ("img", { "bands", "xover1", "xover2", "xover3", "xoverType", "amount", "stz.on", "stz.mode", "stz.amount",
                  "rec.on", "rec.gain", "antiphase" });
    addBands ("img", kMaxBands, false, { "width" });

    add ("exc", { "bands", "xover1", "xover2", "xover3", "xoverType", "os", "post.on", "post.freq", "post.gain" });
    addBands ("exc", kMaxBands, true, { "on", "mode", "drive", "mix" });

    add ("max", { "mode", "style3", "style4", "gain", "ceiling", "link", "truePeak", "character", "upward", "clip.on",
                  "clip.mode", "clip.amount", "trans.on", "trans.amount", "si.trans", "si.sus", "learnTarget" });
    return ids;
}

bool isPlainAscii (const std::string& s)
{
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7e)
            return false;
    return ! s.empty();
}
} // namespace

TEST_CASE ("params. ids and host names are unique and plain ASCII")
{
    std::set<std::string> ids, names;
    for (const auto& s : paramSpecs())
    {
        CHECK (ids.insert (s.id).second);
        CHECK (names.insert (s.name).second);
        CHECK (isPlainAscii (s.id));
        CHECK (isPlainAscii (s.name));
        for (const auto& c : s.choices)
            CHECK (isPlainAscii (c));
    }
    std::cout << "    " << paramSpecs().size() << " parameters\n";
    CHECK_EQ (ids.size(), paramSpecs().size());
}

TEST_CASE ("params. defaults lie within range and kinds are consistent")
{
    for (const auto& s : paramSpecs())
    {
        CHECK (s.minValue < s.maxValue);
        CHECK (s.defaultValue >= s.minValue && s.defaultValue <= s.maxValue);
        CHECK (s.module >= -1 && s.module < kNumModuleTypes);

        if (s.kind == ParamKind::Choice)
        {
            CHECK (s.choices.size() >= 1);
            CHECK_EQ (s.minValue, 0.0f);
            CHECK_EQ (s.maxValue, (float) s.choices.size() - 1.0f);
            CHECK_EQ (s.defaultValue, std::round (s.defaultValue));
        }
        else if (s.kind == ParamKind::Bool)
        {
            CHECK (s.defaultValue == 0.0f || s.defaultValue == 1.0f);
        }
        else
        {
            CHECK (s.choices.empty());
            if (s.skewCentre != 0.0f)
                CHECK (s.skewCentre > s.minValue && s.skewCentre < s.maxValue);
        }
    }
}

TEST_CASE ("params. every id documented in Params.h exists")
{
    const auto ids = documentedIds();
    int missing = 0;
    for (const auto& id : ids)
    {
        if (paramIndex (id) < 0)
        {
            ++missing;
            pstest::reportFailure (__FILE__, __LINE__, "missing id " + id);
        }
    }
    CHECK_EQ (missing, 0);
    // Nothing undocumented either: the table is exactly the documented set.
    CHECK_EQ (ids.size(), paramSpecs().size());
}

TEST_CASE ("params. order: global io first, Core modules in ModuleType order, dither, then the phase 2 appendix")
{
    const auto& specs = paramSpecs();
    REQUIRE (specs.size() > 20);
    CHECK_EQ (specs.front().id, std::string ("io.bypass"));
    const int ditherEnd = paramIndex ("dith.dc");
    REQUIRE (ditherEnd > 0);

    // 1. the 0.1.1 part: globals, the six Core modules in ModuleType order, Dither last
    int lastModule = -1;
    bool seenDither = false;
    for (int i = 0; i <= ditherEnd; ++i)
    {
        const auto& s = specs[(size_t) i];
        if (s.id.rfind ("dith.", 0) == 0)
        {
            seenDither = true;
            CHECK_EQ (s.module, -1);
            continue;
        }
        CHECK (! seenDither);
        if (s.module < 0)
        {
            CHECK_EQ (lastModule, -1);   // globals only before the first module
            continue;
        }
        CHECK (s.module < kNumCoreModuleTypes);
        CHECK (s.module >= lastModule);
        if (s.module != lastModule)
            CHECK_EQ (s.id, std::string (moduleInfo ((ModuleType) s.module).idPrefix) + ".on");
        lastModule = s.module;
    }

    // 2. the appendix: Core T/S additions (eq1, eq2, exc, img), then each phase 2 module, every block starting with
    // its ".on" and in ModuleType order. Nothing global after dither.
    const int appendixStart = ditherEnd + 1;
    CHECK_EQ (specs[(size_t) appendixStart].id, std::string ("eq1.ts.sens"));
    const int firstPhase2 = paramIndex ("deq.on");
    REQUIRE (firstPhase2 > appendixStart);
    lastModule = -1;
    for (int i = appendixStart; i < (int) specs.size(); ++i)
    {
        const auto& s = specs[(size_t) i];
        CHECK (s.module >= 0);
        if (i < firstPhase2)
        {
            CHECK (s.module < kNumCoreModuleTypes);
            CHECK (s.module >= lastModule);
            lastModule = s.module;
            continue;
        }
        if (i == firstPhase2)
            lastModule = -1;
        CHECK (s.module >= kNumCoreModuleTypes);
        CHECK (s.module >= lastModule);
        if (s.module != lastModule)
            CHECK_EQ (s.id, std::string (moduleInfo ((ModuleType) s.module).idPrefix) + ".on");
        lastModule = s.module;
    }

    // slot blocks: chMode / a.on / b.on / link follow each other (right after ".on" except for the Imager, whose
    // channel slot is appended), then the T/S split controls
    for (int m = 0; m < kNumModuleTypes; ++m)
    {
        const std::string p = moduleInfo ((ModuleType) m).idPrefix;
        const int on = paramIndex (p + ".on");
        CHECK (on >= 0);
        if (supportedChannelModes ((ModuleType) m).size() < 2)
        {
            CHECK_EQ (paramIndex (p + ".chMode"), -1);
            continue;
        }
        const int ch = paramIndex (p + ".chMode");
        if ((ModuleType) m != ModuleType::Imager)
            CHECK_EQ (ch, on + 1);
        else
            CHECK (ch > ditherEnd);
        CHECK_EQ (paramIndex (p + ".a.on"), ch + 1);
        CHECK_EQ (paramIndex (p + ".b.on"), ch + 2);
        CHECK_EQ (paramIndex (p + ".link"), ch + 3);
        if (supportsTransientSustain ((ModuleType) m))
        {
            CHECK (paramIndex (p + ".ts.sens") > ditherEnd);
            CHECK_EQ (paramIndex (p + ".ts.len"), paramIndex (p + ".ts.sens") + 1);
            if (m >= kNumCoreModuleTypes)
                CHECK_EQ (paramIndex (p + ".ts.sens"), ch + 4);
        }
        else
        {
            CHECK_EQ (paramIndex (p + ".ts.sens"), -1);
        }
    }
}

TEST_CASE ("params. paramIndex round trip")
{
    const auto& specs = paramSpecs();
    for (size_t i = 0; i < specs.size(); ++i)
        CHECK_EQ (paramIndex (specs[i].id), (int) i);
    CHECK_EQ (paramIndex ("nope"), -1);
    CHECK_EQ (paramIndex (""), -1);
    CHECK_EQ (paramIndex ("eq1.a.b9.freq"), -1);
}

TEST_CASE ("params. host names follow the naming scheme")
{
    const auto& specs = paramSpecs();
    CHECK_EQ (specs[(size_t) paramIndex ("eq1.a.b3.freq")].name, std::string ("EQ 1 Band 3 Freq (A)"));
    CHECK_EQ (specs[(size_t) paramIndex ("exc.b.b2.drive")].name, std::string ("Exciter Band 2 Drive (B)"));
    CHECK_EQ (specs[(size_t) paramIndex ("img.b4.width")].name, std::string ("Imager Band 4 Width"));
    CHECK_EQ (specs[(size_t) paramIndex ("dyn.bands")].name, std::string ("Dynamics Bands"));
    CHECK_EQ (specs[(size_t) paramIndex ("in.gainL")].name, std::string ("Input Gain L"));
    CHECK_EQ (specs[(size_t) paramIndex ("eq2.b.b8.shape")].choices[(size_t) EqShape::LowShelfBaxandall],
              std::string ("Low Shelf Baxandall"));
    CHECK_EQ (specs[(size_t) paramIndex ("eq2.b.b8.shape")].choices.size(), (size_t) EqShape::Count);
    CHECK_EQ (specs[(size_t) paramIndex ("eq1.a.b1.freq")].unit, std::string ("Hz"));

    // log ranges have their skew centre at the geometric mean
    const auto& f = specs[(size_t) paramIndex ("eq1.a.b1.freq")];
    CHECK (std::abs (f.skewCentre - std::sqrt (20.0f * 20000.0f)) < 0.01f);
}

TEST_CASE ("params. read of default values reproduces the struct defaults")
{
    const auto v = defaultValues();
    const ParamValues pv = v.data();

    {
        IoParams r, d;
        r.bypass = true; r.inputGainDb[0] = 5.0f;   // must be overwritten
        readIoParams (pv, r);
        for (int c = 0; c < 2; ++c)
        {
            CHECK_EQ (r.inputGainDb[c], d.inputGainDb[c]);
            CHECK_EQ (r.outputGainDb[c], d.outputGainDb[c]);
        }
        CHECK_EQ (r.bypass, d.bypass);
        CHECK_EQ (r.gainMatch, d.gainMatch);
        CHECK_EQ (r.gainMatchModern, d.gainMatchModern);
        CHECK_EQ (r.sumToMono, d.sumToMono);
        CHECK_EQ (r.swapChannels, d.swapChannels);
    }

    for (int m = 0; m < kNumModuleTypes; ++m)
    {
        SlotParams r, d;
        r.power = false; r.channelMode = ChannelMode::LeftRight; r.channelOn[1] = false; r.channelLink = false;
        readSlotParams (pv, (ModuleType) m, r);
        CHECK_EQ (r.power, d.power);
        CHECK_EQ (r.channelMode, d.channelMode);
        CHECK_EQ (r.channelOn[0], d.channelOn[0]);
        CHECK_EQ (r.channelOn[1], d.channelOn[1]);
        CHECK_EQ (r.channelLink, d.channelLink);
    }

    for (auto type : { ModuleType::Equalizer1, ModuleType::Equalizer2 })
    {
        EqParams r, d;
        r.filterMode = EqFilterMode::Digital; r.amountPercent = 3.0f; r.bands[1][7].on = true;
        readEqParams (pv, type, r);
        CHECK_EQ (r.filterMode, d.filterMode);
        CHECK_EQ (r.amountPercent, d.amountPercent);
        CHECK_EQ (r.softSaturation, d.softSaturation);
        CHECK_EQ (r.firSizeIndex, d.firSizeIndex);
        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kEqBands; ++b)
            {
                const auto& x = r.bands[s][b];
                const auto& y = d.bands[s][b];
                CHECK_EQ (x.on, y.on);
                CHECK_EQ (x.shape, y.shape);
                CHECK_EQ (x.freqHz, y.freqHz);
                CHECK_EQ (x.gainDb, y.gainDb);
                CHECK_EQ (x.q, y.q);
                CHECK_EQ (x.slope, y.slope);
                CHECK_EQ (x.phasePercent, y.phasePercent);
            }
    }

    {
        DynamicsParams r, d;
        r.numBands = 4; r.bands[1][3].lim.on = true; r.bands[0][0].comp.ratio = 9.0f;
        readDynamicsParams (pv, r);
        CHECK_EQ (r.numBands, d.numBands);
        for (int i = 0; i < kMaxCrossovers; ++i)
            CHECK_EQ (r.crossoverHz[i], d.crossoverHz[i]);
        CHECK_EQ (r.crossoverType, d.crossoverType);
        CHECK_EQ (r.detection, d.detection);
        CHECK_EQ (r.detectionFilter, d.detectionFilter);
        CHECK_EQ (r.detectionFilterHz, d.detectionFilterHz);
        CHECK_EQ (r.detectionFilterSlopeIndex, d.detectionFilterSlopeIndex);
        CHECK_EQ (r.detectionTiltDb, d.detectionTiltDb);
        CHECK_EQ (r.lookaheadMs, d.lookaheadMs);
        CHECK_EQ (r.adaptiveRelease, d.adaptiveRelease);
        CHECK_EQ (r.autoGain, d.autoGain);
        CHECK_EQ (r.outputGainDb, d.outputGainDb);
        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kMaxBands; ++b)
            {
                const auto& x = r.bands[s][b];
                const auto& y = d.bands[s][b];
                CHECK_EQ (x.on, y.on);
                CHECK_EQ (x.comp.on, y.comp.on);
                CHECK_EQ (x.comp.thresholdDb, y.comp.thresholdDb);
                CHECK_EQ (x.comp.ratio, y.comp.ratio);
                CHECK_EQ (x.comp.attackMs, y.comp.attackMs);
                CHECK_EQ (x.comp.releaseMs, y.comp.releaseMs);
                CHECK_EQ (x.comp.kneeDb, y.comp.kneeDb);
                CHECK_EQ (x.lim.on, y.lim.on);
                CHECK_EQ (x.lim.thresholdDb, y.lim.thresholdDb);
                CHECK_EQ (x.lim.ratio, y.lim.ratio);
                CHECK_EQ (x.lim.attackMs, y.lim.attackMs);
                CHECK_EQ (x.lim.releaseMs, y.lim.releaseMs);
                CHECK_EQ (x.lim.kneeDb, y.lim.kneeDb);
                CHECK_EQ (x.parallelPercent, y.parallelPercent);
                CHECK_EQ (x.gainDb, y.gainDb);
            }
    }

    {
        ImagerParams r, d;
        r.numBands = 3; r.widthPercent[2] = 50.0f; r.stereoizeMode = StereoizeMode::ModeI;
        readImagerParams (pv, r);
        CHECK_EQ (r.numBands, d.numBands);
        for (int i = 0; i < kMaxCrossovers; ++i)
            CHECK_EQ (r.crossoverHz[i], d.crossoverHz[i]);
        CHECK_EQ (r.crossoverType, d.crossoverType);
        for (int b = 0; b < kMaxBands; ++b)
            CHECK_EQ (r.widthPercent[b], d.widthPercent[b]);
        CHECK_EQ (r.amountPercent, d.amountPercent);
        CHECK_EQ (r.stereoizeOn, d.stereoizeOn);
        CHECK_EQ (r.stereoizeMode, d.stereoizeMode);
        CHECK_EQ (r.stereoizeAmount, d.stereoizeAmount);
        CHECK_EQ (r.recoverSidesOn, d.recoverSidesOn);
        CHECK_EQ (r.recoverSidesGainDb, d.recoverSidesGainDb);
        CHECK_EQ (r.preventAntiphase, d.preventAntiphase);
    }

    {
        ExciterParams r, d;
        r.oversampling = false; r.bands[1][2].mode = ExciterMode::Tape;
        readExciterParams (pv, r);
        CHECK_EQ (r.numBands, d.numBands);
        for (int i = 0; i < kMaxCrossovers; ++i)
            CHECK_EQ (r.crossoverHz[i], d.crossoverHz[i]);
        CHECK_EQ (r.crossoverType, d.crossoverType);
        CHECK_EQ (r.oversampling, d.oversampling);
        CHECK_EQ (r.postFilterOn, d.postFilterOn);
        CHECK_EQ (r.postFilterHz, d.postFilterHz);
        CHECK_EQ (r.postFilterGainDb, d.postFilterGainDb);
        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kMaxBands; ++b)
            {
                CHECK_EQ (r.bands[s][b].on, d.bands[s][b].on);
                CHECK_EQ (r.bands[s][b].mode, d.bands[s][b].mode);
                CHECK_EQ (r.bands[s][b].drivePercent, d.bands[s][b].drivePercent);
                CHECK_EQ (r.bands[s][b].mixPercent, d.bands[s][b].mixPercent);
            }
    }

    {
        MaximizerParams r, d;
        r.mode = MaximizerMode::Spectral; r.truePeak = false; r.outputLevelDb = -5.0f;
        readMaximizerParams (pv, r);
        CHECK_EQ (r.mode, d.mode);
        CHECK_EQ (r.aggressiveStyle, d.aggressiveStyle);
        CHECK_EQ (r.spectralStyle, d.spectralStyle);
        CHECK_EQ (r.gainDb, d.gainDb);
        CHECK_EQ (r.outputLevelDb, d.outputLevelDb);
        CHECK_EQ (r.linkGainOutput, d.linkGainOutput);
        CHECK_EQ (r.truePeak, d.truePeak);
        CHECK_EQ (r.character, d.character);
        CHECK_EQ (r.upwardCompressDb, d.upwardCompressDb);
        CHECK_EQ (r.softClipOn, d.softClipOn);
        CHECK_EQ (r.softClipMode, d.softClipMode);
        CHECK_EQ (r.softClipAmount, d.softClipAmount);
        CHECK_EQ (r.transientEmphasisOn, d.transientEmphasisOn);
        CHECK_EQ (r.transientEmphasis, d.transientEmphasis);
        CHECK_EQ (r.stereoIndependenceTransient, d.stereoIndependenceTransient);
        CHECK_EQ (r.stereoIndependenceSustain, d.stereoIndependenceSustain);
        CHECK_EQ (r.learnTargetLufs, d.learnTargetLufs);
    }

    {
        DitherParams r, d;
        r.on = true; r.bits = DitherBits::Bits8; r.noiseShaping = 1.0f;
        readDitherParams (pv, r);
        CHECK_EQ (r.on, d.on);
        CHECK_EQ (r.bits, d.bits);
        CHECK_EQ (r.amount, d.amount);
        CHECK_EQ (r.harmonicSuppression, d.harmonicSuppression);
        CHECK_EQ (r.noiseShaping, d.noiseShaping);
        CHECK_EQ (r.limitPeaks, d.limitPeaks);
        CHECK_EQ (r.autoBlank, d.autoBlank);
        CHECK_EQ (r.dcFilter, d.dcFilter);
    }
}

TEST_CASE ("params. read maps values to the right struct fields")
{
    auto v = defaultValues();
    setValue (v, "eq2.b.b5.gain", -7.5f);
    setValue (v, "eq2.b.b5.shape", (float) EqShape::HighShelfVintage);
    setValue (v, "eq2.mode", 1.0f);
    setValue (v, "dyn.b.b4.lim.rel", 321.0f);
    setValue (v, "dyn.bands", 3.0f);
    setValue (v, "exc.a.b3.drive", 42.0f);
    setValue (v, "img.b2.width", -60.0f);
    setValue (v, "max.mode", (float) MaximizerMode::Multiband);
    setValue (v, "dith.bits", (float) DitherBits::Bits24);
    setValue (v, "out.gainR", 3.25f);
    setValue (v, "dyn.chMode", 1.0f);
    setValue (v, "eq1.chMode", 2.0f);

    EqParams eq;
    readEqParams (v.data(), ModuleType::Equalizer2, eq);
    CHECK_EQ (eq.bands[1][4].gainDb, -7.5f);
    CHECK_EQ (eq.bands[1][4].shape, EqShape::HighShelfVintage);
    CHECK_EQ (eq.bands[0][4].gainDb, 0.0f);
    CHECK_EQ (eq.filterMode, EqFilterMode::Digital);
    EqParams eq1;
    readEqParams (v.data(), ModuleType::Equalizer1, eq1);
    CHECK_EQ (eq1.filterMode, EqFilterMode::Analog);
    CHECK_EQ (eq1.bands[1][4].gainDb, 0.0f);

    DynamicsParams dyn;
    readDynamicsParams (v.data(), dyn);
    CHECK_EQ (dyn.bands[1][3].lim.releaseMs, 321.0f);
    CHECK_EQ (dyn.numBands, 3);

    ExciterParams exc;
    readExciterParams (v.data(), exc);
    CHECK_EQ (exc.bands[0][2].drivePercent, 42.0f);

    ImagerParams img;
    readImagerParams (v.data(), img);
    CHECK_EQ (img.widthPercent[1], -60.0f);

    MaximizerParams mx;
    readMaximizerParams (v.data(), mx);
    CHECK_EQ (mx.mode, MaximizerMode::Multiband);

    DitherParams di;
    readDitherParams (v.data(), di);
    CHECK_EQ (di.bits, DitherBits::Bits24);

    IoParams io;
    readIoParams (v.data(), io);
    CHECK_EQ (io.outputGainDb[1], 3.25f);
    CHECK_EQ (io.outputGainDb[0], 0.0f);

    SlotParams slot;
    readSlotParams (v.data(), ModuleType::Dynamics, slot);
    CHECK_EQ (slot.channelMode, ChannelMode::MidSide);
    readSlotParams (v.data(), ModuleType::Equalizer1, slot);
    CHECK_EQ (slot.channelMode, ChannelMode::LeftRight);
    readSlotParams (v.data(), ModuleType::Imager, slot);
    CHECK_EQ (slot.channelMode, ChannelMode::Stereo);   // (img.chMode at its default)
}

TEST_CASE ("params. crossovers are read ascending and clamped")
{
    auto v = defaultValues();
    setValue (v, "dyn.xover1", 9000.0f);
    setValue (v, "dyn.xover2", 500.0f);
    setValue (v, "dyn.xover3", 30000.0f);
    DynamicsParams d;
    readDynamicsParams (v.data(), d);
    CHECK_EQ (d.crossoverHz[0], 9000.0f);
    CHECK_EQ (d.crossoverHz[1], 9000.0f);
    CHECK_EQ (d.crossoverHz[2], 20000.0f);
    for (int i = 1; i < kMaxCrossovers; ++i)
        CHECK (d.crossoverHz[i] >= d.crossoverHz[i - 1]);

    setValue (v, "exc.xover1", 5.0f);
    setValue (v, "exc.bands", 7.0f);
    ExciterParams e;
    readExciterParams (v.data(), e);
    CHECK_EQ (e.crossoverHz[0], 20.0f);
    CHECK_EQ (e.numBands, kMaxBands);
}

TEST_CASE ("params. supported channel modes match the chMode choices")
{
    const auto& specs = paramSpecs();
    using CM = ChannelMode;
    const std::vector<CM> all { CM::Stereo, CM::MidSide, CM::LeftRight, CM::TransientSustain };
    const std::vector<CM> msTs { CM::Stereo, CM::MidSide, CM::TransientSustain };
    const std::vector<CM> ms { CM::Stereo, CM::MidSide };
    const std::vector<CM> st { CM::Stereo };
    const std::vector<CM> stTs { CM::Stereo, CM::TransientSustain };
    CHECK (supportedChannelModes (ModuleType::Equalizer1) == all);
    CHECK (supportedChannelModes (ModuleType::Equalizer2) == all);
    CHECK (supportedChannelModes (ModuleType::Dynamics) == ms);
    CHECK (supportedChannelModes (ModuleType::Exciter) == msTs);
    CHECK (supportedChannelModes (ModuleType::Imager) == stTs);
    CHECK (supportedChannelModes (ModuleType::Maximizer) == st);
    CHECK (supportedChannelModes (ModuleType::DynamicEq) == all);
    CHECK (supportedChannelModes (ModuleType::MatchEq) == all);
    CHECK (supportedChannelModes (ModuleType::ToneBalance) == msTs);
    CHECK (supportedChannelModes (ModuleType::Definition) == msTs);
    CHECK (supportedChannelModes (ModuleType::Smoother) == msTs);
    CHECK (supportedChannelModes (ModuleType::Punch) == ms);
    CHECK (supportedChannelModes (ModuleType::LowContrast) == msTs);
    CHECK (supportedChannelModes (ModuleType::BassShaper) == st);
    CHECK (supportedChannelModes (ModuleType::ClassicComp) == ms);
    CHECK (supportedChannelModes (ModuleType::PassiveEq) == all);
    CHECK (supportedChannelModes (ModuleType::ClassicLimiter) == st);
    CHECK (supportedChannelModes (ModuleType::Tape) == st);

    // the Exciter keeps its released two-entry choice list; T/S is the appended exc.ts.on switch
    CHECK (channelModeChoices (ModuleType::Exciter) == ms);
    CHECK_EQ (specs[(size_t) paramIndex ("exc.chMode")].choices.size(), (size_t) 2);

    for (int m = 0; m < kNumModuleTypes; ++m)
    {
        const auto type = (ModuleType) m;
        const auto modes = supportedChannelModes (type);
        CHECK_EQ (modes.front(), ChannelMode::Stereo);
        CHECK_EQ (supportsTransientSustain (type),
                  std::find (modes.begin(), modes.end(), ChannelMode::TransientSustain) != modes.end());

        // every offered mode can be selected through channelModeParamValues and reads back through readSlotParams
        for (auto mode : modes)
        {
            auto v = defaultValues();
            for (const auto& [id, value] : channelModeParamValues (type, mode))
                setValue (v, id, value);
            SlotParams slot;
            readSlotParams (v.data(), type, slot);
            CHECK_EQ (slot.channelMode, mode);
        }
        if (modes.size() < 4)
            CHECK (channelModeParamValues (type, ChannelMode::LeftRight).empty()
                   || std::find (modes.begin(), modes.end(), ChannelMode::LeftRight) != modes.end());

        if (modes.size() < 2)
        {
            // Stereo-only: no channel parameters, the slot always reads Stereo with both channels on
            const std::string p = moduleInfo ((ModuleType) m).idPrefix;
            CHECK_EQ (paramIndex (p + ".chMode"), -1);
            CHECK_EQ (paramIndex (p + ".a.on"), -1);
            auto v = defaultValues();
            SlotParams slot;
            slot.channelMode = ChannelMode::MidSide; slot.channelOn[1] = false; slot.channelLink = false;
            readSlotParams (v.data(), (ModuleType) m, slot);
            CHECK_EQ (slot.channelMode, ChannelMode::Stereo);
            CHECK (slot.channelOn[0] && slot.channelOn[1] && slot.channelLink);
            continue;
        }
        const auto& s = specs[(size_t) paramIndex (std::string (moduleInfo ((ModuleType) m).idPrefix) + ".chMode")];
        const auto choices = channelModeChoices (type);
        CHECK_EQ (s.choices.size(), choices.size());

        auto v = defaultValues();
        for (size_t i = 0; i < choices.size(); ++i)
        {
            v[(size_t) paramIndex (s.id)] = (float) i;
            SlotParams slot;
            readSlotParams (v.data(), (ModuleType) m, slot);
            CHECK_EQ (slot.channelMode, choices[i]);
        }
    }

    // exc.ts.on overrides exc.chMode
    auto v = defaultValues();
    setValue (v, "exc.chMode", 1.0f);
    setValue (v, "exc.ts.on", 1.0f);
    SlotParams slot;
    readSlotParams (v.data(), ModuleType::Exciter, slot);
    CHECK_EQ (slot.channelMode, ChannelMode::TransientSustain);
    setValue (v, "exc.ts.sens", 80.0f);
    setValue (v, "exc.ts.len", 40.0f);
    readSlotParams (v.data(), ModuleType::Exciter, slot);
    CHECK_EQ (slot.tsSensitivity, 80.0f);
    CHECK_EQ (slot.tsLengthMs, 40.0f);
    readSlotParams (v.data(), ModuleType::Maximizer, slot);   // no T/S: defaults
    CHECK_EQ (slot.tsSensitivity, 50.0f);
    CHECK_EQ (slot.tsLengthMs, 20.0f);
}

TEST_CASE ("params. phase 2 defaults reproduce the struct defaults and are neutral")
{
    const auto v = defaultValues();
    const ParamValues pv = v.data();

    {
        DynamicEqParams r, d;
        r.filterMode = EqFilterMode::Digital; r.bands[1][5].on = true; r.bands[0][0].ratio = 9.0f;
        readDynamicEqParams (pv, r);
        CHECK_EQ (r.filterMode, d.filterMode);
        CHECK_EQ (r.firSizeIndex, d.firSizeIndex);
        CHECK_EQ (r.lookaheadMs, d.lookaheadMs);
        const float freqs[kDynEqBands] { 60.0f, 200.0f, 500.0f, 1500.0f, 4000.0f, 10000.0f };
        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kDynEqBands; ++b)
            {
                const auto& x = r.bands[s][b];
                const auto& y = d.bands[s][b];
                CHECK (! x.on);
                CHECK_EQ (x.shape, y.shape);
                CHECK_EQ (x.freqHz, freqs[b]);
                CHECK_EQ (x.gainDb, 0.0f);
                CHECK_EQ (x.q, y.q);
                CHECK_EQ (x.direction, y.direction);
                CHECK_EQ (x.thresholdDb, y.thresholdDb);
                CHECK_EQ (x.ratio, y.ratio);
                CHECK_EQ (x.kneeDb, y.kneeDb);
                CHECK_EQ (x.attackMs, y.attackMs);
                CHECK_EQ (x.releaseMs, y.releaseMs);
                CHECK_EQ (x.autoScale, y.autoScale);
                CHECK_EQ (x.offsetDb, y.offsetDb);
                CHECK_EQ (x.dynamic, y.dynamic);
                CHECK_EQ (x.sidechain, y.sidechain);
            }
    }
    {
        MatchEqParams r, d;
        r.sets[1].amountPercent = 3.0f;
        readMatchEqParams (pv, r);
        CHECK_EQ (r.phase, d.phase);
        CHECK_EQ (r.firSizeIndex, d.firSizeIndex);
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            CHECK_EQ (r.sets[s].amountPercent, d.sets[s].amountPercent);
            CHECK_EQ (r.sets[s].smoothPercent, d.sets[s].smoothPercent);
            CHECK_EQ (r.sets[s].lowHz, d.sets[s].lowHz);
            CHECK_EQ (r.sets[s].highHz, d.sets[s].highHz);
            CHECK_EQ (r.sets[s].maxBoostDb, d.sets[s].maxBoostDb);
            CHECK_EQ (r.sets[s].maxCutDb, d.sets[s].maxCutDb);
        }
    }
    {
        ToneBalanceParams r, d;
        r.target = ToneTarget::Trap; r.sets[0].amountPercent = 40.0f;
        readToneBalanceParams (pv, r);
        CHECK_EQ (r.target, d.target);
        CHECK_EQ (r.mode, d.mode);
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            CHECK_EQ (r.sets[s].amountPercent, 0.0f);   // neutral
            CHECK_EQ (r.sets[s].speed, d.sets[s].speed);
            CHECK_EQ (r.sets[s].smoothing, d.sets[s].smoothing);
            CHECK_EQ (r.sets[s].cutSpeed, d.sets[s].cutSpeed);
            CHECK_EQ (r.sets[s].sensitivity, d.sets[s].sensitivity);
            CHECK_EQ (r.sets[s].tameTransients, d.sets[s].tameTransients);
            CHECK_EQ (r.sets[s].lowPercent, d.sets[s].lowPercent);
            CHECK_EQ (r.sets[s].midPercent, d.sets[s].midPercent);
            CHECK_EQ (r.sets[s].highPercent, d.sets[s].highPercent);
        }
        CHECK_EQ (paramSpecs()[(size_t) paramIndex ("tbal.target")].choices.size(), (size_t) ToneTarget::Count);
    }
    {
        DefinitionParams r, d;
        r.sets[1].tiltDbPerOct = 2.0f;
        readDefinitionParams (pv, r);
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            CHECK_EQ (r.sets[s].amountPercent, 0.0f);
            CHECK_EQ (r.sets[s].tiltDbPerOct, d.sets[s].tiltDbPerOct);
            CHECK_EQ (r.sets[s].attackMs, d.sets[s].attackMs);
            CHECK_EQ (r.sets[s].releaseMs, d.sets[s].releaseMs);
            CHECK_EQ (r.sets[s].lowHz, d.sets[s].lowHz);
            CHECK_EQ (r.sets[s].highHz, d.sets[s].highHz);
        }
    }
    {
        SmootherParams r, d;
        r.sets[0].mode = SmootherMode::Strong;
        readSmootherParams (pv, r);
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            CHECK_EQ (r.sets[s].amountPercent, 0.0f);
            CHECK_EQ (r.sets[s].mode, d.sets[s].mode);
            CHECK_EQ (r.sets[s].attackMs, d.sets[s].attackMs);
            CHECK_EQ (r.sets[s].releaseMs, d.sets[s].releaseMs);
            CHECK_EQ (r.sets[s].tonePercent, d.sets[s].tonePercent);
            CHECK_EQ (r.sets[s].lowHz, d.sets[s].lowHz);
            CHECK_EQ (r.sets[s].highHz, d.sets[s].highHz);
        }
    }
    {
        PunchParams r, d;
        r.numBands = 4; r.bands[1][3].amountPercent = 10.0f;
        readPunchParams (pv, r);
        CHECK_EQ (r.numBands, 1);   // neutral: no split
        for (int i = 0; i < kMaxCrossovers; ++i)
            CHECK_EQ (r.crossoverHz[i], d.crossoverHz[i]);
        CHECK_EQ (r.crossoverType, d.crossoverType);
        CHECK_EQ (r.amountPercent, d.amountPercent);
        CHECK_EQ (r.envelopeMs, d.envelopeMs);
        CHECK_EQ (r.sync, d.sync);
        CHECK_EQ (r.syncNote, d.syncNote);
        CHECK_EQ (r.stereoLink, d.stereoLink);
        CHECK_EQ (r.autoGain, d.autoGain);
        CHECK_EQ (r.linkBands, d.linkBands);
        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kMaxBands; ++b)
            {
                CHECK_EQ (r.bands[s][b].on, d.bands[s][b].on);
                CHECK_EQ (r.bands[s][b].amountPercent, 0.0f);
            }
        CHECK_EQ (paramSpecs()[(size_t) paramIndex ("pnch.syncNote")].choices.size(), (size_t) kPunchSyncNotes);
        CHECK_EQ (paramSpecs()[(size_t) paramIndex ("pnch.syncNote")].choices[(size_t) d.syncNote], std::string ("1/16"));
    }
    {
        LowContrastParams r, d;
        r.autoGain = true; r.sets[1].contrastPercent = 50.0f;
        readLowContrastParams (pv, r);
        CHECK_EQ (r.lowHz, d.lowHz);
        CHECK_EQ (r.highHz, d.highHz);
        CHECK_EQ (r.autoGain, d.autoGain);
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            CHECK_EQ (r.sets[s].mode, d.sets[s].mode);
            CHECK_EQ (r.sets[s].contrastPercent, 0.0f);
            CHECK_EQ (r.sets[s].gainDb, 0.0f);
        }
    }
    {
        BassShaperParams r, d;
        r.peakOn = true; r.balanceDb = 3.0f;
        readBassShaperParams (pv, r);
        CHECK_EQ (r.cutoffHz, d.cutoffHz);
        CHECK_EQ (r.balanceDb, 0.0f);
        CHECK_EQ (r.punchDb, 0.0f);
        CHECK (! r.sustainOn && ! r.peakOn && ! r.autoGain);
        CHECK_EQ (r.sustainThresholdDb, d.sustainThresholdDb);
        CHECK_EQ (r.sustainAmountPercent, d.sustainAmountPercent);
        CHECK_EQ (r.sustainCharacterPercent, d.sustainCharacterPercent);
        CHECK_EQ (r.peakThresholdDb, d.peakThresholdDb);
        CHECK_EQ (r.peakCharacterPercent, d.peakCharacterPercent);
        CHECK_EQ (r.peakMixPercent, d.peakMixPercent);
    }
    {
        ImagerParams r;
        r.setB.widthPercent[3] = 40.0f; r.setB.stereoizeOn = true;
        readImagerParams (pv, r);
        const ImagerSetParams d;
        for (int b = 0; b < kMaxBands; ++b)
            CHECK_EQ (r.setB.widthPercent[b], d.widthPercent[b]);
        CHECK_EQ (r.setB.stereoizeOn, d.stereoizeOn);
        CHECK_EQ (r.setB.stereoizeMode, d.stereoizeMode);
        CHECK_EQ (r.setB.stereoizeAmount, d.stereoizeAmount);
        CHECK_EQ (r.setB.recoverSidesOn, d.recoverSidesOn);
        CHECK_EQ (r.setB.recoverSidesGainDb, d.recoverSidesGainDb);
    }
}

TEST_CASE ("params. phase 2 reads map to the right fields and keep regions ordered")
{
    auto v = defaultValues();
    setValue (v, "deq.b.b6.thr", -77.0f);
    setValue (v, "deq.b.b6.sc", (float) DynEqSidechain::OtherChannel);
    setValue (v, "deq.a.b2.shape", (float) DynEqShape::HighShelfAnalog);
    setValue (v, "meq.b.amount", 12.5f);
    setValue (v, "meq.a.lo", 5000.0f);
    setValue (v, "meq.a.hi", 1000.0f);        // below lo: raised to lo * 2^(1/6)
    setValue (v, "tbal.b.high", 33.0f);
    setValue (v, "tbal.target", (float) ToneTarget::Flat);
    setValue (v, "defn.a.low", 20000.0f);     // at the top: low lowered
    setValue (v, "smth.b.tone", -40.0f);
    setValue (v, "pnch.b.b3.amount", -55.0f);
    setValue (v, "pnch.syncNote", 99.0f);     // clamped
    setValue (v, "lowc.b.gain", 4.0f);
    setValue (v, "lowc.low", 200.0f);
    setValue (v, "lowc.high", 100.0f);
    setValue (v, "bass.peak.mix", 25.0f);
    setValue (v, "img.b.b2.width", 70.0f);
    setValue (v, "img.chMode", 1.0f);

    DynamicEqParams deq;
    readDynamicEqParams (v.data(), deq);
    CHECK_EQ (deq.bands[1][5].thresholdDb, -77.0f);
    CHECK_EQ (deq.bands[1][5].sidechain, DynEqSidechain::OtherChannel);
    CHECK_EQ (deq.bands[0][1].shape, DynEqShape::HighShelfAnalog);
    CHECK_EQ (deq.bands[0][5].thresholdDb, -30.0f);
    CHECK_EQ (dynEqToEqShape (deq.bands[0][1].shape), EqShape::HighShelfAnalog);

    MatchEqParams meq;
    readMatchEqParams (v.data(), meq);
    CHECK_EQ (meq.sets[1].amountPercent, 12.5f);
    CHECK_EQ (meq.sets[0].lowHz, 5000.0f);
    CHECK (std::abs (meq.sets[0].highHz - 5000.0f * std::pow (2.0f, 1.0f / 6.0f)) < 0.5f);

    ToneBalanceParams tb;
    readToneBalanceParams (v.data(), tb);
    CHECK_EQ (tb.sets[1].highPercent, 33.0f);
    CHECK_EQ (tb.target, ToneTarget::Flat);

    DefinitionParams de;
    readDefinitionParams (v.data(), de);
    CHECK_EQ (de.sets[0].highHz, 20000.0f);
    CHECK (std::abs (de.sets[0].lowHz - 20000.0f / std::pow (2.0f, 1.0f / 3.0f)) < 1.0f);

    SmootherParams sm;
    readSmootherParams (v.data(), sm);
    CHECK_EQ (sm.sets[1].tonePercent, -40.0f);
    CHECK_EQ (sm.sets[0].tonePercent, 0.0f);

    PunchParams pu;
    readPunchParams (v.data(), pu);
    CHECK_EQ (pu.bands[1][2].amountPercent, -55.0f);
    CHECK_EQ (pu.syncNote, kPunchSyncNotes - 1);

    LowContrastParams lc;
    readLowContrastParams (v.data(), lc);
    CHECK_EQ (lc.sets[1].gainDb, 4.0f);
    CHECK_EQ (lc.lowHz, 200.0f);
    CHECK (lc.highHz >= 200.0f * std::pow (2.0f, 1.0f / 3.0f) - 0.01f);

    BassShaperParams bs;
    readBassShaperParams (v.data(), bs);
    CHECK_EQ (bs.peakMixPercent, 25.0f);

    ImagerParams img;
    readImagerParams (v.data(), img);
    CHECK_EQ (img.setB.widthPercent[1], 70.0f);
    CHECK_EQ (img.widthPercent[1], 0.0f);
    useImagerSet (img, img.setB);
    CHECK_EQ (img.widthPercent[1], 70.0f);

    SlotParams slot;
    readSlotParams (v.data(), ModuleType::Imager, slot);
    CHECK_EQ (slot.channelMode, ChannelMode::TransientSustain);
}

TEST_CASE ("params. vintage defaults reproduce the struct defaults")
{
    const auto v = defaultValues();
    const ParamValues pv = v.data();

    {
        ClassicCompParams r, d;
        r.sets[1].ratio = 9.0f; r.sets[0].mode = ClassicCompMode::Smooth; r.sets[1].autoGain = false;
        readClassicCompParams (pv, r);
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            const auto& x = r.sets[s];
            const auto& y = d.sets[s];
            CHECK_EQ (x.thresholdDb, y.thresholdDb);
            CHECK_EQ (x.ratio, y.ratio);
            CHECK_EQ (x.attackMs, y.attackMs);
            CHECK_EQ (x.releaseMs, y.releaseMs);
            CHECK_EQ (x.mode, y.mode);
            CHECK_EQ (x.makeupDb, y.makeupDb);
            CHECK_EQ (x.autoGain, y.autoGain);
            CHECK_EQ (x.scHighPassHz, y.scHighPassHz);
            CHECK_EQ (x.scBellHz, y.scBellHz);
            CHECK_EQ (x.scBellDb, y.scBellDb);
            CHECK_EQ (x.scShelfHz, y.scShelfHz);
            CHECK_EQ (x.scShelfDb, y.scShelfDb);
        }
        CHECK_EQ (paramSpecs()[(size_t) paramIndex ("ccmp.a.ratio")].skewCentre, 4.0f);
        // the confirmed ranges (the same numbers as the reference's controls)
        const auto range = [] (const char* id)
        {
            const auto& sp = paramSpecs()[(size_t) paramIndex (id)];
            return std::make_pair (sp.minValue, sp.maxValue);
        };
        CHECK (range ("ccmp.b.thr") == std::make_pair (-60.0f, 0.0f));
        CHECK (range ("ccmp.a.release") == std::make_pair (30.0f, 150.0f));
        CHECK (range ("ccmp.a.makeup") == std::make_pair (-30.0f, 30.0f));
        CHECK (range ("ccmp.a.sc.hpf") == std::make_pair (20.0f, 400.0f));
        CHECK (range ("ccmp.a.sc.bellHz") == std::make_pair (800.0f, 9000.0f));
        CHECK (range ("ccmp.a.sc.shelfHz") == std::make_pair (4000.0f, 12000.0f));
        CHECK (range ("tape.drive") == std::make_pair (-30.0f, 30.0f));
    }
    {
        PassiveEqParams r, d;
        r.drive = 3.0f; r.sets[1].midHzIndex = 9; r.sets[0].lowBoost = 4.0f;
        readPassiveEqParams (pv, r);
        CHECK_EQ (r.drive, 0.0f);   // neutral tube stage
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            const auto& x = r.sets[s];
            const auto& y = d.sets[s];
            CHECK_EQ (x.lowHzIndex, y.lowHzIndex);
            CHECK_EQ (x.lowBoost, 0.0f);
            CHECK_EQ (x.lowCut, 0.0f);
            CHECK_EQ (x.highHzIndex, y.highHzIndex);
            CHECK_EQ (x.highBoost, 0.0f);
            CHECK_EQ (x.highQ, 0.0f);   // broadest
            CHECK_EQ (x.highCutHzIndex, y.highCutHzIndex);
            CHECK_EQ (x.highCut, 0.0f);
            CHECK_EQ (x.lowMidHzIndex, y.lowMidHzIndex);
            CHECK_EQ (x.lowMidBoost, 0.0f);
            CHECK_EQ (x.midHzIndex, y.midHzIndex);
            CHECK_EQ (x.midCut, 0.0f);
            CHECK_EQ (x.highMidHzIndex, y.highMidHzIndex);
            CHECK_EQ (x.highMidBoost, 0.0f);
        }
        // the frequency choices list the Types.h arrays; the defaults point at 45 Hz, 8 kHz, 10 kHz, 500 Hz, 1.5 kHz, 3 kHz
        const auto& specs = paramSpecs();
        CHECK_EQ (specs[(size_t) paramIndex ("peq.a.lowHz")].choices.size(), kPassiveEqLowHz.size());
        CHECK_EQ (specs[(size_t) paramIndex ("peq.b.highHz")].choices.size(), kPassiveEqHighHz.size());
        CHECK_EQ (specs[(size_t) paramIndex ("peq.a.highCutHz")].choices.size(), kPassiveEqHighCutHz.size());
        CHECK_EQ (specs[(size_t) paramIndex ("peq.a.lmHz")].choices.size(), kPassiveEqLowMidHz.size());
        CHECK_EQ (specs[(size_t) paramIndex ("peq.a.midHz")].choices.size(), kPassiveEqMidHz.size());
        CHECK_EQ (specs[(size_t) paramIndex ("peq.a.hmHz")].choices.size(), kPassiveEqHighMidHz.size());
        const auto label = [&specs] (const char* id, int index)
        {
            return specs[(size_t) paramIndex (id)].choices[(size_t) index];
        };
        CHECK_EQ (label ("peq.a.lowHz", d.sets[0].lowHzIndex), std::string ("45 Hz"));
        CHECK_EQ (label ("peq.a.highHz", d.sets[0].highHzIndex), std::string ("8 kHz"));
        CHECK_EQ (label ("peq.a.highCutHz", d.sets[0].highCutHzIndex), std::string ("10 kHz"));
        CHECK_EQ (label ("peq.a.lmHz", d.sets[0].lowMidHzIndex), std::string ("500 Hz"));
        CHECK_EQ (label ("peq.a.midHz", d.sets[0].midHzIndex), std::string ("1.5 kHz"));
        CHECK_EQ (label ("peq.a.midHz", 4), std::string ("1 kHz"));
        CHECK_EQ (label ("peq.a.hmHz", d.sets[0].highMidHzIndex), std::string ("3 kHz"));
        // the boost / cut amounts are shown in dB, as labelled on the reference
        for (const char* amount : { "lowBoost", "lowCut", "highBoost", "highCut", "lmBoost", "midCut", "hmBoost" })
        {
            const auto& sp = specs[(size_t) paramIndex (std::string ("peq.a.") + amount)];
            CHECK_EQ (sp.unit, std::string ("dB"));
            CHECK_EQ (sp.minValue, 0.0f);
            CHECK_EQ (sp.maxValue, 10.0f);
        }
    }
    {
        ClassicLimiterParams r, d;
        r.mode = ClassicLimiterMode::Modern; r.truePeak = true; r.linkThresholdCeiling = true;
        readClassicLimiterParams (pv, r);
        CHECK_EQ (r.thresholdDb, d.thresholdDb);
        CHECK_EQ (r.ceilingDb, d.ceilingDb);
        CHECK_EQ (r.character, d.character);
        CHECK_EQ (r.mode, d.mode);
        CHECK_EQ (r.truePeak, d.truePeak);
        CHECK_EQ (r.linkThresholdCeiling, d.linkThresholdCeiling);
    }
    {
        TapeParams r, d;
        r.speed = TapeSpeed::Ips30; r.harmonics = 7.0f;
        readTapeParams (pv, r);
        CHECK_EQ (r.speed, d.speed);
        CHECK_EQ (r.driveDb, d.driveDb);
        CHECK_EQ (r.bias, d.bias);
        CHECK_EQ (r.harmonics, d.harmonics);
        CHECK_EQ (r.lowEmphasis, d.lowEmphasis);
        CHECK_EQ (r.highEmphasis, 4.0f);   // flat highs (documented)
        CHECK_EQ (paramSpecs()[(size_t) paramIndex ("tape.speed")].choices[(size_t) d.speed], std::string ("30 ips"));
    }
}

TEST_CASE ("params. vintage reads map to the right fields and clamp the stepped choices")
{
    auto v = defaultValues();
    setValue (v, "ccmp.b.sc.shelfDb", -5.5f);
    setValue (v, "ccmp.b.mode", 99.0f);       // clamped to Smooth
    setValue (v, "ccmp.a.release", 120.0f);
    setValue (v, "peq.b.midHz", 99.0f);       // clamped to 7 kHz
    setValue (v, "peq.a.hmBoost", 6.5f);
    setValue (v, "peq.drive", 2.0f);
    setValue (v, "clim.ceiling", -0.3f);
    setValue (v, "clim.mode", (float) ClassicLimiterMode::Tube);
    setValue (v, "tape.speed", 0.0f);
    setValue (v, "tape.bias", -3.0f);

    ClassicCompParams cc;
    readClassicCompParams (v.data(), cc);
    CHECK_EQ (cc.sets[1].scShelfDb, -5.5f);
    CHECK_EQ (cc.sets[0].scShelfDb, 0.0f);
    CHECK_EQ (cc.sets[1].mode, ClassicCompMode::Smooth);
    CHECK_EQ (cc.sets[0].releaseMs, 120.0f);

    PassiveEqParams pe;
    readPassiveEqParams (v.data(), pe);
    CHECK_EQ (pe.sets[1].midHzIndex, (int) kPassiveEqMidHz.size() - 1);
    CHECK_EQ (pe.sets[0].highMidBoost, 6.5f);
    CHECK_EQ (pe.sets[1].highMidBoost, 0.0f);
    CHECK_EQ (pe.drive, 2.0f);

    ClassicLimiterParams cl;
    readClassicLimiterParams (v.data(), cl);
    CHECK_EQ (cl.ceilingDb, -0.3f);
    CHECK_EQ (cl.mode, ClassicLimiterMode::Tube);

    TapeParams tp;
    readTapeParams (v.data(), tp);
    CHECK_EQ (tp.speed, TapeSpeed::Ips7_5);
    CHECK_EQ (kTapeSpeedIps[(size_t) tp.speed], 7.5);
    CHECK_EQ (tp.bias, -3.0f);

    // Passive EQ offers T/S (twin + split controls); the others do not
    CHECK (supportsTransientSustain (ModuleType::PassiveEq));
    CHECK (paramIndex ("peq.ts.sens") >= 0 && paramIndex ("peq.ts.len") >= 0);
    for (auto t : { ModuleType::ClassicComp, ModuleType::ClassicLimiter, ModuleType::Tape })
        CHECK (! supportsTransientSustain (t));
}

