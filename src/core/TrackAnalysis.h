#pragma once

#include "core/Common.h"
#include "core/Fft.h"
#include "core/Loudness.h"
#include "core/TruePeak.h"

#include <array>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

// Whole-track analysis (ported from RefMatch, the family's reference-track matcher; shared with the Assistant and, later,
// Track Referencing): live input (worker thread) and offline audio (decoded file, loader thread): loudness, peaks, dynamics, averaged spectrum, per-band level and stereo
// image. Pure C++17, deterministic.
namespace ps
{
//==============================================================================
// Frequency bands used everywhere (tonal balance, stereo width, hints).
inline constexpr int kNumBands = 6;

enum class Band
{
    Sub = 0,     //    20 -    60 Hz  felt more than heard: sub bass, kick tail
    Low,         //    60 -   200 Hz  bass and kick body, warmth
    LowMid,      //   200 -   500 Hz  "mud" / boxiness
    Mid,         //   500 -  2000 Hz  body of instruments and vocals, "honk"
    HighMid,     //  2000 -  6000 Hz  presence, detail, harshness
    Air          //  6000 - 20000 Hz  brightness, sizzle, air
};

struct BandInfo
{
    const char* name;        // "Low mids"
    const char* shortName;   // "Low-mid" (fits a 70 px column)
    float lowHz;
    float highHz;
    const char* character;   // one-line plain-language description of what lives there
};

// Band edges are contiguous: band i's highHz == band i+1's lowHz. Defined in Analysis.cpp.
const BandInfo& bandInfo (int bandIndex);
inline const BandInfo& bandInfo (Band b) { return bandInfo ((int) b); }

// Low-end mono compatibility is judged below this frequency.
inline constexpr float kLowEndMonoHz = 120.0f;

//==============================================================================
// Smoothed spectrum curve: kCurvePoints log-spaced points from kCurveMinHz to kCurveMaxHz (inclusive).
inline constexpr int kCurvePoints = 160;
inline constexpr float kCurveMinHz = 20.0f;
inline constexpr float kCurveMaxHz = 20000.0f;

inline float curveFrequency (int pointIndex) noexcept
{
    const double t = (double) pointIndex / (double) (kCurvePoints - 1);
    return (float) (kCurveMinHz * std::pow ((double) kCurveMaxHz / kCurveMinHz, t));
}

using Curve = std::array<float, kCurvePoints>;

//==============================================================================
struct BandStats
{
    // Mean-square level of the band in dBFS, averaged over the two channels (a 0 dBFS-peak sine inside the band in
    // both channels reads -3.01 dB). kNoValue when unknown.
    float levelDb = kNoValue;

    // Stereo width = 100 * side energy / mid energy (mid = (L+R)/2, side = (L-R)/2), clamped to 0..300.
    // 0 = mono, 100 = as wide as two unrelated (decorrelated) channels, > 100 = mostly out-of-phase content.
    float widthPct = 0.0f;

    // Phase correlation of L and R in the band: sum Re(L conj R) / sqrt(sum |L|^2 * sum |R|^2), -1..+1.
    // +1 = mono (identical), 0 = unrelated, -1 = polarity inverted (cancels in mono). 1 when silent.
    float correlation = 1.0f;
};

struct TrackProfile
{
    // True once the spectrum / stereo averages contain at least kMinProfileSeconds of non-silent audio.
    bool valid = false;

    // Seconds of non-silent audio analysed since reset (for "Recent" averaging this keeps growing but old audio
    // fades out of the spectrum / stereo averages with a kRecentSeconds time constant).
    double analysedSeconds = 0.0;

    // ---- Loudness (BS.1770 / EBU R128) -------------------------------------------------------------------------
    float integratedLufs = kNoValue;   // since reset
    float shortTermLufs = kNoValue;    // last 3 s
    float momentaryLufs = kNoValue;    // last 400 ms
    float loudnessRangeLu = 0.0f;      // LRA since reset

