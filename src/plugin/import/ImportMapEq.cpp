#include "plugin/import/ImportCommon.h"
#include "plugin/import/ImportEngine.h"
#include "dsp/DynamicEq.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Preset importer, EQ family: Equalizer 1, Equalizer 2, Dynamic EQ, Match EQ.
// Mapping conventions: out/importer/spec.md. Key comments are in our own words; the foreign strings are never written
// here (`PureSheenDev importkey "<ParamID>"` prints the template and key of a stored name).
//
// Choices made where the format does not say (all listed with their evidence in the private defaults notes; the
// "def" columns below are the one place to correct them):
// - Equalizer band shapes: indices 0..16 are confirmed with one test save per shape (writer 120000; the index is
//   stored, so it is read the same way for every writer). 17..19 (three digital-only shapes) occur only in older
//   files and are kept from where they occur.
// - Absent band frequency / Q / shape take a per-version guess of the foreign node layout. Bands that a file does not
//   switch on are inaudible at 0 dB, so these guesses only matter for bands a file enables without storing them.
// - The foreign band phase (Digital mode) is stored as -(percent / 100) and 0 % is not stored (confirmed with test
//   saves); -value x 100 % is our linear (0) .. minimum phase (100) amount.
// - Dynamic EQ: the foreign "auto" timing stores multipliers of a frequency-dependent time; our automatic curves are
//   the same (measured), applied as they are at the reference times 15 / 100 ms, so a multiplier becomes reference x
//   multiplier. Ratio, knee, sidechain and the static switch have no foreign counterpart and keep our defaults.
// - Match EQ: amount, smoothing and the processed region are imported. The stored reference spectrum (a binary child
//   of the section; the slot flagged in use) is attached to the plan; the JUCE side decodes it into our reference
//   (XmlPresetImport.cpp), so it arrives in both parts as part of the same undo step.
namespace ps::imp
{
namespace
{
// The writer version, with "absent" (0) treated as the newest writer.
int writer (const ModuleImport& m) noexcept
{
    const int v = m.pluginVersion();
    return v <= 0 ? std::numeric_limits<int>::max() : v;
}

// A stored enum index as an int; anything that is not a small non-negative number gives -1.
int indexOf (double v) noexcept
{
    return (v >= -0.5 && v < 1000.0) ? (int) std::lround (v) : -1;
}

//==============================================================================
// Shared numbering of the EQ-family band controls (the same templates in Equalizer and Dynamic EQ)
constexpr Key kBandEnable      = 0x198c4f92a2310482ull;   // band on, numbered from 1
constexpr Key kBandVisible     = 0xb50157810ac212a5ull;   // node drawn in the view (always equals band on)
constexpr Key kBandFreq        = 0xe16185810a5b2c75ull;   // band frequency in Hz
constexpr Key kBandGain        = 0xe6fa16efcbd9c742ull;   // band gain in dB
constexpr Key kBandQ           = 0x76d9fa20754977d8ull;   // band Q
constexpr Key kBandShape       = 0x831a1cda735d7b8aull;   // band filter shape (module-specific index)
constexpr Key kProcessingMode  = 0xf0abeb64a4caf959ull;   // channel-mode index (St, M/S, L/R, T/S)
constexpr Key kSetSelection    = 0x1955db007b6ac60full;   // which channel set the view edits
constexpr Key kAuditionFreq    = 0xf568c98b107adf33ull;   // last band-audition position (view)
constexpr Key kAuditionGain    = 0x6d62377ff2d2f52aull;   // last band-audition width (view)
constexpr Key kCurveRangeMax   = 0x025d7db62c0712b0ull;   // display range of the curve, top
constexpr Key kCurveRangeMin   = 0x024293b62bf0705aull;   // display range of the curve, bottom

// Band on / node drawn: the "node drawn" flag is the fallback when only it is stored (never seen; robustness).
void bandVisibleFallback (ModuleImport& m, int numBands)
{
    for (int set = 0; set < 2; ++set)
        for (int n = 1; n <= numBands; ++n)
        {
            if (! m.has (kBandEnable, n, set) && m.has (kBandVisible, n, set))
                m.set (set == 0 ? "~.a.b#.on" : "~.b.b#.on", xf::boolean (m.get (kBandVisible, n, set), m, n, set), n);
            else
                m.ignore (kBandVisible, n, set);
        }
}

//==============================================================================
// Equalizer 1 and 2 share one table: target ids use "~" (the module prefix "eq1" / "eq2").
constexpr Key kEq1SectionTag = 0x08fb5b07b5965403ull;   // main equalizer section (tag)
constexpr Key kEq1ChainName  = 0xb4ecd1805373188full;   // main equalizer (ElementID / chain name)
constexpr Key kEq2SectionTag = 0xd96fdf198e710f43ull;   // second (post) equalizer section (tag)
constexpr Key kEq2ChainName  = 0xc9ef7673ea0f7f4full;   // second (post) equalizer (ElementID / chain name)

constexpr Key kEqBandPhase     = 0x65ed77c57b8447ecull;   // band phase in Digital mode, -(percent / 100)
constexpr Key kEqFilterMode    = 0xd1c4ce22808a1df6ull;   // 0 = minimum phase (analog), 1 = linear phase (digital)
constexpr Key kEqAnalyserSmooth= 0x6ab838f9e55c8b83ull;   // spectrum analyser smoothing (view)
constexpr Key kEqMsSelection   = 0xb6004e16c48d94d3ull;   // which of mid / side the view edits
constexpr Key kEqActiveView    = 0x73ad98cdad267400ull;   // which view page is shown
constexpr Key kEqAuditionQ     = 0xa8c9fb548c3ede50ull;   // last band-audition Q (view)
constexpr Key kEqDelayRangeMax = 0x7983242fc27f16b4ull;   // display range of the group-delay view, top
constexpr Key kEqDelayRangeMin = 0x7967122fc2667d66ull;   // display range of the group-delay view, bottom
constexpr Key kEqPhaseRangeMax = 0x14ab85bd68ae1934ull;   // display range of the phase view, top
constexpr Key kEqPhaseRangeMin = 0x148f73bd68957fe6ull;   // display range of the phase view, bottom
constexpr Key kEqSpecRangeMax  = 0xb1fb136e6ec6bca6ull;   // display range of the analyser, top
constexpr Key kEqSpecRangeMin  = 0xb1e0056e6eafdd24ull;   // display range of the analyser, bottom
constexpr Key kEqRulerMax      = 0xde378e4addd52bf7ull;   // display range of the frequency ruler, top
constexpr Key kEqRulerMin      = 0xde527c4addebd519ull;   // display range of the frequency ruler, bottom

// Foreign band shape index -> our EqShape (each one has a same-named shape here). NaN (not representable): our bell
// stays, counted as not imported.
//   0 bell                  15 proportional-Q bell   16 flat-topped band
//   1 / 2   analog low / high shelf (Q shapes the corner)
//   3 / 4   resonant low-pass / high-pass            5 / 6   flat low-pass / high-pass
//   7 / 8   brickwall low-pass / high-pass           9 / 10  passive-style low / high shelf (dip past the corner)
//   11 / 12 gentle (fixed-slope) low / high shelf    13 / 14 resonant low / high shelf
//   17 surgical bell; 18 / 19 surgical low / high shelf (older files only)
double eqShape (double v, ModuleImport& m, int, int)
{
    switch (indexOf (v))
    {
        case 0:  return (double) EqShape::Bell;
        case 1:  return (double) EqShape::LowShelfAnalog;
        case 2:  return (double) EqShape::HighShelfAnalog;
        case 3:  return (double) EqShape::LowPassResonant;
        case 4:  return (double) EqShape::HighPassResonant;
        case 5:  return (double) EqShape::LowPassFlat;
        case 6:  return (double) EqShape::HighPassFlat;
        case 7:  return (double) EqShape::LowPassBrickwall;
        case 8:  return (double) EqShape::HighPassBrickwall;
        case 9:  return (double) EqShape::LowShelfVintage;
        case 10: return (double) EqShape::HighShelfVintage;
        case 11: return (double) EqShape::LowShelfBaxandall;
        case 12: return (double) EqShape::HighShelfBaxandall;
        case 13: return (double) EqShape::LowShelfResonant;
        case 14: return (double) EqShape::HighShelfResonant;
        case 15: return (double) EqShape::ProportionalQ;
        case 16: return (double) EqShape::BandShelf;
        case 17: return (double) EqShape::Surgical;
        case 18:
            m.note ("EQ: a surgical low shelf was imported as an analog low shelf");
            return (double) EqShape::LowShelfAnalog;
        case 19:
            m.note ("EQ: a surgical high shelf was imported as an analog high shelf");
            return (double) EqShape::HighShelfAnalog;
        default:
            m.note ("EQ: a band shape we do not have was left as a bell");
            return kNoValue;
    }
}

// Bands on by default: the odd bands below 110000, none from 110000 on (both sets, both equalizers).
double eqEnableDefault (const ModuleImport& m, int n, int)
{
    return (writer (m) < 110000 && n % 2 == 1) ? 1.0 : 0.0;
}

// Default shape index per band and writer era (the foreign index, mapped through eqShape). The indices come from the
// never-stored sets of each era; bands 2..6 = bell is confirmed for writer 120000 (band 2), band 1 is proven not to be
// a bell there (an explicit bell is stored), its real default is still open.
double eqShapeDefault (const ModuleImport& m, int n, int)
{
    const int v = writer (m);
    if (n >= 2 && n <= 6)
        return v < 7000 ? 15.0 : 0.0;                                  // middle bands: prop. Q (oldest), then bell
    if (v < 9000)
        return n == 1 ? 1.0 : 2.0;                                     // oldest writers: analog shelves
    if (n == 8)
        return 3.0;                                                    // top band: resonant low-pass
    if (v < 100000)
        return n == 1 ? 11.0 : 12.0;                                   // gentle shelves
    return n == 1 ? 1.0 : 2.0;                                         // analog shelves
}

// Default node frequencies. Bands 3, 5 and 7 (540 Hz, 2.7 kHz, 10 kHz) were read in the real product; the
// other bands are a plausible layout consistent with every stored value.
double eqFreqDefault (const ModuleImport&, int n, int)
{
    static constexpr double hz[8] { 50.0, 100.0, 540.0, 1200.0, 2700.0, 5200.0, 10000.0, 15000.0 };
    return (n >= 1 && n <= 8) ? hz[n - 1] : kNoValue;
}

// Default Q: 0.707, except the outer shelf bands of the newer writers (where 0.707 is stored explicitly): band 1 shows
// Q 24 in the real product (writer 120000), band 7 is a guess. Our analog shelves use at most Q 1, so 24 sounds as 1.
double eqQDefault (const ModuleImport& m, int n, int)
{
    if (writer (m) >= 100000 && n == 1)
        return 24.0;
    return (writer (m) >= 100000 && n == 7) ? 1.0 : 0.707;
}

// Band phase -(percent / 100) (Digital mode) -> our 0 (linear) .. 100 % (minimum phase). A value outside 0 .. -1 is
// clamped by set() and counted.
double eqPhase (double v, ModuleImport&, int, int) { return std::clamp (-v, -1.0, 2.0) * 100.0 + 0.0; }   // + 0: no -0

//------------------------------------------------------------------------------------------------------------------
// EQUALIZER TABLE. The "def" column is the foreign default of each control (the one place to correct them).
const std::vector<Rule>& equalizerRules()
{
    static const std::vector<Rule> rules {
        //  key               act               first count band        set A id         set B id         transform        foreign default                        what
        { kBandEnable,       Act::Map,          1, 8, Band::Same, "~.a.b#.on",     "~.b.b#.on",     xf::boolean,     byVersion (eqEnableDefault, Conf::Known), "band on" },
        { kBandShape,        Act::Map,          1, 8, Band::Same, "~.a.b#.shape",  "~.b.b#.shape",  eqShape,         byVersion (eqShapeDefault, Conf::Guess),  "band shape" },
        { kBandFreq,         Act::Map,          1, 8, Band::Same, "~.a.b#.freq",   "~.b.b#.freq",   nullptr,         byVersion (eqFreqDefault, Conf::Guess),   "band frequency" },
        { kBandGain,         Act::Map,          1, 8, Band::Same, "~.a.b#.gain",   "~.b.b#.gain",   nullptr,         known (0.0),                              "band gain" },
        { kBandQ,            Act::Map,          1, 8, Band::Same, "~.a.b#.q",      "~.b.b#.q",      nullptr,         byVersion (eqQDefault, Conf::Likely),     "band Q" },
        { kEqBandPhase,      Act::Map,          1, 8, Band::Same, "~.a.b#.phase",  "~.b.b#.phase",  eqPhase,         known (0.0),                              "band phase" },
        { kBandVisible,      Act::Hook,         1, 8, Band::Same, nullptr,         nullptr,         nullptr,         byVersion (eqEnableDefault, Conf::Known), "node drawn" },
        { kEqFilterMode,     Act::Map,          0, 0, Band::Same, "~.mode",        nullptr,         xf::roundIndex,  known (0.0),                              "filter mode" },
        { kProcessingMode,   Act::ChannelMode,  0, 0, Band::Same, nullptr,         nullptr,         nullptr,         known (0.0),                              "channel mode" },
        { kEqAnalyserSmooth, Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "analyser smoothing" },
        { kSetSelection,     Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "edited set" },
        { kEqMsSelection,    Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "edited mid / side" },
        { kEqActiveView,     Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "view page" },
        { kAuditionFreq,     Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "audition frequency" },
        { kAuditionGain,     Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "audition width" },
        { kEqAuditionQ,      Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "audition Q" },
        { kCurveRangeMax,    Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "curve range top" },
        { kCurveRangeMin,    Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "curve range bottom" },
        { kEqDelayRangeMax,  Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "delay view range top" },
        { kEqDelayRangeMin,  Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "delay view range bottom" },
        { kEqPhaseRangeMax,  Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "phase view range top" },
        { kEqPhaseRangeMin,  Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "phase view range bottom" },
        { kEqSpecRangeMax,   Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "analyser range top" },
        { kEqSpecRangeMin,   Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "analyser range bottom" },
        { kEqRulerMax,       Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "ruler range top" },
        { kEqRulerMin,       Act::Ignore,       0, 0, Band::Same, nullptr,         nullptr,         nullptr,         noDefault(),                              "ruler range bottom" },
    };
    return rules;
}

void equalizerFinish (ModuleImport& m) { bandVisibleFallback (m, kEqBands); }

//==============================================================================
// Dynamic EQ
constexpr Key kDeqSectionTag = 0xd3823b9138d42320ull;   // dynamic EQ section (tag)
constexpr Key kDeqChainName  = 0x92905bc574fc92e4ull;   // dynamic EQ (ElementID / chain name)

constexpr Key kDeqThreshold  = 0x1a0b6e613335297cull;   // band threshold in dB
constexpr Key kDeqDirection  = 0xc1442229e3b63311ull;   // 0 = move down over the threshold, 1 = move up
constexpr Key kDeqAutoTimes  = 0xb258ac1e1d11036cull;   // automatic (frequency-scaled) attack / release on
constexpr Key kDeqAttack     = 0x3663c4915542c341ull;   // attack in ms (used with automatic timing off)
constexpr Key kDeqRelease    = 0xb13197a1fb35ce5aull;   // release in ms (used with automatic timing off)
constexpr Key kDeqAttScale   = 0xed5e946509ee9304ull;   // attack multiplier (used with automatic timing on)
constexpr Key kDeqRelScale   = 0xf71de8ad0a450599ull;   // release multiplier (used with automatic timing on)
constexpr Key kDeqOffset     = 0xbcf5254e49344e96ull;   // static gain offset in dB
constexpr Key kDeqFilterMode = 0x5fe2038288055b22ull;   // 0 = analog, 1 = digital (linear phase)

// Reference times: the foreign defaults with automatic timing off (read in the real product), and the set times at
// which our automatic curves (the same as the foreign ones) apply as they are, so a multiplier scales them.
constexpr double kDeqAutoAttackMs = kDynEqAutoRefAttackMs;
constexpr double kDeqAutoReleaseMs = kDynEqAutoRefReleaseMs;

double scaledTime (double baseMs, double multiplier) noexcept
{
    const double limit = std::numeric_limits<double>::max() / baseMs;
    if (multiplier > limit)
        return std::numeric_limits<double>::max();
    if (multiplier < -limit)
        return -std::numeric_limits<double>::max();
    return baseMs * multiplier;
}

// Foreign shape index -> our DynEqShape (the first five are in the same order).
double deqShape (double v, ModuleImport& m, int, int)
{
    const int i = indexOf (v);
    if (i >= 0 && i <= (int) DynEqShape::HighShelfBaxandall)
        return (double) i;
    m.note ("Dynamic EQ: a band shape we do not have was left as a proportional-Q bell");
    return kNoValue;
}

// Bands on by default: 1, 3, 4, 6 below 110000, none from 110000 on.
double deqEnableDefault (const ModuleImport& m, int n, int)
{
    return (writer (m) < 110000 && (n == 1 || n == 3 || n == 4 || n == 6)) ? 1.0 : 0.0;
}

// Default shapes: low shelf on band 1, high shelf on band 6; bands 2..5 are proportional-Q bells below 110000 and plain
// bells from 110000 on (writer 120000: the first new band is the low shelf, the next ones bells).
double deqShapeDefault (const ModuleImport& m, int n, int)
{
    if (n == 1 || n == 6)
        return n == 1 ? 3.0 : 4.0;
    return writer (m) >= 110000 ? 0.0 : 1.0;
}

// Default Q: 0.7 on bands 2..6 from 110000 on (a new band, confirmed for writer 120000). Band 1 stores an explicit
// 0.7 in a 110000 file and band 3 one in a 9000-era file, so those are not 0.7: they keep the older guess 0.707.
double deqQDefault (const ModuleImport& m, int n, int)
{
    return (writer (m) >= 110000 && n >= 2) ? 0.7 : 0.707;
}

// Default frequencies: unknown; our own layout (the same as our parameter defaults).
double deqFreqDefault (const ModuleImport&, int n, int)
{
    static constexpr double hz[6] { 60.0, 200.0, 500.0, 1500.0, 4000.0, 10000.0 };
    return (n >= 1 && n <= 6) ? hz[n - 1] : kNoValue;
}

//------------------------------------------------------------------------------------------------------------------
// DYNAMIC EQ TABLE.
const std::vector<Rule>& dynamicEqRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band        set A id          set B id          transform       foreign default                         what
        { kBandEnable,      Act::Map,          1, 6, Band::Same, "~.a.b#.on",      "~.b.b#.on",      xf::boolean,    byVersion (deqEnableDefault, Conf::Known), "band on" },
        { kBandShape,       Act::Map,          1, 6, Band::Same, "~.a.b#.shape",   "~.b.b#.shape",   deqShape,       byVersion (deqShapeDefault, Conf::Likely), "band shape" },
        { kBandFreq,        Act::Map,          1, 6, Band::Same, "~.a.b#.freq",    "~.b.b#.freq",    nullptr,        byVersion (deqFreqDefault, Conf::Guess),   "band frequency" },
        { kBandGain,        Act::Map,          1, 6, Band::Same, "~.a.b#.gain",    "~.b.b#.gain",    nullptr,        known (0.0),                               "band gain" },
        { kBandQ,           Act::Map,          1, 6, Band::Same, "~.a.b#.q",       "~.b.b#.q",       nullptr,        byVersion (deqQDefault, Conf::Likely),     "band Q" },
        { kDeqThreshold,    Act::Map,          1, 6, Band::Same, "~.a.b#.thr",     "~.b.b#.thr",     nullptr,        known (-30.0),                             "band threshold" },
        { kDeqDirection,    Act::Map,          1, 6, Band::Same, "~.a.b#.dir",     "~.b.b#.dir",     xf::boolean,    known (0.0),                               "band direction" },
        { kDeqAutoTimes,    Act::Map,          1, 6, Band::Same, "~.a.b#.auto",    "~.b.b#.auto",    xf::boolean,    known (1.0),                               "band automatic timing" },
        { kDeqAttack,       Act::Hook,         1, 6, Band::Same, nullptr,          nullptr,          nullptr,        known (kDeqAutoAttackMs),                  "band attack" },
        { kDeqRelease,      Act::Hook,         1, 6, Band::Same, nullptr,          nullptr,          nullptr,        known (kDeqAutoReleaseMs),                 "band release" },
        { kDeqAttScale,     Act::Hook,         1, 6, Band::Same, nullptr,          nullptr,          nullptr,        likely (1.0),                              "band attack multiplier" },
        { kDeqRelScale,     Act::Hook,         1, 6, Band::Same, nullptr,          nullptr,          nullptr,        likely (1.0),                              "band release multiplier" },
        { kDeqOffset,       Act::Map,          1, 6, Band::Same, "~.a.b#.offset",  "~.b.b#.offset",  nullptr,        known (0.0),                               "band static offset" },
        { kBandVisible,     Act::Hook,         1, 6, Band::Same, nullptr,          nullptr,          nullptr,        byVersion (deqEnableDefault, Conf::Known), "node drawn" },
        { kDeqFilterMode,   Act::Map,          0, 0, Band::Same, "~.mode",         nullptr,          xf::roundIndex, known (0.0),                               "filter mode" },
        { kProcessingMode,  Act::ChannelMode,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,        known (0.0),                               "channel mode" },
        { kSetSelection,    Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,        noDefault(),                               "edited set" },
        { kAuditionFreq,    Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,        noDefault(),                               "audition frequency" },
        { kAuditionGain,    Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,        noDefault(),                               "audition width" },
        { kCurveRangeMax,   Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,        noDefault(),                               "curve range top" },
        { kCurveRangeMin,   Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,        noDefault(),                               "curve range bottom" },
    };
    return rules;
}

