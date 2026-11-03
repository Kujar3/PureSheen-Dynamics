#include "dsp/Tape.h"

#include "dsp/Filters.h"
#include "dsp/Oversampler.h"

#include <algorithm>
#include <cmath>
#include <complex>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_TAPE_HAS_SSE 1
#endif

// Tape (mod-tape). Contract and signal flow: Tape.h. Choices made here (the header leaves them open), every number in
// the constant block below so the fit to the reference's renders only changes numbers:
//
// - Everything runs per channel at 4x (ps::Oversampler MinimumPhase, 5 samples). The linear sections are analog
//   prototypes (s-domain, below) designed by the bilinear transform prewarped at their corner; at 4x the frequency
//   warping is < 5 % at 20 kHz (44.1 kHz), and the response view (responseDb) evaluates the same prototypes. They run as
//   ps::SmoothedBiquad (modulation-safe), whose ramp covers one control period, so a moving control interpolates
//   between the designs of consecutive periods.
// - Record / reproduce EQ: pre = (1 + S) / (1 + S / r), S = s / w0, w0 = the speed's equal-distortion frequency, r the
//   plateau (kPreEmphasisDb, more at negative bias). De-emphasis is its exact reciprocal (same bilinear map), so the
//   small-signal path is flat to double rounding.
// - Tape curve c (x) on x = G * pre (in), G = drive * kTapeScale (* the over-bias drive at positive bias): a blend of
//   three saturators with unit slope at 0, x / (1 + |x|^p)^(1 / p) for p = 4 (hard, bias -10), 2 (bias 0) and 1 (soft,
//   bias +10), interpolated linearly in bias (a blend of monotonic curves with slope 1 at 0 stays monotonic).
// - Hysteresis (rate independent, applied to the curve's residual r (x) = x - c (x), so small signals stay linear):
//   R follows r with dR = (r - R) * min (1, |dx| / w) (w = loop width, kHystWidth over bias), y = c (x) - (r (x) - R).
//   On a rising slope R lags behind r, the output sits under the curve (it lags the input, as magnetisation does).
//   A moving drive rescales R with the input, and a slow leak (kHystLeakMs) lets a residue settle without motion.
// - Even harmonics: e (x) = (c (d + x) + c (d - x)) / 2 - c (d), d = harmonics / 10 * kEvenOffsetMax: the even part of
//   the offset curve (exactly 0 at harmonics 0), its DC removed by a first-order high-pass (kEvenDcHz) on e alone, in
//   output units (e / G), so a drive change moves its DC without a 1 / G-amplified transient.
// - Head bump: a resonant peak (bell) at kBumpHz on a Butterworth high-pass at kRollOffRatio x kBumpHz (the roll-off,
//   kept at lowEmph 0). The bell's design gain includes the high-pass loss at kBumpHz, so lowEmph 10 peaks at exactly
//   kBumpMaxDb.
// - High end: the gap / spacing loss and the reproduce EQ that compensates it are one net first-order high shelf at
//   the speed's kHfLossHz (so highEmph 4 is exactly flat, no residual phase): kHighEmphMinDb at 0 .. 0 at 4 ..
//   kHighEmphMaxDb at 10, plus the bias's secondary HF tilt (kBiasHfDb, a lift at negative bias).
// - Make-up (static): out = in for a steady -18 dBFS RMS pink noise (20 Hz .. 20 kHz). The curve's input is Gaussian
//   with sigma = G * sigma_pre * 10^(-18 / 20) (sigma_pre = the pre-emphasis's RMS gain on that pink noise). By
//   quadrature, the curve output (c + e, DC removed) splits into its linear part (Bussgang gain a) and the distortion;
//   the de-emphasis returns the linear part to the input's level, and keeps kDistortionShare of the distortion power
//   (fitted on band-limited pink noise: within 0.15 dB from drive -30 to +20, every speed / bias / harmonics). The
//   hysteresis is not in the estimate (its deficit is small at -18 dBFS). The make-up also undoes the pink-noise loss
//   of the fixed linear parts (the speed's roll-off and the bias's HF tilt; the emphasis controls stay pure EQ), so the
//   small-signal mid band sits a few tenths of a dB above 0 dB (+0.3 dB at 15 ips, drive 0).
// - Controls: drive (dB), bias, harmonics, lowEmph and highEmph glide (a critically damped two-stage one-pole, 90 %
//   of a step after kGlideMs, no corner at either end), evaluated once per control period of kControl base samples (at
//   fixed sample positions, so the output does not depend on the host's block size) and interpolated per sample inside
//   it. A speed change crossfades two processing lanes (raised cosine, kSpeedFadeMs): the new lane starts from a copy
//   of the old lane's state with the new speed's filters.
// - Meters: inputDriveDb = peak of |pre (in) * drive| (dBFS); saturationDb = 20 log10 (peak |x| / peak |curve out|) on
//   the curve's internal signal, both peaks with a kMeterReleaseMs release.
// - Latency: the oversampler's 5 samples is the group delay of the whole module from 1.5 to 4 kHz (within 0.2 sample at
//   44.1 / 48 / 96 kHz); lower down the head-bump roll-off adds its own phase (a tape machine shifts phase, the docs say
//   so), and the minimum-phase impulse response peaks one sample later.
namespace ps
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kOsLog2 = 2;                 // 4x
    constexpr int kOs = 1 << kOsLog2;
    constexpr int kControl = 8;                // control period (base samples)
    constexpr double kGlideMs = 20.0;          // drive / bias / harmonics / emphasis glide: 90 % of a step
    constexpr double kGlideStageRatio = 0.257; // stage time constant / kGlideMs (two stages: 90 % after 3.89 tau)
    constexpr double kSpeedFadeMs = 20.0;      // speed crossfade

    // record EQ (pre-emphasis) and its exact inverse
    constexpr double kPreEmphasisHz[3] { 500.0, 1000.0, 2000.0 };   // 7.5 / 15 / 30 ips: equal distortion (docs)
    constexpr double kPreEmphasisDb = 10.0;                        // plateau at bias 0
    constexpr double kBiasPreEmphasisDb = 4.0;                     // + at bias -10, - at bias +10

    // tape curve
    constexpr double kTapeScale = 0.5;           // curve units per dBFS unit at drive 0 (0 dBFS = 0.5: the knee is 1)
    constexpr double kOverBiasDriveDb = 10.0;    // extra drive into the curve at bias +10 (over-biased: compressed)
    constexpr double kHystWidth[3] { 0.12, 0.06, 0.02 };   // loop width (curve units) at bias -10 / 0 / +10
    constexpr double kHystLeakMs = 50.0;         // the loop memory settles without signal motion (rate independent
                                                 //   above a few Hz)
    constexpr double kEvenOffsetMax = 0.35;      // curve offset d at harmonics 10
    constexpr double kEvenDcHz = 5.0;            // DC removal of the even part

    // head bump
    constexpr double kBumpHz[3] { 40.0, 60.0, 110.0 };
    constexpr double kBumpQ = 1.2;
    constexpr double kBumpMaxDb = 10.0;           // at lowEmph 10 (documented maximum)
    constexpr double kBumpLaw[11] { 0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0 };   // lowEmph -> share
    constexpr double kRollOffRatio = 0.35;        // high-pass corner / bump frequency
    constexpr double kRollOffQ = 0.7071067811865476;

    // high end
    constexpr double kHfLossHz[3] { 6000.0, 9000.0, 13000.0 };   // corner of the net loss / emphasis shelf per speed
    constexpr double kHighEmphMinDb = -9.0;       // highEmph 0: the loss uncompensated (gentle roll-off)
    constexpr double kHighEmphMaxDb = 6.0;        // highEmph 10: "shimmer"
    constexpr double kHighEmphFlat = 4.0;         // highEmph value of the flat response (documented)
    constexpr double kBiasHfDb = 1.5;             // HF lift at bias -10 (cut at +10)

    // make-up reference
    constexpr double kRefDbfs = -18.0;            // steady pink noise level the make-up holds
    constexpr double kPinkLoHz = 20.0, kPinkHiHz = 20000.0;
    constexpr int kPinkPoints = 64, kQuadPoints = 64;
    constexpr double kQuadRange = 6.0;            // +-6 sigma
    constexpr double kDistortionShare = 0.4;      // share of the distortion power left after de-emphasis (fit)

    constexpr double kMeterReleaseMs = 300.0;
    constexpr float kMaxInput = 1.0e6f;          // reject corrupt audio above +120 dBFS before the float oversampler

    inline double dbToGain (double db) noexcept { return std::pow (10.0, db * 0.05); }

    //==========================================================================
    // Analog prototypes in S = s / w0: H (S) = (b0 + b1 S + b2 S^2) / (a0 + a1 S + a2 S^2).
    struct Proto
    {
        double hz = 1000.0;
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

        double magnitude (double f) const noexcept
        {
            const std::complex<double> S (0.0, f / hz);
            return std::abs ((b0 + b1 * S + b2 * S * S) / (a0 + a1 * S + a2 * S * S));
        }

        // Bilinear transform prewarped at hz: S = c (1 - z^-1) / (1 + z^-1), c = 1 / tan (pi hz / fs).
        BiquadCoeffs digital (double fs) const noexcept
        {
            const double f = std::min (hz, 0.45 * fs);
            const double c = 1.0 / std::tan (kPi * f / fs), c2 = c * c;
            BiquadCoeffs k;
            if (b2 == 0.0 && a2 == 0.0)
            {
                // first order stays first order (the second-order map would add a pole-zero pair at Nyquist)
                const double e0 = a0 + a1 * c;
                k.b0 = (b0 + b1 * c) / e0;
                k.b1 = (b0 - b1 * c) / e0;
                k.a1 = (a0 - a1 * c) / e0;
                return k;
            }
            const double n0 = b0 + b1 * c + b2 * c2, n1 = 2.0 * (b0 - b2 * c2), n2 = b0 - b1 * c + b2 * c2;
            const double d0 = a0 + a1 * c + a2 * c2, d1 = 2.0 * (a0 - a2 * c2), d2 = a0 - a1 * c + a2 * c2;
            k.b0 = n0 / d0;
            k.b1 = n1 / d0;
            k.b2 = n2 / d0;
            k.a1 = d1 / d0;
            k.a2 = d2 / d0;
            return k;
        }
    };

    Proto preEmphasis (double hz, double plateauDb) noexcept
    {
        Proto p;
        p.hz = hz;
        p.b0 = 1.0; p.b1 = 1.0;
        p.a0 = 1.0; p.a1 = 1.0 / dbToGain (plateauDb);
        return p;
    }

    Proto deEmphasis (double hz, double plateauDb) noexcept
    {
        Proto p = preEmphasis (hz, plateauDb);
        std::swap (p.b0, p.a0);
        std::swap (p.b1, p.a1);
        return p;
    }

    Proto highPass2 (double hz, double q) noexcept
    {
        Proto p;
        p.hz = hz;
        p.b0 = 0.0; p.b1 = 0.0; p.b2 = 1.0;
        p.a0 = 1.0; p.a1 = 1.0 / q; p.a2 = 1.0;
        return p;
    }

    Proto bell (double hz, double q, double gainDb) noexcept
    {
        const double a = std::pow (10.0, gainDb / 40.0);
        Proto p;
        p.hz = hz;
        p.b0 = 1.0; p.b1 = a / q; p.b2 = 1.0;
        p.a0 = 1.0; p.a1 = 1.0 / (a * q); p.a2 = 1.0;
        return p;
    }

    // First-order high shelf: DC gain 1, HF gain g, half the gain (in dB) at hz.
    Proto highShelf (double hz, double gainDb) noexcept
    {
        const double sg = std::pow (10.0, gainDb / 40.0);
        Proto p;
        p.hz = hz;
        p.b0 = 1.0; p.b1 = sg;
        p.a0 = 1.0; p.a1 = 1.0 / sg;
        return p;
    }

    //==========================================================================
    // Control-value laws (shared by the DSP and responseDb).
    inline int speedIndex (TapeSpeed s) noexcept { return std::clamp ((int) s, 0, 2); }
    inline double biasUnit (double bias) noexcept { return std::clamp (bias / 10.0, -1.0, 1.0); }
    inline double preEmphasisDb (double bias) noexcept { return kPreEmphasisDb - biasUnit (bias) * kBiasPreEmphasisDb; }

    double bumpShare (double lowEmph) noexcept
    {
        const double x = std::clamp (lowEmph, 0.0, 10.0);
        const int i = std::min (9, (int) x);
        return kBumpLaw[i] + (x - i) * (kBumpLaw[i + 1] - kBumpLaw[i]);
    }

    double highShelfDb (double highEmph, double bias) noexcept
    {
        const double e = std::clamp (highEmph, 0.0, 10.0);
        const double emph = e <= kHighEmphFlat ? kHighEmphMinDb * (kHighEmphFlat - e) / kHighEmphFlat
                                               : kHighEmphMaxDb * (e - kHighEmphFlat) / (10.0 - kHighEmphFlat);
        return emph - biasUnit (bias) * kBiasHfDb;
    }

    Proto rollOffFor (int speed) noexcept { return highPass2 (kBumpHz[speed] * kRollOffRatio, kRollOffQ); }
    Proto biasTiltFor (int speed, double bias) noexcept
    {
        return highShelf (kHfLossHz[speed], -biasUnit (bias) * kBiasHfDb);
    }

    Proto bumpFor (int speed, double lowEmph) noexcept
    {
        const double share = bumpShare (lowEmph);
        const double hpLossDb = -20.0 * std::log10 (rollOffFor (speed).magnitude (kBumpHz[speed]));
        return bell (kBumpHz[speed], kBumpQ, share * (kBumpMaxDb + hpLossDb));
    }

    Proto shelfFor (int speed, double highEmph, double bias) noexcept
    {
        return highShelf (kHfLossHz[speed], highShelfDb (highEmph, bias));
    }

    //==========================================================================
    // The tape curve: c (x) = (1 - |b|) c2 (x) + |b| (b < 0 ? c4 : c1) (x), b = bias / 10.
    inline double c1 (double x) noexcept { return x / (1.0 + std::abs (x)); }
    inline double c2 (double x) noexcept { return x / std::sqrt (1.0 + x * x); }
    inline double c4 (double x) noexcept { const double x2 = x * x; return x / std::sqrt (std::sqrt (1.0 + x2 * x2)); }

    struct Curve
    {
        double wHard = 0.0, wSoft = 0.0;   // weights of c4 and c1 (c2 gets the rest)

        void set (double b) noexcept
        {
            wHard = b < 0.0 ? -b : 0.0;
            wSoft = b > 0.0 ? b : 0.0;
        }

        inline double operator() (double x) const noexcept
        {
            double y = (1.0 - wHard - wSoft) * c2 (x);
            if (wHard > 0.0)
                y += wHard * c4 (x);
            if (wSoft > 0.0)
                y += wSoft * c1 (x);
            return y;
        }
    };

    inline double hystWidthFor (double b) noexcept
    {
        return b < 0.0 ? kHystWidth[1] + (-b) * (kHystWidth[0] - kHystWidth[1])
                       : kHystWidth[1] + b * (kHystWidth[2] - kHystWidth[1]);
    }

    inline double curveGainFor (double driveDb, double bias) noexcept
    {
        return dbToGain (driveDb + std::max (0.0, biasUnit (bias)) * kOverBiasDriveDb) * kTapeScale;
    }

    // RMS gain of the pre-emphasis on pink noise (equal power per octave, kPinkLoHz .. kPinkHiHz).
    double pinkPreGain (int speed, double bias) noexcept
    {
        const auto pre = preEmphasis (kPreEmphasisHz[speed], preEmphasisDb (bias));
        const double span = std::log (kPinkHiHz / kPinkLoHz);
        double sum = 0.0;
        for (int i = 0; i < kPinkPoints; ++i)
        {
            const double f = kPinkLoHz * std::exp (span * (i + 0.5) / kPinkPoints);
            const double m = pre.magnitude (f);
            sum += m * m;
        }
        return std::sqrt (sum / kPinkPoints);
    }

    // RMS gain of the fixed linear parts (roll-off, bias tilt) on the same pink noise.
    double pinkLinearGain (int speed, double bias) noexcept
    {
        const auto roll = rollOffFor (speed);
        const auto tilt = biasTiltFor (speed, bias);
        const double span = std::log (kPinkHiHz / kPinkLoHz);
        double sum = 0.0;
        for (int i = 0; i < kPinkPoints; ++i)
        {
            const double f = kPinkLoHz * std::exp (span * (i + 0.5) / kPinkPoints);
            const double m = roll.magnitude (f) * tilt.magnitude (f);
            sum += m * m;
        }
        return std::sqrt (sum / kPinkPoints);
    }

    // Make-up gain of the curve for a Gaussian input of RMS sigma (curve units). Bussgang split of the output y = c + e
    // (DC removed): the linear part a x (a = E [x y] / sigma^2) and the distortion (the rest, power P). The output keeps
    // the linear part's power and kDistortionShare of P (the distortion is broader than the pre-emphasised spectrum,
    // so the de-emphasis and the downsampler remove more of it): make-up = sigma / sqrt (a^2 sigma^2 + share P).
    double makeupFor (double sigma, const Curve& curve, double d) noexcept
    {
        const double cd = curve (d);
        double sw = 0.0, sy2 = 0.0, se = 0.0, sxc = 0.0;
        for (int i = 0; i < kQuadPoints; ++i)   // u > 0 only: c is odd, e even
        {
            const double u = kQuadRange * (i + 0.5) / kQuadPoints;
            const double w = std::exp (-0.5 * u * u);
            const double x = sigma * u;
            const double c = curve (x);
            const double e = d != 0.0 ? 0.5 * (curve (d + x) + curve (d - x)) - cd : 0.0;
            sw += w;
            sy2 += w * (c * c + e * e);   // E [c e] = 0 (odd times even)
            se += w * e;
            sxc += w * x * c;             // E [x e] = 0
        }
        const double meanE = se / sw;
        const double var = sy2 / sw - meanE * meanE;
        const double lin = sxc / sw;      // a sigma^2
        const double linPow = lin * lin / (sigma * sigma);
        const double out = linPow + kDistortionShare * std::max (0.0, var - linPow);
        return out > 1.0e-30 ? sigma / std::sqrt (out) : 1.0;
    }

    //==========================================================================
    // Critically damped two-stage one-pole glide, stepped once per control period (C1: no corner where it starts or
    // stops, unlike a linear ramp). Snaps exactly onto the target once both stages are within 1e-9 of it.
    struct Glide
    {
        double value = 0.0, stage = 0.0, target = 0.0, c = 0.0;

        void snap (double v) noexcept { value = stage = target = v; }
        void setTarget (double t) noexcept { target = t; }

        void advance() noexcept
        {
            if (value == target && stage == target)
                return;
            stage = target + c * (stage - target);
            value = stage + c * (value - stage);
            if (std::abs (stage - target) < 1.0e-9 && std::abs (value - target) < 1.0e-9)
                value = stage = target;
        }
    };

    inline double finiteOr (double v, double fallback) noexcept { return std::isfinite (v) ? v : fallback; }
} // namespace

