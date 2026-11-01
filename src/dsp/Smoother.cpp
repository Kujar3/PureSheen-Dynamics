#include "dsp/Smoother.h"

#include "dsp/SpectralEngine.h"

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__) || defined(__x86_64__)
 #include <xmmintrin.h>
 #define PS_SMTH_HAS_SSE_CSR 1
#endif

// Smoother (dsp-smth): a cut-only, 32-band spectral compressor on ps::SpectralEngine (base N 1024, hop N / 4,
// latency N), measured from the reference (see the gain computer below). The algorithm is the steps of Smoother.h;
// this file adds the choices the header leaves open:
//   - Detector of a lane: Stereo / T/S = the mean of both channels' bin powers (a power-sum detector scaled so a mono
//     signal reads its per-channel level); M/S = each channel alone. X (b) is in dB re a full-scale sine per octave.
//   - Detector integration: X uses the band power integrated with tau = clamp (4 / bandwidth, 5 ms, 40 ms) (a
//     time x bandwidth product of 4), so the narrow low bands do not flicker on noise.
//   - Amount ramps linearly over 50 ms (per hop); Tone and the region limits glide with a 30 ms one-pole per hop; a
//     mode change runs the final curve through an extra 80 ms one-pole for 300 ms. The pole is eased away over
//     the last 80 ms, so changing material cannot keep the extra smoothing active indefinitely.
//   - The gate uses X (unweighted). A band whose reduction released to within 1e-6 dB of 0 is set to exactly 0, so
//     the module returns to an exact null (idle skip) once every band has released.
//   - Region solo (a monitor): per bin gain = 1 inside the region, a raised cosine to 0 over 1/6 octave outside
//     (floored at -100 dB); the detector, ballistics, meters and learn keep running meanwhile.
//   - Learn (our own): scores what stands more than 3 dB above the frame's mean band level (gated bands score 0).
//     reset() keeps the scores (only a new learnRegion start clears them).
//   - Frames the engine flags silent hold every state, publish the band levels at -150 dB and return unity.
namespace ps
{
double smootherBandHz (int band, double sampleRate) noexcept
{
    auto mel = [] (double hz) { return 2595.0 * std::log10 (1.0 + hz / 700.0); };
    auto hzOf = [] (double m) { return 700.0 * (std::pow (10.0, m / 2595.0) - 1.0); };
    const double top = std::min (20000.0, 0.475 * (sampleRate > 0.0 ? sampleRate : 48000.0));
    const double lo = mel (20.0), hi = mel (top);
    const double centre = lo + (hi - lo) * ((double) std::clamp (band, 0, kSmootherBands - 1) + 1.0) / (kSmootherBands + 1.0);
    return hzOf (centre);
}

namespace
{
constexpr int kB = kSmootherBands;
constexpr double kPi = 3.14159265358979323846;
constexpr double kLn2 = 0.69314718055994530942;
constexpr double kFloorDb = -150.0;
constexpr double kSnapDb = 1.0e-6;          // a released band this close to 0 dB becomes exactly 0
constexpr double kSoloFloor = 1.0e-5;       // region solo: -100 dB outside the region
constexpr double kDetectTB = 4.0;           // detector integration: time x bandwidth >= 4 (a stable level estimate)
constexpr double kDetectMinSec = 0.005, kDetectMaxSec = 0.040;

#if PS_SMTH_HAS_SSE_CSR
struct ScopedFlushDenormals
{
    ScopedFlushDenormals() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }   // FTZ | DAZ
    ~ScopedFlushDenormals() { _mm_setcsr (saved); }
    unsigned int saved;
};
#else
struct ScopedFlushDenormals {};
#endif

// The gain computer, measured from the reference (out/importer/tools/spsfit.py, spsfit2.py: pink noise in level steps,
// Amount 60..100, the three styles, tilt -100 / +50 / +100; fit rms 0.3..0.6 dB):
//   e  = X + 6.98 - 3.44 log2 (f / 1 kHz) - 0.173 tone log2 (f / f_c) - T,  T = -65.1 + (100 - amount) dB
//        (X: octave density re a full-scale sine per octave, so X + 6.98 is the level of pink noise of that density;
//        the detector reads per-Hz level, 3.44 dB / octave below the octave density; f_c = the region's centre)
//   GR = -mask M tanh (r softknee (e, K) / M): a compressor (slope r) that saturates at M
struct ModeShape
{
    double slope, knee, maxGr;
};

ModeShape modeShape (SmootherMode m) noexcept
{
    switch (m)
    {
        case SmootherMode::Gentle: return { 0.534, 2.5, 18.1 };
        case SmootherMode::Strong: return { 1.0, 2.9, 19.8 };
        case SmootherMode::Medium:
        default:                   return { 0.772, 6.0, 17.6 };
    }
}

constexpr double kPinkOffsetDb = 6.98, kDetectorSlopeDb = 3.44, kToneSlopeDb = 0.173;
constexpr double kThresholdAt100 = -65.1, kThresholdPerPercent = 1.0;
constexpr double kMaskWidthOct = 1.3;
constexpr double kLearnMarginDb = 3.0;  // Learn scores what stands this far above the frame's mean band level   // the region edges: raised cosine this wide, centred on each limit

double softKnee (double e, double w) noexcept
{
    if (! (e > -0.5 * w))
        return 0.0;
    if (e >= 0.5 * w)
        return e;
    const double t = e + 0.5 * w;
    return t * t / (2.0 * w);
}

double dbToPower (double db) noexcept { return std::pow (10.0, 0.1 * db); }

// Region mask (measured: the reference's reduction is half at each limit): raised-cosine edges kMaskWidthOct wide,
// centred on the limits (log2 Hz); 1 well inside, 0 well outside.
double regionMask (double l2, double lo, double hi) noexcept
{
    auto edge = [] (double x) { const double t = std::clamp (x / kMaskWidthOct + 0.5, 0.0, 1.0); return 0.5 - 0.5 * std::cos (kPi * t); };
    return edge (l2 - lo) * edge (hi - l2);
}

// Region solo (a monitor): 1 inside [lo, hi], raised cosine to 0 over 1/6 octave outside.
double soloMask (double l2, double lo, double hi) noexcept
{
    const double d = std::max (lo - l2, l2 - hi);
    if (d <= 0.0)
        return 1.0;
    if (d >= 1.0 / 6.0)
        return 0.0;
    return 0.5 * (1.0 + std::cos (kPi * d * 6.0));
}
} // namespace

