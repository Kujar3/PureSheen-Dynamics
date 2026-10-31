#include "plugin/import/ImportCommon.h"

#include <algorithm>
#include <cmath>

// Preset importer, phase 2 family: Tone Balance, Definition, Smoother, Punch, Low Contrast, Bass Shaper.
// Mapping conventions: out/importer/spec.md. Key comments are in our own words; the foreign strings are never written
// here (`PureSheenDev importkey "<ParamID>"` prints the template and key of a stored name).
//
// Every table's "def" column is the FOREIGN default of the writer version (sparse files: an absent value takes it).
// Likely / Guess rows are listed in out/importer/defaults-to-confirm.md ("Phase 2 family") and by
// `PureSheenDev import --defaults`. Known rows include the defaults confirmed in the real product for writer 120000
// (Reset screenshots); none of those exact values is stored by any writer in the files, so they hold for every era.
//
// Decisions (approximations are implemented in the hooks and noted in the report):
// - The three spectral modules' Amount defaults to 0 in the foreign writers: every file whose module is in the chain
//   stores it, except presets that only move the second (sustain) set, and a stored 50 exists in sparse files.
// - Tone Balance: older writers (< 110000) have one speed control for both modes, so their speed also drives our
//   cut speed.
// - Smoother: older writers (< 110000) store an absolute threshold instead of an amount; it is converted to our
//   relative amount by a heuristic (approximate, noted).
// - Punch, Low Contrast: controls the foreign module keeps per channel set but ours shares (envelope, sync, amount,
//   region) take set A; a differing set B value is reported as not imported (with a note).
// - Bass Shaper: every section follows its power switch (on by default, confirmed); at the confirmed defaults
//   (thresholds 0 dB, sustain amount 0 %) an untouched section does nothing in either module.
namespace ps::imp
{
namespace
{
//==============================================================================
// Controls shared by several modules of this family (one key per stored spelling)
constexpr Key kAmount         = 0xba98a7152d80b949ull;   // module amount in % (per set)
constexpr Key kChannelMode    = 0x3aab520ef53d71edull;   // channel-mode index (spelling of the newer modules)
constexpr Key kViewSet        = 0x4c4855f455a0b008ull;   // which channel set the view shows (mid / side)
constexpr Key kViewPart       = 0x53dc6644b60e4224ull;   // which part the view shows (transient / sustain)
constexpr Key kPowerA         = 0x0e70d45a0d49af20ull;   // set A bypass (1 = bypassed)
constexpr Key kPowerB         = 0x867ef565a6ecab61ull;   // set B bypass (1 = bypassed)
constexpr Key kSoloA          = 0x5bee457d7236ad03ull;   // set A solo (monitoring)
constexpr Key kSoloB          = 0x32bdf69412e4df5cull;   // set B solo (monitoring)
constexpr Key kLink           = 0x6e5204262edf23c3ull;   // link the two sets' controls (view)
constexpr Key kListen         = 0x1b646b7189883206ull;   // listen to the difference (monitoring)
constexpr Key kAutoGain       = 0x0335f12bf0c70628ull;   // per-module automatic gain match
constexpr Key kRegionLow      = 0x27e10818db60cd62ull;   // action region start in Hz
constexpr Key kRegionHigh     = 0xb756d41b932976aeull;   // action region end in Hz

// The foreign set (0 = A, 1 = B) -> our id of a per-set control.
const char* setId (int set, const char* a, const char* b) { return set == 0 ? a : b; }

// A control the foreign module keeps per set while ours has one value: set A wins. A differing set B value is
// reported with a note: a stored one always (and it counts as not imported), set B's sparse default only when the
// file uses both sets (any channel mode but Stereo; in Stereo the foreign set B is inaudible).
void sharedFromSetA (ModuleImport& m, Key key, const char* id, Transform xf, const char* noteText)
{
    auto mapped = [&m, xf] (double v) { return xf != nullptr ? xf (v, m, -1, 0) : v; };

    const bool storedA = m.has (key, -1, 0);
    const double a = m.get (key, -1, 0);
    double out = kNoValue;
    if (! std::isnan (a))
    {
        out = mapped (a);
        if (std::isnan (out) && storedA)
        {
            m.reject (key, -1, 0, "value");                // cannot be represented: the foreign default instead
            const double d = m.defaultOf (key, -1, 0);
            out = std::isnan (d) ? kNoValue : mapped (d);
        }
        m.set (id, out);
    }

    const bool storedB = m.has (key, -1, 1);
    const double b = m.peek (key, -1, 1);
    if (! std::isnan (b))
    {
        const double other = mapped (b);
        if (std::isnan (other))
        {
            if (storedB)
                m.reject (key, -1, 1, "value");
        }
        else if (std::isnan (out) || std::abs (other - out) > 1.0e-3 * std::max (1.0, std::abs (out)))
        {
            if (storedB)
                m.reject (key, -1, 1, "set B");
            if (storedB || m.peek (kChannelMode, -1, 0) >= 0.5)
                m.note (noteText);
        }
        else if (storedB)
            m.get (key, -1, 1);
    }
}

// Index remaps: a foreign index outside the known list cannot be represented (NaN: not imported, our default kept).
double indexUpTo (double v, int last)
{
    const double r = std::round (v);
    if (std::abs (v - r) > 1.0e-3 || r < 0.0 || r > (double) last)
        return kNoValue;
    return r;
}

//==============================================================================
// Tone Balance
constexpr Key kTbalSection    = 0xda6529a178d531c4ull;   // tone balance section (tag and chain name are the same)
constexpr Key kTbalTarget     = 0xff5bb7742c2896c8ull;   // target curve index
constexpr Key kTbalMode       = 0x37cbecaec58e2192ull;   // correction mode (shape / cut / boost)
constexpr Key kTbalSpeed      = 0xe24130f1123560a0ull;   // speed of the shape correction (per set)
constexpr Key kTbalSmooth     = 0x8b022f5799d95e3bull;   // frequency smoothing (per set)
constexpr Key kTbalCutSpeed   = 0xc9e83af84a685d27ull;   // speed of the cut mode (per set; newer writers only)
constexpr Key kTbalSens       = 0x7959747b05f5918full;   // sensitivity of the cut mode (per set)
constexpr Key kTbalTame       = 0xa63ef16aab7174bbull;   // react to transients at once (per set)
constexpr Key kTbalLow        = 0x2db69e5bbc232dc2ull;   // correction amount below 100 Hz (per set)
constexpr Key kTbalMid        = 0xa62e37f085614dfbull;   // correction amount 100 Hz .. 5.6 kHz (per set)
constexpr Key kTbalHigh       = 0x694ad8e3f06f1886ull;   // correction amount above 5.6 kHz (per set)
constexpr Key kTbalFlatten    = 0x901f34474fd3983cull;   // internal flattening amount of the shape mode (no control)

// Target curves: the foreign list is ours in the same order (0 = balanced .. 34); our last entry (35, flat pink) has
// no foreign counterpart and is never produced.
double toneTarget (double v, ModuleImport&, int, int) { return indexUpTo (v, 34); }
// Correction mode: 0 shape, 1 cut, 2 boost (the same order as ours).
double toneMode (double v, ModuleImport&, int, int) { return indexUpTo (v, 2); }

//------------------------------------------------------------------------------------------------------------------
// TONE BALANCE TABLE. The "def" column is the foreign default of each control (the one place to correct them).
const std::vector<Rule>& toneBalanceRules()
{
    static const std::vector<Rule> rules {
        //  key            act               first count band        set A id          set B id          transform     foreign default   what
        { kTbalTarget,    Act::Map,          0, 0, Band::Same, "~.target",       nullptr,          toneTarget,   known (0.0),      "target curve" },
        { kTbalMode,      Act::Map,          0, 0, Band::Same, "~.mode",         nullptr,          toneMode,     known (0.0),      "correction mode" },
        { kAmount,        Act::Map,          0, 0, Band::Same, "~.a.amount",     "~.b.amount",     nullptr,      known (0.0),      "amount" },
        { kTbalSpeed,     Act::Map,          0, 0, Band::Same, "~.a.speed",      "~.b.speed",      nullptr,      known (50.0),     "speed" },
        { kTbalSmooth,    Act::Map,          0, 0, Band::Same, "~.a.smooth",     "~.b.smooth",     nullptr,      known (50.0),     "smoothing" },
        { kTbalCutSpeed,  Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,      known (100.0),    "cut speed" },
        { kTbalSens,      Act::Map,          0, 0, Band::Same, "~.a.sens",       "~.b.sens",       nullptr,      known (30.0),     "cut sensitivity" },
        { kTbalTame,      Act::Map,          0, 0, Band::Same, "~.a.tame",       "~.b.tame",       xf::boolean,  known (0.0),      "tame transients" },
        { kTbalLow,       Act::Map,          0, 0, Band::Same, "~.a.low",        "~.b.low",        nullptr,      known (100.0),    "low amount" },
        { kTbalMid,       Act::Map,          0, 0, Band::Same, "~.a.mid",        "~.b.mid",        nullptr,      known (100.0),    "mid amount" },
        { kTbalHigh,      Act::Map,          0, 0, Band::Same, "~.a.high",       "~.b.high",       nullptr,      known (100.0),    "high amount" },
        { kChannelMode,   Act::ChannelMode,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,      known (0.0),      "channel mode" },
        { kPowerA,        Act::Map,          0, 0, Band::Same, "~.a.on",         nullptr,          xf::invertBool, known (0.0),    "set A bypass" },
        { kPowerB,        Act::Map,          0, 0, Band::Same, "~.b.on",         nullptr,          xf::invertBool, known (0.0),    "set B bypass" },
        { kLink,          Act::Map,          0, 0, Band::Same, "~.link",         nullptr,          xf::boolean,  noDefault(),      "link sets" },
        { kAutoGain,      Act::Unsupported,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,      known (0.0),      "module auto gain" },
        { kTbalFlatten,   Act::Unsupported,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "shape flattening" },
        { kViewSet,       Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "selected set" },
        { kViewPart,      Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "selected part" },
        { kSoloA,         Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "set A solo" },
        { kSoloB,         Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "set B solo" },
        { kListen,        Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "listen difference" },
    };
    return rules;
}

// Cut speed: newer writers store their own control; older ones (< 110000) have one speed for every mode, which then
// also drives our cut speed.
void toneBalanceFinish (ModuleImport& m)
{
    const int v = m.pluginVersion();
    const bool olderWriter = v != 0 && v < 110000;
    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const char* id = setId (set, "~.a.cutSpeed", "~.b.cutSpeed");
        if (olderWriter)
        {
            m.ignore (kTbalCutSpeed, -1, set);   // a newer control cannot override this writer's shared speed
            m.set (id, m.get (kTbalSpeed, -1, set));
        }
        else
            m.set (id, m.get (kTbalCutSpeed, -1, set));
    }
}

//==============================================================================
// Definition
constexpr Key kDefnSection    = 0xd3eac79f739323dfull;   // definition section (tag and chain name are the same)
constexpr Key kDefnTilt       = 0x48de89fb300c23a6ull;   // target tilt in dB/oct (per set)
constexpr Key kDefnAttack     = 0x41431f2bbdc5fd3dull;   // attack in ms (per set)
constexpr Key kDefnRelease    = 0x1cdeecda0baf365eull;   // release in ms (per set)

//------------------------------------------------------------------------------------------------------------------
// DEFINITION TABLE
const std::vector<Rule>& definitionRules()
{
    static const std::vector<Rule> rules {
        //  key            act               first count band        set A id          set B id          transform     foreign default   what
        { kAmount,        Act::Map,          0, 0, Band::Same, "~.a.amount",     "~.b.amount",     nullptr,      known (0.0),      "amount" },
        { kDefnTilt,      Act::Map,          0, 0, Band::Same, "~.a.tilt",       "~.b.tilt",       nullptr,      known (0.0),      "tilt" },
        { kDefnAttack,    Act::Map,          0, 0, Band::Same, "~.a.att",        "~.b.att",        nullptr,      known (100.0),    "attack" },
        { kDefnRelease,   Act::Map,          0, 0, Band::Same, "~.a.rel",        "~.b.rel",        nullptr,      known (100.0),    "release" },
        { kRegionLow,     Act::Map,          0, 0, Band::Same, "~.a.low",        "~.b.low",        nullptr,      known (300.0),    "region start" },
        { kRegionHigh,    Act::Map,          0, 0, Band::Same, "~.a.high",       "~.b.high",       nullptr,      known (20000.0),  "region end" },
        { kChannelMode,   Act::ChannelMode,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,      known (0.0),      "channel mode" },
        { kPowerA,        Act::Map,          0, 0, Band::Same, "~.a.on",         nullptr,          xf::invertBool, known (0.0),    "set A bypass" },
        { kPowerB,        Act::Map,          0, 0, Band::Same, "~.b.on",         nullptr,          xf::invertBool, known (0.0),    "set B bypass" },
        { kLink,          Act::Map,          0, 0, Band::Same, "~.link",         nullptr,          xf::boolean,  noDefault(),      "link sets" },
        { kAutoGain,      Act::Unsupported,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,      known (0.0),      "module auto gain" },
        { kViewSet,       Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "selected set" },
        { kViewPart,      Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "selected part" },
        { kSoloA,         Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "set A solo" },
        { kSoloB,         Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "set B solo" },
        { kListen,        Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "listen difference" },
    };
    return rules;
}

//==============================================================================
// Smoother (older writers spell every control in capitals with underscores)
constexpr Key kSmthSectionTag = 0x103f61e6f268c1d8ull;   // smoother section (tag)
constexpr Key kSmthChainName  = 0xa73ef598dc89eef2ull;   // smoother (ElementID / chain name)
constexpr Key kSmthThreshold  = 0x158ab9ac67a8a3d8ull;   // absolute threshold in dBFS (older writers, per set)
constexpr Key kSmthMode       = 0x58cca4b9d6bdb0c5ull;   // reduction style: light / medium / heavy (per set)
constexpr Key kSmthAttack     = 0xd49fa4aa416ab57full;   // attack in ms (per set)
constexpr Key kSmthRelease    = 0x987cf36ebdc76f2aull;   // release in ms (per set)
constexpr Key kSmthTone       = 0xa36700f612b84629ull;   // detector tilt in % (per set)
constexpr Key kSmthLow        = 0x9dc8008dd4b24997ull;   // action region start in Hz (per set)
constexpr Key kSmthHigh       = 0xc6da18c0f5379a9bull;   // action region end in Hz (per set)
constexpr Key kSmthChMode     = 0x787b4f86b731a004ull;   // channel-mode index
constexpr Key kSmthViewSet    = 0x27e9397d794f5a60ull;   // which channel set the view shows
constexpr Key kSmthSoloA      = 0xe33683db5b40a55eull;   // set A solo (monitoring)
constexpr Key kSmthSoloB      = 0xb60ab79dddbdb4cfull;   // set B solo (monitoring)
constexpr Key kSmthPowerA     = 0x3e9be6a9994470bfull;   // set A bypass (1 = bypassed)
constexpr Key kSmthPowerB     = 0x816546714e30dca8ull;   // set B bypass (1 = bypassed)
constexpr Key kSmthLink       = 0xc920d4959c9fc5acull;   // link the two sets' controls (view)
constexpr Key kSmthRegionSolo = 0x5e1e626308633a0aull;   // solo the action region (monitoring)
constexpr Key kSmthLearn      = 0x56750e27217d1955ull;   // learn the action region (transient state)
constexpr Key kSmthMeterView  = 0xd751155394bb21fbull;   // which meter the view shows
constexpr Key kSmthResidual   = 0xd0a68959515c4d42ull;   // listen to the removed part (monitoring)

// Reduction style: 0 light, 1 medium, 2 heavy -> our Gentle / Medium / Strong.
double smootherMode (double v, ModuleImport&, int, int) { return indexUpTo (v, 2); }

// Older writers: an absolute threshold (lower = more reduction) instead of our relative amount. Heuristic: -3 dB ->
// 13.6 %, -20 dB -> 34 %, -45 dB -> 64 % (capped at 70 %); a threshold at full scale (above -1 dB, which is also the
// assumed default) never acts: 0 %.
double smootherThresholdToAmount (double thresholdDb)
{
    if (thresholdDb > -1.0)
        return 0.0;
    return std::clamp (10.0 - 1.2 * thresholdDb, 0.0, 70.0);
}

//------------------------------------------------------------------------------------------------------------------
// SMOOTHER TABLE
const std::vector<Rule>& smootherRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band        set A id          set B id          transform     foreign default   what
        { kAmount,          Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,      known (0.0),      "amount" },
        { kSmthThreshold,   Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,      guess (0.0),      "threshold (older files)" },
        { kSmthMode,        Act::Map,          0, 0, Band::Same, "~.a.mode",       "~.b.mode",       smootherMode, known (1.0),      "reduction style" },
        { kSmthAttack,      Act::Map,          0, 0, Band::Same, "~.a.att",        "~.b.att",        nullptr,      known (1.0),      "attack" },
        { kSmthRelease,     Act::Map,          0, 0, Band::Same, "~.a.rel",        "~.b.rel",        nullptr,      known (30.0),     "release" },
        { kSmthTone,        Act::Map,          0, 0, Band::Same, "~.a.tone",       "~.b.tone",       nullptr,      known (0.0),      "tone" },
        { kSmthLow,         Act::Map,          0, 0, Band::Same, "~.a.low",        "~.b.low",        nullptr,      known (2000.0),   "region start" },
        { kSmthHigh,        Act::Map,          0, 0, Band::Same, "~.a.high",       "~.b.high",       nullptr,      known (8000.0),   "region end" },
        { kSmthChMode,      Act::ChannelMode,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,      known (0.0),      "channel mode" },
        { kSmthPowerA,      Act::Map,          0, 0, Band::Same, "~.a.on",         nullptr,          xf::invertBool, known (0.0),    "set A bypass" },
        { kSmthPowerB,      Act::Map,          0, 0, Band::Same, "~.b.on",         nullptr,          xf::invertBool, known (0.0),    "set B bypass" },
        { kSmthLink,        Act::Map,          0, 0, Band::Same, "~.link",         nullptr,          xf::boolean,  noDefault(),      "link sets" },
        { kAutoGain,        Act::Unsupported,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,      known (0.0),      "module auto gain" },
        { kSmthViewSet,     Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "selected set" },
        { kViewPart,        Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "selected part" },
        { kSmthSoloA,       Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "set A solo" },
        { kSmthSoloB,       Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "set B solo" },
        { kSmthRegionSolo,  Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "region solo" },
        { kSmthLearn,       Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "region learn" },
        { kSmthMeterView,   Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "meter view" },
        { kSmthResidual,    Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "listen removed part" },
        { kListen,          Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),      "listen difference" },
    };
    return rules;
}

