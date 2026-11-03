#include "dsp/ToneBalance.h"
#include "dsp/SpectralEngine.h"
#include "dsp/ToneTargets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__) || defined(__x86_64__)
 #include <xmmintrin.h>
 #define PS_TBAL_HAS_SSE_CSR 1
#endif

// Tone Balance (dsp-tone). Contract: ToneBalance.h. An adaptive EQ on ps::SpectralEngine (base size 2048, hop N / 4,
// latency N): per hop it measures the octave-density band levels, compares them with the target curve that floats with
// the program level and steers a smooth, slew-limited gain curve toward it.
//
// Implementation choices (where the header leaves room, or where the measured behaviour asked for it):
// - Band grid: the nominal 1/12-octave grid 20 Hz * 2^(i / 12) up to min (20 kHz, 0.45 fs); a grid point is kept only
//   when it lies at least 2 FFT bins above the previous kept centre (so ~17 merged bands below 800 Hz, ~73 bands in
//   all at 44.1 / 48 kHz, the same at the higher rates because N scales with fs). Band powers use triangular weights in
//   log f between neighbouring centres (a partition of unity; the end bands get a mirrored half triangle); the DC bin
//   is excluded. The octave width of a band is the weighted sum of its bins' exact octave spans, so pink noise
//   measures flat in every band (also the merged low ones).
// - Shape / Boost reading: each band reads the larger of its own power and the power seen through skirts of
//   12 dB / octave on both sides (a power-domain smoothing over log f, kSkirtDbPerOct) before the detector. Measured (Stab-Empty.wav, top octaves 24 .. 60 dB down
//   above a raised-cosine edge at 4 .. 5.6 kHz): the reference's lift near the edge does not grow with the depth
//   (+3.7 .. +4.2 dB at 6.3 kHz at Amount 100) and reaches its cap ~2/3 octave above the edge; narrow bands read the
//   hole at its full depth and lift it from the edge on. Smooth spectra (pink, brown, white) read the same either way.
//   Taking the larger of the two keeps peaks and dense music on the narrow reading (the skirted reading alone lost
//   0.2 .. 0.4 dB of match on rock and test slice S; with the max they gain 0.5 .. 0.9).
//   The bottom band (20 Hz: one FFT bin, which cannot resolve that octave) reads the bands above it through a
//   2 dB / octave skirt (kBottomSkirtDbPerOct), so a sub under a loud bass reads as the bass's continuation instead of
//   a hole. Measured (Round20-Test.wav, the reference at Rock / Amount 25): pink with the sub 24 dB down gets its sub
//   lifted +0.2 .. +0.9 dB, the same with 100-250 Hz +12 dB gets none (-0.3 .. +0.1), nor does slice 12's spectrum
//   (sub ~25 dB under its bass). Ours at 20 Hz: +1.2 -> +0.3 and +1.5 -> +0.3 (40-63 Hz still 0.4 .. 0.6 dB high);
//   17-slice sweep +0.9 dB summed, no slice below -0.05. Every band below 100 Hz on 2 .. 4 dB / octave lost 08 / 01.
//   The gate and the Shape / Boost onsets use the skirted levels; the Cut detectors (F, the 1.5 s reference) and the
//   Cut onsets keep the narrow bands (resonances).
// - Shape / Boost detector S: a one-pole on band power (tauS), in dB afterwards (measured: the reference's Shape on
//   music matches a power average better than a dB average, which under-reads peaky bands). A rising band attacks with
//   min (0.3 s, tauS): on sparse, dynamic material the reference reads a band at its recent loud moments (an acoustic
//   slice's highs, 8 dB of level spread, got ~+1.7 dB from the reference where a symmetric average gave +5); on steady
//   noise it reads the narrowest bands ~0.4 dB high at Speed 0 (less at faster Speeds). Cut: F (tauC) and the 1.5 s
//   reference are one-poles on band power too (peak-leaning in dB, which suits a cut detector).
//   Every detector uses a growing memory after reset (coefficient min (a, n / (n + 1)) for the n-th measured frame), so
//   the first estimate is an average, not a single noisy frame; the correction also fades in over 300 ms after reset.
// - All three detectors (Shape / Boost S at tauS, Cut F at tauC, Cut reference S at 1.5 s) always run, so a mode change
//   only glides the curve (80 ms one-pole, at least 300 ms and until it has converged) and never restarts detection.
// - Shape corrects 80 % of the deviation at Amount 100 (the reference's noise renders: pink vs brown). Its level anchor
//   is POWER-neutral (unweighted): the reference keeps the output power within ~0.2 dB of the input on pink noise with
//   band level steps at every Amount, while its K-weighted loudness drops 0.4 .. 2.8 LU (cutting highs costs more
//   loudness than power); a K-weighted (loudness-neutral) anchor read 1.1 .. 1.8 dB high in every band. (An earlier
//   energy-weighted offset anchor without this solve lifted quiet acoustic music by 2.5 LU.)
// - Power neutrality (Shape) is computed on the amount-scaled, region-weighted curve, and its level shift x is applied
//   in proportion to the region weight w (Newton steps solve sum Q 10^(final (C - x w) / 10) = sum Q, where final ()
//   applies the limits below). So the curve is power neutral at every Amount (not only at 100), the limits are taken
//   into account, and a region at 0 % gets no correction at all, the shift included. Q is the power of every bin,
//   averaged over 1.5 s (kPowerAverageSec; measured: with a 50 ms average our level followed every frame's spectrum
//   and its output power sat 0.1 .. 0.5 dB above the reference on music at the default Speed, Tone Balance alone;
//   with 1.5 s it is within ~0.2 dB on all seven slices measured, and the full chains match ~0.5 dB better each). The
//   solve interpolates the final dB curve at each bin before converting to power gain, exactly as synthesis does;
//   averaging endpoint power gains would overestimate energy.
//   A bounded Newton solve keeps a sign bracket and bisects unsafe steps at the limiter / presence kinks.
//   The limits scale with Amount: cuts -12 x Amount / 100 dB, boosts +9 x (Amount / 100)^0.84 dB
//   (toneBalanceMaxBoostDb; measured: the reference's plateau on white noise is +9.0 dB at Amount 100, +5.1 at 50 and
//   +2.7 at 25, where a linear law gives 4.5 / 2.25).
// - Shape / Boost: the unscaled correction is limited to [-12, cap / amount] BEFORE the frequency smoothing (measured:
//   a deep hole does not spill its deviation into the neighbouring octave; the reference's 4 kHz stays put while the
//   top octaves go from -24 to -60 dB). The power-neutral shift is solved WITHOUT the boost cap, which applies
//   afterwards: on white noise, whose bass boost is capped, the reference keeps the rest of the curve 1.5 .. 3 dB lower
//   than a cap-aware solve would (the output loses power when boosts are capped). Boosts below ~40 Hz are rolled off
//   before that limit (kBoostRollHz): the sub of pink noise gets less than the octave above it, white noise stays at
//   the cap.
// - Boosts are scaled by a smoothed presence of the band (0 while it is gated): a gated band holds a level that may be
//   stale (e.g. the leakage of a tone's onset). There is no "empty band" rule beyond the gate: the reference lifts
//   bands 40-60 dB under the loudest one (dark lo-fi / ambient slices with a bright target got +4..6 dB more air from it
//   than from ours, which faded boosts out 34-40 dB under the loudest band).
// - Tame Transients: an onset needs L - S > 6 dB (Cut: L - F) in the band and in a neighbour (a lone narrow band
//   jumping 6 dB is far more often a noise peak than a hit); from an onset the detector attacks at once for 3 hops
//   (it takes L whenever L is above it, so a hit rising over two frames is caught completely) and, in Shape, boosts
//   are frozen for those 3 hops.
// - Ballistics run separately on the boost part (rise <= 1 dB / hop, fall 50 ms x (1 kHz / f)^1.5, at most the
//   release) and the cut part (attack at once, release one-pole); the applied curve is their sum. The frequency law of
//   the boost's fall is measured: the reference pulls a treble boost back within a frame of a loud strum but keeps a
//   bass boost through kicks (a fast fall at every frequency lost 1-2 dB of match on rock and on test slice S).
//   Values within 1e-5 dB of a zero target snap to exactly 0, so after Amount returns to 0 the module nulls
//   bit-exactly again (the engine then skips the synthesis).
// - Continuous controls are smoothed per hop (Amount: linear 50 ms ramp; the others a 50 ms one-pole), so automation
//   moves the target curve continuously; the 75 %-overlap synthesis crossfades every hop's curve.
// - The Flat target is exactly 0 dB at all frequencies (ToneTargets.cpp).
// - levelDb meters: Shape / Boost show S, Cut shows F (the measurement the cut compares with the target).
namespace ps
{
double toneBalanceMaxBoostDb (double amount) noexcept
{
    return amount > 0.0 ? 9.0 * std::pow (std::min (amount, 1.0), 0.84) : 0.0;
}

namespace
{
constexpr int kBaseFftSize = 2048;
constexpr int kNumTargets = (int) ToneTarget::Count;
constexpr double kDbToLn = 0.11512925464970228;     // ln (10) / 20
constexpr double kPowDbToLn = 0.23025850929940457;  // ln (10) / 10
constexpr double kMaxCutDb = 12.0;
// Boosts in the bottom octave are weighted 1 / (1 + (29.4 Hz / f)^3.75) before the cap (measured: on pink noise the
// reference's boost at Amount 100 is +1.5 / +2.3 / +3.0 dB at 31 / 50 / 79 Hz where a flat weight gives ~+2.6 each;
// white noise, whose bass boost is far above the cap, stays at the cap down to 31 Hz).
constexpr double kBoostRollHz = 29.4;
constexpr double kBoostRollOrder = 3.75;
constexpr double kShapeFraction = 0.8;   // Shape corrects 80 % of the deviation at Amount 100 (measured: the reference)
// ... of the target's shape; the measured part of the deviation (the music's shape relative to pink) counts 0.875 x:
// the reference follows a darker or brighter spectrum ~0.87 x as far as a plain 0.8 law (pink with the highs -6 / -12
// / -18 dB: 2.5 / 5.5 / 8.3 dB of change, the plain law 2.8 / 6.3 / 9.7; a +6 dB high step 68 % vs 78 %). Pink itself
// is unchanged (its measured shape is flat).
constexpr double kMeasuredShape = 0.875;
constexpr double kGateAbsDb = -80.0, kGateRelDb = 60.0;
constexpr double kOnsetDb = 6.0;
constexpr double kSnapDb = 1.0e-5;
constexpr double kParamSmoothSec = 0.05;
constexpr double kWarmSec = 0.3;
constexpr double kGlideTauSec = 0.08, kGlideMinSec = 0.3;
constexpr double kCutRefTauSec = 1.5;
// Shape / Boost detector attack: a band whose level rises is read within ~0.3 s (min with tauS); the fall keeps tauS.
// Measured: the reference drops a band's correction within ~0.3 s of the band getting louder at every Speed, and
// gives it back at the Speed's pace (test-track slices and level steps).
constexpr double kAttackSec = 0.3;
// A boost shrinks with 50 ms x (1 kHz / f)^1.5 (clamped to 1/16 .. 16 x, at most the release max (2 hops, tauS / 2)):
// the reference pulls a high boost back at once when the band gets loud (its gain on an acoustic slice's highs falls
// 5 dB within ~0.3 s of a strum) but keeps a bass boost through kicks.
constexpr double kBoostDropSec = 0.05, kBoostDropExp = 1.5;
constexpr double kPowerAverageSec = 1.5;   // bin power average of the power-neutral solve
constexpr double kOffsetLoHz = 100.0, kOffsetHiHz = 5600.0;
constexpr double kSkirtDbPerOct = 12.0;   // Shape / Boost band reading: power skirts on both sides
constexpr double kBottomSkirtDbPerOct = 2.0;   // ... the bottom band's skirt toward the bands above it

#if PS_TBAL_HAS_SSE_CSR
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

double powToDb (double p) noexcept { return 10.0 * std::log10 (p + 1.0e-30); }

double raisedCosineStep (double x) noexcept   // 0 for x <= 0, 1 for x >= 1
{
    const double t = std::clamp (x, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos (3.14159265358979323846 * t);
}

// 0.5 dB soft dead band: |D| < 1: D^2 / 2, else |D| - 0.5 (continuous in value and slope)
double deadBand (double d) noexcept
{
    const double a = std::abs (d);
    const double v = a < 1.0 ? 0.5 * a * a : a - 0.5;
    return d < 0.0 ? -v : v;
}

// soft knee of width w centred on 0: 0 below -w/2, x above w/2, quadratic in between
double softKnee (double x, double w) noexcept
{
    const double h = 0.5 * w;
    if (x <= -h)
        return 0.0;
    if (x >= h)
        return x;
    return (x + h) * (x + h) / (2.0 * w);
}
} // namespace

double toneBalanceDisplayHz (int index) noexcept
{
    const int i = std::clamp (index, 0, kToneDisplayPoints - 1);
    return 20.0 * std::pow (1000.0, (double) i / (double) (kToneDisplayPoints - 1));
}

struct ToneBalance::Impl final : SpectralEngine::Client
{
    SpectralEngine engine;
    ToneBalanceMeters meters;
    ToneBalanceParams params;
    bool prepared = false;
    int latency = kBaseFftSize;
    double sampleRate = 48000.0, hopSec = 512.0 / 48000.0;

