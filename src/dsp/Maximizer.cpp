#include "dsp/Maximizer.h"

#include "dsp/MaximizerBands.h"
#include "dsp/MaximizerLearn.h"
#include "dsp/MaximizerStages.h"
#include "core/Loudness.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
 #include <xmmintrin.h>
 #define PS_MAX_HAS_SSE 1
#endif

// Maximizer (dsp-maximizer).
//
// Pipeline (every stage in double precision where gains are formed):
//   x * Gain -> Upward Compress -> Transient Emphasis -> Soft Clip (4x) -> mode stage -> Guard -> Fine -> clamp
// - Output Level is the ceiling, not an output gain: material below it passes untouched (at neutral settings the
//   module is an exact delay). Gain and Output Level are smoothed over 30 ms. The ceiling (and the True Peak switch)
//   is stamped on every sample when it enters, and each stage limits a sample against its own stamp, so a ceiling or
//   True Peak change is never violated while it travels through the lookahead: the limiters see it coming and ramp.
// - Mode stage:
//     Low Latency : nothing (the Guard alone: attack 0.25 ms, character sets its release 0.05-100 ms)
//     Smooth      : LimiterCore, 5 ms lookahead, a fast release that lets go right after each peak (0.1 ms at
//                   character 0 .. 20 ms at 10), attack 0.05 .. 3 ms, no hold; above character 4 a slow stage fades in
//                   that settles on sustained limiting (fitted values: applySettings)
//     Transparent : the Smooth core at a slightly lower character below 5
//     Aggressive  : 7 ms lookahead, the fastest limiting up to character 1.4, then a slow stage fading in; styles
//                   Clipping, Crisp (the plain core), Balanced, Pumping (a slow stage that calms sustained bass)
//     Spectral    : SpectralStage (critical-band groups, water-filling) + LimiterCore 2.5 ms with the Smooth core
//     Multiband   : MultibandStage (4 linear-phase bands; on material with sparse highs it takes the peaks out of the
//                   low / mid bands, sharing by contribution; under steady highs bass-made peaks go through a smooth
//                   path in the bands < 1 kHz alone) + LimiterCore 2.5 ms with the Aggressive core
//   Every mode is fitted to renders of the reference (applySettings). The reference's spectral algorithm matched a
//   broadband core on the fitting material, so its band stage currently takes no share of the reduction (it stays in
//   the pipeline: constant latency; a share can come back with more reference material).
//   With True Peak on, the cores also detect half-sample peaks and aim 0.03 dB under the ceiling.
// - Guard: true-peak limiter on a 1/8-sample grid containing ps::TruePeakMeter's points (sample peak with True Peak
//   off), target 0.02 dB under the ceiling with True Peak on; Fine: the same at the exact ceiling with a 4-sample
//   attack (removes the last hundredths of a dB the Guard's gain modulation can leave); then a sample clamp at the
//   ceiling (makes the sample-peak contract exact; never active with True Peak on in practice).
// - Character 0..10 maps to attack / release of every stage (0 = clip-like, 10 = very slow); styles are presets of
//   the same controls. Stereo Independence: Transient = the fast release stage and the Guard, Sustain = the slow stage
//   and the spectral / low multiband bands.
// - Upward Compress: a PEAK detector (linked: the louder channel's |x|; instant attack, 200 ms release),
//   lift = amount * clamp ((-0.5 dBFS - peak level) / 16 dB, 0, 1), faded out below -60 dB so noise floors are not
//   raised. Lift rises over 50 ms, falls over 20 ms. Measured (round 20, Up-Test.wav: steady noise, noise bursts and
//   decaying tones at four levels, amount 3 / 6, gain +10 / +20): the lift follows the peak level, not the RMS (bursts
//   with the RMS of steady noise get ~0.1 of its lift; with its peak, the same lift), full amount below ~-17 dBFS
//   peak, none at 0 dBFS, linear in the amount. So dense, limited masters (peaks at the ceiling after their gain) get
//   little lift and a quiet piano a lot. Fitted on six Assistant slices (Maximizer alone): match 156.5 -> 187.1 dB
//   summed, none worse.
// - Transient Emphasis: up to +8 dB on onsets (0.3 ms / 15 ms envelope against a 20 ms / 200 ms one), applied before
//   the limiter, so the limiter's gain envelope and the soft clip shape the hits.
// - Soft Clip: relative to the output level (onset -3 / -9 / -30 dB), residue form on 4x oversampled audio (linear
//   phase; minimum phase in Low Latency to keep it ~1 ms).
// - Mode changes: the output fades out (raised cosine 10 ms), the pipeline is flushed and reconfigured, refilled for
//   the new latency and faded in. Latency is constant per mode and sample rate (not affected by True Peak, styles,
//   character or soft clip).
// - Delta: output - (x * Gain) aligned with the output (what upward compression, emphasis, clipping and limiting
//   changed). Written to ProcessContext::deltaOut when given; with only ProcessContext::delta set (older chain), the
//   delta replaces the output.
// - Learn: the audio thread copies 5 s of non-silent input (blocks above -70 dBFS RMS; the transport state is not
//   required, so it also works in a stopped FL Studio with live input). takeLearnedGain() (message thread) then
//   searches the gain whose simulated output from the complete selected pipeline reaches the learn target in
//   integrated LUFS (ps::LoudnessMeter), 0..20 dB. Settings are captured with the completed input, and simulation
//   excludes pipeline latency while rendering the full captured duration.
// - Meters: gainReductionDb = the largest limiter change of the last block (<= 0, all limiting stages; for the band
//   stages their energy-weighted change); peakGainReductionDb holds the most negative value since the UI last
//   exchanged it (also <= 0).
namespace ps
{
namespace
{
using namespace maxi;

constexpr double kLn10Over20 = 0.11512925464970228;
constexpr double kGuardTpMargin = 0.99770006382;   // -0.02 dB
constexpr double kCoreTpMargin = 0.99655206468;    // -0.03 dB (the core also detects half-sample peaks)
constexpr double kFadeMs = 10.0;
constexpr double kLearnSeconds = 5.0;

double lerpExp (double a, double b, double t) noexcept { return a * std::pow (b / a, t); }

// A time over character 0..10 from its values at the fitted characters 0, 1.4, 4, 7, 10 (log-linear in between).
double smoothCurve (const double (&ms)[5], double c) noexcept
{
    static constexpr double at[5] { 0.0, 1.4, 4.0, 7.0, 10.0 };
    for (int i = 1; i < 5; ++i)
        if (c <= at[i])
            return lerpExp (ms[i - 1], ms[i], (c - at[i - 1]) / (at[i] - at[i - 1]));
    return ms[4];
}

// The default algorithm's core, fitted to renders of the reference at character 0, 1.4, 4, 7 and 10 (music on identical
// input: sample match 26..30 dB, loudness within 0.15 dB; sustained 60 Hz / 1 kHz tones and kicks at 1.4 and 7). Fast
// release for transients at every setting; the slow stage that settles on sustained limiting fades in above character
// 4. No hold (it made tones cleaner than the reference).
void fastCore (maxi::CoreSettings& cs, double c) noexcept;

void smoothCore (maxi::CoreSettings& cs, double c) noexcept
{
    static constexpr double attackMs[5] { 0.05, 0.37, 1.0, 2.5, 3.0 };
    static constexpr double fastMs[5] { 0.1, 0.26, 1.0, 5.0, 20.0 };
    static constexpr double slowReleaseMs[5] { 100.0, 100.0, 100.0, 100.0, 200.0 };
    cs.attackMs = smoothCurve (attackMs, c);
    cs.holdMs = 0.0;
    cs.fastReleaseMs = smoothCurve (fastMs, c);
    cs.slowReleaseMs = smoothCurve (slowReleaseMs, c);
    cs.slowChargeMs = c <= 4.0 ? 0.0 : (c >= 7.0 ? 30.0 : 1000.0 * std::pow (0.03, (c - 4.0) / 3.0));
    cs.lfAdapt = 0.0;
    // Dense limiting: once more than ~60 % of the recent time is limited, the fast release slows (up to +100 ms), so
    // steady material driven far into the limiter is turned down rather than flattened. Measured on the spectral
    // algorithm at character 2 (Up-Test.wav, Gain +20: steady noise 10 dB over comes out 2.7 dB quieter than a plain
    // fast release; 60 ms noise bursts at 16 dB over and decaying tones barely change). Music (limited ~15-45 % of the
    // time at +8 dB) stays under the knee.
    cs.denseReleaseMs = 100.0;
    cs.denseKnee = 0.6;
    cs.denseChargeMs = 50.0;
    cs.denseReleaseEnvMs = 100.0;
    cs.densePower = 3.0;
}

// The aggressive algorithm's core (fitted like smoothCore; the reference's multiband algorithm matches it too): the
// fastest limiting up to character 1.4, then a slow stage fading in (charge 30 ms at 5, 100 ms at 10).
void fastCore (maxi::CoreSettings& cs, double c) noexcept
{
    static constexpr double fastMs[5] { 0.1, 0.1, 0.1, 0.3, 1.0 };
    cs.attackMs = 0.05;
    cs.holdMs = 0.0;
    cs.fastReleaseMs = smoothCurve (fastMs, c);
    cs.lfAdapt = 0.0;
    cs.slowChargeMs = 0.0;
    cs.slowReleaseMs = 100.0;
    if (c > 1.4)
    {
        cs.slowChargeMs = c < 5.0 ? 1000.0 * std::pow (0.03, (c - 1.4) / 3.6) : lerpExp (30.0, 100.0, (c - 5.0) / 5.0);
        cs.slowReleaseMs = c < 5.0 ? 100.0 : lerpExp (100.0, 300.0, (c - 5.0) / 5.0);
    }
}

// The largest float that is <= v (so a float sample clamped to it is <= v in double).
float floatAtMost (double v) noexcept
{
    float f = (float) v;
    if ((double) f > v)
        f = std::nextafter (f, 0.0f);
    return f;
}

struct RampD
{
    double current = 1.0, target = 1.0, step = 0.0;
    int remaining = 0, rampSamples = 1;

