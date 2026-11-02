#pragma once

#include "core/TrackAnalysis.h"

#include <array>
#include <vector>

// The Assistant's analysis of the captured audio (pure C++17, no JUCE, worker / loader thread only: it allocates).
//
// One pass over the excerpt gives:
//   - the whole-track TrackProfile (loudness, PLR, 1/3-octave curve, six bands of level / width / correlation);
//   - a short-time sub-band picture: STFT frames (2048 at 44.1 / 48 kHz, scaled with the rate; Hann, hop 1/4) summed
//     into kSubBands log-spaced sub-bands (1/6 octave, 20 Hz .. 20 kHz) of mid (L+R)/2 and side (L-R)/2 power per
//     frame. Any band's level percentiles, width, correlation and onset strength come from that grid;
//   - the track's tonal balance relative to pink noise (octave density), normalised to 0 dB mean over 100 Hz .. 5.6
//     kHz (the span the tone targets are level-matched over), at the TrackProfile curve points;
//   - the four processing bands the suggestions use (crossovers 120 Hz, 2 kHz, 10 kHz: the multiband modules'
//     layout in the default chain).
namespace ps::assist
{
inline constexpr int kSubBands = 60;                 // 1/6 octave from 20 Hz
inline constexpr int kProcBands = 4;
inline constexpr std::array<double, kProcBands - 1> kProcCrossovers { 120.0, 2000.0, 10000.0 };

double subBandLowHz (int subBand) noexcept;          // edges: 20 Hz * 2^(b / 6), clipped to 20 kHz at the top
double subBandCentreHz (int subBand) noexcept;

struct BandFeatures
{
    float levelDb = kNoValue;      // mean power (mid + side) over the non-silent frames, dB
    float p50Db = kNoValue;        // frame-power percentiles, dB
    float p90Db = kNoValue;
    float p99Db = kNoValue;
    float widthPct = 0.0f;         // 100 x side energy / mid energy (TrackProfile definition)
    float correlation = 1.0f;      // (M - S) / (M + S) energy form, -1..1
    float onsetDb = 0.0f;          // mean positive frame-to-frame rise of the band level (dB per frame, hop ~11 ms)
    float crestDb = 0.0f;          // p99 - p50 of the frame power
};

struct Analysis
{
    bool valid = false;
    double sampleRate = 0.0;
    double seconds = 0.0;          // non-silent audio analysed
    TrackProfile profile;

    int numFrames = 0;             // non-silent frames kept
    double framesPerSecond = 0.0;
    std::vector<float> midPow, sidePow;   // [frame * kSubBands + subBand], power (mean square share)

    Curve octaveDb {};             // relative to pink, 0 dB mean over 100 Hz .. 5.6 kHz (kNoValue where unknown)
    std::array<BandFeatures, kProcBands> procBands {};
};

// Analyses the whole buffer (`right` may be nullptr = mono). Frames whose mean power is below -70 dBFS are skipped.
Analysis analyse (const float* left, const float* right, long long numSamples, double sampleRate);

// Features of an arbitrary band [loHz, hiHz) from the sub-band grid (partial sub-bands weighted by overlap in log f).
BandFeatures bandFeatures (const Analysis&, double loHz, double hiHz);

// Frame-power percentile (0..100) of a band, dB.
float bandPercentileDb (const Analysis&, double loHz, double hiHz, double percentile);

// Linear interpolation of a curve (TrackProfile points) in log f; kNoValue outside / unknown.
float curveAt (const Curve&, double hz) noexcept;
} // namespace ps::assist