    // band grid
    int nb = 0;
    std::vector<double> bandLog2;
    std::vector<float> bandHz, bandWidthOct, bandRegion;   // bandRegion: 3 per band (low, mid, high basis)
    std::vector<float> bandBoostDrop;                             // boost fall time per band (kBoostDropSec law), s
    std::vector<float> bandBoostRoll;                             // bottom-octave weight of boosts (before the cap)
    std::vector<uint8_t> bandInOffset;                            // 100 Hz .. 5.6 kHz
    std::vector<float> skirtW;                                    // [b * nb + j]: weight of band j's density in band b
    std::vector<float> skirtPow;                                  // scratch: skirted band powers of one lane
    // measurement: each bin contributes to up to two bands
    std::vector<int> binBandA, binBandB;
    std::vector<float> binWA, binWB;
    // band -> bin interpolation
    std::vector<int> binLo;
    std::vector<float> binT;
    int dispLo[kToneDisplayPoints] {};
    float dispT[kToneDisplayPoints] {};
    // targets [target * nb + band]
    std::vector<float> targetDb, targetTol;

    struct Lane
    {
        // sPow: power pre-average of the Shape / Boost detector, sLev: its dB one-pole (S); slowPow: the Cut
        // reference (1.5 s), fastPow: F (tauC); sDb / refDb: per hop, see updateLane
        std::vector<float> power, sPow, sLev, slowPow, fastPow, sDb, refDb, c, tmp, gBoost, gCut, out;
        // power per bin and its 1.5 s average (the power-neutral solve's reference). Keep bins separate: interpolation
        // in dB does not commute with conversion to power gain, especially on sparse spectra in the merged low bands.
        std::vector<float> loudPow, loudAvg;
        std::vector<double> shiftGain, shiftSlope;   // per-band scratch for the loudness solve
        std::vector<float> presence;   // 0..1: smoothed "this band is measured" (0 while gated); scales boosts
        int loudCount = 0;
        std::vector<uint16_t> count;
        std::vector<uint8_t> gated, freeze, onset;
        std::vector<float> kernel;   // nb x nb row-normalised Gaussian weights
        std::vector<int> kernelLo, kernelHi;
        double kernelSigma = -1.0;
        double offset = 0.0, sMean = 0.0;   // o, and the weighted mean of S over the offset bands
        bool offsetValid = false;
        double maxBoost = 0.0, maxCut = 0.0;   // of the applied curve (meters)
        // smoothed controls
        bool controlsInit = false;
        double amount = 0.0, speed = 50.0, smooth = 50.0, cutSpeed = 50.0, sens = 50.0, low = 100.0, mid = 100.0,
               high = 100.0;
        double warm = 0.0;
    };
    Lane lanes[kNumChannelSets];