    // ---- Peaks & dynamics (since reset) -----------------------------------------------------------------------
    float truePeakDb = kNoValue;       // dBTP
    float samplePeakDb = kNoValue;     // dBFS
    float rmsDb = kNoValue;            // RMS of the non-silent audio, dBFS (mean of channels, sine 0 dBFS -> -3.01)
    float crestDb = kNoValue;          // samplePeakDb - rmsDb
    // Peak-to-loudness ratio: samplePeakDb - integratedLufs, i.e. how far the peaks rise above the average loudness.
    // Deliberately the SAMPLE peak: hard-clipped masters have 1-3 dB of inter-sample overshoot (truePeakDb is higher),
    // and the more a master is clipped the more overshoot it has, so a true-peak PLR makes a squashed track look more
    // dynamic. True peak (streaming safety) has its own readout.
    float plrDb = kNoValue;

    // ---- Tonal balance ----------------------------------------------------------------------------------------
    // K-weighted power of the averaged spectrum in LUFS-like units (-0.691 + 10 log10 sum_k K^2(f_k) (P_L + P_R)).
    // Approximates the loudness of exactly the audio in the spectrum average; used to loudness-normalise tonal
    // balance comparisons so they match what you hear with level matching on.
    float spectralLoudnessDb = kNoValue;

    std::array<BandStats, kNumBands> bands {};

    // Power spectral density (dB re 1/Hz, mean of channels) at curveFrequency(i), smoothed with a Hann (raised-cosine)
    // kernel over log2 f: 1/3-octave equivalent width, support +-kCurveSmoothingOctaves around the point (each FFT bin
    // weighted by its exact share of the kernel). kNoValue where unknown (at or above Nyquist). No tilt applied (the UI
    // adds a display tilt).
    Curve curveDb {};

    // ---- Stereo -------------------------------------------------------------------------------------------------
    float correlation = 1.0f;          // broadband (20 Hz - 20 kHz) phase correlation
    float widthPct = 0.0f;             // broadband width, same definition as BandStats::widthPct
    float lowEndCorrelation = 1.0f;    // correlation below kLowEndMonoHz (mono compatibility of the bass)
    bool isMono = false;               // fed with right == nullptr, or L and R identical

    TrackProfile() { curveDb.fill (kNoValue); }
};

inline constexpr double kMinProfileSeconds = 1.0;

//==============================================================================
enum class Averaging
{
    Recent = 0,     // spectrum / stereo averages forget with a kRecentSeconds time constant (loudness stays standard)
    SinceReset = 1  // everything averaged since reset (the reference always uses this)
};

// Time constant of "Recent" averaging: about one 16-bar loop at 124 BPM, so looping one section gives a steady
// picture instead of following whichever bars just played.
inline constexpr double kRecentSeconds = 30.0;

// Half-width (octaves) of the Hann smoothing kernel of TrackProfile::curveDb. A raised cosine of half-width B has an
// equivalent (and half-power) width of B, so 1/3 gives 1/3-octave smoothing.
inline constexpr double kCurveSmoothingOctaves = 1.0 / 3.0;

// FFT order used for a sample rate: 13 (8192) up to 50 kHz, 14 up to 100 kHz, 15 above. Frames use a Hann window
// and 50 % overlap.
int fftOrderForSampleRate (double sampleRate) noexcept;

class TrackAnalyser
{
public:
    TrackAnalyser() = default;

    // Allocates everything (FFT, accumulators, scratch); calls reset().
    void prepare (double sampleRate, Averaging averaging);

    // Changes the averaging mode (no allocation). Both averages are always kept, so switching shows the other one
    // immediately and correctly: "Recent" fades old audio with the kRecentSeconds time constant whatever the mode
    // was before, "Since reset" is the plain average of everything since reset.
    void setAveraging (Averaging) noexcept;
    Averaging getAveraging() const noexcept { return averaging; }

