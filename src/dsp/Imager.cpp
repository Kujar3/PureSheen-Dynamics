#include "dsp/Imager.h"

#include "dsp/Crossover.h"
#include "dsp/Envelope.h"
#include "dsp/Filters.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_IMG_HAS_SSE_CSR 1
#else
 #define PS_IMG_HAS_SSE_CSR 0
#endif

// Multiband stereo Imager (dsp-stereo).
//
// Signal flow (per band b of the crossover, M = (L + R) / 2, S = (L - R) / 2):
//   w      = width_b / 100 * amount / 100                    (-1 .. +1, linear 30 ms ramp)
//   S1     = S (1 + w)                                        (w = -1: exact mono)
//   S1    += c_b z_b(M)        c_b = 0.7 * stzAmount * w      (Stereoize, bands with w > 0 only)
//   M1     = M g_rec                                          (Recover Sides, bands with w < 0 only)
//   S2     = S1 g_anti                                        (Prevent Antiphase)
//   L = M1 + S2, R = M1 - S2, summed over the bands (with the solo gains)
//
// Choices (where the research is silent):
//   - Stereoize Mode I: Haas-style. z = LP(M delayed 18 ms) + HP(M delayed 9 ms) (1st-order split at 1.6 kHz, so lows
//     get the longer delay), high-passed at 40 Hz. Classic complementary comb in L and R, nothing in L + R.
//   - Stereoize Mode II: z = M through a cascade of six 2nd-order all-passes (120 Hz .. 14 kHz, Q 0.6): a frequency-
//     dependent phase scramble with only a few ms of group delay, so transients stay put. Flat |z| = |M|.
//     Both modes only ever touch S, so L + R is unchanged (mono compatible to float rounding).
//     Switching mode crossfades the two decorrelators (30 ms).
//   - Recover Sides: the S power removed by narrowing, P_S (1 - (1 + w)^2), is put back into the band's M as a smoothed
//     gain: g_rec = sqrt (1 + r^2 P_removed / P_M) (r = 10^(gain/20), 60 ms power detectors, capped at +12 dB). At
//     w = -1 and 0 dB a hard-panned source keeps its total power (P_L + P_R), symmetrically for left and right.
//     When M cannot carry the removed energy within that cap, a smoothed copy of S restores the remainder, with
//     M/S covariance included in the gain calculation. Pure S keeps its waveform, using S's polarity in mono.
//   - Prevent Antiphase: correlation = (P_M - P_S) / (P_M + P_S), so it is >= 0 exactly when P_S <= P_M. The band's
//     output S is limited to P_S <= 0.9 P_M (30 ms detectors, 5 ms attack / 120 ms release gain), i.e. correlation
//     >= ~0.05. Fully antiphase content (M = 0) is removed, as it would be in mono.
//   - Neutral settings (every band at width 0, no antiphase, no solo): the output is the input itself (delayed by the
//     crossover latency), not the crossover's all-pass sum, so the module nulls. Leaving / entering neutral crossfades
//     (20 ms) between the input and the band sum; for a multiband Analog / Hybrid crossover the two differ in phase
//     near the crossover frequencies, so that one 20 ms fade can colour those frequencies briefly. 1 band and Digital
//     are exact.
//   - Band-count and crossover-type edits crossfade two warm engines over 30 ms, including the neutral dry path.
//     The requested latency is reported immediately; a newer topology requested during a fade is queued.
//   - Meters: per band correlation and S / M RMS ratio of the band's output, exponentially averaged (300 ms).
//   - Latency = the crossover's latency (Digital only); the module adds none.
namespace ps
{
namespace
{
#if PS_IMG_HAS_SSE_CSR
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

