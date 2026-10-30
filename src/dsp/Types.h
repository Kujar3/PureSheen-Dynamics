#pragma once

#include <array>
#include <cstdint>

// Shared enums and constants of the PureSheen DSP core (pure C++17, no JUCE).
// Every enum is APPEND-ONLY: values are stored in presets / host projects and mapped to parameter choice indices.
namespace ps
{
//==============================================================================
// Chain modules. One instance per type in a chain (Equalizer 1 and 2 are two types). Phase-2/3 modules get appended.
enum class ModuleType : int
{
    Equalizer1 = 0,
    Equalizer2,
    Dynamics,
    Imager,
    Exciter,
    Maximizer,
    // ---- phase 2+ (append here) ----
    Count
};

inline constexpr int kNumModuleTypes = (int) ModuleType::Count;

struct ModuleInfo
{
    const char* idPrefix;     // parameter id prefix, "eq1"
    const char* name;         // "Equalizer 1"
    const char* shortName;    // "EQ 1" (chain tiles, host parameter names)
};

const ModuleInfo& moduleInfo (ModuleType);   // Types.cpp

// The chain a fresh instance starts with (the reference starts empty; we start with a useful default so the plug-in does
// something sensible straight away): EQ 1 -> Dynamics -> Imager -> Exciter -> Maximizer, everything at neutral
// settings except the Maximizer (Gain 0 dB, Output -1.0 dBFS, True Peak on), which only catches overs.
inline constexpr std::array<ModuleType, 5> kDefaultChain {
    ModuleType::Equalizer1, ModuleType::Dynamics, ModuleType::Imager, ModuleType::Exciter, ModuleType::Maximizer
};

//==============================================================================
// Per-module channel processing (the reference's "channel modes"). Parameter set A = Stereo / Mid / Left, set B = Side / Right.
//   Stereo     : both channels use set A, detectors / gain stages stereo-linked
//   MidSide    : the host M/S-encodes (M = (L+R)/2, S = (L-R)/2), ch0 = Mid with set A, ch1 = Side with set B, decodes
//   LeftRight  : ch0 = Left with set A, ch1 = Right with set B, unlinked
//   TransientSustain : reserved for phase 2 (shared transient splitter); treated as Stereo until implemented
enum class ChannelMode : int { Stereo = 0, MidSide, LeftRight, TransientSustain };

inline constexpr int kNumChannelSets = 2;   // A, B

//==============================================================================
// Crossovers of the multiband modules (Dynamics, Imager, Exciter).
//   Analog  : 4th-order Linkwitz-Riley (zero latency, phase shift, magnitude-flat sum)
//   Digital : linear-phase FIR crossover (latency, phase-perfect, perfect reconstruction)
//   Hybrid  : perfect-reconstruction IIR (LR4 with allpass phase compensation of the other bands, zero latency)
enum class CrossoverType : int { Analog = 0, Digital, Hybrid };

inline constexpr int kMaxBands = 4;                 // multiband modules
inline constexpr int kMaxCrossovers = kMaxBands - 1;
inline constexpr int kEqBands = 8;                  // per Equalizer, per channel set

//==============================================================================
// Equalizer filter shapes (one menu per node; Surgical only in Digital mode, falls back to Bell in Analog).
enum class EqShape : int
{
    Bell = 0,
    ProportionalQ,       // Q narrows as |gain| rises
    BandShelf,           // flat-topped band (two shelves)
    LowShelfAnalog,
    LowShelfBaxandall,   // fixed, very gentle slope (Q not used)
    LowShelfVintage,     // shelf with a resonant dip above the corner
    LowShelfResonant,    // shelf with a resonant bump at the corner
    HighShelfAnalog,
    HighShelfBaxandall,
    HighShelfVintage,
    HighShelfResonant,
    HighPassFlat,        // Butterworth, slope from EqSlope
    HighPassResonant,    // Q sets the corner resonance
    HighPassBrickwall,   // steep elliptic / FIR (Digital: FIR brickwall)
    LowPassFlat,
    LowPassResonant,
    LowPassBrickwall,
    Surgical,            // very narrow bell (Digital mode only)
    Count
};

// Slopes for HP / LP Flat and Resonant (dB per octave).
enum class EqSlope : int { Db6 = 0, Db12, Db18, Db24, Db36, Db48, Count };
inline constexpr std::array<int, (int) EqSlope::Count> kEqSlopeDb { 6, 12, 18, 24, 36, 48 };

enum class EqFilterMode : int { Analog = 0, Digital };   // minimum-phase IIR / linear-phase FIR

//==============================================================================
enum class DetectionMode : int { Peak = 0, Envelope, Rms };
enum class DetectionFilter : int { None = 0, HighPass, Tilt };

//==============================================================================
enum class ExciterMode : int { Analog = 0, Retro, Tape, Tube, Warm, Triode, DualTriode, Count };

enum class StereoizeMode : int { ModeI = 0, ModeII };

//==============================================================================
// Maximizer algorithms (our own; behaviour modelled on the published descriptions of the reference's limiter modes).
enum class MaximizerMode : int
{
    LowLatency = 0,   // single-band lookahead limiter, ~1 ms lookahead
    Smooth,           // psychoacoustic: fast release on transients, slow on sustained bass
    Transparent,      // Smooth + transient preservation
    Aggressive,       // most loudness, psychoacoustic speed model, styles
    Spectral,         // limits the critical bands that cause the peak
    Multiband,        // 4-band, per-band envelopes, peak-contributing bands first
    Count
};
enum class AggressiveStyle : int { Clipping = 0, Crisp, Balanced, Pumping };
enum class SpectralStyle : int { Classic = 0, Modern, Transient };
enum class SoftClipMode : int { Light = 0, Moderate, Heavy };   // onset -3 / -9 / -30 dBFS

//==============================================================================
enum class DitherBits : int { Bits24 = 0, Bits20, Bits16, Bits12, Bits8 };
inline constexpr std::array<int, 5> kDitherBitDepth { 24, 20, 16, 12, 8 };
enum class DitherAmount : int { Strong = 0, Medium, Low, Off };

} // namespace ps