//==============================================================================
struct Tape::Impl
{
    // Values of one control period's end (the start is the previous period's end), interpolated per sample.
    struct Shape
    {
        double g = kTapeScale;     // input gain into the curve
        double invG = 1.0 / kTapeScale;
        double d = 0.0, cd = 0.0;  // even offset and c (d)
        double bias = 0.0;         // bias / 10 (curve blend)
        double width = kHystWidth[1];
        double driveGain = 1.0;    // drive alone (meter)
    };

    struct Channel
    {
        SmoothedBiquad pre, de, roll, bump, shelf;
        double evenX1 = 0.0, evenY1 = 0.0;   // first-order DC removal of the even part
        double hystR = 0.0, xPrev = 0.0, gPrev = kTapeScale;
    };

    struct Lane
    {
        int speed = 1;
        Channel ch[2];
        double makeupStart = 1.0, makeupEnd = 1.0;
        // designed control values (filters are re-targeted only when these change)
        double dBias = 1.0e9, dLow = 1.0e9, dHigh = 1.0e9;
        double mDrive = 1.0e9, mBias = 1.0e9, mHarm = 1.0e9;
    };

    TapeMeters meters;
    Oversampler os;
    double sr = 48000.0, osr = 192000.0;
    bool prepared = false, haveParams = false;