// Amount per set: the stored amount; else a stored threshold (or, for an older writer, the threshold default)
// converted by the heuristic; else the amount default. Newer writers' leftover thresholds are always stale,
// including when their amount is absent and therefore takes its foreign default.
void smootherFinish (ModuleImport& m)
{
    const int v = m.pluginVersion();
    const bool olderWriter = v != 0 && v < 110000;
    for (int set = 0; set < kNumChannelSets; ++set)
    {
        const char* id = setId (set, "~.a.amount", "~.b.amount");
        if (m.has (kAmount, -1, set))
        {
            m.set (id, m.get (kAmount, -1, set));
            m.ignore (kSmthThreshold, -1, set);
        }
        else if (olderWriter || (v == 0 && m.has (kSmthThreshold, -1, set)))
        {
            const double thr = m.get (kSmthThreshold, -1, set);
            if (! std::isnan (thr))
            {
                m.set (id, smootherThresholdToAmount (thr));
                if (m.has (kSmthThreshold, -1, set))
                    m.note ("Smoother: threshold converted to amount (approximate)");
            }
        }
        else
        {
            m.ignore (kSmthThreshold, -1, set);
            m.set (id, m.get (kAmount, -1, set));
        }
    }
}

//==============================================================================
// Punch
constexpr Key kPnchSection    = 0xc15d8f40cc6bf2d7ull;   // punch section (tag and chain name are the same)
constexpr Key kPnchAmount     = 0x214c6dc6650cc172ull;   // header amount in % (kept per set; ours is shared)
constexpr Key kPnchEnvelope   = 0x2088520fc0b3e635ull;   // envelope time in ms (kept per set; ours is shared)
constexpr Key kPnchSyncOn     = 0xefeaf03195001995ull;   // envelope from the host tempo (kept per set)
constexpr Key kPnchSyncNote   = 0x998ea28e8da0d2d4ull;   // note value of the synced envelope (kept per set)
constexpr Key kPnchBandAmount = 0xc7739cff132acf75ull;   // band amount in %, numbered from 1
constexpr Key kPnchBandBypass = 0x188aeb4ab4e6d2a1ull;   // band bypass, numbered from 0 (shared by both sets)
constexpr Key kPnchBandActive = 0x07e68eebf4f6765bull;   // band present, numbered from 0
constexpr Key kPnchBandSolo   = 0xee312148cb05ebd6ull;   // band solo (monitoring), numbered from 0
constexpr Key kPnchCutoff     = 0x1b9a923d565ab03bull;   // crossover frequency, numbered from 1 (lower edge of slot n)
constexpr Key kPnchLearnedHz  = 0xe640e1e5658f6328ull;   // crossover learn result, numbered from 1 (learn state)
constexpr Key kPnchLearnDone  = 0x51a4def809081cf2ull;   // crossover learn finished (learn state)
constexpr Key kPnchMakeUp     = 0x1fc591663dc1b629ull;   // automatic make-up gain on
constexpr Key kPnchViewBand   = 0xc56e49739be780c3ull;   // selected band in the view

