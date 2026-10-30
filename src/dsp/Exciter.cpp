#include "dsp/Exciter.h"

#include "dsp/Crossover.h"
#include "dsp/Envelope.h"
#include "dsp/Filters.h"
#include "dsp/Oversampler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_EXC_HAS_SSE_CSR 1
#else
 #define PS_EXC_HAS_SSE_CSR 0
#endif

// Multiband harmonic Exciter (dsp-stereo).
//
// Structure (per band b, per channel):
//   D_b   = OS_down ( f (OS_up (band_b)) - OS_up (band_b) )        harmonics + level change only ("difference" path)
//   dry_b = band_b delayed by the oversampler latency (so dry and wet are sample aligned)
//   wet_b = dry_b + D_b                                             (= f (band_b) where the oversampler is flat)
//   out   = sum_b solo_b [ (1 - mix_b) dry_b ] + PostShelf ( sum_b solo_b mix_b wet_b )
// Computing f(u) - u in the oversampled domain and adding it to the delayed band keeps the dry part bit-exact (drive 0
// is exactly transparent, nothing above 0.45 fs is lost) and the nonlinearity still runs at 4x. The difference
// signal is DC-blocked (2nd-order high pass at 5 Hz) before it is mixed in. When a band stops driving, the oversampler
// is flushed with silence and the DC blocker rings out (no state is cut mid-signal); the detectors always run, so a
// band that starts processing never sees a fake attack.
//
// Curves (our own). Every model is f(x) = comp (drive) * s_k (x) with slope 1 at x = 0, where k = drive gain:
//   k = (10^(0.012 * drive%) - 1) * modelScale    (drive 100 % = +24 dB into the curve; Triode scale 0.5: subtle)
//   Analog     s = v / (1 + v^4)^(1/4) / k, v = k x     symmetric, hard-ish knee: odd harmonics, gritty
//   Retro      s = tanh (k' x) / k',  k' = k (1 + delta_decay)   odd; the drive rises while the level falls after a
//              loud passage (a 300 ms release follower of a fast two-pole 3 ms power detector): harmonics
//              decay slowly
//   Tape       pre-emphasis (1st-order high shelf +6 dB at 4 kHz) -> v / sqrt (1 + v^2) -> exact inverse de-emphasis,
//              plus a mild direction-dependent offset (hysteresis-like, proportional to tanh of the slope; zero for a
//              static input): brighter odd harmonics
//   Tube       s = (tanh (k x + b) - tanh b) / (k (1 - tanh^2 b)), b = 0.35 + 0.5 delta_attack: asymmetric even + odd;
//              the bias rises on attacks (fast detector above a 60 ms one), so transients get more harmonics
//   Warm       s = x + 0.5 (sqrt (1 + (k x)^2) - 1) / k: an even (rectifier-like) term only: even harmonics only
//   Triode     t = tanh (k x), s = (t + 0.2 t^2) / k: one stage, subtle asymmetric overdrive
//   DualTriode two such stages in series (even term 0.25 each): stronger, and the even terms add up
//   The dynamic terms (Retro, Tube) use a dead zone, so they are exactly zero for steady signals (steady tones above
//   ~100 Hz; for lower tones the detector ripple reaches the dead zone and Retro / Tube move a little within each
//   cycle); Tape's emphasis is unity at DC and its offset needs a moving input. transferCurve() therefore equals the
//   audio path for static (steady) input.
// Level compensation: comp (drive) keeps the RMS of a -12 dBFS sine through the curve equal to its input RMS (table per
// model, 1 % drive steps, interpolated; capped at +-12 dB), so drive changes the harmonics more than the level.
// Choices (research silent): oversampling 4x linear phase (latency is reported, identical for every band); the post
// filter is a matched 2nd-order high shelf (Q 0.707) on the summed wet signal (linear, so equal to filtering each
// band's wet); a band that is off is dry. Neutral settings (every band at mix 0, or at drive 0 with the post filter
// flat; no solo) pass the input itself (delayed by the latency) instead of the crossover's all-pass sum; leaving /
// entering neutral crossfades over 20 ms. Model changes crossfade over 20 ms; drive and mix ramp over 20 ms.
namespace ps
{
namespace
{
#if PS_EXC_HAS_SSE_CSR
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

    constexpr int kNumModes = (int) ExciterMode::Count;
    constexpr int kOsLog2 = 2;   // 4x
    constexpr double kRampMs = 20.0;
    constexpr double kNeutralFadeMs = 20.0;
    constexpr double kModeFadeMs = 20.0;
    constexpr double kSoloRampMs = 10.0;
    constexpr double kShelfRampMs = 10.0;
    constexpr double kDcHz = 5.0;
    constexpr double kMeterTauMs = 300.0;

    constexpr double kDriveDbPerPercent = 0.24;    // 100 % = +24 dB
    constexpr double kCompRefPeak = 0.25;          // -12 dBFS
    constexpr double kCompMax = 3.9810717055349722;   // +12 dB
    constexpr double kWarmMix = 0.5;
    constexpr double kTubeBias = 0.35;
    constexpr double kTubeAttackBias = 0.5;
    constexpr double kTriodeEven = 0.2;
    constexpr double kDualTriodeEven = 0.25;
    constexpr double kRetroDecayDrive = 1.0;
    constexpr double kTapeEmphasisHz = 4000.0;
    constexpr double kTapeEmphasisDb = 6.0;
    constexpr double kTapeHysteresis = 0.04;
    constexpr double kTapeSlopeRefHz = 4000.0;
    constexpr double kDeadZone = 0.1;
    constexpr double kFastMs = 3.0, kSlowReleaseMs = 300.0, kSlowAttackRefMs = 60.0;
    constexpr double kTiny = 1.0e-12;
    constexpr double kTwoPi = 6.283185307179586;

