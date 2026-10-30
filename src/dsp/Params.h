#pragma once

#include "dsp/Types.h"

#include <string>
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
// Host-visible names: "<shortName> <Band n> <Name> [(A)|(B)]", e.g. "EQ 1 Band 3 Freq (A)". Set B is labelled "(B)";
// the UI shows A/B as "Mid"/"Side" or "Left"/"Right" depending on the channel mode.
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
// Modules with a single channel mode (Imager, Maximizer: Stereo only) have only "<prefix>.on": no chMode (a choice
// needs >= 2 entries), no a.on / b.on / link (no effect in Stereo; and "max.link" is the Maximizer's Gain/Output
// link). readSlotParams then returns Stereo, both channels on, link on.
struct SlotParams
{
    bool power = true;                // "<prefix>.on"
    ChannelMode channelMode = ChannelMode::Stereo;   // "<prefix>.chMode" (choices the module supports, see below)
    bool channelOn[kNumChannelSets] { true, true };  // "<prefix>.a.on" "<prefix>.b.on" (M/S, L/R only)
    bool channelLink = true;          // "<prefix>.link": UI edits A and B together (no DSP effect)
};
void readSlotParams (ParamValues, ModuleType, SlotParams&) noexcept;

// Channel modes each module offers (reference-suite parity): EQ1/EQ2 all; Dynamics, Exciter Stereo + M/S; Imager,
// Maximizer Stereo only (the choice list of "<prefix>.chMode" contains only these, in this order).
std::vector<ChannelMode> supportedChannelModes (ModuleType);

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
};
void readImagerParams (ParamValues, ImagerParams&) noexcept;

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

} // namespace ps
