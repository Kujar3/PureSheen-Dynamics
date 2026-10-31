#include "plugin/import/ImportCommon.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Preset importer, core dynamics family: Dynamics, Imager, Exciter, Maximizer and the global section (I/O, dither).
// Mapping conventions: out/importer/spec.md. Key comments are in our own words; the foreign strings are never written
// here (`PureSheenDev importkey "<ParamID>"` prints the template and key of a stored name).
//
// Status: every table is complete. Defaults marked likely / guess are listed (with their evidence) in the private
// out/importer/defaults-to-confirm.md and printed by `PureSheenDev import --defaults`; the "def" column of each table
// is the one place to correct them. Rows marked known include the defaults confirmed in the real product (writer
// 120000: Reset screenshots and one-change test saves); where the files prove an older writer differed, that writer
// keeps its own default (byVersion).
//
// Choices made here (no foreign behaviour documented; what a mastering engineer would expect):
// - Controls the foreign module keeps per channel set while ours are shared (Dynamics detection, release mode,
//   detection filter, auto gain; Exciter post filter) take set A; a different set B value in a two-set file is dropped
//   with a note.
// - The Dynamics set B output gain and both sets' overall mix have no counterpart: they are folded into the band gains
//   and band mixes of their set (a note says so).
// - A Dynamics band limiter is switched on only where its threshold is below 0 dB (at 0 dB the foreign limiter only
//   catches overs; ours stays off there, which is the same in practice and costs nothing).
// - The Maximizer's older threshold control (a negative dB value) and the newer gain control drive our gain: gain =
//   -threshold. Its transient control spans twice our range (0..200 -> 0..100 %).
// - The Dynamics knee is stored in the foreign knob's display units, 0..10 (10 = the maximum and the default, for the
//   compressor and the limiter). It is read 1:1 as our knee width in dB (0..10 dB of our 0..30 dB): the foreign
//   scale is unitless, and 10 dB is a usual soft knee (a 3x stretch to our full range would make the default knee
//   30 dB wide, far softer than any foreign setting sounds).
// - Dither and the I/O section are never stored by the foreign writer, so there are no rules for them: a whole-chain
//   import leaves them at our defaults (spec decision).
namespace ps::imp
{
namespace
{
// Reject foreign enum values before converting to an integer. Hostile finite exponents must not reach lround.
int indexOf (double value, int count) noexcept
{
    return value > -0.5 && value < (double) count - 0.5 ? (int) std::lround (value) : -1;
}

// Keep compound gain arithmetic finite until set() clamps it to the destination range.
double addFinite (double a, double b) noexcept
{
    constexpr double limit = std::numeric_limits<double>::max();
    if (b > 0.0 && a > limit - b)
        return limit;
    if (b < 0.0 && a < -limit - b)
        return -limit;
    return a + b;
}

//==============================================================================
// Shared numbering of the multiband modules (the same templates in Dynamics, Imager and Exciter)
constexpr Key kBandActive      = 0x07e68eebf4f6765bull;   // per-slot "band present" flag, numbered from 0
constexpr Key kNumBands        = 0x96045c01593bb88bull;   // band count of older writers (< 9000)
constexpr Key kBandBypass      = 0x188aeb4ab4e6d2a1ull;   // per-slot band bypass, numbered from 0
constexpr Key kBandSolo        = 0xee312148cb05ebd6ull;   // per-slot band solo (monitoring), numbered from 0
constexpr Key kCrossoverCutoff = 0x1b9a923d565ab03bull;   // crossover frequency, numbered from 1 (lower edge of slot n)
constexpr Key kCrossoverType   = 0x21540db3875688f3ull;   // crossover kind, 3 values
constexpr Key kProcessingMode  = 0xf0abeb64a4caf959ull;   // channel-mode index (spelling of the older modules)
constexpr Key kActiveBandView  = 0xad212a744163732eull;   // selected band in the view
constexpr Key kVisibleMeter    = 0x8f16c894bd62e272ull;   // which meter the view shows

// Slot 0 exists by default, slots 1..3 do not (one band; only "1" is ever stored for slots 1..3).
double firstSlotOnly (const ModuleImport&, int number, int) { return number == 0 ? 1.0 : 0.0; }

// Crossover kind: 0 = linear phase, 1 = analog, 2 (default, never stored) = linear phase too: renders of the real
// product (writer 120000, Dynamics and Imager at their default kind) have the peaks of our Digital crossover and its
// latency, not those of an IIR split. The magnitude is flat for every kind, but per-band gain changes (widening,
// compression) sum back with the crossover's phase, so the kind sets the peak level that reaches the limiter.
double crossoverKind (double v, ModuleImport&, int, int)
{
    switch (indexOf (v, 3))
    {
        case 0:  return (double) CrossoverType::Digital;
        case 1:  return (double) CrossoverType::Analog;
        case 2:  return (double) CrossoverType::Digital;
        default: return kNoValue;
    }
}

// Writers before 9000 (the two oldest eras). PluginVer 0 (absent) counts as the newest.
bool olderWriter (const ModuleImport& m) noexcept
{
    const int v = m.pluginVersion();
    return v > 0 && v < 9000;
}

// Times of 0 ms (stored by the foreign writer) are our shortest time: the same thing, not a clamp worth counting.
double attackMs (double v, ModuleImport&, int, int) { return std::max (v, 0.1); }
double releaseMs (double v, ModuleImport&, int, int) { return std::max (v, 1.0); }

// The foreign channel-mode index of this section (stored or its default): non-zero = a two-set mode (M/S or T/S).
bool twoSetMode (ModuleImport& m) { return m.getOr (kProcessingMode, -1, 0, 0.0) >= 0.5; }

// A foreign per-set control that is shared in ours: set A (stored, else the rule default) is written to `id`; a stored
// set B value is read as ignored, with a note when it (stored or defaulted) differs from set A in a two-set file.
// Linked older files inherit set A when set B was omitted. Returns set A's value
// (NaN when neither stored nor defaulted: nothing written).
double sharedFromSetA (ModuleImport& m, Key key, const char* id, Transform xf, const char* noteText,
                       bool copyMissingSetB = false)
{
    const double a = m.get (key, -1, 0);
    if (! std::isnan (a))
    {
        const double ours = xf != nullptr ? xf (a, m, -1, 0) : a;
        if (std::isnan (ours))
            m.reject (key, -1, 0, "value");   // a stored index outside the list (ours kept)
        else
            m.set (id, ours);
    }

    const bool hasSetB = m.has (key, -1, 1);
    m.ignore (key, -1, 1);                     // first, so the read below does not count it as imported
    double b = m.get (key, -1, 1);
    if (! hasSetB && copyMissingSetB)
        b = a;
    if (twoSetMode (m) && ! std::isnan (b) && (std::isnan (a) || std::abs (addFinite (b, -a)) > 1.0e-4))
        m.note (noteText);
    return a;
}

//==============================================================================
// Imager
constexpr Key kImgSectionTag   = 0xfc8530b2fd0f3018ull;   // stereo imager section (tag)
constexpr Key kImgChainName    = 0x51c06f04b2585fbaull;   // stereo imager (ElementID / chain name)
constexpr Key kImgWidth        = 0x15f68410de483196ull;   // band width in %, numbered from 1
constexpr Key kImgStzOn        = 0x12522034ae039942ull;   // stereoize on
constexpr Key kImgStzMode      = 0xb18a6bd91b5f2bc8ull;   // stereoize mode (0 = delay based = our Mode I, 1 = Mode II)
constexpr Key kImgStzDelay     = 0x24996d3dbd6015ceull;   // stereoize delay in ms
constexpr Key kImgRecOn        = 0x52b68e02267bfaaaull;   // recover sides on
constexpr Key kImgRecGain      = 0x4b4762f34dd1a482ull;   // recover sides gain offset in dB
constexpr Key kImgAntiphase    = 0x2e66d03053a47daeull;   // prevent antiphase (never seen stored)
constexpr Key kImgAmount       = 0x987bfebac727bfc3ull;   // module amount in % (never seen stored)
constexpr Key kImgBandDelay    = 0xe793b77d5cf2dd51ull;   // per-band delay (no counterpart)
constexpr Key kImgVectorscope  = 0xc8651bff7c08b32bull;   // vectorscope smoothing (view)
constexpr Key kImgPartView     = 0x395c02b9fcbbb955ull;   // which part (transient / sustain) the view shows

// The stereoize mode default moved between writer versions (sparse storage: only the non-default is ever stored):
// 0 below 9900, 1 from 9900 to 109999, 0 from 110000 on (0 = our Mode I, confirmed for writer 120000).
double imagerStereoizeModeDefault (const ModuleImport& m, int, int)
{
    const int v = m.pluginVersion();
    return (v >= 9900 && v < 110000) ? 1.0 : 0.0;
}

// The foreign delay control (ms) drives our stereoize amount (%): 0..20 ms -> 0..100 % (range assumed).
double imagerDelayToAmount (double ms, ModuleImport&, int, int) { return std::clamp (ms, 0.0, 20.0) * 5.0; }

//------------------------------------------------------------------------------------------------------------------
// IMAGER TABLE. The "def" column is the foreign default of each control (the one place to correct them).
const std::vector<Rule>& imagerRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band          set A id          set B id            transform            foreign default                          what
        { kBandActive,      Act::Hook,         0, 4, Band::Same,   nullptr,          nullptr,            nullptr,             byVersion (firstSlotOnly, Conf::Known),  "band present" },
        { kNumBands,        Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,            nullptr,             likely (4.0),                            "band count (older files)" },
        { kCrossoverCutoff, Act::Hook,         1, 3, Band::Same,   nullptr,          nullptr,            nullptr,             noDefault(),                             "crossover frequency" },
        { kBandBypass,      Act::Hook,         0, 4, Band::Same,   nullptr,          nullptr,            nullptr,             known (0.0),                             "band bypass" },
        { kCrossoverType,   Act::Map,          0, 0, Band::Same,   "~.xoverType",    nullptr,            crossoverKind,       known (2.0),                             "crossover kind" },
        { kImgWidth,        Act::Map,          1, 4, Band::Layout, "~.b#.width",     "~.b.b#.width",     nullptr,             known (0.0),                             "band width" },
        { kImgStzOn,        Act::Map,          0, 0, Band::Same,   "~.stz.on",       "~.b.stz.on",       xf::boolean,         known (0.0),                             "stereoize on" },
        { kImgStzMode,      Act::Map,          0, 0, Band::Same,   "~.stz.mode",     "~.b.stz.mode",     xf::roundIndex,      byVersion (imagerStereoizeModeDefault, Conf::Known), "stereoize mode" },
        { kImgStzDelay,     Act::Map,          0, 0, Band::Same,   "~.stz.amount",   "~.b.stz.amount",   imagerDelayToAmount, known (6.0),                             "stereoize amount" },
        { kImgRecOn,        Act::Map,          0, 0, Band::Same,   "~.rec.on",       "~.b.rec.on",       xf::boolean,         known (0.0),                             "recover sides on" },
        { kImgRecGain,      Act::Map,          0, 0, Band::Same,   "~.rec.gain",     "~.b.rec.gain",     nullptr,             known (0.0),                             "recover sides gain" },
        { kImgAntiphase,    Act::Map,          0, 0, Band::Same,   "~.antiphase",    nullptr,            xf::boolean,         likely (0.0),                            "prevent antiphase" },
        { kImgAmount,       Act::Map,          0, 0, Band::Same,   "~.amount",       nullptr,            nullptr,             known (100.0),                           "module amount" },
        { kProcessingMode,  Act::ChannelMode,  0, 0, Band::Same,   nullptr,          nullptr,            nullptr,             known (0.0),                             "channel mode" },
        { kImgBandDelay,    Act::Unsupported,  1, 4, Band::Same,   nullptr,          nullptr,            nullptr,             noDefault(),                             "per-band delay" },
        { kBandSolo,        Act::Ignore,       0, 4, Band::Same,   nullptr,          nullptr,            nullptr,             noDefault(),                             "band solo" },
        { kActiveBandView,  Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,            nullptr,             noDefault(),                             "selected band" },
        { kVisibleMeter,    Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,            nullptr,             noDefault(),                             "meter view" },
        { kImgVectorscope,  Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,            nullptr,             noDefault(),                             "vectorscope view" },
        { kImgPartView,     Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,            nullptr,             noDefault(),                             "selected part" },
    };
    return rules;
}