// Timing: with automatic timing on, the foreign attack / release controls are multipliers of a frequency-dependent
// time, which our reference time times the multiplier reproduces (the absolute ms values are then inactive state);
// with it off, the ms values are used as they are (and the multipliers are inactive).
void dynamicEqFinish (ModuleImport& m)
{
    bool scaled = false;
    for (int set = 0; set < 2; ++set)
    {
        const char* att = set == 0 ? "~.a.b#.att" : "~.b.b#.att";
        const char* rel = set == 0 ? "~.a.b#.rel" : "~.b.b#.rel";
        for (int n = 1; n <= kDynEqBands; ++n)
        {
            const bool automatic = m.getOr (kDeqAutoTimes, n, set, 1.0) >= 0.5;
            if (automatic)
            {
                scaled = scaled || m.has (kDeqAttScale, n, set) || m.has (kDeqRelScale, n, set);
                m.set (att, scaledTime (kDeqAutoAttackMs, m.getOr (kDeqAttScale, n, set, 1.0)), n);
                m.set (rel, scaledTime (kDeqAutoReleaseMs, m.getOr (kDeqRelScale, n, set, 1.0)), n);
                m.ignore (kDeqAttack, n, set);
                m.ignore (kDeqRelease, n, set);
            }
            else
            {
                m.set (att, m.getOr (kDeqAttack, n, set, kDeqAutoAttackMs), n);
                m.set (rel, m.getOr (kDeqRelease, n, set, kDeqAutoReleaseMs), n);
                m.ignore (kDeqAttScale, n, set);
                m.ignore (kDeqRelScale, n, set);
            }
        }
    }
    if (scaled)
        m.note ("Dynamic EQ: automatic attack / release multipliers applied to the frequency curves");

    bandVisibleFallback (m, kDynEqBands);
}