    TapeParams target;
    Glide drive, bias, harmonics, lowEmph, highEmph;

    Lane lanes[2];
    int active = 0;
    bool fading = false;
    int fadePos = 0, fadeLen = 3840;

    Shape shapeStart, shapeEnd;
    int controlPos = 0;   // base samples into the current control period (0 = a new period starts)

    double evenDcA = 0.0;   // even-part DC removal: y = a (y1 + x - x1)
    double hystLeak = 0.0;  // per high-rate sample

    // meters (internal units)
    double peakIn = 0.0, peakLin = 0.0, peakOut = 0.0, meterDecay = 0.0;

    //==========================================================================
    void setLaneFilters (Lane& lane, bool snap) noexcept
    {
        const double b = bias.value, lo = lowEmph.value, hi = highEmph.value;
        const int s = lane.speed;
        if (snap || b != lane.dBias)
        {
            const double pd = preEmphasisDb (b);
            const auto pre = preEmphasis (kPreEmphasisHz[s], pd).digital (osr);
            const auto de = deEmphasis (kPreEmphasisHz[s], pd).digital (osr);
            for (auto& c : lane.ch)
            {
                c.pre.setTarget (pre);
                c.de.setTarget (de);
            }
        }
        if (snap || lo != lane.dLow)
        {
            const auto bump = bumpFor (s, lo).digital (osr);
            for (auto& c : lane.ch)
                c.bump.setTarget (bump);
        }
        if (snap || hi != lane.dHigh || b != lane.dBias)
        {
            const auto shelf = shelfFor (s, hi, b).digital (osr);
            for (auto& c : lane.ch)
                c.shelf.setTarget (shelf);
        }
        if (snap)
        {
            const auto roll = rollOffFor (s).digital (osr);
            for (auto& c : lane.ch)
            {
                c.roll.setTarget (roll);
                c.pre.snapToTarget();
                c.de.snapToTarget();
                c.roll.snapToTarget();
                c.bump.snapToTarget();
                c.shelf.snapToTarget();
            }
        }
        lane.dBias = b;
        lane.dLow = lo;
        lane.dHigh = hi;
    }

