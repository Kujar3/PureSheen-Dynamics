#include "dsp/ClassicLimiter.h"

#include "dsp/MaximizerStages.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_CLIM_HAS_SSE 1
#endif

// Classic Limiter (mod-clim). Contract and signal flow: ClassicLimiter.h. The model and every constant below are fitted
// to the reference's renders (black-box measurement: 1 kHz / 100 Hz / 5 kHz / 10 kHz staircases, 1 kHz level steps
// with 20 ms .. 3 s holds, sweeps, noise and music; private notes out/vintage). Choices the data could not decide are
// marked "(not measured)".
//
// Per channel (the reference limits the channels independently: a loud left leaves the right untouched) and per mode
// (all three always run; a mode change crossfades their gains over 20 ms), on the overshoot O = 20 log10 (|G x| / Ct)
// dB (> 0 only above Ct = ceiling - kCeilingMarginDb, the reference's output peak):
// - Make-up: G = ceiling - threshold + kMakeupDb[mode] (measured: +0.35 dB Analog / Tube, +0.125 dB Modern, the same
//   at 2 / 3 / 6 / 12 dB of gain). Under 2 dB of gain (not measured) it scales down to 0 at threshold = ceiling, so
//   that setting is unity (the module's neutral setting). Below the ceiling the module is this gain and nothing else
//   (no knee, no colour). The make-up glides with the gain; each input sample carries its own (makeupRing).
// - Slow stage S (dB): a one-pole attack (kSlowAttackMs) toward the depth T = kDepthK (O - kDepthB O^kDepthC);
//   released linearly in dB at S / tau (C) x w (S) per second, w = 1 / (1 + (S / kDeepKneeDb)^kDeepPower) (deep
//   reductions release slower; measured at 8.8 dB), tau (C) = the measured per-mode laws (kTauS).
// - Fast stage (dB): the rest of the overshoot, r = max (0, O - S'), S' = S less the most it can release over the
//   window, held over the window W (kWindowMs; a sliding maximum), then a medium part Fm (one-pole charge
//   kMediumChargeMs toward kMediumShare x the held value, decay kMediumReleaseMs) and a fast part
//   Ff = max (held - Fm, Ff x decay kFastReleaseMs).
// - Gain g = 10^(-(S + Fm + Ff) / 20), averaged over the last W detector samples (a box): every gain value that touches
//   a peak is at most the gain that peak needs (the sliding maximum covers the box), so the sample peak stays <= Ct.
// The engines run W - 1 samples ahead of the output (their detector frontier), so every mode's gain belongs to the same
// output sample whatever its window; the core latency is the longest window of any mode and Character, less one. A
// Character change moves the fractional window one sample per kWindowStepMs, interpolating adjacent averages
// starting at the same output sample (integer jumps would step the held reduction). Then
// maxi::Guard (true peak with True Peak on, sample peak at the ceiling otherwise, so it is idle there) and a sample
// clamp at the ceiling.
//
// Meters per block: muReductionDb = the slow stage at the output sample (deepest channel, mode-weighted, capped by the
// applied total), brickwallReductionDb = the minimum of the rest (fast stage, guard, clamp). History per 64 output
// samples: the minimum slow and total reduction over the two channels (total = the applied gain, never above slow).
namespace ps
{
namespace
{
    using maxi::kChunk;   // 64: internal chunk, history decimation
    constexpr double kTwentyOverLn10 = 8.6858896380650366;   // 20 / ln (10)
    constexpr double kLn10Over20 = 0.11512925464970229;      // ln (10) / 20

