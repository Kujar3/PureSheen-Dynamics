#include "plugin/import/ImportCommon.h"

#include <cmath>

// Preset importer, vintage-style family (phase 3): Classic Compressor, Passive EQ, Classic Limiter, Tape.
// Mapping conventions: out/importer/spec.md. Key comments are in our own words; the foreign strings are never written
// here (`PureSheenDev importkey "<ParamID>"` prints the template and key of a stored name).
//
// Every table's "def" column is the FOREIGN default of the writer version (sparse files: an absent value takes it).
// The defaults of this family are NOT confirmed yet: they come from the user's screenshots and min-max notes of the
// real product, checked against sparse storage (the one-change check saves of writer 120000 and the factory presets
// of writer 9030 never store them). Known = proven by sparse storage or documented; Likely = a screenshot value the
// files agree with; Guess = read off a screenshot curve. Every Likely / Guess row is listed in
// out/importer/defaults-to-confirm.md ("Vintage-style family") and by `PureSheenDev import --defaults`.
//
// Decisions:
// - Values are read on our scales 1:1 and clamped into our ranges (counted as clamped): the foreign Classic
//   Compressor threshold / gain and the Tape drive reach further than ours (-60 dB / +-30 dB / +30 dB), the Passive
//   EQ boost / cut amounts (0..10, shown in dB there) land on our 0..10 knobs. Our modules are not fitted to the
//   reference's renders yet, so the same numbers do not sound identical yet.
// - Passive EQ high-boost bandwidth: the foreign control is a Q (0 = broad .. 10 = narrow, black-box renders of the
//   check saves), ours a bandwidth (0 = sharp .. 10 = broad): ours = 10 - theirs.
// - Tape speed: the foreign index runs 0 = 30 ips (its default), 1 = 15, 2 = 7.5 (check saves); ours is the reverse.
// - The Passive EQ tube drive and the Classic Compressor's sidechain shape have no foreign counterpart: our defaults.
namespace ps::imp
{
namespace
{
//==============================================================================
// Controls shared by several modules of this family (one key per stored spelling)
constexpr Key kThreshold      = 0xe19cc57dd8675498ull;   // threshold in dB (compressor per set, limiter)
constexpr Key kProcessingMode = 0xf0abeb64a4caf959ull;   // channel-mode index (stereo, mid/side, ...)
constexpr Key kViewSet        = 0xb6004e16c48d94d3ull;   // which channel set the view edits (mid / side)

// Index remaps: a foreign index outside the known list cannot be represented (NaN: not imported, our default kept).
double indexUpTo (double v, int last)
{
    const double r = std::round (v);
    if (std::abs (v - r) > 1.0e-3 || r < 0.0 || r > (double) last)
        return kNoValue;
    return r;
}

//==============================================================================
// FOREIGN DEFAULTS OF THE VINTAGE-STYLE FAMILY (not confirmed yet; the one place to correct them).
// Each value is the reference's default in the reference's own units / index, as the table rows read it.
namespace vdef
{
    // Classic Compressor (per set)
    constexpr Def compThreshold  = likely (-12.0);   // dB (screenshot)
    constexpr Def compRatio      = likely (1.5);     // :1 (screenshot)
    constexpr Def compAttack     = likely (20.0);    // ms (screenshot)
    constexpr Def compRelease    = likely (50.0);    // ms (screenshot)
    constexpr Def compKneeMode   = known (1.0);      // Balanced (a Balanced check save stores no mode)
    constexpr Def compMakeup     = likely (0.0);     // dB (screenshot)
    constexpr Def compAutoGain   = known (1.0);      // on (the check save that turns it off stores 0)
    constexpr Def compChMode     = known (0.0);      // stereo (the mid/side check save stores 1)
    constexpr Def compScHpf      = guess (45.0);     // Hz (read off the detection-filter curve)
    constexpr Def compScBellHz   = guess (2000.0);   // Hz (read off the curve; a check save stores 1000)
    constexpr Def compScBellDb   = likely (0.0);     // dB
    constexpr Def compScShelfHz  = guess (6000.0);   // Hz (read off the curve)
    constexpr Def compScShelfDb  = likely (0.0);     // dB
    // Passive EQ (per set; frequencies are indices into the foreign lists, which equal ours)
    constexpr Def eqLowHz        = likely (2.0);     // 45 Hz (screenshot; check saves store 20 / 60 / 100 Hz)
    constexpr Def eqAmount       = known (0.0);      // every boost / cut (the default render is flat)
    constexpr Def eqHighHz       = known (3.0);      // 8 kHz (the 8 kHz check save stores no frequency)
    constexpr Def eqHighQ        = known (0.0);      // broadest (the "minimum" check save stores no Q)
    constexpr Def eqHighCutHz    = known (1.0);      // 10 kHz (check saves store 5 k and 20 k)
    constexpr Def eqLowMidHz     = likely (2.0);     // 500 Hz (screenshot; a check save stores 300 Hz)
    constexpr Def eqMidHz        = likely (5.0);     // 1.5 kHz (screenshot; a check save stores 1 kHz)
    constexpr Def eqHighMidHz    = likely (2.0);     // 3 kHz (screenshot)
    constexpr Def eqChMode       = known (0.0);      // stereo
    // Classic Limiter
    constexpr Def limThreshold   = likely (-2.0);    // dB (screenshot)
    constexpr Def limCeiling     = likely (0.0);     // dB (screenshot)
    constexpr Def limCharacter   = likely (2.0);     // 0..10 (screenshot; check saves store 0 / 5 / 10)
    constexpr Def limMode        = known (1.0);      // Tube (the Tube check save stores no mode)
    constexpr Def limTruePeak    = known (0.0);      // off (the true-peak check save stores 1)
    constexpr Def limLink        = guess (0.0);      // off (never stored)
    // Tape
    constexpr Def tapeSpeed      = known (0.0);      // index 0 = 30 ips (the 15 / 7.5 ips check saves store 1 / 2)
    constexpr Def tapeDrive      = likely (0.0);     // dB (screenshot)
    constexpr Def tapeBias       = likely (0.0);     // (screenshot)
    constexpr Def tapeHarmonics  = known (0.0);      // (the 0 check save stores nothing)
    constexpr Def tapeLowEmph    = likely (2.0);     // head bump (screenshot)
    constexpr Def tapeHighEmph   = known (4.0);      // flat highs (documented)
} // namespace vdef

//==============================================================================
// Classic Compressor
constexpr Key kCompSectionTag = 0x26c11f0faab3834cull;   // classic compressor section (tag)
constexpr Key kCompChainName  = 0x72e0609f4e29b1baull;   // classic compressor (ElementID / chain name)
constexpr Key kCompRatio      = 0xbb890b77f38d0044ull;   // ratio (per set)
constexpr Key kCompAttack     = 0x41431f2bbdc5fd3dull;   // attack in ms (per set)
constexpr Key kCompRelease    = 0x8a5fbb778b3d2a8aull;   // transient release in ms (per set)
constexpr Key kCompKneeMode   = 0x4f48c63a3f19b605ull;   // knee / ballistics mode: sharp, balanced, smooth (per set)
constexpr Key kCompMakeup     = 0x5931607a01a0c9c1ull;   // make-up gain in dB (per set)
constexpr Key kCompAutoGain   = 0x7a9f5dd5877343a1ull;   // automatic gain compensation (per set)
constexpr Key kCompScHpf      = 0xb89d69f2431ab17aull;   // detector high-pass in Hz (per set)
constexpr Key kCompScBellHz   = 0x8ab36a5055b448b2ull;   // detector bell frequency in Hz (per set)
constexpr Key kCompScBellDb   = 0xf24dcbb9439f1497ull;   // detector bell gain in dB (per set)
constexpr Key kCompScShelfHz  = 0x72d4447552fa9167ull;   // detector high shelf frequency in Hz (per set)
constexpr Key kCompScShelfDb  = 0x5769e6cd882651f8ull;   // detector high shelf gain in dB (per set)

// Mode: 0 sharp, 1 balanced, 2 smooth (the same order as ours).
double compMode (double v, ModuleImport&, int, int) { return indexUpTo (v, 2); }

//------------------------------------------------------------------------------------------------------------------
// CLASSIC COMPRESSOR TABLE
const std::vector<Rule>& classicCompRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band        set A id          set B id          transform     foreign default        what
        { kThreshold,       Act::Map,          0, 0, Band::Same, "~.a.thr",        "~.b.thr",        nullptr,      vdef::compThreshold,   "threshold" },
        { kCompRatio,       Act::Map,          0, 0, Band::Same, "~.a.ratio",      "~.b.ratio",      nullptr,      vdef::compRatio,       "ratio" },
        { kCompAttack,      Act::Map,          0, 0, Band::Same, "~.a.attack",     "~.b.attack",     nullptr,      vdef::compAttack,      "attack" },
        { kCompRelease,     Act::Map,          0, 0, Band::Same, "~.a.release",    "~.b.release",    nullptr,      vdef::compRelease,     "release" },
        { kCompKneeMode,    Act::Map,          0, 0, Band::Same, "~.a.mode",       "~.b.mode",       compMode,     vdef::compKneeMode,    "mode" },
        { kCompMakeup,      Act::Map,          0, 0, Band::Same, "~.a.makeup",     "~.b.makeup",     nullptr,      vdef::compMakeup,      "makeup" },
        { kCompAutoGain,    Act::Map,          0, 0, Band::Same, "~.a.autoGain",   "~.b.autoGain",   xf::boolean,  vdef::compAutoGain,    "auto gain" },
        { kCompScHpf,       Act::Map,          0, 0, Band::Same, "~.a.sc.hpf",     "~.b.sc.hpf",     nullptr,      vdef::compScHpf,       "sidechain high-pass" },
        { kCompScBellHz,    Act::Map,          0, 0, Band::Same, "~.a.sc.bellHz",  "~.b.sc.bellHz",  nullptr,      vdef::compScBellHz,    "sidechain bell frequency" },
        { kCompScBellDb,    Act::Map,          0, 0, Band::Same, "~.a.sc.bellDb",  "~.b.sc.bellDb",  nullptr,      vdef::compScBellDb,    "sidechain bell gain" },
        { kCompScShelfHz,   Act::Map,          0, 0, Band::Same, "~.a.sc.shelfHz", "~.b.sc.shelfHz", nullptr,      vdef::compScShelfHz,   "sidechain shelf frequency" },
        { kCompScShelfDb,   Act::Map,          0, 0, Band::Same, "~.a.sc.shelfDb", "~.b.sc.shelfDb", nullptr,      vdef::compScShelfDb,   "sidechain shelf gain" },
        { kProcessingMode,  Act::ChannelMode,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,      vdef::compChMode,      "channel mode" },
        { kViewSet,         Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,      noDefault(),           "selected set" },
    };
    return rules;
}