    constexpr double kWidthRampMs = 30.0;
    constexpr double kNeutralFadeMs = 20.0;
    constexpr double kSoloRampMs = 10.0;
    constexpr float kStereoizeMax = 0.7f;
    constexpr int kNumDiffusers = 6;
    constexpr double kDiffuserHz[kNumDiffusers] { 120.0, 330.0, 900.0, 2400.0, 6000.0, 14000.0 };
    constexpr double kDiffuserQ = 0.6;
    constexpr double kHaasLongMs = 18.0;
    constexpr double kHaasShortMs = 9.0;
    constexpr double kHaasSplitHz = 1600.0;
    constexpr double kStereoizeLowCutHz = 40.0;
    constexpr double kRecoverTauMs = 60.0;
    constexpr double kAntiTauMs = 30.0;
    constexpr double kAntiAttackMs = 5.0;
    constexpr double kAntiReleaseMs = 120.0;
    constexpr double kRecoverGainTauMs = 30.0;
    constexpr double kAntiMargin = 0.9;
    constexpr double kMaxRecoverGain = 3.9810717055349722;   // +12 dB
    constexpr double kMeterTauMs = 300.0;
    constexpr double kTinyPower = 1.0e-24;

    double poleFor (double ms, double sr) noexcept
    {
        const double samples = ms * 0.001 * sr;
        return samples > 0.0 ? 1.0 - std::exp (-1.0 / samples) : 1.0;
    }

    struct BandState
    {
        LinearSmoother width, stereoize, solo, mode;   // mode: 0 = Mode I, 1 = Mode II

        // Mode I (Haas)
        std::vector<float> haas;
        int haasPos = 0, haasMask = 0;
        Biquad haasLow, haasHigh, haasCut;
        bool modeIRunning = false;

        // Mode II (diffusion)
        Biquad diffuser[kNumDiffusers];
        bool modeIIRunning = false;

        // detectors (power)
        double recM = 0.0, recS = 0.0, recMS = 0.0, antiM = 0.0, antiS = 0.0;
        double gRec = 1.0, gRecSide = 0.0, gAnti = 1.0;

        // meters (exponentially weighted sums)
        double sLR = 0.0, sLL = 0.0, sRR = 0.0, sMM = 0.0, sSS = 0.0;

        void resetModeI() noexcept
        {
            std::fill (haas.begin(), haas.end(), 0.0f);
            haasPos = 0;
            haasLow.reset();
            haasHigh.reset();
            haasCut.reset();
        }

        void resetModeII() noexcept
        {
            for (auto& d : diffuser)
                d.reset();
        }

        void resetDynamics() noexcept
        {
            recM = recS = recMS = antiM = antiS = gRecSide = 0.0;
            gRec = gAnti = 1.0;
        }

        void resetMeters() noexcept { sLR = sLL = sRR = sMM = sSS = 0.0; }
    };
} // namespace

//==============================================================================
namespace
{
struct ImagerEngine
{
    double sr = 48000.0;
    int maxBlock = 512;
    bool prepared = false;

    Crossover xover;
    ImagerParams params;
    ModuleMonitor monitor;
    std::atomic<int> latency { 0 };

    float targetWidth[kMaxBands] {};
    float targetStereoize[kMaxBands] {};
    bool fresh = true;   // snap the ramps on the next setParams
    bool snapActive = true;   // ... and the neutral crossfade on the next process

    BandState bands[kMaxBands];
    LinearSmoother active;   // 0 = neutral (input passes), 1 = band processing

    std::vector<float> bandBuf[kMaxBands][2];
    float* bandChans[kMaxBands][2] {};
    float* const* bandPtrs[kMaxBands] {};
    std::vector<float> neutralBuf[2];
    DelayLine neutralDelay[2];
    std::vector<double> outBuf[2];

    int haasLong = 0, haasShort = 0;
    double aRec = 0.0, aAnti = 0.0, aAntiAtt = 0.0, aAntiRel = 0.0, aRecGain = 0.0;
    double aMeter = 1.0;

    ImagerMeters meters;

    //==========================================================================
    void prepare (double sampleRate, int maxBlockSize)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock = std::max (1, maxBlockSize);

        xover.prepare (sr, maxBlock);
        for (int b = 0; b < kMaxBands; ++b)
        {
            for (int c = 0; c < 2; ++c)
            {
                bandBuf[b][c].assign ((size_t) maxBlock, 0.0f);
                bandChans[b][c] = bandBuf[b][c].data();
            }
            bandPtrs[b] = bandChans[b];
        }
        for (int c = 0; c < 2; ++c)
        {
            neutralBuf[c].assign ((size_t) maxBlock, 0.0f);
            outBuf[c].assign ((size_t) maxBlock, 0.0);
            neutralDelay[c].prepare (std::max (1, xover.getLatencySamples (CrossoverType::Digital)));
        }

