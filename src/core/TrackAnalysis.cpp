#include "core/TrackAnalysis.h"

#include <algorithm>
#include <cmath>
#include <limits>

//==============================================================================
// Spectrum and stereo analysis
//
// Frames of fftSize samples (Hann, 50 % overlap) go through ONE complex FFT with L as the real and R as the imaginary
// part; the two spectra are separated with X_L = (Z[k] + conj Z[N-k]) / 2 and X_R = (Z[k] - conj Z[N-k]) / 2i. Per
// one-sided bin the analyser accumulates |X_L|^2, |X_R|^2 and Re(X_L conj X_R) twice: as plain sums since reset
// (acc*, frame count weightSum) and with exponential forgetting at a kRecentSeconds time constant (accRecent*, the same
// weighting of the frame count in weightRecent). accumulator / weight is always a properly normalised average, and
// keeping both makes switching the averaging mode instant and exact.
//
// Scale: the power of bin k is c_k |X[k]|^2 / (N * sum w^2) with c_k = 2 for 0 < k < N/2 (one-sided), 1 at DC and
// Nyquist. Summed over all bins this is the mean-square value of the signal (Parseval), so a sine of peak A gives
// A^2 / 2 however its energy is spread over neighbouring bins, and power / bin width is a PSD in 1/Hz that does not
// depend on the FFT size. Band and curve values integrate that PSD over a frequency range, each bin counting as the
// piecewise-constant density over [(k - 0.5) df, (k + 0.5) df] clipped to [0, Nyquist] (so the DC and Nyquist bins
// are half as wide).
//
// Curve: the PSD averaged over log2 f with a Hann (raised-cosine) kernel of half-width B = kCurveSmoothingOctaves,
// k(u) = 0.5 + 0.5 cos (pi u / B) for |u| < B, u = log2 (f / fc). Each bin gets its exact share of the kernel over its
// log-frequency extent, w_k = W (log2 (hi_k / fc)) - W (log2 (lo_k / fc)) with W (u) = 0.5 (u + B) + B / (2 pi)
// sin (pi u / B) (the integral of k, u clamped to [-B, B]), and the result is divided by sum w_k, so it stays a PSD in
// 1/Hz and the kernel is renormalised where Nyquist truncates it. A smooth kernel (instead of a hard-edged 1/3-octave
// box) keeps single partials from jumping in and out at the box edges, which drew a zig-zag that is not in the music.
//==============================================================================

namespace ps
{
const BandInfo& bandInfo (int bandIndex)
{
    static const BandInfo bands[kNumBands] = {
        { "Sub bass", "Sub", 20.0f, 60.0f, "Felt more than heard: sub bass, kick tail" },
        { "Bass", "Bass", 60.0f, 200.0f, "Bass and kick body, warmth" },
        { "Low mids", "Low-mid", 200.0f, 500.0f, "Mud and boxiness live here" },
        { "Mids", "Mid", 500.0f, 2000.0f, "Body of instruments and vocals" },
        { "High mids", "High-mid", 2000.0f, 6000.0f, "Presence, detail, harshness" },
        { "Air", "Air", 6000.0f, 20000.0f, "Brightness, sizzle and air" },
    };
    return bands[bandIndex < 0 ? 0 : (bandIndex >= kNumBands ? kNumBands - 1 : bandIndex)];
}

int fftOrderForSampleRate (double sampleRate) noexcept { return sampleRate <= 50000.0 ? 13 : (sampleRate <= 100000.0 ? 14 : 15); }

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSilenceGatePower = 1.0e-7;    // -70 dBFS mean square (mean of channels)
constexpr double kTinyPower = 1.0e-20;          // below this a band counts as empty
constexpr double kLoudnessOffsetDb = -0.691;
constexpr double kBroadbandLowHz = 20.0, kBroadbandHighHz = 20000.0;
constexpr long long kOfflineChunk = 1 << 16;

float dbOrNoValue (double power) noexcept
{
    const float db = powerToDb (power);
    return hasValue (db) ? db : kNoValue;
}

// Weighted sums of the three accumulators over a frequency range.
struct RangeSums
{
    double left = 0.0, right = 0.0, cross = 0.0;
};

// width = 100 * side / mid energy, correlation = cross / sqrt (left * right); see BandStats.
void stereoStats (const RangeSums& s, float& widthPct, float& correlation) noexcept
{
    const double mid = 0.25 * (s.left + s.right + 2.0 * s.cross);
    const double side = 0.25 * (s.left + s.right - 2.0 * s.cross);

    if (mid > kTinyPower)
        widthPct = (float) std::clamp (100.0 * std::max (0.0, side) / mid, 0.0, 300.0);
    else
        widthPct = side > kTinyPower ? 300.0f : 0.0f;

    const double lr = s.left * s.right;
    correlation = lr > kTinyPower * kTinyPower ? (float) std::clamp (s.cross / std::sqrt (lr), -1.0, 1.0) : 1.0f;
}
} // namespace