//==============================================================================
// Passive EQ
constexpr Key kEqSectionTag   = 0x14ed3e9e040de2d3ull;   // passive EQ section (tag)
constexpr Key kEqChainName    = 0x06506d82d1caaaddull;   // passive EQ (ElementID / chain name)
constexpr Key kEqLowHz        = 0x023fa60c9d1404fdull;   // program EQ low frequency index (per set)
constexpr Key kEqLowBoost     = 0xc0e364d4ab010e78ull;   // program EQ low boost (per set)
constexpr Key kEqLowCut       = 0x023fd21fbd6ef07bull;   // program EQ low attenuation (per set)
constexpr Key kEqHighHz       = 0x00295568ea62c662ull;   // program EQ high boost frequency index (per set)
constexpr Key kEqHighBoost    = 0xd253f3aa4dab40acull;   // program EQ high boost (per set)
constexpr Key kEqHighQ        = 0x05323d7bcbc12a8full;   // program EQ high boost sharpness (per set)
constexpr Key kEqHighCutHz    = 0x24a5e9c6ae145f71ull;   // program EQ high attenuation frequency index (per set)
constexpr Key kEqHighCut      = 0xd6794922b4776297ull;   // program EQ high attenuation (per set)
constexpr Key kEqLowMidHz     = 0xf0003245069cbb72ull;   // midrange EQ low-mid frequency index (per set)
constexpr Key kEqLowMidBoost  = 0xc44d48c9536c53efull;   // midrange EQ low-mid boost (per set)
constexpr Key kEqMidHz        = 0x795369c2510645daull;   // midrange EQ mid cut frequency index (per set)
constexpr Key kEqMidCut       = 0xdf9b26ee3db22a64ull;   // midrange EQ mid cut (per set)
constexpr Key kEqHighMidHz    = 0x6ae7cfe13febfdd4ull;   // midrange EQ high-mid frequency index (per set)
constexpr Key kEqHighMidBoost = 0x97e2c4696e7d4831ull;   // midrange EQ high-mid boost (per set)