    void setTarget (double v) noexcept
    {
        if (v == target)
            return;
        target = v;
        remaining = rampSamples;
        step = (target - current) / (double) rampSamples;
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
#if PS_MAX_HAS_SSE
    unsigned int saved = 0;
    DenormalGuard() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }   // FTZ | DAZ
    ~DenormalGuard() { _mm_setcsr (saved); }
#endif
};

enum class Phase { Running, FadeOut, Flush, Fill, FadeIn };
} // namespace

//==============================================================================
struct Maximizer::Impl
{
    MaximizerMeters meters;
    double sr = 48000.0;
    bool prepared = false;
    int family = 1;

    MaximizerParams params;
    bool haveParams = false, settingsDirty = true, domainUnlinked = false;
    bool snapNext = true;   // set by resetAll(): the first setParams after a reset applies its values directly
    MaximizerMode activeMode = MaximizerMode::Transparent, targetMode = MaximizerMode::Transparent;
    std::atomic<int> reportedLatency { 0 };
    std::array<int, (int) MaximizerMode::Count> modeLatency {};

    // stages
    SoftClipStage clip;
    LimiterCore core;
    SpectralStage spectral;
    MultibandStage multiband;
    Guard guard, fine;
    int clipLatencyLinear = 0, clipLatencyMinimum = 0;