    // ---- fitted coefficients (mode order Analog, Tube, Modern; character anchors 0, 2.5, 5, 7.5, 10) ----
    // Measured directly: the margin, the make-up, the slow depth at 2.35 / 6.35 / 12.35 dB over, the release laws.
    // Fitted (least squares on the rendered waveforms of every reference render of the mode, laws over Character
    // with anchors at 0 / 5 / 10, the times log-interpolated to 2.5 / 7.5): the windows, the attack, the medium and
    // fast parts. The data has Analog 0 / 2.5 / 5 / 7.5 / 10, Tube 0 / 2 / 5 / 10, Modern 0 / 5 / 10.
    constexpr double kCeilingMarginDb = 0.01;                 // output sample peak = ceiling - 0.01 dB (measured)
    constexpr double kMakeupDb[3] { 0.35, 0.35, 0.125 };      // small-signal gain = ceiling - threshold + this
    constexpr double kMakeupFullDb = 2.0;                     //   from this much gain on (measured at 2 .. 12 dB);
                                                              //   less: scaled down to 0 at 0 dB (not measured)
    constexpr double kWindowMs[3][5] {                        // hold / box window of the gain (Analog: Character-free)
        { 0.5, 0.5, 0.5, 0.5, 0.5 }, { 0.3075, 0.9315, 1.5555, 1.5555, 1.5555 }, { 0.2521, 0.348, 0.444, 0.5187, 0.5934 } };
    constexpr double kDepthK[3] { 1.0, 1.0, 0.21 };           // slow depth T = k (O - b O^c)
    constexpr double kDepthB[3] { 0.345, 0.345, 0.0 };
    constexpr double kDepthC[3] { 0.961, 0.961, 1.0 };
    // slow release tau (s) = a + b C + c C^2 (measured)
    constexpr double kTauS[3][3] { { 0.39, 0.163, 0.0 }, { 0.30, 0.33, 0.008 }, { 0.061, 0.027, 0.0054 } };
    constexpr double kDeepKneeDb = 7.05;                      // deep reductions release slower (w (S) above)
    constexpr int kDeepPower = 7;
    constexpr double kSlowAttackMs[3][5] {
        { 0.6, 0.6, 0.6, 0.6, 0.6 }, { 0.6527, 0.6416, 0.6307, 0.952, 1.4369 }, { 0.3986, 0.6968, 1.2181, 1.5573, 1.9911 } };
    constexpr double kMediumChargeMs[3][5] {                  // 0: no medium part (Analog)
        { 0, 0, 0, 0, 0 }, { 56.59, 56.59, 56.59, 56.59, 56.59 }, { 142.17, 33.457, 7.873, 5.2035, 3.4391 } };
    constexpr double kMediumShare[3][5] {                     // Fm charges toward this x the held fast reduction
        { 1, 1, 1, 1, 1 }, { 0.5901, 0.5469, 0.5037, 0.3537, 0.2036 }, { 0.2456, 0.232, 0.2185, 0.1218, 0.0251 } };
    constexpr double kMediumReleaseMs[3][5] {
        { 0, 0, 0, 0, 0 }, { 0.0965, 10.749, 1196.78, 1547.11, 2000.0 }, { 164.6, 265.61, 428.61, 485.12, 549.08 } };
    constexpr double kFastReleaseMs[3][5] {                   // 0: none (the window alone)
        { 0, 0, 0, 0, 0 }, { 0.2633, 1.5525, 9.1546, 24.778, 67.064 }, { 0.1159, 0.1159, 0.1159, 0.1159, 0.1159 } };
    // ---- end of the fitted block ----
    constexpr double kGuardAttackMs = 0.5;     // true-peak guard attack (its latency)
    constexpr double kGuardReleaseMs = 10.0;
    constexpr double kGuardTpMargin = 0.99770006382;    // -0.02 dB (True Peak on)
    constexpr double kGlideMs = 20.0;          // input gain and ceiling glide
    constexpr double kModeFadeMs = 20.0;       // mode crossfade
    constexpr double kWindowStepMs = 0.25;     // a Character change moves the window by one sample per this

    constexpr float kMaxInput = 1.0e6f;        // reject corrupt audio before the detectors and the rings

    // A character 0..10 from a 5-anchor table (0, 2.5, 5, 7.5, 10; linear in between).
    double anchors (const double (&t)[5], double character) noexcept
    {
        const double c = std::clamp (character, 0.0, 10.0) / 2.5;
        const int i = std::min (3, (int) c);
        return t[i] + (c - i) * (t[i + 1] - t[i]);
    }