struct Smoother::Impl final : SpectralEngine::Client
{
    SmootherMeters meters;
    SpectralEngine engine;
    double sampleRate = 48000.0;
    int hop = 256, numBins = 513;

    // band grid (prepare)
    double bandHz[kB] {}, bandLog2[kB] {}, toneOct[kB] {}, invWidthOct[kB] {};
    int refLo[kB] {}, refHi[kB] {}, refCount[kB] {};
    std::vector<int> binBand;          // lower covering band of bin k (-1: only band 0 or none)
    std::vector<float> binW;           // weight of band binBand + 1 (the upper one); the lower one has 1 - binW
    std::vector<float> binWLoN, binWHiN;   // normalised weights for the gain interpolation
    std::vector<unsigned char> binCovered;
    std::vector<double> binLog2;

    // per-hop coefficients (prepare)
    double kRefFall = 0.0, kGlide = 0.0, kMode = 0.0, amountStep = 1.0, twoHopsMs = 10.0;
    double kDetect[kB] {};
    int modeHopsTotal = 1, modeFadeHops = 1;

    // parameters / monitor (audio thread)
    SmootherParams params;
    bool soloRegion = false, learnRegion = false;
    bool dual = false, prevDual = false;

    struct Lane
    {
        double ref[kB] {}, gr[kB] {}, out[kB] {}, learn[kB] {}, det[kB] {};
        double x[kB] {}, target[kB] {};
        float lin[kB] {};
        bool refInit = false, paramsInit = false;
        double amount = 0.0, tone = 0.0, lowL2 = 0.0, highL2 = 0.0;
        int mode = -1, modeHops = 0;
        double regionPeak = kFloorDb, maxGr = 0.0;