    // current configuration (of activeMode)
    int latClip = 0, latStage = 0, latCore = 0, latGuard = 0, latFine = 0, latTotal = 0;

    // smoothing
    RampD gain, ceiling;
    double upAmountDb = 0.0, upTargetDb = 0.0, upStep = 0.0;
    std::array<double, 2> upEnv {}, upLiftDb {};
    double kUpRel = 0.0, kLiftUp = 0.0, kLiftDown = 0.0;
    double emAmount = 0.0, emTarget = 0.0, emStep = 0.0;
    std::array<double, 2> emFast {}, emSlow {}, emBoostDb {};
    double kEmFastAtt = 0.0, kEmFastRel = 0.0, kEmSlowAtt = 0.0, kEmSlowRel = 0.0, kEmGainAtt = 0.0, kEmGainRel = 0.0;

    // stamps / reference rings (index = absolute input sample index)
    std::vector<double> ceilRing;
    std::vector<std::uint8_t> tpRing;
    std::array<std::vector<float>, 2> refRing;
    std::vector<float> fadeRing;
    long long ringMask = 0;
    long long counter = 0;

    // mode switching
    Phase phase = Phase::Running;
    int fadeLength = 480, fadePos = 0, phaseCount = 0;

    // chunk buffers
    float buf[6][2][kChunk] {};
    double thrBuf[kChunk] {};
    std::uint8_t tpBuf[kChunk] {};

    // learn (audio thread writes the capture, message thread reads it once it is complete)
    std::array<std::vector<float>, 2> capture;
    int captureLength = 0, captured = 0;
    unsigned audioLearnGen = 0;
    LearnCaptureMailbox learnCapture;
    MaximizerParams capturedParams;
    bool capturedUnlinked = false;
    LoudnessMeter simMeter;
    float simBuf[2][kChunk] {};

    // meters
    double upMeterDb = 0.0;

    //==========================================================================
    int engineLatencyFor (MaximizerMode m) const noexcept
    {
        switch (m)
        {
            case MaximizerMode::LowLatency:  return 0;
            case MaximizerMode::Spectral:    return spectral.getLatency() + coreLookaheadFor (m) + kMidDetectorDelay;
            case MaximizerMode::Multiband:   return multiband.getLatency() + coreLookaheadFor (m) + kMidDetectorDelay;
            default:                         return coreLookaheadFor (m) + kMidDetectorDelay;
        }
    }

    int coreLookaheadFor (MaximizerMode m) const noexcept
    {
        const double ms = m == MaximizerMode::Aggressive ? 7.0
                        : (m == MaximizerMode::Smooth || m == MaximizerMode::Transparent) ? 5.0 : 2.5;
        return std::max (2, (int) std::lround (msToSamples (ms, sr)));
    }

    int guardMaxAttackFor (MaximizerMode m) const noexcept
    {
        const double ms = m == MaximizerMode::LowLatency ? 0.25 : 0.75;
        return std::max (2, (int) std::lround (msToSamples (ms, sr)));
    }

    static constexpr int kFineAttack = 4;

    int latencyFor (MaximizerMode m) const noexcept
    {
        const int c = m == MaximizerMode::LowLatency ? clipLatencyMinimum : clipLatencyLinear;
        return c + engineLatencyFor (m) + (guardMaxAttackFor (m) + kTpDetectorDelay + 6)
               + (kFineAttack + kTpDetectorDelay + 6);
    }

    //==========================================================================
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        family = sr <= 50000.0 ? 1 : (sr <= 100000.0 ? 2 : (sr <= 200000.0 ? 4 : 8));

        clip.prepare (sr);
        clip.setMinimumPhase (false);
        clipLatencyLinear = clip.getLatency();
        clip.setMinimumPhase (true);
        clipLatencyMinimum = clip.getLatency();

        core.prepare (sr, 8.0);
        spectral.prepare (sr);
        multiband.prepare (sr);
        guard.prepare (sr, guardMaxAttackFor (MaximizerMode::Smooth));
        fine.prepare (sr, kFineAttack);
        fine.setMaxAttack (kFineAttack);

        int maxLat = 0;
        for (int m = 0; m < (int) MaximizerMode::Count; ++m)
        {
            modeLatency[(size_t) m] = latencyFor ((MaximizerMode) m);
            maxLat = std::max (maxLat, modeLatency[(size_t) m]);
        }
        const int ringSize = nextPow2 (maxLat + 4 * kChunk + 64);
        ringMask = ringSize - 1;
        ceilRing.assign ((size_t) ringSize, 1.0);
        tpRing.assign ((size_t) ringSize, 1);
        fadeRing.assign ((size_t) ringSize, 0.0f);
        for (auto& r : refRing)
            r.assign ((size_t) ringSize, 0.0f);

        gain.rampSamples = ceiling.rampSamples = std::max (1, (int) std::lround (msToSamples (30.0, sr)));
        fadeLength = std::max (1, (int) std::lround (msToSamples (kFadeMs, sr)));
        upStep = 12.0 / std::max (1.0, msToSamples (50.0, sr));      // amount ramps: full range in 50 ms
        emStep = 1.0 / std::max (1.0, msToSamples (50.0, sr));
        kUpRel = chargeCoeff (msToSamples (200.0, sr));
        kLiftUp = chargeCoeff (msToSamples (50.0, sr));
        kLiftDown = chargeCoeff (msToSamples (20.0, sr));
        kEmFastAtt = chargeCoeff (msToSamples (0.3, sr));
        kEmFastRel = chargeCoeff (msToSamples (15.0, sr));
        kEmSlowAtt = chargeCoeff (msToSamples (20.0, sr));
        kEmSlowRel = chargeCoeff (msToSamples (200.0, sr));
        kEmGainAtt = chargeCoeff (msToSamples (0.5, sr));
        kEmGainRel = chargeCoeff (msToSamples (20.0, sr));

