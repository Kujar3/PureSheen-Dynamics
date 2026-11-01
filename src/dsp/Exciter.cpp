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
//   wet_b = HP14 (dry_b + D_b)                                      (= HP14 (f (band_b)) where the oversampler is flat)
//   out   = sum_b solo_b [ (1 - mix_b) dry_b ] + PostShelf ( sum_b solo_b mix_b wet_b )
// Computing f(u) - u in the oversampled domain and adding it to the delayed band keeps the dry part bit-exact (drive 0
// is exactly transparent, nothing above 0.45 fs is lost) and the nonlinearity still runs at 4x. Each band's wet signal
// dry + D goes through a first-order high pass at 14.1 Hz (the reference's wet path has exactly this one; it also
// removes the DC of the asymmetric curves) while the band is processed. When a band stops driving, the oversampler is
// flushed with silence (no state is cut mid-signal).
//
// Curves: measured from the reference (out/importer/tools/gen_exc.py, tone test, one band, mix 100 %): the static
// transfer curve of each model at the reference's amounts 3, 7.5 and 15 (its maximum), which are our drive 20 / 50 /
// 100 %. Each table holds y (x) on kExcCurvePoints points over |x| <= 0.631 (the loudest test sine, -4 dBFS) with
// y (0) = 0 and continues linearly with its end slope beyond. Between anchors the curves are interpolated linearly in
// drive (drive 0 = the identity). There is no level compensation: like the reference, more drive saturates louder
// signals harder and most models lose level (Tube at 100 % is close to a hard clip at -19 dBFS). Measured residual
// (waveform error re the sine amplitude over the four test levels): Analog / Retro / Tube / Warm, and Triode / Dual
// Triode at low drive, about 1..4 %; Tape 2..13 % and Triode / Dual Triode at full drive up to 17 %: there the
// reference also moves its operating point with the level (a bias shift) and has hysteresis, which a static curve
// cannot follow. Below ~200 Hz the reference filters the harmonics of Warm / Triode / Dual Triode / Tape more than its
// 14.1 Hz wet high pass (not modelled).
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
    constexpr double kMeterTauMs = 300.0;

    constexpr double kWetHighPassHz = 14.1;
    constexpr double kTwoPi = 6.283185307179586;