    void setLaneMakeup (Lane& lane, bool snap) noexcept
    {
        lane.makeupStart = lane.makeupEnd;
        if (snap || drive.value != lane.mDrive || bias.value != lane.mBias || harmonics.value != lane.mHarm)
        {
            Curve curve;
            curve.set (biasUnit (bias.value));
            const double sigma = curveGainFor (drive.value, bias.value) * pinkPreGain (lane.speed, bias.value)
                                 * dbToGain (kRefDbfs);
            const double d = std::clamp (harmonics.value, 0.0, 10.0) / 10.0 * kEvenOffsetMax;
            lane.makeupEnd = finiteOr (makeupFor (sigma, curve, d) / pinkLinearGain (lane.speed, bias.value), 1.0);
            lane.mDrive = drive.value;
            lane.mBias = bias.value;
            lane.mHarm = harmonics.value;
        }
        if (snap)
            lane.makeupStart = lane.makeupEnd;
    }

    Shape shapeNow() const noexcept
    {
        Shape s;
        s.g = curveGainFor (drive.value, bias.value);
        s.invG = 1.0 / s.g;
        s.bias = biasUnit (bias.value);
        s.d = std::clamp (harmonics.value, 0.0, 10.0) / 10.0 * kEvenOffsetMax;
        Curve c;
        c.set (s.bias);
        s.cd = c (s.d);
        s.width = hystWidthFor (s.bias);
        s.driveGain = dbToGain (drive.value);
        return s;
    }

