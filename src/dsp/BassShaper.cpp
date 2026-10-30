#include "dsp/BassShaper.h"

#include "dsp/Envelope.h"
#include "dsp/Oversampler.h"
#include "dsp/Quadrature.h"

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_BASS_HAS_SSE 1
#endif

// Bass Shaper (dsp-bass). Contract and signal flow: BassShaper.h. Choices made here (the header leaves them open):
//
// - Split: a private 2-band Linkwitz-Riley 24 dB/oct split (two trapezoidal state-variable Butterworth sections per
//   band, double precision; LP^2 + HP^2 = the 2nd-order all-pass at the cutoff, exactly, so low + high is flat and the
//   bands are in phase). It behaves exactly like ps::Crossover's Analog mode with 2 bands, but without the two unused
//   splits ps::Crossover always runs (their all-passes at 1 and 6 kHz would rotate the phase of the mids and highs of
//   a module that only works below 300 Hz) and at a third of its cost. The cutoff glides in the log domain (20 ms time
//   constant, coefficients per sample; the SVF stays transient-free under modulation).
// - Neutral switch: the whole processing always runs (warm states, meters work while every section is off). At
//   neutral settings the processed signal is the split's all-pass of the input, which is -180 degrees at the cutoff,
//   so a plain crossfade with the exact delay (what ps::Crossover does for 1 <-> N bands) would null a tone at the
//   cutoff halfway through (and dip 2.75 dB an octave either side). Instead the transition is phase-continuous:
//   x_D fades (10 ms) into d = AP (x_D), a 2nd-order all-pass parked at 8 Hz (the fade dips at most 0.2 dB at 40 Hz,
//   1.6 dB at 20 Hz, for a few ms); d's all-pass then glides to the cutoff at a speed proportional to its frequency
//   (0.25 ln-units per second per Hz, i.e. ~0.11 e-folds per own group delay; constant below 25 Hz), where it equals
//   the split's sum; then d fades (20 ms) into the processed signal. A time-varying all-pass is not exactly
//   magnitude-flat: tones stay within +-0.65 dB during the glide (measured). Engaging takes 0.3-0.36 s before the new
//   settings are fully heard; returning starts once the controls have been neutral and every smoother has settled
//   for 250 ms (automation that only passes through 0 does not toggle it) and runs the steps backwards.
// - Dirty sustain: the header's normalisation u = v / A would let u^3 explode for a sustain far above the threshold
//   (thr -40 dB, a -6 dBFS bass: u = 50). We normalise by A_eff = max (A, hold), hold = a peak hold of the lifted
//   sustain (max over both channels, held 30 ms - re-armed by any peak within 0.01 dB of it - then released with a
//   300 ms one-pole), so |u| <= 1 always: at or below the threshold this is exactly the header's formula, above it
//   the harmonic ratio stays constant (-16.5 dBc 2nd, -30.5 dBc 3rd for a sine at character 0). A steady tone keeps A_eff constant (the polynomial stays order 3, no
//   aliasing); only the very first rising quarter cycle of an onset follows |v| (a short rectifier, harmonics falling
//   at 12 dB / octave: < -90 dB at Nyquist for bass).
// - Transient mask detector: the header's quadrature power, but max (quadrature power, m^2). The elliptic all-pass
//   pair lags ~10 ms at 55 Hz (measured: 50 % of a step after 508 samples at 48 kHz), far more than the 1 ms
//   lookahead, so the first ~10 ms of every kick would count as sustain (no punch on the strongest part, and the
//   sustain lift would raise it). m^2 never exceeds the squared envelope of a steady tone (no ripple is added), but
//   it reaches the envelope within a quarter cycle of an onset. The slow envelope Ps keeps the quadrature power alone
//   (its lag only delays its 80 ms rise, which keeps the first ~20 ms of a hit fully transient). Both quadrature
//   detectors run at 24..48 kHz (every 2nd sample at 44.1 / 48 kHz, 4th at 88.2 / 96 kHz, 8th above): the LR4 low band
//   has no content up there to alias (< -100 dB above 6 kHz).
// - Sustain / peak sections fade on and off (section enable); thresholds, amount, characters, the mix, balance and
//   punch are smoothed by a critically damped two-stage one-pole (6 ms per stage: 90 % after ~23 ms), which has no kink
//   (a linear 20 ms ramp of a 24 dB balance jump puts a corner into the waveform). The sustain threshold moves both
//   the gain computer and A.
// - Clean peak limiter: hold = sliding max over L2 + 1 samples of ln (|x| / thr), released (one-pole on the reduction,
//   50 ms), box average of exp (-hold) over L2 + 1 samples, applied to the low part delayed by L2: provably <= the
//   threshold (the threshold of the sample's own input time).
// - Dirty clipper: per channel, at 4x (ps::Oversampler MinimumPhase, 5 samples), linear up to thr - 0.25 dB, a
//   quadratic knee (slope continuous) that reaches the threshold at thr + 0.24 dB, flat above. The oversampler idles
//   while the dirty branch cannot be heard (peak off, or character 100); when it is needed again it restarts from
//   silence and runs 5 ms unheard while the peak enable / character smoothers wait.
// - Auto gain compares the input delayed by D with the output (3 s mean squares); the gain glides (1 s one-pole in
//   dB, evaluated per 64-sample chunk and ramped linearly) and returns to unity with 150 ms while the module goes
//   back to its neutral passthrough, where it is exactly 1.
// - Meters: crest = 20 log10 (peak held 1 s, then decaying with a 1 s time constant) - 10 log10 (mean square, 1 s
//   one-pole). Peak = max (|L|, |R|), power = (L^2 + R^2) / 2. The output meters, scope, mask envelope and lowPeakDb
//   read the low part of the output BEFORE auto gain (so they line up with the thresholds drawn on the scope).
namespace ps
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kSqrt2 = 1.4142135623730951;
    constexpr int kChunk = 64;            // internal processing chunk (oversampler block)
    constexpr int kScopeDecim = 16;       // samples per scope / mask FIFO pair
    constexpr double kSmoothStageMs = 6.0;   // per stage of Smooth2: 90 % of a step after ~23 ms, settled < 60 ms
    constexpr double kLn10Over20 = 0.11512925464970229;   // ln (10) / 20
    constexpr double kTenOverLn10 = 4.3429448190325175;   // 10 / ln (10)
    constexpr double kParkHz = 8.0;           // d's all-pass when parked
    constexpr double kGlideKappa = 0.25;      // glide speed: ln-units per second per Hz of the all-pass
    constexpr double kGlideFloorHz = 25.0;    // below this, the speed of a kGlideFloorHz all-pass
    constexpr double kHoldRefresh = 0.9988;                // -0.01 dB
    constexpr double kRatioOnset = 1.4125375446227544;    // 10^(1.5 / 10): mask starts
    constexpr double kRatioFull = 5.6234132519034912;     // 10^(7.5 / 10): mask fully transient

    inline double poleFor (double ms, double sr) noexcept
    {
        const double samples = ms * 0.001 * sr;
        return samples > 0.0 ? std::exp (-1.0 / samples) : 0.0;
    }

    inline float finiteOr (float v, float fallback) noexcept { return std::isfinite (v) ? v : fallback; }

    inline void flushTiny (double& v) noexcept
    {
        if (std::abs (v) < 1.0e-30)
            v = 0.0;
    }

    //==========================================================================
    // Trapezoidal state-variable filter (A. Simper's form): bilinear-exact LP / BP / HP, modulation-safe.
    struct SvfCoeffs
    {
        double k = kSqrt2, a1 = 1.0, a2 = 0.0, a3 = 0.0;

        void set (double g, double damping) noexcept
        {
            k = damping;
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
    };

    struct SvfState
    {
        double ic1 = 0.0, ic2 = 0.0;
        void flush() noexcept { flushTiny (ic1); flushTiny (ic2); }
    };

    inline double svfTick (const SvfCoeffs& c, SvfState& s, double x, double& lp) noexcept
    {
        const double v3 = x - s.ic2;
        const double v1 = c.a1 * s.ic1 + c.a2 * v3;
        const double v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
        s.ic1 = 2.0 * v1 - s.ic1;
        s.ic2 = 2.0 * v2 - s.ic2;
        lp = v2;
        return v1;
    }

    inline double svfLow (const SvfCoeffs& c, SvfState& s, double x) noexcept
    {
        double lp;
        svfTick (c, s, x, lp);
        return lp;
    }

    inline double svfHigh (const SvfCoeffs& c, SvfState& s, double x) noexcept
    {
        double lp;
        const double bp = svfTick (c, s, x, lp);
        return x - c.k * bp - lp;
    }

    //==========================================================================
    // Running maximum over a window of stamps (monotonic deque in a power-of-two ring).
    struct RingMax
    {
        std::vector<double> vals;
        std::vector<long long> stamps;
        unsigned head = 0, tail = 0, mask = 0;

        void prepare (int capacity)
        {
            unsigned size = 4;
            while (size < (unsigned) capacity + 2u)
                size <<= 1;
            vals.assign (size, 0.0);
            stamps.assign (size, 0);
            mask = size - 1u;
            reset();
        }

        void reset() noexcept { head = tail = 0; }

        void push (long long stamp, double v) noexcept
        {
            while (tail != head && vals[(tail - 1u) & mask] <= v)
                --tail;
            vals[tail & mask] = v;
            stamps[tail & mask] = stamp;
            ++tail;
            if (tail - head > mask + 1u)
                ++head;
        }

        void dropOlderThan (long long minStamp) noexcept
        {
            while (head != tail && stamps[head & mask] < minStamp)
                ++head;
        }

        double max() const noexcept { return head != tail ? vals[head & mask] : 0.0; }
    };

    // Critically damped two-stage one-pole smoother (C1-continuous: no kink in a gain ramp, unlike a linear ramp).
    // Snaps exactly onto the target once both stages are within 1e-9 of it.
    struct Smooth2
    {
        double y1 = 0.0, y2 = 0.0, target = 0.0, c = 0.0;

        void prepare (double sr, double stageMs) noexcept { c = poleFor (stageMs, sr); }
        void snap (double v) noexcept { y1 = y2 = target = v; }
        void setTarget (double v) noexcept { target = v; }
        bool isSettled() const noexcept { return y2 == target && y1 == target; }
        double current() const noexcept { return y2; }

        inline double next() noexcept
        {
            if (y2 == target && y1 == target)
                return y2;
            y1 = target + c * (y1 - target);
            y2 = y1 + c * (y2 - y1);
            if (std::abs (y1 - target) < 1.0e-9 && std::abs (y2 - target) < 1.0e-9)
                y1 = y2 = target;
            return y2;
        }
    };

    // Clipper with a quadratic knee: linear below a, slope 1 -> 0 between a and b, flat at ceil above b
    // (b = 2 ceil - a, so the curve is C1 and reaches ceil exactly at b).
    // Branch-free (the 4x loop vectorises): e = clamp (|x| - a, 0, b - a), |y| = min (|x|, a) + e - e^2 / (2 (b - a)).
    inline float kneeClip (float x, float a, float bMinusA, float inv2w) noexcept
    {
        const float ax = std::abs (x);
        const float e = std::min (std::max (ax - a, 0.0f), bMinusA);
        const float y = std::min (ax, a) + e - e * e * inv2w;
        return std::copysign (y, x);
    }

    // Meters of one side (input or output): balance (3 s) and crest (1 s) of the low band.
    struct SideMeters
    {
        double lowPow = 0.0, totPow = 0.0, rms = 0.0;     // one-poles updated once per 16-sample block
        double sumLow = 0.0, sumTot = 0.0;
        double blockPeak = 0.0, display = 0.0;
        int blockFill = 0;
        long long blockIndex = 0;
        RingMax holdMax;
        int holdBlocks = 1;
        double c3s = 0.0, c1s = 0.0, decayPerBlock = 0.0;

        void prepare (double sr)
        {
            c3s = poleFor (3000.0, sr / kScopeDecim);
            c1s = poleFor (1000.0, sr / kScopeDecim);
            holdBlocks = std::max (1, (int) std::lround (sr / kScopeDecim));
            decayPerBlock = poleFor (1000.0, sr / kScopeDecim);
            holdMax.prepare (holdBlocks + 2);
            reset();
        }

        void reset() noexcept
        {
            lowPow = totPow = rms = blockPeak = display = sumLow = sumTot = 0.0;
            blockFill = 0;
            blockIndex = 0;
            holdMax.reset();
        }

        inline void add (double lowL, double lowR, double totL, double totR) noexcept
        {
            sumLow += lowL * lowL + lowR * lowR;
            sumTot += totL * totL + totR * totR;
            blockPeak = std::max (blockPeak, std::max (std::abs (lowL), std::abs (lowR)));
            if (++blockFill >= kScopeDecim)
            {
                const double pl = sumLow * (0.5 / kScopeDecim), pt = sumTot * (0.5 / kScopeDecim);
                sumLow = sumTot = 0.0;
                lowPow = pl + c3s * (lowPow - pl);
                totPow = pt + c3s * (totPow - pt);
                rms = pl + c1s * (rms - pl);
                holdMax.push (blockIndex, blockPeak);
                holdMax.dropOlderThan (blockIndex - holdBlocks + 1);
                ++blockIndex;
                display = std::max (holdMax.max(), display * decayPerBlock);
                blockPeak = 0.0;
                blockFill = 0;
            }
        }

        void flush() noexcept
        {
            flushTiny (lowPow);
            flushTiny (totPow);
            flushTiny (rms);
            flushTiny (display);
        }

        float balanceDb() const noexcept
        {
            if (totPow < 1.0e-14 || lowPow <= 0.0)
                return -60.0f;
            return (float) std::clamp (10.0 * std::log10 (lowPow / totPow), -60.0, 6.0);
        }

        float crestDb() const noexcept
        {
            if (rms < 1.0e-14 || display <= 0.0)
                return 0.0f;
            return (float) std::max (0.0, 20.0 * std::log10 (display) - 10.0 * std::log10 (rms));
        }
    };
} // namespace