//==============================================================================
void TrackAnalyser::prepare (double newSampleRate, Averaging newAveraging)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    averaging = newAveraging;

    const int order = fftOrderForSampleRate (sampleRate);
    fft = std::make_unique<Fft> (order);
    fftSize = fft->size();
    hop = fftSize / 2;

    // Periodic Hann: overlapping frames at 50 % sum to a constant.
    window.resize ((size_t) fftSize);
    windowEnergy = 0.0;
    for (int i = 0; i < fftSize; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * i / fftSize);
        window[(size_t) i] = (float) w;
        windowEnergy += w * w;
    }

    inL.assign ((size_t) fftSize, 0.0f);
    inR.assign ((size_t) fftSize, 0.0f);
    scratch.assign ((size_t) fftSize, {});

    const size_t numBins = (size_t) (fftSize / 2 + 1);
    accPowL.assign (numBins, 0.0);
    accPowR.assign (numBins, 0.0);
    accCross.assign (numBins, 0.0);
    accRecentL.assign (numBins, 0.0);
    accRecentR.assign (numBins, 0.0);
    accRecentCross.assign (numBins, 0.0);

    KBiquad shelf, highPass;
    designKWeighting (sampleRate, shelf, highPass);
    kWeightPower.resize (numBins);
    for (size_t k = 0; k < numBins; ++k)
    {
        const double f = (double) k * sampleRate / fftSize;
        const double g = shelf.magnitudeAt (f, sampleRate) * highPass.magnitudeAt (f, sampleRate);
        kWeightPower[k] = g * g;
    }

    prepareCurveWeights();

    forget = std::exp (-((double) hop / sampleRate) / kRecentSeconds);

    loudness.prepare (sampleRate);
    peaks.prepare (sampleRate);
    reset();
}

void TrackAnalyser::prepareCurveWeights()
{
    const int half = fftSize / 2;
    const double binHz = sampleRate / fftSize;
    const double nyquist = 0.5 * sampleRate;
    const double b = kCurveSmoothingOctaves;
    constexpr double tinyHz = 1.0e-9;   // lower edge of the DC bin for the logarithm (it lies outside every kernel)

    // Integral of the kernel 0.5 + 0.5 cos (pi u / B) from -B to u.
    const auto kernelIntegral = [b] (double u)
    {
        u = std::clamp (u, -b, b);
        return 0.5 * (u + b) + b / (2.0 * kPi) * std::sin (kPi * u / b);
    };

    curveWeights.clear();
    std::vector<double> shares;

    for (int i = 0; i < kCurvePoints; ++i)
    {
        curveWeightStart[(size_t) i] = (int) curveWeights.size();
        curveFirstBin[(size_t) i] = 0;

        const double fc = curveFrequency (i);
        if (fc >= nyquist)
            continue;

        const double lo = fc * std::exp2 (-b), hi = std::min (fc * std::exp2 (b), nyquist);
        const int k0 = std::clamp ((int) std::floor (lo / binHz + 0.5), 0, half);
        const int k1 = std::clamp ((int) std::floor (hi / binHz + 0.5), k0, half);

        shares.assign ((size_t) (k1 - k0 + 1), 0.0);
        double mass = 0.0;

        for (int k = k0; k <= k1; ++k)
        {
            const double binLo = std::max (0.0, (k - 0.5) * binHz), binHi = std::min (nyquist, (k + 0.5) * binHz);
            if (! (binHi > binLo))
                continue;

            const double share = std::max (0.0, kernelIntegral (std::log2 (binHi / fc))
                                                    - kernelIntegral (std::log2 (std::max (binLo, tinyHz) / fc)));
            const double oneSided = (k == 0 || k == half) ? 1.0 : 2.0;

            // The bin's PSD is oneSided * power / its width; weighted by the bin's share of the kernel.
            shares[(size_t) (k - k0)] = share * oneSided / (binHi - binLo);
            mass += share;
        }

        if (! (mass > 0.0))
            continue;

        curveFirstBin[(size_t) i] = k0;
        for (double s : shares)
            curveWeights.push_back ((float) (s / mass));
    }

    curveWeightStart[(size_t) kCurvePoints] = (int) curveWeights.size();
}