#include "dsp/ExciterCurves.inc"

    // Evaluates one measured table at x: linear between points, linear with the end slope beyond the table.
    inline double tableAt (const float* t, double x) noexcept
    {
        constexpr double scale = (double) (kExcCurvePoints - 1) / (2.0 * kExcCurveXMax);
        constexpr int last = kExcCurvePoints - 1;
        const double pos = (x + kExcCurveXMax) * scale;
        if (pos <= 0.0)
            return (double) t[0] + ((double) t[1] - (double) t[0]) * pos;
        if (pos >= (double) last)
            return (double) t[last] + ((double) t[last] - (double) t[last - 1]) * (pos - (double) last);
        const int i = (int) pos;
        const double f = pos - (double) i;
        return (double) t[i] + f * ((double) t[i + 1] - (double) t[i]);
    }

    // The two curves around a drive and the weight between them; lo == nullptr is the identity (drive 0).
    struct CurvePick
    {
        const float* lo = nullptr;
        const float* hi = nullptr;
        double w = 0.0;
    };

    CurvePick pickCurves (ExciterMode m, double drivePercent) noexcept
    {
        CurvePick c;
        const double d = std::clamp (drivePercent, 0.0, 100.0);
        if (! (d > 0.0))
            return c;
        const int mi = std::clamp ((int) m, 0, kNumModes - 1);
        double below = 0.0;
        for (int a = 0; a < 3; ++a)
        {
            if (d <= kExcAnchorDrive[a] || a == 2)
            {
                c.lo = a > 0 ? kExcCurve[mi][a - 1] : nullptr;
                c.hi = kExcCurve[mi][a];
                c.w = std::clamp ((d - below) / (kExcAnchorDrive[a] - below), 0.0, 1.0);
                return c;
            }
            below = kExcAnchorDrive[a];
        }
        return c;
    }

    inline double curveSample (const CurvePick& c, double x) noexcept
    {
        if (c.hi == nullptr)
            return x;
        const double a = c.lo != nullptr ? tableAt (c.lo, x) : x;
        return a + c.w * (tableAt (c.hi, x) - a);
    }

    // Per-sample settings of one model.
    struct ModelRun
    {
        ExciterMode mode = ExciterMode::Warm;
        CurvePick curve;
    };

    struct ChanState
    {
        LinearSmoother drive, mix;
        float driveTarget = 0.0f, mixTarget = 1.0f;
        ExciterMode curMode = ExciterMode::Warm, nextMode = ExciterMode::Warm, wantedMode = ExciterMode::Warm;
        LinearSmoother fade;
        bool fading = false;

        Biquad dc;   // the wet high pass (on dry + D)
        bool nlRunning = false;
        double meterPower = 0.0;
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
    std::vector<double> dryAcc[2], wetAcc[2];
    std::vector<float> neutralBuf[2];
    DelayLine neutralDelay[2];

    SmoothedBiquad shelf[2];
    bool shelfIdentityTarget = true;
    bool shelfIdle = true;
    bool lastPostOn = false;
    float lastPostHz = -1.0f, lastPostDb = -100.0f;

    double meterTau = 1.0;

    ExciterMeters meters;

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

        auto& b0 = bands[0];
        b0.os.prepare (kOsLog2, Oversampler::Quality::LinearPhase, maxBlock, 2);
        osLatency = b0.os.getLatencySamples();

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
                cs.dc.setCoeffs (design::highPass1 (sr, kWetHighPassHz));
            }
            band.solo.prepare (sr, kSoloRampMs);
        }

        for (int c = 0; c < 2; ++c)
        {
            diffBuf[c].assign ((size_t) maxBlock, 0.0f);
            dryAcc[c].assign ((size_t) maxBlock, 0.0);
            wetAcc[c].assign ((size_t) maxBlock, 0.0);
            neutralBuf[c].assign ((size_t) maxBlock, 0.0f);
            neutralDelay[c].prepare (std::max (1, xover.getLatencySamples (CrossoverType::Digital) + osLatency));
            shelf[c].setRampLength ((int) std::lround (kShelfRampMs * 0.001 * sr));
        }
        active.prepare (sr, kNeutralFadeMs);

        meterTau = kMeterTauMs * 0.001 * sr;

        prepared = true;
        reset();
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
                band.ch[c].dc.reset();
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
                cs.dc.reset();
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
            cs.fading = false;
            cs.fade.snap (0.0f);
        }
        if (! cs.fading && cs.wantedMode != cs.curMode)
        {
            cs.nextMode = cs.wantedMode;
            cs.fading = true;
            cs.fade.snap (0.0f);
            cs.fade.setTarget (1.0f);
        }
    }

    // Writes D = f(u) - u for channel c of band b into data (n * factor samples at the processing rate).
    void shapeChannel (int b, int c, float* data, int n, int factor) noexcept
    {
        auto& cs = bands[b].ch[c];

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
                cur.curve = pickCurves (cur.mode, (double) drive);
                if (cs.fading)
                    next.curve = pickCurves (next.mode, (double) drive);
            }

            float* d = data + (size_t) i * (size_t) factor;
            if (cur.curve.hi == nullptr && next.curve.hi == nullptr)
            {
                for (int j = 0; j < factor; ++j)
                    d[j] = 0.0f;
                continue;
            }

            for (int j = 0; j < factor; ++j)
            {
                const double u = (double) d[j];
                double y = curveSample (cur.curve, u);
                if (cs.fading)
                {
                    const double y2 = curveSample (next.curve, u);
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
        bool any = false;
        for (int c = 0; c < 2; ++c)
        {
            auto& cs = band.ch[c];
            updateModeFade (cs);
            need[c] = ! driveIdleZero (cs) && ! mixIdleZero (cs);
            cs.nlRunning = need[c];
            any = any || need[c];
        }

        // dry, aligned with the oversampled path
        float* dry[2] { bandChans[b][0], bandChans[b][1] };

        // When the nonlinear path stops, the oversampler is run on silence for a little longer so the harmonics still
        // inside its down-sampling filter come out (no cut); the DC blocker likewise rings out instead of being reset.
        const bool flushing = ! any && osOn && band.osFlush > 0;
        const bool runOs = osOn && (any || flushing);

        if (any)
            band.nlRunning = true;

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

        bool live = runOs;   // the meters read D (the harmonics and level change, before the wet high pass)
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

            live = live || need[c];
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

        // the wet path's high pass (14.1 Hz, first order) acts on the whole wet signal dry + D, as in the reference;
        // it runs for as long as the band is processed (the module's neutral fade covers switching it in and out)
        for (int c = 0; c < 2; ++c)
        {
            auto& hp = band.ch[c].dc;
            float* d = diffBuf[c].data();
            const float* x = dry[c];
            for (int i = 0; i < n; ++i)
            {
                const double xd = (double) x[i];
                d[i] = (float) (hp.processSample (xd + (double) d[i]) - xd);
            }
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
        (void) dual;
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
    return (float) curveSample (pickCurves (mode, d), (double) x);
}
} // namespace ps