double bassShaperBalanceTargetDb (double cutoffHz) noexcept
{
    return -4.0 + 3.0 * std::log2 ((cutoffHz > 1.0 ? cutoffHz : 120.0) / 120.0);
}

//==============================================================================
struct BassShaper::Impl
{
    BassShaperMeters meters;
    StereoSampleFifo scope, mask;

    double sr = 48000.0;
    int l1 = 48, l2 = 72, osLatency = 5, latency = 125;
    bool prepared = false;

    BassShaperParams params;
    bool haveParams = false, paramsNeutral = true;

    // split
    double curLog = std::log (120.0), tgtLog = std::log (120.0), glide = 0.0;
    SvfCoeffs xo;
    SvfState xs[2][4];

    // harmonic band limiting (HP 20 Hz, LP 8 x cutoff)
    SvfCoeffs hpCo, lpCo;
    SvfState hs[2][2];
    bool harmonicsSilent = true;

    // smoothed controls
    Smooth2 balSm, punchSm, susEnSm, susThrSm, susAmountSm, susCharSm, peakEnSm, peakThrSm, peakCharSm, peakMixSm;
    double peakAp = 0.5;   // 10^(peak thr / 20), cached while the threshold does not move

    // transient mask
    // The quadrature detectors run on every qDecim-th sample (24..48 kHz rate): the low band is band-limited by the
    // LR4 split (< -100 dB above 6 kHz for cutoffs <= 300 Hz), so this decimation cannot alias; the last value is held.
    QuadratureEnvelope qMask, qSus;
    int qDecim = 1, qPhase = 0;
    double pqHeld = 0.0, spHeld = 0.0;
    double pf = 0.0, ps = 0.0, w = 0.0;
    double cPfAtt = 0.0, cPfRel = 0.0, cPsAtt = 0.0, cPsRel = 0.0, cWAtt = 0.0, cWRel = 0.0;

