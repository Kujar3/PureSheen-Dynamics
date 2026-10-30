#pragma once

#include "dsp/Params.h"

#include <array>
#include <memory>

// Long-term spectrum analysis and the matched curve of the Match EQ (pure C++17, no JUCE: shared with the phase 3
// Track Referencing feature). Owner: dsp-meq. Implementation: MatchAnalysis.cpp.
//
// Spectra live on a fixed, sample-rate independent grid: kMatchPoints log-spaced points f_i = 10 Hz x 2^(i / 48)
// (1/48 octave, 10 Hz .. 23.6 kHz). Values are power spectral densities in dB re 1/Hz per channel Left, Right,
// Mid = (L + R) / 2, Side = (L - R) / 2; -200 = no value (at or above the analysis Nyquist, or no data).
namespace ps
{
inline constexpr int kMatchPoints = 540;
inline constexpr float kMatchNoValue = -200.0f;
double matchGridHz (int index) noexcept;                // 10 x 2^(index / 48)

enum class MatchRole : int { Reference = 0, Current };
enum class MatchChannel : int { Left = 0, Right, Mid, Side, StereoMean };   // StereoMean = power mean of L and R

// Which curve a channel set uses: Stereo / T/S -> StereoMean (both sets); M/S -> set A Mid, set B Side; L/R -> set A
// Left, set B Right.
MatchChannel matchChannelFor (ChannelMode, int channelSet) noexcept;

struct MatchSpectrum
{
    bool valid = false;                 // >= 1 s of non-silent audio
    double seconds = 0.0;               // non-silent audio analysed
    double sampleRate = 0.0;            // of the analysed audio (information only; the grid is in Hz)
    std::array<float, kMatchPoints> psdDb[4] {};   // per MatchChannel Left .. Side; kMatchNoValue = no value
    std::array<char, 96> source {};     // ASCII tag, zero terminated: "capture", "file:<name>", "import:<name>", ...

    float valueDb (MatchChannel, int index) const noexcept;   // StereoMean derived from Left / Right
};

//==============================================================================
// Welch-style long-term average of a stereo signal.
//   Hann window, 50 % overlap, FFT 16384 at <= 50 kHz, 32768 at <= 100 kHz, 65536 above. L and R in one complex FFT;
//   per bin, in double, plain sums since reset: |L|^2, |R|^2, Re (L conj R), so |M|^2 = (|L|^2 + |R|^2 + 2 Re) / 4 and
//   |S|^2 = (|L|^2 + |R|^2 - 2 Re) / 4 exactly. Frames whose mean power is below -70 dBFS are skipped (a pause does not
//   dilute the average). Grid mapping: per grid point a raised-cosine kernel over log2 f, half-width
//   max (1/48 octave, 1.5 bins), each bin weighted by its share; one-sided, divided by the bin width -> PSD.
//   Calibration: a 1 kHz sine at -20 dBFS peak integrates (sum of PSD x df over 900..1100 Hz) to -23.01 dB.
// Threading: prepare() allocates (any thread that owns the analyser, never the audio thread); push / spectrum / reset
// are allocation-free. One analyser per thread (the Match EQ worker, a file loader, phase 3 referencing).
class MatchAnalyser
{
public:
    MatchAnalyser();
    ~MatchAnalyser();

    void prepare (double sampleRate);
    void reset() noexcept;
    void push (const float* left, const float* right, int numSamples) noexcept;   // right may be null (mono)
    double getSeconds() const noexcept;                 // non-silent audio accumulated
    void spectrum (MatchSpectrum& out) const noexcept;  // the average so far (valid once >= 1 s)

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

//==============================================================================
// Smoothing width in octaves for a smoothing percentage: (1/48) x 96^(s / 100) (0 % = 1/48 oct, 50 % ~ 0.2 oct,
// 100 % = 2 oct).
double matchSmoothingOctaves (float smoothPercent) noexcept;

// The matched curve (dB per grid point), deterministic and allocation-free (worker thread and UI):
//   1. R, C = the chosen channel's PSD of reference / current, to power; points without a value are held flat
//   2. both smoothed with a raised-cosine kernel over log2 f in the power domain, half-width matchSmoothingOctaves
//      (weights renormalised near the grid ends / Nyquist)
//   3. level normalisation: D = R_s - C_s - (Lr - Lc), L = 10 log10 sum K^2 (f) P_s (f) df over 20 Hz .. 20 kHz
//      (K = the BS.1770 K-weighting magnitude): the curve redistributes energy, no overall level change
//   4. region: x T (f), T = 1 inside [lowHz, highHz], raised-cosine taper to 0 over 1/2 octave outside, 0 beyond
//   5. amount: x amountPercent / 100
//   6. soft limits: x > 0 -> maxBoost tanh (x / maxBoost), x < 0 -> maxCut tanh (x / maxCut) (limit 0 -> 0)
//   7. 0 dB below 10 Hz and wherever either spectrum has no value
// A set whose reference or current spectrum is invalid (or whose mean level is below -150 dB, e.g. the Side of a mono
// file) gets a flat curve (all 0).
void computeMatchCurve (const MatchSpectrum& reference, const MatchSpectrum& current, MatchChannel,
                        const MatchEqSetParams&, float* outDb /* kMatchPoints */) noexcept;

} // namespace ps