    int windowFor (int mode, double character, double fs) noexcept
    {
        return std::max (1, (int) std::lround (anchors (kWindowMs[mode], character) * 1.0e-3 * fs));
    }
    // The longest window of any mode and character (the anchors bound the interpolated values).
    int maxWindowFor (double fs) noexcept
    {
        int w = 1;
        for (int m = 0; m < 3; ++m)
            for (int a = 0; a < 5; ++a)
                w = std::max (w, windowFor (m, 2.5 * a, fs));
        return w;
    }
    int guardAttackFor (double fs) noexcept { return std::max (1, (int) std::lround (kGuardAttackMs * 1.0e-3 * fs)); }

    inline double decayFor (double ms, double fs) noexcept { return ms > 0.0 ? std::exp (-1.0e3 / (ms * fs)) : 0.0; }

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

    // One mode's coefficients at the current character (sample-rate dependent).
    struct ModeCoeffs
    {
        int window = 1;
        double kSlowAtt = 1.0, slowRelPerSample = 0.0, windowFloor = 1.0;
        double kMedCharge = 0.0, medShare = 1.0, kMedRelease = 0.0, kFastRelease = 0.0;
    };

    // One mode's gain computer for one channel. Each step() takes the next detector sample (index last + 1); the box then
    // holds the gains of the last `window` detector samples, i.e. `gain` belongs to output sample last - window + 1.
    struct Engine
    {
        maxi::MaxDeque window;
        maxi::BoxAverage box;
        std::vector<double> rawGains;
        int rawMask = 0, averageLength = 1;
        double slow = 0.0, medium = 0.0, fast = 0.0, gain = 1.0;
        long long last = -1;

        void prepare (int maxWindow)
        {
            window.prepare (maxWindow + 2);
            box.prepare (maxWindow);
            rawGains.assign ((size_t) maxi::nextPow2 (maxWindow + 1), 1.0);
            rawMask = (int) rawGains.size() - 1;
        }
        void reset (int windowLength, long long lastIndex) noexcept
        {
            window.reset();
            box.setLength (windowLength);
            box.reset();
            slow = medium = fast = 0.0;
            gain = 1.0;
            last = lastIndex;
            averageLength = windowLength;
            std::fill (rawGains.begin(), rawGains.end(), 1.0);
        }

        // On a shrinking frontier no new detector sample arrives. Resize the average anyway, removing the oldest
        // value, instead of holding the previous output sample's gain. The next step uses the resized box state.
        void resizeAverage (int length) noexcept
        {
            if (length == averageLength)
                return;
            double sum = gain * averageLength;
            for (int j = length; j < averageLength; ++j)
                sum -= rawGains[(size_t) ((last - j) & rawMask)];
            for (int j = averageLength; j < length; ++j)
                sum += rawGains[(size_t) ((last - j) & rawMask)];
            box.setLength (length);
            averageLength = length;
            gain = sum / length;
        }

        double interpolatedGain (double length) const noexcept
        {
            if (averageLength == 1 || length == (double) averageLength)
                return gain;
            // Both averages START at the output sample: the shorter one omits the newest detector value.
            // Blending averages with a common end would move the gain in time whenever the frontier advances.
            const double shorter = (gain * averageLength - rawGains[(size_t) (last & rawMask)]) / (averageLength - 1);
            const double fraction = length - (averageLength - 1);
            return std::clamp (shorter + fraction * (gain - shorter), 0.0, 1.0);
        }