// Four bands by default, at 160 / 1000 / 6000 Hz (writer 120000 Reset, all three read in the real product). No file
// stores any of them at its own number.
double punchCutoffDefault (const ModuleImport&, int number, int)
{
    switch (number)
    {
        case 1:  return 160.0;
        case 2:  return 1000.0;
        case 3:  return 6000.0;
        default: return kNoValue;
    }
}

// Sync note: the foreign list starts at 1/32 triplet and ends at 1 dotted (0..17); ours starts three steps lower at
// 1/64 triplet and ends at 1/2 dotted, so ours = foreign + 3. 1 triplet / 1 / 1 dotted become 1/2 dotted (noted;
// our envelope tops out at 1000 ms anyway).
double punchSyncNote (double v, ModuleImport& m, int, int)
{
    const double r = indexUpTo (v, 17);
    if (std::isnan (r))
        return kNoValue;
    if (r > 14.0)
    {
        m.note ("Punch: sync note longer than 1/2 dotted set to 1/2 dotted");
        return 17.0;
    }
    return r + 3.0;
}

//------------------------------------------------------------------------------------------------------------------
// PUNCH TABLE
const std::vector<Rule>& punchRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band          set A id          set B id            transform     foreign default                             what
        { kPnchBandActive,  Act::Hook,         0, 4, Band::Same,   nullptr,          nullptr,            nullptr,      known (1.0),                                "band present" },
        { kPnchCutoff,      Act::Hook,         1, 3, Band::Same,   nullptr,          nullptr,            nullptr,      byVersion (punchCutoffDefault, Conf::Known), "crossover frequency" },
        { kPnchBandBypass,  Act::Hook,         0, 4, Band::Same,   nullptr,          nullptr,            nullptr,      known (0.0),                                "band bypass" },
        { kPnchBandAmount,  Act::Map,          1, 4, Band::Layout, "~.a.b#.amount",  "~.b.b#.amount",    nullptr,      known (0.0),                                "band amount" },
        { kPnchAmount,      Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      known (100.0),                              "module amount" },
        { kPnchEnvelope,    Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      likely (100.0),                             "envelope" },
        { kPnchSyncOn,      Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      known (1.0),                                "envelope sync" },
        { kPnchSyncNote,    Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      known (4.0),                                "sync note" },
        { kPnchMakeUp,      Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      known (0.0),                                "auto gain" },
        { kAutoGain,        Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      known (0.0),                                "auto gain (module switch)" },
        { kChannelMode,     Act::ChannelMode,  0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      known (0.0),                                "channel mode" },
        { kPowerA,          Act::Map,          0, 0, Band::Same,   "~.a.on",         nullptr,            xf::invertBool, known (0.0),                              "set A bypass" },
        { kPowerB,          Act::Map,          0, 0, Band::Same,   "~.b.on",         nullptr,            xf::invertBool, known (0.0),                              "set B bypass" },
        { kLink,            Act::Map,          0, 0, Band::Same,   "~.link",         nullptr,            xf::boolean,  noDefault(),                                "link sets" },
        { kPnchBandSolo,    Act::Ignore,       0, 4, Band::Same,   nullptr,          nullptr,            nullptr,      noDefault(),                                "band solo" },
        { kPnchLearnedHz,   Act::Ignore,       1, 3, Band::Same,   nullptr,          nullptr,            nullptr,      noDefault(),                                "learned crossover" },
        { kPnchLearnDone,   Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      noDefault(),                                "crossover learn done" },
        { kPnchViewBand,    Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      noDefault(),                                "selected band" },
        { kViewSet,         Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      noDefault(),                                "selected set" },
        { kSoloA,           Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      noDefault(),                                "set A solo" },
        { kSoloB,           Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      noDefault(),                                "set B solo" },
        { kListen,          Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,            nullptr,      noDefault(),                                "listen difference" },
    };
    return rules;
}