        haasLong = (int) std::lround (kHaasLongMs * 0.001 * sr);
        haasShort = (int) std::lround (kHaasShortMs * 0.001 * sr);
        int ring = 1;
        while (ring < haasLong + 2)
            ring <<= 1;

        for (auto& b : bands)
        {
            b.width.prepare (sr, kWidthRampMs);
            b.stereoize.prepare (sr, kWidthRampMs);
            b.solo.prepare (sr, kSoloRampMs);
            b.mode.prepare (sr, kWidthRampMs);
            b.haas.assign ((size_t) ring, 0.0f);
            b.haasMask = ring - 1;
            b.haasLow.setCoeffs (design::lowPass1 (sr, kHaasSplitHz));
            b.haasHigh.setCoeffs (design::highPass1 (sr, kHaasSplitHz));
            b.haasCut.setCoeffs (design::highPass (sr, kStereoizeLowCutHz, 0.7071067811865476));
            for (int k = 0; k < kNumDiffusers; ++k)
                b.diffuser[k].setCoeffs (design::allPass (sr, std::min (kDiffuserHz[k], 0.42 * sr), kDiffuserQ));
        }
        active.prepare (sr, kNeutralFadeMs);

        aRec = poleFor (kRecoverTauMs, sr);
        aAnti = poleFor (kAntiTauMs, sr);
        aAntiAtt = poleFor (kAntiAttackMs, sr);
        aAntiRel = poleFor (kAntiReleaseMs, sr);
        aRecGain = poleFor (kRecoverGainTauMs, sr);
        aMeter = poleFor (kMeterTauMs, sr);

        prepared = true;
        reset();
    }

    void reset() noexcept
    {
        xover.reset();
        for (auto& d : neutralDelay)
            d.reset();
        for (auto& b : bands)
        {
            b.resetModeI();
            b.resetModeII();
            b.modeIRunning = b.modeIIRunning = false;
            b.resetDynamics();
            b.resetMeters();
        }
        for (int b = 0; b < kMaxBands; ++b)
        {
            meters.correlation[b].store (0.0f);
            meters.widthRatio[b].store (0.0f);
        }
        fresh = true;
        snapActive = true;
    }

    //==========================================================================
    void setParams (const ImagerParams& requested, const ImagerParams* topology = nullptr) noexcept
    {
        const int oldBands = params.numBands;
        params = requested;
        if (topology != nullptr)
        {
            params.numBands = topology->numBands;
            params.crossoverType = topology->crossoverType;
        }
        params.numBands = std::clamp (params.numBands, 1, kMaxBands);
        // A removed band must not freeze its decorrelator or detector history and replay it on re-addition.
        for (int b = std::min (oldBands, params.numBands); b < std::max (oldBands, params.numBands); ++b)
        {
            bands[b].resetModeI();
            bands[b].resetModeII();
            bands[b].modeIRunning = bands[b].modeIIRunning = false;
            bands[b].resetDynamics();
            bands[b].resetMeters();
        }
        xover.setup (params.numBands, params.crossoverHz, params.crossoverType);

        const int lat = xover.getLatencySamples (params.crossoverType);
        latency.store (lat, std::memory_order_relaxed);
        for (auto& d : neutralDelay)
            d.setDelay (lat);

        const float amount = std::clamp (std::isfinite (params.amountPercent) ? params.amountPercent : 100.0f, 0.0f, 100.0f) * 0.01f;
        const float stz = params.stereoizeOn ? std::clamp (params.stereoizeAmount, 0.0f, 100.0f) * 0.01f : 0.0f;

        for (int b = 0; b < kMaxBands; ++b)
        {
            const float wp = std::isfinite (params.widthPercent[b]) ? params.widthPercent[b] : 0.0f;
            const float w = std::clamp (wp * 0.01f, -1.0f, 1.0f) * amount;
            targetWidth[b] = w;
            // Width is applied after smoothing, so a narrowing band cannot retain a positive injection
            // coefficient from the other half of a sign-changing width ramp.
            targetStereoize[b] = w > 0.0f ? kStereoizeMax * stz : 0.0f;

            auto& s = bands[b];
            const float modeTarget = params.stereoizeMode == StereoizeMode::ModeII ? 1.0f : 0.0f;
            if (fresh)
            {
                s.width.snap (targetWidth[b]);
                s.stereoize.snap (targetStereoize[b]);
                s.mode.snap (modeTarget);
            }
            else
            {
                s.width.setTarget (targetWidth[b]);
                s.stereoize.setTarget (targetStereoize[b]);
                s.mode.setTarget (modeTarget);
            }
        }
        applySoloTargets (fresh);
        fresh = false;
    }