        captureLength = (int) std::lround (kLearnSeconds * sr);
        for (auto& c : capture)
            c.assign ((size_t) captureLength, 0.0f);
        simMeter.prepare (sr);
        learnCapture.cancel();

        prepared = true;
        if (! haveParams)
            params = MaximizerParams {};
        targetMode = params.mode;
        resetAll();
    }

    //==========================================================================
    void configure (MaximizerMode m) noexcept
    {
        activeMode = m;
        const bool ll = m == MaximizerMode::LowLatency;
        clip.setMinimumPhase (ll);
        core.setLookahead (coreLookaheadFor (m));
        guard.setMaxAttack (guardMaxAttackFor (m));
        latClip = clip.getLatency();
        latStage = m == MaximizerMode::Spectral ? spectral.getLatency()
                 : (m == MaximizerMode::Multiband ? multiband.getLatency() : 0);
        latCore = ll ? 0 : core.getLatency();
        latGuard = guard.getLatency();
        latFine = fine.getLatency();
        latTotal = latClip + latStage + latCore + latGuard + latFine;
        // Publish the delay of the pipeline actually producing audio. During FadeOut it
        // still belongs to the old mode; changing it early misaligns the host's dry path.
        reportedLatency.store (latTotal, std::memory_order_relaxed);
        settingsDirty = true;
        applySettings();
        clip.reset();
        core.reset();
        spectral.reset();
        multiband.reset();
        guard.reset();
        fine.reset();
    }

    void resetAll() noexcept
    {
        if (! prepared)
            return;
        configure (targetMode);
        gain.snap (std::pow (10.0, params.gainDb / 20.0));
        ceiling.snap (std::pow (10.0, params.outputLevelDb / 20.0));
        std::fill (ceilRing.begin(), ceilRing.end(), ceiling.current);
        std::fill (tpRing.begin(), tpRing.end(), (std::uint8_t) (params.truePeak ? 1 : 0));
        std::fill (fadeRing.begin(), fadeRing.end(), 1.0f);
        for (auto& r : refRing)
            std::fill (r.begin(), r.end(), 0.0f);
        counter = 0;
        phase = Phase::Running;
        fadePos = 0;
        upTargetDb = upAmountDb = params.upwardCompressDb;
        snapNext = true;   // nothing is playing yet: the next parameters apply directly (no glide, no mode fade)
        upEnv.fill (0.0);
        upLiftDb.fill (0.0);
        emTarget = emAmount = params.transientEmphasisOn ? params.transientEmphasis * 0.01 : 0.0;
        emFast.fill (0.0);
        emSlow.fill (0.0);
        emBoostDb.fill (0.0);
        upMeterDb = 0.0;
    }

    //==========================================================================
    void applySettings() noexcept
    {
        if (! settingsDirty)
            return;
        settingsDirty = false;
        const auto& p = params;
        const double t = std::clamp ((double) p.character, 0.0, 10.0) / 10.0;
        const double siT = domainUnlinked ? 1.0 : std::clamp ((double) p.stereoIndependenceTransient, 0.0, 100.0) * 0.01;
        const double siS = domainUnlinked ? 1.0 : std::clamp ((double) p.stereoIndependenceSustain, 0.0, 100.0) * 0.01;

        clip.setParams (p.softClipOn, (int) p.softClipMode, p.softClipAmount * 0.01);

        CoreSettings cs;
        cs.siTrans = siT;
        cs.siSus = siS;
        cs.midDetect = p.truePeak;
        double guardAttackMs = 0.75, guardReleaseMs = lerpExp (3.0, 30.0, t), guardIndep = siT;

        switch (activeMode)
        {
            case MaximizerMode::LowLatency:
            {
                // Fitted to the reference's low latency algorithm at character 1.4, 5 and 10 (music match 25..28 dB,
                // loudness within 0.1 dB; tones at 1.4): the longest attack the low latency allows, release from
                // 0.05 ms to 100 ms.
                static constexpr double releaseMs[5] { 0.05, 0.1, 1.9, 18.5, 100.0 };
                guardAttackMs = 0.25;
                guardReleaseMs = smoothCurve (releaseMs, std::clamp ((double) p.character, 0.0, 10.0));
                break;
            }

            case MaximizerMode::Smooth:
                smoothCore (cs, std::clamp ((double) p.character, 0.0, 10.0));
                break;

            case MaximizerMode::Transparent:
            {
                // The reference's transparent algorithm matches the default one at a slightly lower character below 5
                // (1.4 ~ 1.0; music match 28..30 dB at 1.4, 5 and 10). Transient preservation made its kicks less
                // like the reference, so it is not used.
                const double c = std::clamp ((double) p.character, 0.0, 10.0);
                smoothCore (cs, c < 5.0 ? c * (1.0 - 0.06 * (5.0 - c)) : c);
                break;
            }

            case MaximizerMode::Aggressive:
            {
                // Fitted to the reference's aggressive algorithm (every style at 1.4, Balanced at 5 and 10; music
                // match 20..31 dB, loudness within 0.15 dB): the fastest limiting up to character 1.4, then a slow stage
                // fading in (charge 30 ms at 5, 100 ms at 10). The styles mostly differ on sustained bass there:
                // Balanced and Pumping add a slow stage that calms it (as far as it costs the mix no more than
                // ~0.1 / 0.3 dB), Crisp and Clipping none.
                const double c = std::clamp ((double) p.character, 0.0, 10.0);
                fastCore (cs, c);
                double charge = cs.slowChargeMs, release = cs.slowReleaseMs;
                double styleCharge = 0.0;
                const double styleRelease = lerpExp (100.0, 300.0, c / 10.0);
                switch (p.aggressiveStyle)
                {
                    case AggressiveStyle::Clipping: guardAttackMs = 0.1;  guardReleaseMs = lerpExp (1.0, 10.0, t); break;
                    case AggressiveStyle::Crisp:    guardAttackMs = 0.25; guardReleaseMs = lerpExp (1.5, 15.0, t); break;
                    case AggressiveStyle::Balanced: guardAttackMs = 0.4;  guardReleaseMs = lerpExp (2.0, 20.0, t);
                                                    styleCharge = 100.0; break;
                    case AggressiveStyle::Pumping:  guardAttackMs = 0.5;  guardReleaseMs = 10.0;
                                                    styleCharge = 30.0; break;
                }
                if (styleCharge > 0.0 && (charge <= 0.0 || styleCharge < charge))
                {
                    charge = styleCharge;
                    release = std::max (release, styleRelease);
                }
                cs.slowChargeMs = charge;
                cs.slowReleaseMs = release;
                break;
            }

            case MaximizerMode::Spectral:
            {
                // The reference's spectral algorithm matched a broadband core on the fitting material (every style:
                // music match 25..31 dB; any share of our band reduction made it quieter than the reference), so the
                // band stage is kept in the pipeline (constant latency) with no share, and the core follows the
                // default algorithm (the Transient style 1.4 lower in character).
                SpectralStage::Settings ss;
                ss.independence = siS;
                ss.amount = 0.0;
                ss.selectivity = p.spectralStyle == SpectralStyle::Classic ? 0.35 : 1.0;
                ss.releaseMs = lerpExp (20.0, 400.0, t);
                spectral.setSettings (ss);
                const double c = std::clamp ((double) p.character, 0.0, 10.0);
                smoothCore (cs, p.spectralStyle == SpectralStyle::Transient ? std::max (0.0, c - 1.4) : c);
                break;
            }

            case MaximizerMode::Multiband:
            {
                // The band stage takes the peaks the low and mid bands make on material with sparse highs (fitted on
                // the reference's test-track renders: release 0.5 ms at character 2, scaling like the other modes'
                // releases); everything else goes to the aggressive core, which the reference matches on dense
                // material.
                MultibandStage::Settings ms;
                ms.releaseMs = 0.0254 * lerpExp (10.0, 300.0, t);
                ms.siSus = siS;
                ms.siTrans = siT;
                ms.amount = 1.0;
                ms.steadyBassReleaseMs = lerpExp (2.0, 50.0, (std::clamp ((double) p.character, 0.0, 10.0) - 1.4) / 5.6);
                multiband.setSettings (ms);
                fastCore (cs, std::clamp ((double) p.character, 0.0, 10.0));
                break;
            }

            case MaximizerMode::Count:
                break;
        }

        core.setSettings (cs);
        guard.setTimes (std::max (1, (int) std::lround (msToSamples (guardAttackMs, sr))), guardReleaseMs, guardIndep,
                        activeMode == MaximizerMode::LowLatency ? siS : -1.0);
        fine.setTimes (kFineAttack, 2.0, 1.0);
    }

