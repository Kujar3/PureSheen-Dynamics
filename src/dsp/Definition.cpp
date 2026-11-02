#include "dsp/Definition.h"

#include "core/Loudness.h"
#include "dsp/SpectralEngine.h"

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_DEFN_HAS_SSE_CSR 1
#else
 #define PS_DEFN_HAS_SSE_CSR 0
#endif

// Definition (Definition.h): adaptive spectral contour toward pink + tilt, on ps::SpectralEngine (base size 1024,
// latency N = 1024 x k, hop N / 4).
//
// Per hop and lane (Stereo / T/S: one lane, power-sum detector, one curve for both channels; M/S: lane 0 = Mid with
// set A, lane 1 = Side with set B):
//   bands     1/6 octave centres 250 Hz x 2^(i/6) up to min (20 kHz, 0.45 fs); a centre is used only when it lies
//             >= 2 bins above the previous one (the bottom merges to ~1/2 octave at 48 kHz: 250, 354, 500, 630, 794,
//             891, 1000 Hz, then 1/6 octave; 32 bands at 48 kHz). Triangular weights in log f between neighbouring
//             centres (a partition of unity), mirrored half triangles at both ends. Octave width of a band = its pink
//             response (sum of w (k) binHz / (f_k ln 2)), so pink noise reads flat.
//   detector  band power through a 10 ms one-pole (per hop; never slower than the attack, so an attack of 5 ms
//             lands within 2 hops), L = 10 log10 (P / octWidth) (Stereo: power sum - 3 dB).
//             Gate: L < -80 dB or more than 60 dB under the loudest band -> the band keeps its last desired gain
//             and does not drive the offset.
//   target    line (b) = tilt x log2 (f_b / 1 kHz), o = mask x octave-width weighted mean of L - line over the
//             ungated bands of the region, D = line + o - L, raw = clamp (0.6 D, -6, +6).
//   smoothing Gaussian over log2 f (sigma 1/6 octave, truncated at 3 sigma, octave-width weighted), then x Amount.
//   loudness  (our choice) the per-band targets are shifted by one common c so the K-weighted power change over the
//             region is exactly zero: sum_b P_b K_b (10^(m_b clamp (s_b - c, +-6 A) / 10) - 1) = 0 (Newton, warm
//             start). That is the "K-weighted power-weighted mean dB change" of the header in its energy form, which
//             keeps the integrated loudness of steady material within ~0.1 LU. Definition then redistributes energy
//             (dull mixes: highs up, the dense mids slightly down) instead of mostly brightening, so A/B comparisons
//             stay fair (Gain Match is still available). |c| <= 3 dB x Amount: a band that holds most of the energy
//             (a +15..30 dB resonance) is still cut by at least half the limit instead of being lifted back by the
//             shift (such extreme input then loses a little loudness).
//   limits    target u_b = clamp (s_b - c, -6 A, +6 A): never more than 6 dB (Amount 100) / 6 A dB anywhere.
//   ballistics per band in dB (per-hop coefficients): attack tau while |u| grows with the same sign, release tau
//             (>= 2 hops) toward 0 or on a sign change; a rise above 0 dB is limited to +1 dB per hop. The detector's
//             own share of the response is taken off the one-pole time constants (attack - 5 ms, release - 15 ms), so
//             the measured 63 % times with equal Attack / Release track 50 / 150 / 500 ms within 10 % (CoreTests).
//             With fast attack / slow release, detector fluctuations bias the resting gain and shorten the measured
//             return to that resting gain. A gain within 1e-4 dB of its target snaps to it, so Amount 0 returns to
//             exact unity (engine idle skip, bit-exact null against the input delayed by N).
//   region    the mask is applied per BIN after the ballistics (dB x m (f_k), m = 1 inside [low, high], raised
//             cosine to 0 over 1/3 octave outside, exactly 0 below 300 Hz x 2^(-1/3) = 238 Hz), so bins outside the
//             region are exactly unity and the region handles respond at once (low / high glide over 20 ms). The
//             header orders mask before ballistics; applying it after is equivalent for a still region and keeps the
//             edges exact. The bin curve is then slope-limited to 3 dB per bin (|gain| only, toward 0), which keeps
//             the implied impulse response short (the 238..300 Hz fade is only ~1.3 bins wide at 48 kHz).
//   apply     band -> bin: linear in log f between centres (flat beyond the end centres), 10^(dB/20).
// Smoothing of the controls: Amount ramps linearly over 50 ms, tilt / low / high glide with a 20 ms one-pole; all of
// them act on targets that pass the ballistics. Region solo (ModuleMonitor::soloRegion): bin gain = m (f) (the input
// inside the region, no dry path, same latency), crossfaded over 20 ms (per hop) with the normal gains.
// Silent frames (< -150 dBFS) hold the detector and return unity; controls and gain ballistics keep advancing so
// edits made during a pause have settled before the next onset. Meters: the applied bin curve sampled at the display
// points (so they show exactly what the audio gets), the detector level (octave density), the largest boost / cut of
// the applied curve. Stereo / T/S: set B mirrors set A.
// Measured (CoreTests "defn."): on a dull music-like mix (pink + harmonic tones + kicks through a one-pole low-pass
// at 3 kHz), Amount 100 lifts the 6-16 kHz energy by +5.3 dB (10-16 kHz +6.0 dB), lowers 300 Hz - 2 kHz by 1.7 dB and
// leaves the integrated loudness within 0.02 LU (Amount 50: +2.8 dB). See tests/DefinitionTests.cpp for every number.
namespace ps
{
double definitionMaxGainDb (double amount) noexcept
{
    return amount > 0.0 ? 6.0 * std::pow (std::min (amount, 1.0), 0.91) : 0.0;
}

namespace
{
#if PS_DEFN_HAS_SSE_CSR
struct ScopedFlushDenormals
{
    ScopedFlushDenormals() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }   // FTZ | DAZ
    ~ScopedFlushDenormals() { _mm_setcsr (saved); }
    ScopedFlushDenormals (const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator= (const ScopedFlushDenormals&) = delete;
    unsigned int saved;
};
#else
struct ScopedFlushDenormals {};
#endif

constexpr double kPi = 3.14159265358979323846;
constexpr double kLn10 = 2.302585092994046;
constexpr double kFirstCentreHz = 250.0;
constexpr double kTopHz = 20000.0;
constexpr double kFade = 1.0 / 3.0;                     // octaves
constexpr double kFloorLog2 = 8.228818690495881 - kFade; // log2 (300) - 1/3: exactly 0 below 238 Hz
constexpr double kMaxDb = 6.0;   // the limit at Amount 100; definitionMaxGainDb scales it as Amount^0.91 (measured:
                                 // the reference's plateau on near-empty top octaves is +1.7 dB at Amount 25 and +6.0
                                 // at 100, a linear law gives 1.5). The unscaled gain is limited where that cap binds.
constexpr double kGateAbsDb = -80.0;
constexpr double kGateRelDb = 60.0;
constexpr double kDetectorMs = 10.0;
constexpr double kAmountRampMs = 50.0;
constexpr double kGlideMs = 20.0;
constexpr double kSoloMs = 20.0;
constexpr double kBoostSlewDb = 1.0;
constexpr double kSnapDb = 1.0e-4;
constexpr double kBinSlopeDb = 3.0;
constexpr double kMaxCompDb = 3.0;                     // bound of the loudness shift c (x Amount)
constexpr double kAttackLeadMs = 5.0;                  // the detector's share of the attack time
constexpr double kReleaseLeadMs = 15.0;                // the detector's share of the release time
constexpr float kLevelFloorDb = -120.0f;

double regionMask (double x, double lo, double hi) noexcept
{
    if (x < kFloorLog2 || x <= lo - kFade || x >= hi + kFade)
        return 0.0;
    if (x < lo)
        return 0.5 - 0.5 * std::cos (kPi * (x - (lo - kFade)) / kFade);
    if (x > hi)
        return 0.5 - 0.5 * std::cos (kPi * ((hi + kFade) - x) / kFade);
    return 1.0;
}
} // namespace

double definitionDisplayHz (int index) noexcept
{
    return 250.0 * std::pow (80.0, (double) index / (double) (kDefinitionDisplayPoints - 1));
}

struct Definition::Impl final : SpectralEngine::Client
{
    struct Band
    {
        double hz = 1000.0, log2Hz = 10.0, octWidth = 1.0, kPow = 1.0;
        int firstBin = 0, numBins = 0, weightOffset = 0;
    };