    void applySoloTargets (bool snap) noexcept
    {
        const int solo = monitor.soloBand;
        const bool soloing = solo >= 0 && solo < params.numBands;
        for (int b = 0; b < kMaxBands; ++b)
        {
            const float g = ! soloing || b == solo ? 1.0f : 0.0f;
            if (snap)
                bands[b].solo.snap (g);
            else
                bands[b].solo.setTarget (g);
        }
    }

    bool isNeutral() const noexcept
    {
        if (params.preventAntiphase)
            return false;
        for (int b = 0; b < params.numBands; ++b)
        {
            const auto& s = bands[b];
            if (targetWidth[b] != 0.0f || s.width.isSmoothing() || s.width.getCurrent() != 0.0f)
                return false;
            if (s.stereoize.getCurrent() != 0.0f || s.stereoize.isSmoothing())
                return false;
            if (s.solo.getCurrent() != 1.0f || s.solo.isSmoothing())
                return false;
            if (s.gRec != 1.0 || s.gRecSide != 0.0 || s.gAnti != 1.0)
                return false;
        }
        return true;
    }

    //==========================================================================
    double modeISample (BandState& s, double m) noexcept
    {
        s.haas[(size_t) s.haasPos] = (float) m;
        const double longTap = s.haas[(size_t) ((s.haasPos - haasLong) & s.haasMask)];
        const double shortTap = s.haas[(size_t) ((s.haasPos - haasShort) & s.haasMask)];
        s.haasPos = (s.haasPos + 1) & s.haasMask;
        return s.haasCut.processSample (s.haasLow.processSample (longTap) + s.haasHigh.processSample (shortTap));
    }

    static double modeIISample (BandState& s, double m) noexcept
    {
        double z = m;
        for (auto& d : s.diffuser)
            z = d.processSample (z);
        return z;
    }