    //==========================================================================
    void setParams (ParamValues v) noexcept
    {
        MaximizerParams p;
        readMaximizerParams (v, p);
        const bool changedSettings = ! haveParams || p.character != params.character
                                     || p.aggressiveStyle != params.aggressiveStyle || p.spectralStyle != params.spectralStyle
                                     || p.stereoIndependenceTransient != params.stereoIndependenceTransient
                                     || p.stereoIndependenceSustain != params.stereoIndependenceSustain
                                     || p.truePeak != params.truePeak || p.softClipOn != params.softClipOn
                                     || p.softClipMode != params.softClipMode || p.softClipAmount != params.softClipAmount;
        const bool first = ! haveParams || snapNext;
        params = p;
        haveParams = true;
        if (changedSettings)
            settingsDirty = true;

        targetMode = p.mode;
        if (! prepared)
            return;

        gain.setTarget (std::pow (10.0, (double) std::clamp (p.gainDb, 0.0f, 20.0f) / 20.0));
        ceiling.setTarget (std::pow (10.0, (double) std::clamp (p.outputLevelDb, -20.0f, 0.0f) / 20.0));
        upTargetDb = std::clamp ((double) p.upwardCompressDb, 0.0, 12.0);
        emTarget = p.transientEmphasisOn ? std::clamp ((double) p.transientEmphasis, 0.0, 100.0) * 0.01 : 0.0;

        if (first)
        {
            gain.snap (gain.target);
            ceiling.snap (ceiling.target);
            upAmountDb = upTargetDb;
            emAmount = emTarget;
            if (activeMode != targetMode)
                resetAll();
            snapNext = false;
        }
    }

    //==========================================================================
    void fillThr (int n, long long idx0, int offset, double margin) noexcept
    {
        for (int i = 0; i < n; ++i)
            thrBuf[i] = ceilRing[(size_t) ((idx0 + i - offset) & ringMask)] * margin;
    }

    void fillTp (int n, long long idx0, int offset) noexcept
    {
        for (int i = 0; i < n; ++i)
            tpBuf[i] = tpRing[(size_t) ((idx0 + i - offset) & ringMask)];
    }

    double fadeValue() noexcept
    {
        switch (phase)
        {
            case Phase::Running: return 1.0;
            case Phase::FadeOut:
            {
                if (fadePos >= fadeLength)
                    return 0.0;
                const double v = 0.5 * (1.0 + std::cos (3.14159265358979323846 * (double) fadePos / fadeLength));
                ++fadePos;
                return v;
            }
            case Phase::FadeIn:
            {
                if (fadePos >= fadeLength)
                    return 1.0;
                const double v = 0.5 * (1.0 - std::cos (3.14159265358979323846 * (double) fadePos / fadeLength));
                ++fadePos;
                return v;
            }
            case Phase::Flush:
            case Phase::Fill:
            default: return 0.0;
        }
    }

