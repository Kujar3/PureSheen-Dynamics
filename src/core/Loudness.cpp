#include "core/Loudness.h"

#include <algorithm>
#include <cmath>
#include <complex>

//==============================================================================
// BS.1770-4 loudness
//
// Each channel runs through the two K-weighting biquads; the squared outputs of all channels are summed over 100 ms
// steps (boundaries at round (k * sampleRate / 10), so step k always ends at k * 100 ms). The last 30 step sums live in
// a ring together with their sample counts, so momentary (4 steps) and short-term (30 steps) windows are exact mean
// squares over 400 ms / 3 s. Every completed step adds the 400 ms block power to `blockHist` (integrated loudness)
// and, after 3 s, the short-term power to `shortTermHist` (LRA). Both histograms keep the exact linear energy per
// 0.01 LU bin, so gated means are exact and only the classification of blocks within 0.01 LU of a gate is rounded.
//==============================================================================

namespace ps
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kLoudnessOffsetDb = -0.691;
constexpr double kRelativeGateLu = -10.0;      // integrated loudness (BS.1770)
constexpr double kLraRelativeGateLu = -20.0;   // loudness range (EBU Tech 3342)
constexpr double kLraLowPercentile = 0.10;
constexpr double kLraHighPercentile = 0.95;
constexpr int kMomentarySteps = 4;             // 400 ms
constexpr double kTinyState = 1.0e-20;         // filter states below this are flushed (no denormals in silence)

// BS.1770 loudness of a K-weighted mean-square power summed over channels; kNoValue for (near) silence.
float lufsFromPower (double power) noexcept
{
    if (! (power > 0.0) || ! std::isfinite (power))
        return kNoValue;

    const double lufs = kLoudnessOffsetDb + 10.0 * std::log10 (power);
    return hasValue ((float) lufs) ? (float) lufs : kNoValue;
}

void flushTinyState (KBiquad& b) noexcept
{
    if (std::abs (b.z1) < kTinyState)
        b.z1 = 0.0;
    if (std::abs (b.z2) < kTinyState)
        b.z2 = 0.0;
}
} // namespace

//==============================================================================
double KBiquad::magnitudeAt (double frequencyHz, double sampleRate) const noexcept
{
    const double w = 2.0 * kPi * frequencyHz / sampleRate;
    const std::complex<double> zInv = std::polar (1.0, -w);
    const std::complex<double> zInv2 = zInv * zInv;
    const std::complex<double> num = b0 + b1 * zInv + b2 * zInv2;
    const std::complex<double> den = 1.0 + a1 * zInv + a2 * zInv2;
    return std::abs (num / den);
}

void designKWeighting (double sampleRate, KBiquad& stage1Shelf, KBiquad& stage2HighPass)
{
    // Stage 1: high shelf (libebur128 parameters, which reproduce the BS.1770 table at 48 kHz).
    {
        const double f0 = 1681.974450955533;
        const double gainDb = 3.999843853973347;
        const double q = 0.7071752369554196;

        const double k = std::tan (kPi * f0 / sampleRate);
        const double vh = std::pow (10.0, gainDb / 20.0);
        const double vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;

        stage1Shelf = {};
        stage1Shelf.b0 = (vh + vb * k / q + k * k) / a0;
        stage1Shelf.b1 = 2.0 * (k * k - vh) / a0;
        stage1Shelf.b2 = (vh - vb * k / q + k * k) / a0;
        stage1Shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        stage1Shelf.a2 = (1.0 - k / q + k * k) / a0;
    }

    // Stage 2: RLB high-pass. The numerator is left unnormalised (1, -2, 1) exactly as in BS.1770.
    {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;

        const double k = std::tan (kPi * f0 / sampleRate);
        const double a0 = 1.0 + k / q + k * k;

        stage2HighPass = {};
        stage2HighPass.b0 = 1.0;
        stage2HighPass.b1 = -2.0;
        stage2HighPass.b2 = 1.0;
        stage2HighPass.a1 = 2.0 * (k * k - 1.0) / a0;
        stage2HighPass.a2 = (1.0 - k / q + k * k) / a0;
    }
}