    struct Lane
    {
        std::vector<double> power;              // smoothed band power
        std::vector<double> levelDb, raw, smooth, target, gainDb, mask;
        std::vector<float> binDb;
        double amount = 0.0, tilt = 0.0, logLow = 0.0, logHigh = 0.0, solo = 0.0, comp = 0.0;
        bool fresh = true;

        void allocate (int numBands, int numBins)
        {
            power.assign ((size_t) numBands, 0.0);
            levelDb.assign ((size_t) numBands, (double) kLevelFloorDb);
            raw.assign ((size_t) numBands, 0.0);
            smooth.assign ((size_t) numBands, 0.0);
            target.assign ((size_t) numBands, 0.0);
            gainDb.assign ((size_t) numBands, 0.0);
            mask.assign ((size_t) numBands, 0.0);
            binDb.assign ((size_t) numBins, 0.0f);
        }

        void clear() noexcept
        {
            std::fill (power.begin(), power.end(), 0.0);
            std::fill (levelDb.begin(), levelDb.end(), (double) kLevelFloorDb);
            std::fill (raw.begin(), raw.end(), 0.0);
            std::fill (smooth.begin(), smooth.end(), 0.0);
            std::fill (target.begin(), target.end(), 0.0);
            std::fill (gainDb.begin(), gainDb.end(), 0.0);
            std::fill (mask.begin(), mask.end(), 0.0);
            std::fill (binDb.begin(), binDb.end(), 0.0f);
            amount = tilt = logLow = logHigh = solo = comp = 0.0;
            fresh = true;
        }