void imagerPrepare (ModuleImport& m)
{
    m.readLayout (kBandActive, kNumBands);
    m.writeCrossovers (kCrossoverCutoff);
}

// A bypassed band has no counterpart in ours (no band power): its width becomes 0 (neutral), in both sets, which also
// keeps it out of stereoize / recover sides.
void imagerFinish (ModuleImport& m)
{
    for (int slot = 0; slot < kMaxBands; ++slot)
    {
        const int band = m.layout().bandOfSlot[slot];
        if (band <= 0)
        {
            m.ignore (kBandBypass, slot, 0);
            continue;
        }
        if (m.get (kBandBypass, slot, 0) >= 0.5)
        {
            m.set ("~.b#.width", 0.0, band);
            m.set ("~.b.b#.width", 0.0, band);
        }
    }
}

//==============================================================================
// Shared by the Dynamics and the Exciter
constexpr Key kBandMix         = 0x5b8333c618e04315ull;   // band mix in %, numbered from 1, per set
constexpr Key kSetView         = 0xb6004e16c48d94d3ull;   // which channel set the view shows
constexpr Key kPartView        = kImgPartView;            // which part (transient / sustain) the view shows

// The band layout and crossovers of a multiband section (same templates in every multiband module of the format).
void multibandPrepare (ModuleImport& m)
{
    m.readLayout (kBandActive, kNumBands);
    m.writeCrossovers (kCrossoverCutoff);
}