    void snapAll() noexcept
    {
        drive.snap (std::clamp ((double) target.driveDb, -30.0, 20.0));
        bias.snap (std::clamp ((double) target.bias, -10.0, 10.0));
        harmonics.snap (std::clamp ((double) target.harmonics, 0.0, 10.0));
        lowEmph.snap (std::clamp ((double) target.lowEmphasis, 0.0, 10.0));
        highEmph.snap (std::clamp ((double) target.highEmphasis, 0.0, 10.0));
        fading = false;
        active = 0;
        lanes[0].speed = speedIndex (target.speed);
        setLaneFilters (lanes[0], true);
        setLaneMakeup (lanes[0], true);
        shapeStart = shapeEnd = shapeNow();
        controlPos = 0;
    }

    void resetState() noexcept
    {
        os.reset();
        for (auto& lane : lanes)
            for (auto& c : lane.ch)
            {
                c.pre.reset();
                c.de.reset();
                c.roll.reset();
                c.bump.reset();
                c.shelf.reset();
                c.evenX1 = c.evenY1 = c.hystR = c.xPrev = 0.0;
                c.gPrev = kTapeScale;
            }
        peakIn = peakLin = peakOut = 0.0;
        meters.inputDriveDb.store (-100.0f);
        meters.saturationDb.store (0.0f);
        if (haveParams)
            snapAll();
    }