        void copyFrom (const Lane& o) noexcept
        {
            std::copy (o.power.begin(), o.power.end(), power.begin());
            std::copy (o.levelDb.begin(), o.levelDb.end(), levelDb.begin());
            std::copy (o.raw.begin(), o.raw.end(), raw.begin());
            std::copy (o.smooth.begin(), o.smooth.end(), smooth.begin());
            std::copy (o.target.begin(), o.target.end(), target.begin());
            std::copy (o.gainDb.begin(), o.gainDb.end(), gainDb.begin());
            std::copy (o.mask.begin(), o.mask.end(), mask.begin());
            std::copy (o.binDb.begin(), o.binDb.end(), binDb.begin());
            amount = o.amount; tilt = o.tilt; logLow = o.logLow; logHigh = o.logHigh; solo = o.solo; comp = o.comp;
            fresh = o.fresh;
        }
    };

    SpectralEngine engine;
    DefinitionMeters meters;
    DefinitionParams params;
    bool soloRegion = false;
    bool dual = false, prepared = false;

    double sampleRate = 48000.0, binHz = 46.875, hopSec = 256.0 / 48000.0;
    int numBins = 513, numBands = 0;
    std::vector<Band> bands;
    std::vector<float> weights;                 // per band, numBins weights from firstBin
    std::vector<double> smoothK;                // numBands x numBands (row b: weights of the neighbours of b)
    std::vector<double> binLog2;                // log2 (k binHz), k >= 1
    std::vector<int> binBand;                   // interpolation: band index b (value between b and b + 1)
    std::vector<double> binFrac;
    std::vector<double> bandPower;              // scratch: this hop's band power
    double dispBinPos[kDefinitionDisplayPoints] {};
    int dispBand[kDefinitionDisplayPoints] {};
    double dispFrac[kDefinitionDisplayPoints] {};
    Lane lanes[kNumChannelSets];