void punchPrepare (ModuleImport& m)
{
    m.readLayout (kPnchBandActive);
    m.writeCrossovers (kPnchCutoff);
}

void punchFinish (ModuleImport& m)
{
    // A band bypass is shared by both sets: our band power in both.
    for (int slot = 0; slot < kMaxBands; ++slot)
    {
        const int band = m.layout().bandOfSlot[slot];
        if (band <= 0)
        {
            m.ignore (kPnchBandBypass, slot, 0);   // a removed band's leftover
            continue;
        }
        const double bypass = m.get (kPnchBandBypass, slot, 0);
        const double on = (! std::isnan (bypass) && bypass >= 0.5) ? 0.0 : 1.0;
        m.set ("~.a.b#.on", on, band);
        m.set ("~.b.b#.on", on, band);
    }

    sharedFromSetA (m, kPnchAmount, "~.amount", nullptr, "Punch: set B amount not imported (shared here)");
    sharedFromSetA (m, kPnchEnvelope, "~.env", nullptr, "Punch: set B envelope not imported (shared here)");
    sharedFromSetA (m, kPnchSyncOn, "~.sync", xf::boolean, "Punch: set B sync not imported (shared here)");
    sharedFromSetA (m, kPnchSyncNote, "~.syncNote", punchSyncNote, "Punch: set B sync note not imported (shared here)");

    // Two stored switches of the make-up gain; either one on turns ours on.
    const double makeUp = m.get (kPnchMakeUp);
    const double moduleSwitch = m.get (kAutoGain);
    m.set ("~.autoGain", (makeUp >= 0.5 || moduleSwitch >= 0.5) ? 1.0 : 0.0);
}