    void processBand (int b, int n) noexcept
    {
        auto& s = bands[b];
        const float* inL = bandChans[b][0];
        const float* inR = bandChans[b][1];
        double* outL = outBuf[0].data();
        double* outR = outBuf[1].data();

        const bool recoverOn = params.recoverSidesOn;
        const double recoverAmp = std::pow (10.0, (double) std::clamp (params.recoverSidesGainDb, -12.0f, 12.0f) / 20.0);
        const double recoverAmp2 = recoverAmp * recoverAmp;
        const bool antiOn = params.preventAntiphase;

        // stereoize decorrelators: run while they contribute (state reset when they start)
        const bool stzNeeded = s.stereoize.getCurrent() != 0.0f || s.stereoize.isSmoothing();
        const bool needI = stzNeeded && (s.mode.getCurrent() < 1.0f || s.mode.isSmoothing());
        const bool needII = stzNeeded && (s.mode.getCurrent() > 0.0f || s.mode.isSmoothing());
        if (needI && ! s.modeIRunning)
            s.resetModeI();
        if (needII && ! s.modeIIRunning)
            s.resetModeII();
        s.modeIRunning = needI;
        s.modeIIRunning = needII;

        const bool recActive = (recoverOn && targetWidth[b] < 0.0f) || s.gRec != 1.0 || s.gRecSide != 0.0
                               || s.width.getCurrent() < 0.0f;
        const bool antiActive = antiOn || s.gAnti != 1.0;

        for (int i = 0; i < n; ++i)
        {
            const double w = (double) s.width.next();
            const double c = (double) s.stereoize.next() * std::max (0.0, w);
            const double g = (double) s.solo.next();
            const double mx = (double) s.mode.next();

            const double L = (double) inL[i], R = (double) inR[i];
            const double M = 0.5 * (L + R);
            const double S = 0.5 * (L - R);

            double S1 = S * (1.0 + w);

            if (needI || needII)
            {
                const double zI = needI ? modeISample (s, M) : 0.0;
                const double zII = needII ? modeIISample (s, M) : 0.0;
                S1 += c * (zI + mx * (zII - zI));
            }

            double M1 = M;
            if (recActive)
            {
                s.recM += aRec * (M * M + kTinyPower - s.recM);
                s.recS += aRec * (S * S - s.recS);
                s.recMS += aRec * (M * S - s.recMS);
                double target = 1.0;
                double sideTarget = 0.0;
                if (recoverOn && w < 0.0)
                {
                    const double removed = recoverAmp2 * s.recS * (1.0 - (1.0 + w) * (1.0 + w));
                    target = std::min (kMaxRecoverGain, std::sqrt (1.0 + removed / s.recM));
                    // Gain alone cannot recover a pure Side signal (M == 0). Put the energy beyond the
                    // bounded Mid gain into a copy of S, accounting for its covariance with M. This keeps
                    // normal hard-panned sources on the gain-only path and preserves pure-S waveforms.
                    const double residual = std::max (0.0, removed - (target * target - 1.0) * s.recM);
                    if (residual > kTinyPower && s.recS > kTinyPower)
                    {
                        const double cov = target * std::abs (s.recMS);
                        const double magnitude = residual / (std::sqrt (cov * cov + s.recS * residual) + cov);
                        sideTarget = s.recMS < 0.0 ? -magnitude : magnitude;
                    }
                }
                s.gRec += aRecGain * (target - s.gRec);
                s.gRecSide += aRecGain * (sideTarget - s.gRecSide);
                if (std::abs (s.gRec - target) < 1.0e-7 && target == 1.0)
                    s.gRec = 1.0;
                if (std::abs (s.gRecSide) < 1.0e-7 && sideTarget == 0.0)
                    s.gRecSide = 0.0;
                M1 = M * s.gRec + S * s.gRecSide;
            }

            if (antiActive)
            {
                s.antiM += aAnti * (M1 * M1 + kTinyPower - s.antiM);
                s.antiS += aAnti * (S1 * S1 - s.antiS);
                double target = 1.0;
                if (antiOn && s.antiS > kAntiMargin * s.antiM)
                    target = std::sqrt (kAntiMargin * s.antiM / s.antiS);
                s.gAnti += (target < s.gAnti ? aAntiAtt : aAntiRel) * (target - s.gAnti);
                if (std::abs (s.gAnti - target) < 1.0e-7 && target == 1.0)
                    s.gAnti = 1.0;
                S1 *= s.gAnti;
            }

            const double Lo = M1 + S1;
            const double Ro = M1 - S1;
            updateMeter (s, g * Lo, g * Ro, g * M1, g * S1);

            outL[i] += g * Lo;
            outR[i] += g * Ro;
        }

        if (! recActive)
            s.recM = s.recS = s.recMS = 0.0;
        if (! antiActive)
            s.antiM = s.antiS = 0.0;

        publishMeter (b);
    }

    void meterBandPassive (int b, int n) noexcept
    {
        const float* l = bandChans[b][0];
        const float* r = bandChans[b][1];
        for (int i = 0; i < n; ++i)
        {
            const double L = (double) l[i], R = (double) r[i];
            const double M = 0.5 * (L + R), S = 0.5 * (L - R);
            updateMeter (bands[b], L, R, M, S);
        }
        publishMeter (b);
    }

    void updateMeter (BandState& s, double L, double R, double M, double S) const noexcept
    {
        // A sample EMA gives the same result for every host block partition, including changing block sizes.
        s.sLR += aMeter * (L * R - s.sLR);
        s.sLL += aMeter * (L * L - s.sLL);
        s.sRR += aMeter * (R * R - s.sRR);
        s.sMM += aMeter * (M * M - s.sMM);
        s.sSS += aMeter * (S * S - s.sSS);
    }

    void publishMeter (int b) noexcept
    {
        auto& s = bands[b];
        if (s.sLL < 1.0e-30 && s.sRR < 1.0e-30)
            s.resetMeters();

        const double e = std::sqrt (s.sLL * s.sRR);
        const double corr = e > 1.0e-20 ? std::clamp (s.sLR / e, -1.0, 1.0) : 0.0;
        double ratio = 0.0;
        if (s.sMM > 1.0e-20)
            ratio = std::min (10.0, std::sqrt (s.sSS / s.sMM));
        else if (s.sSS > 1.0e-20)
            ratio = 10.0;
        meters.correlation[b].store ((float) corr);
        meters.widthRatio[b].store ((float) ratio);
    }