    double modelScale (ExciterMode m) noexcept { return m == ExciterMode::Triode ? 0.5 : 1.0; }

    double driveToK (ExciterMode m, double drivePercent) noexcept
    {
        const double d = std::clamp (drivePercent, 0.0, 100.0);
        return (std::pow (10.0, d * kDriveDbPerPercent / 20.0) - 1.0) * modelScale (m);
    }

    // The static curve without compensation (k > 0). retroBoost / tubeBias: the dynamic terms (0 for static input).
    inline double rawCurve (ExciterMode m, double x, double k, double retroBoost, double tubeBias) noexcept
    {
        switch (m)
        {
            case ExciterMode::Analog:
            {
                const double v = k * x;
                const double v2 = v * v;
                return v / std::sqrt (std::sqrt (1.0 + v2 * v2)) / k;
            }
            case ExciterMode::Retro:
            {
                const double kk = k * (1.0 + retroBoost);
                return std::tanh (kk * x) / kk;
            }
            case ExciterMode::Tape:
            {
                const double v = k * x;
                return v / std::sqrt (1.0 + v * v) / k;
            }
            case ExciterMode::Tube:
            {
                const double b = kTubeBias + tubeBias;
                const double tb = std::tanh (b);
                return (std::tanh (k * x + b) - tb) / (k * (1.0 - tb * tb));
            }
            case ExciterMode::Warm:
            {
                const double v = k * x;
                return x + kWarmMix * (std::sqrt (1.0 + v * v) - 1.0) / k;
            }
            case ExciterMode::Triode:
            {
                const double t = std::tanh (k * x);
                return (t + kTriodeEven * t * t) / k;
            }
            case ExciterMode::DualTriode:
            case ExciterMode::Count:
            default:
            {
                const double t1 = std::tanh (k * x);
                const double y1 = t1 + kDualTriodeEven * t1 * t1;
                const double t2 = std::tanh (y1);
                return (t2 + kDualTriodeEven * t2 * t2) / k;
            }
        }
    }

    // Level compensation table: comp[m][d] for drive d = 0..100 %.
    struct CompTable
    {
        float comp[kNumModes][101] {};

        CompTable()
        {
            constexpr int kPoints = 512;
            for (int m = 0; m < kNumModes; ++m)
            {
                comp[m][0] = 1.0f;
                for (int d = 1; d <= 100; ++d)
                {
                    const double k = driveToK ((ExciterMode) m, (double) d);
                    double sumIn = 0.0, sumOut = 0.0, sumOut2 = 0.0;
                    for (int i = 0; i < kPoints; ++i)
                    {
                        const double x = kCompRefPeak * std::sin (kTwoPi * ((double) i + 0.5) / (double) kPoints);
                        const double y = rawCurve ((ExciterMode) m, x, k, 0.0, 0.0);
                        sumIn += x * x;
                        sumOut += y;
                        sumOut2 += y * y;
                    }
                    const double mean = sumOut / kPoints;
                    const double acPower = std::max (1.0e-30, sumOut2 / kPoints - mean * mean);
                    const double c = std::sqrt ((sumIn / kPoints) / acPower);
                    comp[m][d] = (float) std::clamp (c, 1.0 / kCompMax, kCompMax);
                }
            }
        }
    };

    const CompTable& compTable() noexcept
    {
        static const CompTable table;
        return table;
    }

    double compFor (ExciterMode m, double drivePercent) noexcept
    {
        const double d = std::clamp (drivePercent, 0.0, 100.0);
        const int i = std::min (99, (int) d);
        const double f = d - (double) i;
        const auto& row = compTable().comp[std::clamp ((int) m, 0, kNumModes - 1)];
        return (double) row[i] + f * ((double) row[i + 1] - (double) row[i]);
    }

    double poleFor (double ms, double sr) noexcept
    {
        const double samples = ms * 0.001 * sr;
        return samples > 0.0 ? 1.0 - std::exp (-1.0 / samples) : 1.0;
    }

    // one-pole section (first-order biquad) in double
    struct FirstOrder
    {
        double b0 = 1.0, b1 = 0.0, a1 = 0.0, z = 0.0;
        void set (const BiquadCoeffs& c) noexcept { b0 = c.b0; b1 = c.b1; a1 = c.a1; }
        void setInverseOf (const BiquadCoeffs& c) noexcept
        {
            b0 = 1.0 / c.b0;
            b1 = c.a1 / c.b0;
            a1 = c.b1 / c.b0;
        }
        inline double process (double x) noexcept
        {
            const double y = b0 * x + z;
            z = b1 * x - a1 * y;
            return y;
        }
        void reset() noexcept { z = 0.0; }
    };

    struct TapeState
    {
        FirstOrder pre, de;
        double prevV = 0.0;
        void reset() noexcept
        {
            pre.reset();
            de.reset();
            prevV = 0.0;
        }
    };

    // Per-sample settings of one model.
    struct ModelRun
    {
        ExciterMode mode = ExciterMode::Warm;
        double k = 0.0, comp = 1.0;
    };

    struct ChanState
    {
        LinearSmoother drive, mix;
        float driveTarget = 0.0f, mixTarget = 1.0f;
        ExciterMode curMode = ExciterMode::Warm, nextMode = ExciterMode::Warm, wantedMode = ExciterMode::Warm;
        LinearSmoother fade;
        bool fading = false;

