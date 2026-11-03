#include "dsp/ClassicLimiter.h"

#include "dsp/Envelope.h"
#include "dsp/Filters.h"
#include "dsp/MaximizerStages.h"
#include "dsp/Oversampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_CLIM_HAS_SSE 1
#endif

// Classic Limiter (mod-clim). Contract and signal flow: ClassicLimiter.h. Choices made here (the header leaves them
// open), first-principles values until the reference renders are fitted (only the constant block below changes then):
//
// - Variable-mu element: g = 1 / (1 + u^kMuExponent) with kMuExponent 2 (a remote-cutoff tube's gain falls roughly
//   with the square of the control voltage above its knee). In the feedback loop a steady tone settles where
//   y = a g (y): reduction 0.5 dB at 4 dB over the knee start, 1.0 dB at 6 dB, 2.7 dB at 10 dB (the ceiling), 6.0 dB
//   at 16 dB (the stage output then sits at the ceiling), 9.7 dB at 22 dB: the effective ratio rises with level
//   (1.3:1 just above the knee .. 2.4:1 at 16 dB over); the brickwall takes what is left.
// - Detector: the gain element's output v = G x g of the previous sample, BEFORE the saturator (the saturator runs
//   oversampled, 3 samples late; it changes the level by < 0.4 dB). Peak level max (|L|, |R|); Analog through a 2nd
//   order Butterworth high-pass at kAnalogDetHpHz. Both detectors always run; the mode's 20 ms crossfade blends them.
//   env: attack one-pole while the level is above env, release one-pole below it.
// - Program-dependent release: held share h, a one-pole (kHoldTauMs) toward 1 while the current reduction is deeper
//   than kHoldMinDb and toward 0 otherwise; release time = (1 + h (kSlowReleaseMul - 1)) x the table's release.
// - Times: the tables below (log-spaced from character 0 to 10, linear interpolation in between); during a mode change
//   they are the weighted mean of the modes' times. Coefficients update every 16 samples of the global sample count
//   (so the output does not depend on the host's block size).
// - Tube saturation (residue form, as the Maximizer's soft clip): the output is v delayed by 3 samples plus the
//   residue d C (sat (w) - w), w = v / C, sat (w) - w = kTubeOdd (tanh (w) - w) + kTubeEven w^2 / (1 + w^2), computed on
//   2x minimum-phase oversampled v and downsampled; the residue passes a kResidueDcHz high-pass (the even term's DC).
//   d = mode weight (Analog 0, Tube 1, Modern kModernBlend) x clamp (|20 log10 g| / kSatFullDb, 0, 1). While d is 0 the
//   residue is exactly 0 (the oversampler still runs, so its state stays warm).
// - Brickwall: maxi::LimiterCore, attack = the whole lookahead, fast release kWallReleaseMs (character 0 .. 10, log
//   interpolated), no slow stage, linked; half-sample detection with True Peak on, aiming kCoreTpMargin under the
//   ceiling. maxi::Guard: attack = its maximum (0.5 ms), release kGuardReleaseMs, linked, true peak / sample peak per
//   sample (stamped), aiming kGuardTpMargin under the ceiling with True Peak on. Then a sample clamp at the ceiling.
//   The ceiling and the True Peak switch are stamped on every input sample and each stage limits a sample against its
//   own stamp (as the Maximizer), so a ceiling change is never violated while it travels through the lookahead.
// - Meters per block: muReductionDb = the deepest mu reduction, brickwallReductionDb = the minimum combined wall
//   gain at output time. History per 64 output samples: the minimum mu and total reduction, with each stage's gain
//   delayed to the same output sample before combining it. Minima never combine unrelated transients.
namespace ps
{
namespace
{
    using maxi::kChunk;   // 64: internal chunk, history decimation
    constexpr double kTwentyOverLn10 = 8.6858896380650366;   // 20 / ln (10)