    // Start of a control period: advance the glides by one period, re-design, maybe start a speed crossfade.
    void beginPeriod() noexcept
    {
        for (auto* g : { &drive, &bias, &harmonics, &lowEmph, &highEmph })
            g->advance();

        if (! fading && speedIndex (target.speed) != lanes[active].speed)
        {
            const int next = 1 - active;
            lanes[next] = lanes[active];
            lanes[next].speed = speedIndex (target.speed);
            setLaneFilters (lanes[next], true);
            setLaneMakeup (lanes[next], true);
            lanes[next].makeupStart = lanes[next].makeupEnd;
            fading = true;
            fadePos = 0;
        }

        setLaneFilters (lanes[active], false);
        setLaneMakeup (lanes[active], false);
        if (fading)
        {
            setLaneFilters (lanes[1 - active], false);
            setLaneMakeup (lanes[1 - active], false);
        }

        shapeStart = shapeEnd;
        shapeEnd = shapeNow();
    }

    // One channel of one lane at the high rate. lin / out: |x| and |curve output| (curve units, for the meters).
    static inline double tick (Channel& c, double in, double g, double invG, const Curve& curve, double d, double cd,
                               double width, double leak, double evenA, double& lin, double& out, double& pre) noexcept
    {
        pre = c.pre.processSample (in);
        const double x = pre * g;

        // a moving drive rescales the loop's memory with the input (no jump of the output-referred deficit, which
        // 1 / G would otherwise amplify after a drive cut); the leak lets a residue settle without signal motion
        if (g != c.gPrev)
        {
            const double k = g / c.gPrev;
            c.hystR *= k;
            c.xPrev *= k;
            c.gPrev = g;
        }
        const double cx = curve (x);
        const double r = x - cx;
        const double step = std::min (1.0, std::abs (x - c.xPrev) / width + leak);
        c.hystR += (r - c.hystR) * step;
        c.xPrev = x;
        const double y = cx - (r - c.hystR);

        lin = std::abs (x);
        out = std::abs (y);
        double v = y * invG;

        // even part, DC removed in output units (a drive change moves its DC without a 1 / G-amplified transient)
        if (d != 0.0 || c.evenY1 != 0.0 || c.evenX1 != 0.0)
        {
            const double e = d != 0.0 ? (0.5 * (curve (d + x) + curve (d - x)) - cd) * invG : 0.0;
            double eh = evenA * (c.evenY1 + e - c.evenX1);
            if (e == 0.0 && std::abs (eh) < 1.0e-30)
                eh = 0.0;
            c.evenX1 = e;
            c.evenY1 = eh;
            v += eh;
            out = std::abs (y + eh * g);
        }

        v = c.de.processSample (v);
        v = c.roll.processSample (v);
        v = c.bump.processSample (v);
        return c.shelf.processSample (v);
    }