// A bypassed band: the foreign switch has no set B variant, so it switches the band off in both of our sets. Values
// of removed slots are stale.
void bypassBandsInBothSets (ModuleImport& m)
{
    for (int slot = 0; slot < kMaxBands; ++slot)
    {
        const int band = m.layout().bandOfSlot[slot];
        if (band <= 0)
        {
            m.ignore (kBandBypass, slot, 0);
            continue;
        }
        if (m.getOr (kBandBypass, slot, 0, 0.0) >= 0.5)
        {
            m.set ("~.a.b#.on", 0.0, band);
            m.set ("~.b.b#.on", 0.0, band);
        }
    }
}

//==============================================================================
// Dynamics
constexpr Key kDynSection      = 0xdf75e696671ed527ull;   // dynamics section (tag and chain name are the same)
// per band, numbered from 1 (multiband slot + 1), per set
constexpr Key kDynCompThr      = 0x6bc6f43e63902cb5ull;   // compressor threshold in dB
constexpr Key kDynCompRatio    = 0x02a9e68d3bfa9649ull;   // compressor ratio
constexpr Key kDynCompAtt      = 0xbeeb0197c8a89a76ull;   // compressor attack in ms
constexpr Key kDynCompRel      = 0x35ddf47e6c800c7bull;   // compressor release in ms
constexpr Key kDynCompKnee     = 0xd1cb2accf9898509ull;   // compressor knee, display units 0..10 (= our dB)
constexpr Key kDynLimThr       = 0x572cb2c2e342a9a2ull;   // band limiter threshold in dB
constexpr Key kDynLimRatio     = 0x40303d2a81640962ull;   // band limiter ratio
constexpr Key kDynLimAtt       = 0x43332321a7bb57dfull;   // band limiter attack in ms
constexpr Key kDynLimRel       = 0x203b6cfd9ab2ba8cull;   // band limiter release in ms
constexpr Key kDynLimKnee      = 0x00724232b98d93deull;   // band limiter knee, display units 0..10 (= our dB)
constexpr Key kDynBandGain     = 0xe6fa16efcbd9c742ull;   // band output gain in dB
// module controls (per set where noted)
constexpr Key kDynDetection    = 0xee969af2907d970dull;   // level detection kind, per set
constexpr Key kDynReleaseMode  = 0x37cbecaec58e2192ull;   // release mode, per set (0 = adaptive)
constexpr Key kDynScFilter     = 0x1c68621476103f56ull;   // detection filter kind, per set
constexpr Key kDynScHighPass   = 0x43bbdbbb860176d5ull;   // detection high-pass frequency in Hz, per set
constexpr Key kDynScSlope      = 0x6b34eba5f47f41c4ull;   // detection high-pass slope index, per set
constexpr Key kDynScTilt       = 0x575b1cd545b8e317ull;   // detection tilt amount in %, per set
constexpr Key kDynScLowPass    = 0x66ce7fbb99fdc909ull;   // detection low-pass frequency (no counterpart)
constexpr Key kDynScSolo       = 0xd1f5c71c4012de3eull;   // listen to the detector (monitoring)
constexpr Key kDynLookahead    = 0x1f9cb036134c2953ull;   // lookahead in ms
constexpr Key kDynOutGain      = 0xfec5e397f76d170dull;   // module output gain in dB, per set
constexpr Key kDynOutMix       = 0x1b31250749ce3a14ull;   // module mix in %, per set
constexpr Key kDynAutoGain     = 0x7a9f5dd5877343a1ull;   // automatic gain compensation, per set
constexpr Key kDynSetAOff      = 0x44e37721b5e56276ull;   // channel set A switched off
constexpr Key kDynSetBOff      = 0x078c17e61cf4c457ull;   // channel set B switched off
constexpr Key kDynLinkSets     = 0x8b4fe1477806e44bull;   // older writers: both sets edited together
constexpr Key kDynXoverQ       = 0xbb72e442d015b6aeull;   // linear-phase crossover steepness (no counterpart)
constexpr Key kDynXoverBuffer  = 0xfa4c329ed1fdd192ull;   // linear-phase crossover length (no counterpart)
constexpr Key kDynXoverBufMs   = 0x70b3ed75f215255cull;   // the same length in ms (no counterpart)
constexpr Key kDynLinkBands    = 0x56aa461a655a2ba6ull;   // bands edited together (view)
constexpr Key kDynShowBands    = 0x41b328cffdade7e7ull;   // show every band (view)
constexpr Key kDynCrestLearn   = 0x4ab2335d6c31735eull;   // detector learn state (view)

// The set link switch only exists in the older writers (default off); for newer files ours is kept.
double dynLinkDefault (const ModuleImport& m, int, int) { return olderWriter (m) ? 0.0 : kNoValue; }

// Absent crossovers: the third one shows 10 kHz in the real product (writer 120000, a four-band file storing only the
// first two); the first two stay unknown (ours are kept).
double dynCutoffDefault (const ModuleImport&, int number, int) { return number == 3 ? 10000.0 : kNoValue; }

// Detection kind: 0 (the default) = peak, 1 = RMS, 2 = envelope (confirmed with one test save per kind).
double dynDetection (double v, ModuleImport&, int, int)
{
    switch (indexOf (v, 3))
    {
        case 0:  return (double) DetectionMode::Peak;
        case 1:  return (double) DetectionMode::Rms;
        case 2:  return (double) DetectionMode::Envelope;
        default: return kNoValue;
    }
}

// Release mode: 0 (the default) = adaptive release on, 1 = fixed release.
double dynAdaptive (double v, ModuleImport&, int, int) { return v < 0.5 ? 1.0 : 0.0; }