    // mode / target glide
    bool haveLast = false;
    ToneBalanceMode lastMode = ToneBalanceMode::Shape;
    ToneTarget lastTarget = ToneTarget::Balanced;
    int glideHops = 0;
    bool glideActive = false;

    void buildGrid (double binHz, int numBins)
    {
        const double fmax = std::min (20000.0, 0.45 * sampleRate);
        std::vector<double> centres;
        for (int i = 0;; ++i)
        {
            const double f = 20.0 * std::pow (2.0, (double) i / 12.0);
            if (f > fmax * (1.0 + 1.0e-9))
                break;
            if (centres.empty() || f - centres.back() >= 2.0 * binHz - 1.0e-9)
                centres.push_back (f);
        }
        nb = (int) centres.size();
        bandLog2.assign ((size_t) nb, 0.0);
        bandHz.assign ((size_t) nb, 0.0f);
        bandWidthOct.assign ((size_t) nb, 0.0f);
        bandRegion.assign ((size_t) nb * 3, 0.0f);
        bandInOffset.assign ((size_t) nb, 0);
        bandBoostDrop.assign ((size_t) nb, 0.0f);
        bandBoostRoll.assign ((size_t) nb, 1.0f);
        for (int b = 0; b < nb; ++b)
        {
            const double f = centres[(size_t) b];
            bandLog2[(size_t) b] = std::log2 (f);
            bandHz[(size_t) b] = (float) f;
            bandBoostRoll[(size_t) b] = (float) (1.0 / (1.0 + std::pow (kBoostRollHz / f, kBoostRollOrder)));
            // region basis: raised-cosine crossfades 1/3 octave wide centred on 100 Hz and 5.6 kHz
            const double sLow = raisedCosineStep ((std::log2 (f / 100.0) + 1.0 / 6.0) * 3.0);
            const double sHigh = raisedCosineStep ((std::log2 (f / 5600.0) + 1.0 / 6.0) * 3.0);
            bandRegion[(size_t) b * 3 + 0] = (float) (1.0 - sLow);
            bandRegion[(size_t) b * 3 + 1] = (float) (sLow * (1.0 - sHigh));
            bandRegion[(size_t) b * 3 + 2] = (float) sHigh;
            bandInOffset[(size_t) b] = (uint8_t) (f >= kOffsetLoHz && f <= kOffsetHiHz ? 1 : 0);
            bandBoostDrop[(size_t) b] = (float) (kBoostDropSec * std::clamp (std::pow (1000.0 / f, kBoostDropExp), 1.0 / 16.0, 16.0));
        }

        // measurement weights (triangles in log f, mirrored virtual neighbours at both ends)
        binBandA.assign ((size_t) numBins, -1);
        binBandB.assign ((size_t) numBins, -1);
        binWA.assign ((size_t) numBins, 0.0f);
        binWB.assign ((size_t) numBins, 0.0f);
        binLo.assign ((size_t) numBins, 0);
        binT.assign ((size_t) numBins, 0.0f);
        auto centreAt = [this] (int b) noexcept
        {
            if (b < 0)
                return 2.0 * bandLog2[0] - (nb > 1 ? bandLog2[1] : bandLog2[0] + 1.0);
            if (b >= nb)
                return 2.0 * bandLog2[(size_t) nb - 1] - (nb > 1 ? bandLog2[(size_t) nb - 2] : bandLog2[0] - 1.0);
            return bandLog2[(size_t) b];
        };
        std::vector<double> width ((size_t) nb, 0.0);
        for (int k = 1; k < numBins; ++k)
        {
            const double lf = std::log2 ((double) k * binHz);
            const double spanOct = std::log2 (((double) k + 0.5) / ((double) k - 0.5));
            int j = -1;   // lf in [centre (j), centre (j + 1))
            while (j < nb && lf >= centreAt (j + 1))
                ++j;
            if (j >= -1 && j < nb && lf >= centreAt (-1))
            {
                const double c0 = centreAt (j), c1 = centreAt (j + 1);
                const double w1 = std::clamp ((lf - c0) / (c1 - c0), 0.0, 1.0);
                const double w0 = 1.0 - w1;
                if (j >= 0 && w0 > 0.0)
                {
                    binBandA[(size_t) k] = j;
                    binWA[(size_t) k] = (float) w0;
                    width[(size_t) j] += w0 * spanOct;
                }
                if (j + 1 < nb && w1 > 0.0)
                {
                    binBandB[(size_t) k] = j + 1;
                    binWB[(size_t) k] = (float) w1;
                    width[(size_t) j + 1] += w1 * spanOct;
                }
            }

            // gain interpolation, linear in log f between centres
            if (lf <= bandLog2[0] || nb == 1)
            {
                binLo[(size_t) k] = 0;
                binT[(size_t) k] = 0.0f;
            }
            else if (lf >= bandLog2[(size_t) nb - 1])
            {
                binLo[(size_t) k] = std::max (0, nb - 2);
                binT[(size_t) k] = 1.0f;
            }
            else
            {
                int lo = 0;
                while (lo + 1 < nb - 1 && lf >= bandLog2[(size_t) lo + 1])
                    ++lo;
                binLo[(size_t) k] = lo;
                binT[(size_t) k] = (float) ((lf - bandLog2[(size_t) lo]) / (bandLog2[(size_t) lo + 1] - bandLog2[(size_t) lo]));
            }
        }
        binLo[0] = 0;
        binT[0] = 0.0f;
        if (nb == 1)
            for (auto& t : binT)
                t = 0.0f;
        for (int b = 0; b < nb; ++b)
            bandWidthOct[(size_t) b] = (float) std::max (width[(size_t) b], 1.0e-3);
        // skirted reading: power_b = width_b sum_j w_j d_j / sum_j w_j (d: density), w_j = width_j 10^(-skirt |dlog2 f| / 10);
        // the detector takes the larger of this and the band's own power
        skirtW.assign ((size_t) nb * (size_t) nb, 0.0f);
        skirtPow.assign ((size_t) nb, 0.0f);
        for (int b = 0; b < nb; ++b)
        {
            float* row = skirtW.data() + (size_t) b * (size_t) nb;
            double sum = 0.0;
            for (int j = 0; j < nb; ++j)
            {
                const double d = std::abs (bandLog2[(size_t) j] - bandLog2[(size_t) b]);
                const double skirt = b == 0 && j > 0 ? kBottomSkirtDbPerOct : kSkirtDbPerOct;
                const double w = (double) bandWidthOct[(size_t) j] * std::pow (10.0, -skirt * d / 10.0);
                row[j] = (float) w;
                sum += w;
            }
            for (int j = 0; j < nb; ++j)
                row[j] = (float) ((double) row[j] * (double) bandWidthOct[(size_t) b] / (sum * (double) bandWidthOct[(size_t) j]));
        }

        for (int i = 0; i < kToneDisplayPoints; ++i)
        {
            const double lf = std::log2 (toneBalanceDisplayHz (i));
            if (nb == 1 || lf <= bandLog2[0])
            {
                dispLo[i] = 0;
                dispT[i] = 0.0f;
            }
            else if (lf >= bandLog2[(size_t) nb - 1])
            {
                dispLo[i] = nb - 2;
                dispT[i] = 1.0f;
            }
            else
            {
                int lo = 0;
                while (lo + 1 < nb - 1 && lf >= bandLog2[(size_t) lo + 1])
                    ++lo;
                dispLo[i] = lo;
                dispT[i] = (float) ((lf - bandLog2[(size_t) lo]) / (bandLog2[(size_t) lo + 1] - bandLog2[(size_t) lo]));
            }
        }

        targetDb.assign ((size_t) kNumTargets * (size_t) nb, 0.0f);
        targetTol.assign ((size_t) kNumTargets * (size_t) nb, 0.0f);
        for (int t = 0; t < kNumTargets; ++t)
            for (int b = 0; b < nb; ++b)
            {
                double tol = 0.0;
                targetDb[(size_t) (t * nb + b)] = (float) toneTargetDb ((ToneTarget) t, bandHz[(size_t) b], &tol);
                targetTol[(size_t) (t * nb + b)] = (float) tol;
            }

        for (auto& lane : lanes)
        {
            const auto n = (size_t) nb;
            for (auto* v : { &lane.power, &lane.sPow, &lane.sLev, &lane.slowPow, &lane.fastPow, &lane.sDb, &lane.refDb, &lane.c,
                             &lane.tmp, &lane.gBoost, &lane.gCut, &lane.out })
                v->assign (n, 0.0f);
            lane.count.assign (n, 0);
            lane.loudPow.assign ((size_t) numBins, 0.0f);
            lane.loudAvg.assign ((size_t) numBins, 0.0f);
            lane.shiftGain.assign (n, 0.0);
            lane.shiftSlope.assign (n, 0.0);
            lane.presence.assign (n, 0.0f);
            lane.gated.assign (n, 0);
            lane.freeze.assign (n, 0);
            lane.onset.assign (n, 0);
            lane.kernel.assign (n * n, 0.0f);
            lane.kernelLo.assign (n, 0);
            lane.kernelHi.assign (n, 0);
            lane.kernelSigma = -1.0;
        }
    }