        void clear() noexcept
        {
            std::fill (std::begin (ref), std::end (ref), 0.0);
            std::fill (std::begin (det), std::end (det), 0.0);
            std::fill (std::begin (gr), std::end (gr), 0.0);
            std::fill (std::begin (out), std::end (out), 0.0);
            std::fill (std::begin (x), std::end (x), kFloorDb);
            std::fill (std::begin (target), std::end (target), 0.0);
            std::fill (std::begin (lin), std::end (lin), 1.0f);
            refInit = paramsInit = false;
            amount = 0.0;
            mode = -1;
            modeHops = 0;
            regionPeak = kFloorDb;
            maxGr = 0.0;
        }
    };
    Lane lanes[2];
    double power[2][kB] {};

    //==========================================================================
    void prepare (double sr, int maxBlock)
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        engine.prepare (sampleRate, 1024, maxBlock);
        engine.setClient (this);
        hop = engine.getHopSize();
        numBins = engine.getFftSize() / 2 + 1;
        const double binHz = engine.getBinHz();
        const double hopSec = (double) hop / sampleRate;

        kRefFall = 1.0 - std::exp (-hopSec / 0.25);
        kGlide = 1.0 - std::exp (-hopSec / 0.03);
        kMode = 1.0 - std::exp (-hopSec / 0.08);
        amountStep = hopSec / 0.05;
        twoHopsMs = 2000.0 * hopSec;
        modeHopsTotal = std::max (1, (int) std::ceil (0.3 / hopSec));
        modeFadeHops = std::max (2, (int) std::ceil (0.08 / hopSec));

        auto mel = [] (double hz) { return 2595.0 * std::log10 (1.0 + hz / 700.0); };
        const double top = std::min (20000.0, 0.475 * sampleRate);
        const double melLo = mel (20.0), melHi = mel (top);
        const double step = (melHi - melLo) / (kB + 1.0);

        for (int b = 0; b < kB; ++b)
        {
            bandHz[b] = smootherBandHz (b, sampleRate);
            bandLog2[b] = std::log2 (bandHz[b]);
            toneOct[b] = std::log2 (bandHz[b] / 1000.0);
        }
        for (int b = 0; b < kB; ++b)
        {
            int lo = b, hi = b;
            while (lo > 0 && bandLog2[lo - 1] >= bandLog2[b] - 1.0)
                --lo;
            while (hi < kB - 1 && bandLog2[hi + 1] <= bandLog2[b] + 1.0)
                ++hi;
            refLo[b] = lo;
            refHi[b] = hi;
            int n = 0;
            for (int c = lo; c <= hi; ++c)
                n += std::abs (c - b) > 1 ? 1 : 0;
            refCount[b] = n;
        }