// Detection tilt amount (%) -> our tilt (dB per octave): 100 % = +3 dB per octave (the pink-noise slope).
double dynTilt (double v, ModuleImport&, int, int) { return v * 0.03; }

// The band rules whose set B value an older writer left out when both sets were linked (set A is copied there).
struct BandCopy
{
    Key key;
    const char* idB;
    Transform xf;
};
const BandCopy kDynLinkedBandValues[] {
    { kDynCompThr,   "~.b.b#.comp.thr",   nullptr },
    { kDynCompRatio, "~.b.b#.comp.ratio", nullptr },
    { kDynCompAtt,   "~.b.b#.comp.att",   attackMs },
    { kDynCompRel,   "~.b.b#.comp.rel",   releaseMs },
    { kDynCompKnee,  "~.b.b#.comp.knee",  nullptr },
    { kDynLimThr,    "~.b.b#.lim.thr",    nullptr },
    { kDynLimRatio,  "~.b.b#.lim.ratio",  nullptr },
    { kDynLimAtt,    "~.b.b#.lim.att",    attackMs },
    { kDynLimRel,    "~.b.b#.lim.rel",    releaseMs },
    { kDynLimKnee,   "~.b.b#.lim.knee",   nullptr },
    { kBandMix,      "~.b.b#.mix",        nullptr },
    { kDynBandGain,  "~.b.b#.gain",       nullptr },
};

//------------------------------------------------------------------------------------------------------------------
// DYNAMICS TABLE. The "def" column is the foreign default of each control (the one place to correct them).
// Band defaults confirmed for writer 120000 (Reset and a new band): compressor -12 dB, 2:1, 20 / 60 ms, knee 10;
// limiter 0 dB, 10:1, 20 / 100 ms, knee 10; mix 100 %, gain 0 dB, peak detection. No file of any writer stores one
// of these exact values, so they hold for every writer.
const std::vector<Rule>& dynamicsRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band          set A id               set B id               transform         foreign default                          what
        { kBandActive,      Act::Hook,         0, 4, Band::Same,   nullptr,               nullptr,               nullptr,          byVersion (firstSlotOnly, Conf::Known),  "band present" },
        { kNumBands,        Act::Hook,         0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          likely (4.0),                            "band count (older files)" },
        { kCrossoverCutoff, Act::Hook,         1, 3, Band::Same,   nullptr,               nullptr,               nullptr,          byVersion (dynCutoffDefault, Conf::Known), "crossover frequency" },
        { kBandBypass,      Act::Hook,         0, 4, Band::Same,   nullptr,               nullptr,               nullptr,          known (0.0),                             "band bypass" },
        { kCrossoverType,   Act::Map,          0, 0, Band::Same,   "~.xoverType",         nullptr,               crossoverKind,    known (2.0),                             "crossover kind" },
        { kDynCompThr,      Act::Map,          1, 4, Band::Layout, "~.a.b#.comp.thr",     "~.b.b#.comp.thr",     nullptr,          known (-12.0),                           "compressor threshold" },
        { kDynCompRatio,    Act::Map,          1, 4, Band::Layout, "~.a.b#.comp.ratio",   "~.b.b#.comp.ratio",   nullptr,          known (2.0),                             "compressor ratio" },
        { kDynCompAtt,      Act::Map,          1, 4, Band::Layout, "~.a.b#.comp.att",     "~.b.b#.comp.att",     attackMs,         known (20.0),                            "compressor attack" },
        { kDynCompRel,      Act::Map,          1, 4, Band::Layout, "~.a.b#.comp.rel",     "~.b.b#.comp.rel",     releaseMs,        known (60.0),                            "compressor release" },
        { kDynCompKnee,     Act::Map,          1, 4, Band::Layout, "~.a.b#.comp.knee",    "~.b.b#.comp.knee",    nullptr,          known (10.0),                            "compressor knee" },
        { kDynLimThr,       Act::Map,          1, 4, Band::Layout, "~.a.b#.lim.thr",      "~.b.b#.lim.thr",      nullptr,          known (0.0),                             "limiter threshold" },
        { kDynLimRatio,     Act::Map,          1, 4, Band::Layout, "~.a.b#.lim.ratio",    "~.b.b#.lim.ratio",    nullptr,          known (10.0),                            "limiter ratio" },
        { kDynLimAtt,       Act::Map,          1, 4, Band::Layout, "~.a.b#.lim.att",      "~.b.b#.lim.att",      attackMs,         known (20.0),                            "limiter attack" },
        { kDynLimRel,       Act::Map,          1, 4, Band::Layout, "~.a.b#.lim.rel",      "~.b.b#.lim.rel",      releaseMs,        known (100.0),                           "limiter release" },
        { kDynLimKnee,      Act::Map,          1, 4, Band::Layout, "~.a.b#.lim.knee",     "~.b.b#.lim.knee",     nullptr,          known (10.0),                            "limiter knee" },
        { kBandMix,         Act::Map,          1, 4, Band::Layout, "~.a.b#.mix",          "~.b.b#.mix",          nullptr,          known (100.0),                           "band mix" },
        { kDynBandGain,     Act::Map,          1, 4, Band::Layout, "~.a.b#.gain",         "~.b.b#.gain",         nullptr,          known (0.0),                             "band gain" },
        { kDynDetection,    Act::Hook,         0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          known (0.0),                             "detection kind" },
        { kDynReleaseMode,  Act::Hook,         0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          known (0.0),                             "release mode" },
        { kDynScFilter,     Act::Hook,         0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          known (0.0),                             "detection filter kind" },
        { kDynScHighPass,   Act::Hook,         0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          guess (100.0),                           "detection filter frequency" },
        { kDynScSlope,      Act::Hook,         0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          likely (1.0),                            "detection filter slope" },
        { kDynScTilt,       Act::Hook,         0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          guess (50.0),                            "detection tilt amount" },
        { kDynAutoGain,     Act::Hook,         0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          known (0.0),                             "auto gain" },
        { kDynOutGain,      Act::Hook,         0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          known (0.0),                             "output gain" },
        { kDynOutMix,       Act::Hook,         0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          known (100.0),                           "module mix" },
        { kDynLookahead,    Act::Map,          0, 0, Band::Same,   "~.lookahead",         nullptr,               nullptr,          noDefault(),                             "lookahead" },
        { kDynSetAOff,      Act::Map,          0, 0, Band::Same,   "~.a.on",              nullptr,               xf::invertBool,   known (0.0),                             "set A off" },
        { kDynSetBOff,      Act::Map,          0, 0, Band::Same,   "~.b.on",              nullptr,               xf::invertBool,   known (0.0),                             "set B off" },
        { kDynLinkSets,     Act::Map,          0, 0, Band::Same,   "~.link",              nullptr,               xf::boolean,      byVersion (dynLinkDefault, Conf::Known), "set link" },
        { kProcessingMode,  Act::ChannelMode,  0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          known (0.0),                             "channel mode" },
        { kDynXoverQ,       Act::Unsupported,  0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "crossover steepness" },
        { kDynXoverBuffer,  Act::Unsupported,  0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "crossover length" },
        { kDynXoverBufMs,   Act::Unsupported,  0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "crossover length in ms" },
        { kDynScLowPass,    Act::Unsupported,  0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "detection low-pass" },
        { kBandSolo,        Act::Ignore,       0, 4, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "band solo" },
        { kDynScSolo,       Act::Ignore,       0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "detector solo" },
        { kActiveBandView,  Act::Ignore,       0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "selected band" },
        { kVisibleMeter,    Act::Ignore,       0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "meter view" },
        { kDynLinkBands,    Act::Ignore,       0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "bands linked" },
        { kDynShowBands,    Act::Ignore,       0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "all bands shown" },
        { kSetView,         Act::Ignore,       0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "selected set" },
        { kDynCrestLearn,   Act::Ignore,       0, 0, Band::Same,   nullptr,               nullptr,               nullptr,          noDefault(),                             "detector learn" },
    };
    return rules;
}