        double envFast = 0.0, envFast2 = 0.0, envSlowRel = 0.0, envSlowAtt = 0.0;
        TapeState tape[2];   // [0] = current model, [1] = model fading in
        Biquad dc;
        bool dcLive = false;
        bool nlRunning = false;
        double meterPower = 0.0;

        void resetEnvelopes() noexcept { envFast = envFast2 = envSlowRel = envSlowAtt = 0.0; }

        void resetTape() noexcept
        {
            tape[0].reset();
            tape[1].reset();
        }
    };

    struct BandState
    {
        Oversampler os;
        bool osFresh = true;
        int osFlush = 0;   // samples of silence still to push through the oversampler after the drive stopped
        DelayLine dryDelay[2];
        ChanState ch[2];
        LinearSmoother solo;
        bool nlRunning = false;
    };
} // namespace

//==============================================================================
struct ExciterEngine
{
    double sr = 48000.0;
    int maxBlock = 512;
    bool prepared = false;
    bool fresh = true;
    bool snapActive = true;

    Crossover xover;
    ExciterParams params;
    ModuleMonitor monitor;
    std::atomic<int> latency { 0 };
    int osLatency = 0;
    bool osOn = true;

    BandState bands[kMaxBands];
    LinearSmoother active;

    std::vector<float> bandBuf[kMaxBands][2];
    float* bandChans[kMaxBands][2] {};
    float* const* bandPtrs[kMaxBands] {};
    std::vector<float> diffBuf[2];
    std::vector<float> retroBuf[2], tubeBuf[2];
    std::vector<double> dryAcc[2], wetAcc[2];
    std::vector<float> neutralBuf[2];
    DelayLine neutralDelay[2];

    SmoothedBiquad shelf[2];
    bool shelfIdentityTarget = true;
    bool shelfIdle = true;
    bool lastPostOn = false;
    float lastPostHz = -1.0f, lastPostDb = -100.0f;

    BiquadCoeffs tapePreBase, tapePreOs;
    double aFast = 0, aSlowRel = 0, aSlowRef = 0;
    double slopeScaleBase = 1.0, slopeScaleOs = 1.0;
    double meterTau = 1.0;

    ExciterMeters meters;

    //==========================================================================
    void prepare (double sampleRate, int maxBlockSize)
    {
        (void) compTable();   // build the static table here, not on the audio thread

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

        auto& b0 = bands[0];
        b0.os.prepare (kOsLog2, Oversampler::Quality::LinearPhase, maxBlock, 2);
        osLatency = b0.os.getLatencySamples();

        const double osRate = sr * (double) (1 << kOsLog2);
        tapePreBase = design::highShelf1 (sr, std::min (kTapeEmphasisHz, 0.3 * sr), kTapeEmphasisDb);
        tapePreOs = design::highShelf1 (osRate, kTapeEmphasisHz, kTapeEmphasisDb);
        slopeScaleBase = sr / (kTwoPi * kTapeSlopeRefHz);
        slopeScaleOs = osRate / (kTwoPi * kTapeSlopeRefHz);

        for (auto& band : bands)
        {
            if (&band != &b0)
                band.os.prepare (kOsLog2, Oversampler::Quality::LinearPhase, maxBlock, 2);
            for (int c = 0; c < 2; ++c)
            {
                band.dryDelay[c].prepare (std::max (1, osLatency));
                auto& cs = band.ch[c];
                cs.drive.prepare (sr, kRampMs);
                cs.mix.prepare (sr, kRampMs);
                cs.fade.prepare (sr, kModeFadeMs);
                cs.dc.setCoeffs (design::highPass (sr, kDcHz, 0.7071067811865476));
            }
            band.solo.prepare (sr, kSoloRampMs);
        }

        for (int c = 0; c < 2; ++c)
        {
            diffBuf[c].assign ((size_t) maxBlock, 0.0f);
            retroBuf[c].assign ((size_t) maxBlock, 0.0f);
            tubeBuf[c].assign ((size_t) maxBlock, 0.0f);
            dryAcc[c].assign ((size_t) maxBlock, 0.0);
            wetAcc[c].assign ((size_t) maxBlock, 0.0);
            neutralBuf[c].assign ((size_t) maxBlock, 0.0f);
            neutralDelay[c].prepare (std::max (1, xover.getLatencySamples (CrossoverType::Digital) + osLatency));
            shelf[c].setRampLength ((int) std::lround (kShelfRampMs * 0.001 * sr));
        }
        active.prepare (sr, kNeutralFadeMs);

        aFast = poleFor (kFastMs, sr);
        aSlowRel = poleFor (kSlowReleaseMs, sr);
        aSlowRef = poleFor (kSlowAttackRefMs, sr);
        meterTau = kMeterTauMs * 0.001 * sr;

        applyTapeCoeffs();
        prepared = true;
        reset();
    }

    void applyTapeCoeffs() noexcept
    {
        const auto& pre = osOn ? tapePreOs : tapePreBase;
        for (auto& band : bands)
            for (auto& cs : band.ch)
                for (auto& t : cs.tape)
                {
                    t.pre.set (pre);
                    t.de.setInverseOf (pre);
                    t.reset();
                }
    }

    void reset() noexcept
    {
        xover.reset();
        for (auto& d : neutralDelay)
            d.reset();
        for (auto& band : bands)
        {
            band.os.reset();
            band.osFresh = true;
            band.osFlush = 0;
            band.nlRunning = false;
            for (int c = 0; c < 2; ++c)
            {
                band.dryDelay[c].reset();
                band.ch[c].resetEnvelopes();
                band.ch[c].resetTape();
                band.ch[c].dc.reset();
                band.ch[c].dcLive = false;
                band.ch[c].nlRunning = false;
                band.ch[c].meterPower = 0.0;
            }
        }
        for (auto& s : shelf)
            s.reset();
        for (int set = 0; set < kNumChannelSets; ++set)
            for (int b = 0; b < kMaxBands; ++b)
                meters.harmonicsDb[set][b].store (-120.0f);
        fresh = true;
        snapActive = true;
    }