void TrackAnalyser::setAveraging (Averaging newAveraging) noexcept
{
    // Both averages are always accumulated (see analyseFrame), so this only picks the one profile() reports.
    averaging = newAveraging;
}

void TrackAnalyser::reset() noexcept
{
    loudness.reset();
    peaks.reset();

    std::fill (inL.begin(), inL.end(), 0.0f);
    std::fill (inR.begin(), inR.end(), 0.0f);
    std::fill (accPowL.begin(), accPowL.end(), 0.0);
    std::fill (accPowR.begin(), accPowR.end(), 0.0);
    std::fill (accCross.begin(), accCross.end(), 0.0);
    std::fill (accRecentL.begin(), accRecentL.end(), 0.0);
    std::fill (accRecentR.begin(), accRecentR.end(), 0.0);
    std::fill (accRecentCross.begin(), accRecentCross.end(), 0.0);

    fill = 0;
    sawDifferentChannels = false;
    weightSum = 0.0;
    weightRecent = 0.0;
    analysedSeconds = 0.0;
    samplesPushed = 0;
    sumSquares = 0.0;
    chunkSquares = 0.0;
    chunkFill = 0;
    nonSilentSamples = 0;
}

void TrackAnalyser::push (const float* left, const float* right, int numSamples) noexcept
{
    if (fft == nullptr || left == nullptr || numSamples <= 0)
        return;

    loudness.process (left, right, numSamples);
    peaks.process (left, right, numSamples);
    samplesPushed += numSamples;

    if (right != nullptr && ! sawDifferentChannels)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            if (left[i] != right[i])
            {
                sawDifferentChannels = true;
                break;
            }
        }
    }

    // Mono input is analysed as L == R, so band levels (mean of channels) equal the level of the one channel.
    const float* r = right != nullptr ? right : left;

    for (int pos = 0; pos < numSamples;)
    {
        const int n = std::min (numSamples - pos, fftSize - fill);

        for (int i = 0; i < n; ++i)
        {
            const float xl = left[pos + i], xr = r[pos + i];
            inL[(size_t) (fill + i)] = xl;
            inR[(size_t) (fill + i)] = xr;

            chunkSquares += 0.5 * ((double) xl * xl + (double) xr * xr);

            if (++chunkFill == hop)
            {
                const double meanSquare = chunkSquares / hop;
                if (meanSquare >= kSilenceGatePower && std::isfinite (meanSquare))
                {
                    sumSquares += chunkSquares;
                    nonSilentSamples += hop;
                }

                chunkSquares = 0.0;
                chunkFill = 0;
            }
        }

        fill += n;
        pos += n;

        if (fill == fftSize)
        {
            analyseFrame();
            std::copy (inL.begin() + hop, inL.end(), inL.begin());
            std::copy (inR.begin() + hop, inR.end(), inR.begin());
            fill = fftSize - hop;
        }
    }
}

void TrackAnalyser::analyseFrame() noexcept
{
    double power = 0.0;
    for (int i = 0; i < fftSize; ++i)
        power += (double) inL[(size_t) i] * inL[(size_t) i] + (double) inR[(size_t) i] * inR[(size_t) i];

    power /= 2.0 * fftSize;

    // Silence gate (the negated comparison also skips NaN frames, isfinite skips inf).
    if (! (power >= kSilenceGatePower) || ! std::isfinite (power))
        return;

    for (int i = 0; i < fftSize; ++i)
        scratch[(size_t) i] = { inL[(size_t) i] * window[(size_t) i], inR[(size_t) i] * window[(size_t) i] };

    fft->forward (scratch.data());

    const double f = forget;
    const int mask = fftSize - 1;

    for (int k = 0; k <= fftSize / 2; ++k)
    {
        const std::complex<float> z = scratch[(size_t) k];
        const std::complex<float> zc = std::conj (scratch[(size_t) ((fftSize - k) & mask)]);

        // X_L = (z + zc) / 2;  X_R = (z - zc) / 2i = (Im (z - zc) - i Re (z - zc)) / 2
        const double lRe = 0.5 * ((double) z.real() + zc.real());
        const double lIm = 0.5 * ((double) z.imag() + zc.imag());
        const double rRe = 0.5 * ((double) z.imag() - zc.imag());
        const double rIm = -0.5 * ((double) z.real() - zc.real());

        const double pl = lRe * lRe + lIm * lIm, pr = rRe * rRe + rIm * rIm, cross = lRe * rRe + lIm * rIm;
        const size_t bin = (size_t) k;

        accPowL[bin] += pl;
        accPowR[bin] += pr;
        accCross[bin] += cross;

        accRecentL[bin] = accRecentL[bin] * f + pl;
        accRecentR[bin] = accRecentR[bin] * f + pr;
        accRecentCross[bin] = accRecentCross[bin] * f + cross;
    }

    // The first frame covers fftSize new samples, every later one hop new samples.
    analysedSeconds += (weightSum > 0.0 ? hop : fftSize) / sampleRate;
    weightSum += 1.0;
    weightRecent = weightRecent * f + 1.0;
}