    // ---- tunable coefficients (the fit to the reference's renders changes only these) ----
    constexpr double kKneeDb = 10.0;           // the mu element's soft knee starts this far under the ceiling
    constexpr double kMuExponent = 2.0;        // g = 1 / (1 + u^k)
    constexpr double kAnalogDetHpHz = 60.0;    // Analog: detector high-pass ("tighter lows")
    // Attack / release (ms) per mode (Analog, Tube, Modern) at character 0, 1, .. 10
    constexpr double kAttackMs[3][11] {
        { 0.1, 0.141, 0.197, 0.277, 0.39, 0.548, 0.77, 1.08, 1.52, 2.14, 3.0 },
        { 0.4, 0.562, 0.79, 1.11, 1.56, 2.19, 3.08, 4.33, 6.08, 8.54, 12.0 },
        { 0.2, 0.281, 0.395, 0.555, 0.78, 1.1, 1.54, 2.16, 3.04, 4.27, 6.0 },
    };
    constexpr double kReleaseMs[3][11] {
        { 40.0, 54.0, 72.8, 98.3, 133.0, 179.0, 241.0, 326.0, 439.0, 593.0, 800.0 },
        { 80.0, 108.0, 146.0, 197.0, 265.0, 358.0, 483.0, 651.0, 879.0, 1190.0, 1600.0 },
        { 60.0, 81.0, 109.0, 147.0, 199.0, 268.0, 362.0, 489.0, 659.0, 889.0, 1200.0 },
    };
    constexpr double kHoldTauMs = 300.0;       // program-dependent release: held share time constant
    constexpr double kHoldMinDb = 1.0;         //   reduction deeper than this counts as "held"
    constexpr double kSlowReleaseMul = 4.0;    //   fully held: the release is this many times slower
    constexpr double kSatFullDb = 6.0;         // tube saturation reaches full drive at this much mu reduction
    constexpr double kTubeOdd = 0.15;          // odd (tanh) share of the tube curve
    constexpr double kTubeEven = 0.08;         // even (asymmetric) share: mostly 2nd harmonic
    constexpr double kResidueDcHz = 10.0;      // high-pass on the saturation residue (removes the even term's DC)
    constexpr double kModernBlend = 0.5;       // Modern: share of the tube colour (the rest is the clean element)
    constexpr double kLookaheadMs = 1.5;       // brickwall LimiterCore lookahead
    constexpr double kGuardAttackMs = 0.5;     // true-peak guard attack (its latency)
    constexpr double kWallReleaseMs[2] { 5.0, 80.0 };   // LimiterCore fast release at character 0 / 10
    constexpr double kGuardReleaseMs = 10.0;
    constexpr double kCoreTpMargin = 0.99655206468;     // -0.03 dB (True Peak on)
    constexpr double kGuardTpMargin = 0.99770006382;    // -0.02 dB (True Peak on)
    constexpr double kGlideMs = 20.0;          // input gain and ceiling glide
    constexpr double kModeFadeMs = 20.0;       // mode crossfade
    // ---- end of the tunable block ----

    constexpr float kMaxInput = 1.0e6f;        // reject corrupt audio before detector, delay and float oversampler
    constexpr int kSatDelay = 3;               // 2x MinimumPhase oversampler round trip (Oversampler.h)
    constexpr int kTimeUpdate = 16;            // samples between time-coefficient updates

    int lookaheadFor (double fs) noexcept { return std::max (2, (int) std::lround (kLookaheadMs * 1.0e-3 * fs)); }
    int guardAttackFor (double fs) noexcept { return std::max (1, (int) std::lround (kGuardAttackMs * 1.0e-3 * fs)); }

    inline double muPower (double u) noexcept
    {
        if constexpr (kMuExponent == 2.0)
            return u * u;
        else
            return std::pow (u, kMuExponent);
    }

    // Time of a character 0..10 from an 11-entry table (linear in between).
    double tableMs (const double (&t)[11], double character) noexcept
    {
        const double c = std::clamp (character, 0.0, 10.0);
        const int i = std::min (9, (int) c);
        return t[i] + (c - i) * (t[i + 1] - t[i]);
    }

    inline double chargeFor (double samples) noexcept { return samples > 1.0e-9 ? -std::expm1 (-1.0 / samples) : 1.0; }

    // The largest float that is <= v (so a float sample clamped to it is <= v in double).
    float floatAtMost (double v) noexcept
    {
        float f = (float) v;
        if ((double) f > v)
            f = std::nextafter (f, 0.0f);
        return f;
    }

    struct Ramp
    {
        double current = 1.0, target = 1.0, step = 0.0;
        int remaining = 0, length = 1;

        void setTarget (double v) noexcept
        {
            if (v == target)
                return;
            target = v;
            remaining = length;
            step = (target - current) / (double) length;
        }
        void snap (double v) noexcept
        {
            current = target = v;
            remaining = 0;
            step = 0.0;
        }
        double next() noexcept
        {
            if (remaining > 0)
                current = (--remaining == 0) ? target : current + step;
            return current;
        }
    };