float kWeightingResponseDb (double frequencyHz, double sampleRate)
{
    KBiquad shelf, highPass;
    designKWeighting (sampleRate, shelf, highPass);

    const double gain = shelf.magnitudeAt (frequencyHz, sampleRate) * highPass.magnitudeAt (frequencyHz, sampleRate);
    return gain > 1.0e-10 ? (float) (20.0 * std::log10 (gain) + kLoudnessOffsetDb) : kNoValue;
}

//==============================================================================
void LoudnessMeter::Histogram::clear() noexcept
{
    std::fill (energy.begin(), energy.end(), 0.0);
    std::fill (count.begin(), count.end(), 0u);
    totalEnergy = 0.0;
    totalCount = 0;
}

void LoudnessMeter::Histogram::add (double power, float lufs) noexcept
{
    // Absolute gate (the negated comparison also rejects NaN).
    if (count.empty() || ! (lufs >= kHistMinLufs) || ! std::isfinite (power))
        return;

    const double binsPerLu = kHistBins / (double) (kHistMaxLufs - kHistMinLufs);
    const int bin = std::clamp ((int) ((lufs - kHistMinLufs) * binsPerLu), 0, kHistBins - 1);

    energy[(size_t) bin] += power;
    ++count[(size_t) bin];
    totalEnergy += power;
    ++totalCount;
}

int LoudnessMeter::Histogram::firstBinAboveRelativeGate (double gateLu) const noexcept
{
    if (totalCount == 0)
        return kHistBins;

    const double gateLufs = (double) lufsFromPower (totalEnergy / (double) totalCount) + gateLu;

    // binCentreLufs (bin) > gateLufs  <=>  bin > (gateLufs - min) * binsPerLu - 0.5
    // The top bin always passes: the loudest block is above any relative gate (gate < mean <= max), and with extreme
    // input (beyond kHistMaxLufs) all blocks end up in that bin.
    const double binsPerLu = kHistBins / (double) (kHistMaxLufs - kHistMinLufs);
    const double firstAbove = std::floor ((gateLufs - kHistMinLufs) * binsPerLu - 0.5) + 1.0;
    return (int) std::clamp (firstAbove, 0.0, (double) (kHistBins - 1));
}

double LoudnessMeter::binCentreLufs (int bin) noexcept
{
    return kHistMinLufs + (bin + 0.5) * (double) (kHistMaxLufs - kHistMinLufs) / kHistBins;
}

//==============================================================================
int LoudnessMeter::stepLength (long long stepIndex) const noexcept
{
    const auto boundary = [this] (long long k) { return std::llround ((double) k * sampleRate / 10.0); };
    return (int) std::max (1LL, boundary (stepIndex + 1) - boundary (stepIndex));
}

void LoudnessMeter::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    for (size_t ch = 0; ch < shelf.size(); ++ch)
        designKWeighting (sampleRate, shelf[ch], highPass[ch]);

    for (auto* h : { &blockHist, &shortTermHist })
    {
        h->energy.assign ((size_t) kHistBins, 0.0);
        h->count.assign ((size_t) kHistBins, 0u);
    }

    reset();
}

void LoudnessMeter::reset() noexcept
{
    for (size_t ch = 0; ch < shelf.size(); ++ch)
    {
        shelf[ch].reset();
        highPass[ch].reset();
    }

    ring.fill (0.0);
    ringSamples.fill (0);
    ringWrite = 0;
    samplesInStep = 0;
    stepAccumulator = 0.0;
    steps = 0;
    samplesPerStep = stepLength (0);
    totalSamples = 0;
    blockHist.clear();
    shortTermHist.clear();
}

void LoudnessMeter::process (const float* left, const float* right, int numSamples) noexcept
{
    if (left == nullptr || numSamples <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        const double yl = highPass[0].process (shelf[0].process ((double) left[i]));
        double sum = yl * yl;

        if (right != nullptr)
        {
            const double yr = highPass[1].process (shelf[1].process ((double) right[i]));
            sum += yr * yr;
        }

        stepAccumulator += sum;

        if (++samplesInStep >= samplesPerStep)
            completeStep();
    }

    totalSamples += numSamples;
}