    void updatePhaseBeforeChunk() noexcept
    {
        const bool wantSwitch = targetMode != activeMode;
        switch (phase)
        {
            case Phase::Running:
                if (wantSwitch)
                {
                    phase = Phase::FadeOut;
                    fadePos = 0;
                }
                break;
            case Phase::FadeIn:
                if (wantSwitch)
                {
                    phase = Phase::FadeOut;
                    fadePos = fadeLength - std::min (fadePos, fadeLength);
                }
                break;
            case Phase::FadeOut:
                if (! wantSwitch)
                {
                    phase = Phase::FadeIn;
                    fadePos = fadeLength - std::min (fadePos, fadeLength);
                }
                break;
            case Phase::Flush:
                if (! wantSwitch)
                {
                    phase = Phase::FadeIn;
                    fadePos = 0;
                }
                break;
            case Phase::Fill:
                if (wantSwitch)
                {
                    phase = Phase::Flush;
                    phaseCount = 0;
                }
                break;
        }
    }

    void updatePhaseAfterChunk (int n) noexcept
    {
        switch (phase)
        {
            case Phase::FadeOut:
                if (fadePos >= fadeLength)
                {
                    phase = Phase::Flush;
                    phaseCount = latGuard + latFine;
                }
                break;
            case Phase::Flush:
                phaseCount -= n;
                if (phaseCount <= 0)
                {
                    configure (targetMode);
                    std::fill (fadeRing.begin(), fadeRing.end(), 0.0f);
                    phase = Phase::Fill;
                    phaseCount = latClip + latStage + latCore;
                }
                break;
            case Phase::Fill:
                phaseCount -= n;
                if (phaseCount <= 0)
                {
                    phase = Phase::FadeIn;
                    fadePos = 0;
                }
                break;
            case Phase::FadeIn:
                if (fadePos >= fadeLength)
                    phase = Phase::Running;
                break;
            case Phase::Running:
                break;
        }
    }

    //==========================================================================
    void preStages (float* const* io, int n, long long idx0) noexcept
    {
        float* a0 = buf[0][0];
        float* a1 = buf[0][1];
        const auto tp = (std::uint8_t) (params.truePeak ? 1 : 0);
        for (int i = 0; i < n; ++i)
        {
            const double g = gain.next();
            const long long idx = (idx0 + i) & ringMask;
            const float u0 = (float) ((double) io[0][i] * g);
            const float u1 = (float) ((double) io[1][i] * g);
            refRing[0][(size_t) idx] = u0;
            refRing[1][(size_t) idx] = u1;
            ceilRing[(size_t) idx] = ceiling.next();
            tpRing[(size_t) idx] = tp;
            a0[i] = u0;
            a1[i] = u1;
        }

        // Upward Compress
        if (upAmountDb > 0.0 || upTargetDb > 0.0 || upLiftDb[0] > 0.0 || upLiftDb[1] > 0.0)
        {
            double maxLift = 0.0;
            for (int i = 0; i < n; ++i)
            {
                if (upAmountDb < upTargetDb)
                    upAmountDb = std::min (upTargetDb, upAmountDb + upStep);
                else if (upAmountDb > upTargetDb)
                    upAmountDb = std::max (upTargetDb, upAmountDb - upStep);
                const double x0 = a0[i], x1 = a1[i];
                for (size_t c = 0; c < 2; ++c)
                {
                    const double x = c == 0 ? x0 : x1;
                    const double pw = domainUnlinked ? x * x : std::max (x0 * x0, x1 * x1);   // peak (squared)
                    upEnv[c] = pw > upEnv[c] ? pw : upEnv[c] + (pw - upEnv[c]) * kUpRel;
                    const double level = upEnv[c] > 1.0e-20 ? 10.0 * std::log10 (upEnv[c]) : -200.0;
                    double lift = upAmountDb * std::clamp ((-0.5 - level) / 16.0, 0.0, 1.0);
                    lift *= std::clamp ((level + 80.0) / 20.0, 0.0, 1.0);
                    upLiftDb[c] += (lift - upLiftDb[c]) * (lift > upLiftDb[c] ? kLiftUp : kLiftDown);
                    if (upLiftDb[c] < 1.0e-9 && upAmountDb <= 0.0)
                        upLiftDb[c] = 0.0;
                    if (upLiftDb[c] > 0.0)
                        buf[0][c][i] = (float) (x * std::exp (upLiftDb[c] * kLn10Over20));
                    maxLift = std::max (maxLift, upLiftDb[c]);
                }
            }
            upMeterDb = maxLift;
        }
        else
        {
            upMeterDb = 0.0;
        }

        // Transient Emphasis
        if (emAmount > 0.0 || emTarget > 0.0 || emBoostDb[0] > 0.0 || emBoostDb[1] > 0.0)
        {
            for (int i = 0; i < n; ++i)
            {
                if (emAmount < emTarget)
                    emAmount = std::min (emTarget, emAmount + emStep);
                else if (emAmount > emTarget)
                    emAmount = std::max (emTarget, emAmount - emStep);
                const double x0 = a0[i], x1 = a1[i];
                for (size_t c = 0; c < 2; ++c)
                {
                    const double x = c == 0 ? x0 : x1;
                    const double a = domainUnlinked ? std::abs (x) : std::max (std::abs (x0), std::abs (x1));
                    emFast[c] += (a - emFast[c]) * (a > emFast[c] ? kEmFastAtt : kEmFastRel);
                    emSlow[c] += (a - emSlow[c]) * (a > emSlow[c] ? kEmSlowAtt : kEmSlowRel);
                    const double ratio = emFast[c] / (emSlow[c] + 1.0e-9);
                    const double boost = emAmount * 8.0 * std::clamp ((ratio - 1.15) / 0.85, 0.0, 1.0);
                    emBoostDb[c] += (boost - emBoostDb[c]) * (boost > emBoostDb[c] ? kEmGainAtt : kEmGainRel);
                    if (emBoostDb[c] < 1.0e-9 && emAmount <= 0.0)
                        emBoostDb[c] = 0.0;
                    if (emBoostDb[c] > 0.0)
                        buf[0][c][i] = (float) (x * std::exp (emBoostDb[c] * kLn10Over20));
                }
            }
        }
    }

