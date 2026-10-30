#pragma once

#include "dsp/Convolver.h"
#include "dsp/MaximizerStages.h"
#include "core/Fft.h"

#include <complex>
#include <memory>

// Band-splitting stages of the Maximizer's Spectral and Multiband modes (owner: dsp-maximizer; internal, not a
// contract). Both are written in "residual" form: output = input delayed + sum over bands of (gain - 1) * band, so with
// no reduction the output is exactly the delayed input (null test), and the bands always add back to the input.
namespace ps::maxi
{
//==============================================================================
// Spectral: STFT (sqrt-Hann, 75 % overlap, 512 points at 44.1 / 48 kHz, scaled with the rate) with ~24 critical-band
// groups. Per frame and channel, the reduction the frame's peak needs is distributed over the bands by "water
// filling": the band amplitudes are capped at a common level so the strongest bands (the ones that make the peak)
// give up their level first. Selectivity blends that with a broadband reduction. Instant attack (the frame looks
// ahead), per-band release. Latency = frame size.
class SpectralStage
{
public:
    struct Settings
    {
        double selectivity = 1.0;      // 0 = broadband, 1 = fully band-selective
        double releaseMs = 120.0;
        double highReleaseScale = 1.0; // release multiplier for bands above 4 kHz
        double lowReleaseScale = 1.5;  // ... below 300 Hz
        double independence = 0.0;     // stereo independence
        double amount = 1.0;           // fraction (in dB) of the needed reduction done here
    };

    SpectralStage();
    ~SpectralStage();
    void prepare (double sampleRate);
    void reset() noexcept;
    int getLatency() const noexcept { return frameSize; }
    void setSettings (const Settings&) noexcept;

    // thr[i] = threshold of input sample i.
    void process (const float* const* in, float* const* out, const double* thr, int n) noexcept;

    double minGain[2] { 1.0, 1.0 };

private:
    void processFrame() noexcept;
    void waterFill (const double* amps, double requirement, double* reductionOut) noexcept;

    double sr = 48000.0;
    int frameSize = 512, hop = 128, pos = 0, numBins = 257, numBands = 0, dryMask = 0;
    long long t = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> window;
    std::array<std::vector<float>, 2> inBuf, outAcc, dry;
    std::vector<double> thrBuf;
    std::vector<std::complex<float>> z, spec[2];
    std::vector<int> bandStart;            // numBands + 1 bin indices
    std::vector<int> binLo;                // per bin: lower band for interpolation
    std::vector<double> binFrac;
    std::array<std::vector<double>, 2> state, target;
    std::vector<double> relCoef, amps[2], sorted, binRed, binSmooth;
    std::vector<int> holdFrames;
    std::array<std::vector<int>, 2> holdLeft;
    Settings settings;
    double controlStep = 1.0, independenceNow = 0.0, selectivityNow = 1.0, amountNow = 1.0;
};

//==============================================================================
// Multiband: 4 bands (linear-phase complementary FIR crossovers at 120 Hz, 1 kHz, 6 kHz: transition about 0.8 of the
// crossover frequency, > 50 dB separation), per-band lookahead envelopes. At each sample over the threshold the excess
// is taken from the bands whose signed contribution to that sample is largest (water filling on the contributions in
// the direction of the peak), so the bands that make the peak are attenuated first and bands that oppose it are left
// alone. Per band: attack 4 / 2 / 1 / 0.5 ms, hold 8 / 5 / 4 / 4 ms (about half a bass period), release, stereo
// independence (sustain amount for the two low bands, transient amount for the two high bands).
class MultibandStage
{
public:
    static constexpr int kBands = 4;

    struct Settings
    {
        double releaseMs = 80.0;     // the low band x2, the low mid band x1.3
        double siSus = 0.0, siTrans = 0.0;
    };

    MultibandStage();
    ~MultibandStage();
    void prepare (double sampleRate);
    void reset() noexcept;
    int getLatency() const noexcept { return splitLatency + lookahead; }
    int getSplitLatency() const noexcept { return splitLatency; }
    void setSettings (const Settings&) noexcept;

    // thr[i] = threshold of the band sample with index (input index - getSplitLatency()).
    void process (const float* const* in, float* const* out, const double* thr, int n) noexcept;

    double minGain[2] { 1.0, 1.0 };

private:
    double sr = 48000.0;
    int firLength = 1, splitLatency = 0, lookahead = 1, ringMask = 0;
    long long t = 0;
    Convolver lowpass[3];
    std::array<DelayLine, 2> inputDelay;
    std::array<std::array<std::vector<float>, 2>, 3> lpBuf;
    std::array<std::vector<float>, 2> xdBuf;
    std::array<std::array<std::vector<float>, 2>, kBands> bandRing;
    std::array<std::vector<float>, 2> xdRing;
    std::array<std::array<std::vector<double>, 2>, kBands> redRing;
    std::array<std::array<MaxDeque, 2>, kBands> windowMax;
    std::array<std::array<BoxAverage, 2>, kBands> box;
    std::array<std::array<double, 3>, kBands> release {};
    std::array<int, kBands> attack {}, hold {};
    std::array<double, kBands> kRelease {}, independence {};
    std::array<double, kBands> independenceNow {};
    double controlStep = 1.0;
    Settings settings;
};

} // namespace ps::maxi
