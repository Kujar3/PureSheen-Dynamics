#pragma once

#include "assist/AssistFeatures.h"
#include "dsp/Types.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

// The Assistant's suggestion engine (pure C++17, no JUCE; worker thread, allocates). Owner: assistant.
//
// From an Analysis of the captured audio and the user's targets it builds several complete starting points. Each one
// is a chain (module order) plus a full preset-scope parameter vector (paramSpecs() order, our defaults where the
// suggestion says nothing) that the processor applies as one undo step; afterwards it is an ordinary chain.
//
// The default suggestion ("Suggested") follows the reference's assistant, fitted black-box to its choices on seven
// test tracks (private notes): the chain EQ 1 > Punch > Imager > Definition > Tone Balance > Dynamic EQ > Maximizer, a
// detected tone target, an EQ that leans the track toward that target by a few dB at most, Punch / Imager band settings
// from the measured transients / width per band, a Dynamic EQ on the low-mid and presence resonances, and Maximizer
// settings + a loudness target that come with the tone target (AssistProfiles.cpp).
//
// Macros: a small set of controls that map ABSOLUTELY onto module controls of the applied suggestion (its values are
// the base): the tone match (EQ 1 Amount), tilt (two reserved EQ 1 shelves), dynamics match (Punch Amount), width
// match (Imager Amount) and per-band width offsets, compress (a glue Dynamics, inserted when first used), Definition
// and Tone Balance amounts, and the loudness target (Maximizer gain through the measured gain-to-loudness slope).
// "Loudness matched" adds a Maximizer gain correction from per-macro tables of the pre-Maximizer loudness, so moving
// the tone or width macros does not change how loud the master is.
namespace ps::assist
{
enum class Intensity : int { Gentle = 0, Balanced, Strong };

// What the Assistant may use (Custom page). Bit = 1 << (int) ModuleType.
inline constexpr unsigned kAllModules = 0xffffffffu;

// The loudness targets cards, settings and the Loudness macro use (one range everywhere).
inline constexpr float kMinTargetLufs = -20.0f, kMaxTargetLufs = -5.0f;

struct Settings
{
    int profile = -1;                  // -1 = detect, else (int) ToneTarget
    bool useReference = true;          // a valid reference is the target (else the profile, the reference stays loaded)
    bool autoLoudness = true;          // true: the profile's loudness
    float loudnessLufs = -10.0f;       // used when autoLoudness is false (kMinTargetLufs .. kMaxTargetLufs)
    Intensity intensity = Intensity::Balanced;
    bool useMaximizer = true;
    float ceilingDb = -0.1f;
    bool truePeak = false;
    unsigned allowedModules = kAllModules;
};

// A reference track as a target: its tonal balance (relative to pink, as Analysis::octaveDb), its width per processing
// band and its loudness. Filled from an Analysis of the reference audio (referenceFromAnalysis); Track Referencing
// will fill the same struct from its selected reference.
struct ReferenceTarget
{
    bool valid = false;
    std::string name;
    Curve octaveDb {};
    std::array<float, kProcBands> widthPct {};
    float integratedLufs = kNoValue;
    float plrDb = kNoValue;
};

ReferenceTarget referenceFromAnalysis (const Analysis&, const std::string& name);

// The profile a tone target brings (AssistProfiles.cpp).
struct Profile
{
    float loudnessLufs = -10.0f;
    MaximizerMode mode = MaximizerMode::Transparent;
    float character = 2.0f;
    float upwardDb = 1.6f;
    bool clipOn = true;
    int clipMode = 1;
    float clipAmount = 10.0f;
    float stereoIndependence = 100.0f;           // transient
    float stereoIndependenceSustain = 100.0f;
    float definitionTilt = 0.0f;
};
const Profile& profileFor (ToneTarget) noexcept;
const char* targetName (ToneTarget) noexcept;   // "Rock", "Hip-Hop", ...

// Detected tone target (example-based: nearest labelled example in a small feature space). `distance` (optional)
// receives the distance to that example (lower = more certain).
ToneTarget detectTarget (const Analysis&, float* distance = nullptr);
std::array<float, 8> detectorFeatures (const Analysis&);   // exposed for the tests / dev tool

//==============================================================================
enum class Macro : int
{
    ToneMatch = 0,   // EQ 1 Amount, 0..200 %
    Tilt,            // dB at 20 kHz vs 20 Hz half each way, -4..+4 (two Baxandall shelves on EQ 1 bands 6 / 7)
    Dynamics,        // Punch Amount, 0..100 %
    Width,           // Imager Amount, 0..100 %
    WidthLow, WidthLowMid, WidthHighMid, WidthHigh,   // offsets on the Imager band widths, -100..+100
    Compress,        // 0..100: glue Dynamics (inserted before Tone Balance when it leaves 0)
    Definition,      // Definition Amount, 0..100 %
    ToneBalance,     // Tone Balance Amount, 0..100 %
    Loudness,        // target LUFS, -20..-5
    Count
};
inline constexpr int kNumMacros = (int) Macro::Count;

struct MacroInfo
{
    const char* id;          // state key
    const char* name;        // UI
    float minValue, maxValue;
    const char* unit;
    bool loudnessMatched;    // has a compensation table
};
const MacroInfo& macroInfo (Macro) noexcept;

using MacroValues = std::array<float, kNumMacros>;

struct Suggestion
{
    std::string name;                  // "Suggested", "Gentle", ...
    std::string description;           // one line for the card
    ToneTarget target = ToneTarget::Balanced;
    float loudnessLufs = -10.0f;
    std::vector<ModuleType> order;
    std::vector<float> values;         // paramSpecs() order
    MacroValues macros {};             // the macro positions this suggestion starts at
    bool isDefault = false;