//==============================================================================
// Match EQ (+ its stored reference spectrum, handed on as an attachment)
constexpr Key kMeqSectionTag = 0xf77deb3e2941dec8ull;   // match EQ section (tag)
constexpr Key kMeqChainName  = 0xed65a5842e3016c6ull;   // match EQ (ElementID / chain name)

constexpr Key kMeqAmount     = 0x8b02b1d1acdaea3eull;   // matching amount in %
constexpr Key kMeqSmoothing  = 0x6ab838f9e55c8b83ull;   // curve smoothing in %
constexpr Key kMeqRegionLow  = 0xdc16d3b4ba9222ceull;   // processed region, lower edge in Hz
constexpr Key kMeqRegionHigh = 0xdbfac5b4ba79904cull;   // processed region, upper edge in Hz
constexpr Key kMeqSpectrumOn = 0x9395e559057ad155ull;   // a stored reference spectrum slot is in use, numbered from 1
constexpr Key kMeqSpectrumData = 0xce256a8918a7f18dull; // element of the binary child holding the spectrum records

//------------------------------------------------------------------------------------------------------------------
// MATCH EQ TABLE.
const std::vector<Rule>& matchEqRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band        set A id     set B id     transform  foreign default  what
        { kMeqAmount,       Act::Map,          0, 0, Band::Same, "~.a.amount", "~.b.amount", nullptr,   known (10.0),    "match amount" },
        { kMeqSmoothing,    Act::Map,          0, 0, Band::Same, "~.a.smooth", "~.b.smooth", nullptr,   known (50.0),    "match smoothing" },
        { kMeqRegionLow,    Act::Map,          0, 0, Band::Same, "~.a.lo",     "~.b.lo",     nullptr,   known (20.0),    "region low edge" },
        { kMeqRegionHigh,   Act::Map,          0, 0, Band::Same, "~.a.hi",     "~.b.hi",     nullptr,   known (20000.0), "region high edge" },
        { kProcessingMode,  Act::ChannelMode,  0, 0, Band::Same, nullptr,      nullptr,      nullptr,   known (0.0),     "channel mode" },
        { kMeqSpectrumOn,   Act::Hook,         1, 7, Band::Same, nullptr,      nullptr,      nullptr,   known (0.0),     "reference spectrum slot" },
    };
    return rules;
}