    // sustain power
    double psus = 0.0, lift = 0.0;
    double susThrCached = 1.0, susThrAmp = 1.0, susThrPow = 1.0;   // 10^(thr / 20) and its square, for thr
    double gsLiftDb = 0.0, gsGain = 1.0;                           // the sustain gain of the last evaluated lift
    double cSusAtt = 0.0, cSusRel = 0.0, cLiftFall = 0.0, cLiftRise = 0.0;
    double hold = 0.0, cHoldRel = 0.0;
    int holdLeft = 0, holdSamples = 1;

    // delays
    DelayLine dryDelay[2], highDelay[2], l1Delay[2], l2Delay[2], osCleanDelay[2], osDryDelay[2], wDelay;

    // clean limiter
    RingMax limMax;
    std::vector<double> box;
    int boxLen = 73, boxPos = 0;
    double boxSum = 73.0, relHold = 0.0, cLimRel = 0.0, gLim = 1.0;
    long long limIndex = 0;

    Oversampler os;
    bool osRunning = true;      // idle (not run) while the dirty clipper cannot be heard
    int osWarmLeft = 0, osWarmSamples = 240;

    // neutral passthrough crossfade
    // Engaging: x_D -> d (10 ms fade), d's all-pass glides from 8 Hz to the cutoff, d -> processed (20 ms).
    // d = AP (x_D), a 2nd-order all-pass equal to the split's sum once it sits at the cutoff. Disengaging runs the
    // same steps backwards (see the notes at the top).
    double wetTarget = 0.0;
    double fadeC = 0.0, fadeA = 0.0;      // x_D -> d, d -> processed
    double stepC = 0.001, stepA = 0.001;
    double parkLog = std::log (kParkHz), apLog = std::log (kParkHz);   // d's all-pass frequency (log Hz)
    double glideRate = 0.0, glideFloorRate = 0.0;    // per sample: ln-units x Hz, ln-units
    SvfCoeffs parkCo, glideCo;
    SvfState apState[2];
    long long neutralSamples = 0, neutralHold = 12000;

    // auto gain
    double agPin = 0.0, agPout = 0.0, agTargetDb = 0.0, agDb = 0.0, cAgPow = 0.0, cAgGlide = 0.0, cAgReturn = 0.0;

    // meters
    SideMeters inMeters, outMeters;
    double shareState = 0.0, cShare = 0.0;
    RingMax envMax;            // block peaks of the low mid over the last 30 ms (mask FIFO envelope)
    int envBlocks = 90;
    long long envIndex = 0;
    float scopeMin = 0.0f, scopeMax = 0.0f;
    double scopeW = 0.0;
    int scopeFill = 0;

    // chunk buffers
    float bufX[2][kChunk] {}, bufHigh[2][kChunk] {}, bufClean[2][kChunk] {}, bufDry[2][kChunk] {},
          bufDirty[2][kChunk] {};
    float ctlChar[kChunk] {}, ctlMix[kChunk] {}, ctlAp[kChunk] {}, ctlW[kChunk] {};
    double bufAllPass[2][kChunk] {}, ctlFadeA[kChunk] {}, ctlFadeC[kChunk] {};
    float fifoA[kChunk / kScopeDecim + 2] {}, fifoB[kChunk / kScopeDecim + 2] {},
          fifoC[kChunk / kScopeDecim + 2] {}, fifoD[kChunk / kScopeDecim + 2] {};