//==============================================================================
// Low Contrast
constexpr Key kLowcSectionTag = 0x6e0b5a229bf873eaull;   // low contrast section (tag)
constexpr Key kLowcChainName  = 0x7d36f366cd0a6b78ull;   // low contrast (ElementID / chain name)
constexpr Key kLowcContrast   = 0x82c21acdbcd6e830ull;   // contrast in % (per set)
constexpr Key kLowcGain       = 0xb5767f7e05ad941eull;   // region make-up gain in dB (per set)
constexpr Key kLowcCharacter  = 0x164a1df1032eb103ull;   // character: 0 punchy, 1 smooth (per set)
constexpr Key kLowcMix        = 0x1dfd7119b50f64a1ull;   // wet / dry mix in % (no counterpart)
constexpr Key kLowcRegionSolo = 0xebbb102a716f997cull;   // solo the action region (monitoring)

// Character: 0 punchy, 1 smooth (the same order as ours).
double lowContrastMode (double v, ModuleImport&, int, int) { return indexUpTo (v, 1); }

//------------------------------------------------------------------------------------------------------------------
// LOW CONTRAST TABLE
const std::vector<Rule>& lowContrastRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band        set A id          set B id          transform        foreign default   what
        { kLowcContrast,    Act::Map,          0, 0, Band::Same, "~.a.contrast",   "~.b.contrast",   nullptr,         known (0.0),      "contrast" },
        { kLowcGain,        Act::Map,          0, 0, Band::Same, "~.a.gain",       "~.b.gain",       nullptr,         known (0.0),      "region gain" },
        { kLowcCharacter,   Act::Map,          0, 0, Band::Same, "~.a.mode",       "~.b.mode",       lowContrastMode, known (0.0),      "character" },
        { kRegionLow,       Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,         known (20.0),     "region start" },
        { kRegionHigh,      Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,         known (250.0),    "region end" },
        { kLowcMix,         Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,         guess (100.0),    "mix" },
        { kAutoGain,        Act::Map,          0, 0, Band::Same, "~.autoGain",     nullptr,          xf::boolean,     known (0.0),      "auto gain" },
        { kChannelMode,     Act::ChannelMode,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,         known (0.0),      "channel mode" },
        { kPowerA,          Act::Map,          0, 0, Band::Same, "~.a.on",         nullptr,          xf::invertBool,  known (0.0),      "set A bypass" },
        { kPowerB,          Act::Map,          0, 0, Band::Same, "~.b.on",         nullptr,          xf::invertBool,  known (0.0),      "set B bypass" },
        { kLink,            Act::Map,          0, 0, Band::Same, "~.link",         nullptr,          xf::boolean,     noDefault(),      "link sets" },
        { kLowcRegionSolo,  Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,         noDefault(),      "region solo" },
        { kViewSet,         Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,         noDefault(),      "selected set" },
        { kViewPart,        Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,         noDefault(),      "selected part" },
        { kSoloA,           Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,         noDefault(),      "set A solo" },
        { kSoloB,           Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,         noDefault(),      "set B solo" },
        { kListen,          Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,         noDefault(),      "listen difference" },
    };
    return rules;
}