// The foreign frequency lists are ours, in the same order.
double eqLowHz (double v, ModuleImport&, int, int) { return indexUpTo (v, (int) kPassiveEqLowHz.size() - 1); }
double eqHighHz (double v, ModuleImport&, int, int) { return indexUpTo (v, (int) kPassiveEqHighHz.size() - 1); }
double eqHighCutHz (double v, ModuleImport&, int, int) { return indexUpTo (v, (int) kPassiveEqHighCutHz.size() - 1); }
double eqLowMidHz (double v, ModuleImport&, int, int) { return indexUpTo (v, (int) kPassiveEqLowMidHz.size() - 1); }
double eqMidHz (double v, ModuleImport&, int, int) { return indexUpTo (v, (int) kPassiveEqMidHz.size() - 1); }
double eqHighMidHz (double v, ModuleImport&, int, int) { return indexUpTo (v, (int) kPassiveEqHighMidHz.size() - 1); }
// Their sharpness (0 broad .. 10 narrow) -> our bandwidth (0 sharp .. 10 broad).
double eqHighBandwidth (double v, ModuleImport&, int, int) { return 10.0 - v; }

//------------------------------------------------------------------------------------------------------------------
// PASSIVE EQ TABLE
const std::vector<Rule>& passiveEqRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band        set A id          set B id          transform        foreign default     what
        { kEqLowHz,         Act::Map,          0, 0, Band::Same, "~.a.lowHz",      "~.b.lowHz",      eqLowHz,         vdef::eqLowHz,      "low frequency" },
        { kEqLowBoost,      Act::Map,          0, 0, Band::Same, "~.a.lowBoost",   "~.b.lowBoost",   nullptr,         vdef::eqAmount,     "low boost" },
        { kEqLowCut,        Act::Map,          0, 0, Band::Same, "~.a.lowCut",     "~.b.lowCut",     nullptr,         vdef::eqAmount,     "low cut" },
        { kEqHighHz,        Act::Map,          0, 0, Band::Same, "~.a.highHz",     "~.b.highHz",     eqHighHz,        vdef::eqHighHz,     "high boost frequency" },
        { kEqHighBoost,     Act::Map,          0, 0, Band::Same, "~.a.highBoost",  "~.b.highBoost",  nullptr,         vdef::eqAmount,     "high boost" },
        { kEqHighQ,         Act::Map,          0, 0, Band::Same, "~.a.highBw",     "~.b.highBw",     eqHighBandwidth, vdef::eqHighQ,      "high boost bandwidth" },
        { kEqHighCutHz,     Act::Map,          0, 0, Band::Same, "~.a.highCutHz",  "~.b.highCutHz",  eqHighCutHz,     vdef::eqHighCutHz,  "high cut frequency" },
        { kEqHighCut,       Act::Map,          0, 0, Band::Same, "~.a.highCut",    "~.b.highCut",    nullptr,         vdef::eqAmount,     "high cut" },
        { kEqLowMidHz,      Act::Map,          0, 0, Band::Same, "~.a.lmHz",       "~.b.lmHz",       eqLowMidHz,      vdef::eqLowMidHz,   "low-mid frequency" },
        { kEqLowMidBoost,   Act::Map,          0, 0, Band::Same, "~.a.lmBoost",    "~.b.lmBoost",    nullptr,         vdef::eqAmount,     "low-mid boost" },
        { kEqMidHz,         Act::Map,          0, 0, Band::Same, "~.a.midHz",      "~.b.midHz",      eqMidHz,         vdef::eqMidHz,      "mid frequency" },
        { kEqMidCut,        Act::Map,          0, 0, Band::Same, "~.a.midCut",     "~.b.midCut",     nullptr,         vdef::eqAmount,     "mid cut" },
        { kEqHighMidHz,     Act::Map,          0, 0, Band::Same, "~.a.hmHz",       "~.b.hmHz",       eqHighMidHz,     vdef::eqHighMidHz,  "high-mid frequency" },
        { kEqHighMidBoost,  Act::Map,          0, 0, Band::Same, "~.a.hmBoost",    "~.b.hmBoost",    nullptr,         vdef::eqAmount,     "high-mid boost" },
        { kProcessingMode,  Act::ChannelMode,  0, 0, Band::Same, nullptr,          nullptr,          nullptr,         vdef::eqChMode,     "channel mode" },
        { kViewSet,         Act::Ignore,       0, 0, Band::Same, nullptr,          nullptr,          nullptr,         noDefault(),        "selected set" },
    };
    return rules;
}