    double glideCoeff = 0.0, amountStep = 1.0, soloStep = 1.0;

    void prepare (double sr, int maxBlock)
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        engine.prepare (sampleRate, 1024, maxBlock);
        engine.setClient (this);
        binHz = engine.getBinHz();
        numBins = engine.getFftSize() / 2 + 1;
        const int hop = engine.getHopSize();
        hopSec = (double) hop / sampleRate;

        // band centres
        const double top = std::min (kTopHz, 0.45 * sampleRate);
        std::vector<double> centres;
        for (int i = 0;; ++i)
        {
            const double c = kFirstCentreHz * std::pow (2.0, (double) i / 6.0);
            if (c > top * 1.000001)
                break;
            if (centres.empty() || c - centres.back() >= 2.0 * binHz - 1.0e-9)
                centres.push_back (c);
        }
        numBands = (int) centres.size();
        bands.assign ((size_t) numBands, Band {});
        weights.clear();

        binLog2.assign ((size_t) numBins, 0.0);
        for (int k = 1; k < numBins; ++k)
            binLog2[(size_t) k] = std::log2 ((double) k * binHz);
        binLog2[0] = -100.0;

        for (int b = 0; b < numBands; ++b)
        {
            auto& band = bands[(size_t) b];
            band.hz = centres[(size_t) b];
            band.log2Hz = std::log2 (band.hz);
            const double up = b + 1 < numBands ? std::log2 (centres[(size_t) b + 1]) - band.log2Hz
                                               : (b > 0 ? band.log2Hz - std::log2 (centres[(size_t) b - 1]) : 1.0 / 6.0);
            const double down = b > 0 ? band.log2Hz - std::log2 (centres[(size_t) b - 1]) : up;
            const double lo = band.log2Hz - down, hi = band.log2Hz + up;
            int first = -1, last = -1;
            for (int k = 1; k < numBins; ++k)
                if (binLog2[(size_t) k] > lo && binLog2[(size_t) k] < hi)
                {
                    if (first < 0)
                        first = k;
                    last = k;
                }
            band.firstBin = std::max (first, 1);
            band.numBins = first < 0 ? 0 : last - first + 1;
            band.weightOffset = (int) weights.size();
            double oct = 0.0;
            for (int j = 0; j < band.numBins; ++j)
            {
                const int k = band.firstBin + j;
                const double x = binLog2[(size_t) k];
                const double w = x <= band.log2Hz ? 1.0 - (band.log2Hz - x) / down : 1.0 - (x - band.log2Hz) / up;
                const double wc = std::clamp (w, 0.0, 1.0);
                weights.push_back ((float) wc);
                oct += wc * binHz / ((double) k * binHz * std::log (2.0));
            }
            band.octWidth = std::max (oct, 1.0e-6);
            band.kPow = std::pow (10.0, (double) kWeightingResponseDb (band.hz, sampleRate) / 10.0);
        }

        // Gaussian smoothing over log2 f, sigma 1/6 octave, truncated at 3 sigma, octave-width weighted
        const double sigma = 1.0 / 6.0;
        smoothK.assign ((size_t) numBands * (size_t) numBands, 0.0);
        for (int b = 0; b < numBands; ++b)
        {
            double sum = 0.0;
            for (int j = 0; j < numBands; ++j)
            {
                const double d = bands[(size_t) j].log2Hz - bands[(size_t) b].log2Hz;
                if (std::abs (d) > 3.0 * sigma + 1.0e-9)
                    continue;
                const double w = std::exp (-0.5 * d * d / (sigma * sigma)) * bands[(size_t) j].octWidth;
                smoothK[(size_t) b * (size_t) numBands + (size_t) j] = w;
                sum += w;
            }
            for (int j = 0; j < numBands; ++j)
                smoothK[(size_t) b * (size_t) numBands + (size_t) j] /= sum;
        }