    void processSegment (float* const* ch, int n) noexcept
    {
        // A single non-finite sample would otherwise poison the recursive upsampler indefinitely. Very large finite
        // samples can also overflow its float buffers. Replace only corrupt samples; keep both channels' histories.
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                if (! std::isfinite (ch[c][i]) || std::abs (ch[c][i]) > kMaxInput)
                    ch[c][i] = 0.0f;
        float* const* up = os.upsample (ch, n);
        const int n4 = n * kOs;
        const double period4 = (double) (kControl * kOs);
        const int base4 = controlPos * kOs;
        const double fadeStep = 1.0 / (double) fadeLen;

        double blockIn = 0.0, blockLin = 0.0, blockOut = 0.0;
        Curve curve;

        for (int i = 0; i < n4; ++i)
        {
            const double t = (double) (base4 + i + 1) / period4;
            auto lerp = [t] (double a, double b) { return a + (b - a) * t; };
            const double g = lerp (shapeStart.g, shapeEnd.g);
            const double invG = 1.0 / g;
            const double d = lerp (shapeStart.d, shapeEnd.d);
            const double b = lerp (shapeStart.bias, shapeEnd.bias);
            curve.set (b);
            const double cd = d != 0.0 ? curve (d) : 0.0;
            const double width = lerp (shapeStart.width, shapeEnd.width);
            const double driveGain = lerp (shapeStart.driveGain, shapeEnd.driveGain);

            double wNew = 0.0;
            if (fading)
            {
                const double p = std::min (1.0, (fadePos + 1) * fadeStep);
                wNew = 0.5 - 0.5 * std::cos (kPi * p);
            }

            for (int c = 0; c < 2; ++c)
            {
                const double x = up[c][i];
                auto& la = lanes[active];
                double lin = 0.0, out = 0.0, pre = 0.0;
                double y = tick (la.ch[c], x, g, invG, curve, d, cd, width, hystLeak, evenDcA, lin, out, pre)
                           * lerp (la.makeupStart, la.makeupEnd);
                if (fading)
                {
                    auto& lb = lanes[1 - active];
                    double lin2 = 0.0, out2 = 0.0, pre2 = 0.0;
                    const double y2 = tick (lb.ch[c], x, g, invG, curve, d, cd, width, hystLeak, evenDcA, lin2, out2, pre2)
                                      * lerp (lb.makeupStart, lb.makeupEnd);
                    y += wNew * (y2 - y);
                    if (wNew > 0.5)
                    {
                        lin = lin2;
                        out = out2;
                        pre = pre2;
                    }
                }
                up[c][i] = (float) y;
                blockIn = std::max (blockIn, std::abs (pre) * driveGain);
                blockLin = std::max (blockLin, lin);
                blockOut = std::max (blockOut, out);
            }

            if (fading && ++fadePos >= fadeLen)
            {
                fading = false;
                active = 1 - active;
            }
        }

        os.downsample (ch, n);

        const double decay = std::pow (meterDecay, (double) n);
        peakIn = std::max (blockIn, peakIn * decay);
        peakLin = std::max (blockLin, peakLin * decay);
        peakOut = std::max (blockOut, peakOut * decay);
        if (peakIn < 1.0e-30) peakIn = 0.0;
        if (peakLin < 1.0e-30) peakLin = 0.0;
        if (peakOut < 1.0e-30) peakOut = 0.0;
    }

    void publishMeters() noexcept
    {
        meters.inputDriveDb.store (peakIn > 1.0e-5 ? (float) std::max (-100.0, 20.0 * std::log10 (peakIn)) : -100.0f);
        const double sat = peakLin > 1.0e-9 && peakOut > 1.0e-12 ? 20.0 * std::log10 (peakLin / peakOut) : 0.0;
        meters.saturationDb.store ((float) std::clamp (finiteOr (sat, 0.0), 0.0, 60.0));
    }
};

