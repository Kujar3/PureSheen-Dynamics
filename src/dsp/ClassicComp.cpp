#include "dsp/ClassicComp.h"

#include "dsp/Filters.h"

#include <algorithm>
#include <cmath>
#include <complex>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_CCMP_HAS_SSE 1
#endif

// Classic Compressor (mod-ccmp). Contract and signal flow: ClassicComp.h. The model and every constant below are
// fitted to the reference's renders (black-box measurement only; the fit, its data and the error tables live in the
// private out/vintage notes). What the measurements decided:
//
// - Feedback: the detector reads the gain-stage output of the previous sample. The reduction ripple of a steady tone
//   grows with ratio - 1 (ratio 2 / 4 / 20: 0.28 / 0.83 / 5.0 dB peak to peak at 100 Hz), the feedback signature, and
//   the static slope is exactly 1 / ratio with c = ratio - 1.
// - Detector = a diode peak detector: V1 charges toward |d| (only while |d| > V1) with the attack time constant and
//   discharges toward zero all the time (also while charging) with the release time constant; a slow capacitor V2 is
//   coupled to V1 (V2 follows V1 with t2, and loads V1 m times as strongly). The charge into V2 delays a fresh onset
//   and slowly lets the reading creep up during held reduction (+1.7 dB of reduction over 1 s at attack 20 / release
//   50), and V2 feeds V1 back after a long hold, so a long stretch releases slower than a short hit (the reference's
//   program-dependent release). A steady sine reads below its peak by the charge / discharge balance (the static
//   threshold offset: 0.4 dB at attack 1, 2.6 dB at attack 20, 4.6 dB at attack 50, release 50), and the threshold is
//   the knob on that reading exactly.
// - The charge step is capped at the loop's dead-beat value 1 / (1 + slope) (slope = the gain computer's local dB / dB
//   slope). This is a local linear bound, not a guarantee against nonlinear onset overshoot; at the fastest attack
//   it matches the reference's fast-attack render better than the uncapped step.
// - The sidechain filters the gain-stage output itself, d = EQ (x g): filtering the input and applying g after it
//   (d = g EQ (x)) was tried and matches the reference clearly worse wherever the gain moves within a cycle (the
//   -10 dB sweep below 50 Hz: 0.16 -> 1.7 dB; ratio 20 on bass-heavy music: null -31 -> -18 dB). The cost is shared
//   with the reference's structure: a tone at or below the detector high-pass corner (or below a raised high-pass),
//   at ratio >= 10 with attack <= 5 ms, makes the gain modulation through the filter memory non-periodic. The
//   output stays bounded and finite (tests: ccmp. fastest feedback loop), sensible settings stay periodic.
// - Sidechain: 1st-order high-pass at the knob (bilinear; the default corner is measured at 40 Hz), a bell with half
//   the knob's gain at Q 0.24 and a high shelf with twice the knob's gain at Q 0.45 (both bilinear peaking / shelving
//   biquads at the knob frequency), on the set's mean gain-stage output (Stereo: (L + R) / 2, one gain for both).
// - Knees: Sharp and Balanced quadratic (0.29 / 1.40 dB wide, centred on the threshold); Smooth a softplus knee
//   c W ln (1 + e^((L - T') / W)), W 1.79 dB, T' = threshold + 10 dB, with the same ballistics.
// - Auto gain: 20 log10 (A / B), A / B = one-pole (500 ms) averages of |input| and |gain-stage output| (mean over the
//   set's channels), clamped 0..+20 dB; in silence both decay alike, so the ratio holds ("frozen").
// - No saturator: the odd harmonics (h3 -51 dBc at 1 kHz, -32 dBc at 100 Hz at 0 dBFS, Balanced, ratio 4) are the
//   detector ripple modulating the gain, and come out of the model as such; no even harmonics.
//
// Our choices where the renders cannot decide: the laws in Release (fitted through 30 / 50 / 100 / 150 ms, the whole
// range) are smooth exp-quadratics in ln (Release); attack is linear in the knob (fitted 1..50 ms, extrapolated to
// 0.1 and 100 ms, which the auto-gain renders support); bell / shelf cuts mirror the boosts; at other sample rates the
// time constants are kept in seconds and the filters designed at the knob frequency.
//
// Parameter glides (click-free): threshold and ratio (c) glide through two one-poles in series (S-shaped, 95 % after
// kLoopGlideMs: the reduction follows c (L - T) at once and the detector only discharges at the release rate, so a
// faster glide would dip the gain); the quadratic knee width, the Smooth share (a mode change crossfades the two
// knees), makeup and the auto-gain share are one-pole glides per sample (95 % after kParamGlideMs); the attack time
// and ln (Release) glide in the log domain per sample (the coefficients follow per sample while they move); the
// sidechain filters through SmoothedBiquad over kParamGlideMs. Values snap to their exact target within 1e-9, so
// neutral settings are exact (bit-exact output).
// Detector solo: a second set of sidechain filters runs on the input only while the solo is heard (reset and snapped
// when it starts), crossfaded with the normal output by a 20 ms linear ramp.
// Meters (published once per block): reduction at the block end, the deepest of the last ~1 s (held 1 s, then it
// follows the block minimum), the detector reading, the applied auto gain.
namespace ps
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kLn10Over20 = 0.11512925464970229;   // ln (10) / 20
    constexpr double k20OverLn10 = 8.6858896380650366;    // 20 / ln (10)
    constexpr int kChunk = 64;                            // internal chunk (solo dry copy)
    constexpr int kBothChannels[2] { 0, 1 };
    constexpr int kChannel0[1] { 0 };
    constexpr int kChannel1[1] { 1 };

    //==========================================================================
    // Fitted coefficients (the reference's renders; out/vintage fit notes)

    // detector: V1 charge time = kChargePerAttack x Attack; release laws ln (tau) = a + b v + c v^2 with
    // v = ln (Release / kReleaseRefMs) for the V1 discharge (79 / 88 / 112 / 135 ms at Release 30 / 50 / 100 / 150),
    // the slow capacitor's time t2 (189 / 281 / 441 / 550 ms) and its load ratio m (9.4 / 7.4 / 4.3 / 2.8)
    constexpr double kChargePerAttack = 0.46467;
    constexpr double kReleaseRefMs = 50.0;
    constexpr double kDischargeLaw[3] { -2.43139, 0.27245, 0.10795 };   // ln (seconds)
    constexpr double kSlowTimeLaw[3] { -1.27058, 0.72291, -0.10110 };   // ln (seconds)
    constexpr double kSlowLoadLaw[3] { 1.99635, -0.60582, -0.24714 };   // ln (ratio)
    constexpr double kLevelFloor = 1.0e-12;               // detector reading floor (-240 dB: no reduction)

    // sidechain
    constexpr double kBellGainScale = 0.5;                // the bell gives half the knob's dB ...
    constexpr double kBellQ = 0.23958;                    // ... at this (wide) Q
    constexpr double kShelfGainScale = 2.0;               // the shelf gives twice the knob's dB ...
    constexpr double kShelfQ = 0.45151;                   // ... at this Q

    // gain computer
    constexpr double kKneeDb[3] { 0.29287, 1.40196, 1.7877 };   // Sharp, Balanced (quadratic), Smooth (softplus)
    constexpr double kSmoothThresholdDb = 10.0;          // Smooth's knee sits this far above the threshold
    constexpr double kMaxReductionDb = 80.0;

    // auto gain
    constexpr double kAutoGainMs = 500.0;                 // the |input| / |output| averages
    constexpr double kAutoGainMaxDb = 20.0;
    constexpr double kAutoGainFloor = 1.0e-20;            // below this the ratio holds its last value

    // glides, monitor, meters
    constexpr double kParamGlideMs = 20.0;                // 95 % of a step after this
    constexpr double kLoopGlideMs = 100.0;                // threshold and ratio: 95 % after this, S-shaped
    constexpr double kTwoPoleT95 = 4.7439;                // a critically damped 2-pole reaches 95 % at 4.74 tau
    constexpr double kSoloFadeMs = 20.0;
    constexpr float kMaxInput = 1.0e6f;                  // reject corrupt audio before recursive histories
    constexpr double kGrHoldMs = 1000.0;

    //==========================================================================
    inline double poleFor (double ms, double sr) noexcept
    {
        const double samples = ms * 0.001 * sr;
        return samples > 0.0 ? std::exp (-1.0 / samples) : 0.0;
    }

    inline void flushTiny (double& v) noexcept
    {
        if (std::abs (v) < 1.0e-30)
            v = 0.0;
    }

    inline void glide (double& v, double target, double pole) noexcept
    {
        if (v == target)
            return;
        v = target + pole * (v - target);
        if (std::abs (v - target) < 1.0e-9)
            v = target;
    }

    inline int modeIndex (ClassicCompMode m) noexcept { return std::clamp ((int) m, 0, 2); }

    inline double law (const double (&c)[3], double v) noexcept { return std::exp (c[0] + c[1] * v + c[2] * v * v); }

    inline double releaseVar (double releaseMs) noexcept { return std::log (std::max (1.0, releaseMs) / kReleaseRefMs); }

    inline double chargeSeconds (double attackMs) noexcept { return std::max (1.0e-6, kChargePerAttack * attackMs * 0.001); }

    // Knee shapes (without the c factor): value and dB / dB slope at d = L - threshold.
    inline double quadKnee (double d, double w, double& slope) noexcept
    {
        const double u = d + 0.5 * w;
        if (u <= 0.0)
        {
            slope = 0.0;
            return 0.0;
        }
        if (u >= w)
        {
            slope = 1.0;
            return d;
        }
        slope = u / w;
        return u * u / (2.0 * w);
    }

    inline double softKnee (double d, double w, double& slope) noexcept
    {
        const double u = d / w;
        if (u < -40.0)
        {
            slope = 0.0;
            return 0.0;
        }
        if (u > 40.0)
        {
            slope = 1.0;
            return d;
        }
        const double e = std::exp (-std::abs (u));
        slope = u > 0.0 ? 1.0 / (1.0 + e) : e / (1.0 + e);
        return w * ((u > 0.0 ? u : 0.0) + std::log1p (e));
    }

    // Bilinear designs at the knob frequency (the shapes the fit found; Filters.h's designs are matched instead).
    inline double prewarp (double sr, double hz) noexcept
    {
        return std::tan (kPi * std::clamp (hz, 1.0, 0.49 * sr) / sr);
    }

    BiquadCoeffs highPass1Bilinear (double sr, double hz) noexcept
    {
        const double k = prewarp (sr, hz);
        BiquadCoeffs c;
        c.b0 = 1.0 / (1.0 + k);
        c.b1 = -c.b0;
        c.b2 = 0.0;
        c.a1 = (k - 1.0) / (1.0 + k);
        c.a2 = 0.0;
        return c;
    }

    BiquadCoeffs peakBilinear (double sr, double hz, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * std::clamp (hz, 1.0, 0.49 * sr) / sr;
        const double cw = std::cos (w), al = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + al / A;
        BiquadCoeffs c;
        c.b0 = (1.0 + al * A) / a0;
        c.b1 = -2.0 * cw / a0;
        c.b2 = (1.0 - al * A) / a0;
        c.a1 = -2.0 * cw / a0;
        c.a2 = (1.0 - al / A) / a0;
        return c;
    }

    BiquadCoeffs highShelfBilinear (double sr, double hz, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * std::clamp (hz, 1.0, 0.49 * sr) / sr;
        const double cw = std::cos (w), al = std::sin (w) / (2.0 * q), sA = 2.0 * std::sqrt (A) * al;
        const double a0 = (A + 1.0) - (A - 1.0) * cw + sA;
        BiquadCoeffs c;
        c.b0 = A * ((A + 1.0) + (A - 1.0) * cw + sA) / a0;
        c.b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw) / a0;
        c.b2 = A * ((A + 1.0) + (A - 1.0) * cw - sA) / a0;
        c.a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw) / a0;
        c.a2 = ((A + 1.0) - (A - 1.0) * cw - sA) / a0;
        return c;
    }

    // The sidechain designs of one set, for the sample rate.
    struct ScDesign
    {
        BiquadCoeffs c[3];
    };

    ScDesign designSidechain (const ClassicCompSetParams& p, double sr) noexcept
    {
        ScDesign d;
        d.c[0] = highPass1Bilinear (sr, (double) p.scHighPassHz);
        d.c[1] = peakBilinear (sr, (double) p.scBellHz, kBellQ, kBellGainScale * (double) p.scBellDb);
        d.c[2] = highShelfBilinear (sr, (double) p.scShelfHz, kShelfQ, kShelfGainScale * (double) p.scShelfDb);
        return d;
    }

    bool sameSidechain (const ClassicCompSetParams& a, const ClassicCompSetParams& b) noexcept
    {
        return a.scHighPassHz == b.scHighPassHz && a.scBellHz == b.scBellHz && a.scBellDb == b.scBellDb
            && a.scShelfHz == b.scShelfHz && a.scShelfDb == b.scShelfDb;
    }

    // Steady-sine reading of the detector relative to the sine's peak (dB, <= 0) for charge / discharge times ta, tr:
    // per cycle the charge while |sin| > v balances the discharge, v rho pi = 2 cos (theta) - v (pi - 2 theta),
    // sin (theta) = v, rho = ta / tr (time constants long against the period; V2 carries no current at rest).
    double sineReadingDb (double ta, double tr) noexcept
    {
        const double rho = ta / std::max (1.0e-9, tr);
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < 60; ++i)
        {
            const double v = 0.5 * (lo + hi);
            const double f = v * rho * kPi - 2.0 * std::sqrt (std::max (0.0, 1.0 - v * v)) + v * (kPi - 2.0 * std::asin (v));
            (f > 0.0 ? hi : lo) = v;
        }
        return 20.0 * std::log10 (std::max (1.0e-6, 0.5 * (lo + hi)));
    }
} // namespace