void lowContrastFinish (ModuleImport& m)
{
    // The region is one control here; the foreign module may keep it per set.
    sharedFromSetA (m, kRegionLow, "~.low", nullptr, "Low Contrast: set B region not imported (shared here)");
    sharedFromSetA (m, kRegionHigh, "~.high", nullptr, "Low Contrast: set B region not imported (shared here)");

    // No wet / dry here: a full mix imports, anything else is reported.
    for (int set = 0; set < kNumChannelSets; ++set)
        if (m.has (kLowcMix, -1, set))
        {
            const double mix = m.get (kLowcMix, -1, set);
            if (std::abs (mix - 100.0) > 0.05)
            {
                m.reject (kLowcMix, -1, set, "unsupported");
                m.note ("Low Contrast: wet/dry mix not imported");
            }
        }
}

//==============================================================================
// Bass Shaper
constexpr Key kBassSectionTag = 0xa574f82191c0229full;   // bass shaper section (tag)
constexpr Key kBassChainName  = 0x5f05f4c6e9fe9123ull;   // bass shaper (ElementID / chain name)
constexpr Key kBassCutoff     = 0xfa4ab2e21096420aull;   // cutoff in Hz
constexpr Key kBassBalance    = 0xb8a3d4df7d1c5e6bull;   // level below the cutoff in dB
constexpr Key kBassPunch      = 0x82936072ce490dc3ull;   // level of the low-band hits in dB
constexpr Key kBassSusThr     = 0x75768c3e3f5ecaf6ull;   // sustain lift threshold in dB
constexpr Key kBassSusAmount  = 0x1f4c8969f2a2930full;   // sustain lift amount in %
constexpr Key kBassSusChar    = 0x7e196fdc0a85b8baull;   // sustain character: 0 dirty .. 100 clean
constexpr Key kBassPeakThr    = 0x2c14880e6c49f412ull;   // peak control threshold in dB
constexpr Key kBassPeakChar   = 0xd4253e23ee0fc1aeull;   // peak character: 0 dirty .. 6 clean
constexpr Key kBassBalanceOn  = 0x683c850f9974169cull;   // balance section power
constexpr Key kBassPunchOn    = 0x04e748f7cc218874ull;   // punch section power
constexpr Key kBassSusOn      = 0xa313f4458b25522cull;   // sustain section power
constexpr Key kBassPeakOn     = 0x200c056ea5961eb8ull;   // peak section power
constexpr Key kBassSoloCutoff = 0xebe27af4e818f647ull;   // listen below the cutoff (monitoring)

