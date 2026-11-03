#include "dsp/ClassicComp.h"

#include "dsp/Filters.h"

#include <algorithm>
#include <cmath>
#include <complex>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_CCMP_HAS_SSE 1
#endif

// Classic Compressor (mod-ccmp). Contract and signal flow: ClassicComp.h. Choices made here (the header leaves them
// open), all from first principles of a feedback compressor until the reference's renders are fitted (the fit should
// only change the constants below):
//
// - Detector calibration: the RMS part is read as sqrt (2 x mean square), so a steady sine reads its PEAK level in both
//   parts (RMS and peak agree on a sine; noise reads ~3 dB above its RMS in the RMS part). Threshold and the static
//   curve (staticCurveDb) are therefore in sine-peak dBFS. The blend is linear in amplitude, then converted to dB.
//   Stereo links by taking the louder of the two channels' blended levels.
// - Loop delay: the detector reads the gain-stage output of the previous sample (one sample in the loop, latency 0).
//   The gain stage output here is v = x g, before the mode saturator (so the static curve stays analytic).
// - Gain-referred detector: the RMS and peak followers average the filtered output divided by the gain it was made
//   with, and are read multiplied by the latest gain. For a steady gain this is exactly the output level; while the
//   gain moves, the followers (which only bridge the waveform's cycles) do not hold a stale, pre-reduction level. An
//   output-referred follower held the pre-reduction peak for its whole release, so the loop drove the reduction past
//   its fixed point (measured: a 24 dB step at ratio 4 / attack 1 ms overshot by 26 dB) and slowed the attack at high
//   ratios (ratio 20: 82 ms for a 15 ms knob). This resolves the loop's instantaneous self-dependence, as the
//   reference's documentation describes its feedback design, without adding latency.
// - Closed-loop attack: in a feedback loop the reduction converges (1 + slope) times faster than the open-loop
//   one-pole (slope = the gain computer's local slope: c above the knee). The attack coefficient is divided by
//   (1 + slope)^kAttackLoopExp, so at 1.0 the Attack knob is the closed-loop time constant at every ratio, and the
//   discrete loop is never driven past dead-beat (alpha (1 + slope) <= 1, stable for any attack / ratio). The release
//   is left open-loop (the feedback naturally speeds it up while the signal stays above the threshold, a classic
//   feedback trait; its coefficient is far too small to destabilise the loop).
// - Program-dependent release: time = release x kReleaseScale[mode] x (1 + h (kSlowReleaseMul - 1)), coefficient
//   1 / (time x rate) (the exact one-pole differs by < 0.04 % at the shortest release). The held share h rises toward
//   1 (kHoldTauMs) while the gain computer's TARGET is deeper than kHoldMinDb, i.e. while the signal keeps asking for
//   compression, and falls toward 0 (kHoldDecayMs) otherwise. Driving it by the current reduction instead (the
//   header's first wording) made the release tail of a short hit count as "held" (measured: a 30 ms hit with release
//   200 recovered in 540 ms, a 3 s stretch in 810 ms; now 280 / 640 ms).
// - Mode saturator: e = max (0, |v| - a), a = kSatLinearDb (-30 dBFS); y = v + k (-sign (v) kSatOdd E3 (e) +
//   kSatEven E2 (e)) - dc, E3 = e^3 and E2 = e^2 up to e = 1, continued linearly above (slope-continuous, so the
//   harmonic share stays bounded for hot signals); k = drive (mode) x min (1, |r| / kSatFullDb). dc is a one-pole
//   (kSatDcHz) of the drive-scaled even term, removing the offset it would add (a coupling capacitor's job; its
//   time constant is ~32 ms after the signal falls under -30 dBFS). At full drive a 0 dBFS sine gets H3 -40 dBc and H2
//   -44 dBc.
// - Auto gain: Bass Shaper's scheme (3 s mean squares of the input and of the saturator output, target clamped +-12
//   dB, frozen below -60 dBFS input, a 1 s one-pole glide in dB evaluated per 64-sample chunk and ramped linearly);
//   switching it off returns to 0 dB with kAutoGainReturnMs.
// - Parameter glides: threshold, ratio (c), knee width, drive and makeup are one-pole glides per sample (95 % after
//   kParamGlideMs); the mode-scaled attack / release times glide in the log domain per sample; the sidechain EQ
//   through SmoothedBiquad over kParamGlideMs. Values snap to their exact target within 1e-9, so neutral settings
//   are exact (bit-exact output).
// - Detector solo: a second set of sidechain filters runs on the input only while the solo is heard (reset and
//   snapped when it starts), crossfaded with the normal output by a 20 ms linear ramp.
// - Meters (published once per block): reduction r at the block end, the deepest r of the last ~1 s (held 1 s, then
//   it follows the block minimum), the detector level L, the auto-gain correction.
namespace ps
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kLn10Over20 = 0.11512925464970229;   // ln (10) / 20
    constexpr double k20OverLn10 = 8.6858896380650366;    // 20 / ln (10)
    constexpr double kTenOverLn10 = 4.3429448190325175;   // 10 / ln (10)
    constexpr int kChunk = 64;                            // internal chunk (per-chunk coefficients, auto gain)
    constexpr int kBothChannels[2] { 0, 1 };
    constexpr int kChannel0[1] { 0 };
    constexpr int kChannel1[1] { 1 };

    //==========================================================================
    // Tunable coefficients (first principles; the fit to the reference changes only these)

    // detector
    constexpr double kDetRmsMix = 0.5;                    // RMS share of the level (the rest is the peak follower)
    constexpr double kDetRmsMs = 5.0;                     // mean-square one-pole
    constexpr double kDetPeakReleaseMs = 40.0;            // peak follower release (instant attack)
    constexpr double kDetFloorDb = -120.0;                // level floor (silence)
    constexpr double kScHpfQ = 0.70710678118654752;       // 12 dB/oct Butterworth
    constexpr double kScBellQ = 0.7;
    constexpr double kScShelfQ = 0.70710678118654752;

    // gain computer
    constexpr double kKneeDb[3] { 2.0, 6.0, 12.0 };       // Sharp, Balanced, Smooth (full width, centred on T)
    constexpr double kMaxReductionDb = 60.0;

    // ballistics
    constexpr double kAttackScale[3] { 1.5, 1.0, 0.6 };   // Sharp lets transients through, Smooth catches them
    constexpr double kReleaseScale[3] { 1.0, 1.0, 1.0 };
    constexpr double kAttackLoopExp = 1.0;                // closed-loop attack compensation (see the top)
    constexpr double kSlowReleaseMul = 4.0;               // sustained reduction releases up to 4x slower
    constexpr double kHoldTauMs = 300.0;                  // the "held" share's rise while compression is held
    constexpr double kHoldDecayMs = 1000.0;               // ... and its fall once the signal no longer asks for it
    constexpr double kHoldMinDb = 1.0;                    // a target reduction deeper than this counts as held

    // mode saturator
    constexpr double kBalancedDrive = 0.3;
    constexpr double kSmoothDrive = 1.0;
    constexpr double kSatDrive[3] { 0.0, kBalancedDrive, kSmoothDrive };
    constexpr double kSatLinearDb = -30.0;                // exactly linear below this level
    constexpr double kSatFullDb = 6.0;                    // reduction depth at which the drive is complete
    constexpr double kSatOdd = 0.04;                      // e^3 term (H3 -40 dBc at 0 dBFS, full drive)
    constexpr double kSatEven = 0.012;                    // e^2 term (H2 -44 dBc at 0 dBFS, full drive)
    constexpr double kSatDcHz = 5.0;                      // DC tracker of the even term

    // auto gain
    constexpr double kAutoGainWindowMs = 3000.0;
    constexpr double kAutoGainGlideMs = 1000.0;
    constexpr double kAutoGainReturnMs = 250.0;
    constexpr double kAutoGainMaxDb = 12.0;
    constexpr double kAutoGainFreezeDb = -60.0;

    // glides, monitor, meters
    constexpr double kParamGlideMs = 20.0;                // 95 % of a step after this
    constexpr double kSoloFadeMs = 20.0;
    constexpr float kMaxInput = 1.0e6f;                  // reject corrupt audio before recursive histories
    constexpr double kAutoGainFreezePower = 1.0e-6;       // 10^(kAutoGainFreezeDb / 10)
    static_assert (kAutoGainFreezeDb == -60.0, "freeze power follows kAutoGainFreezeDb");
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

    // E3 / E2 of the saturator: e^3 / e^2 up to 1, then their tangent lines.
    inline double satCube (double e) noexcept { return e < 1.0 ? e * e * e : 1.0 + 3.0 * (e - 1.0); }
    inline double satSquare (double e) noexcept { return e < 1.0 ? e * e : 1.0 + 2.0 * (e - 1.0); }

    // Gain computer (dB, on the detector level L): reduction (<= 0) and its local slope magnitude.
    inline double gainComputer (double levelDb, double thrDb, double c, double kneeDb, double& slope) noexcept
    {
        slope = 0.0;
        if (c <= 0.0)
            return 0.0;
        const double u = levelDb - thrDb + 0.5 * kneeDb;
        if (u <= 0.0)
            return 0.0;
        if (u >= kneeDb)
        {
            slope = c;
            return -c * (levelDb - thrDb);
        }
        slope = c * u / kneeDb;
        return -c * u * u / (2.0 * kneeDb);
    }

    // The sidechain designs of one set, for the sample rate.
    struct ScDesign
    {
        BiquadCoeffs c[3];
    };

    ScDesign designSidechain (const ClassicCompSetParams& p, double sr) noexcept
    {
        ScDesign d;
        d.c[0] = design::highPass (sr, (double) p.scHighPassHz, kScHpfQ);
        d.c[1] = design::peak (sr, (double) p.scBellHz, kScBellQ, (double) p.scBellDb);
        d.c[2] = design::highShelf (sr, (double) p.scShelfHz, kScShelfQ, (double) p.scShelfDb);
        return d;
    }

    bool sameSidechain (const ClassicCompSetParams& a, const ClassicCompSetParams& b) noexcept
    {
        return a.scHighPassHz == b.scHighPassHz && a.scBellHz == b.scBellHz && a.scBellDb == b.scBellDb
            && a.scShelfHz == b.scShelfHz && a.scShelfDb == b.scShelfDb;
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
    double cGlide = 0.0, cRms = 0.0, cPeak = 0.0, cHoldRise = 0.0, cHoldFall = 0.0, cDc = 0.0, cAgPow = 0.0,
           cAgGlide = 0.0, cAgReturn = 0.0, soloStep = 0.0;
    int holdSamples = 1;

    struct SetDsp
    {
        // controls: current (gliding) and target
        double thr = -12.0, thrT = -12.0;
        double c = 1.0, cT = 1.0;
        double knee = 6.0, kneeT = 6.0;
        double drive = 0.0, driveT = 0.0;
        double makeup = 1.0, makeupT = 1.0;
        double logAtt = 0.0, logAttT = 0.0, logRel = 0.0, logRelT = 0.0;   // ln (ms) of the mode-scaled times
        double g = 1.0;                                                    // gain of the previous sample
        // ballistics
        double r = 0.0, h = 0.0, levelDb = kDetFloorDb;
        // auto gain
        double agPin = 0.0, agPout = 0.0, agDb = 0.0, agTargetDb = 0.0;
        double agGain = 1.0, agStep = 0.0;
        int agLeft = 0;
        // meters
        double holdDb = 0.0;
        int holdLeft = 0;
    };
    SetDsp sets[kNumChannelSets];

    struct ChannelDsp
    {
        SmoothedBiquad det[3], listen[3];
        double ms = 0.0, peak = 0.0, dc = 0.0;
        int set = 0;             // the set whose sidechain designs the filters follow
        bool targetsSet = false;
    };
    ChannelDsp chans[2];

    // detector solo
    bool soloOn = false;
    double soloFade = 0.0;
    float dry[2][kChunk] {};

    //==========================================================================
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        cGlide = poleFor (kParamGlideMs / 3.0, sr);            // 95 % after kParamGlideMs
        cRms = 1.0 - poleFor (kDetRmsMs, sr);
        cPeak = poleFor (kDetPeakReleaseMs, sr);
        cHoldRise = 1.0 - poleFor (kHoldTauMs, sr);
        cHoldFall = 1.0 - poleFor (kHoldDecayMs, sr);
        cDc = 1.0 - std::exp (-2.0 * kPi * kSatDcHz / sr);
        cAgPow = 1.0 - poleFor (kAutoGainWindowMs, sr);
        cAgGlide = poleFor (kAutoGainGlideMs, sr);
        cAgReturn = poleFor (kAutoGainReturnMs, sr);
        soloStep = 1.0 / (kSoloFadeMs * 0.001 * sr);
        holdSamples = std::max (1, (int) std::lround (kGrHoldMs * 0.001 * sr));
        const int ramp = std::max (1, (int) std::lround (kParamGlideMs * 0.001 * sr));
        for (auto& ch : chans)
            for (int k = 0; k < 3; ++k)
            {
                ch.det[k].setRampLength (ramp);
                ch.listen[k].setRampLength (ramp);
            }
        designValid[0] = designValid[1] = false;
        prepared = true;
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : sets)
        {
            s.r = s.h = 0.0;
            s.g = 1.0;
            s.levelDb = kDetFloorDb;
            s.agPin = s.agPout = s.agDb = s.agTargetDb = 0.0;
            s.agGain = 1.0;
            s.agStep = 0.0;
            s.agLeft = 0;
            s.holdDb = 0.0;
            s.holdLeft = 0;
        }
        for (auto& ch : chans)
        {
            for (int k = 0; k < 3; ++k)
            {
                ch.det[k].reset();
                ch.listen[k].reset();
            }
            ch.ms = ch.peak = ch.dc = 0.0;
            ch.targetsSet = false;
        }
        soloFade = soloOn ? 1.0 : 0.0;
        snapNext = true;
        publishIdleMeters (0);
        publishIdleMeters (1);
    }

    void publishIdleMeters (int s) noexcept
    {
        meters.gainReductionDb[s].store (0.0f);
        meters.gainReductionHoldDb[s].store (0.0f);
        meters.sidechainDb[s].store ((float) kDetFloorDb);
        meters.autoGainDb[s].store (0.0f);
    }

    //==========================================================================
    void setParams (ParamValues values) noexcept
    {
        readClassicCompParams (values, params);
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            const auto& p = params.sets[s];
            auto& st = sets[s];
            const int m = modeIndex (p.mode);
            st.thrT = std::clamp ((double) p.thresholdDb, -60.0, 0.0);
            st.cT = std::max (0.0, (double) p.ratio - 1.0);
            st.kneeT = kKneeDb[m];
            st.driveT = kSatDrive[m];
            st.makeupT = std::exp ((double) std::clamp (p.makeupDb, -30.0f, 30.0f) * kLn10Over20);
            st.logAttT = std::log (std::max (0.01, (double) p.attackMs * kAttackScale[m]));
            st.logRelT = std::log (std::max (1.0, (double) p.releaseMs * kReleaseScale[m]));
            if (! designValid[s] || ! sameSidechain (p, designedFor.sets[s]))
            {
                designs[s] = designSidechain (p, sr);
                designedFor.sets[s] = p;
                designValid[s] = true;
                for (auto& ch : chans)
                    if (ch.set == s)
                        ch.targetsSet = false;
            }
        }
        if (snapNext)
        {
            for (auto& st : sets)
            {
                st.thr = st.thrT;
                st.c = st.cT;
                st.knee = st.kneeT;
                st.drive = st.driveT;
                st.makeup = st.makeupT;
                st.logAtt = st.logAttT;
                st.logRel = st.logRelT;
            }
        }
    }

    // Points the channel's filters at the designs of `set` (glides, or jumps when snapping).
    void updateChannelTargets (int c, int set, bool snap) noexcept
    {
        auto& ch = chans[c];
        if (ch.targetsSet && ch.set == set && ! snap)
            return;
        ch.set = set;
        for (int k = 0; k < 3; ++k)
        {
            ch.det[k].setTarget (designs[set].c[k]);
            ch.listen[k].setTarget (designs[set].c[k]);
            if (snap)
            {
                ch.det[k].snapToTarget();
                ch.listen[k].snapToTarget();
            }
        }
        ch.targetsSet = true;
    }

    //==========================================================================
    // One chunk (n <= kChunk) of one set over its channels (Stereo: 0 and 1 linked; M/S: one channel each).
    void runSet (int s, float* const* io, const int* chIdx, int numCh, int start, int n) noexcept
    {
        auto& st = sets[s];
        const auto& p = params.sets[s];

        // Cache settled coefficients per chunk. During automation, update per sample so the log-time glide
        // and its ballistics do not depend on host block boundaries (including single-sample blocks).
        const auto attackCoefficient = [&] {
            return 1.0 - std::exp (-1.0 / std::max (1.0e-3, std::exp (st.logAtt) * 0.001 * sr));
        };
        const auto releaseSamples = [&] { return std::max (1.0, std::exp (st.logRel) * 0.001 * sr); };
        double aAtt = attackCoefficient();
        double relSamples = releaseSamples();
        const double invChannels = 1.0 / (double) numCh;
        const double satA = std::exp (kSatLinearDb * kLn10Over20);

        double minR = st.r;
        for (int i = 0; i < n; ++i)
        {
            // Fixed 64-sample auto-gain clock, retained across host blocks.
            if (st.agLeft == 0)
            {
                st.agLeft = kChunk;
                st.agGain = 1.0;
                st.agStep = 0.0;
                const double goal = p.autoGain ? st.agTargetDb : 0.0;
                const double startDb = st.agDb;
                if (st.agDb != goal)
                {
                    st.agDb = goal + std::pow (p.autoGain ? cAgGlide : cAgReturn, (double) kChunk) * (st.agDb - goal);
                    if (std::abs (st.agDb - goal) < 1.0e-3)   // a 1e-4 step (-80 dB)
                        st.agDb = goal;
                }
                if (startDb != 0.0 || st.agDb != 0.0)
                {
                    st.agGain = std::exp (startDb * kLn10Over20);
                    st.agStep = (std::exp (st.agDb * kLn10Over20) - st.agGain) / (double) kChunk;
                }
            }
            if (st.logAtt != st.logAttT)
            {
                glide (st.logAtt, st.logAttT, cGlide);
                aAtt = attackCoefficient();
            }
            if (st.logRel != st.logRelT)
            {
                glide (st.logRel, st.logRelT, cGlide);
                relSamples = releaseSamples();
            }
            glide (st.thr, st.thrT, cGlide);
            glide (st.c, st.cT, cGlide);
            glide (st.knee, st.kneeT, cGlide);
            glide (st.drive, st.driveT, cGlide);
            glide (st.makeup, st.makeupT, cGlide);

            // detector level: the filtered gain-stage output up to the previous sample
            double level = 0.0;
            for (int k = 0; k < numCh; ++k)
            {
                const auto& ch = chans[chIdx[k]];
                level = std::max (level, kDetRmsMix * std::sqrt (2.0 * ch.ms) + (1.0 - kDetRmsMix) * ch.peak);
            }
            level *= st.g;   // the gain-referred followers read at the latest gain (see the top)
            const double levelDb = level > 1.0e-6 ? k20OverLn10 * std::log (level) : kDetFloorDb;
            st.levelDb = levelDb;

            // gain computer and ballistics
            double slope = 0.0;
            const double target = gainComputer (levelDb, st.thr, st.c, st.knee, slope);
            if (target < st.r)
            {
                double a;
                if constexpr (kAttackLoopExp == 1.0)
                    a = aAtt / (1.0 + slope);
                else
                    a = aAtt * std::pow (1.0 + slope, -kAttackLoopExp);
                st.r = std::max (st.r + std::min (a, 1.0 / (1.0 + slope)) * (target - st.r), -kMaxReductionDb);
            }
            else if (target > st.r)
            {
                const double a = 1.0 / (relSamples * (1.0 + st.h * (kSlowReleaseMul - 1.0)));
                st.r += std::min (a, 1.0) * (target - st.r);
                if (target == 0.0 && st.r > -1.0e-7)
                    st.r = 0.0;
            }
            if (target < -kHoldMinDb)
                st.h += cHoldRise * (1.0 - st.h);
            else
                st.h -= cHoldFall * st.h;
            minR = std::min (minR, st.r);

            const double g = st.r == 0.0 ? 1.0 : std::exp (st.r * kLn10Over20);
            st.g = g;
            const double drive = st.drive * std::min (1.0, -st.r / kSatFullDb);
            const double outGain = st.makeup * st.agGain;
            st.agGain += st.agStep;

            double pin = 0.0, pout = 0.0;
            for (int k = 0; k < numCh; ++k)
            {
                auto& ch = chans[chIdx[k]];
                float* const data = io[chIdx[k]] + start;
                const double x = (double) data[i];
                const double v = g == 1.0 ? x : x * g;

                // detector (read at the next sample)
                double d = v;
                for (auto& f : ch.det)
                    d = f.processSample (d);
                const double dRef = g == 1.0 ? d : d / g;   // gain-referred (see the top)
                ch.ms += cRms * (dRef * dRef - ch.ms);
                ch.peak = std::max (std::abs (dRef), ch.peak * cPeak);

                // mode saturator
                double y = v;
                if (drive > 0.0 || ch.dc != 0.0)
                {
                    const double e = std::max (0.0, std::abs (v) - satA);
                    const double even = drive * kSatEven * satSquare (e);
                    ch.dc += cDc * (even - ch.dc);
                    if (e > 0.0)
                        y += drive * kSatOdd * (v < 0.0 ? satCube (e) : -satCube (e));
                    y += even - ch.dc;
                }

                pin += x * x;
                pout += y * y;
                data[i] = (float) (outGain == 1.0 ? y : y * outGain);
            }
            st.agPin += cAgPow * (pin * invChannels - st.agPin);
            st.agPout += cAgPow * (pout * invChannels - st.agPout);
            if (--st.agLeft == 0 && st.agPin > kAutoGainFreezePower && st.agPout > 1.0e-12)
                st.agTargetDb = std::clamp (kTenOverLn10 * std::log (st.agPin / st.agPout), -kAutoGainMaxDb, kAutoGainMaxDb);
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
        flushTiny (st.h);
        flushTiny (st.agPin);
        flushTiny (st.agPout);
        for (int k = 0; k < numCh; ++k)
        {
            auto& ch = chans[chIdx[k]];
            flushTiny (ch.ms);
            flushTiny (ch.peak);
            flushTiny (ch.dc);
        }
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
        updateChannelTargets (0, 0, snap);
        updateChannelTargets (1, linked ? 0 : 1, snap);

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
            meters.gainReductionDb[s].store ((float) st.r);
            meters.gainReductionHoldDb[s].store ((float) std::min (st.holdDb, st.r));
            meters.sidechainDb[s].store ((float) std::max (kDetFloorDb, st.levelDb));
            meters.autoGainDb[s].store ((float) st.agDb);
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
    // the analog prototypes of Filters.cpp (s normalised to each corner)
    using cplx = std::complex<double>;
    const double f = std::max (1.0e-3, hz);

    const cplx sH (0.0, f / std::max (1.0, (double) p.scHighPassHz));
    const cplx hpf = sH * sH / (sH * sH + sH / kScHpfQ + 1.0);

    const double aB = std::pow (10.0, (double) p.scBellDb / 40.0);
    const cplx sB (0.0, f / std::max (1.0, (double) p.scBellHz));
    const cplx bell = (sB * sB + sB * (aB / kScBellQ) + 1.0) / (sB * sB + sB / (aB * kScBellQ) + 1.0);

    const double aS = std::pow (10.0, (double) p.scShelfDb / 40.0);
    const double rS = std::sqrt (aS) / kScShelfQ;
    const cplx sS (0.0, f / std::max (1.0, (double) p.scShelfHz));
    const cplx shelf = aS * (aS * sS * sS + sS * rS + 1.0) / (sS * sS + sS * rS + aS);

    const double mag = std::abs (hpf * bell * shelf);
    return mag > 1.0e-15 ? 20.0 * std::log10 (mag) : -300.0;
}

double ClassicComp::staticCurveDb (const ClassicCompSetParams& p, double inputDb) noexcept
{
    // Fixed point of the feedback loop for a steady tone (levels in sine-peak dB): y = x + r (L = y), with
    // r = -c (L - T) above the knee and -c u^2 / (2 W), u = L - T + W / 2, inside it. Inside the knee
    // u + c u^2 / (2 W) = s, s = x - T + W / 2, so u = W (sqrt (1 + 2 c s / W) - 1) / c; the knee spans
    // 0 <= s <= W (1 + c / 2) on the input side (it widens with the ratio).
    const double c = std::max (0.0, (double) p.ratio - 1.0);
    const double w = kKneeDb[modeIndex (p.mode)];
    const double t = (double) p.thresholdDb;
    const double s = inputDb - t + 0.5 * w;
    if (c <= 0.0 || s <= 0.0)
        return inputDb;
    double y;
    if (s >= w * (1.0 + 0.5 * c))
        y = t + (inputDb - t) / (1.0 + c);
    else
        y = t - 0.5 * w + w * (std::sqrt (1.0 + 2.0 * c * s / w) - 1.0) / c;
    return std::max (y, inputDb - kMaxReductionDb);
}
} // namespace ps