// A band value of one set as the module ends up with it: stored, else (set B of a linked older file) set A's, else the
// rule default.
double dynBandValue (ModuleImport& m, Key key, int number, int set, bool copyA)
{
    if (m.has (key, number, set))
        return m.get (key, number, set);
    if (set == 1 && copyA)
        return m.get (key, number, 0);
    return m.defaultOf (key, number, set);
}

// A module control of one set (stored, else set A's for a linked older file, else the rule default).
double dynSetValue (ModuleImport& m, Key key, int set, bool copyA)
{
    if (m.has (key, -1, set))
        return m.get (key, -1, set);
    if (set == 1 && copyA)
        return m.get (key, -1, 0);
    return m.defaultOf (key, -1, set);
}

void dynamicsFinish (ModuleImport& m)
{
    const auto& layout = m.layout();

    // Older writers with both sets linked left set B out: it takes set A's values.
    const bool copyA = olderWriter (m) && m.getOr (kDynLinkSets, -1, 0, 0.0) >= 0.5;
    if (copyA)
        for (const auto& c : kDynLinkedBandValues)
            for (int slot = 0; slot < kMaxBands; ++slot)
                if (const int band = layout.bandOfSlot[slot]; band > 0 && ! m.has (c.key, slot + 1, 1))
                {
                    const double v = m.get (c.key, slot + 1, 0);
                    if (! std::isnan (v))
                        m.set (c.idB, c.xf != nullptr ? c.xf (v, m, slot + 1, 1) : v, band);
                }

    // Per-set foreign controls that are shared here.
    sharedFromSetA (m, kDynDetection, "~.detect", dynDetection,
                    "Dynamics: the second set's detection kind differs; the first set's is used", copyA);
    sharedFromSetA (m, kDynReleaseMode, "~.adaptRel", dynAdaptive,
                    "Dynamics: the second set's release mode differs; the first set's is used", copyA);
    const double filter = sharedFromSetA (m, kDynScFilter, "~.detFilter", xf::roundIndex,
                                          "Dynamics: the second set's detection filter differs; the first set's is used", copyA);
    sharedFromSetA (m, kDynScHighPass, "~.detFreq", nullptr,
                    "Dynamics: the second set's detection filter differs; the first set's is used", copyA);
    sharedFromSetA (m, kDynScSlope, "~.detSlope", xf::roundIndex,
                    "Dynamics: the second set's detection filter differs; the first set's is used", copyA);
    sharedFromSetA (m, kDynAutoGain, "~.autoGain", xf::boolean,
                    "Dynamics: the second set's auto gain differs; the first set's is used", copyA);

    // The tilt amount acts in the tilt filter kind only (our tilt slope); with any other kind it has no effect in the
    // foreign module either, so a stored value there is inert (ignored, like stale state).
    if (indexOf (filter, 3) == 2)
    {
        sharedFromSetA (m, kDynScTilt, "~.detTilt", dynTilt,
                        "Dynamics: the second set's detection filter differs; the first set's is used", copyA);
    }
    else
    {
        m.ignore (kDynScTilt, -1, 0);
        m.ignore (kDynScTilt, -1, 1);
    }

    // Output gain: set A drives ours (shared); set B's difference goes into its band gains.
    const double gainA = dynSetValue (m, kDynOutGain, 0, copyA);
    const double gainB = dynSetValue (m, kDynOutGain, 1, copyA);
    const double gainDifference = addFinite (gainB, -gainA);
    m.set ("~.gain", gainA);
    if (std::abs (gainDifference) > 1.0e-4)
    {
        for (int slot = 0; slot < kMaxBands; ++slot)
            if (const int band = layout.bandOfSlot[slot]; band > 0)
                m.set ("~.b.b#.gain", addFinite (dynBandValue (m, kDynBandGain, slot + 1, 1, copyA), gainDifference), band);
        if (twoSetMode (m))
            m.note ("Dynamics: the second set's output gain was moved into its band gains");
    }

    // Module mix of each set: folded into that set's band mixes (parallel mixing of the band sum).
    for (int set = 0; set < 2; ++set)
    {
        const double mix = dynSetValue (m, kDynOutMix, set, copyA);
        if (std::isnan (mix) || std::abs (mix - 100.0) < 1.0e-4)
            continue;
        for (int slot = 0; slot < kMaxBands; ++slot)
            if (const int band = layout.bandOfSlot[slot]; band > 0)
                m.set (set == 0 ? "~.a.b#.mix" : "~.b.b#.mix",
                       dynBandValue (m, kBandMix, slot + 1, set, copyA) * (std::clamp (mix, 0.0, 100.0) / 100.0), band);
        m.note ("Dynamics: the module mix was folded into the band mixes");
    }

    // A band limiter is on where its threshold is below 0 dB.
    for (int set = 0; set < 2; ++set)
        for (int slot = 0; slot < kMaxBands; ++slot)
            if (const int band = layout.bandOfSlot[slot]; band > 0)
            {
                const double thr = dynBandValue (m, kDynLimThr, slot + 1, set, copyA);
                m.set (set == 0 ? "~.a.b#.lim.on" : "~.b.b#.lim.on", (! std::isnan (thr) && thr < 0.0) ? 1.0 : 0.0,
                       band);
            }

    for (int slot = 0; slot < kMaxBands; ++slot)
    {
        const int band = layout.bandOfSlot[slot];
        if (band <= 0)
        {
            m.ignore (kBandBypass, slot, 0);
            continue;
        }
        if (m.getOr (kBandBypass, slot, 0, 0.0) < 0.5)
            continue;

        m.set ("~.a.b#.on", 0.0, band);
        if (std::abs (gainDifference) > 1.0e-4)
        {
            // Our band bypass also bypasses its gain. Keep only the set B output-gain correction active.
            m.set ("~.b.b#.on", 1.0, band);
            m.set ("~.b.b#.comp.on", 0.0, band);
            m.set ("~.b.b#.lim.on", 0.0, band);
            m.set ("~.b.b#.mix", 0.0, band);
            m.set ("~.b.b#.gain", gainDifference, band);
        }
        else
        {
            m.set ("~.b.b#.on", 0.0, band);
        }
    }
}