// Peak character 0 (dirty) .. 6 (clean) -> our 0 .. 100 %.
double bassPeakCharacter (double v, ModuleImport&, int, int) { return std::clamp (v / 6.0 * 100.0, 0.0, 100.0); }

//------------------------------------------------------------------------------------------------------------------
// BASS SHAPER TABLE
const std::vector<Rule>& bassShaperRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band        set A id          set B id          transform          foreign default   what
        { kBassCutoff,      Act::Map,          0, 0, Band::Same, "~.cutoff",       nullptr,          nullptr,           known (120.0),    "cutoff" },
        { kBassBalance,     Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,           known (0.0),      "balance" },
        { kBassPunch,       Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,           known (0.0),      "punch" },
        { kBassSusThr,      Act::Map,          0, 0, Band::Same, "~.sus.thr",      nullptr,          nullptr,           known (0.0),      "sustain threshold" },
        { kBassSusAmount,   Act::Map,          0, 0, Band::Same, "~.sus.amount",   nullptr,          nullptr,           known (0.0),      "sustain amount" },
        { kBassSusChar,     Act::Map,          0, 0, Band::Same, "~.sus.char",     nullptr,          nullptr,           known (50.0),     "sustain character" },
        { kBassPeakThr,     Act::Map,          0, 0, Band::Same, "~.peak.thr",     nullptr,          nullptr,           known (0.0),      "peak threshold" },
        { kBassPeakChar,    Act::Map,          0, 0, Band::Same, "~.peak.char",    nullptr,          bassPeakCharacter, known (3.0),      "peak character" },
        { kBassBalanceOn,   Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,           known (1.0),      "balance power" },
        { kBassPunchOn,     Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,           known (1.0),      "punch power" },
        { kBassSusOn,       Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,           known (1.0),      "sustain power" },
        { kBassPeakOn,      Act::Hook,         0, 0, Band::Same, nullptr,          nullptr,          nullptr,           known (1.0),      "peak power" },
        { kBassSoloCutoff,  Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,           noDefault(),      "listen below cutoff" },
        { kListen,          Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,           noDefault(),      "listen difference" },
    };
    return rules;
}