    struct DenormalGuard
    {
#if PS_CLIM_HAS_SSE
        unsigned int saved = 0;
        DenormalGuard() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }   // FTZ | DAZ
        ~DenormalGuard() { _mm_setcsr (saved); }
#endif
    };
} // namespace

int ClassicLimiter::latencyFor (double sampleRate) noexcept
{
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    return kSatDelay + (lookaheadFor (fs) + maxi::kMidDetectorDelay) + (guardAttackFor (fs) + maxi::kTpDetectorDelay + 6);
}

//==============================================================================
struct ClassicLimiter::Impl
{
    ClassicLimiterMeters meters;
    StereoSampleFifo history;

    double sr = 48000.0;
    bool prepared = false, snapNext = true;
    ClassicLimiterParams params;
    int latency = latencyFor (48000.0), latCore = 0, latGuard = 0;

    // controls
    Ramp gain, ceiling;
    std::array<double, 3> modeW { 1.0, 0.0, 0.0 };
    double modeStep = 1.0;

    // variable-mu stage
    double env = 0.0, held = 0.0, kneeLin = 0.31622776601683794;
    double kAtt = 1.0, kRel = 0.0, kHold = 0.0, relBaseSamples = 1.0;
    Biquad detHp[2];
    Oversampler oversampler;
    DelayLine clean[2];
    Biquad residueHp[2];

    // brickwall
    maxi::LimiterCore core;
    maxi::Guard guard;
    maxi::CoreSettings coreSettings;

    // stamps (index = absolute input sample index)
    std::vector<double> ceilRing;
    std::vector<double> coreGainRing;
    std::vector<std::uint8_t> tpRing;
    std::vector<float> muRing;
    std::array<std::vector<float>, 2> refRing;
    long long ringMask = 0, counter = 0;

    // chunk buffers
    float vBuf[2][kChunk] {}, resBuf[2][kChunk] {}, stageBuf[2][kChunk] {}, coreBuf[2][kChunk] {}, outBuf[2][kChunk] {};
    double driveBuf[kChunk] {}, ceilBuf[kChunk] {}, guardGainBuf[kChunk] {};

    // meters (per block / per history entry)
    double blockMuMinDb = 0.0, blockWallMin = 1.0, blockPeak = 0.0, blockTotalMinDb = 0.0;
    double histMuDb = 0.0, histTotalDb = 0.0;

    //==========================================================================
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        kneeLin = std::pow (10.0, -kKneeDb / 20.0);
        oversampler.prepare (1, Oversampler::Quality::MinimumPhase, kChunk, 2);
        for (auto& d : clean)
        {
            d.prepare (kSatDelay);
            d.setDelay (kSatDelay);
        }
        for (auto& f : detHp)
            f.setCoeffs (design::highPass (sr, kAnalogDetHpHz, 0.70710678118654752));
        for (auto& f : residueHp)
            f.setCoeffs (design::highPass1 (sr, kResidueDcHz));

        core.prepare (sr, kLookaheadMs);
        core.setLookahead (lookaheadFor (sr));
        guard.prepare (sr, guardAttackFor (sr));
        guard.setMaxAttack (guardAttackFor (sr));
        guard.setTimes (guardAttackFor (sr), kGuardReleaseMs, 0.0);
        latCore = core.getLatency();
        latGuard = guard.getLatency();
        latency = kSatDelay + latCore + latGuard;   // == latencyFor (sr) (tests check both and the measured delay)

        const int ringSize = maxi::nextPow2 (latency + 2 * kChunk + 64);
        ringMask = ringSize - 1;
        ceilRing.assign ((size_t) ringSize, 1.0);
        coreGainRing.assign ((size_t) ringSize, 1.0);
        tpRing.assign ((size_t) ringSize, 0);
        muRing.assign ((size_t) ringSize, 0.0f);
        for (auto& r : refRing)
            r.assign ((size_t) ringSize, 0.0f);