        // overDb = the overshoot of detector sample last + 1 (>= 0); windowLength = the current window.
        void step (double overDb, const ModeCoeffs& k, int mode, int windowLength) noexcept
        {
            ++last;
            double target = 0.0;
            if (overDb > 0.0)
                target = std::max (0.0, kDepthK[mode] * (overDb - kDepthB[mode] * std::pow (overDb, kDepthC[mode])));
            if (target > slow)
                slow += (target - slow) * k.kSlowAtt;
            else if (slow > 0.0)
            {
                const double q = slow / kDeepKneeDb;
                double qp = q;
                for (int i = 1; i < kDeepPower; ++i)
                    qp *= q;
                slow -= slow * k.slowRelPerSample / (1.0 + qp);
                if (slow < 1.0e-9)
                    slow = 0.0;
            }

            const double rest = std::max (0.0, overDb - slow * k.windowFloor);
            window.push (last, rest);
            window.dropOlderThan (last - windowLength + 1);
            const double h = window.max();
            const double mt = h * k.medShare;
            if (mt > medium)
                medium += (mt - medium) * k.kMedCharge;
            else
                medium *= k.kMedRelease;
            fast = std::max (h - medium, fast * k.kFastRelease);
            if (medium < 1.0e-9)
                medium = 0.0;
            if (fast < 1.0e-9)
                fast = 0.0;
            const double d = slow + medium + fast;
            box.setLength (windowLength);
            averageLength = windowLength;
            const double raw = d > 0.0 ? std::exp (-d * kLn10Over20) : 1.0;
            rawGains[(size_t) (last & rawMask)] = raw;
            gain = box.process (raw);
        }
    };
} // namespace

int ClassicLimiter::latencyFor (double sampleRate) noexcept
{
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    return (maxWindowFor (fs) - 1) + (guardAttackFor (fs) + maxi::kTpDetectorDelay + 6);
}

//==============================================================================
struct ClassicLimiter::Impl
{
    ClassicLimiterMeters meters;
    StereoSampleFifo history;

    double sr = 48000.0;
    bool prepared = false, snapNext = true;
    ClassicLimiterParams params;
    int latency = latencyFor (48000.0), latCore = 0, latGuard = 0, maxWindow = 1;

    // controls
    Ramp gain, ceiling;
    std::array<Ramp, 3> makeupRamp;                 // per mode (linear), glides with the gain
    std::array<double, 3> modeW { 1.0, 0.0, 0.0 };
    double modeStep = 1.0;
    float coeffCharacter = -1.0f;

    // gain computers [mode][channel]
    std::array<ModeCoeffs, 3> coeffs;
    std::array<double, 3> windowNow { 1.0, 1.0, 1.0 };   // fractional windows, one sample per windowStep
    int windowStep = 1;
    std::array<std::array<Engine, 2>, 3> engines;

    // brickwall guard
    maxi::Guard guard;

    // stamps (index = absolute input sample index)
    std::vector<double> ceilRing;
    std::vector<std::uint8_t> tpRing;
    std::array<std::vector<float>, 2> gxRing;      // G x (before the modes' make-up)
    std::array<std::array<std::vector<float>, 2>, 3> slowRing; // per mode and channel, at detector index
    std::vector<float> refRing[2];                 // G x make-up (Delta reference) at the output index
    std::array<std::vector<float>, 2> muRing;       // per-channel slow reduction (dB, <= 0) at the output index
    std::array<std::vector<double>, 3> makeupRing; // per mode: the make-up stamped on each input sample
    std::array<std::vector<double>, 2> stageGainRing;   // the modes' gain (re their make-up) at the output index
    long long ringMask = 0, counter = 0;

    // chunk buffers
    float stageBuf[2][kChunk] {}, outBuf[2][kChunk] {};
    double guardGainBuf[2][kChunk] {};

    // meters (per block / per history entry)
    double blockMuMinDb = 0.0, blockWallMinDb = 0.0, blockPeak = 0.0, blockTotalMinDb = 0.0;
    double histMuDb = 0.0, histTotalDb = 0.0;

    //==========================================================================
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxWindow = maxWindowFor (sr);
        for (auto& modeEngines : engines)
            for (auto& e : modeEngines)
                e.prepare (maxWindow);

        guard.prepare (sr, guardAttackFor (sr));
        guard.setMaxAttack (guardAttackFor (sr));
        guard.setTimes (guardAttackFor (sr), kGuardReleaseMs, 1.0);
        latCore = maxWindow - 1;
        latGuard = guard.getLatency();
        latency = latCore + latGuard;   // == latencyFor (sr) (tests check both and the measured delay)