    void resetState() noexcept
    {
        for (auto& lane : lanes)
        {
            for (auto* v : { &lane.power, &lane.sPow, &lane.sLev, &lane.slowPow, &lane.fastPow, &lane.sDb, &lane.refDb, &lane.c,
                             &lane.tmp, &lane.gBoost, &lane.gCut, &lane.out })
                std::fill (v->begin(), v->end(), 0.0f);
            std::fill (lane.count.begin(), lane.count.end(), (uint16_t) 0);
            std::fill (lane.loudPow.begin(), lane.loudPow.end(), 0.0f);
            std::fill (lane.loudAvg.begin(), lane.loudAvg.end(), 0.0f);
            std::fill (lane.presence.begin(), lane.presence.end(), 0.0f);
            lane.loudCount = 0;
            std::fill (lane.gated.begin(), lane.gated.end(), (uint8_t) 0);
            std::fill (lane.freeze.begin(), lane.freeze.end(), (uint8_t) 0);
            std::fill (lane.onset.begin(), lane.onset.end(), (uint8_t) 0);
            lane.offset = lane.sMean = 0.0;
            lane.offsetValid = false;
            lane.controlsInit = false;
            lane.warm = 0.0;
        }
        haveLast = false;
        glideHops = 0;
        glideActive = false;
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            for (int i = 0; i < kToneDisplayPoints; ++i)
            {
                meters.gainDb[s][i].store (0.0f);
                meters.levelDb[s][i].store (-120.0f);
            }
            meters.targetOffsetDb[s].store (0.0f);
            meters.maxBoostDb[s].store (0.0f);
            meters.maxCutDb[s].store (0.0f);
        }
    }