    //==========================================================================
    void setParams (const ExciterParams& requested, const ExciterParams* topology = nullptr) noexcept
    {
        const int previousBands = params.numBands;
        params = requested;
        if (topology != nullptr)
        {
            params.numBands = topology->numBands;
            params.crossoverType = topology->crossoverType;
            params.oversampling = topology->oversampling;
        }
        // Removed bands must not retain an old delay/filter tail until they are re-added.
        for (int b = params.numBands; b < previousBands; ++b)
        {
            auto& band = bands[b];
            band.os.reset();
            band.osFresh = true;
            band.osFlush = 0;
            band.nlRunning = false;
            for (int c = 0; c < 2; ++c)
            {
                band.dryDelay[c].reset();
                auto& cs = band.ch[c];
                cs.resetEnvelopes();
                cs.resetTape();
                cs.dc.reset();
                cs.dcLive = false;
                cs.nlRunning = false;
                cs.meterPower = 0.0;
            }
        }
        params.numBands = std::clamp (params.numBands, 1, kMaxBands);
        xover.setup (params.numBands, params.crossoverHz, params.crossoverType);

        if (params.oversampling != osOn)
        {
            osOn = params.oversampling;
            for (auto& band : bands)
            {
                band.os.reset();
                band.osFresh = true;
                band.osFlush = 0;
            }
            applyTapeCoeffs();
        }

        const int osLat = osOn ? osLatency : 0;
        const int lat = xover.getLatencySamples (params.crossoverType) + osLat;
        latency.store (lat, std::memory_order_relaxed);
        for (auto& d : neutralDelay)
            d.setDelay (lat);
        for (auto& band : bands)
            for (auto& d : band.dryDelay)
                d.setDelay (osLat);

        // post filter
        const float hz = std::clamp (std::isfinite (params.postFilterHz) ? params.postFilterHz : 8000.0f, 1000.0f, 20000.0f);
        const float db = std::clamp (std::isfinite (params.postFilterGainDb) ? params.postFilterGainDb : 0.0f, -12.0f, 12.0f);
        if (fresh || params.postFilterOn != lastPostOn || hz != lastPostHz || db != lastPostDb)
        {
            lastPostOn = params.postFilterOn;
            lastPostHz = hz;
            lastPostDb = db;
            shelfIdentityTarget = ! params.postFilterOn || db == 0.0f;
            const BiquadCoeffs c = shelfIdentityTarget ? BiquadCoeffs {}
                                                       : design::highShelf (sr, std::min ((double) hz, 0.45 * sr), 0.7071067811865476, (double) db);
            for (auto& s : shelf)
            {
                s.setTarget (c);
                if (fresh)
                    s.snapToTarget();
            }
            if (! shelfIdentityTarget)
                shelfIdle = false;
        }

        applySoloTargets (fresh);
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

    void applyChannelTargets (bool dual) noexcept
    {
        for (int b = 0; b < kMaxBands; ++b)
        {
            for (int c = 0; c < 2; ++c)
            {
                const auto& bp = params.bands[dual ? c : 0][b];
                auto& cs = bands[b].ch[c];
                cs.driveTarget = std::clamp (std::isfinite (bp.drivePercent) ? bp.drivePercent : 0.0f, 0.0f, 100.0f);
                cs.mixTarget = bp.on ? std::clamp (std::isfinite (bp.mixPercent) ? bp.mixPercent : 0.0f, 0.0f, 100.0f) * 0.01f
                                     : 0.0f;
                cs.wantedMode = bp.mode;
                if (fresh)
                {
                    cs.drive.snap (cs.driveTarget);
                    cs.mix.snap (cs.mixTarget);
                    cs.curMode = cs.nextMode = cs.wantedMode;
                    cs.fading = false;
                    cs.fade.snap (0.0f);
                }
                else
                {
                    cs.drive.setTarget (cs.driveTarget);
                    cs.mix.setTarget (cs.mixTarget);
                }
            }
        }
        fresh = false;
    }

    static bool driveIdleZero (const ChanState& cs) noexcept
    {
        return cs.driveTarget == 0.0f && cs.drive.getCurrent() == 0.0f && ! cs.drive.isSmoothing();
    }

    static bool mixIdleZero (const ChanState& cs) noexcept
    {
        return cs.mixTarget == 0.0f && cs.mix.getCurrent() == 0.0f && ! cs.mix.isSmoothing();
    }

    bool isNeutral (bool dual) const noexcept
    {
        const bool postFlat = shelfIdle;
        for (int b = 0; b < params.numBands; ++b)
        {
            const auto& band = bands[b];
            if (band.solo.getCurrent() != 1.0f || band.solo.isSmoothing())
                return false;
            for (int c = 0; c < (dual ? 2 : 1); ++c)
            {
                const auto& cs = band.ch[c];
                if (! (mixIdleZero (cs) || (driveIdleZero (cs) && postFlat)))
                    return false;
            }
        }
        return true;
    }

    //==========================================================================
    static void finishFade (ChanState& cs) noexcept
    {
        cs.curMode = cs.nextMode = cs.wantedMode;
        cs.fading = false;
        cs.fade.snap (0.0f);
    }

    static void updateModeFade (ChanState& cs) noexcept
    {
        if (cs.fading && ! cs.fade.isSmoothing() && cs.fade.getCurrent() >= 1.0f)
        {
            cs.curMode = cs.nextMode;
            std::swap (cs.tape[0], cs.tape[1]);
            cs.fading = false;
            cs.fade.snap (0.0f);
        }
        if (! cs.fading && cs.wantedMode != cs.curMode)
        {
            cs.nextMode = cs.wantedMode;
            cs.tape[1].reset();
            cs.fading = true;
            cs.fade.snap (0.0f);
            cs.fade.setTarget (1.0f);
        }
    }

    // Envelope detectors (linked in Stereo: both channels see max (L^2, R^2)). Writes the dynamic terms per sample.
    void runDetectors (int b, int n, bool dual, bool needRetro, bool needTube) noexcept
    {
        auto& band = bands[b];
        const int numDet = dual ? 2 : 1;
        for (int c = 0; c < numDet; ++c)
        {
            auto& cs = band.ch[c];
            float* retro = retroBuf[c].data();
            float* tube = tubeBuf[c].data();
            const float* x0 = bandChans[b][0];
            const float* x1 = bandChans[b][1];
            for (int i = 0; i < n; ++i)
            {
                double p;
                if (dual)
                {
                    const double v = (double) (c == 0 ? x0[i] : x1[i]);
                    p = v * v;
                }
                else
                {
                    p = std::max ((double) x0[i] * (double) x0[i], (double) x1[i] * (double) x1[i]);
                }
                p += kTiny;
                cs.envFast2 += aFast * (p - cs.envFast2);
                cs.envFast += aFast * (cs.envFast2 - cs.envFast);   // two poles: little ripple on steady tones
                const double f = cs.envFast;
                cs.envSlowRel = f > cs.envSlowRel ? f : cs.envSlowRel + aSlowRel * (f - cs.envSlowRel);
                cs.envSlowAtt += aSlowRef * (f - cs.envSlowAtt);

                double dr = 0.0, da = 0.0;
                if (needRetro)
                {
                    const double r = 1.0 - std::sqrt (cs.envFast / cs.envSlowRel);
                    dr = std::clamp ((r - kDeadZone) / (1.0 - kDeadZone), 0.0, 1.0);
                }
                if (needTube)
                {
                    const double a = 1.0 - std::sqrt (cs.envSlowAtt / cs.envFast);
                    da = std::clamp ((a - kDeadZone) / (1.0 - kDeadZone), 0.0, 1.0);
                }
                retro[i] = (float) dr;
                tube[i] = (float) da;
            }
        }
        if (! dual)
        {
            // Preserve the linked detector history for a later Stereo -> Left/Right switch.
            // Otherwise set B starts from silence and invents a Tube attack / Retro decay.
            band.ch[1].envFast = band.ch[0].envFast;
            band.ch[1].envFast2 = band.ch[0].envFast2;
            band.ch[1].envSlowRel = band.ch[0].envSlowRel;
            band.ch[1].envSlowAtt = band.ch[0].envSlowAtt;
            std::copy (retroBuf[0].begin(), retroBuf[0].begin() + n, retroBuf[1].begin());
            std::copy (tubeBuf[0].begin(), tubeBuf[0].begin() + n, tubeBuf[1].begin());
        }
    }

    // One model's output for input u (at the processing rate).
    inline double modelSample (const ModelRun& r, TapeState& tape, double u, double retroBoost, double tubeBias,
                               double slopeScale) const noexcept
    {
        if (r.mode == ExciterMode::Tape)
        {
            const double pre = tape.pre.process (u);
            const double v = r.k * pre;
            const double hAmt = kTapeHysteresis * (r.k * r.k) / (1.0 + r.k * r.k);
            const double h = hAmt * std::tanh ((v - tape.prevV) * slopeScale);
            tape.prevV = v;
            const double vv = v + h;
            const double y = vv / std::sqrt (1.0 + vv * vv) / r.k;
            return r.comp * tape.de.process (y);
        }
        return r.comp * rawCurve (r.mode, u, r.k, kRetroDecayDrive * retroBoost, kTubeAttackBias * tubeBias);
    }

    // Writes D = f(u) - u for channel c of band b into data (n * factor samples at the processing rate).
    void shapeChannel (int b, int c, float* data, int n, int factor) noexcept
    {
        auto& cs = bands[b].ch[c];
        const float* retro = retroBuf[c].data();
        const float* tube = tubeBuf[c].data();
        const double slopeScale = factor > 1 ? slopeScaleOs : slopeScaleBase;

        ModelRun cur, next;
        cur.mode = cs.curMode;
        next.mode = cs.nextMode;
        float lastDrive = -1.0f;

        for (int i = 0; i < n; ++i)
        {
            const float drive = cs.drive.next();
            const double fade = cs.fading ? (double) cs.fade.next() : 0.0;
            if (drive != lastDrive)
            {
                lastDrive = drive;
                cur.k = driveToK (cur.mode, (double) drive);
                cur.comp = compFor (cur.mode, (double) drive);
                if (cs.fading)
                {
                    next.k = driveToK (next.mode, (double) drive);
                    next.comp = compFor (next.mode, (double) drive);
                }
            }

            float* d = data + (size_t) i * (size_t) factor;
            if (cur.k <= 1.0e-9 && next.k <= 1.0e-9)
            {
                for (int j = 0; j < factor; ++j)
                    d[j] = 0.0f;
                continue;
            }

            const double dr = (double) retro[i], da = (double) tube[i];
            for (int j = 0; j < factor; ++j)
            {
                const double u = (double) d[j];
                double y = cur.k > 1.0e-9 ? modelSample (cur, cs.tape[0], u, dr, da, slopeScale) : u;
                if (cs.fading)
                {
                    const double y2 = next.k > 1.0e-9 ? modelSample (next, cs.tape[1], u, dr, da, slopeScale) : u;
                    y += fade * (y2 - y);
                }
                d[j] = (float) (y - u);
            }
        }
    }

    void processBand (int b, int n, bool dual) noexcept
    {
        auto& band = bands[b];

        // which channels need the nonlinear path
        bool need[2] {};
        bool any = false, needRetro = false, needTube = false;
        for (int c = 0; c < 2; ++c)
        {
            auto& cs = band.ch[c];
            updateModeFade (cs);
            need[c] = ! driveIdleZero (cs) && ! mixIdleZero (cs);
            if (need[c] && ! cs.nlRunning)
                cs.resetTape();
            cs.nlRunning = need[c];
            any = any || need[c];
            for (ExciterMode m : { cs.curMode, cs.nextMode })
            {
                needRetro = needRetro || m == ExciterMode::Retro;
                needTube = needTube || m == ExciterMode::Tube;
            }
        }

        // dry, aligned with the oversampled path
        float* dry[2] { bandChans[b][0], bandChans[b][1] };

        // When the nonlinear path stops, the oversampler is run on silence for a little longer so the harmonics still
        // inside its down-sampling filter come out (no cut); the DC blocker likewise rings out instead of being reset.
        const bool flushing = ! any && osOn && band.osFlush > 0;
        const bool runOs = osOn && (any || flushing);

        // the detectors always run (a band that starts processing must not see a fake attack)
        runDetectors (b, n, dual, any && needRetro, any && needTube);
        if (any)
        {
            if (! band.nlRunning)
                for (auto& cs : band.ch)
                    cs.resetTape();
            band.nlRunning = true;
        }

        if (runOs)
        {
            if (band.osFresh)
            {
                band.os.reset();
                band.osFresh = false;
            }
            const float* src[2] { bandChans[b][0], bandChans[b][1] };
            float* const* up = band.os.upsample (src, n);
            const int factor = band.os.getFactor();
            for (int c = 0; c < 2; ++c)
            {
                if (need[c])
                    shapeChannel (b, c, up[c], n, factor);
                else
                    std::fill (up[c], up[c] + (size_t) n * (size_t) factor, 0.0f);
            }
            float* outs[2] { diffBuf[0].data(), diffBuf[1].data() };
            band.os.downsample (outs, n);
        }
        else if (any)
        {
            for (int c = 0; c < 2; ++c)
            {
                std::copy (bandChans[b][c], bandChans[b][c] + n, diffBuf[c].data());
                if (need[c])
                    shapeChannel (b, c, diffBuf[c].data(), n, 1);
            }
        }

        if (any)
            band.osFlush = 2 * osLatency + 16;
        else if (flushing)
            band.osFlush -= n;
        if (! any && ! flushing)
        {
            band.nlRunning = false;
            band.osFresh = true;
            band.osFlush = 0;
        }

        bool live = runOs;
        for (int c = 0; c < 2; ++c)
        {
            auto& cs = band.ch[c];
            if (! need[c])
            {
                // keep the drive ramp state consistent; a model change is inaudible at drive 0
                for (int i = 0; i < n; ++i)
                    (void) cs.drive.next();
                if (cs.fading)
                    finishFade (cs);
                if (! runOs)
                    std::fill (diffBuf[c].begin(), diffBuf[c].begin() + n, 0.0f);
            }

            if (need[c] || runOs)
            {
                cs.dc.process (diffBuf[c].data(), n);
                cs.dcLive = true;
                live = true;
            }
            else if (cs.dcLive)
            {
                cs.dc.process (diffBuf[c].data(), n);
                live = true;
                float peak = 0.0f;
                for (int i = 0; i < n; ++i)
                    peak = std::max (peak, std::abs (diffBuf[c][(size_t) i]));
                if (peak < 1.0e-9f)
                {
                    cs.dc.reset();
                    cs.dcLive = false;
                }
            }
        }

        for (int c = 0; c < 2; ++c)
            band.dryDelay[c].process (dry[c], n);

        // meters (RMS of wet - dry, before the mix)
        const double decay = std::exp (-(double) n / meterTau);
        for (int c = 0; c < 2; ++c)
        {
            double acc = 0.0;
            if (live)
                for (int i = 0; i < n; ++i)
                    acc += (double) diffBuf[c][(size_t) i] * (double) diffBuf[c][(size_t) i];
            auto& cs = band.ch[c];
            cs.meterPower = cs.meterPower * decay + acc * (1.0 - decay) / (double) std::max (1, n);
            if (cs.meterPower < 1.0e-30)
                cs.meterPower = 0.0;
        }
        if (dual)
        {
            for (int c = 0; c < 2; ++c)
                meters.harmonicsDb[c][b].store (std::max (-120.0f, (float) (10.0 * std::log10 (band.ch[c].meterPower + 1.0e-14))));
        }
        else
        {
            const double p = 0.5 * (band.ch[0].meterPower + band.ch[1].meterPower);
            meters.harmonicsDb[0][b].store (std::max (-120.0f, (float) (10.0 * std::log10 (p + 1.0e-14))));
            meters.harmonicsDb[1][b].store (-120.0f);
        }

        // mix
        auto& c0 = band.ch[0];
        auto& c1 = band.ch[1];
        for (int i = 0; i < n; ++i)
        {
            const double g = (double) band.solo.next();
            const double m0 = (double) c0.mix.next();
            const double m1 = (double) c1.mix.next();
            const double d0 = (double) dry[0][i], d1 = (double) dry[1][i];
            dryAcc[0][(size_t) i] += g * (1.0 - m0) * d0;
            dryAcc[1][(size_t) i] += g * (1.0 - m1) * d1;
            wetAcc[0][(size_t) i] += g * m0 * (d0 + (double) diffBuf[0][(size_t) i]);
            wetAcc[1][(size_t) i] += g * m1 * (d1 + (double) diffBuf[1][(size_t) i]);
        }
    }

    void idleBand (int b, int n, bool dual) noexcept
    {
        runDetectors (b, n, dual, false, false);
        auto& band = bands[b];
        band.nlRunning = false;
        band.osFresh = true;
        band.osFlush = 0;
        for (int c = 0; c < 2; ++c)
        {
            band.dryDelay[c].process (bandChans[b][c], n);
            auto& cs = band.ch[c];
            cs.drive.snap (cs.driveTarget);
            cs.mix.snap (cs.mixTarget);
            finishFade (cs);
            cs.meterPower = 0.0;
            cs.dc.reset();
            cs.dcLive = false;
            cs.nlRunning = false;
        }
        band.solo.snap (band.solo.getCurrent());
        meters.harmonicsDb[0][b].store (-120.0f);
        meters.harmonicsDb[1][b].store (-120.0f);
    }

    //==========================================================================
    void processChunk (float* const* ch, int n, bool dual) noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            std::copy (ch[c], ch[c] + n, neutralBuf[c].data());
            neutralDelay[c].process (neutralBuf[c].data(), n);
        }

