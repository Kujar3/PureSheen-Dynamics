#pragma once

#include "dsp/Types.h"

#include <string>
#include <utility>
#include <vector>

// The parameter table (single source of truth for ids, names, ranges, defaults) and the typed parameter structs the
// DSP modules consume. Implementation: dsp/ParamTable.cpp.
//
// The plugin builds its APVTS from paramSpecs() and, once per audio block, copies every parameter's current plain value
// into a float array laid out in paramSpecs() order (ParamValues). The read* functions turn that array into the typed
// structs below (realtime safe: no allocation, indices precomputed at static init).
//
// ID scheme (stable, stored by hosts; NEVER rename, only append):
//   global          "in.gainL", "out.gainR", "io.bypass", ...
//   module          "<prefix>.<name>"                         e.g. "dyn.bands", "max.mode"
//   module + set    "<prefix>.<a|b>.<name>"                   e.g. "exc.b.b2.drive"
//   band            "<prefix>[.<a|b>].b<1..N>.<name>"         e.g. "eq1.a.b3.freq"
//   T/S split       "<prefix>.ts.sens" / "<prefix>.ts.len"     every module that offers Transient/Sustain
// Order: the 0.1.1 table (I/O, the six Core modules, Dither) is frozen; phase 2 appends after "dith.dc": first the
// Core T/S additions (eq1 / eq2 ts params, the Imager channel slot and its set B, "exc.ts.on" and the exc ts params),
// then each phase 2 module in ModuleType order (slot params, then its own params in struct order). The vintage-style
// modules (phase 3) append after "bass.autoGain" the same way (ccmp, peq, clim, tape), then the De-limiter (dlim).
// tests/ParamCompatTests.cpp guards the 0.1.1 part (index, id, name, range, default, choices of all 479 entries), the
// 0.2 - 0.5 table (841 entries), the vintage appendix and the De-limiter appendix (count + fingerprint each).
// Host-visible names: "<shortName> <Band n> <Name> [(A)|(B)]", e.g. "EQ 1 Band 3 Freq (A)". Set B is labelled "(B)";
// the UI shows A/B as "Mid"/"Side", "Left"/"Right" or "Transient"/"Sustain" depending on the channel mode.
namespace ps
{
enum class ParamKind { Float, Choice, Bool };

struct ParamSpec
{
    std::string id;
    std::string name;              // host-visible
    ParamKind kind = ParamKind::Float;
    float minValue = 0.0f, maxValue = 1.0f, defaultValue = 0.0f;
    float skewCentre = 0.0f;       // Float: value at the middle of the host's 0..1 range (0 = linear)
    float interval = 0.0f;         // Float: step (0 = continuous)
    std::vector<std::string> choices;   // Choice: labels (index = value)
    std::string unit;              // "dB", "Hz", "ms", "%", ":1", ""
    int module = -1;               // (int) ModuleType, -1 = global
};

const std::vector<ParamSpec>& paramSpecs();          // fixed order; append-only across versions
inline constexpr int kNumParams_0_1_1 = 479;         // entries of the released 0.1.1 table (dith.dc is the last)
inline constexpr int kNumParams_0_5_0 = 841;         // entries of the 0.2 .. 0.5 table: 0.1.1 + the phase 2 appendix
inline constexpr int kNumParams_0_6_0 = 920;         // entries of the 0.6 table: 0.5 + the vintage appendix
                                                     //   (bass.autoGain is the last); the vintage appendix follows
int paramIndex (const std::string& id) noexcept;     // -1 when unknown (not realtime: string lookup)

// Plain values, one per paramSpecs() entry (choices / bools as 0, 1, 2 ...).
using ParamValues = const float*;

//==============================================================================
// Global I/O
struct IoParams
{
    float inputGainDb[2] { 0, 0 };    // L, R    "in.gainL" "in.gainR"     -24..+24 dB
    float outputGainDb[2] { 0, 0 };   //         "out.gainL" "out.gainR"   -24..+24 dB
    bool bypass = false;              // "io.bypass"
    bool gainMatch = false;           // "io.gainMatch"
    bool gainMatchModern = false;     // "io.gainMatchModern": false = level the bypassed signal (classic),
                                      //   true = level the processed output to the input
    bool sumToMono = false;           // "io.mono"
    bool swapChannels = false;        // "io.swap"
};
void readIoParams (ParamValues, IoParams&) noexcept;

// Per-module slot controls the host applies around every module.
// Modules with a single channel mode (Maximizer, Bass Shaper, Classic Limiter, Tape: Stereo only) have only
// "<prefix>.on": no chMode (a choice needs >= 2 entries), no a.on / b.on / link (no effect in Stereo; and "max.link" /
// "clim.link" are the limiters' own Gain/Output and Threshold/Ceiling links). readSlotParams then returns Stereo,
// both channels on, link on. (The Imager was single-mode in
// 0.1.x; its "img.chMode" / "img.a.on" / "img.b.on" / "img.link" are appended phase 2 entries.)
struct SlotParams
{
    bool power = true;                // "<prefix>.on"
    ChannelMode channelMode = ChannelMode::Stereo;   // "<prefix>.chMode" (choices the module supports, see below)
    bool channelOn[kNumChannelSets] { true, true };  // "<prefix>.a.on" "<prefix>.b.on" (M/S, L/R, T/S)
    bool channelLink = true;          // "<prefix>.link": UI edits A and B together (no DSP effect)
    // (phase 2) Transient/Sustain split, only used while channelMode == TransientSustain (defaults when the module has
    // no T/S): "<prefix>.ts.sens" 0..100 % (higher = smaller level jumps count as hits), "<prefix>.ts.len" 5..100 ms
    // log (how long the transient part lasts after an onset).
    float tsSensitivity = 50.0f;
    float tsLengthMs = 20.0f;
};
void readSlotParams (ParamValues, ModuleType, SlotParams&) noexcept;

// Channel modes each module offers (reference-suite parity), Stereo first, then M/S, L/R, T/S where offered:
//   Equalizer 1 / 2, Dynamic EQ, Match EQ, Passive EQ                      Stereo, M/S, L/R, T/S
//   Exciter, Tone Balance, Definition, Smoother, Low Contrast              Stereo, M/S, T/S
//   Dynamics, Punch, Classic Compressor                                    Stereo, M/S
//   Imager                                                                 Stereo, T/S
//   Maximizer, Bass Shaper, Classic Limiter, Tape                          Stereo
std::vector<ChannelMode> supportedChannelModes (ModuleType);

// (phase 2) The modes of the "<prefix>.chMode" choice, in choice order (empty for single-mode modules). Equal to
// supportedChannelModes except for the Exciter: its released choice list is { Stereo, Mid/Side }, and its
// Transient/Sustain mode is the appended switch "exc.ts.on" (on = T/S, overrides exc.chMode), so host automation of
// exc.chMode recorded with 0.1.x keeps its meaning.
std::vector<ChannelMode> channelModeChoices (ModuleType);

// (phase 2) The parameter values that select `mode` for a module ("<prefix>.chMode" = choice index, plus "exc.ts.on"
// for the Exciter). For the UI's channel-mode segments (one undo step) and the preset importer. Empty when the module
// does not offer the mode.
std::vector<std::pair<std::string, float>> channelModeParamValues (ModuleType, ChannelMode);

// (phase 2) true when the module offers Transient/Sustain (it then has "<prefix>.ts.sens" / "<prefix>.ts.len" and the
// chain builds a twin instance for it).
bool supportsTransientSustain (ModuleType) noexcept;

//==============================================================================
// Equalizer ("eq1." / "eq2.")
struct EqBandParams
{
    bool on = false;                  // "b<n>.on"      bands start off; the UI "adds" a node by turning one on
    EqShape shape = EqShape::Bell;    // "b<n>.shape"
    float freqHz = 1000.0f;           // "b<n>.freq"    20..20000, log
    float gainDb = 0.0f;              // "b<n>.gain"    -30..+15
    float q = 0.707f;                 // "b<n>.q"       0.1..24, log (shelves: shelf Q; Baxandall / Brickwall ignore)
    EqSlope slope = EqSlope::Db12;    // "b<n>.slope"   HP / LP Flat + Resonant
    float phasePercent = 0.0f;        // "b<n>.phase"   Digital only: 0 = linear phase .. 100 = minimum phase
};
struct EqParams
{
    EqFilterMode filterMode = EqFilterMode::Analog;   // "mode"
    float amountPercent = 100.0f;     // "amount"  0..200, scales every band's gain; scaled gain clamped to -30..+15
    bool softSaturation = false;      // "softSat" analog-style soft clipping applied to boosts only
    int firSizeIndex = 1;             // "firSize" choice 4096 / 8192 / 16384 / 32768 taps at 44.1/48k (doubled at
                                      //   88.2/96k, x4 at 176.4/192k)
    EqBandParams bands[kNumChannelSets][kEqBands];    // "<a|b>.b<1..8>.<name>"
};
void readEqParams (ParamValues, ModuleType eq1or2, EqParams&) noexcept;

//==============================================================================
// Dynamics ("dyn.")
struct CompressorParams
{
    bool on = true;                   // "comp.on"
    float thresholdDb = 0.0f;         // "comp.thr"     -60..0
    float ratio = 2.0f;               // "comp.ratio"   -10..30 ; |ratio| < 1 behaves as 1:1; negative = upward
                                      //                compression by |ratio| (quiet material raised toward thr)
    float attackMs = 10.0f;           // "comp.att"     0.1..500, log
    float releaseMs = 100.0f;         // "comp.rel"     1..5000, log
    float kneeDb = 6.0f;              // "comp.knee"    0..30
};
struct LimiterBandParams
{
    bool on = false;                  // "lim.on"
    float thresholdDb = 0.0f;         // "lim.thr"      -60..0
    float ratio = 10.0f;              // "lim.ratio"    -2.5..30 ; negative = upward expansion by |ratio|
    float attackMs = 1.0f;            // "lim.att"      0.1..100, log
    float releaseMs = 50.0f;          // "lim.rel"      1..2000, log
    float kneeDb = 0.0f;              // "lim.knee"     0..30
};
struct DynamicsBandParams
{
    bool on = true;                   // "b<n>.on" (band bypass)
    CompressorParams comp;            // "b<n>.comp.*"
    LimiterBandParams lim;            // "b<n>.lim.*"
    float parallelPercent = 100.0f;   // "b<n>.mix"     0..100 (wet)
    float gainDb = 0.0f;              // "b<n>.gain"    -24..+24 (band output gain)
};
struct DynamicsParams
{
    int numBands = 1;                 // "bands"        1..4
    float crossoverHz[kMaxCrossovers] { 120.0f, 1000.0f, 6000.0f };   // "xover1..3" 20..20000 log, kept ascending
    CrossoverType crossoverType = CrossoverType::Analog;              // "xoverType"
    DetectionMode detection = DetectionMode::Envelope;                // "detect"
    DetectionFilter detectionFilter = DetectionFilter::None;          // "detFilter"
    float detectionFilterHz = 100.0f;                                 // "detFreq"  20..2000 (HP corner / tilt pivot)
    int detectionFilterSlopeIndex = 1;                                // "detSlope" 6 / 12 / 24 / 48 dB per octave
    float detectionTiltDb = 0.0f;                                     // "detTilt"  -6..+6 dB per octave
    float lookaheadMs = 0.0f;                                         // "lookahead" 0..10
    bool adaptiveRelease = false;                                     // "adaptRel"
    bool autoGain = false;                                            // "autoGain"
    float outputGainDb = 0.0f;                                        // "gain" -24..+24
    DynamicsBandParams bands[kNumChannelSets][kMaxBands];             // "<a|b>.b<n>.*"
};
void readDynamicsParams (ParamValues, DynamicsParams&) noexcept;

//==============================================================================
// Imager ("img.")
// (phase 2) Per-set part of the Imager for Transient/Sustain: set A = the 0.1.x ids ("b<n>.width", "stz.*", "rec.*"),
// set B = the appended "b.b<n>.width", "b.stz.*", "b.rec.*" (default neutral). Bands, crossovers, amount and antiphase
// are shared (latency / band edges identical in both parts).
struct ImagerSetParams
{
    float widthPercent[kMaxBands] { 0, 0, 0, 0 };
    bool stereoizeOn = false;
    StereoizeMode stereoizeMode = StereoizeMode::ModeII;
    float stereoizeAmount = 0.0f;
    bool recoverSidesOn = false;
    float recoverSidesGainDb = 0.0f;
};

struct ImagerParams
{
    int numBands = 1;                 // "bands" 1..4
    float crossoverHz[kMaxCrossovers] { 120.0f, 1000.0f, 6000.0f };   // "xover1..3"
    CrossoverType crossoverType = CrossoverType::Analog;             // "xoverType" (Analog default: 0 latency, bands phase-aligned)
    float widthPercent[kMaxBands] { 0, 0, 0, 0 };                    // "b<n>.width" -100..+100
    float amountPercent = 100.0f;     // "amount" 0..100 (scales every band's width)
    bool stereoizeOn = false;         // "stz.on"   only acts on bands with width > 0
    StereoizeMode stereoizeMode = StereoizeMode::ModeII;              // "stz.mode"
    float stereoizeAmount = 0.0f;     // "stz.amount" 0..100
    bool recoverSidesOn = false;      // "rec.on"   only acts on bands with width < 0
    float recoverSidesGainDb = 0.0f;  // "rec.gain" -12..+12
    bool preventAntiphase = false;    // "antiphase"  keeps correlation >= 0 (limits side above mid)
    ImagerSetParams setB;             // (phase 2) "b.b<n>.width", "b.stz.on|mode|amount", "b.rec.on|gain"
};
void readImagerParams (ParamValues, ImagerParams&) noexcept;
// (phase 2) The fields of set A (widthPercent, stereoize*, recoverSides*) as an ImagerSetParams, and the reverse: an
// Imager twin (getParamSet() == 1) runs with useSet (p, p.setB).
ImagerSetParams imagerSetA (const ImagerParams&) noexcept;
void useImagerSet (ImagerParams&, const ImagerSetParams&) noexcept;

//==============================================================================
// Exciter ("exc.")
struct ExciterBandParams
{
    bool on = true;                   // "b<n>.on"
    ExciterMode mode = ExciterMode::Warm;   // "b<n>.mode"
    float drivePercent = 0.0f;        // "b<n>.drive" 0..100
    float mixPercent = 100.0f;        // "b<n>.mix"   0..100
};
struct ExciterParams
{
    int numBands = 1;                 // "bands"
    float crossoverHz[kMaxCrossovers] { 120.0f, 1000.0f, 6000.0f };
    CrossoverType crossoverType = CrossoverType::Analog;
    bool oversampling = true;         // "os"     4x
    bool postFilterOn = false;        // "post.on"   high shelf on the wet (excited) signal only
    float postFilterHz = 8000.0f;     // "post.freq" 1000..20000
    float postFilterGainDb = 0.0f;    // "post.gain" -12..+12
    ExciterBandParams bands[kNumChannelSets][kMaxBands];   // "<a|b>.b<n>.*"
};
void readExciterParams (ParamValues, ExciterParams&) noexcept;

//==============================================================================
// Maximizer ("max.")
struct MaximizerParams
{
    MaximizerMode mode = MaximizerMode::Transparent;   // "mode"
    AggressiveStyle aggressiveStyle = AggressiveStyle::Balanced;   // "style3"
    SpectralStyle spectralStyle = SpectralStyle::Modern;           // "style4"
    float gainDb = 0.0f;              // "gain"     0..+20 (drive into the limiter)
    float outputLevelDb = -1.0f;      // "ceiling"  -20..0 dBFS
    bool linkGainOutput = false;      // "link"     UI: moving one moves the other inversely (no DSP effect)
    bool truePeak = true;             // "truePeak"
    float character = 3.0f;           // "character" 0 (clipping, fastest) .. 10 (very slow)
    float upwardCompressDb = 0.0f;    // "upward"   0..12 (max gain applied to quiet passages, level-matched at 0 dBFS)
    bool softClipOn = false;          // "clip.on"
    SoftClipMode softClipMode = SoftClipMode::Light;   // "clip.mode"
    float softClipAmount = 100.0f;    // "clip.amount" 0..100 (wet)
    bool transientEmphasisOn = false; // "trans.on"
    float transientEmphasis = 0.0f;   // "trans.amount" 0..100
    float stereoIndependenceTransient = 0.0f;   // "si.trans"  0..100 (0 = fully linked)
    float stereoIndependenceSustain = 0.0f;     // "si.sus"    0..100
    float learnTargetLufs = -10.0f;   // "learnTarget" -24..-5 LUFS (target of Learn Input Gain)
};
void readMaximizerParams (ParamValues, MaximizerParams&) noexcept;

//==============================================================================
// Dither ("dith.", output stage, not a chain module)
struct DitherParams
{
    bool on = false;                  // "dith.on"
    DitherBits bits = DitherBits::Bits16;          // "dith.bits"
    DitherAmount amount = DitherAmount::Medium;    // "dith.amount"
    bool harmonicSuppression = false; // "dith.harm"   only when amount == Off
    float noiseShaping = 50.0f;       // "dith.shape"  0 (off) .. 100 (max, ~14 dB audible-band suppression)
    bool limitPeaks = true;           // "dith.limit"
    bool autoBlank = true;            // "dith.blank"  mute after >= 0.7 s of digital silence on the input
    bool dcFilter = false;            // "dith.dc"     1 Hz high-pass ahead of the Maximizer (host applies it there)
};
void readDitherParams (ParamValues, DitherParams&) noexcept;

//==============================================================================
//==============================================================================
// Phase 2 modules. Every module offering T/S also has "<prefix>.ts.sens" / ".ts.len" (SlotParams).
// Neutral defaults: every module is transparent at its defaults (null < -120 dBFS against the input delayed by its
// latency).

//==============================================================================
// Dynamic EQ ("deq.")
struct DynamicEqBandParams
{
    bool on = false;                  // "b<n>.on"       off = that band is skipped (bit-exact passthrough)
    DynEqShape shape = DynEqShape::ProportionalQ;   // "b<n>.shape"
    float freqHz = 1000.0f;           // "b<n>.freq"     20..20000 log; defaults 60 / 200 / 500 / 1500 / 4000 / 10000
    float gainDb = 0.0f;              // "b<n>.gain"     -30..+15: the band's full dynamic gain (direction rules)
    float q = 0.707f;                 // "b<n>.q"        0.1..24 log (Baxandall shelves ignore it)
    DynEqDirection direction = DynEqDirection::Down;  // "b<n>.dir"
    float thresholdDb = -30.0f;       // "b<n>.thr"      -130..0 (skew centre -30): detector level where the move starts
    float ratio = 2.0f;               // "b<n>.ratio"    1..20 log, ":1": dB of movement per dB over = 1 - 1/ratio
    float kneeDb = 6.0f;              // "b<n>.knee"     0..24 (centred on the threshold)
    float attackMs = 15.0f;           // "b<n>.att"      0.1..500 log (Auto Scale on: scales the frequency curve, 15 = as is)
    float releaseMs = 100.0f;         // "b<n>.rel"      1..5000 log (Auto Scale on: scales the frequency curve, 100 = as is)
    bool autoScale = true;            // "b<n>.auto"     times follow dynEqAutoAttackMs / dynEqAutoReleaseMs (DynamicEq.h)
    float offsetDb = 0.0f;            // "b<n>.offset"   -15..+15 static gain added (total clamped to -30..+15)
    bool dynamic = true;              // "b<n>.dyn"      false = static band at gain + offset
    DynEqSidechain sidechain = DynEqSidechain::Own;   // "b<n>.sc"
};
struct DynamicEqParams
{
    EqFilterMode filterMode = EqFilterMode::Analog;   // "mode"       Analog (0 latency) / Digital (linear phase)
    int firSizeIndex = 1;             // "firSize"   4096 / 8192 / 16384 / 32768 at 44.1/48 kHz (x2 / x4 above)
    float lookaheadMs = 0.0f;         // "lookahead" 0..10 ms (audio delayed, detector not; adds to the latency)
    DynamicEqBandParams bands[kNumChannelSets][kDynEqBands];   // "<a|b>.b<1..6>.<name>"
};
void readDynamicEqParams (ParamValues, DynamicEqParams&) noexcept;

//==============================================================================
// Match EQ ("meq.")
struct MatchEqSetParams
{
    float amountPercent = 50.0f;      // "amount"  0..100: scales the matched curve (dB)
    float smoothPercent = 50.0f;      // "smooth"  0..100: smoothing width (1/48 oct .. 2 oct, see MatchAnalysis.h)
    float lowHz = 20.0f;              // "lo"      20..20000 log: start of the processed region
    float highHz = 20000.0f;          // "hi"      20..20000 log: end (readMatchEqParams keeps hi >= lo * 2^(1/6))
    float maxBoostDb = 12.0f;         // "boost"   0..24: soft limit of positive gains
    float maxCutDb = 12.0f;           // "cut"     0..24: soft limit of negative gains
};
struct MatchEqParams
{
    MatchPhase phase = MatchPhase::Linear;   // "phase"    Linear / Minimum (minimum = latency of one partition)
    int firSizeIndex = 2;             // "firSize"  4096 / 8192 / 16384 / 32768 at 44.1/48 kHz (x2 / x4 above)
    MatchEqSetParams sets[kNumChannelSets];  // "<a|b>.<name>"
};
void readMatchEqParams (ParamValues, MatchEqParams&) noexcept;

//==============================================================================
// Tone Balance ("tbal.")
struct ToneBalanceSetParams
{
    float amountPercent = 0.0f;       // "amount"   0..100 (0 = transparent)
    float speed = 50.0f;              // "speed"    0..100 (Shape / Boost)
    float smoothing = 50.0f;          // "smooth"   0..100 (Shape / Boost)
    float cutSpeed = 50.0f;           // "cutSpeed" 0..100 (Cut)
    float sensitivity = 50.0f;        // "sens"     0..100 (Cut)
    bool tameTransients = false;      // "tame"
    float lowPercent = 100.0f;        // "low"      0..100: correction below 100 Hz
    float midPercent = 100.0f;        // "mid"      0..100: 100 Hz .. 5.6 kHz
    float highPercent = 100.0f;       // "high"     0..100: above 5.6 kHz
};
struct ToneBalanceParams
{
    ToneTarget target = ToneTarget::Balanced;        // "target"
    ToneBalanceMode mode = ToneBalanceMode::Shape;   // "mode"
    ToneBalanceSetParams sets[kNumChannelSets];      // "<a|b>.<name>"
};
void readToneBalanceParams (ParamValues, ToneBalanceParams&) noexcept;

//==============================================================================
// Definition ("defn.")
struct DefinitionSetParams
{
    float amountPercent = 0.0f;       // "amount" 0..100 (0 = transparent)
    float tiltDbPerOct = 0.0f;        // "tilt"   -6..+6 dB/oct target relative to pink (+ brighter)
    float attackMs = 150.0f;          // "att"    5..1000 log
    float releaseMs = 120.0f;         // "rel"    10..1000 log
    float lowHz = 300.0f;             // "low"    300..20000 log: region start
    float highHz = 20000.0f;          // "high"   300..20000 log (readDefinitionParams keeps high >= low * 2^(1/3))
};
struct DefinitionParams
{
    DefinitionSetParams sets[kNumChannelSets];       // "<a|b>.<name>"
};
void readDefinitionParams (ParamValues, DefinitionParams&) noexcept;

//==============================================================================
// Smoother ("smth.")
struct SmootherSetParams
{
    float amountPercent = 0.0f;       // "amount" 0..100 (0 = transparent; higher = lower effective threshold)
    SmootherMode mode = SmootherMode::Medium;   // "mode" Gentle / Medium / Strong
    float attackMs = 8.0f;            // "att"    1..100 log
    float releaseMs = 30.0f;          // "rel"    2..500 log
    float tonePercent = 0.0f;         // "tone"   -100..+100 (+ = less reduction of the highs, brighter)
    float lowHz = 2000.0f;            // "low"    20..20000 log: region start
    float highHz = 12000.0f;          // "high"   20..20000 log (readSmootherParams keeps high >= low * 2^(1/6))
};
struct SmootherParams
{
    SmootherSetParams sets[kNumChannelSets];         // "<a|b>.<name>"
};
void readSmootherParams (ParamValues, SmootherParams&) noexcept;

//==============================================================================
// Punch ("pnch.")
struct PunchBandParams
{
    bool on = true;                   // "b<n>.on"
    float amountPercent = 0.0f;       // "b<n>.amount" -100..+100 (+ = punchier, - = denser)
};
struct PunchParams
{
    int numBands = 1;                 // "bands"     1..4 (1 = no split: neutral is bit-exact)
    float crossoverHz[kMaxCrossovers] { 120.0f, 1000.0f, 6000.0f };   // "xover1..3"
    CrossoverType crossoverType = CrossoverType::Analog;              // "xoverType"
    float amountPercent = 100.0f;     // "amount"    0..100: scales every band's amount
    float envelopeMs = 100.0f;        // "env"       10..1000 log (long envelope; short = env / 10, attack env / 100)
    bool sync = false;                // "sync"      envelope from the host tempo
    int syncNote = 7;                 // "syncNote"  index into kPunchSyncFractions (7 = 1/16)
    bool stereoLink = false;          // "stLink"    Stereo mode: one gain for both channels from the mid
    bool autoGain = false;            // "autoGain"  per-band make-up keeps the long-term level
    bool linkBands = false;           // "bandLink"  UI only: dragging one band's amount moves all (no DSP effect)
    PunchBandParams bands[kNumChannelSets][kMaxBands];               // "<a|b>.b<n>.*"
};
void readPunchParams (ParamValues, PunchParams&) noexcept;

//==============================================================================
// Low Contrast ("lowc.")
struct LowContrastSetParams
{
    LowContrastMode mode = LowContrastMode::Punchy;  // "mode"
    float contrastPercent = 0.0f;     // "contrast" -100..+100
    float gainDb = 0.0f;              // "gain"     -12..+12: make-up gain of the region
};
struct LowContrastParams
{
    float lowHz = 20.0f;              // "low"      20..300 log: region start (shared by both sets)
    float highHz = 250.0f;            // "high"     20..300 log (readLowContrastParams keeps high >= low * 2^(1/3))
    bool autoGain = false;            // "autoGain"
    LowContrastSetParams sets[kNumChannelSets];      // "<a|b>.<name>"
};
void readLowContrastParams (ParamValues, LowContrastParams&) noexcept;

//==============================================================================
// Bass Shaper ("bass.", Stereo only: slot = "bass.on")
struct BassShaperParams
{
    float cutoffHz = 120.0f;          // "cutoff"         65..300 log
    float balanceDb = 0.0f;           // "balance"        -12..+12 (level below the cutoff)
    float punchDb = 0.0f;             // "punch"          -12..+12 (level of the low-end hits)
    bool sustainOn = false;           // "sus.on"
    float sustainThresholdDb = -18.0f;        // "sus.thr"    -40..0
    float sustainAmountPercent = 50.0f;       // "sus.amount" 0..100
    float sustainCharacterPercent = 100.0f;   // "sus.char"   0 = Dirty .. 100 = Clean
    bool peakOn = false;              // "peak.on"
    float peakThresholdDb = -6.0f;            // "peak.thr"   -30..0
    float peakCharacterPercent = 100.0f;      // "peak.char"  0 = Dirty (clip) .. 100 = Clean (limit)
    float peakMixPercent = 100.0f;            // "peak.mix"   0..100
    bool autoGain = false;            // "autoGain"
};
void readBassShaperParams (ParamValues, BassShaperParams&) noexcept;

//==============================================================================
//==============================================================================
// Vintage-style modules (phase 3). Ranges, defaults, choice lists and units are the reference's (confirmed from the
// user's screenshots, min-max notes and saved presets), so every control shows the number the reference's control
// shows for the same setting; the few values still marked (TBC) were read off a screenshot curve or not seen at an
// end stop. The parameter table keeps them in one place (ParamTable.cpp reads these struct defaults). Unlike the
// phase 2 modules, their defaults are the reference's defaults, not neutral settings: the neutral settings are
// documented in each module's header.

//==============================================================================
// Classic Compressor ("ccmp.", Stereo / M/S; per channel set "<a|b>.<name>", Stereo uses set A)
struct ClassicCompSetParams
{
    float thresholdDb = -12.0f;       // "thr"        -60..0
    float ratio = 1.5f;               // "ratio"      1..20, skew centre 4, ":1"
    float attackMs = 20.0f;           // "attack"     0.1..100 log
    float releaseMs = 50.0f;          // "release"    30..150 log: the transient release; sustained reduction
                                      //              releases slower (ClassicComp.h)
    ClassicCompMode mode = ClassicCompMode::Balanced;   // "mode" Sharp / Balanced / Smooth
    float makeupDb = 0.0f;            // "makeup"     -30..+30 (the reference's Gain)
    bool autoGain = true;             // "autoGain"
    float scHighPassHz = 40.0f;       // "sc.hpf"     20..400 log: high-pass on the detector, fixed shape (40: measured)
    float scBellHz = 2000.0f;         // "sc.bellHz"  800..9000 log: wide bell on the detector, fixed shape (2 k: TBC)
    float scBellDb = 0.0f;            // "sc.bellDb"  -8..+8
    float scShelfHz = 6000.0f;        // "sc.shelfHz" 4000..12000 log: high shelf on the detector, fixed shape (6 k: TBC)
    float scShelfDb = 0.0f;           // "sc.shelfDb" -8..+8
};
struct ClassicCompParams
{
    ClassicCompSetParams sets[kNumChannelSets];      // "<a|b>.<name>"
};
void readClassicCompParams (ParamValues, ClassicCompParams&) noexcept;

//==============================================================================
// Passive EQ ("peq.", Stereo / M/S / L/R / T/S; per channel set "<a|b>.<name>"). The frequency selectors are choices
// whose index points into the kPassiveEq*Hz arrays of Types.h; the boost / cut amounts run 0..10 dB as the reference
// labels them (0 = no effect); the gain they really give follows the reference's measured knob law (PassiveEq.cpp).
struct PassiveEqSetParams
{
    // top section (passive LC program EQ)
    int lowHzIndex = 2;               // "lowHz"      kPassiveEqLowHz: 20 / 30 / 45 / 60 / 100 Hz (45)
    float lowBoost = 0.0f;            // "lowBoost"   0..10 dB (label)
    float lowCut = 0.0f;              // "lowCut"     0..10 dB (label)
    int highHzIndex = 3;              // "highHz"     kPassiveEqHighHz: 3 / 4 / 5 / 8 / 10 / 12 / 16 kHz (8 k)
    float highBoost = 0.0f;           // "highBoost"  0..10 dB (label)
    float highQ = 0.0f;               // "highQ"      0 (broad) .. 10 (narrow)
    int highCutHzIndex = 1;           // "highCutHz"  kPassiveEqHighCutHz: 5 / 10 / 20 kHz (10 k)
    float highCut = 0.0f;             // "highCut"    0..10 dB (label)
    // bottom section (passive midrange EQ)
    int lowMidHzIndex = 2;            // "lmHz"       kPassiveEqLowMidHz: 200 / 300 / 500 / 700 / 1000 Hz (500)
    float lowMidBoost = 0.0f;         // "lmBoost"    0..10 dB (label)
    int midHzIndex = 5;               // "midHz"      kPassiveEqMidHz: 200 Hz .. 7 kHz, 11 steps (1.5 k)
    float midCut = 0.0f;              // "midCut"     0..10 dB (label)
    int highMidHzIndex = 2;           // "hmHz"       kPassiveEqHighMidHz: 1.5 / 2 / 3 / 4 / 5 kHz (3 k)
    float highMidBoost = 0.0f;        // "hmBoost"    0..10 dB (label)
};
struct PassiveEqParams
{
    float drive = 0.0f;               // "drive"      0..10 tube make-up stage (shared by both sets; 0 = no colour;
                                      //              ours: the reference has no drive and is linear)
    PassiveEqSetParams sets[kNumChannelSets];        // "<a|b>.<name>"
};
void readPassiveEqParams (ParamValues, PassiveEqParams&) noexcept;

//==============================================================================
// Classic Limiter ("clim.", Stereo only: slot = "clim.on")
struct ClassicLimiterParams
{
    float thresholdDb = -2.0f;        // "thr"        -12..0 (range TBC): gain before the limiter = ceiling - threshold
    float ceilingDb = 0.0f;           // "ceiling"    -12..0 dBFS (range TBC)
    float character = 2.0f;           // "character"  0 (fast) .. 10 (slow)
    ClassicLimiterMode mode = ClassicLimiterMode::Tube;   // "mode" Analog / Tube / Modern
    bool truePeak = false;            // "truePeak"
    bool linkThresholdCeiling = false;   // "link"    UI: Threshold and Ceiling move together (no DSP effect)
};
void readClassicLimiterParams (ParamValues, ClassicLimiterParams&) noexcept;

//==============================================================================
// Tape ("tape.", Stereo only: slot = "tape.on")
struct TapeParams
{
    TapeSpeed speed = TapeSpeed::Ips30;   // "speed"     7.5 / 15 / 30 ips (30)
    float driveDb = 0.0f;             // "drive"      -30..+30 dB into the tape model (output level compensated;
                                      //              the -30 end TBC)
    float bias = 0.0f;                // "bias"       -10..+10
    float harmonics = 0.0f;           // "harmonics"  0..10 even-order harmonics
    float lowEmphasis = 2.0f;         // "lowEmph"    0..10 head bump (0 = no peak, roll-off kept; 10 = +10 dB)
    float highEmphasis = 4.0f;        // "highEmph"   0..10 (4 = flat highs)
};
void readTapeParams (ParamValues, TapeParams&) noexcept;

//==============================================================================
// De-limiter ("dlim.", Stereo only: slot = "dlim.on")
struct DeLimiterParams
{
    float amountPercent = 0.0f;       // "amount"  0..200 % (100 = what was lost, above that an enhanced effect)
    float inputGainDb = 0.0f;         // "input"   -9..+9 dB before the module
    float outputGainDb = 0.0f;        // "output"  -9..+9 dB after the module
    float thresholdDb = 0.0f;         // "thr"     -12..0 dB, where limiting is assumed; Learn Threshold sets it
};
void readDeLimiterParams (ParamValues, DeLimiterParams&) noexcept;

} // namespace ps