        binBand.assign ((size_t) numBins, -1);
        binW.assign ((size_t) numBins, 0.0f);
        binWLoN.assign ((size_t) numBins, 0.0f);
        binWHiN.assign ((size_t) numBins, 0.0f);
        binCovered.assign ((size_t) numBins, 0);
        binLog2.assign ((size_t) numBins, -1000.0);
        double width[kB] {}, hzWidth[kB] {};
        for (int k = 0; k < numBins; ++k)
        {
            const double f = k * binHz;
            if (k > 0)
                binLog2[(size_t) k] = std::log2 (f);
            const double u = (mel (f) - melLo) / step;    // 0 .. kB + 1 over the grid
            if (k == 0 || ! (u > 0.0) || ! (u < kB + 1.0))
                continue;
            const int j = std::min ((int) std::floor (u), kB);  // between edge j and j + 1
            const double frac = u - j;
            const int loBand = j - 1;                     // weight 1 - frac (its falling side)
            const int hiBand = j;                         // weight frac (its rising side)
            const double wLo = loBand >= 0 ? 1.0 - frac : 0.0;
            const double wHi = hiBand < kB ? frac : 0.0;
            if (wLo + wHi <= 0.0)
                continue;
            binBand[(size_t) k] = loBand;
            binW[(size_t) k] = (float) frac;
            binCovered[(size_t) k] = 1;
            binWLoN[(size_t) k] = (float) (wLo / (wLo + wHi));
            binWHiN[(size_t) k] = (float) (wHi / (wLo + wHi));
            const double oct = binHz / (f * kLn2);
            if (loBand >= 0)
            {
                width[loBand] += wLo * oct;
                hzWidth[loBand] += wLo * binHz;
            }
            if (hiBand < kB)
            {
                width[hiBand] += wHi * oct;
                hzWidth[hiBand] += wHi * binHz;
            }
        }
        for (int b = 0; b < kB; ++b)
        {
            invWidthOct[b] = width[b] > 0.0 ? 1.0 / width[b] : 0.0;
            const double tau = std::clamp (kDetectTB / std::max (1.0, hzWidth[b]), kDetectMinSec, kDetectMaxSec);
            kDetect[b] = 1.0 - std::exp (-hopSec / tau);
        }

