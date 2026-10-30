#pragma once

#include "core/Common.h"

#include <array>
#include <vector>

// Loudness measurement per ITU-R BS.1770-4 / EBU R128 (EBU Tech 3341 meter, EBU Tech 3342 loudness range).
//
// Realtime-safe after prepare(): process() and every getter neither allocate nor lock, so a LoudnessMeter may run on
// the audio thread. All memory (filter state, 100 ms sub-block ring, gating histograms) is sized in prepare().
namespace ps
{
//==============================================================================
// Direct-form-II-transposed biquad in double precision. Coefficients normalised so a0 == 1.
struct KBiquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0.0; }

    // Magnitude response |H(e^jw)| at `frequencyHz` for the given sample rate.
    double magnitudeAt (double frequencyHz, double sampleRate) const noexcept;
};

// BS.1770 K-weighting for any sample rate: stage 1 = high-shelf "pre-filter" (+4 dB above ~1.7 kHz, head
// diffraction), stage 2 = "RLB" high-pass (~38 Hz). Bilinear-transform designs (same as libebur128), so at 48 kHz the
// coefficients match the tables in BS.1770 to ~1e-8.
void designKWeighting (double sampleRate, KBiquad& stage1Shelf, KBiquad& stage2HighPass);

// Combined K-weighting gain in dB at a frequency, including the -0.691 dB offset of the loudness formula, so a
// 997 Hz sine reads ~0.0 dB. For tests and for display.
float kWeightingResponseDb (double frequencyHz, double sampleRate);

//==============================================================================
class LoudnessMeter
{
public:
    // Readings below this are treated as silence and ignored by the gating (BS.1770 absolute gate).
    static constexpr float kAbsoluteGateLufs = -70.0f;

    LoudnessMeter() = default;

    // Allocates. Must be called before process(); calls reset().
    void prepare (double sampleRate);

    // Clears every reading and the gating history (no allocation).
    void reset() noexcept;

    // Feeds audio. `right` may be nullptr for a mono signal (then only `left` contributes, channel weight 1.0).
    // Stereo channels both have weight 1.0, so the same signal in both channels reads +3.01 LU louder than mono.
    void process (const float* left, const float* right, int numSamples) noexcept;

    // Momentary loudness (400 ms window), LUFS. kNoValue until 400 ms have been measured, and for digital silence.
    // Not gated: quiet passages (e.g. -80 LUFS) are reported as they are.
    float momentaryLufs() const noexcept;

    // Short-term loudness (3 s window), LUFS. kNoValue until 3 s have been measured.
    float shortTermLufs() const noexcept;

    // Integrated (gated) loudness since prepare()/reset(): absolute gate -70 LUFS, relative gate -10 LU, 400 ms
    // blocks with 75 % overlap (a new block every 100 ms). kNoValue while no block has passed the gates.
    float integratedLufs() const noexcept;

    // Loudness range LRA (EBU Tech 3342) in LU since prepare()/reset(): short-term values (every 100 ms) gated at
    // -70 LUFS absolute and -20 LU relative, LRA = 95th - 10th percentile. 0 while fewer than 2 values passed the
    // gates.
    float loudnessRange() const noexcept;

    // Seconds of audio fed since prepare()/reset().
    double secondsMeasured() const noexcept;

    // Number of completed 100 ms steps since prepare()/reset() (useful to build a short-term curve: after each step
    // read shortTermLufs()).
    long long stepCount() const noexcept { return steps; }

    // Samples in the current 100 ms step. Step boundaries sit at round (k * sampleRate / 10) samples, so at rates not
    // divisible by 10 (e.g. 11025 Hz) the length alternates by one sample and step k always ends at k * 100 ms.
    int getSamplesPerStep() const noexcept { return samplesPerStep; }

    // Samples still needed to complete the current step (1 .. getSamplesPerStep()).
    int samplesToNextStep() const noexcept { return samplesPerStep - samplesInStep; }

    double getSampleRate() const noexcept { return sampleRate; }

private:
    static constexpr int kRingSize = 30;            // 30 x 100 ms = 3 s
    static constexpr float kHistMinLufs = -70.0f;
    static constexpr float kHistMaxLufs = 30.0f;    // float input can be far above 0 dBFS; louder blocks share the top bin
    static constexpr int kHistBins = 10000;         // 0.01 LU resolution

    struct Histogram
    {
        std::vector<double> energy;   // sum of linear block powers falling into each bin
        std::vector<unsigned int> count;
        double totalEnergy = 0.0;     // over all bins (= everything above the absolute gate)
        unsigned long long totalCount = 0;
        void clear() noexcept;
        void add (double power, float lufs) noexcept;

        // Index of the first bin whose centre lies above the relative gate: loudness of the energy mean of
        // everything in the histogram + gateLu (gateLu < 0). kHistBins when the histogram is empty.
        int firstBinAboveRelativeGate (double gateLu) const noexcept;
    };

    static double binCentreLufs (int bin) noexcept;

    void completeStep() noexcept;
    double windowPower (int numSteps) const noexcept;   // mean square over the samples of the last numSteps steps
    int stepLength (long long stepIndex) const noexcept;

    double sampleRate = 48000.0;
    int samplesPerStep = 4800;                       // length of the current step
    int samplesInStep = 0;
    double stepAccumulator = 0.0;
    std::array<KBiquad, 2> shelf {}, highPass {};
    std::array<double, kRingSize> ring {};           // sum of squares (all channels) of each of the last 30 steps
    std::array<int, kRingSize> ringSamples {};       // number of samples in each of those steps
    int ringWrite = 0;
    long long steps = 0;
    long long totalSamples = 0;
    Histogram blockHist, shortTermHist;
};

//==============================================================================
// Integrated loudness of a whole buffer in one call (convenience for tests and offline analysis).
float measureIntegratedLufs (const float* left, const float* right, long long numSamples, double sampleRate);

} // namespace ps