    // Loudness (OfflineChain.h): Maximizer gain change per LU of target change, measured around the target, and the
    // loudness-match tables: the pre-Maximizer loudness change (LU) at kTablePoints positions of each loudness-matched
    // macro, relative to the suggestion's own position. Until measured: slope 1.3, no tables.
    static constexpr int kTablePoints = 5;
    float gainPerLufs = 1.3f;
    bool loudnessMeasured = false;
    bool tablesReady = false;
    std::array<std::array<float, kTablePoints>, kNumMacros> tablePositions {};
    std::array<std::array<float, kTablePoints>, kNumMacros> tableDeltaLu {};
};

// Maximizer gain correction (dB) that keeps the loudness when the macros move away from the suggestion's own
// positions: minus the interpolated pre-Maximizer loudness change, summed over the loudness-matched macros. 0 without
// tables.
float loudnessCompensation (const Suggestion&, const MacroValues&);

struct Context
{
    Analysis analysis;
    Settings settings;
    ReferenceTarget reference;
};

// Builds the cards: the default first ("Suggested", or "Match reference" when a reference is valid), then the
// variations. The Maximizer gain in the values is a first estimate (input loudness + a profile margin); the processor
// refines it with the offline render (OfflineChain.h, refineLoudness).
std::vector<Suggestion> buildSuggestions (const Context&);

// The parameter changes (id index, plain value) and chain that put `macros` on top of `base` (absolute mapping; the
// base suggestion's own values are the reference point). `compensationDb` is added to the Maximizer gain (loudness
// matching; 0 = none). `currentOrder` is the chain as it is now (the user may have edited it): the Compress macro adds
// a Dynamics module when needed, and nothing else changes the chain.
struct MacroApplication
{
    std::vector<std::pair<int, float>> values;
    std::vector<ModuleType> order;
};
MacroApplication applyMacros (const Suggestion& base, const MacroValues& macros, float compensationDb,
                              const std::vector<ModuleType>& currentOrder, const Analysis&);

// Param index lookup for engine code (asserts in debug when the id is unknown).
int pid (const char* id) noexcept;

// EQ curve of one channel set of EQ 1 in `values` at hz (dB, sum of the enabled bands, amount applied).
double eqCurveDb (const std::vector<float>& values, double hz, double sampleRate);
} // namespace ps::assist