//==============================================================================
Tape::Tape() : impl (std::make_unique<Impl>()) {}
Tape::~Tape() = default;

void Tape::prepare (double sampleRate, int)
{
    auto& m = *impl;
    m.sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    m.osr = m.sr * kOs;
    m.os.prepare (kOsLog2, Oversampler::Quality::MinimumPhase, kControl, 2);
    const double glidePole = std::exp (-(double) kControl / (kGlideStageRatio * kGlideMs * 0.001 * m.sr));
    for (auto* g : { &m.drive, &m.bias, &m.harmonics, &m.lowEmph, &m.highEmph })
        g->c = glidePole;
    m.fadeLen = std::max (1, (int) std::lround (kSpeedFadeMs * 0.001 * m.osr));
    // matched first-order high-pass of the even part: y = a (y1 + x - x1), a = 1 / (1 + w T)
    m.evenDcA = 1.0 / (1.0 + 2.0 * kPi * kEvenDcHz / m.osr);
    m.hystLeak = 1.0 / (kHystLeakMs * 0.001 * m.osr);
    m.meterDecay = std::exp (-1.0 / (kMeterReleaseMs * 0.001 * m.sr));
    for (auto& lane : m.lanes)
        for (auto& c : lane.ch)
            for (auto* f : { &c.pre, &c.de, &c.roll, &c.bump, &c.shelf })
                f->setRampLength (kControl * kOs);
    m.prepared = true;
    m.resetState();
}

void Tape::reset() noexcept { impl->resetState(); }

void Tape::setParams (ParamValues v) noexcept
{
    auto& m = *impl;
    TapeParams p;
    readTapeParams (v, p);
    p.driveDb = (float) finiteOr (p.driveDb, 0.0);
    p.bias = (float) finiteOr (p.bias, 0.0);
    p.harmonics = (float) finiteOr (p.harmonics, 0.0);
    p.lowEmphasis = (float) finiteOr (p.lowEmphasis, 5.0);
    p.highEmphasis = (float) finiteOr (p.highEmphasis, kHighEmphFlat);
    m.target = p;

    if (! m.haveParams)
    {
        m.haveParams = true;
        m.snapAll();
        return;
    }

    m.drive.setTarget (std::clamp ((double) p.driveDb, -30.0, 20.0));
    m.bias.setTarget (std::clamp ((double) p.bias, -10.0, 10.0));
    m.harmonics.setTarget (std::clamp ((double) p.harmonics, 0.0, 10.0));
    m.lowEmph.setTarget (std::clamp ((double) p.lowEmphasis, 0.0, 10.0));
    m.highEmph.setTarget (std::clamp ((double) p.highEmphasis, 0.0, 10.0));
}

void Tape::process (float* const* channels, int numSamples, const ProcessContext&) noexcept
{
    auto& m = *impl;
    if (! m.prepared || numSamples <= 0)
        return;
    if (! m.haveParams)
    {
        m.haveParams = true;
        m.snapAll();
    }

#if PS_TAPE_HAS_SSE
    const unsigned savedCsr = _mm_getcsr();
    _mm_setcsr (savedCsr | 0x8040u);   // FTZ | DAZ
#endif

    for (int done = 0; done < numSamples;)
    {
        if (m.controlPos == 0)
            m.beginPeriod();
        const int n = std::min (numSamples - done, kControl - m.controlPos);
        float* seg[2] { channels[0] + done, channels[1] + done };
        m.processSegment (seg, n);
        m.controlPos += n;
        if (m.controlPos >= kControl)
            m.controlPos = 0;
        done += n;
    }
    m.publishMeters();

#if PS_TAPE_HAS_SSE
    _mm_setcsr (savedCsr);
#endif
}

int Tape::getLatencySamples() const noexcept { return kTapeLatency; }
const TapeMeters& Tape::getMeters() const noexcept { return impl->meters; }

double Tape::responseDb (const TapeParams& p, double hz) noexcept
{
    if (! (hz > 0.0) || ! std::isfinite (hz))
        return 0.0;
    const int s = speedIndex (p.speed);
    const double b = std::clamp (finiteOr (p.bias, 0.0), -10.0, 10.0);
    const double mag = rollOffFor (s).magnitude (hz) * bumpFor (s, finiteOr (p.lowEmphasis, 5.0)).magnitude (hz)
                       * shelfFor (s, finiteOr (p.highEmphasis, kHighEmphFlat), b).magnitude (hz);
    return 20.0 * std::log10 (std::max (mag, 1.0e-12));
}
} // namespace ps
