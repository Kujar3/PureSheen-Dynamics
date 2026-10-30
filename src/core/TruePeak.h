#pragma once

#include "core/Common.h"

#include <array>

// True-peak meter per ITU-R BS.1770-4 Annex 2: 4x oversampling with a polyphase FIR interpolator, peak of the absolute
// value of the oversampled signal. (For sample rates >= 176.4 kHz 2x would suffice; 4x is used everywhere.)
//
// Realtime-safe: fixed-size state, no allocation anywhere (prepare() only resets).
namespace ps
{
class TruePeakMeter
{
public:
    static constexpr int kOversampling = 4;
    static constexpr int kTapsPerPhase = 16;   // 64-tap windowed-sinc prototype

    TruePeakMeter();   // designs the (sample-rate independent) interpolation filter

    void prepare (double sampleRate) noexcept;   // resets
    void reset() noexcept;

    // `right` may be nullptr (mono).
    void process (const float* left, const float* right, int numSamples) noexcept;

    // Maximum true peak since reset, dBTP (kNoValue when only silence has been seen). Max over channels.
    float truePeakDb() const noexcept;

    // Maximum absolute sample value since reset, dBFS (kNoValue for silence).
    float samplePeakDb() const noexcept;

    // Linear versions of the above (0 for silence).
    float truePeakLinear() const noexcept { return truePeak; }
    float samplePeakLinear() const noexcept { return samplePeak; }

private:
    void processChannel (int channel, const float* data, int numSamples) noexcept;

    std::array<std::array<float, kTapsPerPhase>, kOversampling> phases {};   // phases[p][k] = h[p + k * 4]
    std::array<std::array<float, kTapsPerPhase>, 2> history {};               // newest sample at index 0
    float truePeak = 0.0f;
    float samplePeak = 0.0f;
};

// Crest factor of a buffer in dB: 20*log10(peak / rms) over all channels. 3.01 dB for a sine, 0 dB for a square.
// kNoValue for silence. `right` may be nullptr.
float crestFactorDb (const float* left, const float* right, long long numSamples);

} // namespace ps