    // Clears every measurement (no allocation).
    void reset() noexcept;

    // Feeds audio (`right` may be nullptr = mono). No allocation, no locks. Frames whose mean power is below
    // -70 dBFS are skipped by the spectrum / stereo / RMS averages, so pausing playback does not erase the picture.
    void push (const float* left, const float* right, int numSamples) noexcept;

    // Builds the profile from the current accumulators (allocation-free apart from the returned value).
    TrackProfile profile() const;

    double getSampleRate() const noexcept { return sampleRate; }
    const LoudnessMeter& loudnessMeter() const noexcept { return loudness; }

private:
    void analyseFrame() noexcept;
    void prepareCurveWeights();   // fills curveFirstBin / curveWeightStart / curveWeights (allocates; from prepare)

    double sampleRate = 48000.0;
    Averaging averaging = Averaging::Recent;
    // (implementation-owned members below; the analysis agent may change them freely)
    LoudnessMeter loudness;
    TruePeakMeter peaks;
    std::unique_ptr<Fft> fft;
    std::vector<float> window, inL, inR;              // inL / inR: the current frame being filled
    std::vector<std::complex<float>> scratch;
    // Per bin, one-sided: sum |X_L|^2, |X_R|^2, Re(X_L conj X_R). acc* = plain sums since reset (weightSum frames),
    // accRecent* = exponentially forgetting sums (weightRecent = the same weighting of the frame count).
    std::vector<double> accPowL, accPowR, accCross;
    std::vector<double> accRecentL, accRecentR, accRecentCross;
    std::vector<double> kWeightPower;                 // |K-weighting|^2 per bin (without the -0.691 dB offset)

    // Curve smoothing, precomputed in prepare(): point i uses bins curveFirstBin[i] ... with the weights
    // curveWeights[curveWeightStart[i] .. curveWeightStart[i + 1]) (kernel share * one-sided factor / bin width /
    // kernel mass). An empty range = no value (at or above Nyquist).
    std::array<int, kCurvePoints> curveFirstBin {};
    std::array<int, kCurvePoints + 1> curveWeightStart {};
    std::vector<float> curveWeights;

    int fftSize = 0, hop = 0, fill = 0;
    bool sawDifferentChannels = false;
    double weightSum = 0.0, weightRecent = 0.0, forget = 1.0, analysedSeconds = 0.0;
    double windowEnergy = 0.0;                        // sum of window^2
    long long samplesPushed = 0;

    // RMS: gated per hop-sized chunk of input (same -70 dBFS gate as the frames)
    double sumSquares = 0.0, chunkSquares = 0.0;
    int chunkFill = 0;
    long long nonSilentSamples = 0;
};

//==============================================================================
// Offline helpers for the reference track.

// Analyses [startSample, endSample) of a buffer with SinceReset averaging. `right` may be nullptr.
TrackProfile analyseRegion (const float* left, const float* right, long long startSample, long long endSample,
                            double sampleRate);

// Short-term loudness of the whole buffer every 100 ms: element i = short-term LUFS of the 3 s window ending at
// (i + 1) * 0.1 s. Elements for the first 3 s (i < 29) are kNoValue. `right` may be nullptr.
std::vector<float> shortTermLoudnessCurve (const float* left, const float* right, long long numSamples,
                                           double sampleRate);

// Value of a shortTermLoudnessCurve at a time in seconds (nearest element at or before; kNoValue outside / unknown).
float shortTermAt (const std::vector<float>& curve, double seconds) noexcept;

// Waveform overview for drawing: `columns` min/max pairs of the mono sum (L+R)/2 over equal slices of the buffer.
struct WaveformOverview
{
    std::vector<float> minimum, maximum;   // both of size `columns`, values in -1..1 (clamped)
};

WaveformOverview makeWaveformOverview (const float* left, const float* right, long long numSamples, int columns);

} // namespace ps