//==============================================================================
// Classic Limiter
constexpr Key kLimSectionTag  = 0x22259cc996cef7b3ull;   // classic limiter section (tag)
constexpr Key kLimChainName   = 0x09d5140dccdc1c0dull;   // classic limiter (ElementID / chain name)
constexpr Key kLimCeiling     = 0x3f1e7a3f2b283d3bull;   // output ceiling in dB
constexpr Key kLimCharacter   = 0xabfe23c45dfe3050ull;   // character: 0 fast .. 10 slow
constexpr Key kLimMode        = 0x7633d44cae026b28ull;   // mode: analog, tube, modern
constexpr Key kLimTruePeak    = 0xd0fa3dd2612f85c0ull;   // true-peak limiting
constexpr Key kLimLink        = 0x2789e20b8343a6e7ull;   // threshold and ceiling move together (UI)

// Mode: 0 analog, 1 tube, 2 modern (the same order as ours).
double limMode (double v, ModuleImport&, int, int) { return indexUpTo (v, 2); }

//------------------------------------------------------------------------------------------------------------------
// CLASSIC LIMITER TABLE
const std::vector<Rule>& classicLimiterRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band        set A id          set B id          transform     foreign default      what
        { kThreshold,       Act::Map,          0, 0, Band::Same, "~.thr",          nullptr,          nullptr,      vdef::limThreshold,  "threshold" },
        { kLimCeiling,      Act::Map,          0, 0, Band::Same, "~.ceiling",      nullptr,          nullptr,      vdef::limCeiling,    "ceiling" },
        { kLimCharacter,    Act::Map,          0, 0, Band::Same, "~.character",    nullptr,          nullptr,      vdef::limCharacter,  "character" },
        { kLimMode,         Act::Map,          0, 0, Band::Same, "~.mode",         nullptr,          limMode,      vdef::limMode,       "mode" },
        { kLimTruePeak,     Act::Map,          0, 0, Band::Same, "~.truePeak",     nullptr,          xf::boolean,  vdef::limTruePeak,   "true peak" },
        { kLimLink,         Act::Map,          0, 0, Band::Same, "~.link",         nullptr,          xf::boolean,  vdef::limLink,       "link threshold and ceiling" },
    };
    return rules;
}