    void updateKernel (Lane& lane, double sigma) noexcept
    {
        if (lane.kernelSigma > 0.0 && std::abs (sigma - lane.kernelSigma) <= 0.005 * lane.kernelSigma)
            return;
        lane.kernelSigma = sigma;
        const double inv = 1.0 / (2.0 * sigma * sigma);
        const double reach = 4.0 * sigma;
        for (int b = 0; b < nb; ++b)
        {
            float* row = lane.kernel.data() + (size_t) b * (size_t) nb;
            int lo = b, hi = b;
            while (lo > 0 && bandLog2[(size_t) b] - bandLog2[(size_t) lo - 1] <= reach)
                --lo;
            while (hi < nb - 1 && bandLog2[(size_t) hi + 1] - bandLog2[(size_t) b] <= reach)
                ++hi;
            double sum = 0.0;
            for (int j = lo; j <= hi; ++j)
            {
                const double d = bandLog2[(size_t) j] - bandLog2[(size_t) b];
                const double w = std::exp (-d * d * inv) * (double) bandWidthOct[(size_t) j];
                row[j] = (float) w;
                sum += w;
            }
            const float norm = (float) (1.0 / sum);
            for (int j = lo; j <= hi; ++j)
                row[j] *= norm;
            lane.kernelLo[(size_t) b] = lo;
            lane.kernelHi[(size_t) b] = hi;
        }
    }

    void smoothControls (Lane& lane, const ToneBalanceSetParams& sp) noexcept
    {
        const double amountTarget = std::clamp ((double) sp.amountPercent, 0.0, 100.0);
        auto clampPct = [] (float v) noexcept { return std::clamp ((double) v, 0.0, 100.0); };
        if (! lane.controlsInit)
        {
            lane.controlsInit = true;
            lane.amount = amountTarget;
            lane.speed = clampPct (sp.speed);
            lane.smooth = clampPct (sp.smoothing);
            lane.cutSpeed = clampPct (sp.cutSpeed);
            lane.sens = clampPct (sp.sensitivity);
            lane.low = clampPct (sp.lowPercent);
            lane.mid = clampPct (sp.midPercent);
            lane.high = clampPct (sp.highPercent);
            return;
        }
        const double step = 100.0 * hopSec / kParamSmoothSec;
        lane.amount = lane.amount < amountTarget ? std::min (amountTarget, lane.amount + step)
                                                 : std::max (amountTarget, lane.amount - step);
        const double a = std::exp (-hopSec / kParamSmoothSec);
        auto glide = [a] (double& v, double target) noexcept
        {
            v = target + (v - target) * a;
            if (std::abs (v - target) < 1.0e-6)
                v = target;
        };
        glide (lane.speed, clampPct (sp.speed));
        glide (lane.smooth, clampPct (sp.smoothing));
        glide (lane.cutSpeed, clampPct (sp.cutSpeed));
        glide (lane.sens, clampPct (sp.sensitivity));
        glide (lane.low, clampPct (sp.lowPercent));
        glide (lane.mid, clampPct (sp.midPercent));
        glide (lane.high, clampPct (sp.highPercent));
    }