        const float* in[2] { ch[0], ch[1] };
        xover.process (in, bandPtrs, n);
        const int numBands = std::min (params.numBands, xover.getNumBands());

        if (shelfIdentityTarget && ! shelf[0].isRamping() && ! shelfIdle)
        {
            shelfIdle = true;
            for (auto& s : shelf)
                s.reset();
        }

        const bool neutral = isNeutral (dual);
        if (snapActive)
            active.snap (neutral ? 0.0f : 1.0f);
        else
            active.setTarget (neutral ? 0.0f : 1.0f);
        snapActive = false;

        if (! active.isSmoothing() && active.getCurrent() == 0.0f)
        {
            for (int b = 0; b < numBands; ++b)
                idleBand (b, n, dual);
            for (int c = 0; c < 2; ++c)
                std::copy (neutralBuf[c].data(), neutralBuf[c].data() + n, ch[c]);
            clearUnusedMeters (numBands);
            return;
        }

        for (int c = 0; c < 2; ++c)
        {
            std::fill (dryAcc[c].begin(), dryAcc[c].begin() + n, 0.0);
            std::fill (wetAcc[c].begin(), wetAcc[c].begin() + n, 0.0);
        }

        for (int b = 0; b < numBands; ++b)
            processBand (b, n, dual);