void LoudnessMeter::completeStep() noexcept
{
    double energy = stepAccumulator;

    // A NaN / inf input would otherwise poison the filters forever: drop the step's energy and restart the filters.
    if (! std::isfinite (energy))
    {
        energy = 0.0;
        for (size_t ch = 0; ch < shelf.size(); ++ch)
        {
            shelf[ch].reset();
            highPass[ch].reset();
        }
    }

    ring[(size_t) ringWrite] = energy;
    ringSamples[(size_t) ringWrite] = samplesInStep;
    ringWrite = (ringWrite + 1) % kRingSize;
    stepAccumulator = 0.0;
    samplesInStep = 0;
    ++steps;
    samplesPerStep = stepLength (steps);

    // Done here (not per block) so results never depend on the block size.
    for (size_t ch = 0; ch < shelf.size(); ++ch)
    {
        flushTinyState (shelf[ch]);
        flushTinyState (highPass[ch]);
    }

    if (steps >= kMomentarySteps)
    {
        const double blockPower = windowPower (kMomentarySteps);
        blockHist.add (blockPower, lufsFromPower (blockPower));
    }

    if (steps >= kRingSize)
    {
        const double shortTermPower = windowPower (kRingSize);
        shortTermHist.add (shortTermPower, lufsFromPower (shortTermPower));
    }
}

double LoudnessMeter::windowPower (int numSteps) const noexcept
{
    double energy = 0.0;
    long long samples = 0;
    int index = ringWrite;

    for (int i = 0; i < numSteps; ++i)
    {
        index = (index + kRingSize - 1) % kRingSize;
        energy += ring[(size_t) index];
        samples += ringSamples[(size_t) index];
    }

    return samples > 0 ? energy / (double) samples : 0.0;
}

//==============================================================================
float LoudnessMeter::momentaryLufs() const noexcept
{
    return steps >= kMomentarySteps ? lufsFromPower (windowPower (kMomentarySteps)) : kNoValue;
}

float LoudnessMeter::shortTermLufs() const noexcept
{
    return steps >= kRingSize ? lufsFromPower (windowPower (kRingSize)) : kNoValue;
}

float LoudnessMeter::integratedLufs() const noexcept
{
    const int first = blockHist.firstBinAboveRelativeGate (kRelativeGateLu);

    double energy = 0.0;
    unsigned long long count = 0;

    for (int bin = first; bin < kHistBins; ++bin)
    {
        energy += blockHist.energy[(size_t) bin];
        count += blockHist.count[(size_t) bin];
    }

    return count > 0 ? lufsFromPower (energy / (double) count) : kNoValue;
}

float LoudnessMeter::loudnessRange() const noexcept
{
    const int first = shortTermHist.firstBinAboveRelativeGate (kLraRelativeGateLu);

    unsigned long long n = 0;
    for (int bin = first; bin < kHistBins; ++bin)
        n += shortTermHist.count[(size_t) bin];

    if (n < 2)
        return 0.0f;

    // Nearest-rank percentiles of the gated short-term values (same rounding as libebur128).
    const auto lowRank = (unsigned long long) ((double) (n - 1) * kLraLowPercentile + 0.5);
    const auto highRank = (unsigned long long) ((double) (n - 1) * kLraHighPercentile + 0.5);

    double low = 0.0, high = 0.0;
    bool lowFound = false;
    unsigned long long seen = 0;

    for (int bin = first; bin < kHistBins; ++bin)
    {
        seen += shortTermHist.count[(size_t) bin];

        if (! lowFound && seen > lowRank)
        {
            low = binCentreLufs (bin);
            lowFound = true;
        }

        if (seen > highRank)
        {
            high = binCentreLufs (bin);
            break;
        }
    }

    return (float) std::max (0.0, high - low);
}

double LoudnessMeter::secondsMeasured() const noexcept
{
    return (double) totalSamples / sampleRate;
}

//==============================================================================
float measureIntegratedLufs (const float* left, const float* right, long long numSamples, double sampleRate)
{
    LoudnessMeter meter;
    meter.prepare (sampleRate);

    constexpr long long kChunk = 1 << 16;

    for (long long pos = 0; left != nullptr && pos < numSamples; pos += kChunk)
    {
        const int n = (int) std::min (kChunk, numSamples - pos);
        meter.process (left + pos, right != nullptr ? right + pos : nullptr, n);
    }

    return meter.integratedLufs();
}

} // namespace ps