    // One hop of one lane. power: band powers already in lane.power. Returns true when the applied curve is all 0.
    bool updateLane (Lane& lane, const ToneBalanceSetParams& sp, ToneBalanceMode mode, int targetIndex,
                     bool glideNow, double aGlide, bool silent) noexcept
    {
        smoothControls (lane, sp);
        const bool cutMode = mode == ToneBalanceMode::Cut;
        const bool tame = sp.tameTransients;

        const double tauS = 4.0 * std::pow (60.0 / 4000.0, lane.speed / 100.0);
        const double tauC = 0.4 * std::pow (5.0 / 400.0, lane.cutSpeed / 100.0);
        const double aS = std::exp (-hopSec / tauS);
        const double aA = std::exp (-hopSec / std::min (kAttackSec, tauS));
        const double aPresence = std::exp (-hopSec / std::clamp (tauS, 0.1, 0.5));
        const double aQ = std::exp (-hopSec / kPowerAverageSec);
        const double aSlow = std::exp (-hopSec / kCutRefTauSec);
        const double aF = std::exp (-hopSec / tauC);
        const double aRel = std::exp (-hopSec / std::max (2.0 * hopSec, cutMode ? tauC : 0.5 * tauS));

        // Silent frames hold detector history, but automation and the gain release still run. Otherwise an
        // Amount change during a pause would resurrect the old correction when the next phrase starts.
        if (! silent)
        {
            // 1. measurement and gate
            double lmax = -1.0e9;
            // the Shape / Boost reading through the skirts (skirtPow); the Cut detectors use lane.power
            for (int b = 0; b < nb; ++b)
            {
                const float* row = skirtW.data() + (size_t) b * (size_t) nb;
                double ps = 0.0;
                for (int j = 0; j < nb; ++j)
                    ps += (double) row[j] * (double) lane.power[(size_t) j];
                ps = std::max (ps, (double) lane.power[(size_t) b]);
                skirtPow[(size_t) b] = (float) ps;
                const double l = powToDb (ps / (double) bandWidthOct[(size_t) b]);
                lane.tmp[(size_t) b] = (float) l;
                lmax = std::max (lmax, l);
            }
            // onsets (Tame Transients): L more than 6 dB above the detector, in this band and in a neighbour (a lone
            // narrow band jumping 6 dB is far more often a noise peak than a hit)
            for (int b = 0; b < nb; ++b)
            {
                const auto i = (size_t) b;
                const double l = cutMode ? powToDb ((double) lane.power[i] / (double) bandWidthOct[i]) : (double) lane.tmp[i];
                const double ref = cutMode ? powToDb ((double) lane.fastPow[i] / (double) bandWidthOct[i])
                                           : (double) lane.sLev[i];
                lane.onset[i] = (uint8_t) (tame && lane.count[i] != 0 && l - ref > kOnsetDb ? 1 : 0);
            }

            bool anyMeasured = false;
            for (int b = 0; b < nb; ++b)
            {
                const auto i = (size_t) b;
                const double l = lane.tmp[i];
                const bool gated = l < kGateAbsDb || l < lmax - kGateRelDb;
                lane.gated[i] = (uint8_t) (gated ? 1 : 0);
                lane.presence[i] = (float) ((gated ? 0.0 : 1.0) + ((double) lane.presence[i] - (gated ? 0.0 : 1.0)) * aPresence);
                if (lane.freeze[i] > 0)
                    --lane.freeze[i];
                if (gated)
                    continue;
                anyMeasured = true;
                const double p = lane.power[i], pS = skirtPow[i];
                const double width = bandWidthOct[i];
                const int n = lane.count[i];
                if (n == 0)
                {
                    lane.slowPow[i] = lane.fastPow[i] = (float) p;
                    lane.sPow[i] = (float) pS;
                    lane.sLev[i] = (float) l;
                }
                else
                {
                    const bool onset = lane.onset[i] != 0 && ((b > 0 && lane.onset[i - 1] != 0)
                                                              || (b + 1 < nb && lane.onset[i + 1] != 0));
                    const double grow = (double) n / (double) (n + 1);
                    const double eS = std::min (aS, grow), eA = std::min (aA, grow), eSlow = std::min (aSlow, grow),
                                 eF = std::min (aF, grow);
                    if (onset)
                        lane.freeze[i] = 3;
                    // Tame Transients: for 3 hops from an onset the detector attacks at once
                    const bool attack = tame && lane.freeze[i] > 0;
                    if (attack && ! cutMode && l > (double) lane.sLev[i])
                    {
                        lane.sPow[i] = (float) std::max (pS, (double) lane.sPow[i]);
                        lane.sLev[i] = (float) l;
                    }
                    else
                    {
                        lane.sPow[i] = (float) (pS + ((double) lane.sPow[i] - pS) * (pS > (double) lane.sPow[i] ? eA : eS));
                        lane.sLev[i] = (float) powToDb ((double) lane.sPow[i] / width);
                    }
                    lane.slowPow[i] = (float) (p + ((double) lane.slowPow[i] - p) * eSlow);
                    if (attack && cutMode && p > (double) lane.fastPow[i])
                        lane.fastPow[i] = (float) p;
                    else
                        lane.fastPow[i] = (float) (p + ((double) lane.fastPow[i] - p) * eF);
                }
                if (n < 65535)
                    lane.count[i] = (uint16_t) (n + 1);
            }
            {
                const double eL = std::min (aQ, (double) lane.loudCount / (double) (lane.loudCount + 1));
                for (size_t k = 0; k < lane.loudPow.size(); ++k)
                {
                    const double q = lane.loudPow[k];
                    lane.loudAvg[k] = (float) (q + ((double) lane.loudAvg[k] - q) * eL);
                }
                lane.loudCount = std::min (lane.loudCount + 1, 1 << 20);
            }
            if (anyMeasured)
                lane.warm = std::min (1.0, lane.warm + hopSec / kWarmSec);
        }
        else
        {
            for (auto& freeze : lane.freeze)
                if (freeze > 0)
                    --freeze;
        }

        // 2. levels in dB (sDb: S, the Shape / Boost detector or the Cut reference; refDb: what the correction compares)
        for (int b = 0; b < nb; ++b)
        {
            const auto i = (size_t) b;
            if (lane.count[i] == 0)
            {
                lane.sDb[i] = -200.0f;
                lane.refDb[i] = -200.0f;
                continue;
            }
            const double width = bandWidthOct[i];
            const double s = cutMode ? powToDb ((double) lane.slowPow[i] / width) : (double) lane.sLev[i];
            lane.sDb[i] = (float) s;
            lane.refDb[i] = cutMode ? (float) powToDb ((double) lane.fastPow[i] / width) : (float) s;
        }

        // 3. target offset o
        const float* tgt = targetDb.data() + (size_t) targetIndex * (size_t) nb;
        const float* tol = targetTol.data() + (size_t) targetIndex * (size_t) nb;
        {
            double num = 0.0, den = 0.0, sNum = 0.0;
            for (int b = 0; b < nb; ++b)
            {
                const auto i = (size_t) b;
                if (! bandInOffset[i] || lane.gated[i] || lane.count[i] == 0)
                    continue;
                num += (double) bandWidthOct[i] * ((double) lane.sDb[i] - (double) tgt[i]);
                sNum += (double) bandWidthOct[i] * (double) lane.sDb[i];
                den += bandWidthOct[i];
            }
            if (den > 0.0)
            {
                lane.offset = num / den;
                lane.sMean = sNum / den;
                lane.offsetValid = true;
            }
        }
        const double o = lane.offset;

        // 4. raw correction
        const double sensFactor = std::pow (1.0 - lane.sens / 100.0, 1.5);
        for (int b = 0; b < nb; ++b)
        {
            const auto i = (size_t) b;
            double c = 0.0;
            if (lane.count[i] != 0 && lane.offsetValid)
            {
                const double t = (double) tgt[i] + o;
                switch (mode)
                {
                    case ToneBalanceMode::Shape:
                    {
                        // t - s = (T - mean T) - (S - mean S); the measured part is scaled (kMeasuredShape)
                        const double sRel = (double) lane.sDb[i] - lane.sMean;
                        c = kShapeFraction * deadBand (t - lane.sMean - kMeasuredShape * sRel);
                        break;
                    }
                    case ToneBalanceMode::Boost: c = std::max (0.0, (t - (double) tol[i]) - (double) lane.sDb[i]); break;
                    case ToneBalanceMode::Cut:
                    default:
                    {
                        const double r = t + ((double) tol[i] + 10.0) * sensFactor;
                        c = -0.8 * softKnee ((double) lane.refDb[i] - r, 3.0);
                        break;
                    }
                }
            }
            lane.c[i] = (float) c;
        }

        // the boost cap (and the pre-Amount limit at which it binds) for this hop
        const double amount = lane.amount / 100.0 * lane.warm;
        const double capBoost = toneBalanceMaxBoostDb (amount), capCut = kMaxCutDb * amount;
        const double rawBoost = amount > 0.0 ? capBoost / amount : toneBalanceMaxBoostDb (1.0);

        // 5. frequency smoothing (Shape / Boost: of the correction limited to [-12, rawBoost] first)
        if (! cutMode)
            for (int b = 0; b < nb; ++b)
            {
                auto& v = lane.c[(size_t) b];
                if (v > 0.0f)
                    v *= bandBoostRoll[(size_t) b];
                v = (float) std::clamp ((double) v, -kMaxCutDb, rawBoost);
            }
        if (cutMode)
        {
            for (int b = 0; b < nb; ++b)
            {
                double sum = 0.5 * (double) lane.c[(size_t) b], w = 0.5;
                if (b > 0)
                {
                    sum += 0.25 * (double) lane.c[(size_t) b - 1];
                    w += 0.25;
                }
                if (b + 1 < nb)
                {
                    sum += 0.25 * (double) lane.c[(size_t) b + 1];
                    w += 0.25;
                }
                lane.tmp[(size_t) b] = (float) (sum / w);
            }
        }
        else
        {
            updateKernel (lane, 0.08 * std::pow (1.5 / 0.08, lane.smooth / 100.0));
            for (int b = 0; b < nb; ++b)
            {
                const float* row = lane.kernel.data() + (size_t) b * (size_t) nb;
                double sum = 0.0;
                for (int j = lane.kernelLo[(size_t) b]; j <= lane.kernelHi[(size_t) b]; ++j)
                    sum += (double) row[j] * (double) lane.c[(size_t) j];
                lane.tmp[(size_t) b] = (float) sum;
            }
        }

        // 6 + 8. region amounts, limits, Amount; the boost presence (0 in gated or never measured bands) goes into
        // lane.c, the region weight into lane.power (free after the measurement). The pre-Amount clamp is the
        // correction at which the boost cap binds (capBoost / amount), so the bracket of step 7 holds.
        const double rl = lane.low / 100.0, rm = lane.mid / 100.0, rh = lane.high / 100.0;
        for (int b = 0; b < nb; ++b)
        {
            const auto i = (size_t) b;
            const double w = (double) bandRegion[i * 3] * rl + (double) bandRegion[i * 3 + 1] * rm
                           + (double) bandRegion[i * 3 + 2] * rh;
            lane.tmp[i] = (float) (std::clamp ((double) lane.tmp[i], -kMaxCutDb, rawBoost) * amount * w);
            lane.power[i] = (float) (amount * w);
            // boosts only in measured bands (a gated band holds its level, which may be stale, e.g. the leakage of a
            // tone's onset)
            lane.c[i] = lane.count[i] != 0 ? lane.presence[i] : 0.0f;
        }

        // final value of band i for a loudness shift x: limits scaled by Amount, boost presence, mode sign (the power-
        // neutral solve leaves the boost cap out: uncapped)
        auto finalValue = [&] (size_t i, double x, double* slope, bool uncapped) noexcept
        {
            const double raw = (double) lane.tmp[i] - x * (double) lane.power[i];
            double v = std::clamp (raw, -capCut, uncapped ? std::max (raw, -capCut) : capBoost);
            double d = raw == v ? -(double) lane.power[i] : 0.0;
            if (v > 0.0)
            {
                v *= (double) lane.c[i];
                d *= (double) lane.c[i];
            }
            if (mode == ToneBalanceMode::Cut && v > 0.0)
                v = d = 0.0;
            else if (mode == ToneBalanceMode::Boost && v < 0.0)
                v = d = 0.0;
            if (slope != nullptr)
                *slope = d;
            return v;
        };

        // 7. power neutral (Shape): Newton steps for the region-weighted shift x with
        //    sum q 10^(interpolate (final (C - x w)) / 10) = sum q   (q: bin power)
        double shift = 0.0;
        if (mode == ToneBalanceMode::Shape && amount > 0.0)
        {
            double q0 = 0.0;
            for (const float q : lane.loudAvg)
                q0 += (double) q;
            if (q0 > 1.0e-30)
            {
                // The pre-limited curve is nonnegative at -12 and nonpositive at rawBoost. Keep that bracket:
                // the boost presence and limiting make the derivative discontinuous, so plain Newton can
                // bounce across a kink and finish far from neutrality on a newly populated, sparse spectrum.
                double lower = -kMaxCutDb, upper = rawBoost;
                double bestShift = shift, bestError = 1.0e30;
                for (int it = 0; it < 16; ++it)
                {
                    for (int b = 0; b < nb; ++b)
                    {
                        const auto i = (size_t) b;
                        lane.shiftGain[i] = finalValue (i, shift, &lane.shiftSlope[i], true);
                    }
                    double f = 0.0, fd = 0.0;
                    for (size_t k = 1; k < lane.loudAvg.size(); ++k)
                    {
                        const double q = lane.loudAvg[k];
                        if (q <= 0.0)
                            continue;
                        const auto lo = (size_t) binLo[k], hi = std::min (lo + 1, (size_t) nb - 1);
                        const double t = binT[k];
                        const double v = lane.shiftGain[lo] + t * (lane.shiftGain[hi] - lane.shiftGain[lo]);
                        const double d = lane.shiftSlope[lo] + t * (lane.shiftSlope[hi] - lane.shiftSlope[lo]);
                        const double e = q * std::exp (kPowDbToLn * v);
                        f += e;
                        fd += e * d;
                    }
                    if (f <= 0.0)
                        break;
                    const double g = std::log (f / q0);              // in nepers of power
                    if (std::abs (g) < bestError)
                    {
                        bestError = std::abs (g);
                        bestShift = shift;
                    }
                    if (bestError < kPowDbToLn * 1.0e-4)
                        break;
                    if (g > 0.0)
                        lower = shift;
                    else
                        upper = shift;
                    const double midpoint = 0.5 * (lower + upper);
                    const double dg = kPowDbToLn * fd / f;           // d g / d x
                    const double next = std::abs (dg) > 1.0e-12 ? shift - g / dg : midpoint;
                    shift = next > lower && next < upper ? next : midpoint;
                }
                shift = bestShift;
            }
        }
        for (int b = 0; b < nb; ++b)
            lane.tmp[(size_t) b] = (float) finalValue ((size_t) b, shift, nullptr, false);

        // 9. ballistics and glide
        const bool freezeBoosts = tame && mode == ToneBalanceMode::Shape;
        bool allZero = true;
        double maxBoost = 0.0, maxCut = 0.0;
        for (int b = 0; b < nb; ++b)
        {
            const auto i = (size_t) b;
            const double c = lane.tmp[i];
            const double cb = std::max (c, 0.0), cc = std::min (c, 0.0);
            double gb = lane.gBoost[i], gc = lane.gCut[i];
            if (cb > gb)
            {
                if (! freezeBoosts || lane.freeze[i] == 0)   // Tame (Shape): boosts frozen for 3 hops after an onset
                    gb = std::min (cb, gb + 1.0);
            }
            else
            {
                const double tDrop = std::min ((double) bandBoostDrop[i], std::max (2.0 * hopSec, cutMode ? tauC : 0.5 * tauS));
                gb = cb + (gb - cb) * std::exp (-hopSec / tDrop);
                if (std::abs (gb - cb) < kSnapDb)
                    gb = cb;
            }
            if (cc < gc)
            {
                gc = cc;
            }
            else
            {
                gc = cc + (gc - cc) * aRel;
                if (std::abs (gc - cc) < kSnapDb)
                    gc = cc;
            }
            lane.gBoost[i] = (float) gb;
            lane.gCut[i] = (float) gc;
            const double g = (double) lane.gBoost[i] + (double) lane.gCut[i];
            double out = g;
            if (glideNow)
            {
                out = g + ((double) lane.out[i] - g) * aGlide;
                if (std::abs (out - g) < kSnapDb)
                    out = g;
            }
            lane.out[i] = (float) out;
            allZero = allZero && lane.out[i] == 0.0f;
            maxBoost = std::max (maxBoost, out);
            maxCut = std::min (maxCut, out);
        }
        lane.maxBoost = maxBoost;
        lane.maxCut = maxCut;
        return allZero;
    }