        const int ringSize = maxi::nextPow2 (latency + maxWindow + 2 * kChunk + 64);
        ringMask = ringSize - 1;
        ceilRing.assign ((size_t) ringSize, 1.0);
        tpRing.assign ((size_t) ringSize, 0);
        for (auto& r : gxRing)
            r.assign ((size_t) ringSize, 0.0f);
        for (auto& mode : slowRing)
            for (auto& r : mode)
                r.assign ((size_t) ringSize, 0.0f);
        for (auto& r : refRing)
            r.assign ((size_t) ringSize, 0.0f);
        for (auto& r : muRing)
            r.assign ((size_t) ringSize, 0.0f);
        for (auto& r : makeupRing)
            r.assign ((size_t) ringSize, 1.0);
        for (auto& r : stageGainRing)
            r.assign ((size_t) ringSize, 1.0);

        gain.length = ceiling.length = std::max (1, (int) std::lround (kGlideMs * 1.0e-3 * sr));
        for (auto& r : makeupRamp)
            r.length = gain.length;
        modeStep = 1.0 / std::max (1.0, kModeFadeMs * 1.0e-3 * sr);
        windowStep = std::max (1, (int) std::lround (kWindowStepMs * 1.0e-3 * sr));
        history.prepare (maxi::nextPow2 (std::max (1 << 12, (int) (sr / kChunk) * 4)));
        prepared = true;
        coeffCharacter = -1.0f;
        updateCoeffs();
        resetAll();
    }

    double gainTarget() const noexcept { return std::pow (10.0, (params.ceilingDb - params.thresholdDb) / 20.0); }
    double ceilingTarget() const noexcept { return std::pow (10.0, params.ceilingDb / 20.0); }
    double makeupTarget (int mode) const noexcept
    {
        const double share = std::clamp ((double) (params.ceilingDb - params.thresholdDb) / kMakeupFullDb, 0.0, 1.0);
        return std::pow (10.0, kMakeupDb[mode] * share / 20.0);
    }

    void updateCoeffs() noexcept
    {
        if (params.character == coeffCharacter)
            return;
        coeffCharacter = params.character;
        const double c = (double) params.character;
        for (int m = 0; m < 3; ++m)
        {
            auto& k = coeffs[(size_t) m];
            k.window = windowFor (m, c, sr);
            k.kSlowAtt = 1.0 - decayFor (anchors (kSlowAttackMs[m], c), sr);
            if (anchors (kSlowAttackMs[m], c) <= 0.0)
                k.kSlowAtt = 1.0;
            const double tau = kTauS[m][0] + c * (kTauS[m][1] + c * kTauS[m][2]);
            k.slowRelPerSample = 1.0 / (tau * sr);
            k.windowFloor = std::exp (-(double) k.window * k.slowRelPerSample);
            const double tc = anchors (kMediumChargeMs[m], c);
            k.kMedCharge = tc > 0.0 ? 1.0 - decayFor (tc, sr) : 0.0;
            k.medShare = anchors (kMediumShare[m], c);
            k.kMedRelease = decayFor (anchors (kMediumReleaseMs[m], c), sr);
            k.kFastRelease = decayFor (anchors (kFastReleaseMs[m], c), sr);
        }
    }

    void snapControls() noexcept
    {
        gain.snap (gainTarget());
        ceiling.snap (ceilingTarget());
        for (int m = 0; m < 3; ++m)
            makeupRamp[(size_t) m].snap (makeupTarget (m));
        modeW.fill (0.0);
        modeW[(size_t) params.mode] = 1.0;
        for (int m = 0; m < 3; ++m)
            windowNow[(size_t) m] = coeffs[(size_t) m].window;
    }

