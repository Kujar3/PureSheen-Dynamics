#include "assist/AssistFeatures.h"

#include "core/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>

// Short-time sub-band grid: mid m = (L + R) / 2 and side s = (L - R) / 2 go through one packed complex FFT per frame
// (m real, s imaginary, separated with Fft::splitPacked). The power of bin k is c_k |X[k]|^2 / (N sum w^2) (one-sided
// Parseval scale, as in TrackAnalysis), and every bin adds to the sub-band that holds its centre frequency. Frames
// whose mean power (mid + side) is below -70 dBFS are dropped, so pauses do not count.
namespace ps::assist
{
namespace
{
constexpr double kMinHz = 20.0;
constexpr double kSilentPower = 1.0e-7;   // -70 dBFS

double powDb (double p) noexcept { return p > 1.0e-20 ? 10.0 * std::log10 (p) : (double) kNoValue; }

// Overlap weight of sub-band b with [lo, hi) in log2 f (0..1).
double overlap (int b, double lo, double hi) noexcept
{
    const double a0 = std::log2 (subBandLowHz (b)), a1 = std::log2 (subBandLowHz (b + 1));
    const double b0 = std::log2 (std::max (lo, 1.0)), b1 = std::log2 (std::max (hi, 1.0));
    const double w = std::min (a1, b1) - std::max (a0, b0);
    return w > 0.0 ? w / (a1 - a0) : 0.0;
}

// Per-frame power of a band (mid + side), plus the mid / side sums over all frames.
void bandSeries (const Analysis& a, double lo, double hi, std::vector<double>& series, double& midSum, double& sideSum)
{
    series.assign ((size_t) a.numFrames, 0.0);
    midSum = sideSum = 0.0;

    for (int b = 0; b < kSubBands; ++b)
    {
        const double w = overlap (b, lo, hi);

        if (w <= 0.0)
            continue;

        for (int f = 0; f < a.numFrames; ++f)
        {
            const double m = a.midPow[(size_t) f * kSubBands + (size_t) b];
            const double s = a.sidePow[(size_t) f * kSubBands + (size_t) b];
            series[(size_t) f] += w * (m + s);
            midSum += w * m;
            sideSum += w * s;
        }
    }
}

double percentileOf (std::vector<double> v, double pct)
{
    if (v.empty())
        return 0.0;

    const double pos = std::clamp (pct, 0.0, 100.0) / 100.0 * (double) (v.size() - 1);
    const auto i = (size_t) pos;
    std::nth_element (v.begin(), v.begin() + (long) i, v.end());
    const double lo = v[i];

    if (i + 1 >= v.size())
        return lo;

    const double hi = *std::min_element (v.begin() + (long) i + 1, v.end());
    return lo + (hi - lo) * (pos - (double) i);
}
} // namespace

double subBandLowHz (int subBand) noexcept
{
    return std::min (20000.0, kMinHz * std::pow (2.0, (double) subBand / 6.0));
}

double subBandCentreHz (int subBand) noexcept
{
    return std::sqrt (subBandLowHz (subBand) * subBandLowHz (subBand + 1));
}

float curveAt (const Curve& c, double hz) noexcept
{
    if (! (hz >= kCurveMinHz && hz <= kCurveMaxHz))
        return kNoValue;

    const double t = std::log (hz / kCurveMinHz) / std::log ((double) kCurveMaxHz / kCurveMinHz) * (kCurvePoints - 1);
    const int i = std::min ((int) t, kCurvePoints - 2);
    const double u = t - i;

    if (c[(size_t) i] <= kNoValue || c[(size_t) i + 1] <= kNoValue)
        return kNoValue;

    return (float) (c[(size_t) i] + (c[(size_t) i + 1] - c[(size_t) i]) * u);
}

BandFeatures bandFeatures (const Analysis& a, double lo, double hi)
{
    BandFeatures f;

    if (! a.valid || a.numFrames < 2)
        return f;

    std::vector<double> series;
    double midSum = 0.0, sideSum = 0.0;
    bandSeries (a, lo, hi, series, midSum, sideSum);

    double total = 0.0;
    for (double p : series)
        total += p;

    f.levelDb = (float) powDb (total / (double) a.numFrames);
    // (side energy with no mid = fully out of phase: the widest value, as in TrackAnalysis)
    f.widthPct = midSum > 1.0e-30 ? (float) std::clamp (100.0 * sideSum / midSum, 0.0, 300.0)
                                  : (sideSum > 1.0e-30 ? 300.0f : 0.0f);
    f.correlation = midSum + sideSum > 1.0e-30 ? (float) ((midSum - sideSum) / (midSum + sideSum)) : 1.0f;

    // Onsets: mean positive rise of the band level from one frame to the next, with a floor 60 dB under the band's
    // loudest frames so near-silent frames do not count as rises.
    std::vector<double> db (series.size());
    const double peak = *std::max_element (series.begin(), series.end());
    const double floorPow = std::max (peak * 1.0e-6, 1.0e-20);

    for (size_t i = 0; i < series.size(); ++i)
        db[i] = 10.0 * std::log10 (std::max (series[i], floorPow));

    double rise = 0.0;
    for (size_t i = 1; i < db.size(); ++i)
        rise += std::max (0.0, db[i] - db[i - 1]);

    f.onsetDb = (float) (rise / (double) (db.size() - 1));
    f.p50Db = (float) percentileOf (db, 50.0);
    f.p90Db = (float) percentileOf (db, 90.0);
    f.p99Db = (float) percentileOf (db, 99.0);
    f.crestDb = f.p99Db - f.p50Db;
    return f;
}

float bandPercentileDb (const Analysis& a, double lo, double hi, double percentile)
{
    if (! a.valid || a.numFrames < 1)
        return kNoValue;

    std::vector<double> series;
    double m = 0.0, s = 0.0;
    bandSeries (a, lo, hi, series, m, s);

    for (auto& p : series)
        p = powDb (p);

    return (float) percentileOf (series, percentile);
}

Analysis analyse (const float* left, const float* right, long long numSamples, double sampleRate)
{
    Analysis a;
    a.sampleRate = sampleRate;

    if (left == nullptr || numSamples <= 0 || ! (sampleRate > 0.0))
        return a;

    a.profile = analyseRegion (left, right, 0, numSamples, sampleRate);

    // ~46 ms frames at 44.1 / 48 kHz (2048), scaled with the rate; hop = 1/4 frame.
    int order = 11;
    while (sampleRate / (double) (1 << order) > 32.0 && order < 15)
        ++order;

    const int n = 1 << order, hop = n / 4, half = n / 2;
    const Fft fft (order);
    std::vector<float> window ((size_t) n);
    double windowEnergy = 0.0;

    for (int i = 0; i < n; ++i)
    {
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / n));
        windowEnergy += (double) window[(size_t) i] * window[(size_t) i];
    }

    // Bin -> sub-band map
    std::vector<int> binBand ((size_t) half + 1, -1);
    for (int k = 1; k <= half; ++k)
    {
        const double hz = k * sampleRate / n;
        if (hz < kMinHz || hz >= 20000.0)
            continue;
        binBand[(size_t) k] = std::clamp ((int) std::floor (6.0 * std::log2 (hz / kMinHz)), 0, kSubBands - 1);
    }

    std::vector<std::complex<float>> z ((size_t) n), mid ((size_t) half + 1), side ((size_t) half + 1);
    std::array<double, kSubBands> mp {}, sp {};
    const double scale = 1.0 / ((double) n * windowEnergy);

    for (long long start = 0; start + n <= numSamples; start += hop)
    {
        double frameSq = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const float l = left[start + i];
            const float r = right != nullptr ? right[start + i] : l;
            frameSq += 0.5 * ((double) l * l + (double) r * r);
            z[(size_t) i] = { 0.5f * (l + r) * window[(size_t) i], 0.5f * (l - r) * window[(size_t) i] };
        }

        if (frameSq / n < kSilentPower)
            continue;

        fft.forward (z.data());
        fft.splitPacked (z.data(), mid.data(), side.data());
        mp.fill (0.0);
        sp.fill (0.0);

        for (int k = 1; k <= half; ++k)
        {
            const int b = binBand[(size_t) k];
            if (b < 0)
                continue;
            const double c = (k == half ? 1.0 : 2.0) * scale;
            mp[(size_t) b] += c * std::norm (mid[(size_t) k]);
            sp[(size_t) b] += c * std::norm (side[(size_t) k]);
        }

        for (int b = 0; b < kSubBands; ++b)
        {
            a.midPow.push_back ((float) mp[(size_t) b]);
            a.sidePow.push_back ((float) sp[(size_t) b]);
        }

        ++a.numFrames;
    }

    a.framesPerSecond = sampleRate / hop;
    a.seconds = a.profile.analysedSeconds;
    a.valid = a.numFrames >= 8 && a.profile.valid;

    // Tonal balance relative to pink (octave density = PSD + 10 log10 f), 0 dB mean over 100 Hz .. 5.6 kHz.
    a.octaveDb.fill (kNoValue);
    double sum = 0.0;
    int count = 0;

    for (int i = 0; i < kCurvePoints; ++i)
    {
        const float v = a.profile.curveDb[(size_t) i];
        if (v <= kNoValue)
            continue;
        const double hz = curveFrequency (i);
        a.octaveDb[(size_t) i] = (float) (v + 10.0 * std::log10 (hz / 1000.0));
        if (hz >= 100.0 && hz <= 5600.0)
        {
            sum += a.octaveDb[(size_t) i];
            ++count;
        }
    }

    for (auto& v : a.octaveDb)
        if (v > kNoValue && count > 0)
            v -= (float) (sum / count);

    const double edges[kProcBands + 1] { 20.0, kProcCrossovers[0], kProcCrossovers[1], kProcCrossovers[2], 20000.0 };
    for (int b = 0; b < kProcBands; ++b)
        a.procBands[(size_t) b] = bandFeatures (a, edges[b], edges[b + 1]);

    return a;
}
} // namespace ps::assist