//==============================================================================
// Exciter
constexpr Key kExcSection      = 0x3c9082b8a86da743ull;   // exciter section (tag and chain name are the same)
constexpr Key kExcBandMode     = 0x62fadd95a6120f52ull;   // band saturation model, numbered from 1, per set
constexpr Key kExcAmount       = 0xc7739cff132acf75ull;   // band drive in %, numbered from 1, per set
constexpr Key kExcModeAll      = 0x37cbecaec58e2192ull;   // model of every band, per set (7 = chosen per band)
constexpr Key kExcOversample   = 0x5aa9799c6087e41dull;   // oversampling switch (ours stays on: quality only)
constexpr Key kExcPostFreq     = 0x77bf02674a3b29f6ull;   // post filter frequency in Hz, per set
constexpr Key kExcPostGain     = 0xf388d007723499abull;   // post filter gain in dB, per set
constexpr Key kExcPostQ        = 0xe03a22c5262affdbull;   // post filter width (no counterpart)

// The models are stored in our order (0 analog .. 6 dual triode); 7 in the every-band control = "per band".
constexpr int kExcPerBand = 7;

//------------------------------------------------------------------------------------------------------------------
// EXCITER TABLE. The "def" column is the foreign default of each control (the one place to correct them).
const std::vector<Rule>& exciterRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band          set A id          set B id          transform        foreign default                          what
        { kBandActive,      Act::Hook,         0, 4, Band::Same,   nullptr,          nullptr,          nullptr,         byVersion (firstSlotOnly, Conf::Known),  "band present" },
        { kNumBands,        Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         likely (4.0),                            "band count (older files)" },
        { kCrossoverCutoff, Act::Hook,         1, 3, Band::Same,   nullptr,          nullptr,          nullptr,         noDefault(),                             "crossover frequency" },
        { kBandBypass,      Act::Hook,         0, 4, Band::Same,   nullptr,          nullptr,          nullptr,         known (0.0),                             "band bypass" },
        { kCrossoverType,   Act::Map,          0, 0, Band::Same,   "~.xoverType",    nullptr,          crossoverKind,   likely (2.0),                            "crossover kind" },
        { kExcBandMode,     Act::Hook,         1, 4, Band::Same,   nullptr,          nullptr,          nullptr,         known (5.0),                             "band model" },
        { kExcModeAll,      Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         known (5.0),                             "model of every band" },
        { kExcAmount,       Act::Map,          1, 4, Band::Layout, "~.a.b#.drive",   "~.b.b#.drive",   nullptr,         known (0.0),                             "band drive" },
        { kBandMix,         Act::Map,          1, 4, Band::Layout, "~.a.b#.mix",     "~.b.b#.mix",     nullptr,         known (100.0),                           "band mix" },
        { kExcPostFreq,     Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         noDefault(),                             "post filter frequency" },
        { kExcPostGain,     Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         likely (0.0),                            "post filter gain" },
        { kProcessingMode,  Act::ChannelMode,  0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         known (0.0),                             "channel mode" },
        { kExcPostQ,        Act::Unsupported,  0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         noDefault(),                             "post filter width" },
        { kExcOversample,   Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         noDefault(),                             "oversampling" },
        { kBandSolo,        Act::Ignore,       0, 4, Band::Same,   nullptr,          nullptr,          nullptr,         noDefault(),                             "band solo" },
        { kActiveBandView,  Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         noDefault(),                             "selected band" },
        { kVisibleMeter,    Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         noDefault(),                             "meter view" },
        { kSetView,         Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         noDefault(),                             "selected set" },
        { kPartView,        Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,          nullptr,         noDefault(),                             "selected part" },
    };
    return rules;
}

void exciterFinish (ModuleImport& m)
{
    const auto& layout = m.layout();
    const int numModels = (int) ExciterMode::Count;

    // Band models. The every-band control (stored, else its foreign default) decides: a model index applies to
    // every band; "per band" takes each band's own control (stored, else its default).
    for (int set = 0; set < 2; ++set)
    {
        const char* modeId = set == 0 ? "~.a.b#.mode" : "~.b.b#.mode";
        int all = indexOf (m.get (kExcModeAll, -1, set), kExcPerBand + 1);
        if (all != kExcPerBand && (all < 0 || all >= numModels))
        {
            m.reject (kExcModeAll, -1, set, "value");
            all = indexOf (m.defaultOf (kExcModeAll, -1, set), kExcPerBand + 1);
        }
        const bool perBand = all == kExcPerBand;

        for (int slot = 0; slot < kMaxBands; ++slot)
        {
            const int number = slot + 1;
            const int band = layout.bandOfSlot[slot];
            if (band <= 0 || ! perBand)
            {
                m.ignore (kExcBandMode, number, set);   // a removed band's leftover, or overridden by the every-band model
                if (band > 0)
                    m.set (modeId, all, band);
                continue;
            }

            int model = indexOf (m.getOr (kExcBandMode, number, set, (double) ExciterMode::Triode), numModels);
            if (model < 0 || model >= numModels)
            {
                m.reject (kExcBandMode, number, set, "value");
                model = indexOf (m.defaultOf (kExcBandMode, number, set), numModels);
            }
            m.set (modeId, model, band);
        }
    }

    // Post filter (ours is shared; a gain of 0 dB is no filter).
    const double gainA = m.getOr (kExcPostGain, -1, 0, 0.0);
    const double freqA = m.get (kExcPostFreq, -1, 0);
    m.set ("~.post.gain", gainA);
    m.set ("~.post.on", std::abs (gainA) > 0.01 ? 1.0 : 0.0);
    if (! std::isnan (freqA))
        m.set ("~.post.freq", freqA);

    // Set B is read as ignored (marked first, so reading it does not count it as imported).
    m.ignore (kExcPostGain, -1, 1);
    m.ignore (kExcPostFreq, -1, 1);
    const double gainB = m.getOr (kExcPostGain, -1, 1, 0.0);
    const double freqB = m.get (kExcPostFreq, -1, 1);
    const bool gainsDiffer = std::abs (addFinite (gainB, -gainA)) > 1.0e-4;
    const bool freqsDiffer = (std::abs (gainA) > 0.01 || std::abs (gainB) > 0.01) && ! std::isnan (freqA)
                             && ! std::isnan (freqB) && std::abs (addFinite (freqB, -freqA)) > 0.01;
    if (twoSetMode (m) && (gainsDiffer || freqsDiffer))
        m.note ("Exciter: the second set's post filter differs; the first set's is used");

    bypassBandsInBothSets (m);
}