    void processChunk (float* const* io, float* const* deltaOut, bool deltaInPlace, int n) noexcept
    {
        updatePhaseBeforeChunk();
        applySettings();
        const long long idx0 = counter;
        preStages (io, n, idx0);

        float* in0[2] { buf[0][0], buf[0][1] };
        float* b1[2] { buf[1][0], buf[1][1] };
        float* b2[2] { buf[2][0], buf[2][1] };
        float* b3[2] { buf[3][0], buf[3][1] };
        float* b4[2] { buf[4][0], buf[4][1] };
        float* b5[2] { buf[5][0], buf[5][1] };

        // The clipping curve follows each input sample's ceiling, including ramps.
        fillThr (n, idx0, 0, 1.0);
        clip.process (in0, b1, thrBuf, n);

        // mode stage
        const bool tpOn = params.truePeak;
        const double coreMargin = tpOn ? kCoreTpMargin : 1.0;
        float* const* engineOut = b1;
        switch (activeMode)
        {
            case MaximizerMode::LowLatency:
                break;
            case MaximizerMode::Spectral:
                fillThr (n, idx0, latClip, 1.0);
                spectral.process (b1, b2, thrBuf, n);
                fillThr (n, idx0, latClip + latStage + kMidDetectorDelay, coreMargin);
                core.process (b2, b3, thrBuf, n);
                engineOut = b3;
                break;
            case MaximizerMode::Multiband:
                fillThr (n, idx0, latClip + multiband.getSplitLatency(), 1.0);
                multiband.process (b1, b2, thrBuf, n);
                fillThr (n, idx0, latClip + latStage + kMidDetectorDelay, coreMargin);
                core.process (b2, b3, thrBuf, n);
                engineOut = b3;
                break;
            default:
                fillThr (n, idx0, latClip + kMidDetectorDelay, coreMargin);
                core.process (b1, b3, thrBuf, n);
                engineOut = b3;
                break;
        }

        // mode-switch fade (before the guard, so the guard keeps the contract through it)
        const int latPre = latClip + latStage + latCore;
        for (int i = 0; i < n; ++i)
        {
            const double f = fadeValue();
            fadeRing[(size_t) ((idx0 + i - latPre) & ringMask)] = (float) f;
            if (f != 1.0)
            {
                engineOut[0][i] = (float) (engineOut[0][i] * f);
                engineOut[1][i] = (float) (engineOut[1][i] * f);
            }
        }

        // guard + fine
        fillThr (n, idx0, latPre + kTpDetectorDelay, tpOn ? kGuardTpMargin : 1.0);
        fillTp (n, idx0, latPre + kTpDetectorDelay);
        guard.process (engineOut, b4, thrBuf, tpBuf, n);
        fillThr (n, idx0, latPre + latGuard + kTpDetectorDelay, 1.0);
        fillTp (n, idx0, latPre + latGuard + kTpDetectorDelay);
        fine.process (b4, b5, thrBuf, tpBuf, n);

        // clamp, delta, output
        for (int i = 0; i < n; ++i)
        {
            const long long oi = (idx0 + i - latTotal) & ringMask;
            const float c = floatAtMost (ceilRing[(size_t) oi]);
            const float f = fadeRing[(size_t) oi];
            for (int ch = 0; ch < 2; ++ch)
            {
                const float y = std::clamp (b5[ch][i], -c, c);
                const float d = y - f * refRing[(size_t) ch][(size_t) oi];
                if (deltaOut != nullptr)
                    deltaOut[ch][i] = d;
                io[ch][i] = deltaInPlace ? d : y;
            }
        }

        counter += n;
        updatePhaseAfterChunk (n);
    }

    //==========================================================================
    void captureLearn (const float* const* io, int n) noexcept
    {
        const auto request = learnCapture.load();
        if (LearnCaptureMailbox::state (request) != LearnCaptureMailbox::Listening)
            return;
        const unsigned g = LearnCaptureMailbox::generation (request);
        if (g != audioLearnGen)
        {
            audioLearnGen = g;
            captured = 0;
        }
        double e = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                e += (double) io[c][i] * (double) io[c][i];
        if (n <= 0 || e / (2.0 * n) < 1.0e-7)   // silence / not playing
            return;
        const int take = std::min (n, captureLength - captured);
        for (int c = 0; c < 2; ++c)
            std::memcpy (capture[(size_t) c].data() + captured, io[c], sizeof (float) * (size_t) take);
        captured += take;
        if (captured >= captureLength)
        {
            capturedParams = params;
            capturedUnlinked = domainUnlinked;
        }
        learnCapture.publish (request, (unsigned) captured, captured >= captureLength ? LearnCaptureMailbox::Captured
                                                                                   : LearnCaptureMailbox::Listening);
    }

    // Message thread: integrated loudness of the simulated output at a gain.
    float simulateLufs (double gainDb, Impl& simulator)
    {
        simulator.params.gainDb = (float) gainDb;
        simulator.resetAll();
        simMeter.reset();
        float* io[2] { simBuf[0], simBuf[1] };
        const int latency = simulator.latTotal;
        const int end = captureLength + latency;
        for (int start = 0; start < end; start += kChunk)
        {
            const int n = std::min (kChunk, end - start);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                    simBuf[ch][i] = start + i < captureLength ? capture[(size_t) ch][(size_t) (start + i)] : 0.0f;
            simulator.processChunk (io, nullptr, false, n);
            const int skip = std::clamp (latency - start, 0, n);
            if (n > skip)
                simMeter.process (simBuf[0] + skip, simBuf[1] + skip, n - skip);
        }
        return simMeter.integratedLufs();
    }