    //==========================================================================
    void processChunk (float* const* ch, int n) noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            std::copy (ch[c], ch[c] + n, neutralBuf[c].data());
            neutralDelay[c].process (neutralBuf[c].data(), n);
        }

        const float* in[2] { ch[0], ch[1] };
        xover.process (in, bandPtrs, n);

        const int numBands = std::min (params.numBands, xover.getNumBands());
        for (int b = numBands; b < kMaxBands; ++b)
        {
            meters.correlation[b].store (0.0f);
            meters.widthRatio[b].store (0.0f);
        }
        const bool neutral = isNeutral();
        if (snapActive)
            active.snap (neutral ? 0.0f : 1.0f);
        else
            active.setTarget (neutral ? 0.0f : 1.0f);
        snapActive = false;

        if (! active.isSmoothing() && active.getCurrent() == 0.0f)
        {
            for (int c = 0; c < 2; ++c)
                std::copy (neutralBuf[c].data(), neutralBuf[c].data() + n, ch[c]);
            for (int b = 0; b < numBands; ++b)
            {
                meterBandPassive (b, n);
                // keep the ramps of the idle bands where their targets are
                bands[b].width.snap (targetWidth[b]);
                bands[b].stereoize.snap (targetStereoize[b]);
                bands[b].mode.snap (params.stereoizeMode == StereoizeMode::ModeII ? 1.0f : 0.0f);
                bands[b].modeIRunning = bands[b].modeIIRunning = false;
            }
            return;
        }

        for (int c = 0; c < 2; ++c)
            std::fill (outBuf[c].begin(), outBuf[c].begin() + n, 0.0);

        for (int b = 0; b < numBands; ++b)
            processBand (b, n);

        if (! active.isSmoothing() && active.getCurrent() == 1.0f)
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    ch[c][i] = (float) outBuf[c][(size_t) i];
        }
        else
        {
            for (int i = 0; i < n; ++i)
            {
                const double a = (double) active.next();
                for (int c = 0; c < 2; ++c)
                {
                    const double d = (double) neutralBuf[c][(size_t) i];
                    ch[c][i] = (float) (d + a * (outBuf[c][(size_t) i] - d));
                }
            }
        }

    }
};
} // namespace

// A topology edit changes which audio reaches each band's width/detector history and can also move the dry
// delay tap. Keep a second engine warm and crossfade complete outputs, so the neutral shortcut has exactly
// the same click-free transition guarantee as active processing. All storage is allocated in prepare().
struct Imager::Impl
{
    ImagerEngine engines[2];
    ImagerMeters meters;
    std::atomic<int> latency { 0 };
    ModuleMonitor monitor;
    ImagerParams latestParams, transitionTopology;
    LinearSmoother topologyFade;
    std::vector<float> otherBuf[2];
    int maxBlock = 512, current = 0;
    bool prepared = false, initialized = false, transitioning = false;

    static bool sameTopology (const ImagerParams& a, const ImagerParams& b) noexcept
    {
        return a.numBands == b.numBands && a.crossoverType == b.crossoverType;
    }

    void prepare (double sr, int block)
    {
        maxBlock = std::max (1, block);
        for (auto& e : engines)
            e.prepare (sr, maxBlock);
        for (auto& b : otherBuf)
            b.assign ((size_t) maxBlock, 0.0f);
        topologyFade.prepare (sr > 0.0 ? sr : 48000.0, kWidthRampMs);
        prepared = true;
        reset();
    }

    void reset() noexcept
    {
        for (auto& e : engines)
            e.reset();
        current = 0;
        initialized = transitioning = false;
        topologyFade.snap (1.0f);
        // reset() preserves the last requested settings, including a request that was still queued.
        // Re-prime both engines while their histories are empty; the next setParams may still snap.
        for (auto& e : engines)
        {
            e.setParams (latestParams);
            e.fresh = true;
        }
        latency.store (engines[0].xover.getLatencySamples (latestParams.crossoverType), std::memory_order_relaxed);
        for (int b = 0; b < kMaxBands; ++b)
        {
            meters.correlation[b].store (0.0f);
            meters.widthRatio[b].store (0.0f);
        }
    }