    double maxGlideError (int numLanes) const noexcept
    {
        double e = 0.0;
        for (int l = 0; l < numLanes; ++l)
            for (int b = 0; b < nb; ++b)
            {
                const auto& lane = lanes[l];
                e = std::max (e, std::abs ((double) lane.out[(size_t) b]
                                           - ((double) lane.gBoost[(size_t) b] + (double) lane.gCut[(size_t) b])));
            }
        return e;
    }

    void fillGains (const Lane& lane, float* gain, int numBins) const noexcept
    {
        const float* g = lane.out.data();
        for (int k = 0; k < numBins; ++k)
        {
            const int lo = binLo[(size_t) k];
            const float t = binT[(size_t) k];
            const float d = nb > 1 ? g[lo] + t * (g[lo + 1] - g[lo]) : g[0];
            gain[k] = d == 0.0f ? 1.0f : std::exp (d * (float) kDbToLn);
        }
    }

    void publishMeters (const Lane& lane, int set) noexcept
    {
        const float* g = lane.out.data();
        const float* s = lane.refDb.data();
        for (int i = 0; i < kToneDisplayPoints; ++i)
        {
            const int lo = dispLo[i];
            const float t = dispT[i];
            const int hi = nb > 1 ? lo + 1 : lo;
            meters.gainDb[set][i].store (g[lo] + t * (g[hi] - g[lo]));
            const float a = std::max (s[lo], -120.0f), b = std::max (s[hi], -120.0f);
            meters.levelDb[set][i].store (a + t * (b - a));
        }
        meters.targetOffsetDb[set].store ((float) lane.offset);
        meters.maxBoostDb[set].store ((float) lane.maxBoost);
        meters.maxCutDb[set].store ((float) lane.maxCut);
    }