    //==========================================================================
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        l1 = std::max (1, (int) std::lround (0.001 * sr));
        l2 = std::max (1, (int) std::lround (0.0015 * sr));

        os.prepare (2, Oversampler::Quality::MinimumPhase, kChunk, 2);
        osWarmSamples = std::max (kChunk, (int) std::lround (0.005 * sr));
        osLatency = os.getLatencySamples();
        latency = l1 + l2 + osLatency;

        for (int c = 0; c < 2; ++c)
        {
            dryDelay[c].prepare (latency);
            dryDelay[c].setDelay (latency);
            highDelay[c].prepare (latency);
            highDelay[c].setDelay (latency);
            l1Delay[c].prepare (l1);
            l1Delay[c].setDelay (l1);
            l2Delay[c].prepare (l2);
            l2Delay[c].setDelay (l2);
            osCleanDelay[c].prepare (std::max (1, osLatency));
            osCleanDelay[c].setDelay (osLatency);
            osDryDelay[c].prepare (std::max (1, osLatency));
            osDryDelay[c].setDelay (osLatency);
        }
        wDelay.prepare (l2 + osLatency);
        wDelay.setDelay (l2 + osLatency);

        boxLen = l2 + 1;
        box.assign ((size_t) boxLen, 1.0);
        limMax.prepare (boxLen + 2);

        qDecim = std::max (1, (int) std::floor (sr / 24000.0 + 1.0e-9));
        qDecim = qDecim >= 8 ? 8 : (qDecim >= 4 ? 4 : (qDecim >= 2 ? 2 : 1));
        qMask.prepare (sr / qDecim, 20.0);
        qSus.prepare (sr / qDecim, 20.0);

        glide = 1.0 - std::exp (-1.0 / (0.020 * sr));
        cPfAtt = poleFor (2.0, sr);
        cPfRel = poleFor (25.0, sr);
        cPsAtt = poleFor (80.0, sr);
        cPsRel = poleFor (300.0, sr);
        cWAtt = poleFor (0.5, sr);
        cWRel = poleFor (30.0, sr);
        cSusAtt = poleFor (10.0, sr);
        cSusRel = poleFor (150.0, sr);
        cLiftFall = poleFor (10.0, sr);
        cLiftRise = poleFor (150.0, sr);
        cHoldRel = poleFor (300.0, sr);
        holdSamples = std::max (1, (int) std::lround (0.030 * sr));
        cLimRel = poleFor (50.0, sr);
        cAgPow = poleFor (3000.0, sr);
        cAgGlide = poleFor (1000.0, sr);
        cAgReturn = poleFor (150.0, sr);
        cShare = poleFor (300.0, sr);
        envBlocks = std::max (1, (int) std::ceil (0.030 * sr / kScopeDecim));
        envMax.prepare (envBlocks + 2);
        stepC = 1.0 / (0.010 * sr);
        glideRate = kGlideKappa / sr;
        glideFloorRate = kGlideKappa * kGlideFloorHz / sr;
        stepA = 1.0 / (0.020 * sr);
        parkCo.set (std::tan (kPi * kParkHz / sr), kSqrt2);
        neutralHold = (long long) std::lround (0.250 * sr);

        for (auto* s : { &balSm, &punchSm, &susEnSm, &susThrSm, &susAmountSm, &susCharSm, &peakEnSm, &peakThrSm,
                         &peakCharSm, &peakMixSm })
            s->prepare (sr, kSmoothStageMs);

        hpCo.set (std::tan (kPi * 20.0 / sr), kSqrt2);

        inMeters.prepare (sr);
        outMeters.prepare (sr);
        // (the scope FIFOs are sized once in the constructor: re-preparing them here could race with the UI's pull)