        reset();
    }

    void reset() noexcept
    {
        engine.reset();
        for (auto& l : lanes)
            l.clear();
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            for (int b = 0; b < kB; ++b)
            {
                meters.grDb[s][b].store (0.0f);
                meters.bandLevelDb[s][b].store ((float) kFloorDb);
            }
            meters.regionPeakDb[s].store ((float) kFloorDb);
            meters.maxGrDb[s].store (0.0f);
        }
    }

    void clearLearn() noexcept
    {
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            std::fill (std::begin (lanes[s].learn), std::end (lanes[s].learn), 0.0);
            for (int b = 0; b < kB; ++b)
                meters.learnScore[s][b].store (0.0f);
        }
    }

    //==========================================================================
    // One lane, one hop: detector, gain computer, ballistics, smoothing. Returns true when the applied curve is 0 dB.
    bool updateLane (Lane& l, const double* p, const SmootherSetParams& sp) noexcept
    {
        // parameter glides
        const double aTarget = std::clamp ((double) sp.amountPercent * 0.01, 0.0, 1.0);
        const double toneT = std::clamp ((double) sp.tonePercent, -100.0, 100.0);
        const double lowT = std::log2 (std::clamp ((double) sp.lowHz, 20.0, 20000.0));
        const double highT = std::log2 (std::clamp ((double) sp.highHz, 20.0, 20000.0));
        if (! l.paramsInit)
        {
            l.paramsInit = true;
            l.amount = aTarget;
            l.tone = toneT;
            l.lowL2 = lowT;
            l.highL2 = highT;
        }
        else
        {
            l.amount = l.amount < aTarget ? std::min (aTarget, l.amount + amountStep) : std::max (aTarget, l.amount - amountStep);
            l.tone += (toneT - l.tone) * kGlide;
            l.lowL2 += (lowT - l.lowL2) * kGlide;
            l.highL2 += (highT - l.highL2) * kGlide;
        }
        const int modeIndex = (int) sp.mode;
        if (l.mode >= 0 && modeIndex != l.mode)
            l.modeHops = modeHopsTotal;
        l.mode = modeIndex;

        const ModeShape shape = modeShape (sp.mode);
        const double a = l.amount;
        const double threshold = kThresholdAt100 + (1.0 - a) * 100.0 * kThresholdPerPercent;
        const double tilt = kToneSlopeDb * l.tone;
        const double centreL2 = 0.5 * (l.lowL2 + l.highL2);

        // 1. band levels (integrated detector)
        double maxX = kFloorDb;
        for (int b = 0; b < kB; ++b)
        {
            l.det[b] = l.refInit ? l.det[b] + (p[b] - l.det[b]) * kDetect[b] : p[b];
            const double d = l.det[b] * invWidthOct[b];
            const double xdb = d > 1.0e-15 ? 10.0 * std::log10 (d) : kFloorDb;
            l.x[b] = xdb;
            maxX = std::max (maxX, xdb);
        }
        l.refInit = true;
        const double gateDb = std::max (-80.0, maxX - 50.0);
        double meanX = 0.0;
        {
            int count = 0;
            for (int b = 0; b < kB; ++b)
                if (l.x[b] >= gateDb)
                {
                    meanX += l.x[b];
                    ++count;
                }
            meanX = count > 0 ? meanX / count : kFloorDb;
        }

        // 2, 4 - 7. detector level vs the threshold, gain computer, gate, region mask (+ learn)
        double regionPeak = kFloorDb, bestMask = 0.0, bestMaskLevel = kFloorDb;
        for (int b = 0; b < kB; ++b)
        {
            const bool valid = l.x[b] >= gateDb;
            const double level = l.x[b] + kPinkOffsetDb - kDetectorSlopeDb * toneOct[b] - tilt * (bandLog2[b] - centreL2);
            const double e = level - threshold;
            if (learnRegion)
            {
                // Learn (our own): where the detector level stands out from the frame's mean level (a harsh region)
                const double eLearn = l.x[b] - meanX - kLearnMarginDb;
                l.learn[b] += valid && eLearn > 0.0 ? eLearn : 0.0;
            }

            const double m = regionMask (bandLog2[b], l.lowL2, l.highL2);
            if (bandLog2[b] >= l.lowL2 && bandLog2[b] <= l.highL2)
                regionPeak = std::max (regionPeak, l.x[b]);
            if (m > bestMask || (m == bestMask && m > 0.0 && l.x[b] > bestMaskLevel))
            {
                bestMask = m;
                bestMaskLevel = l.x[b];
            }

            double t = 0.0;
            if (valid && m > 0.0 && a > 0.0)
                t = -m * shape.maxGr * std::tanh (shape.slope * softKnee (e, shape.knee) / shape.maxGr);
            l.target[b] = std::min (0.0, t);
        }
        l.regionPeak = regionPeak > kFloorDb ? regionPeak : bestMaskLevel;

        // 8. ballistics
        const double aAtt = std::exp (-(double) hop / (std::max (1.0, (double) sp.attackMs) * 0.001 * sampleRate));
        const double relMs = std::max (2.0, (double) sp.releaseMs);
        const double aRel = std::exp (-(double) hop / (relMs * 0.001 * sampleRate));
        const double aRelFloor = std::exp (-(double) hop / (std::max (relMs, twoHopsMs) * 0.001 * sampleRate));
        bool releasing[kB];
        for (int b = 0; b < kB; ++b)
            releasing[b] = l.target[b] > l.gr[b];
        for (int b = 0; b < kB; ++b)
        {
            const double t = l.target[b];
            if (! releasing[b])
            {
                l.gr[b] = t + (l.gr[b] - t) * aAtt;
                continue;
            }
            const bool neighbour = (b > 0 && releasing[b - 1]) || (b < kB - 1 && releasing[b + 1]);
            l.gr[b] = t + (l.gr[b] - t) * (neighbour ? aRelFloor : aRel);
            if (t == 0.0 && l.gr[b] > -kSnapDb)
                l.gr[b] = 0.0;
        }

        // 9. [0.2 0.6 0.2] across bands (edges replicate), mode-change glide
        double s[kB];
        for (int b = 0; b < kB; ++b)
        {
            const double lo = l.gr[std::max (0, b - 1)], hi = l.gr[std::min (kB - 1, b + 1)];
            s[b] = std::min (0.0, 0.2 * lo + 0.6 * l.gr[b] + 0.2 * hi);
        }
        if (l.modeHops > 0)
        {
            // End the extra filter on a fixed schedule, not when a moving curve happens to settle.
            // Ease its coefficient to unity during the last 80 ms to avoid a discontinuous handoff.
            const double remaining = std::min (1.0, (double) (l.modeHops - 1) / (modeFadeHops - 1));
            const double blend = remaining * remaining * (3.0 - 2.0 * remaining);
            const double rate = 1.0 - (1.0 - kMode) * blend;
            for (int b = 0; b < kB; ++b)
            {
                l.out[b] = l.modeHops == 1 ? s[b] : l.out[b] + (s[b] - l.out[b]) * rate;
                if (l.out[b] > -kSnapDb && s[b] == 0.0)
                    l.out[b] = 0.0;
            }
            --l.modeHops;
        }
        else
        {
            std::copy (std::begin (s), std::end (s), std::begin (l.out));
        }

        bool unity = true;
        double maxGr = 0.0;
        for (int b = 0; b < kB; ++b)
        {
            const double g = std::min (0.0, l.out[b]);
            unity = unity && g == 0.0;
            maxGr = std::min (maxGr, g);
            l.lin[b] = g == 0.0 ? 1.0f : (float) std::pow (10.0, g / 20.0);
        }
        l.maxGr = maxGr;
        return unity;
    }

    void fillBandGains (const Lane& l, float* gain) const noexcept
    {
        for (int k = 0; k < numBins; ++k)
        {
            if (binCovered[(size_t) k] == 0)
                continue;   // stays at exactly 1
            const int lo = binBand[(size_t) k];
            const float gl = lo >= 0 ? l.lin[lo] - 1.0f : 0.0f;
            const float gh = lo + 1 < kB ? l.lin[lo + 1] - 1.0f : 0.0f;
            gain[k] = std::min (1.0f, 1.0f + binWLoN[(size_t) k] * gl + binWHiN[(size_t) k] * gh);
        }
    }

    void fillSoloGains (const Lane& l, float* gain) const noexcept
    {
        for (int k = 0; k < numBins; ++k)
            gain[k] = (float) std::max (kSoloFloor, soloMask (binLog2[(size_t) k], l.lowL2, l.highL2));
    }

    void publish (int set, const Lane& l) noexcept
    {
        for (int b = 0; b < kB; ++b)
        {
            meters.grDb[set][b].store ((float) l.out[b]);
            meters.bandLevelDb[set][b].store ((float) l.x[b]);
            if (learnRegion)
                meters.learnScore[set][b].store ((float) l.learn[b]);
        }
        meters.regionPeakDb[set].store ((float) l.regionPeak);
        meters.maxGrDb[set].store ((float) l.maxGr);
    }

    //==========================================================================
    bool computeGains (SpectralEngine::Frame& f) noexcept override
    {
        const int numLanes = dual ? 2 : 1;
        if (f.silent)
        {
            for (int s = 0; s < kNumChannelSets; ++s)
            {
                for (int b = 0; b < kB; ++b)
                    meters.bandLevelDb[s][b].store ((float) kFloorDb);
                meters.regionPeakDb[s].store ((float) kFloorDb);
            }
            return true;
        }

        for (int b = 0; b < kB; ++b)
            power[0][b] = power[1][b] = 0.0;
        const float* p0 = f.power[0];
        const float* p1 = f.power[1];
        for (int k = 0; k < f.numBins && k < numBins; ++k)
        {
            if (binCovered[(size_t) k] == 0)
                continue;
            const int lo = binBand[(size_t) k];
            const double wHi = binW[(size_t) k], wLo = 1.0 - wHi;
            const double a = dual ? (double) p0[k] : 0.5 * ((double) p0[k] + (double) p1[k]);
            if (lo >= 0) power[0][lo] += wLo * a;
            if (lo + 1 < kB) power[0][lo + 1] += wHi * a;
            if (dual)
            {
                const double c = p1[k];
                if (lo >= 0) power[1][lo] += wLo * c;
                if (lo + 1 < kB) power[1][lo + 1] += wHi * c;
            }
        }

        bool unity = true;
        for (int l = 0; l < numLanes; ++l)
            unity = updateLane (lanes[l], power[l], params.sets[l]) && unity;

        if (dual)
        {
            publish (0, lanes[0]);
            publish (1, lanes[1]);
        }
        else
        {
            publish (0, lanes[0]);
            publish (1, lanes[0]);
        }

        if (soloRegion)
        {
            fillSoloGains (lanes[0], f.gain[0]);
            if (! f.linked)
                fillSoloGains (dual ? lanes[1] : lanes[0], f.gain[1]);
            return false;
        }
        if (unity)
            return true;

        fillBandGains (lanes[0], f.gain[0]);
        if (! f.linked)
            fillBandGains (dual ? lanes[1] : lanes[0], f.gain[1]);
        return false;
    }
};