    void resetAll() noexcept
    {
        if (! prepared)
            return;
        snapControls();
        // the first step (index 0) computes output sample -latCore, whose detector frontier is -latCore + window - 1
        for (int m = 0; m < 3; ++m)
            for (auto& e : engines[(size_t) m])
                e.reset ((int) windowNow[(size_t) m], (long long) (windowNow[(size_t) m] - 2 - latCore));
        guard.reset();
        std::fill (ceilRing.begin(), ceilRing.end(), ceiling.current);
        std::fill (tpRing.begin(), tpRing.end(), (std::uint8_t) (params.truePeak ? 1 : 0));
        for (auto& r : gxRing)
            std::fill (r.begin(), r.end(), 0.0f);
        for (auto& mode : slowRing)
            for (auto& r : mode)
                std::fill (r.begin(), r.end(), 0.0f);
        for (auto& r : refRing)
            std::fill (r.begin(), r.end(), 0.0f);
        for (auto& r : muRing)
            std::fill (r.begin(), r.end(), 0.0f);
        for (int m = 0; m < 3; ++m)
            std::fill (makeupRing[(size_t) m].begin(), makeupRing[(size_t) m].end(), makeupRamp[(size_t) m].current);
        for (auto& r : stageGainRing)
            std::fill (r.begin(), r.end(), 1.0);
        counter = 0;
        histMuDb = histTotalDb = 0.0;
        snapNext = true;
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
        updateCoeffs();
        if (snapNext)
        {
            snapControls();
            for (int m = 0; m < 3; ++m)
                for (auto& e : engines[(size_t) m])
                    e.reset ((int) windowNow[(size_t) m], (long long) (windowNow[(size_t) m] - 2 - latCore));
            std::fill (ceilRing.begin(), ceilRing.end(), ceiling.current);
            std::fill (tpRing.begin(), tpRing.end(), (std::uint8_t) (params.truePeak ? 1 : 0));
            for (int m = 0; m < 3; ++m)
                std::fill (makeupRing[(size_t) m].begin(), makeupRing[(size_t) m].end(), makeupRamp[(size_t) m].current);
            snapNext = false;
        }
        else
        {
            gain.setTarget (gainTarget());
            ceiling.setTarget (ceilingTarget());
            for (int m = 0; m < 3; ++m)
                makeupRamp[(size_t) m].setTarget (makeupTarget (m));
        }
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
        const double margin = std::pow (10.0, -kCeilingMarginDb / 20.0);

        // gain computers (per mode and channel), blended by the mode weights
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

            const double G = gain.next();
            const double C = ceiling.next();
            const auto slot = (size_t) (idx & ringMask);
            ceilRing[slot] = C;
            tpRing[slot] = tp;
            gxRing[0][slot] = (float) ((double) io[0][i] * G);
            gxRing[1][slot] = (float) ((double) io[1][i] * G);
            for (int m = 0; m < 3; ++m)
                makeupRing[(size_t) m][slot] = makeupRamp[(size_t) m].next();

            const long long out = idx - latCore;   // the output sample of this step
            const auto os = (size_t) (out & ringMask);
            double g[2] { 0.0, 0.0 };
            double makeup = 0.0, slowDb[2] {};
            for (int m = 0; m < 3; ++m)
            {
                const auto& k = coeffs[(size_t) m];
                double& wNow = windowNow[(size_t) m];
                wNow += std::clamp ((double) k.window - wNow, -1.0 / windowStep, 1.0 / windowStep);
                const int windowLength = (int) std::ceil (wNow);
                const long long frontier = out + windowLength - 1;
                auto& pair = engines[(size_t) m];
                // The initial frontier is set when controls snap. Thereafter it advances by 0, 1 or 2 samples:
                // windowNow moves by at most one sample, so this loop never has a block-sized catch-up.
                while (pair[0].last < frontier)
                {
                    const long long d = pair[0].last + 1;
                    const auto ds = (size_t) (d & ringMask);
                    const double ct = ceilRing[ds] * margin;
                    for (int c = 0; c < 2; ++c)
                    {
                        const double a = std::abs ((double) gxRing[(size_t) c][ds]) * makeupRing[(size_t) m][ds];
                        pair[(size_t) c].step (a > ct ? kTwentyOverLn10 * std::log (a / ct) : 0.0, k, m, windowLength);
                        slowRing[(size_t) m][(size_t) c][ds] = (float) pair[(size_t) c].slow;
                    }
                }
                const double mk = makeupRing[(size_t) m][os];
                for (int c = 0; c < 2; ++c)
                {
                    auto& e = pair[(size_t) c];
                    e.resizeAverage (windowLength);
                    g[c] += modeW[(size_t) m] * mk * e.interpolatedGain (wNow);
                    slowDb[c] -= modeW[(size_t) m] * (double) slowRing[(size_t) m][(size_t) c][os];
                }
                makeup += modeW[(size_t) m] * mk;
            }
            for (int c = 0; c < 2; ++c)
            {
                const double x = (double) gxRing[(size_t) c][os];
                refRing[c][os] = (float) (x * makeup);
                stageBuf[c][i] = (float) (x * g[c]);
                stageGainRing[(size_t) c][os] = makeup > 0.0 ? g[c] / makeup : 1.0;
                muRing[(size_t) c][os] = (float) slowDb[c];
            }
        }