        // band -> bin interpolation
        binBand.assign ((size_t) numBins, 0);
        binFrac.assign ((size_t) numBins, 0.0);
        for (int k = 0; k < numBins; ++k)
            locate (binLog2[(size_t) k], binBand[(size_t) k], binFrac[(size_t) k]);
        for (int i = 0; i < kDefinitionDisplayPoints; ++i)
        {
            const double hz = definitionDisplayHz (i);
            locate (std::log2 (hz), dispBand[i], dispFrac[i]);
            dispBinPos[i] = std::min (hz / binHz, (double) (numBins - 1));
        }

        bandPower.assign ((size_t) numBands, 0.0);
        for (auto& lane : lanes)
            lane.allocate (numBands, numBins);

        glideCoeff = std::exp (-hopSec / (kGlideMs * 0.001));
        amountStep = hopSec / (kAmountRampMs * 0.001);
        soloStep = hopSec / (kSoloMs * 0.001);
        prepared = true;
        reset();
    }

    // value at log2 f between band centres b and b + 1 (flat beyond the ends)
    void locate (double x, int& b, double& frac) const noexcept
    {
        if (numBands < 2 || x <= bands[0].log2Hz)
        {
            b = 0;
            frac = 0.0;
            return;
        }
        if (x >= bands[(size_t) numBands - 1].log2Hz)
        {
            b = numBands - 2;
            frac = 1.0;
            return;
        }
        int i = 0;
        while (i + 2 < numBands && bands[(size_t) i + 1].log2Hz <= x)
            ++i;
        b = i;
        frac = (x - bands[(size_t) i].log2Hz) / (bands[(size_t) i + 1].log2Hz - bands[(size_t) i].log2Hz);
    }

    template <typename V>
    double interp (const V& v, int b, double frac) const noexcept
    {
        if (numBands < 2)
            return numBands == 1 ? (double) v[0] : 0.0;
        return (double) v[(size_t) b] + frac * ((double) v[(size_t) b + 1] - (double) v[(size_t) b]);
    }

