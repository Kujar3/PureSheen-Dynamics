#pragma once

#include <vector>

// Offline sample-rate conversion for reference tracks (ported from RefMatch). Pure C++17, deterministic; runs on a
// loader thread, never the audio thread.
//
// Band-limited interpolation with a Kaiser-windowed sinc (48 zero crossings per side, beta 9.5, about -95 dB stopband).
// The cut-off sits at 95 % of the lower of the two Nyquist frequencies, so downsampling (a 96 kHz file into a 48 kHz
// session) is anti-aliased and upsampling does not create images. The kernel is a precomputed polyphase table: exact
// phases when the two rates have a small rational ratio (44.1 <-> 48 kHz: 160 / 147 phases), otherwise 4096 quantised
// phases (timing error < 1/8000 sample). Every phase has unity DC gain.
namespace ps
{
class SincResampler
{
public:
    static constexpr double kZeroCrossings = 48.0;
    static constexpr double kKaiserBeta = 9.5;
    static constexpr double kRolloff = 0.95;
    static constexpr long long kMaxExactPhases = 4096;

    SincResampler (double inRate, double outRate);

    // Output length for `numInput` input samples.
    long long outputLength (long long numInput) const noexcept;

    // Resamples output samples [outStart, outEnd) of `numChannels` channels (zero padding outside the input).
    void process (const float* const* in, long long numInput, float* const* out, int numChannels, long long outStart,
                  long long outEnd) const noexcept;

    bool isExact() const noexcept { return exact; }
    int getNumPhases() const noexcept { return numPhases; }

private:
    void position (long long n, long long& i0, int& phase) const noexcept;

    double ratio = 1.0;        // input samples per output sample
    bool exact = false;
    long long upFactor = 1, downFactor = 1;
    int numPhases = 1;
    double cutoff = 1.0;
    int halfTaps = 1, numTaps = 2;
    std::vector<float> phases;   // numPhases x numTaps
};
} // namespace ps
