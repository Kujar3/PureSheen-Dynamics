#pragma once

#include <memory>

// Stereo 2x / 4x / 8x oversampler (cascaded half-band stages). Owner: dsp-common.
//   LinearPhase : FIR half-bands (>= 100 dB stopband, passband ripple < 0.01 dB up to 0.45 fs), latency reported in
//                 base-rate samples (rounded up; the remainder is padded with a fractional delay so the reported value
//                 is exact)
//   MinimumPhase: polyphase IIR half-bands (low latency, small phase shift at the top of the band); latency = the
//                 round trip's group delay at DC rounded to the nearest base-rate sample (2x / 4x / 8x: 3 / 5 / 6;
//                 the phase is not linear, so a dry path delayed by it lines up within half a sample at low
//                 frequencies only)
// Realtime: prepare() allocates; process functions are allocation-free.
namespace ps
{
class Oversampler
{
public:
    enum class Quality { LinearPhase, MinimumPhase };

    Oversampler();
    ~Oversampler();

    void prepare (int factorLog2 /* 1..3 */, Quality, int maxBlockSize, int numChannels = 2);
    void reset() noexcept;

    int getFactor() const noexcept;
    int getLatencySamples() const noexcept;   // at the base rate, integer (exact)

    // Upsamples numSamples base-rate samples; returns numChannels pointers to numSamples * factor samples
    // (owned by the oversampler, valid until the next call).
    float* const* upsample (const float* const* input, int numSamples) noexcept;

    // Downsamples the buffers returned by upsample() (processed in place) into output (numSamples base-rate samples).
    void downsample (float* const* output, int numSamples) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