Smoother::Smoother() : impl (std::make_unique<Impl>()) {}
Smoother::~Smoother() = default;

void Smoother::prepare (double sampleRate, int maxBlockSize) { impl->prepare (sampleRate, maxBlockSize); }

void Smoother::reset() noexcept { impl->reset(); }

void Smoother::setParams (ParamValues v) noexcept
{
    if (v == nullptr)
        return;
    readSmootherParams (v, impl->params);
    if (getParamSet() == 1)
        impl->params.sets[0] = impl->params.sets[1];
}

void Smoother::setMonitor (const ModuleMonitor& m) noexcept
{
    auto& d = *impl;
    if (m.learnRegion && ! d.learnRegion)
        d.clearLearn();
    d.learnRegion = m.learnRegion;
    d.soloRegion = m.soloRegion;
}

void Smoother::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    if (channels == nullptr || numSamples <= 0)
        return;
    ScopedFlushDenormals noDenormals;
    auto& d = *impl;
    d.dual = ctx.channelMode == ChannelMode::MidSide || ctx.channelMode == ChannelMode::LeftRight;
    if (d.dual && ! d.prevDual)
        d.lanes[1].clear();
    d.prevDual = d.dual;
    d.engine.process (channels, numSamples, ! d.dual);
}

int Smoother::getLatencySamples() const noexcept { return impl->engine.getLatencySamples(); }
const SmootherMeters& Smoother::getMeters() const noexcept { return impl->meters; }
double Smoother::getSampleRate() const noexcept { return impl->sampleRate; }