        if (! shelfIdle)
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    wetAcc[c][(size_t) i] = shelf[c].processSample (wetAcc[c][(size_t) i]);

        if (! active.isSmoothing() && active.getCurrent() == 1.0f)
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    ch[c][i] = (float) (dryAcc[c][(size_t) i] + wetAcc[c][(size_t) i]);
        }
        else
        {
            for (int i = 0; i < n; ++i)
            {
                const double a = (double) active.next();
                for (int c = 0; c < 2; ++c)
                {
                    const double d = (double) neutralBuf[c][(size_t) i];
                    const double y = dryAcc[c][(size_t) i] + wetAcc[c][(size_t) i];
                    ch[c][i] = (float) (d + a * (y - d));
                }
            }
        }
        clearUnusedMeters (numBands);
    }

    void clearUnusedMeters (int numBands) noexcept
    {
        for (int b = numBands; b < kMaxBands; ++b)
        {
            meters.harmonicsDb[0][b].store (-120.0f);
            meters.harmonicsDb[1][b].store (-120.0f);
        }
    }
};

// A latency or band-count change changes the whole signal path, including its dry delay.
// Keep two preallocated engines warm and fade their complete outputs. Merely fading the
// nonlinear residue leaves an instantaneous dry tap jump; resetting the only engine also
// cuts its filter tails. Rapid topology requests are queued until the current fade ends.
struct Exciter::Impl
{
    ExciterEngine engines[2];
    std::vector<float> alternate[2];
    LinearSmoother topologyFade;
    ExciterParams topology[2], requested;
    std::atomic<int> latency { 0 };
    ExciterMeters meters;
    int current = 0, maxBlock = 512, fadeLength = 960, fadeRemaining = 0;
    bool prepared = false, initial = true, fading = false, configurePending = false;