//==============================================================================
struct ClassicComp::Impl
{
    ClassicCompMeters meters;

    double sr = 48000.0;
    bool prepared = false;
    bool snapNext = true;            // the first setParams after prepare / reset jumps to its values
    ClassicCompParams params;
    ClassicCompParams designedFor;   // sidechain settings the designs below belong to
    bool designValid[kNumChannelSets] { false, false };
    ScDesign designs[kNumChannelSets];

    // coefficients
    double cGlide = 0.0, cLoop = 0.0, cAg = 0.0, soloStep = 0.0;
    int holdSamples = 1;

    struct SetDsp
    {
        // controls: current (gliding) and target
        double thr = -12.0, thrMid = -12.0, thrT = -12.0;   // two-stage glide (thrMid: the first stage)
        double c = 0.5, cMid = 0.5, cT = 0.5;
        double knee = kKneeDb[1], kneeT = kKneeDb[1];   // quadratic knee width
        double smooth = 0.0, smoothT = 0.0;             // share of the Smooth (softplus) knee
        double makeup = 1.0, makeupT = 1.0;
        double agMix = 1.0, agMixT = 1.0;               // share of the auto gain applied
        double logAtt = 0.0, logAttT = 0.0;             // ln (V1 charge seconds)
        double relV = 0.0, relVT = 0.0;                 // ln (Release / 50 ms)
        // derived coefficients
        double kCharge = 0.0, kDischarge = 0.0, kSlow = 0.0, slowLoad = 0.0;
        // detector (per set: the set's mean gain-stage output)
        SmoothedBiquad det[3];
        double v1 = 0.0, v2 = 0.0, levelDb = -240.0, grDb = 0.0;
        // auto gain
        double agIn = 0.0, agOut = 0.0, agDb = 0.0;
        // meters
        double holdDb = 0.0;
        int holdLeft = 0;
    };
    SetDsp sets[kNumChannelSets];