    void setParams (ParamValues values, int parameterSet) noexcept
    {
        readImagerParams (values, latestParams);
        if (parameterSet == 1)   // T/S sustain twin: set B for both channels (Module.h)
            useImagerSet (latestParams, latestParams.setB);
        latestParams.numBands = std::clamp (latestParams.numBands, 1, kMaxBands);
        latency.store (engines[0].xover.getLatencySamples (latestParams.crossoverType), std::memory_order_relaxed);
        applyLatestParams();
    }

    void applyLatestParams() noexcept
    {
        if (! initialized)
        {
            for (auto& e : engines)
                e.setParams (latestParams);
            initialized = true;
            return;
        }

        const int other = 1 - current;
        if (! transitioning && ! sameTopology (latestParams, engines[current].params))
        {
            transitionTopology = latestParams;
            transitioning = true;
            topologyFade.snap (0.0f);
            topologyFade.setTarget (1.0f);
        }
        if (transitioning)
        {
            const ImagerParams oldTopology = engines[current].params;
            engines[current].setParams (latestParams, &oldTopology);
            engines[other].setParams (latestParams, &transitionTopology);
        }
        else
        {
            for (auto& e : engines)
                e.setParams (latestParams);
        }
    }

    void setMonitor (const ModuleMonitor& m) noexcept
    {
        monitor = m;
        for (auto& e : engines)
        {
            e.monitor = m;
            e.applySoloTargets (false);
        }
    }

    void processChunk (float* const* ch, int n) noexcept
    {
        if (! initialized)
        {
            initialized = true;
            for (auto& e : engines)
                e.fresh = false;
        }
        float* otherCh[2] { otherBuf[0].data(), otherBuf[1].data() };
        for (int c = 0; c < 2; ++c)
            std::copy (ch[c], ch[c] + n, otherCh[c]);
        const int other = 1 - current;
        engines[current].processChunk (ch, n);
        engines[other].processChunk (otherCh, n);
        if (transitioning)
        {
            for (int i = 0; i < n; ++i)
            {
                const double blend = topologyFade.next();
                for (int c = 0; c < 2; ++c)
                    ch[c][i] = (float) ((double) ch[c][i] + blend * ((double) otherCh[c][i] - ch[c][i]));
            }
            if (! topologyFade.isSmoothing())
            {
                current = other;
                transitioning = false;
                // Consume the last request even if the caller does not repeat setParams on later blocks.
                applyLatestParams();
            }
        }
        const auto& source = engines[current].meters;
        for (int b = 0; b < kMaxBands; ++b)
        {
            meters.correlation[b].store (source.correlation[b].load());
            meters.widthRatio[b].store (source.widthRatio[b].load());
        }
    }
};

//==============================================================================
Imager::Imager() : impl (std::make_unique<Impl>()) {}
Imager::~Imager() = default;

void Imager::prepare (double sampleRate, int maxBlockSize) { impl->prepare (sampleRate, maxBlockSize); }
void Imager::reset() noexcept { impl->reset(); }
void Imager::setParams (ParamValues v) noexcept
{
    if (v != nullptr)
        impl->setParams (v, getParamSet());
}

void Imager::setMonitor (const ModuleMonitor& m) noexcept
{
    impl->setMonitor (m);
}

void Imager::process (float* const* channels, int numSamples, const ProcessContext&) noexcept
{
    auto& m = *impl;
    if (! m.prepared || channels == nullptr || channels[0] == nullptr || channels[1] == nullptr || numSamples <= 0)
        return;

    ScopedFlushDenormals noDenormals;
    for (int offset = 0; offset < numSamples; offset += m.maxBlock)
    {
        const int n = std::min (m.maxBlock, numSamples - offset);
        float* chunk[2] { channels[0] + offset, channels[1] + offset };
        m.processChunk (chunk, n);
    }
}

int Imager::getLatencySamples() const noexcept { return impl->latency.load (std::memory_order_relaxed); }
const ImagerMeters& Imager::getMeters() const noexcept { return impl->meters; }
} // namespace ps