//==============================================================================
// Maximizer
constexpr Key kMaxSection      = 0x381bea3dc89257a7ull;   // maximizer section (tag and chain name are the same)
constexpr Key kMaxMode         = 0x37cbecaec58e2192ull;   // limiting algorithm index
constexpr Key kMaxStyle3       = 0xeec55b216f1a1efaull;   // style of the aggressive algorithm (0 pumping .. 3 clipping)
constexpr Key kMaxStyle4       = 0x2c9efb89a0aa8d16ull;   // style of the spectral algorithm (0 classic, 1 modern, 2 transient)
constexpr Key kMaxThreshold    = 0xe19cc57dd8675498ull;   // writers before 110000: threshold in dB (negative)
constexpr Key kMaxGain         = 0xb5767f7e05ad941eull;   // writers from 110000: gain in dB
constexpr Key kMaxCeiling      = 0x3f1e7a3f2b283d3bull;   // output ceiling in dBFS
constexpr Key kMaxLink         = 0x2789e20b8343a6e7ull;   // gain and ceiling linked (view)
constexpr Key kMaxCharacter    = 0xabfe23c45dfe3050ull;   // character 0 (fast) .. 10 (slow), our scale
constexpr Key kMaxTruePeak     = 0x6d40b06fa798fd3dull;   // inter-sample peak limiting
constexpr Key kMaxTransOn      = 0x2520106e5fd0b512ull;   // transient emphasis on
constexpr Key kMaxTransAmount  = 0xec392dd1d9c4966bull;   // transient emphasis 0..200
constexpr Key kMaxClipOn       = 0x5426367b4fd1b4b4ull;   // soft clip on
constexpr Key kMaxClipMode     = 0x8f8ae57635250c54ull;   // soft clip kind (0 light, 1 moderate, 2 heavy)
constexpr Key kMaxClipMix      = 0x91d7f24d5dabccbbull;   // soft clip amount in %
constexpr Key kMaxUpwardOn     = 0x59903f07b26e6ee9ull;   // upward compression on
constexpr Key kMaxUpwardAmount = 0x1d4edbb3d2382620ull;   // upward compression in dB
constexpr Key kMaxSiTransient  = 0x2f18d318f35e9003ull;   // stereo independence of the transients in %
constexpr Key kMaxSiSustain    = 0x90d5ca49d51f58abull;   // stereo independence of the sustain in %
constexpr Key kMaxSiLinked     = 0x57952837a5916257ull;   // both independence amounts edited together (view)
constexpr Key kMaxStereoSwitch = 0xee28768d9a3e8d06ull;   // a stereo link switch (never seen stored; meaning unknown)
constexpr Key kMaxLearnTarget  = 0x2c65a981733db09bull;   // loudness target of the gain learn, in dB
constexpr Key kMaxLearnLevel   = 0x5d470e3af9bbb8eeull;   // learn state
constexpr Key kMaxLearnOn      = 0x2e3b9c82899c93fdull;   // learn state
constexpr Key kMaxLearnGr      = 0xe50487396147fdf9ull;   // learn state

// Algorithm index -> our closest mode (the list confirmed for writer 120000 with one test save per algorithm):
// 0 (the default) = our Smooth, 1 Transparent, 2 Aggressive (with its styles), 3 Spectral (with its styles), 4 the
// multiband algorithm = our Multiband, 5 low latency = our LowLatency. Before 120000 the list had no multiband
// algorithm and index 4 was low latency.
double maximizerMode (double v, ModuleImport& m, int, int)
{
    const int version = m.pluginVersion();
    const bool newList = version == 0 || version >= 120000;
    switch (indexOf (v, 6))
    {
        case 0:  return (double) MaximizerMode::Smooth;
        case 1:  return (double) MaximizerMode::Transparent;
        case 2:  return (double) MaximizerMode::Aggressive;
        case 3:  return (double) MaximizerMode::Spectral;
        case 4:  return (double) (newList ? MaximizerMode::Multiband : MaximizerMode::LowLatency);
        case 5:  return newList ? (double) MaximizerMode::LowLatency : kNoValue;
        default: return kNoValue;
    }
}

// Aggressive style: stored 0 pumping, 1 balanced (the default), 2 crisp, 3 clipping; ours runs the other way. The
// spectral style is stored in our order (0 classic, 1 modern = the default, 2 transient). Both confirmed by test saves.
double maximizerStyle3 (double v, ModuleImport&, int, int)
{
    const int i = indexOf (v, 4);
    return (i >= 0 && i <= 3) ? (double) (3 - i) : kNoValue;
}

// Transient emphasis: the foreign control spans 0..200, ours 0..100 %.
double halfScale (double v, ModuleImport&, int, int) { return v * 0.5; }