    struct ChannelDsp
    {
        SmoothedBiquad listen[3];
        int set = 0;             // the set whose sidechain designs the listen filters follow
        bool targetsSet = false;
    };
    ChannelDsp chans[2];
    bool detTargetsSet[kNumChannelSets] { false, false };

    // detector solo
    bool soloOn = false;
    double soloFade = 0.0;
    float dry[2][kChunk] {};

    //==========================================================================
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        cGlide = poleFor (kParamGlideMs / 3.0, sr);            // 95 % after kParamGlideMs
        cLoop = poleFor (kLoopGlideMs / kTwoPoleT95, sr);      // per stage, 95 % after kLoopGlideMs
        cAg = 1.0 - poleFor (kAutoGainMs, sr);
        soloStep = 1.0 / (kSoloFadeMs * 0.001 * sr);
        holdSamples = std::max (1, (int) std::lround (kGrHoldMs * 0.001 * sr));
        const int ramp = std::max (1, (int) std::lround (kParamGlideMs * 0.001 * sr));
        for (auto& st : sets)
            for (auto& f : st.det)
                f.setRampLength (ramp);
        for (auto& ch : chans)
            for (auto& f : ch.listen)
                f.setRampLength (ramp);
        designValid[0] = designValid[1] = false;
        prepared = true;
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : sets)
        {
            s.v1 = s.v2 = 0.0;
            s.levelDb = -240.0;
            s.grDb = 0.0;
            s.agIn = s.agOut = s.agDb = 0.0;
            s.holdDb = 0.0;
            s.holdLeft = 0;
            for (auto& f : s.det)
                f.reset();
        }
        for (auto& ch : chans)
        {
            for (auto& f : ch.listen)
                f.reset();
            ch.targetsSet = false;
        }
        detTargetsSet[0] = detTargetsSet[1] = false;
        soloFade = soloOn ? 1.0 : 0.0;
        snapNext = true;
        publishIdleMeters (0);
        publishIdleMeters (1);
    }

    void publishIdleMeters (int s) noexcept
    {
        meters.gainReductionDb[s].store (0.0f);
        meters.gainReductionHoldDb[s].store (0.0f);
        meters.sidechainDb[s].store (-100.0f);
        meters.autoGainDb[s].store (0.0f);
    }

    //==========================================================================
    void updateCharge (SetDsp& st) const noexcept
    {
        st.kCharge = 1.0 - std::exp (-1.0 / (std::exp (st.logAtt) * sr));
    }

    void updateRelease (SetDsp& st) const noexcept
    {
        st.kDischarge = 1.0 - std::exp (-1.0 / (law (kDischargeLaw, st.relV) * sr));
        st.kSlow = 1.0 - std::exp (-1.0 / (law (kSlowTimeLaw, st.relV) * sr));
        st.slowLoad = law (kSlowLoadLaw, st.relV);
    }

    void setParams (ParamValues values) noexcept
    {
        readClassicCompParams (values, params);
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            const auto& p = params.sets[s];
            auto& st = sets[s];
            const int m = modeIndex (p.mode);
            st.thrT = std::clamp ((double) p.thresholdDb, -60.0, 0.0);
            st.cT = std::clamp ((double) p.ratio - 1.0, 0.0, 19.0);
            if (m != 2)
                st.kneeT = kKneeDb[m];
            st.smoothT = m == 2 ? 1.0 : 0.0;
            st.makeupT = std::exp ((double) std::clamp (p.makeupDb, -30.0f, 30.0f) * kLn10Over20);
            st.agMixT = p.autoGain ? 1.0 : 0.0;
            st.logAttT = std::log (chargeSeconds (std::clamp ((double) p.attackMs, 0.1, 100.0)));
            st.relVT = releaseVar (std::clamp ((double) p.releaseMs, 30.0, 150.0));
            if (! designValid[s] || ! sameSidechain (p, designedFor.sets[s]))
            {
                designs[s] = designSidechain (p, sr);
                designedFor.sets[s] = p;
                designValid[s] = true;
                detTargetsSet[s] = false;
                for (auto& ch : chans)
                    if (ch.set == s)
                        ch.targetsSet = false;
            }
        }
        if (snapNext)
        {
            for (auto& st : sets)
            {
                st.thr = st.thrMid = st.thrT;
                st.c = st.cMid = st.cT;
                st.knee = st.kneeT;
                st.smooth = st.smoothT;
                st.makeup = st.makeupT;
                st.agMix = st.agMixT;
                st.logAtt = st.logAttT;
                st.relV = st.relVT;
                updateCharge (st);
                updateRelease (st);
            }
        }
    }

    // Points the set's detector filters at its designs (glides, or jumps when snapping).
    void updateDetectorTargets (int s, bool snap) noexcept
    {
        if (detTargetsSet[s] && ! snap)
            return;
        for (int k = 0; k < 3; ++k)
        {
            sets[s].det[k].setTarget (designs[s].c[k]);
            if (snap)
                sets[s].det[k].snapToTarget();
        }
        detTargetsSet[s] = true;
    }

    // Points the channel's listen filters at the designs of `set`.
    void updateListenTargets (int c, int set, bool snap) noexcept
    {
        auto& ch = chans[c];
        if (ch.targetsSet && ch.set == set && ! snap)
            return;
        ch.set = set;
        for (int k = 0; k < 3; ++k)
        {
            ch.listen[k].setTarget (designs[set].c[k]);
            if (snap)
                ch.listen[k].snapToTarget();
        }
        ch.targetsSet = true;
    }

    //==========================================================================
    // One chunk (n <= kChunk) of one set over its channels (Stereo: 0 and 1 linked; M/S: one channel each).
    void runSet (int s, float* const* io, const int* chIdx, int numCh, int start, int n) noexcept
    {
        auto& st = sets[s];
        const double invChannels = 1.0 / (double) numCh;

        double minR = st.grDb;
        for (int i = 0; i < n; ++i)
        {
            if (st.logAtt != st.logAttT)
            {
                glide (st.logAtt, st.logAttT, cGlide);
                updateCharge (st);
            }
            if (st.relV != st.relVT)
            {
                glide (st.relV, st.relVT, cGlide);
                updateRelease (st);
            }
            glide (st.thrMid, st.thrT, cLoop);
            glide (st.thr, st.thrMid, cLoop);
            glide (st.cMid, st.cT, cLoop);
            glide (st.c, st.cMid, cLoop);
            glide (st.knee, st.kneeT, cGlide);
            glide (st.smooth, st.smoothT, cGlide);
            glide (st.makeup, st.makeupT, cGlide);
            glide (st.agMix, st.agMixT, cGlide);

            // gain computer on the detector reading (the gain-stage output up to the previous sample)
            const double levelDb = st.v1 > kLevelFloor ? k20OverLn10 * std::log (st.v1) : -240.0;
            st.levelDb = levelDb;
            double gr = 0.0, slope = 0.0;
            if (st.c > 0.0)
            {
                const double d = levelDb - st.thr;
                double kq = 0.0, sq = 0.0, ks = 0.0, ss = 0.0;
                if (st.smooth < 1.0)
                    kq = quadKnee (d, st.knee, sq);
                if (st.smooth > 0.0)
                    ks = softKnee (d - kSmoothThresholdDb, kKneeDb[2], ss);
                gr = std::min (kMaxReductionDb, st.c * (kq + st.smooth * (ks - kq)));
                slope = st.c * (sq + st.smooth * (ss - sq));
            }
            st.grDb = -gr;
            minR = std::min (minR, -gr);
            const double g = gr == 0.0 ? 1.0 : std::exp (-gr * kLn10Over20);

            // gain stage, detector input and the auto-gain averages
            double det = 0.0, absIn = 0.0, absOut = 0.0;
            for (int k = 0; k < numCh; ++k)
            {
                const double x = (double) io[chIdx[k]][start + i];
                const double v = g == 1.0 ? x : x * g;
                det += v;
                absIn += std::abs (x);
                absOut += std::abs (v);
            }
            det *= invChannels;
            st.agIn += cAg * (absIn * invChannels - st.agIn);
            st.agOut += cAg * (absOut * invChannels - st.agOut);
            if (st.agIn > kAutoGainFloor && st.agOut > kAutoGainFloor)
                st.agDb = std::clamp (k20OverLn10 * std::log (st.agIn / st.agOut), 0.0, kAutoGainMaxDb);

            // detector: sidechain EQ, diode peak detector with the slow capacitor (read at the next sample)
            for (auto& f : st.det)
                det = f.processSample (det);
            const double e = std::abs (det);
            st.v1 -= st.v1 * st.kDischarge;
            if (e > st.v1)
                st.v1 += (e - st.v1) * std::min (st.kCharge, 1.0 / (1.0 + slope));
            const double flow = (st.v1 - st.v2) * st.kSlow;
            st.v2 += flow;
            st.v1 -= flow * st.slowLoad;
            if (st.v1 < 0.0)
                st.v1 = 0.0;

            // output: gain stage, auto gain, makeup
            const double agGain = st.agMix > 0.0 && st.agDb > 0.0 ? std::exp (st.agMix * st.agDb * kLn10Over20) : 1.0;
            const double outGain = g * agGain * st.makeup;
            for (int k = 0; k < numCh; ++k)
            {
                float* const data = io[chIdx[k]] + start;
                data[i] = (float) (outGain == 1.0 ? (double) data[i] : (double) data[i] * outGain);
            }
        }

        // 1 s hold of the deepest reduction
        if (minR <= st.holdDb || st.holdLeft <= 0)
        {
            if (minR <= st.holdDb)
                st.holdLeft = holdSamples;
            st.holdDb = minR;
        }
        else
        {
            st.holdLeft -= n;
        }

        // flush long-lived states (FTZ covers the rest)
        flushTiny (st.v1);
        flushTiny (st.v2);
        flushTiny (st.agIn);
        flushTiny (st.agOut);
    }

    //==========================================================================
    void process (float* const* io, int numSamples, ChannelMode mode) noexcept
    {
        // Keep corrupt samples out of the detector, auto gain and listen filters.
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < numSamples; ++i)
                if (! std::isfinite (io[c][i]) || std::abs (io[c][i]) > kMaxInput)
                    io[c][i] = 0.0f;
        const bool linked = mode == ChannelMode::Stereo;
        const bool snap = snapNext;
        snapNext = false;
        updateDetectorTargets (0, snap);
        updateDetectorTargets (1, snap);
        updateListenTargets (0, 0, snap);
        updateListenTargets (1, linked ? 0 : 1, snap);

        for (int start = 0; start < numSamples; start += kChunk)
        {
            const int n = std::min (kChunk, numSamples - start);
            const bool listen = soloOn || soloFade > 0.0;
            if (listen)
                for (int c = 0; c < 2; ++c)
                    std::copy (io[c] + start, io[c] + start + n, dry[c]);

            if (linked)
            {
                runSet (0, io, kBothChannels, 2, start, n);
            }
            else
            {
                runSet (0, io, kChannel0, 1, start, n);
                runSet (1, io, kChannel1, 1, start, n);
            }

            if (listen)
            {
                double f = soloFade;
                for (int i = 0; i < n; ++i)
                {
                    f = soloOn ? std::min (1.0, f + soloStep) : std::max (0.0, f - soloStep);
                    for (int c = 0; c < 2; ++c)
                    {
                        double d = (double) dry[c][i];
                        for (auto& flt : chans[c].listen)
                            d = flt.processSample (d);
                        float* const data = io[c] + start;
                        data[i] = (float) ((double) data[i] + f * (d - (double) data[i]));
                    }
                }
                soloFade = f;
            }
        }

        // meters
        const int numSets = linked ? 1 : 2;
        for (int s = 0; s < numSets; ++s)
        {
            const auto& st = sets[s];
            meters.gainReductionDb[s].store ((float) st.grDb);
            meters.gainReductionHoldDb[s].store ((float) std::min (st.holdDb, st.grDb));
            meters.sidechainDb[s].store ((float) std::max (-100.0, st.levelDb));
            meters.autoGainDb[s].store ((float) (st.agMix * st.agDb));
        }
        if (linked)
            publishIdleMeters (1);
    }

    void setSolo (bool on) noexcept
    {
        if (on && ! soloOn && soloFade <= 0.0)
        {
            // the listen path starts from silence on the current designs
            for (auto& ch : chans)
                for (auto& f : ch.listen)
                {
                    f.reset();
                    f.snapToTarget();
                }
        }
        soloOn = on;
    }
};