        prepared = true;
        reset();
    }

    void designSplit() noexcept
    {
        const double hz = std::exp (curLog);
        xo.set (std::tan (kPi * std::min (hz, 0.45 * sr) / sr), kSqrt2);
        lpCo.set (std::tan (kPi * std::min (8.0 * hz, 0.45 * sr) / sr), kSqrt2);
    }

    void reset() noexcept
    {
        for (auto& ch : xs)
            for (auto& s : ch)
                s = SvfState {};
        for (auto& ch : hs)
            for (auto& s : ch)
                s = SvfState {};
        harmonicsSilent = true;
        apState[0] = apState[1] = SvfState {};

        for (int c = 0; c < 2; ++c)
        {
            dryDelay[c].reset();
            highDelay[c].reset();
            l1Delay[c].reset();
            l2Delay[c].reset();
            osCleanDelay[c].reset();
            osDryDelay[c].reset();
        }
        wDelay.reset();

        qMask.reset();
        qSus.reset();
        qPhase = 0;
        pqHeld = spHeld = 0.0;
        pf = ps = w = 0.0;
        psus = 0.0;
        hold = 0.0;
        holdLeft = 0;

        limMax.reset();
        std::fill (box.begin(), box.end(), 1.0);
        boxPos = 0;
        boxSum = (double) boxLen;
        relHold = 0.0;
        gLim = 1.0;
        limIndex = 0;

        os.reset();
        osRunning = true;
        osWarmLeft = 0;

        agPin = agPout = 0.0;
        agTargetDb = 0.0;

        inMeters.reset();
        outMeters.reset();
        shareState = 0.0;
        envMax.reset();
        envIndex = 0;
        scopeMin = scopeMax = 0.0f;
        scopeW = 0.0;
        scopeFill = 0;

        meters.balanceDb.store (-60.0f);
        meters.crestDb.store (0.0f);
        meters.balanceOutDb.store (-60.0f);
        meters.crestOutDb.store (0.0f);
        meters.transientShare.store (0.0f);
        meters.sustainLiftDb.store (0.0f);
        meters.peakReductionDb.store (0.0f);
        meters.lowPeakDb.store (-200.0f);

        applyParams (true);
    }

    //==========================================================================
    void setParams (ParamValues v) noexcept
    {
        BassShaperParams np;
        readBassShaperParams (v, np);
        np.cutoffHz = std::clamp (finiteOr (np.cutoffHz, 120.0f), 65.0f, 300.0f);
        np.balanceDb = std::clamp (finiteOr (np.balanceDb, 0.0f), -12.0f, 12.0f);
        np.punchDb = std::clamp (finiteOr (np.punchDb, 0.0f), -12.0f, 12.0f);
        np.sustainThresholdDb = std::clamp (finiteOr (np.sustainThresholdDb, -18.0f), -40.0f, 0.0f);
        np.sustainAmountPercent = std::clamp (finiteOr (np.sustainAmountPercent, 50.0f), 0.0f, 100.0f);
        np.sustainCharacterPercent = std::clamp (finiteOr (np.sustainCharacterPercent, 100.0f), 0.0f, 100.0f);
        np.peakThresholdDb = std::clamp (finiteOr (np.peakThresholdDb, -6.0f), -30.0f, 0.0f);
        np.peakCharacterPercent = std::clamp (finiteOr (np.peakCharacterPercent, 100.0f), 0.0f, 100.0f);
        np.peakMixPercent = std::clamp (finiteOr (np.peakMixPercent, 100.0f), 0.0f, 100.0f);
        params = np;
        const bool first = ! haveParams;
        haveParams = true;
        applyParams (first);
    }

    void applyParams (bool snap) noexcept
    {
        const auto& p = params;
        tgtLog = std::log ((double) p.cutoffHz);
        const float balG = (float) std::exp ((double) p.balanceDb * kLn10Over20);
        const float punchG = (float) std::exp ((double) p.punchDb * kLn10Over20);
        paramsNeutral = p.balanceDb == 0.0f && p.punchDb == 0.0f && ! p.sustainOn && ! p.peakOn;

        if (snap)
        {
            curLog = tgtLog;
            designSplit();
            balSm.snap ((double) balG);
            punchSm.snap ((double) punchG);
            susEnSm.snap (p.sustainOn ? 1.0 : 0.0);
            susThrSm.snap ((double) p.sustainThresholdDb);
            susAmountSm.snap ((double) p.sustainAmountPercent * 0.01);
            susCharSm.snap ((double) p.sustainCharacterPercent * 0.01);
            peakEnSm.snap (p.peakOn ? 1.0 : 0.0);
            peakThrSm.snap ((double) p.peakThresholdDb);
            peakCharSm.snap ((double) p.peakCharacterPercent * 0.01);
            peakMixSm.snap ((double) p.peakMixPercent * 0.01);
            peakAp = std::exp ((double) p.peakThresholdDb * kLn10Over20);
            lift = 0.0;
            gsLiftDb = 0.0;
            gsGain = 1.0;
            wetTarget = paramsNeutral ? 0.0 : 1.0;
            fadeC = fadeA = wetTarget;
            apLog = paramsNeutral ? parkLog : curLog;
            neutralSamples = paramsNeutral ? neutralHold : 0;
            agDb = 0.0;
        }
        else
        {
            balSm.setTarget ((double) balG);
            punchSm.setTarget ((double) punchG);
            susEnSm.setTarget (p.sustainOn ? 1.0 : 0.0);
            susThrSm.setTarget ((double) p.sustainThresholdDb);
            susAmountSm.setTarget ((double) p.sustainAmountPercent * 0.01);
            susCharSm.setTarget ((double) p.sustainCharacterPercent * 0.01);
            peakEnSm.setTarget (p.peakOn ? 1.0 : 0.0);
            peakThrSm.setTarget ((double) p.peakThresholdDb);
            peakCharSm.setTarget ((double) p.peakCharacterPercent * 0.01);
            peakMixSm.setTarget ((double) p.peakMixPercent * 0.01);
        }
    }

    //==========================================================================
    void process (float* const* channels, int numSamples) noexcept
    {
        if (! prepared || channels == nullptr || channels[0] == nullptr || channels[1] == nullptr || numSamples <= 0)
            return;

        // one exact sum per call keeps the limiter's running box sum free of drift
        boxSum = 0.0;
        for (double v : box)
            boxSum += v;

        float lowPeak = 0.0f;
        for (int start = 0; start < numSamples; start += kChunk)
        {
            const int n = std::min (kChunk, numSamples - start);
            float* io[2] { channels[0] + start, channels[1] + start };
            processChunk (io, n, lowPeak);
        }

        // flush the long-lived double states (only reachable after very long silences; FTZ covers the rest)
        for (auto& ch : xs)
            for (auto& s : ch)
                s.flush();
        for (auto& ch : hs)
            for (auto& s : ch)
                s.flush();
        apState[0].flush();
        apState[1].flush();
        flushTiny (pf);
        flushTiny (ps);
        flushTiny (w);
        flushTiny (psus);
        flushTiny (hold);
        flushTiny (agPin);
        flushTiny (agPout);
        flushTiny (shareState);
        flushTiny (relHold);
        inMeters.flush();
        outMeters.flush();

        meters.balanceDb.store (inMeters.balanceDb());
        meters.crestDb.store (inMeters.crestDb());
        meters.balanceOutDb.store (outMeters.balanceDb());
        meters.crestOutDb.store (outMeters.crestDb());
        meters.transientShare.store ((float) std::clamp (shareState, 0.0, 1.0));
        meters.sustainLiftDb.store ((float) (lift * susEnSm.current()));
        meters.peakReductionDb.store (peakEnSm.current() > 0.0 ? gainToDbF ((float) gLim) : 0.0f);
        meters.lowPeakDb.storeMax (gainToDbF (lowPeak));
    }

    void updateNeutralState (int n) noexcept
    {
        const bool settled = paramsNeutral && balSm.isSettled() && punchSm.isSettled() && susEnSm.isSettled()
                             && peakEnSm.isSettled();
        if (settled)
        {
            neutralSamples = std::min (neutralSamples + n, neutralHold);
            if (neutralSamples >= neutralHold)
                wetTarget = 0.0;
        }
        else
        {
            neutralSamples = 0;
            wetTarget = 1.0;
        }
    }

    // The dirty branch is heard when its weight (1 - character) x enable can be > 0. The oversampler idles otherwise
    // and, when it is needed again, restarts from silence and runs 5 ms unheard before the controls move on.
    void updateOversamplerState() noexcept
    {
        const bool canHear = (peakEnSm.target > 0.0 || peakEnSm.current() > 0.0)
                             && (peakCharSm.target < 1.0 || peakCharSm.current() < 1.0);
        if (canHear && ! osRunning)
        {
            os.reset();
            osRunning = true;
            osWarmLeft = osWarmSamples;
        }
        else if (! canHear && osRunning)
        {
            osRunning = false;
            osWarmLeft = 0;
        }
    }

    void processChunk (float* const* io, int n, float& lowPeak) noexcept
    {
        updateOversamplerState();

        const auto& p = params;
        const double eps = 1.0e-10;

        //----------------------------------------------------------------------
        // pass 1 (base rate): split, balance, mask, punch, sustain power, harmonics, clean limiter
        for (int i = 0; i < n; ++i)
        {
            updateNeutralState (1);
            if (curLog != tgtLog)
            {
                curLog += (tgtLog - curLog) * glide;
                if (std::abs (tgtLog - curLog) < 1.0e-7)
                    curLog = tgtLog;
                designSplit();
            }

            const double x[2] { (double) io[0][i], (double) io[1][i] };
            double low[2], high[2];
            for (int c = 0; c < 2; ++c)
            {
                low[c] = svfLow (xo, xs[c][1], svfLow (xo, xs[c][0], x[c]));
                high[c] = svfHigh (xo, xs[c][3], svfHigh (xo, xs[c][2], x[c]));
                bufX[c][i] = dryDelay[c].process (io[c][i]);
                bufHigh[c][i] = highDelay[c].process ((float) high[c]);
            }
            inMeters.add (low[0], low[1], x[0], x[1]);

            const double gb = balSm.next();
            low[0] *= gb;
            low[1] *= gb;

            // transient mask from the undelayed low-band mid
            // fast envelope: quadrature power, or the instantaneous m^2 where it is higher (m^2 never exceeds the
            // squared envelope of a steady tone, so no ripple is added, but it rises within a quarter cycle at an
            // onset, while the all-pass pair lags ~10 ms at 55 Hz: more than the 1 ms lookahead). The slow
            // (sustain-level) envelope takes the quadrature power alone: its lag only delays its own 80 ms rise.
            const double mNow = 0.5 * (low[0] + low[1]);
            const bool qTick = ++qPhase >= qDecim;
            if (qTick)
            {
                qPhase = 0;
                pqHeld = qMask.process (mNow);
            }
            const double pq = pqHeld;
            const double power = std::max (pq, mNow * mNow);
            pf = power + (power > pf ? cPfAtt : cPfRel) * (pf - power);
            ps = pq + (pq > ps ? cPsAtt : cPsRel) * (ps - pq);
            const double q = (pf + eps) / (ps + eps);
            double wRaw = 0.0;
            if (q >= kRatioFull)
                wRaw = 1.0;
            else if (q > kRatioOnset)
                wRaw = (kTenOverLn10 * std::log (q) - 1.5) / 6.0;
            w = wRaw + (wRaw > w ? cWAtt : cWRel) * (w - wRaw);
            shareState = w + cShare * (shareState - w);

            // low band delayed by the 1 ms lookahead
            double lowD[2], t[2], s[2];
            for (int c = 0; c < 2; ++c)
            {
                lowD[c] = (double) l1Delay[c].process ((float) low[c]);
                t[c] = w * lowD[c];
                s[c] = lowD[c] - t[c];
            }
            ctlW[i] = wDelay.process ((float) w);

            const double gp = punchSm.next();

            // sustain power: upward compression of the sustain part
            const double thrSus = susThrSm.next();
            const double susAmount = susAmountSm.next();
            const double eS = susEnSm.next();
            if (qTick)
                spHeld = qSus.process (0.5 * (s[0] + s[1]));
            const double sp = spHeld;
            psus = sp + (sp > psus ? cSusAtt : cSusRel) * (psus - sp);
            if (thrSus != susThrCached)
            {
                susThrCached = thrSus;
                susThrAmp = std::exp (thrSus * kLn10Over20);
                susThrPow = susThrAmp * susThrAmp;
            }
            double liftTarget = 0.0;
            // Keep the gain computer running throughout the smooth enable fade. The raw on/off switch would
            // force an immediate 10 ms gain release and kink the waveform before that fade can take effect.
            if (eS > 0.0 && psus < susThrPow)   // at or above the threshold the lift is 0 (no log needed)
            {
                const double ls = psus > 1.0e-30 ? kTenOverLn10 * std::log (psus) : -300.0;
                liftTarget = std::clamp ((thrSus - ls) * susAmount, 0.0, 12.0);
                liftTarget *= std::clamp ((ls - (thrSus - 48.0)) / 12.0, 0.0, 1.0);
            }
            lift = liftTarget + (liftTarget < lift ? cLiftFall : cLiftRise) * (lift - liftTarget);
            if (lift < 1.0e-9)
                lift = 0.0;
            const double liftApplied = lift * eS;
            // the gain is re-evaluated when the lift moved by more than 1e-5 dB (a step of 1.2e-6, -118 dB)
            if (std::abs (liftApplied - gsLiftDb) > 1.0e-5 || (liftApplied == 0.0 && gsLiftDb != 0.0))
            {
                gsLiftDb = liftApplied;
                gsGain = liftApplied > 0.0 ? std::exp (liftApplied * kLn10Over20) : 1.0;
            }
            const double gs = gsGain;

            double v[2] { s[0] * gs, s[1] * gs };

            // dirty sustain harmonics (order-3 polynomial of the lifted sustain, band-limited)
            const double dirt = (1.0 - susCharSm.next()) * eS;
            double h[2] { 0.0, 0.0 };
            if (dirt > 0.0 || ! harmonicsSilent)
            {
                const double pk = std::max (std::abs (v[0]), std::abs (v[1]));
                if (pk >= hold * kHoldRefresh)
                {
                    // a peak within 0.01 dB of the hold re-arms it: sampled peaks of a steady tone differ by up to
                    // 2e-4, and a hold that expired between them would modulate the harmonics
                    hold = std::max (hold, pk);
                    holdLeft = holdSamples;
                }
                else if (holdLeft > 0)
                    --holdLeft;
                else
                    hold *= cHoldRel;

                const double aEff = std::max (susThrAmp, hold);
                const double invA = 1.0 / aEff;
                for (int c = 0; c < 2; ++c)
                {
                    const double u = v[c] * invA;
                    double hc = dirt > 0.0 ? aEff * dirt * u * u * (0.30 + 0.12 * u) : 0.0;
                    hc = svfHigh (hpCo, hs[c][0], hc);
                    hc = svfLow (lpCo, hs[c][1], hc);
                    h[c] = hc;
                }
                if (dirt <= 0.0)   // decaying after the dirt was turned off: stop once the filters are silent
                {
                    double energy = 0.0;
                    for (const auto& ch : hs)
                        for (const auto& st : ch)
                            energy += std::abs (st.ic1) + std::abs (st.ic2);
                    harmonicsSilent = energy < 1.0e-25;
                    if (harmonicsSilent)
                        for (auto& ch : hs)
                            for (auto& st : ch)
                                st = SvfState {};
                }
                else
                    harmonicsSilent = false;
            }
            else
            {
                hold = 0.0;
                holdLeft = 0;
            }

            double lowP[2];
            for (int c = 0; c < 2; ++c)
                lowP[c] = gp * t[c] + v[c] + h[c];

            // clean limiter (lookahead L2, brickwall)
            if (! peakThrSm.isSettled())
                peakAp = std::exp (peakThrSm.next() * kLn10Over20);
            const double ap = peakAp;
            const double level = std::max (std::abs (lowP[0]), std::abs (lowP[1]));
            const double apGuard = ap * 0.999999;
            const double d = level > apGuard ? std::log (level / apGuard) : 0.0;
            limMax.push (limIndex, d);
            limMax.dropOlderThan (limIndex - boxLen + 1);
            ++limIndex;
            relHold = std::max (limMax.max(), relHold * cLimRel);
            if (relHold < 1.0e-12)
                relHold = 0.0;
            const double e = relHold > 0.0 ? std::exp (-relHold) : 1.0;
            const double leaving = box[(size_t) boxPos];
            box[(size_t) boxPos] = e;
            boxPos = boxPos + 1 < boxLen ? boxPos + 1 : 0;
            boxSum += e - leaving;
            gLim = std::min (1.0, boxSum / (double) boxLen);

            for (int c = 0; c < 2; ++c)
            {
                const float delayed = l2Delay[c].process ((float) lowP[c]);
                bufDry[c][i] = delayed;
                bufClean[c][i] = (float) ((double) delayed * gLim);
            }

            ctlAp[i] = (float) ap;
            // while the oversampler warms up after idling, the controls that would expose the dirty branch wait
            const double cNow = osWarmLeft > 0 ? peakCharSm.current() : peakCharSm.next();
            const double eNow = osWarmLeft > 0 ? peakEnSm.current() : peakEnSm.next();
            ctlChar[i] = (float) cNow;
            ctlMix[i] = (float) (peakMixSm.next() * eNow);

            // Keep the transition filter and its fade clock on this sample's cutoff. Advancing it in pass 3
            // would reuse the chunk's FINAL coefficients for every sample and staircase cutoff automation.
            processNeutralTransition (i);
        }

        //----------------------------------------------------------------------
        // pass 2: dirty clipper at 4x on the L2-delayed low part (idle while nothing can hear it)
        if (osRunning)
        {
            const float* in[2] { bufDry[0], bufDry[1] };
            float* const* up = os.upsample (in, n);
            constexpr float kKneeLow = 0.97162795f;   // 10^(-0.25 / 20)
            for (int i = 0; i < n; ++i)
            {
                const float ceil = ctlAp[i];
                const float a = ceil * kKneeLow;
                const float bMinusA = 2.0f * (ceil - a);
                const float inv2w = 0.5f / bMinusA;
                for (int c = 0; c < 2; ++c)
                {
                    float* sm = up[c] + 4 * i;
                    for (int k = 0; k < 4; ++k)
                        sm[k] = kneeClip (sm[k], a, bMinusA, inv2w);
                }
            }
            float* out[2] { bufDirty[0], bufDirty[1] };
            os.downsample (out, n);
            osWarmLeft = std::max (0, osWarmLeft - n);
        }
        else
        {
            std::copy (bufDry[0], bufDry[0] + n, bufDirty[0]);   // never heard (weight 0)
            std::copy (bufDry[1], bufDry[1] + n, bufDirty[1]);
        }

        //----------------------------------------------------------------------
        // pass 3: align, mix, sum, neutral fade, auto gain, meters, scope

        // auto gain: the one-pole glide (1 s, dB) evaluated at the chunk ends, the gain ramped linearly in between
        double agGain = 1.0, agStep = 0.0;
        {
            // toward the neutral passthrough the gain returns faster (150 ms), so it is unity by the time the output
            // is the exact delay again
            const double agGoal = (p.autoGain && wetTarget > 0.0) ? agTargetDb : 0.0;
            const double startDb = agDb;
            if (agDb != agGoal)
            {
                agDb = agGoal + std::pow (wetTarget > 0.0 ? cAgGlide : cAgReturn, (double) n) * (agDb - agGoal);
                if (std::abs (agDb - agGoal) < 1.0e-3)   // a 1e-4 step (-80 dB)
                    agDb = agGoal;
            }
            if (startDb != 0.0 || agDb != 0.0)
            {
                agGain = std::exp (startDb * kLn10Over20);
                agStep = (std::exp (agDb * kLn10Over20) - agGain) / (double) n;
            }
        }
        for (int i = 0; i < n; ++i)
        {
            const double c = (double) ctlChar[i];
            const double mix = (double) ctlMix[i];
            double lowOut[2], y[2];
            for (int ch = 0; ch < 2; ++ch)
            {
                const double clean = (double) osCleanDelay[ch].process (bufClean[ch][i]);
                const double dry = (double) osDryDelay[ch].process (bufDry[ch][i]);
                const double dirty = (double) bufDirty[ch][i];
                const double peakOut = c >= 1.0 ? clean : c * clean + (1.0 - c) * dirty;
                lowOut[ch] = mix <= 0.0 ? dry : (mix >= 1.0 ? peakOut : mix * peakOut + (1.0 - mix) * dry);
                y[ch] = (double) bufHigh[ch][i] + lowOut[ch];
            }

            float out[2];
            if (ctlFadeC[i] <= 0.0)
            {
                out[0] = bufX[0][i];
                out[1] = bufX[1][i];
            }
            else if (ctlFadeA[i] >= 1.0)
            {
                out[0] = (float) y[0];
                out[1] = (float) y[1];
            }
            else
            {
                const double a = 0.5 - 0.5 * std::cos (kPi * ctlFadeA[i]);
                const double c2 = 0.5 - 0.5 * std::cos (kPi * ctlFadeC[i]);
                for (int ch = 0; ch < 2; ++ch)
                {
                    const double inner = (1.0 - a) * bufAllPass[ch][i] + a * y[ch];
                    out[ch] = (float) ((1.0 - c2) * (double) bufX[ch][i] + c2 * inner);
                }
            }

            // auto gain (input delayed by D vs output)
            {
                const double pin = 0.5 * ((double) bufX[0][i] * bufX[0][i] + (double) bufX[1][i] * bufX[1][i]);
                const double pout = 0.5 * ((double) out[0] * out[0] + (double) out[1] * out[1]);
                agPin = pin + cAgPow * (agPin - pin);
                agPout = pout + cAgPow * (agPout - pout);
            }
            if (agStep != 0.0 || agGain != 1.0)
            {
                agGain += agStep;
                out[0] = (float) ((double) out[0] * agGain);
                out[1] = (float) ((double) out[1] * agGain);
            }

            io[0][i] = out[0];
            io[1][i] = out[1];

            // output meters and scope (low part before auto gain)
            outMeters.add (lowOut[0], lowOut[1], y[0], y[1]);
            const float lp = (float) std::max (std::abs (lowOut[0]), std::abs (lowOut[1]));
            lowPeak = std::max (lowPeak, lp);

            const float mid = (float) (0.5 * (lowOut[0] + lowOut[1]));
            if (scopeFill == 0)
            {
                scopeMin = scopeMax = mid;
                scopeW = 0.0;
            }
            scopeMin = std::min (scopeMin, mid);
            scopeMax = std::max (scopeMax, mid);
            scopeW += (double) ctlW[i];
            if (++scopeFill >= kScopeDecim)
            {
                scopeFill = 0;
                envMax.push (envIndex, (double) std::max (-scopeMin, scopeMax));
                envMax.dropOlderThan (envIndex - envBlocks + 1);
                ++envIndex;
                pushScope (scopeMin, scopeMax, (float) (scopeW / kScopeDecim), (float) envMax.max());
            }
        }
        flushScope();

        // auto gain target (frozen below -60 dBFS input)
        if (agPin > 1.0e-6 && agPout > 1.0e-12)
            agTargetDb = std::clamp (kTenOverLn10 * std::log (agPin / agPout), -12.0, 12.0);
    }

    int pendingFifo = 0;

    void processNeutralTransition (int i) noexcept
    {
        if (wetTarget > 0.0)
        {
            if (fadeC < 1.0)
                fadeC = std::min (1.0, fadeC + stepC);
            else if (apLog != curLog && fadeA <= 0.0)
                glideAllPass (curLog);
            else
            {
                apLog = curLog;
                fadeA = std::min (1.0, fadeA + stepA);
            }
        }
        else
        {
            if (fadeA > 0.0)
            {
                apLog = curLog;
                fadeA = std::max (0.0, fadeA - stepA);
            }
            else if (apLog != parkLog)
                glideAllPass (parkLog);
            else if (fadeC > 0.0)
                fadeC = std::max (0.0, fadeC - stepC);
        }

        const SvfCoeffs* apCo = &glideCo;
        if (apLog == curLog)
            apCo = &xo;
        else if (apLog == parkLog)
            apCo = &parkCo;
        else
            glideCo.set (std::tan (kPi * std::min (std::exp (apLog), 0.45 * sr) / sr), kSqrt2);
        for (int ch = 0; ch < 2; ++ch)
        {
            double lpv;
            const double bp = svfTick (*apCo, apState[ch], (double) bufX[ch][i], lpv);
            bufAllPass[ch][i] = (double) bufX[ch][i] - 2.0 * apCo->k * bp;
        }
        ctlFadeA[i] = fadeA;
        ctlFadeC[i] = fadeC;
    }

    // Moves d's all-pass toward `target` (log Hz) at a speed proportional to its frequency (a fixed fraction of its
    // own group delay per step, so the sweep stays slow where the all-pass is slow), never below the floor speed.
    void glideAllPass (double target) noexcept
    {
        const double hz = std::exp (apLog);
        const double step = std::max (glideRate * hz, glideFloorRate);
        if (std::abs (target - apLog) <= step)
            apLog = target;
        else
            apLog += target > apLog ? step : -step;
    }

    void pushScope (float mn, float mx, float wv, float env) noexcept
    {
        if (pendingFifo >= kChunk / kScopeDecim + 2)
            flushScope();
        fifoA[pendingFifo] = mn;
        fifoB[pendingFifo] = mx;
        fifoC[pendingFifo] = wv;
        fifoD[pendingFifo] = env;
        ++pendingFifo;
    }

    void flushScope() noexcept
    {
        if (pendingFifo > 0)
        {
            scope.push (fifoA, fifoB, pendingFifo);
            mask.push (fifoC, fifoD, pendingFifo);
            pendingFifo = 0;
        }
    }
};