    bool computeGains (SpectralEngine::Frame& f) noexcept override
    {
        if (nb == 0)
            return true;

        const auto mode = params.mode;
        const auto target = params.target;
        const int targetIndex = std::clamp ((int) target, 0, kNumTargets - 1);
        if (! haveLast)
        {
            haveLast = true;
            lastMode = mode;
            lastTarget = target;
        }
        else if (mode != lastMode || target != lastTarget)
        {
            lastMode = mode;
            lastTarget = target;
            glideHops = (int) std::ceil (kGlideMinSec / hopSec);
            glideActive = true;
        }
        const double aGlide = std::exp (-hopSec / kGlideTauSec);
        const bool glideNow = glideActive;

        const int numLanes = f.linked ? 1 : 2;
        bool unity = true;
        for (int l = 0; l < numLanes; ++l)
        {
            auto& lane = lanes[l];
            std::fill (lane.power.begin(), lane.power.end(), 0.0f);
            std::fill (lane.loudPow.begin(), lane.loudPow.end(), 0.0f);
            const float* p0 = f.power[0];
            const float* p1 = f.power[1];
            float* bp = lane.power.data();
            float* lp = lane.loudPow.data();
            for (int k = 1; ! f.silent && k < f.numBins; ++k)
            {
                const float p = f.linked ? 0.5f * (p0[k] + p1[k]) : (l == 0 ? p0[k] : p1[k]);
                const int a = binBandA[(size_t) k], b = binBandB[(size_t) k];
                if (a >= 0)
                    bp[a] += binWA[(size_t) k] * p;
                if (b >= 0)
                    bp[b] += binWB[(size_t) k] * p;
                lp[k] = p;
            }

            const auto& sp = params.sets[f.linked ? 0 : l];
            const bool zero = updateLane (lane, sp, mode, targetIndex, glideNow, aGlide, f.silent);
            if (f.linked)
            {
                publishMeters (lane, 0);
                publishMeters (lane, 1);
            }
            else
            {
                publishMeters (lane, l);
            }
            if (! zero && ! f.silent)
            {
                fillGains (lane, f.gain[l], f.numBins);
                unity = false;
            }
        }

        if (glideActive)
        {
            if (glideHops > 0)
                --glideHops;
            if (glideHops == 0 && maxGlideError (numLanes) < 0.01)
            {
                glideActive = false;
                for (int l = 0; l < numLanes; ++l)
                    for (int b = 0; b < nb; ++b)
                        lanes[l].out[(size_t) b] = lanes[l].gBoost[(size_t) b] + lanes[l].gCut[(size_t) b];
            }
        }
        return unity;
    }
};

ToneBalance::ToneBalance() : impl (std::make_unique<Impl>()) {}
ToneBalance::~ToneBalance() = default;

void ToneBalance::prepare (double sampleRate, int maxBlockSize)
{
    auto& d = *impl;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    d.engine.prepare (d.sampleRate, kBaseFftSize, std::max (1, maxBlockSize));
    d.engine.setClient (&d);
    d.latency = d.engine.getLatencySamples();
    d.hopSec = (double) d.engine.getHopSize() / d.sampleRate;
    d.buildGrid (d.engine.getBinHz(), d.engine.getFftSize() / 2 + 1);
    d.prepared = true;
    d.resetState();
}

void ToneBalance::reset() noexcept
{
    auto& d = *impl;
    if (! d.prepared)
        return;
    d.engine.reset();
    d.resetState();
}

void ToneBalance::setParams (ParamValues values) noexcept
{
    auto& d = *impl;
    readToneBalanceParams (values, d.params);
    if (getParamSet() == 1)
        d.params.sets[0] = d.params.sets[1];
}

void ToneBalance::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    auto& d = *impl;
    if (! d.prepared || numSamples <= 0)
        return;
    ScopedFlushDenormals noDenormals;
    // Stereo (and the T/S parts, which the chain runs as Stereo): one linked curve from the power-sum detector.
    // M/S (and L/R, should it ever be routed here): ch0 with set A, ch1 with set B.
    const bool linked = ctx.channelMode == ChannelMode::Stereo || ctx.channelMode == ChannelMode::TransientSustain;
    d.engine.process (channels, numSamples, linked);
}

int ToneBalance::getLatencySamples() const noexcept { return impl->latency; }
const ToneBalanceMeters& ToneBalance::getMeters() const noexcept { return impl->meters; }
} // namespace ps