    static bool sameTopology (const ExciterParams& a, const ExciterParams& b) noexcept
    {
        return a.numBands == b.numBands && a.crossoverType == b.crossoverType
               && a.oversampling == b.oversampling;
    }

    void prepare (double sampleRate, int maxBlockSize)
    {
        maxBlock = std::max (1, maxBlockSize);
        for (auto& engine : engines)
            engine.prepare (sampleRate, maxBlock);
        for (auto& buffer : alternate)
            buffer.assign ((size_t) maxBlock, 0.0f);
        const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        topologyFade.prepare (rate, kModeFadeMs);
        fadeLength = std::max (1, (int) std::lround (rate * kModeFadeMs * 0.001));
        prepared = true;
        reset();
    }

    void reset() noexcept
    {
        const bool hadSettings = ! initial;
        for (auto& engine : engines)
            engine.reset();
        current = 0;
        initial = true;
        fading = false;
        configurePending = false;
        fadeRemaining = 0;
        topologyFade.snap (0.0f);
        for (auto& set : meters.harmonicsDb)
            for (auto& meter : set)
                meter.store (-120.0f);
        // reset clears history, not the last requested configuration. Engine 0
        // may still have the old topology when a fade has just reached engine 1.
        if (hadSettings)
        {
            configureEngines();
            const int osLat = requested.oversampling ? engines[0].osLatency : 0;
            latency.store (engines[0].xover.getLatencySamples (requested.crossoverType) + osLat,
                           std::memory_order_relaxed);
        }
    }