//------------------------------------------------------------------------------------------------------------------
// MAXIMIZER TABLE. The "def" column is the foreign default of each control (the one place to correct them).
const std::vector<Rule>& maximizerRules()
{
    static const std::vector<Rule> rules {
        //  key              act               first count band          set A id          set B id  transform         foreign default  what
        { kMaxMode,         Act::Map,          0, 0, Band::Same,   "~.mode",         nullptr,  maximizerMode,    known (0.0),     "algorithm" },
        { kMaxStyle3,       Act::Map,          0, 0, Band::Same,   "~.style3",       nullptr,  maximizerStyle3,  known (1.0),     "aggressive style" },
        { kMaxStyle4,       Act::Map,          0, 0, Band::Same,   "~.style4",       nullptr,  xf::roundIndex,   known (1.0),     "spectral style" },
        { kMaxThreshold,    Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,  nullptr,          known (0.0),     "threshold (older files)" },
        { kMaxGain,         Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,  nullptr,          known (0.0),     "gain" },
        { kMaxCeiling,      Act::Map,          0, 0, Band::Same,   "~.ceiling",      nullptr,  nullptr,          known (0.0),     "output ceiling" },
        { kMaxLink,         Act::Map,          0, 0, Band::Same,   "~.link",         nullptr,  xf::boolean,      known (0.0),     "gain and ceiling linked" },
        { kMaxCharacter,    Act::Map,          0, 0, Band::Same,   "~.character",    nullptr,  nullptr,          known (2.0),     "character" },
        { kMaxTruePeak,     Act::Map,          0, 0, Band::Same,   "~.truePeak",     nullptr,  xf::boolean,      known (0.0),     "true peak" },
        { kMaxTransOn,      Act::Map,          0, 0, Band::Same,   "~.trans.on",     nullptr,  xf::boolean,      known (0.0),     "transient emphasis on" },
        { kMaxTransAmount,  Act::Map,          0, 0, Band::Same,   "~.trans.amount", nullptr,  halfScale,        known (0.0),     "transient emphasis" },
        { kMaxClipOn,       Act::Map,          0, 0, Band::Same,   "~.clip.on",      nullptr,  xf::boolean,      known (0.0),     "soft clip on" },
        { kMaxClipMode,     Act::Map,          0, 0, Band::Same,   "~.clip.mode",    nullptr,  xf::roundIndex,   known (1.0),     "soft clip kind" },
        { kMaxClipMix,      Act::Map,          0, 0, Band::Same,   "~.clip.amount",  nullptr,  nullptr,          known (0.0),     "soft clip amount" },
        { kMaxUpwardOn,     Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,  nullptr,          known (0.0),     "upward compression on" },
        { kMaxUpwardAmount, Act::Hook,         0, 0, Band::Same,   nullptr,          nullptr,  nullptr,          known (0.0),     "upward compression" },
        { kMaxSiTransient,  Act::Map,          0, 0, Band::Same,   "~.si.trans",     nullptr,  nullptr,          known (0.0),     "transient independence" },
        { kMaxSiSustain,    Act::Map,          0, 0, Band::Same,   "~.si.sus",       nullptr,  nullptr,          known (0.0),     "sustain independence" },
        { kMaxLearnTarget,  Act::Map,          0, 0, Band::Same,   "~.learnTarget",  nullptr,  nullptr,          known (-11.0),   "learn target" },
        { kMaxStereoSwitch, Act::Unsupported,  0, 0, Band::Same,   nullptr,          nullptr,  nullptr,          noDefault(),     "stereo link switch" },
        { kMaxSiLinked,     Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,  nullptr,          noDefault(),     "independence linked" },
        { kMaxLearnLevel,   Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,  nullptr,          noDefault(),     "learn level" },
        { kMaxLearnOn,      Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,  nullptr,          noDefault(),     "learn switch" },
        { kMaxLearnGr,      Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,  nullptr,          noDefault(),     "learn of gain reduction" },
        { kVisibleMeter,    Act::Ignore,       0, 0, Band::Same,   nullptr,          nullptr,  nullptr,          noDefault(),     "meter view" },
    };
    return rules;
}

void maximizerFinish (ModuleImport& m)
{
    // Drive into the limiter: the newer gain control, else minus the older threshold (never both in one file).
    double drive = 0.0;
    if (m.has (kMaxGain))
    {
        drive = m.get (kMaxGain);
        m.ignore (kMaxThreshold);
    }
    else if (m.has (kMaxThreshold))
    {
        drive = -m.get (kMaxThreshold);
    }
    // The foreign output level also turns the whole signal down (limit at 0 dBFS, then the level: measured on renders
    // of the real product, where the most gain any passage gets is gain + level). Ours is only a ceiling, so lowering
    // the drive by the level gives the same output (our limiter is relative to its ceiling).
    const double level = std::min (0.0, m.getOr (kMaxCeiling, -1, 0, 0.0));
    if (drive >= 0.0 && drive + level < 0.0)
        m.note ("Maximizer: an output level below the gain cannot be matched exactly; quiet passages stay louder");
    m.set ("~.gain", drive + level);   // below 0: clamped and counted

    // Upward compression: its amount only counts while it is on.
    if (m.getOr (kMaxUpwardOn, -1, 0, 0.0) >= 0.5)
    {
        m.set ("~.upward", m.getOr (kMaxUpwardAmount, -1, 0, 0.0));
    }
    else
    {
        m.ignore (kMaxUpwardAmount);
        m.set ("~.upward", 0.0);
    }
}

//==============================================================================
// Global section: I/O switches and the chain blob (the blob is read by the engine).
constexpr Key kGlobalSection   = 0x681c086b8f7afcaeull;   // global section (tag and ElementID)
constexpr Key kGlobalGainMatch = 0x18bed2c2d861ace9ull;   // automatic gain match on
constexpr Key kGlobalStemMode  = 0x8685e6b310ea85bbull;   // stem target (never built)

const std::vector<Rule>& globalRules()
{
    static const std::vector<Rule> rules {
        // Gain match is a listening aid here: presets never change it (ARCHITECTURE "monitoring parameters").
        { kGlobalGainMatch, Act::Ignore,      0, 0, Band::Same, nullptr, nullptr, nullptr, known (0.0),  "gain match" },
        { kGlobalStemMode,  Act::Unsupported, 0, 0, Band::Same, nullptr, nullptr, nullptr, known (0.0),  "stem target" },
    };
    return rules;
}
} // namespace

//==============================================================================
const std::vector<ModuleSpec>& coreFamilySpecs()
{
    static const std::vector<ModuleSpec> specs {
        { ModuleType::Dynamics,  kDynSection,    kDynSection,   &dynamicsRules(),  multibandPrepare, dynamicsFinish },
        { ModuleType::Imager,    kImgSectionTag, kImgChainName, &imagerRules(),    imagerPrepare,    imagerFinish },
        { ModuleType::Exciter,   kExcSection,    kExcSection,   &exciterRules(),   multibandPrepare, exciterFinish },
        { ModuleType::Maximizer, kMaxSection,    kMaxSection,   &maximizerRules(), nullptr,          maximizerFinish },
    };
    return specs;
}

const ModuleSpec& globalSpec()
{
    static const ModuleSpec spec { ModuleType::Count, kGlobalSection, kGlobalSection, &globalRules(), nullptr, nullptr };
    return spec;
}
} // namespace ps::imp