    bool takeLearned (float& gainDbOut)
    {
        const auto request = learnCapture.load();
        if (LearnCaptureMailbox::state (request) != LearnCaptureMailbox::Captured)
            return false;

        const double target = capturedParams.learnTargetLufs;
        // This method runs only on the message thread. The completed request keeps its capture immutable until
        // the result is consumed, while a separate pipeline makes the simulation independent of live audio state.
        Impl simulator;
        simulator.params = capturedParams;
        simulator.domainUnlinked = capturedUnlinked;
        simulator.haveParams = true;
        simulator.prepare (sr);
        DenormalGuard denormals;

        double lo = 0.0, hi = 20.0, result;
        const float atLo = simulateLufs (lo, simulator);
        const float atHi = simulateLufs (hi, simulator);
        if (! hasValue (atLo) || ! hasValue (atHi))
            result = 0.0;
        else if (atLo >= target)
            result = 0.0;
        else if (atHi <= target)
            result = 20.0;
        else
        {
            for (int it = 0; it < 10; ++it)
            {
                const double mid = 0.5 * (lo + hi);
                const float l = simulateLufs (mid, simulator);
                if (l < target)
                    lo = mid;
                else
                    hi = mid;
            }
            result = 0.5 * (lo + hi);
        }
        gainDbOut = (float) (std::round (result * 10.0) / 10.0);
        learnCapture.publish (request, (unsigned) captureLength, LearnCaptureMailbox::Idle);
        return true;
    }
};

//==============================================================================
Maximizer::Maximizer() : impl (std::make_unique<Impl>()) {}
Maximizer::~Maximizer() = default;

void Maximizer::prepare (double sampleRate, int)
{
    impl->prepare (sampleRate);
    impl->reportedLatency.store (impl->modeLatency[(size_t) (int) impl->targetMode]);
}

void Maximizer::reset() noexcept { impl->resetAll(); }

void Maximizer::setParams (ParamValues v) noexcept { impl->setParams (v); }

void Maximizer::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    auto& m = *impl;
    if (! m.prepared || numSamples <= 0)
        return;
    DenormalGuard denormals;

    const bool unlinked = ctx.channelMode == ChannelMode::MidSide || ctx.channelMode == ChannelMode::LeftRight;
    if (m.domainUnlinked != unlinked)
    {
        m.domainUnlinked = unlinked;
        m.settingsDirty = true;
        // The host supplies the channel domain only with process(). On the first
        // block after reset there is no old audio to crossfade from.
        if (m.counter == 0)
            m.configure (m.activeMode);
    }

    m.captureLearn (channels, numSamples);

    for (int c = 0; c < 2; ++c)
    {
        m.core.minGain[c] = m.guard.minGain[c] = m.fine.minGain[c] = 1.0;
        m.spectral.minGain[c] = m.multiband.minGain[c] = 1.0;
    }

    double upMax = 0.0;
    for (int start = 0; start < numSamples; start += kChunk)
    {
        const int n = std::min (kChunk, numSamples - start);
        float* io[2] { channels[0] + start, channels[1] + start };
        float* d[2] { nullptr, nullptr };
        if (ctx.deltaOut != nullptr)
        {
            d[0] = ctx.deltaOut[0] + start;
            d[1] = ctx.deltaOut[1] + start;
        }
        m.processChunk (io, ctx.deltaOut != nullptr ? d : nullptr, ctx.deltaOut == nullptr && ctx.delta, n);
        upMax = std::max (upMax, m.upMeterDb);
    }

    double grTotal = 0.0;
    for (int c = 0; c < 2; ++c)
    {
        double g = m.guard.minGain[c] * m.fine.minGain[c];
        if (m.activeMode != MaximizerMode::LowLatency)
            g *= m.core.minGain[c];
        if (m.activeMode == MaximizerMode::Spectral)
            g *= m.spectral.minGain[c];
        if (m.activeMode == MaximizerMode::Multiband)
            g *= m.multiband.minGain[c];
        const double db = std::min (0.0, 20.0 * std::log10 (std::max (g, 1.0e-10)));
        m.meters.gainReductionDb[c].store ((float) db);
        grTotal = std::min (grTotal, db);
    }
    m.meters.upwardGainDb.store ((float) upMax);
    if ((float) grTotal < m.meters.peakGainReductionDb.load())
        m.meters.peakGainReductionDb.store ((float) grTotal);
}

int Maximizer::getLatencySamples() const noexcept { return impl->reportedLatency.load (std::memory_order_relaxed); }

const MaximizerMeters& Maximizer::getMeters() const noexcept { return impl->meters; }
MaximizerMeters& Maximizer::getMeters() noexcept { return impl->meters; }

void Maximizer::startLearn() noexcept
{
    auto& m = *impl;
    if (m.captureLength <= 0)
        return;
    m.learnCapture.start();
}

void Maximizer::cancelLearn() noexcept
{
    impl->learnCapture.cancel();
}

bool Maximizer::isLearning() const noexcept
{
    return maxi::LearnCaptureMailbox::state (impl->learnCapture.load()) != maxi::LearnCaptureMailbox::Idle;
}

float Maximizer::getLearnProgress() const noexcept
{
    return (float) maxi::LearnCaptureMailbox::count (impl->learnCapture.load()) / (float) std::max (1, impl->captureLength);
}

bool Maximizer::takeLearnedGain (float& gainDb) noexcept { return impl->takeLearned (gainDb); }

} // namespace ps
