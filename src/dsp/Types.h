#pragma once

#include <array>
#include <cstdint>

// Shared enums and constants of the PureSheen DSP core (pure C++17, no JUCE).
// Every enum is APPEND-ONLY: values are stored in presets / host projects and mapped to parameter choice indices.
namespace ps
{
//==============================================================================
// Chain modules. One instance per type in a chain (Equalizer 1 and 2 are two types). Phase-3 modules get appended.
// States, presets and the chain order store moduleInfo().idPrefix, never this value; the parameter table records it
// in ParamSpec::module, so the order is fixed once released.
enum class ModuleType : int
{
    Equalizer1 = 0,
    Equalizer2,
    Dynamics,
    Imager,
    Exciter,
    Maximizer,
    // ---- phase 2 ----
    DynamicEq,      // "deq"  Dynamic EQ: 6 dynamic bands per channel set
    MatchEq,        // "meq"  Match EQ: linear / minimum phase FIR that matches a reference spectrum
    ToneBalance,    // "tbal" Tone Balance: adaptive EQ toward a tonal-balance target (Shape / Cut / Boost)
    Definition,     // "defn" Definition: adaptive spectral contour toward pink + tilt (above 300 Hz)
    Smoother,       // "smth" Smoother: 32-band adaptive reduction of harsh peaks inside a region
    Punch,          // "pnch" Punch: multiband micro-dynamics (short / long envelope ratio)
    LowContrast,    // "lowc" Low Contrast: time-frequency contrast of the low end (20..300 Hz)
    BassShaper,     // "bass" Bass Shaper: balance, punch, sustain power and peak control below a cutoff
    // ---- phase 3+ (append here) ----
    Count
};

inline constexpr int kNumCoreModuleTypes = (int) ModuleType::DynamicEq;   // the six types released in 0.1.x

inline constexpr int kNumModuleTypes = (int) ModuleType::Count;

struct ModuleInfo
{
    const char* idPrefix;     // parameter id prefix, "eq1"
    const char* name;         // "Equalizer 1"
    const char* shortName;    // "EQ 1" (chain tiles, host parameter names)
};

const ModuleInfo& moduleInfo (ModuleType);   // Types.cpp

// The chain a fresh instance starts with (the reference suite starts empty; we start with a useful default so the plug-in does
// something sensible straight away): EQ 1 -> Dynamics -> Imager -> Exciter -> Maximizer, everything at neutral
// settings except the Maximizer (Gain 0 dB, Output -1.0 dBFS, True Peak on), which only catches overs.
inline constexpr std::array<ModuleType, 5> kDefaultChain {
    ModuleType::Equalizer1, ModuleType::Dynamics, ModuleType::Imager, ModuleType::Exciter, ModuleType::Maximizer
};

//==============================================================================
// Per-module channel processing ("channel modes"). Parameter set A = Stereo / Mid / Left, set B = Side / Right.
//   Stereo     : both channels use set A, detectors / gain stages stereo-linked
//   MidSide    : the host M/S-encodes (M = (L+R)/2, S = (L-R)/2), ch0 = Mid with set A, ch1 = Side with set B, decodes
//   LeftRight  : ch0 = Left with set A, ch1 = Right with set B, unlinked
//   TransientSustain : the host splits the input into a transient and a sustain part (ps::TransientSplitter; both
//                      stereo, T + S == input delayed by the splitter latency). Instance A of the module processes T
//                      as Stereo with set A, a second instance (the chain's "twin") processes S as Stereo with set B
//                      (Module::getParamSet() == 1, also passed as ProcessContext::paramSet). The parts are summed at
//                      the slot output. Set A = Transient, set B = Sustain.
enum class ChannelMode : int { Stereo = 0, MidSide, LeftRight, TransientSustain };

inline constexpr int kNumChannelSets = 2;   // A, B

//==============================================================================
// Crossovers of the multiband modules (Dynamics, Imager, Exciter).
//   Analog  : 4th-order Linkwitz-Riley (zero latency, phase shift, magnitude-flat sum)
//   Digital : linear-phase FIR crossover (latency, phase-perfect, perfect reconstruction)
//   Hybrid  : steep LR8 (48 dB/oct) IIR tree with allpass phase compensation of the other bands, zero latency
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
    LowShelfVintage,     // shelf with a resonant dip above the corner (classic passive-EQ style)
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
// Maximizer algorithms (our own; behaviour modelled on published descriptions of mastering limiters).
enum class MaximizerMode : int
{
    LowLatency = 0,   // single-band lookahead limiter, ~1 ms lookahead
    Smooth,           // lets go right after each peak; Character sets how fast
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

//==============================================================================
// Phase 2 module enums (choice indices of their parameters; append-only like everything here).

// Dynamic EQ band shapes. Every shape maps onto the Equalizer shape of the same name (dynEqToEqShape), so a static
// Dynamic EQ band measures exactly like the Equalizer band.
enum class DynEqShape : int
{
    Bell = 0,
    ProportionalQ,
    BandShelf,
    LowShelfBaxandall,
    HighShelfBaxandall,
    LowShelfAnalog,
    HighShelfAnalog,
    Count
};

constexpr EqShape dynEqToEqShape (DynEqShape s) noexcept
{
    switch (s)
    {
        case DynEqShape::Bell:               return EqShape::Bell;
        case DynEqShape::ProportionalQ:      return EqShape::ProportionalQ;
        case DynEqShape::BandShelf:          return EqShape::BandShelf;
        case DynEqShape::LowShelfBaxandall:  return EqShape::LowShelfBaxandall;
        case DynEqShape::HighShelfBaxandall: return EqShape::HighShelfBaxandall;
        case DynEqShape::LowShelfAnalog:     return EqShape::LowShelfAnalog;
        case DynEqShape::HighShelfAnalog:    return EqShape::HighShelfAnalog;
        case DynEqShape::Count:              break;
    }
    return EqShape::Bell;
}

// Down: the band moves toward its gain when the level rises above the threshold (a cut = de-esser; a boost backs off
// when loud). Up: the opposite direction (a boost that grows when loud; a cut that opens when loud).
enum class DynEqDirection : int { Down = 0, Up };

// Whose level a Dynamic EQ band listens to: its own band, the detector of band n of the same channel set, or this
// band's detector filter on the other channel of the module's domain (M/S, L/R; = Own in Stereo / T/S).
enum class DynEqSidechain : int { Own = 0, Band1, Band2, Band3, Band4, Band5, Band6, OtherChannel, Count };

inline constexpr int kDynEqBands = 6;   // per channel set

enum class MatchPhase : int { Linear = 0, Minimum };

enum class ToneBalanceMode : int { Shape = 0, Cut, Boost };

// Tone Balance targets (curves relative to pink noise: measured from the reference where rendered, else our own; see
// ToneTargets.cpp). Index = choice value.
enum class ToneTarget : int
{
    Balanced = 0, Cinematic, Country, Edm, Folk, HipHop, Jazz, Pop, Reggae, RnbSoul, Rock,
    AltRock, Ambient, ClassicHipHop, ClassicRock, DancePop, DrumAndBass, Dubstep, Electropop, FutureBass, House,
    Hyperpop, IndiePop, IndieRock, KpopJpop, LatinPop, LoFi, Metal, PopCountry, PostRock, PunkRock, Reggaeton, Techno,
    Trap, VocalJazz, Flat,
    Count
};

enum class SmootherMode : int { Gentle = 0, Medium, Strong };

enum class LowContrastMode : int { Punchy = 0, Smooth };

// Punch envelope sync: note values (index = "pnch.syncNote" choice) as fractions of a whole note.
// envelope ms = fraction * 4 * 60000 / bpm, clamped to 10..1000 ms (bpm <= 0 or not finite -> 120).
inline constexpr int kPunchSyncNotes = 18;
inline constexpr std::array<double, kPunchSyncNotes> kPunchSyncFractions {
    1.0 / 96.0, 1.0 / 64.0, 3.0 / 128.0,     // 1/64T 1/64 1/64D
    1.0 / 48.0, 1.0 / 32.0, 3.0 / 64.0,      // 1/32T 1/32 1/32D
    1.0 / 24.0, 1.0 / 16.0, 3.0 / 32.0,      // 1/16T 1/16 1/16D
    1.0 / 12.0, 1.0 / 8.0, 3.0 / 16.0,       // 1/8T  1/8  1/8D
    1.0 / 6.0, 1.0 / 4.0, 3.0 / 8.0,         // 1/4T  1/4  1/4D
    1.0 / 3.0, 1.0 / 2.0, 3.0 / 4.0          // 1/2T  1/2  1/2D
};

} // namespace ps