//==============================================================================
TrackProfile TrackAnalyser::profile() const
{
    TrackProfile p;

    p.integratedLufs = loudness.integratedLufs();
    p.shortTermLufs = loudness.shortTermLufs();
    p.momentaryLufs = loudness.momentaryLufs();
    p.loudnessRangeLu = loudness.loudnessRange();

    p.truePeakDb = peaks.truePeakDb();
    p.samplePeakDb = peaks.samplePeakDb();
    p.rmsDb = nonSilentSamples > 0 ? dbOrNoValue (sumSquares / (double) nonSilentSamples) : kNoValue;

    if (hasValue (p.samplePeakDb) && hasValue (p.rmsDb))
        p.crestDb = p.samplePeakDb - p.rmsDb;

    // Sample peak, not true peak: see TrackProfile::plrDb.
    if (hasValue (p.samplePeakDb) && hasValue (p.integratedLufs))
        p.plrDb = p.samplePeakDb - p.integratedLufs;

    p.isMono = samplesPushed > 0 && ! sawDifferentChannels;
    p.analysedSeconds = analysedSeconds;
    p.valid = weightSum > 0.0 && analysedSeconds >= kMinProfileSeconds - 1.0e-9;

    if (! (weightSum > 0.0) || fft == nullptr)
        return p;

    // The average this profile reports (weightRecent > 0 whenever weightSum > 0).
    const bool recent = averaging == Averaging::Recent;
    const std::vector<double>& accL = recent ? accRecentL : accPowL;
    const std::vector<double>& accR = recent ? accRecentR : accPowR;
    const std::vector<double>& accX = recent ? accRecentCross : accCross;
    const double weight = recent ? weightRecent : weightSum;

    if (! (weight > 0.0))
        return p;

    const int half = fftSize / 2;
    const double binHz = sampleRate / fftSize;
    const double nyquist = 0.5 * sampleRate;
    const double norm = 1.0 / (weight * fftSize * windowEnergy);

    // Linear power of bin k (per channel, already averaged over frames).
    const auto binScale = [&] (int k) { return (k == 0 || k == half) ? norm : 2.0 * norm; };

    // Integrates the PSD over [lo, hi) Hz: power in that range.
    const auto integrate = [&] (double lo, double hi)
    {
        RangeSums s;
        lo = std::max (lo, 0.0);
        hi = std::min (hi, nyquist);

        if (! (hi > lo))
            return s;

        const int k0 = std::max (0, (int) std::floor (lo / binHz + 0.5));
        const int k1 = std::min (half, (int) std::floor (hi / binHz + 0.5));

        for (int k = k0; k <= k1; ++k)
        {
            const double binLo = std::max (0.0, (k - 0.5) * binHz), binHi = std::min (nyquist, (k + 0.5) * binHz);
            const double overlap = std::min (hi, binHi) - std::max (lo, binLo);
            if (overlap <= 0.0)
                continue;

            const double w = binScale (k) * overlap / (binHi - binLo);
            s.left += accL[(size_t) k] * w;
            s.right += accR[(size_t) k] * w;
            s.cross += accX[(size_t) k] * w;
        }

        return s;
    };

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& info = bandInfo (b);
        const RangeSums s = integrate (info.lowHz, info.highHz);
        auto& band = p.bands[(size_t) b];

        band.levelDb = dbOrNoValue (0.5 * (s.left + s.right));
        stereoStats (s, band.widthPct, band.correlation);
    }

    {
        float lowEndWidth = 0.0f;
        stereoStats (integrate (kBroadbandLowHz, kBroadbandHighHz), p.widthPct, p.correlation);
        stereoStats (integrate (kBroadbandLowHz, kLowEndMonoHz), lowEndWidth, p.lowEndCorrelation);
    }

    if (p.isMono)
    {
        // L and R were bit-identical: report the exact values instead of rounding noise.
        for (auto& band : p.bands)
        {
            band.widthPct = 0.0f;
            band.correlation = 1.0f;
        }

        p.widthPct = 0.0f;
        p.correlation = 1.0f;
        p.lowEndCorrelation = 1.0f;
    }

    // Hann-smoothed PSD (weights from prepareCurveWeights: kernel share * one-sided factor / bin width / kernel mass).
    for (int i = 0; i < kCurvePoints; ++i)
    {
        const int begin = curveWeightStart[(size_t) i], end = curveWeightStart[(size_t) i + 1];
        if (begin >= end)
            continue;

        double sum = 0.0;
        for (int j = begin, k = curveFirstBin[(size_t) i]; j < end; ++j, ++k)
            sum += (double) curveWeights[(size_t) j] * (accL[(size_t) k] + accR[(size_t) k]);

        p.curveDb[(size_t) i] = dbOrNoValue (0.5 * norm * sum);
    }

    // K-weighted power of the averaged spectrum, on the BS.1770 scale.
    double weighted = 0.0;
    for (int k = 0; k <= half; ++k)
        weighted += kWeightPower[(size_t) k] * (accL[(size_t) k] + accR[(size_t) k]) * binScale (k);

    if (weighted > 0.0)
    {
        const float db = (float) (kLoudnessOffsetDb + 10.0 * std::log10 (weighted));
        p.spectralLoudnessDb = hasValue (db) ? db : kNoValue;
    }

    return p;
}