//==============================================================================
ClassicComp::ClassicComp() : impl (std::make_unique<Impl>()) {}
ClassicComp::~ClassicComp() = default;

void ClassicComp::prepare (double sampleRate, int)
{
    impl->prepare (sampleRate);
}

void ClassicComp::reset() noexcept
{
    if (impl->prepared)
        impl->reset();
}

void ClassicComp::setParams (ParamValues values) noexcept
{
    if (values != nullptr)
        impl->setParams (values);
}

void ClassicComp::setMonitor (const ModuleMonitor& m) noexcept
{
    impl->setSolo (m.soloDetectionFilter);
}

void ClassicComp::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    if (! impl->prepared || channels == nullptr || numSamples <= 0)
        return;
#if PS_CCMP_HAS_SSE
    const unsigned int savedCsr = _mm_getcsr();
    _mm_setcsr (savedCsr | 0x8040u);   // FTZ | DAZ
#endif
    impl->process (channels, numSamples, ctx.channelMode);
#if PS_CCMP_HAS_SSE
    _mm_setcsr (savedCsr);
#endif
}

int ClassicComp::getLatencySamples() const noexcept { return 0; }
const ClassicCompMeters& ClassicComp::getMeters() const noexcept { return impl->meters; }

//==============================================================================
double ClassicComp::detectorResponseDb (const ClassicCompSetParams& p, double hz) noexcept
{
    // the analog prototypes of the bilinear designs (s normalised to each corner)
    using cplx = std::complex<double>;
    const double f = std::max (1.0e-3, hz);

    const cplx sH (0.0, f / std::max (1.0, (double) p.scHighPassHz));
    const cplx hpf = sH / (sH + 1.0);

    const double aB = std::pow (10.0, kBellGainScale * (double) p.scBellDb / 40.0);
    const cplx sB (0.0, f / std::max (1.0, (double) p.scBellHz));
    const cplx bell = (sB * sB + sB * (aB / kBellQ) + 1.0) / (sB * sB + sB / (aB * kBellQ) + 1.0);

    const double aS = std::pow (10.0, kShelfGainScale * (double) p.scShelfDb / 40.0);
    const double rS = std::sqrt (aS) / kShelfQ;
    const cplx sS (0.0, f / std::max (1.0, (double) p.scShelfHz));
    const cplx shelf = aS * (aS * sS * sS + sS * rS + 1.0) / (sS * sS + sS * rS + aS);

    const double mag = std::abs (hpf * bell * shelf);
    return mag > 1.0e-15 ? 20.0 * std::log10 (mag) : -300.0;
}