    void reset() noexcept
    {
        engine.reset();
        for (auto& lane : lanes)
            lane.clear();
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            for (int i = 0; i < kDefinitionDisplayPoints; ++i)
            {
                meters.gainDb[s][i].store (0.0f);
                meters.levelDb[s][i].store (kLevelFloorDb);
            }
            meters.maxBoostDb[s].store (0.0f);
            meters.maxCutDb[s].store (0.0f);
        }
    }

    bool computeGains (SpectralEngine::Frame& f) noexcept override
    {
        if (numBands < 2)
            return true;
        if (f.linked)
            return processLane (0, f.power[0], f.power[1], f.gain[0], f.silent);
        const bool u0 = processLane (0, f.power[0], nullptr, f.gain[0], f.silent);
        const bool u1 = processLane (1, f.power[1], nullptr, f.gain[1], f.silent);
        return u0 && u1;
    }

    // One lane: band detector -> curve -> bin gains (written only when not unity). Returns true for "all unity".
    bool processLane (int l, const float* p0, const float* p1, float* gainOut, bool silent) noexcept
    {
        auto& lane = lanes[l];
        const auto& sp = params.sets[l];
        const bool stereo = p1 != nullptr;

        // controls
        const double amountTarget = std::clamp ((double) sp.amountPercent, 0.0, 100.0) * 0.01;
        const double lowTarget = std::log2 (std::clamp ((double) sp.lowHz, 300.0, 20000.0));
        const double highTarget = std::log2 (std::clamp ((double) sp.highHz, 300.0, 20000.0));
        const double tiltTarget = std::clamp ((double) sp.tiltDbPerOct, -6.0, 6.0);
        const double soloTarget = soloRegion ? 1.0 : 0.0;
        if (lane.fresh)
        {
            lane.amount = amountTarget;
            lane.logLow = lowTarget;
            lane.logHigh = highTarget;
            lane.tilt = tiltTarget;
            lane.solo = soloTarget;
            lane.fresh = false;
        }
        else
        {
            lane.amount = lane.amount < amountTarget ? std::min (amountTarget, lane.amount + amountStep)
                                                     : std::max (amountTarget, lane.amount - amountStep);
            lane.solo = lane.solo < soloTarget ? std::min (soloTarget, lane.solo + soloStep)
                                               : std::max (soloTarget, lane.solo - soloStep);
            lane.logLow = lowTarget + glideCoeff * (lane.logLow - lowTarget);
            lane.logHigh = highTarget + glideCoeff * (lane.logHigh - highTarget);
            lane.tilt = tiltTarget + glideCoeff * (lane.tilt - tiltTarget);
            if (std::abs (lane.logLow - lowTarget) < 1.0e-7) lane.logLow = lowTarget;
            if (std::abs (lane.logHigh - highTarget) < 1.0e-7) lane.logHigh = highTarget;
            if (std::abs (lane.tilt - tiltTarget) < 1.0e-7) lane.tilt = tiltTarget;
        }
        const double A = lane.amount;
        const double lo = std::max (lane.logLow, std::log2 (300.0)), hi = lane.logHigh;

        // 1. detector (10 ms; never slower than the attack, so 5 ms attacks land within 2 hops)
        const double det = std::exp (-hopSec / (std::clamp ((double) sp.attackMs, 1.0, kDetectorMs) * 0.001));
        const double stereoOffsetDb = stereo ? -3.010299956639812 : 0.0;
        double maxL = -1.0e9;
        for (int b = 0; b < numBands; ++b)
        {
            const auto& band = bands[(size_t) b];
            auto& ps = lane.power[(size_t) b];
            if (! silent)
            {
                const float* w = weights.data() + band.weightOffset;
                double p = 0.0;
                if (stereo)
                    for (int j = 0; j < band.numBins; ++j)
                        p += (double) w[j] * ((double) p0[band.firstBin + j] + (double) p1[band.firstBin + j]);
                else
                    for (int j = 0; j < band.numBins; ++j)
                        p += (double) w[j] * (double) p0[band.firstBin + j];
                ps = p + det * (ps - p);
                if (ps < 1.0e-30)
                    ps = 0.0;
            }
            const double L = 10.0 * std::log10 (ps / band.octWidth + 1.0e-20) + stereoOffsetDb;
            lane.levelDb[(size_t) b] = L;
            bandPower[(size_t) b] = ps;
            maxL = std::max (maxL, L);
            lane.mask[(size_t) b] = regionMask (band.log2Hz, lo, hi);
        }
        const double gate = std::max (kGateAbsDb, maxL - kGateRelDb);

        // 2. offset over the region (gated bands excluded)
        double num = 0.0, den = 0.0;
        for (int b = 0; b < numBands; ++b)
        {
            const double m = lane.mask[(size_t) b];
            if (m <= 0.0 || lane.levelDb[(size_t) b] < gate)
                continue;
            const auto& band = bands[(size_t) b];
            const double line = lane.tilt * (band.log2Hz - 9.965784284662087);   // log2 (f / 1 kHz)
            const double w = m * band.octWidth;
            num += w * (lane.levelDb[(size_t) b] - line);
            den += w;
        }

        // 3. desired gains (unscaled), gated bands hold
        const double lim = definitionMaxGainDb (A);
        const double rawLim = A > 0.0 ? lim / A : kMaxDb;
        if (den > 0.0)
        {
            const double o = num / den;
            for (int b = 0; b < numBands; ++b)
            {
                if (lane.levelDb[(size_t) b] < gate)
                    continue;
                const double line = lane.tilt * (bands[(size_t) b].log2Hz - 9.965784284662087);
                const double D = line + o - lane.levelDb[(size_t) b];
                lane.raw[(size_t) b] = std::clamp (0.6 * D, -rawLim, rawLim);
            }
        }

        // smoothing over frequency, Amount
        if (A <= 0.0)
        {
            std::fill (lane.target.begin(), lane.target.end(), 0.0);
            lane.comp = 0.0;
        }
        else
        {
            for (int b = 0; b < numBands; ++b)
            {
                const double* k = smoothK.data() + (size_t) b * (size_t) numBands;
                double s = 0.0;
                for (int j = 0; j < numBands; ++j)
                    s += k[j] * lane.raw[(size_t) j];
                lane.smooth[(size_t) b] = A * s;
            }

            // 4. loudness neutral: one common shift c, K-weighted power change over the region = 0
            double total = 0.0;
            for (int b = 0; b < numBands; ++b)
                total += bandPower[(size_t) b] * bands[(size_t) b].kPow * lane.mask[(size_t) b];
            const double compLim = kMaxCompDb * A;
            double c = std::clamp (lane.comp, -compLim, compLim);
            if (total > 1.0e-30)
            {
                const double inv = 1.0 / total;
                for (int it = 0; it < 8; ++it)
                {
                    double fv = 0.0, fp = 0.0;
                    for (int b = 0; b < numBands; ++b)
                    {
                        const double m = lane.mask[(size_t) b];
                        if (m <= 0.0)
                            continue;
                        const double W = bandPower[(size_t) b] * bands[(size_t) b].kPow * inv;
                        const double v = lane.smooth[(size_t) b] - c;
                        const bool clamped = v > lim || v < -lim;
                        const double e = std::exp (m * std::clamp (v, -lim, lim) * (kLn10 / 10.0));
                        fv += W * (e - 1.0);
                        if (! clamped)
                            fp += W * e * m * (kLn10 / 10.0);
                    }
                    if (fp < 1.0e-12)
                        break;
                    const double dc = fv / fp;
                    c = std::clamp (c + dc, -compLim, compLim);
                    if (std::abs (dc) < 1.0e-5)
                        break;
                }
            }
            else
            {
                c = 0.0;
            }
            lane.comp = c;
            for (int b = 0; b < numBands; ++b)
                lane.target[(size_t) b] = std::clamp (lane.smooth[(size_t) b] - c, -lim, lim);
        }

        // 6. ballistics
        const double att = std::exp (-hopSec / (std::max ((double) sp.attackMs - kAttackLeadMs, 0.5) * 0.001));
        const double rel = std::exp (-hopSec / std::max (((double) sp.releaseMs - kReleaseLeadMs) * 0.001, 2.0 * hopSec));
        bool anyGain = false;
        for (int b = 0; b < numBands; ++b)
        {
            const double t = lane.target[(size_t) b];
            const double g = lane.gainDb[(size_t) b];
            const bool away = std::abs (t) > std::abs (g) && (g == 0.0 || (t > 0.0) == (g > 0.0));
            double n = t + (away ? att : rel) * (g - t);
            if (n > g && n > 0.0)
                n = std::min (n, std::max (g, 0.0) + kBoostSlewDb);
            if (std::abs (n - t) < kSnapDb)
                n = t;
            lane.gainDb[(size_t) b] = n;
            anyGain = anyGain || n != 0.0;
        }

        // 7. bin curve
        const bool soloing = lane.solo > 0.0;
        bool unity = ! soloing;
        float maxBoost = 0.0f, maxCut = 0.0f;
        if (anyGain)
        {
            for (int k = 0; k < numBins; ++k)
            {
                const double m = regionMask (binLog2[(size_t) k], lo, hi);
                lane.binDb[(size_t) k] = m > 0.0 ? (float) (m * interp (lane.gainDb, binBand[(size_t) k], binFrac[(size_t) k]))
                                                 : 0.0f;
            }
            // slope limit (magnitude only, toward 0): bins outside the region stay exactly 0
            const float slope = (float) kBinSlopeDb;
            for (int k = 1; k < numBins; ++k)
            {
                const float v = lane.binDb[(size_t) k], cap = std::abs (lane.binDb[(size_t) k - 1]) + slope;
                if (std::abs (v) > cap)
                    lane.binDb[(size_t) k] = v > 0.0f ? cap : -cap;
            }
            for (int k = numBins - 2; k >= 0; --k)
            {
                const float v = lane.binDb[(size_t) k], cap = std::abs (lane.binDb[(size_t) k + 1]) + slope;
                if (std::abs (v) > cap)
                    lane.binDb[(size_t) k] = v > 0.0f ? cap : -cap;
            }
            for (int k = 0; k < numBins; ++k)
            {
                const float v = lane.binDb[(size_t) k];
                maxBoost = std::max (maxBoost, v);
                maxCut = std::min (maxCut, v);
                unity = unity && v == 0.0f;
            }
        }
        else
        {
            std::fill (lane.binDb.begin(), lane.binDb.end(), 0.0f);
        }

        if (! unity && ! silent)
        {
            const double s = lane.solo;
            for (int k = 0; k < numBins; ++k)
            {
                const float v = lane.binDb[(size_t) k];
                double g = v == 0.0f ? 1.0 : std::exp ((double) v * (kLn10 / 20.0));
                if (soloing)
                    g = (1.0 - s) * g + s * regionMask (binLog2[(size_t) k], lo, hi);
                gainOut[k] = (float) g;
            }
        }

        publishMeters (l, stereo, maxBoost, maxCut);
        return silent || unity;
    }

    void publishMeters (int l, bool stereo, float maxBoost, float maxCut) noexcept
    {
        const auto& lane = lanes[l];
        for (int i = 0; i < kDefinitionDisplayPoints; ++i)
        {
            const double pos = dispBinPos[i];
            const int k0 = std::min ((int) pos, numBins - 2);
            const double fr = pos - (double) k0;
            const float g = (float) ((double) lane.binDb[(size_t) k0]
                                     + fr * ((double) lane.binDb[(size_t) k0 + 1] - (double) lane.binDb[(size_t) k0]));
            const float lv = (float) std::max ((double) kLevelFloorDb, interp (lane.levelDb, dispBand[i], dispFrac[i]));
            meters.gainDb[l][i].store (g);
            meters.levelDb[l][i].store (lv);
            if (stereo)
            {
                meters.gainDb[1][i].store (g);
                meters.levelDb[1][i].store (lv);
            }
        }
        meters.maxBoostDb[l].store (maxBoost);
        meters.maxCutDb[l].store (maxCut);
        if (stereo)
        {
            meters.maxBoostDb[1].store (maxBoost);
            meters.maxCutDb[1].store (maxCut);
        }
    }
};