//==============================================================================
TrackProfile analyseRegion (const float* left, const float* right, long long startSample, long long endSample,
                            double sampleRate)
{
    TrackAnalyser analyser;
    analyser.prepare (sampleRate, Averaging::SinceReset);

    startSample = std::max (0LL, startSample);

    for (long long pos = startSample; left != nullptr && pos < endSample; pos += kOfflineChunk)
    {
        const int n = (int) std::min (kOfflineChunk, endSample - pos);
        analyser.push (left + pos, right != nullptr ? right + pos : nullptr, n);
    }

    return analyser.profile();
}

std::vector<float> shortTermLoudnessCurve (const float* left, const float* right, long long numSamples,
                                           double sampleRate)
{
    std::vector<float> curve;

    if (left == nullptr || numSamples <= 0)
        return curve;

    LoudnessMeter meter;
    meter.prepare (sampleRate);
    curve.reserve ((size_t) ((double) numSamples * 10.0 / meter.getSampleRate()) + 1);

    // One element per completed step; a partial step at the end is not included.
    for (long long pos = 0; pos + meter.samplesToNextStep() <= numSamples;)
    {
        const int n = meter.samplesToNextStep();
        meter.process (left + pos, right != nullptr ? right + pos : nullptr, n);
        pos += n;
        curve.push_back (meter.shortTermLufs());
    }

    return curve;
}

float shortTermAt (const std::vector<float>& curve, double seconds) noexcept
{
    if (! (seconds >= 0.0) || ! std::isfinite (seconds))
        return kNoValue;

    // Element i ends at (i + 1) * 0.1 s; the epsilon keeps exact multiples of 0.1 s on their own element.
    const double index = std::floor (seconds * 10.0 + 1.0e-6) - 1.0;

    if (index < 0.0 || index >= (double) curve.size())
        return kNoValue;

    return curve[(size_t) index];
}

WaveformOverview makeWaveformOverview (const float* left, const float* right, long long numSamples, int columns)
{
    WaveformOverview o;
    const size_t numColumns = (size_t) std::max (0, columns);
    o.minimum.assign (numColumns, 0.0f);
    o.maximum.assign (numColumns, 0.0f);

    if (left == nullptr || numSamples <= 0 || numColumns == 0)
        return o;

    for (long long c = 0; c < (long long) numColumns; ++c)
    {
        const long long start = c * numSamples / (long long) numColumns;
        const long long end = std::max (start + 1, (c + 1) * numSamples / (long long) numColumns);

        float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();

        for (long long i = start; i < end; ++i)
        {
            const float v = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];
            lo = std::min (lo, v);
            hi = std::max (hi, v);
        }

        o.minimum[(size_t) c] = std::clamp (lo, -1.0f, 1.0f);
        o.maximum[(size_t) c] = std::clamp (hi, -1.0f, 1.0f);
    }

    return o;
}

} // namespace ps