    void setParams (ParamValues values, int parameterSet) noexcept
    {
        readExciterParams (values, requested);
        if (parameterSet == 1)   // T/S sustain twin: set B for both channels (Module.h)
            for (int b = 0; b < kMaxBands; ++b)
                requested.bands[0][b] = requested.bands[1][b];
        requested.numBands = std::clamp (requested.numBands, 1, kMaxBands);
        configureEngines();
        // As for Crossover, report the most recently requested destination latency.
        const int osLat = requested.oversampling ? engines[0].osLatency : 0;
        latency.store (engines[0].xover.getLatencySamples (requested.crossoverType) + osLat,
                       std::memory_order_relaxed);
    }

    void configureEngines() noexcept
    {
        configurePending = false;
        if (initial)
        {
            topology[0] = topology[1] = requested;
            initial = false;
        }
        else if (! fading && ! sameTopology (requested, topology[current]))
        {
            topology[1 - current] = requested;
            topologyFade.snap (0.0f);
            topologyFade.setTarget (1.0f);
            fadeRemaining = fadeLength;
            fading = true;
        }
        else if (! fading)
        {
            topology[1 - current] = topology[current];
        }

        for (int e = 0; e < 2; ++e)
            engines[e].setParams (requested, &topology[e]);
    }

    void setMonitor (const ModuleMonitor& monitor) noexcept
    {
        for (auto& engine : engines)
        {
            engine.monitor = monitor;
            engine.applySoloTargets (false);
        }
    }

    void process (float* const* channels, int numSamples, bool dual) noexcept
    {
        for (auto& engine : engines)
            engine.applyChannelTargets (dual);
        for (int offset = 0; offset < numSamples;)
        {
            // A queued topology request must be consumed even when the caller does
            // not send setParams again after the fade. Store the latest full settings.
            if (configurePending)
            {
                configureEngines();
                for (auto& engine : engines)
                    engine.applyChannelTargets (dual);
            }
            const int n = std::min (std::min (maxBlock, numSamples - offset),
                                    fading ? fadeRemaining : maxBlock);
            float* main[2] { channels[0] + offset, channels[1] + offset };
            float* other[2] { alternate[0].data(), alternate[1].data() };
            for (int c = 0; c < 2; ++c)
                std::copy (main[c], main[c] + n, other[c]);
            engines[current].processChunk (main, n, dual);
            engines[1 - current].processChunk (other, n, dual);
            if (fading)
            {
                for (int i = 0; i < n; ++i)
                {
                    const double phase = topologyFade.next();
                    // Zero endpoint slope masks the short FIR/filter refill after a
                    // repartition and avoids a derivative step at either endpoint.
                    const double f = 0.5 - 0.5 * std::cos (0.5 * kTwoPi * phase);
                    for (int c = 0; c < 2; ++c)
                        main[c][i] = (float) ((double) main[c][i] + f * ((double) other[c][i] - main[c][i]));
                }
                fadeRemaining -= n;
                if (! topologyFade.isSmoothing())
                {
                    current = 1 - current;
                    fading = false;
                    configurePending = true;
                }
            }
            offset += n;
        }
        const auto& source = engines[fading ? 1 - current : current].meters;
        for (int set = 0; set < kNumChannelSets; ++set)
            for (int b = 0; b < kMaxBands; ++b)
                meters.harmonicsDb[set][b].store (source.harmonicsDb[set][b].load());
    }
};

//==============================================================================
Exciter::Exciter() : impl (std::make_unique<Impl>()) {}
Exciter::~Exciter() = default;

void Exciter::prepare (double sampleRate, int maxBlockSize) { impl->prepare (sampleRate, maxBlockSize); }
void Exciter::reset() noexcept { impl->reset(); }

void Exciter::setParams (ParamValues v) noexcept
{
    if (v != nullptr)
        impl->setParams (v, getParamSet());
}

void Exciter::setMonitor (const ModuleMonitor& m) noexcept
{
    impl->setMonitor (m);
}

void Exciter::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    auto& m = *impl;
    if (! m.prepared || channels == nullptr || channels[0] == nullptr || channels[1] == nullptr || numSamples <= 0)
        return;

    ScopedFlushDenormals noDenormals;
    const bool dual = ctx.channelMode == ChannelMode::MidSide || ctx.channelMode == ChannelMode::LeftRight;
    m.process (channels, numSamples, dual);
}

int Exciter::getLatencySamples() const noexcept { return impl->latency.load (std::memory_order_relaxed); }
const ExciterMeters& Exciter::getMeters() const noexcept { return impl->meters; }

float Exciter::transferCurve (ExciterMode mode, float x, float drivePercent) noexcept
{
    if (! std::isfinite (x))
        return 0.0f;
    const double d = std::clamp (std::isfinite (drivePercent) ? (double) drivePercent : 0.0, 0.0, 100.0);
    const double k = driveToK (mode, d);
    if (k <= 1.0e-9)
        return x;
    return (float) (compFor (mode, d) * rawCurve (mode, (double) x, k, 0.0, 0.0));
}
} // namespace ps