Definition::Definition() : impl (std::make_unique<Impl>()) {}
Definition::~Definition() = default;

void Definition::prepare (double sampleRate, int maxBlockSize)
{
    impl->prepare (sampleRate, maxBlockSize);
}

void Definition::reset() noexcept
{
    if (impl->prepared)
        impl->reset();
}

void Definition::setParams (ParamValues v) noexcept
{
    readDefinitionParams (v, impl->params);
    if (getParamSet() == 1)
        impl->params.sets[0] = impl->params.sets[1];
}

void Definition::setMonitor (const ModuleMonitor& m) noexcept
{
    impl->soloRegion = m.soloRegion;
}

void Definition::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    auto& d = *impl;
    if (! d.prepared || numSamples <= 0)
        return;
    ScopedFlushDenormals noDenormals;
    const bool dual = ctx.channelMode == ChannelMode::MidSide || ctx.channelMode == ChannelMode::LeftRight;
    if (dual && ! d.dual)
        d.lanes[1].copyFrom (d.lanes[0]);   // start the second lane from the linked state (no fade-in from zero)
    d.dual = dual;
    d.engine.process (channels, numSamples, ! dual);
}

int Definition::getLatencySamples() const noexcept { return impl->engine.getLatencySamples(); }

const DefinitionMeters& Definition::getMeters() const noexcept { return impl->meters; }
} // namespace ps