// The first slot flagged in use in each set is its reference; further slots are extra display curves. Our import
// has one shared reference for both parts: prefer set A, otherwise take set B. Matching flags share one attachment;
// differing references cannot both be represented, so report the second set's selection as unsupported.
void matchEqFinish (ModuleImport& m)
{
    int slots[2] {};
    for (int set = 0; set < 2; ++set)
        for (int n = 1; n <= 7; ++n)
        {
            if (! m.has (kMeqSpectrumOn, n, set))
                continue;
            if (slots[set] == 0 && m.peek (kMeqSpectrumOn, n, set) >= 0.5)
                slots[set] = n;
            else
                m.ignore (kMeqSpectrumOn, n, set);
        }
    const int slot = slots[0] > 0 ? slots[0] : slots[1];
    if (slot == 0)
        return;

    if (slots[0] > 0 && slots[1] > 0 && slots[0] != slots[1])
    {
        m.reject (kMeqSpectrumOn, slots[1], 1, "unsupported");
        m.note ("Match EQ: the second set selected a different reference; only the first set's can be imported");
    }

    const auto* text = m.extraBytes (kMeqSpectrumData);
    auto bytes = text != nullptr ? decodeBase64 (*text) : std::nullopt;
    if (! bytes.has_value() || bytes->empty())
    {
        for (int set = 0; set < 2; ++set)
            if (slots[set] == slot)
                m.reject (kMeqSpectrumOn, slot, set, "value");
        m.note ("Match EQ: the stored reference spectrum could not be read; capture or load a reference again");
        return;
    }
    for (int set = 0; set < 2; ++set)
        if (slots[set] == slot)
            m.get (kMeqSpectrumOn, slot, set);   // imported (the JUCE side decodes the records)
    m.attach (slot, std::move (*bytes), (slots[0] == slot ? 1 : 0) + (slots[1] == slot ? 1 : 0));
}
} // namespace

//==============================================================================
const std::vector<ModuleSpec>& eqFamilySpecs()
{
    static const std::vector<ModuleSpec> specs {
        { ModuleType::Equalizer1, kEq1SectionTag, kEq1ChainName, &equalizerRules(), nullptr, equalizerFinish },
        { ModuleType::Equalizer2, kEq2SectionTag, kEq2ChainName, &equalizerRules(), nullptr, equalizerFinish },
        { ModuleType::DynamicEq,  kDeqSectionTag, kDeqChainName, &dynamicEqRules(), nullptr, dynamicEqFinish },
        { ModuleType::MatchEq,    kMeqSectionTag, kMeqChainName, &matchEqRules(),   nullptr, matchEqFinish },
    };
    return specs;
}
} // namespace ps::imp