        // guard: true peak (True Peak on) or sample peak at the ceiling
        for (int i = 0; i < n; ++i)
        {
            const long long idx = idx0 + i;
            float* stage[2] { stageBuf[0] + i, stageBuf[1] + i };
            float* out[2] { outBuf[0] + i, outBuf[1] + i };
            guard.minGain[0] = guard.minGain[1] = 1.0;
            const auto gs = (size_t) ((idx - latCore - maxi::kTpDetectorDelay) & ringMask);
            const double guardThreshold = ceilRing[gs] * (tpRing[gs] != 0 ? kGuardTpMargin : 1.0);
            guard.process (stage, out, &guardThreshold, &tpRing[gs], 1);
            guardGainBuf[0][i] = guard.minGain[0];
            guardGainBuf[1][i] = guard.minGain[1];
        }

        // clamp, delta, output, meters
        for (int i = 0; i < n; ++i)
        {
            const auto oi = (size_t) ((idx0 + i - latency) & ringMask);
            const float c = floatAtMost (ceilRing[oi]);
            double totalDb = 0.0, slowDb = 0.0, wallDb = 0.0;
            for (int ch = 0; ch < 2; ++ch)
            {
                const double magnitude = std::abs ((double) outBuf[ch][i]);
                const double clampGain = magnitude > (double) c ? (double) c / magnitude : 1.0;
                const float y = std::clamp (outBuf[ch][i], -c, c);
                const float dy = y - refRing[(size_t) ch][oi];
                if (deltaOut != nullptr)
                    deltaOut[ch][i] = dy;
                io[ch][i] = deltaInPlace ? dy : y;
                blockPeak = std::max (blockPeak, (double) std::abs (y));
                const double total = stageGainRing[(size_t) ch][oi] * guardGainBuf[ch][i] * clampGain;
                const double channelTotal = std::min (0.0, kTwentyOverLn10 * std::log (std::max (total, 1.0e-10)));
                const double channelSlow = std::max ((double) muRing[(size_t) ch][oi], channelTotal);
                totalDb = std::min (totalDb, channelTotal);
                slowDb = std::min (slowDb, channelSlow);
                wallDb = std::min (wallDb, channelTotal - channelSlow);
            }
            // the total is the applied gain; the slow share is capped by it (the box average of a releasing slow
            // stage can sit a hair above its value at the output sample)
            blockMuMinDb = std::min (blockMuMinDb, slowDb);
            blockWallMinDb = std::min (blockWallMinDb, wallDb);
            blockTotalMinDb = std::min (blockTotalMinDb, totalDb);
            histTotalDb = std::min (histTotalDb, totalDb);
            histMuDb = std::min (histMuDb, slowDb);
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
        blockWallMinDb = 0.0;
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
        meters.brickwallReductionDb.store ((float) blockWallMinDb);
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