//==============================================================================
BassShaper::BassShaper() : impl (std::make_unique<Impl>())
{
    impl->scope.prepare (1 << 16);
    impl->mask.prepare (1 << 16);
}

BassShaper::~BassShaper() = default;

void BassShaper::prepare (double sampleRate, int)
{
    impl->prepare (sampleRate);
}

void BassShaper::reset() noexcept
{
    if (impl->prepared)
        impl->reset();
}

void BassShaper::setParams (ParamValues values) noexcept
{
    if (values != nullptr)
        impl->setParams (values);
}

void BassShaper::process (float* const* channels, int numSamples, const ProcessContext&) noexcept
{
#if PS_BASS_HAS_SSE
    const unsigned int savedCsr = _mm_getcsr();
    _mm_setcsr (savedCsr | 0x8040u);   // FTZ | DAZ
#endif
    impl->process (channels, numSamples);
#if PS_BASS_HAS_SSE
    _mm_setcsr (savedCsr);
#endif
}

int BassShaper::getLatencySamples() const noexcept { return impl->latency; }
const BassShaperMeters& BassShaper::getMeters() const noexcept { return impl->meters; }
BassShaperMeters& BassShaper::getMeters() noexcept { return impl->meters; }
StereoSampleFifo& BassShaper::getScopeFifo() noexcept { return impl->scope; }
StereoSampleFifo& BassShaper::getMaskFifo() noexcept { return impl->mask; }
} // namespace ps