double ClassicComp::staticCurveDb (const ClassicCompSetParams& p, double inputDb) noexcept
{
    // Fixed point of the feedback loop for a steady tone (sine-peak dB, a tone above the high-pass in the flat part of
    // the sidechain EQ): the detector reads the output's peak plus off (sineReadingDb, <= 0), r = c knee (L - T).
    // With x' = x + off and y' = y + off: y' = x' - c knee (y' - T). Quadratic knee: r = c u^2 / (2 W),
    // u = y' - T + W / 2, so u = W (sqrt (1 + 2 c s / W) - 1) / c with s = x' - T + W / 2 inside the knee
    // (0 <= s <= W (1 + c / 2)), and y' = T + (x' - T) / (1 + c) above it. Softplus (Smooth): solved by bisection.
    const double c = std::clamp ((double) p.ratio - 1.0, 0.0, 19.0);
    if (c <= 0.0)
        return inputDb;
    const int m = modeIndex (p.mode);
    const double att = std::clamp ((double) p.attackMs, 0.1, 100.0);
    const double v = releaseVar (std::clamp ((double) p.releaseMs, 30.0, 150.0));
    const double off = sineReadingDb (chargeSeconds (att), law (kDischargeLaw, v));
    const double t = (double) p.thresholdDb;
    const double x = inputDb + off;
    double y;
    if (m == 2)
    {
        const double ts = t + kSmoothThresholdDb, w = kKneeDb[2];
        double lo = x - kMaxReductionDb, hi = x;
        for (int i = 0; i < 80; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            double slope = 0.0;
            (mid + c * softKnee (mid - ts, w, slope) > x ? hi : lo) = mid;
        }
        y = 0.5 * (lo + hi);
    }
    else
    {
        const double w = kKneeDb[m];
        const double s = x - t + 0.5 * w;
        if (s <= 0.0)
            y = x;
        else if (s >= w * (1.0 + 0.5 * c))
            y = t + (x - t) / (1.0 + c);
        else
            y = t - 0.5 * w + w * (std::sqrt (1.0 + 2.0 * c * s / w) - 1.0) / c;
    }
    return std::max (y - off, inputDb - kMaxReductionDb);
}
} // namespace ps