//==============================================================================
// Tape
constexpr Key kTapeSectionTag = 0x6a5cc98556008c35ull;   // tape section (tag)
constexpr Key kTapeChainName  = 0xf4e4871d0736260full;   // tape (ElementID / chain name)
constexpr Key kTapeSpeed      = 0xf38c7612996eca7aull;   // tape speed index
constexpr Key kTapeDrive      = 0xcdfb7205284666c4ull;   // input drive in dB
constexpr Key kTapeBias       = 0x094040a7d218bd84ull;   // bias -10 .. 10
constexpr Key kTapeHarmonics  = 0x8c375c424c6383b5ull;   // even harmonics 0 .. 10
constexpr Key kTapeLowEmph    = 0x9fab6885365c2ec9ull;   // head bump (low emphasis) 0 .. 10
constexpr Key kTapeHighEmph   = 0xebbf1165b72df9cfull;   // high emphasis 0 .. 10

// Their 0 = 30, 1 = 15, 2 = 7.5 ips -> ours (7.5, 15, 30 ips).
double tapeSpeed (double v, ModuleImport&, int, int)
{
    const double r = indexUpTo (v, 2);
    return std::isnan (r) ? kNoValue : 2.0 - r;
}

//------------------------------------------------------------------------------------------------------------------
// TAPE TABLE
const std::vector<Rule>& tapeRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band        set A id          set B id          transform     foreign default      what
        { kTapeSpeed,       Act::Map,          0, 0, Band::Same, "~.speed",        nullptr,          tapeSpeed,    vdef::tapeSpeed,     "speed" },
        { kTapeDrive,       Act::Map,          0, 0, Band::Same, "~.drive",        nullptr,          nullptr,      vdef::tapeDrive,     "drive" },
        { kTapeBias,        Act::Map,          0, 0, Band::Same, "~.bias",         nullptr,          nullptr,      vdef::tapeBias,      "bias" },
        { kTapeHarmonics,   Act::Map,          0, 0, Band::Same, "~.harmonics",    nullptr,          nullptr,      vdef::tapeHarmonics, "harmonics" },
        { kTapeLowEmph,     Act::Map,          0, 0, Band::Same, "~.lowEmph",      nullptr,          nullptr,      vdef::tapeLowEmph,   "low emphasis" },
        { kTapeHighEmph,    Act::Map,          0, 0, Band::Same, "~.highEmph",     nullptr,          nullptr,      vdef::tapeHighEmph,  "high emphasis" },
    };
    return rules;
}
} // namespace

//==============================================================================
const std::vector<ModuleSpec>& vintageFamilySpecs()
{
    static const std::vector<ModuleSpec> specs {
        { ModuleType::ClassicComp,    kCompSectionTag, kCompChainName, &classicCompRules(),    nullptr, nullptr },
        { ModuleType::PassiveEq,      kEqSectionTag,   kEqChainName,   &passiveEqRules(),      nullptr, nullptr },
        { ModuleType::ClassicLimiter, kLimSectionTag,  kLimChainName,  &classicLimiterRules(), nullptr, nullptr },
        { ModuleType::Tape,           kTapeSectionTag, kTapeChainName, &tapeRules(),           nullptr, nullptr },
    };
    return specs;
}
} // namespace ps::imp