// (added by ui-spectral) The Learn rule (Smoother.h).
bool smootherLearnedRegion (const SmootherMeters& m, int set, double sampleRate, double& lowHz, double& highHz) noexcept
{
    const int s = std::clamp (set, 0, kNumChannelSets - 1);
    double sc[kSmootherBands], total = 0.0;

    for (int b = 0; b < kSmootherBands; ++b)
    {
        const double v = (double) m.learnScore[s][b].load();
        sc[b] = std::isfinite (v) ? std::max (0.0, v) : 0.0;
        total += sc[b];
    }

    if (! (total > 0.0) || ! std::isfinite (total))
        return false;

    int bestLo = 0, bestHi = kSmootherBands - 1;

    for (int lo = 0; lo < kSmootherBands; ++lo)
    {
        double acc = 0.0;

        for (int hi = lo; hi < kSmootherBands; ++hi)
        {
            acc += sc[hi];

            if (acc >= 0.75 * total)
            {
                if (hi - lo < bestHi - bestLo)
                {
                    bestLo = lo;
                    bestHi = hi;
                }

                break;
            }
        }
    }

    // Half a band beyond the span.
    auto outer = [sampleRate] (int b, int dir)
    {
        const double c = smootherBandHz (b, sampleRate);
        const int nb = b + dir;

        if (nb >= 0 && nb < kSmootherBands)
            return std::sqrt (c * smootherBandHz (nb, sampleRate));

        return c * std::sqrt (c / smootherBandHz (b - dir, sampleRate));
    };

    lowHz = outer (bestLo, -1);
    highHz = outer (bestHi, 1);
    return true;
}
} // namespace ps