// Balance / punch: 0 dB while their section is off. Sustain / peak: our switch follows the section power (stored,
// else on); an untouched section sits at the confirmed defaults, where it does nothing.
void bassShaperFinish (ModuleImport& m)
{
    auto level = [&m] (Key powerKey, Key valueKey, const char* id)
    {
        if (m.get (powerKey) >= 0.5)
            m.set (id, m.get (valueKey));
        else
        {
            m.ignore (valueKey);   // stale while the section is off
            m.set (id, 0.0);
        }
    };
    level (kBassBalanceOn, kBassBalance, "~.balance");
    level (kBassPunchOn, kBassPunch, "~.punch");

    m.set ("~.sus.on", m.get (kBassSusOn) >= 0.5 ? 1.0 : 0.0);
    m.set ("~.peak.on", m.get (kBassPeakOn) >= 0.5 ? 1.0 : 0.0);
}
} // namespace

//==============================================================================
const std::vector<ModuleSpec>& phase2FamilySpecs()
{
    static const std::vector<ModuleSpec> specs {
        { ModuleType::ToneBalance, kTbalSection,    kTbalSection,   &toneBalanceRules(), nullptr,      toneBalanceFinish },
        { ModuleType::Definition,  kDefnSection,    kDefnSection,   &definitionRules(),  nullptr,      nullptr },
        { ModuleType::Smoother,    kSmthSectionTag, kSmthChainName, &smootherRules(),    nullptr,      smootherFinish },
        { ModuleType::Punch,       kPnchSection,    kPnchSection,   &punchRules(),       punchPrepare, punchFinish },
        { ModuleType::LowContrast, kLowcSectionTag, kLowcChainName, &lowContrastRules(), nullptr,      lowContrastFinish },
        { ModuleType::BassShaper,  kBassSectionTag, kBassChainName, &bassShaperRules(),  nullptr,      bassShaperFinish },
    };
    return specs;
}
} // namespace ps::imp