        gain.length = ceiling.length = std::max (1, (int) std::lround (kGlideMs * 1.0e-3 * sr));
        modeStep = 1.0 / std::max (1.0, kModeFadeMs * 1.0e-3 * sr);
        kHold = chargeFor (kHoldTauMs * 1.0e-3 * sr);
        history.prepare (maxi::nextPow2 (std::max (1 << 12, (int) (sr / kChunk) * 4)));
        prepared = true;
        applyCoreSettings();
        resetAll();
    }

    double gainTarget() const noexcept { return std::pow (10.0, (params.ceilingDb - params.thresholdDb) / 20.0); }
    double ceilingTarget() const noexcept { return std::pow (10.0, params.ceilingDb / 20.0); }

    void snapControls() noexcept
    {
        gain.snap (gainTarget());
        ceiling.snap (ceilingTarget());
        modeW.fill (0.0);
        modeW[(size_t) params.mode] = 1.0;
    }

    void resetAll() noexcept
    {
        if (! prepared)
            return;
        snapControls();
        env = held = 0.0;
        for (auto& f : detHp)
            f.reset();
        for (auto& f : residueHp)
            f.reset();
        for (auto& d : clean)
            d.reset();
        oversampler.reset();
        core.reset();
        guard.reset();
        std::fill (ceilRing.begin(), ceilRing.end(), ceiling.current);
        std::fill (coreGainRing.begin(), coreGainRing.end(), 1.0);
        std::fill (tpRing.begin(), tpRing.end(), (std::uint8_t) (params.truePeak ? 1 : 0));
        std::fill (muRing.begin(), muRing.end(), 0.0f);
        for (auto& r : refRing)
            std::fill (r.begin(), r.end(), 0.0f);
        counter = 0;
        histMuDb = histTotalDb = 0.0;
        updateTimes();
        snapNext = true;
    }

    void applyCoreSettings() noexcept
    {
        coreSettings.attackMs = kLookaheadMs;
        coreSettings.holdMs = 0.0;
        const double t = std::clamp ((double) params.character, 0.0, 10.0) / 10.0;
        coreSettings.fastReleaseMs = kWallReleaseMs[0] * std::pow (kWallReleaseMs[1] / kWallReleaseMs[0], t);
        coreSettings.slowChargeMs = 0.0;
        coreSettings.slowReleaseMs = 100.0;
        coreSettings.lfAdapt = 0.0;
        coreSettings.transientPreserve = 0.0;
        coreSettings.siTrans = coreSettings.siSus = 0.0;
        coreSettings.midDetect = params.truePeak;
        coreSettings.denseReleaseMs = 0.0;
        core.setSettings (coreSettings);
    }

    void setParams (ParamValues v) noexcept
    {
        readClassicLimiterParams (v, params);
        if (! std::isfinite (params.thresholdDb) || ! std::isfinite (params.ceilingDb) || ! std::isfinite (params.character))
        {
            const ClassicLimiterParams d;
            if (! std::isfinite (params.thresholdDb)) params.thresholdDb = d.thresholdDb;
            if (! std::isfinite (params.ceilingDb)) params.ceilingDb = d.ceilingDb;
            if (! std::isfinite (params.character)) params.character = d.character;
        }
        params.thresholdDb = std::clamp (params.thresholdDb, -12.0f, 0.0f);
        params.ceilingDb = std::clamp (params.ceilingDb, -12.0f, 0.0f);
        params.character = std::clamp (params.character, 0.0f, 10.0f);
        if (! prepared)
            return;
        applyCoreSettings();
        if (snapNext)
        {
            snapControls();
            std::fill (ceilRing.begin(), ceilRing.end(), ceiling.current);
            std::fill (tpRing.begin(), tpRing.end(), (std::uint8_t) (params.truePeak ? 1 : 0));
            updateTimes();
            snapNext = false;
        }
        else
        {
            gain.setTarget (gainTarget());
            ceiling.setTarget (ceilingTarget());
        }
    }

    // Attack / release coefficients from the character, the mode weights and the held share.
    void updateTimes() noexcept
    {
        double att = 0.0, rel = 0.0;
        for (int m = 0; m < 3; ++m)
        {
            att += modeW[(size_t) m] * tableMs (kAttackMs[m], params.character);
            rel += modeW[(size_t) m] * tableMs (kReleaseMs[m], params.character);
        }
        kAtt = chargeFor (att * 1.0e-3 * sr);
        relBaseSamples = rel * 1.0e-3 * sr;
        kRel = chargeFor (relBaseSamples * (1.0 + held * (kSlowReleaseMul - 1.0)));
    }

    //==========================================================================
    void processChunk (float* const* io, float* const* deltaOut, bool deltaInPlace, int n) noexcept
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                if (! std::isfinite (io[c][i]) || std::abs (io[c][i]) > kMaxInput)
                    io[c][i] = 0.0f;
        const long long idx0 = counter;
        const int target = (int) params.mode;
        const auto tp = (std::uint8_t) (params.truePeak ? 1 : 0);
        double maxDrive = 0.0;

        // variable-mu stage (base rate, feedback detector)
        for (int i = 0; i < n; ++i)
        {
            const long long idx = idx0 + i;
            // Transfer weight from the current blend to the target. Independently
            // subtracting a step from both other modes loses weight on interrupted fades.
            const double oldTarget = modeW[(size_t) target];
            const double newTarget = std::min (1.0, oldTarget + modeStep);
            const double scale = oldTarget < 1.0 ? (1.0 - newTarget) / (1.0 - oldTarget) : 0.0;
            for (int m = 0; m < 3; ++m)
                modeW[(size_t) m] = m == target ? newTarget : modeW[(size_t) m] * scale;
            if ((idx % kTimeUpdate) == 0)
                updateTimes();

            const double G = gain.next();
            const double C = ceiling.next();
            const auto slot = (size_t) (idx & ringMask);
            const double x0 = (double) io[0][i] * G, x1 = (double) io[1][i] * G;
            ceilRing[slot] = C;
            tpRing[slot] = tp;
            refRing[0][slot] = (float) x0;
            refRing[1][slot] = (float) x1;

            const double K = C * kneeLin;
            const double uk = env > K ? muPower ((env - K) / (C - K)) : 0.0;
            const double g = 1.0 / (1.0 + uk);
            const double redDb = uk > 0.0 ? -kTwentyOverLn10 * std::log1p (uk) : 0.0;
            const double v0 = x0 * g, v1 = x1 * g;
            vBuf[0][i] = (float) v0;
            vBuf[1][i] = (float) v1;
            muRing[slot] = (float) redDb;
            blockMuMinDb = std::min (blockMuMinDb, redDb);

            // detector for the next sample
            const double h0 = detHp[0].processSample (v0), h1 = detHp[1].processSample (v1);
            const double plain = std::max (std::abs (v0), std::abs (v1));
            const double hp = std::max (std::abs (h0), std::abs (h1));
            const double level = plain + modeW[0] * (hp - plain);
            held += ((redDb < -kHoldMinDb ? 1.0 : 0.0) - held) * kHold;
            env += (level - env) * (level > env ? kAtt : kRel);
            if (env < 1.0e-20)
                env = 0.0;

            const double residueW = modeW[1] + kModernBlend * modeW[2];
            const double d = residueW * std::min (1.0, -redDb / kSatFullDb);
            driveBuf[i] = d;
            ceilBuf[i] = C;
            maxDrive = std::max (maxDrive, d);
        }

        // tube saturation residue (2x oversampled)
        {
            const float* in[2] { vBuf[0], vBuf[1] };
            float* const* up = oversampler.upsample (in, n);
            const int n2 = 2 * n;
            for (int c = 0; c < 2; ++c)
            {
                float* u = up[c];
                if (maxDrive <= 0.0)
                {
                    std::fill (u, u + n2, 0.0f);
                    continue;
                }
                for (int j = 0; j < n2; ++j)
                {
                    const double d = driveBuf[j >> 1];
                    if (d <= 0.0)
                    {
                        u[j] = 0.0f;
                        continue;
                    }
                    const double C = ceilBuf[j >> 1];
                    const double w = (double) u[j] / C;
                    const double w2 = w * w;
                    u[j] = (float) (d * C * (kTubeOdd * (std::tanh (w) - w) + kTubeEven * w2 / (1.0 + w2)));
                }
            }
            float* res[2] { resBuf[0], resBuf[1] };
            oversampler.downsample (res, n);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                {
                    const double r = residueHp[c].processSample ((double) resBuf[c][i]);
                    stageBuf[c][i] = (float) ((double) clean[c].process (vBuf[c][i]) + r);
                }
        }

        // brickwall
        for (int i = 0; i < n; ++i)
        {
            const long long idx = idx0 + i;
            float* stage[2] { stageBuf[0] + i, stageBuf[1] + i };
            float* coreOut[2] { coreBuf[0] + i, coreBuf[1] + i };
            float* out[2] { outBuf[0] + i, outBuf[1] + i };
            for (int c = 0; c < 2; ++c)
                core.minGain[c] = guard.minGain[c] = 1.0;
            const auto cs = (size_t) ((idx - kSatDelay - maxi::kMidDetectorDelay) & ringMask);
            const double coreThreshold = ceilRing[cs] * (tpRing[cs] != 0 ? kCoreTpMargin : 1.0);
            core.process (stage, coreOut, &coreThreshold, 1);
            coreGainRing[(size_t) (idx & ringMask)] = std::min (core.minGain[0], core.minGain[1]);
            const auto gs = (size_t) ((idx - kSatDelay - latCore - maxi::kTpDetectorDelay) & ringMask);
            const double guardThreshold = ceilRing[gs] * (tpRing[gs] != 0 ? kGuardTpMargin : 1.0);
            guard.process (coreOut, out, &guardThreshold, &tpRing[gs], 1);
            guardGainBuf[i] = std::min (guard.minGain[0], guard.minGain[1]);
        }

        // clamp, delta, output
        for (int i = 0; i < n; ++i)
        {
            const auto oi = (size_t) ((idx0 + i - latency) & ringMask);
            const float c = floatAtMost (ceilRing[oi]);
            const double muOut = muRing[oi];
            double clampGain = 1.0;
            for (int ch = 0; ch < 2; ++ch)
            {
                const double magnitude = std::abs ((double) outBuf[ch][i]);
                if (magnitude > (double) c)
                    clampGain = std::min (clampGain, (double) c / magnitude);
                const float y = std::clamp (outBuf[ch][i], -c, c);
                const float dy = y - refRing[(size_t) ch][oi];
                if (deltaOut != nullptr)
                    deltaOut[ch][i] = dy;
                io[ch][i] = deltaInPlace ? dy : y;
                blockPeak = std::max (blockPeak, (double) std::abs (y));
            }
            const double wall = coreGainRing[(size_t) ((idx0 + i - latGuard) & ringMask)] * guardGainBuf[i] * clampGain;
            const double wallDb = std::min (0.0, kTwentyOverLn10 * std::log (std::max (wall, 1.0e-10)));
            blockWallMin = std::min (blockWallMin, wall);
            histMuDb = std::min (histMuDb, muOut);
            histTotalDb = std::min (histTotalDb, muOut + wallDb);
            blockTotalMinDb = std::min (blockTotalMinDb, muOut + wallDb);
            if ((idx0 + i + 1) % kChunk == 0)
            {
                const float l = (float) histMuDb, r = (float) histTotalDb;
                history.push (&l, &r, 1);
                histMuDb = histTotalDb = 0.0;
            }
        }
        counter += n;
    }

    void process (float* const* io, int numSamples, const ProcessContext& ctx) noexcept
    {
        DenormalGuard denormals;
        blockMuMinDb = 0.0;
        blockWallMin = 1.0;
        blockPeak = 0.0;
        blockTotalMinDb = 0.0;

        for (int start = 0; start < numSamples;)
        {
            const int n = std::min (numSamples - start, kChunk - (int) (counter % kChunk));
            float* chunk[2] { io[0] + start, io[1] + start };
            float* d[2] { nullptr, nullptr };
            if (ctx.deltaOut != nullptr)
            {
                d[0] = ctx.deltaOut[0] + start;
                d[1] = ctx.deltaOut[1] + start;
            }
            processChunk (chunk, ctx.deltaOut != nullptr ? d : nullptr, ctx.deltaOut == nullptr && ctx.delta, n);
            start += n;
        }

        meters.muReductionDb.store ((float) blockMuMinDb);
        meters.brickwallReductionDb.store ((float) std::min (0.0, kTwentyOverLn10 * std::log (std::max (blockWallMin, 1.0e-10))));
        meters.peakReductionDb.storeMax ((float) -blockTotalMinDb);
        meters.outputPeakDb.storeMax (blockPeak > 1.0e-10 ? (float) (kTwentyOverLn10 * std::log (blockPeak)) : -200.0f);
    }
};

//==============================================================================
ClassicLimiter::ClassicLimiter() : impl (std::make_unique<Impl>()) {}
ClassicLimiter::~ClassicLimiter() = default;

void ClassicLimiter::prepare (double sampleRate, int) { impl->prepare (sampleRate); }
void ClassicLimiter::reset() noexcept { impl->resetAll(); }
void ClassicLimiter::setParams (ParamValues v) noexcept { impl->setParams (v); }

void ClassicLimiter::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    if (! impl->prepared || numSamples <= 0)
        return;
    impl->process (channels, numSamples, ctx);
}

int ClassicLimiter::getLatencySamples() const noexcept { return impl->latency; }
const ClassicLimiterMeters& ClassicLimiter::getMeters() const noexcept { return impl->meters; }
ClassicLimiterMeters& ClassicLimiter::getMeters() noexcept { return impl->meters; }
StereoSampleFifo& ClassicLimiter::getHistoryFifo() noexcept { return impl->history; }
} // namespace ps
