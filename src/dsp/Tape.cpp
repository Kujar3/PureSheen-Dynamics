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

// Tape (mod-tape). Contract and signal flow: Tape.h. The model and every number in the constant block below are
// fitted to black-box renders of the reference (sweeps from 5 Hz, 100 Hz / 1 / 5 / 10 kHz staircases, step blocks
// and music at every speed, drive -10 .. +30, bias -10 .. +10, harmonics 5 / 10, low emphasis 0 / 2 / 5 / 10 and
// high emphasis 0 / 4 / 7 / 10). Choices made here:
//
// - Per channel at 4x (ps::Oversampler MinimumPhase, 5 samples) up to the downsampler; the low-frequency reproduce
//   sections (all below 600 Hz) and the head bump run at the base rate after it. The linear sections are analog
//   prototypes (s-domain, below) designed by the bilinear transform prewarped at their own corner (the high-frequency
//   sections at kHfWarpHz instead, so the band below 20 kHz is exact); the response view (responseDb) evaluates the same
//   prototypes. The control-driven ones run as ps::SmoothedBiquad (modulation-safe, the ramp covers one control
//   period), a speed's fixed high-rate sections as plain biquads (a speed change crossfades lanes).
// - Record side (before the curve): the bias tilt (a 2nd-order high shelf at kTiltHz times a 1st-order low shelf at
//   kTiltLowHz with its own gain; measured anchors at bias -10 / -5 / +5 / +10, a quadratic in |bias| per side through
//   them, 0 at bias 0), then the pre-emphasis P: a 2nd-order high shelf of kPreDb centred kPreHz x (speed / 30 ips)
//   (the highs reach the curve kPreDb hotter above a corner that moves with the speed, as measured on the 100 Hz ..
//   10 kHz staircases). The tilt sits before the curve because the low-level third harmonic moves with it.
// - The curve (memoryless: no hysteresis or memory was found, the harmonic phases are those of a memoryless curve
//   followed by the reproduce filters): u = kCurveScale * 10^((drive + biasDrive) / 20) * x, the quadratic soft clipper
//   f (u) = u - u |u| / 4 for |u| <= 2, sign (u) beyond (h3 rises 1 dB per dB, h5 = h3 - 17 dB), even harmonics by an
//   operating-point shift f (u + b) - f (b), b = kEvenPerUnit x harmonics (no third harmonic until |u| > b; the small
//   signal gain drops by f' (b) = 1 - b / 2, -0.22 dB at 10, as measured), divided by the same input gain, then the
//   exact inverse of P (small-signal path unchanged). Bias drives the curve harder on both sides of 0 (kBiasDrive*).
// - Reproduce side, per speed (kSpeed* tables, index 0 = 7.5, 1 = 15, 2 = 30 ips): 2nd-order high-pass, 1st-order
//   high-pass, 1st-order low shelf, the head bump (S^2 + S / qn + 1) / (S^2 + S / (qn G) + 1), G = 1 + a x lowEmph (0 =
//   flat, a peak of exactly G at the bump frequency), the ripple bell just above the bump, a mid bell, a broad top
//   bell (the gap / spacing loss at the slow speeds) and a contour bell (the low-frequency contour ripple). The high-pass also removes the DC of the even harmonics.
// - High emphasis: a 1st-order high shelf, measured anchors at 0 / 7 / 10 (4 = exactly flat), linear in dB between
//   them (the corner log-interpolated between 7 and 10).
// - Top roll-off: the reference's response falls from 18 kHz (-0.64 dB at 19 kHz, -2.1 dB at 20 kHz at 44.1 kHz,
//   likely its oversampling filter): one elliptic-type section (a zero pair on the axis, a resonant pole pair) at
//   kRollZeroHz / kRollPoleHz, scaled with the sample rate (at 48 kHz and above it moves above 20 kHz).
// - Level: the reference's drive compensation is static and under-compensates high drives: the output is multiplied by
//   c (drive) / c (0), c = U / f (U), U = kCompRefU x 10^(drive / 20) (a sine of peak U keeps its peak level; bias is not
//   compensated), times the speed's kSpeedGainDb (the reference's small-signal level, +0.66 dB at 1 kHz at 30 ips).
// - Controls: drive (dB), bias, harmonics, lowEmph and highEmph glide (a critically damped two-stage one-pole, 90 %
//   of a step after kGlideMs, no corner at either end), evaluated once per control period of kControl base samples (at
//   fixed sample positions, so the output does not depend on the host's block size) and interpolated per sample inside
//   it. A speed change crossfades two processing lanes (raised cosine, kSpeedFadeMs): the new lane starts from a copy
//   of the old lane's state with the new speed's filters. The base-rate sections belong to the lanes too: during the
//   crossfade both lanes' sets filter the (crossfaded) downsampled signal and their outputs are crossfaded alike.
// - Meters: inputDriveDb = peak of |P (tilt (in)) x drive x bias drive| (dBFS); saturationDb = 20 log10 (peak |u| /
//   peak |curve out|), both peaks with a kMeterReleaseMs release.
// - Latency: the oversampler's 5 samples at every rate. The reproduce filters add their own minimum-phase group delay:
//   at 2 kHz at most 3.9 us (30 ips, no head bump; less at the other settings), a constraint of the fit
//   (fit_tape_lin.py GD_MAX), so the measured 2 kHz delay stays within half a sample of 5 up to 96 kHz; across the mid
//   band the fitted bells ripple within 1 sample, lower down the head-bump roll-off adds its own phase. (The reference also leads by a constant ~15 degrees over the band with zero group delay: not causal, so not
//   reproduced; see Tape.h.)
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
    constexpr double kDriveMinDb = -30.0;      // the drive control's range (Params.h)
    constexpr double kDriveMaxDb = 30.0;
    constexpr double kHfWarpHz = 16000.0;      // bilinear prewarp of the sections centred above it

    // record side: pre-emphasis (2nd-order high shelf), its corner proportional to the speed
    constexpr double kPreDb = 1.578;
    constexpr double kPreHz = 1316.8;          // at 30 ips
    constexpr double kPreQ = 0.776;

    // the curve
    constexpr double kCurveScale = 0.2704;     // u per unit of input at drive 0
    constexpr double kCurveLimit = 2.0;        // f (u) = sign (u) beyond (the flat top at 1)
    constexpr double kEvenPerUnit = 0.00516;   // operating-point shift b per unit of harmonics
    // bias: extra drive into the curve (dB), measured at |bias| 5 and 10 per side (quadratic in |bias| through them)
    constexpr double kBiasDriveNeg[2] { 2.6870, 5.9505 };    // bias -5, -10
    constexpr double kBiasDrivePos[2] { 4.4204, 15.8708 };   // bias +5, +10
    // bias tilt (record side): overall gain (dB), 2nd-order high shelf (dB), 1st-order low shelf (dB) per side
    // at |bias| 5 and 10, and the shapes' corners (the measured corners agree within 1 %, the low shelf's within 20 %)
    constexpr double kTiltGainNeg[2] { -0.4571, -0.8510 }, kTiltGainPos[2] { 0.6283, 1.9124 };
    constexpr double kTiltHighNeg[2] { 1.0190, 2.0248 }, kTiltHighPos[2] { -1.9803, -3.9655 };
    constexpr double kTiltLowNeg[2] { 0.3200, 0.5688 }, kTiltLowPos[2] { -0.5644, -1.1382 };
    constexpr double kTiltHz = 1206.4, kTiltQ = 0.701, kTiltLowHz = 107.6;

    // reproduce side per speed (index 0 = 7.5, 1 = 15, 2 = 30 ips)
    constexpr double kSpeedGainDb[3] { 0.2187, 0.9127, 0.8153 };   // level (with the bells below: +0.66 dB at 1 kHz,
                                                                  //   30 ips)
    constexpr double kHp2Hz[3] { 12.849, 22.892, 42.933 }, kHp2Q[3] { 0.9758, 0.7846, 0.8060 };
    constexpr double kHp1Hz[3] { 45.825, 32.444, 14.899 };
    constexpr double kLowShelfHz[3] { 61.36, 41.22, 305.20 }, kLowShelfDb[3] { 5.0723, 4.1583, -0.6398 };
    constexpr double kBumpHz[3] { 15.075, 25.036, 44.240 };
    constexpr double kBumpQn[3] { 0.8319, 0.8939, 0.8069 };       // numerator Q of the bump
    constexpr double kBumpPerUnit[3] { 0.2794, 0.2443, 0.2742 };  // G = 1 + a x lowEmph (+11.3 dB at 10, 30 ips)
    constexpr double kRippleHz[3] { 35.148, 43.683, 85.039 }, kRippleQ[3] { 2.5234, 1.8955, 1.2613 };
    constexpr double kRippleDb[3] { 0.2926, -0.7155, -1.3969 };
    constexpr double kContourHz[3] { 59.53, 215.06, 404.43 }, kContourQ[3] { 2.7066, 0.7483, 1.2502 };
    constexpr double kContourDb[3] { -0.6486, 0.4048, 0.3358 };
    constexpr double kMidHz[3] { 455.90, 5044.52, 854.91 }, kMidQ[3] { 0.8070, 0.4504, 3.5184 };
    constexpr double kMidDb[3] { 0.8420, -0.7587, -0.1362 };
    constexpr double kTopHz[3] { 8208.28, 10494.44, 11051.11 }, kTopQ[3] { 0.5750, 0.8426, 0.6294 };
    constexpr double kTopDb[3] { -1.0416, -0.7178, -0.1683 };

    // high emphasis (1st-order high shelf): anchors at 0, 7 and 10; 4 = flat
    constexpr double kHighEmphFlat = 4.0;
    constexpr double kHigh0Db = -4.9578, kHigh0Hz = 26276.9;
    constexpr double kHigh7Db = 3.0839, kHigh7Hz = 21708.9;
    constexpr double kHigh10Db = 4.9620, kHigh10Hz = 17246.2;

    // top roll-off at 44.1 kHz (scaled with the sample rate)
    constexpr double kRollRefRate = 44100.0;
    constexpr double kRollZeroHz = 21660.0, kRollPoleHz = 21326.0, kRollPoleQ = 6.48;

    // drive compensation: c (drive) / c (0), c = U / f (U), U = kCompRefU x 10^(drive / 20)
    constexpr double kCompRefU = 0.09957;

    constexpr double kResponseRefHz = 1000.0;   // responseDb is relative to the small-signal level here

    constexpr double kMeterReleaseMs = 300.0;
    constexpr float kMaxInput = 1.0e6f;          // reject corrupt audio above +120 dBFS before the float oversampler

    inline double dbToGain (double db) noexcept { return std::pow (10.0, db * 0.05); }

    //==========================================================================
    // Analog prototypes in S = s / w0: H (S) = (b0 + b1 S + b2 S^2) / (a0 + a1 S + a2 S^2), digitised at `warp` Hz.
    struct Proto
    {
        double hz = 1000.0, warp = 1000.0;
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

        std::complex<double> response (double f) const noexcept
        {
            const std::complex<double> S (0.0, f / hz);
            return (b0 + b1 * S + b2 * S * S) / (a0 + a1 * S + a2 * S * S);
        }
        double magnitude (double f) const noexcept { return std::abs (response (f)); }

        // Bilinear transform prewarped at `warp`: S = c (1 - z^-1) / (1 + z^-1), c = (warp / hz) / tan (pi warp / fs).
        BiquadCoeffs digital (double fs) const noexcept
        {
            const double w = std::min (warp, 0.45 * fs);
            const double c = (w / hz) / std::tan (kPi * w / fs), c2 = c * c;
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

    Proto proto (double hz) noexcept
    {
        Proto p;
        p.hz = hz;
        p.warp = std::min (hz, kHfWarpHz);
        return p;
    }

    Proto scaled (Proto p, double g) noexcept
    {
        p.b0 *= g;
        p.b1 *= g;
        p.b2 *= g;
        return p;
    }

    Proto highPass2 (double hz, double q) noexcept
    {
        Proto p = proto (hz);
        p.b0 = 0.0; p.b1 = 0.0; p.b2 = 1.0;
        p.a0 = 1.0; p.a1 = 1.0 / q; p.a2 = 1.0;
        return p;
    }

    Proto highPass1 (double hz) noexcept
    {
        Proto p = proto (hz);
        p.b0 = 0.0; p.b1 = 1.0;
        p.a0 = 1.0; p.a1 = 1.0;
        return p;
    }

    // First-order high shelf: DC gain 1, HF gain 10^(gainDb / 20), half the gain (in dB) at hz.
    Proto highShelf1 (double hz, double gainDb) noexcept
    {
        const double sg = std::pow (10.0, gainDb / 40.0);
        Proto p = proto (hz);
        p.b0 = 1.0; p.b1 = sg;
        p.a0 = 1.0; p.a1 = 1.0 / sg;
        return p;
    }

    // First-order low shelf: DC gain 10^(gainDb / 20), HF gain 1, half the gain at hz.
    Proto lowShelf1 (double hz, double gainDb) noexcept
    {
        return scaled (highShelf1 (hz, -gainDb), dbToGain (gainDb));
    }

    // Second-order high shelf: DC gain 1, HF gain A^2 (A = 10^(gainDb / 40)), A at hz.
    Proto highShelf2 (double hz, double gainDb, double q) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0), sa = std::sqrt (A);
        Proto p = proto (hz);
        p.b0 = A; p.b1 = A * sa / q; p.b2 = A * A;
        p.a0 = A; p.a1 = sa / q; p.a2 = 1.0;
        return p;
    }

    Proto inverse (Proto p) noexcept
    {
        std::swap (p.b0, p.a0);
        std::swap (p.b1, p.a1);
        std::swap (p.b2, p.a2);
        return p;
    }

    Proto bell (double hz, double q, double gainDb) noexcept
    {
        const double a = std::pow (10.0, gainDb / 40.0);
        Proto p = proto (hz);
        p.b0 = 1.0; p.b1 = a / q; p.b2 = 1.0;
        p.a0 = 1.0; p.a1 = 1.0 / (a * q); p.a2 = 1.0;
        return p;
    }

    // The head bump: (S^2 + S / qn + 1) / (S^2 + S / (qn G) + 1), exactly G at hz, 1 at G = 1.
    Proto headBump (double hz, double qn, double g) noexcept
    {
        Proto p = proto (hz);
        p.b0 = 1.0; p.b1 = 1.0 / qn; p.b2 = 1.0;
        p.a0 = 1.0; p.a1 = 1.0 / (qn * g); p.a2 = 1.0;
        return p;
    }

    // Top roll-off: (1 + S^2 (fp / fz)^2) / (1 + S / Q + S^2), S = s / wp (a zero pair on the axis at fz).
    Proto rollOff (double rate) noexcept
    {
        const double k = rate / kRollRefRate, fz = kRollZeroHz * k, fp = kRollPoleHz * k;
        Proto p;
        p.hz = fp;
        p.warp = fp;
        const double r = fp / fz;
        p.b0 = 1.0; p.b1 = 0.0; p.b2 = r * r;
        p.a0 = 1.0; p.a1 = 1.0 / kRollPoleQ; p.a2 = 1.0;
        return p;
    }

    //==========================================================================
    // Control-value laws (shared by the DSP and responseDb).
    inline int speedIndex (TapeSpeed s) noexcept { return std::clamp ((int) s, 0, 2); }

    // c1 |b| + c2 b^2 through the values at |b| = 5 and 10 (0 at 0).
    inline double sideLaw (const double (&v)[2], double absBias) noexcept
    {
        const double c2 = (v[1] - 2.0 * v[0]) / 50.0;
        const double c1 = (v[0] - 25.0 * c2) / 5.0;
        return absBias * (c1 + c2 * absBias);
    }

    inline double biasLaw (double bias, const double (&neg)[2], const double (&pos)[2]) noexcept
    {
        const double b = std::clamp (bias, -10.0, 10.0);
        return b < 0.0 ? sideLaw (neg, -b) : sideLaw (pos, b);
    }

    inline double biasDriveDb (double bias) noexcept { return biasLaw (bias, kBiasDriveNeg, kBiasDrivePos); }

    Proto tiltHighFor (double bias) noexcept
    {
        return scaled (highShelf2 (kTiltHz, biasLaw (bias, kTiltHighNeg, kTiltHighPos), kTiltQ),
                       dbToGain (biasLaw (bias, kTiltGainNeg, kTiltGainPos)));
    }

    Proto tiltLowFor (double bias) noexcept { return lowShelf1 (kTiltLowHz, biasLaw (bias, kTiltLowNeg, kTiltLowPos)); }

    Proto preFor (int s) noexcept { return highShelf2 (kPreHz * kTapeSpeedIps[(size_t) s] / 30.0, kPreDb, kPreQ); }

    Proto bumpFor (int s, double lowEmph) noexcept
    {
        return headBump (kBumpHz[s], kBumpQn[s], 1.0 + kBumpPerUnit[s] * std::clamp (lowEmph, 0.0, 10.0));
    }

    Proto highEmphFor (double highEmph) noexcept
    {
        const double e = std::clamp (highEmph, 0.0, 10.0);
        if (e <= kHighEmphFlat)
            return highShelf1 (kHigh0Hz, kHigh0Db * (kHighEmphFlat - e) / kHighEmphFlat);
        if (e <= 7.0)
            return highShelf1 (kHigh7Hz, kHigh7Db * (e - kHighEmphFlat) / (7.0 - kHighEmphFlat));
        const double t = (e - 7.0) / 3.0;
        return highShelf1 (kHigh7Hz * std::pow (kHigh10Hz / kHigh7Hz, t), kHigh7Db + t * (kHigh10Db - kHigh7Db));
    }

    // The reproduce sections of a speed that run at the high rate (they reach into the top octave), in order: the
    // de-emphasis (exact reciprocal of the record pre-emphasis), the mid bell, the top bell.
    constexpr int kNumHigh = 3;
    void highSectionsFor (int s, Proto (&out)[kNumHigh]) noexcept
    {
        out[0] = inverse (preFor (s));
        out[1] = bell (kMidHz[s], kMidQ[s], kMidDb[s]);
        out[2] = bell (kTopHz[s], kTopQ[s], kTopDb[s]);
    }

    // The low-frequency reproduce sections of a speed, at the base rate after the downsampler (all below 600 Hz, so
    // the bilinear map is exact enough there): high-passes, low shelf, ripple and contour bells. The head bump follows.
    constexpr int kNumLow = 5;
    void lowSectionsFor (int s, Proto (&out)[kNumLow]) noexcept
    {
        out[0] = highPass2 (kHp2Hz[s], kHp2Q[s]);
        out[1] = highPass1 (kHp1Hz[s]);
        out[2] = lowShelf1 (kLowShelfHz[s], kLowShelfDb[s]);
        out[3] = bell (kRippleHz[s], kRippleQ[s], kRippleDb[s]);
        out[4] = bell (kContourHz[s], kContourQ[s], kContourDb[s]);
    }

    //==========================================================================
    // The tape curve: f (u) = u - u |u| / 4 for |u| <= 2, sign (u) beyond.
    inline double curve (double u) noexcept
    {
        const double a = std::min (std::abs (u), kCurveLimit);
        const double y = a - 0.25 * a * a;
        return u < 0.0 ? -y : y;
    }

    // Drive compensation c (drive) / c (0).
    double compFor (double driveDb) noexcept
    {
        const auto c = [] (double u) { return u / curve (u); };
        return c (kCompRefU * dbToGain (driveDb)) / c (kCompRefU);
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
        double g = kCurveScale;    // input gain into the curve (u per unit of the pre-emphasised input)
        double b = 0.0;            // even-harmonics operating point
        double meterGain = 1.0;    // drive and bias drive alone (meter)
    };

    // High-rate sections of one channel of one lane. The speed's own sections are fixed for the lane's life (a speed
    // change crossfades lanes); the control-driven ones glide.
    struct Channel
    {
        SmoothedBiquad tiltHigh, tiltLow, emph;
        Biquad pre, high[kNumHigh], roll;
    };

    // Base-rate low-frequency sections of one channel of one lane (after the shared downsampler: during a speed
    // crossfade both lanes' sets filter the crossfaded signal and their outputs are crossfaded the same way).
    struct LowChannel
    {
        SmoothedBiquad low[kNumLow], bump;
    };

    struct Lane
    {
        int speed = 2;
        Channel ch[2];
        LowChannel lowCh[2];
        double makeupStart = 1.0, makeupEnd = 1.0;
        // designed control values (filters are re-targeted only when these change)
        double dBias = 1.0e9, dLow = 1.0e9, dHigh = 1.0e9, mDrive = 1.0e9;
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

    // meters (internal units)
    double peakIn = 0.0, peakLin = 0.0, peakOut = 0.0, meterDecay = 0.0;

    //==========================================================================
    void setLaneFilters (Lane& lane, bool snap) noexcept
    {
        const double b = bias.value, lo = lowEmph.value, hi = highEmph.value;
        if (snap || b != lane.dBias)
        {
            const auto th = tiltHighFor (b).digital (osr);
            const auto tl = tiltLowFor (b).digital (osr);
            for (auto& c : lane.ch)
            {
                c.tiltHigh.setTarget (th);
                c.tiltLow.setTarget (tl);
            }
        }
        if (snap || hi != lane.dHigh)
        {
            const auto emph = highEmphFor (hi).digital (osr);
            for (auto& c : lane.ch)
                c.emph.setTarget (emph);
        }
        if (snap || lo != lane.dLow)
        {
            const auto bump = bumpFor (lane.speed, lo).digital (sr);
            for (auto& c : lane.lowCh)
                c.bump.setTarget (bump);
        }
        if (snap)
        {
            Proto lp[kNumLow];
            lowSectionsFor (lane.speed, lp);
            for (int i = 0; i < kNumLow; ++i)
            {
                const auto k = lp[i].digital (sr);
                for (auto& c : lane.lowCh)
                {
                    c.low[i].setTarget (k);
                    c.low[i].snapToTarget();
                }
            }
            for (auto& c : lane.lowCh)
                c.bump.snapToTarget();
            Proto hp[kNumHigh];
            highSectionsFor (lane.speed, hp);
            const auto pre = preFor (lane.speed).digital (osr);
            const auto roll = rollOff (sr).digital (osr);
            for (auto& c : lane.ch)
            {
                c.pre.setCoeffs (pre);
                for (int i = 0; i < kNumHigh; ++i)
                    c.high[i].setCoeffs (hp[i].digital (osr));
                c.roll.setCoeffs (roll);
                c.tiltHigh.snapToTarget();
                c.tiltLow.snapToTarget();
                c.emph.snapToTarget();
            }
        }
        lane.dBias = b;
        lane.dLow = lo;
        lane.dHigh = hi;
    }

    void setLaneMakeup (Lane& lane, bool snap) noexcept
    {
        lane.makeupStart = lane.makeupEnd;
        if (snap || drive.value != lane.mDrive)
        {
            lane.makeupEnd = finiteOr (compFor (drive.value) * dbToGain (kSpeedGainDb[lane.speed]), 1.0);
            lane.mDrive = drive.value;
        }
        if (snap)
            lane.makeupStart = lane.makeupEnd;
    }

    Shape shapeNow() const noexcept
    {
        Shape s;
        s.meterGain = dbToGain (drive.value + biasDriveDb (bias.value));
        s.g = kCurveScale * s.meterGain;
        s.b = kEvenPerUnit * std::clamp (harmonics.value, 0.0, 10.0);
        return s;
    }

    void snapAll() noexcept
    {
        drive.snap (std::clamp ((double) target.driveDb, kDriveMinDb, kDriveMaxDb));
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
                for (auto* f : { &c.tiltHigh, &c.tiltLow, &c.emph })
                    f->reset();
                c.pre.reset();
                c.roll.reset();
                for (auto& f : c.high)
                    f.reset();
            }
        for (auto& lane : lanes)
            for (auto& c : lane.lowCh)
            {
                c.bump.reset();
                for (auto& f : c.low)
                    f.reset();
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

    // One channel of one lane at the high rate. lin / out: |u| and |curve output| (for the meters), pre: the
    // pre-emphasised input.
    static inline double tick (Channel& c, double in, double g, double invG, double b, double fb, double& lin,
                               double& out, double& pre) noexcept
    {
        pre = c.pre.processSample (c.tiltLow.processSample (c.tiltHigh.processSample (in)));
        const double u = pre * g;
        const double y = curve (u + b) - fb;
        lin = std::abs (u);
        out = std::abs (y);
        double v = y * invG;
        for (auto& f : c.high)
            v = f.processSample (v);
        v = c.emph.processSample (v);
        return c.roll.processSample (v);
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

        double sampleIn = 0.0, sampleLin = 0.0, sampleOut = 0.0;
        // the lanes at the segment's start and the new lane's crossfade weight at each base sample
        const int laneA = active;
        const bool fadeAtStart = fading;
        double wBase[kControl] {};

        for (int i = 0; i < n4; ++i)
        {
            const double t = (double) (base4 + i + 1) / period4;
            auto lerp = [t] (double a, double b) { return a + (b - a) * t; };
            const double g = lerp (shapeStart.g, shapeEnd.g);
            const double invG = 1.0 / g;
            const double b = lerp (shapeStart.b, shapeEnd.b);
            const double fb = curve (b);
            const double meterGain = lerp (shapeStart.meterGain, shapeEnd.meterGain);

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
                double y = tick (la.ch[c], x, g, invG, b, fb, lin, out, pre) * lerp (la.makeupStart, la.makeupEnd);
                if (fading)
                {
                    auto& lb = lanes[1 - active];
                    double lin2 = 0.0, out2 = 0.0, pre2 = 0.0;
                    const double y2 = tick (lb.ch[c], x, g, invG, b, fb, lin2, out2, pre2)
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
                sampleIn = std::max (sampleIn, std::abs (pre) * meterGain);
                sampleLin = std::max (sampleLin, lin);
                sampleOut = std::max (sampleOut, out);
            }

            if ((i + 1) % kOs == 0)
            {
                wBase[i / kOs] = fading ? wNew : (fadeAtStart ? 1.0 : 0.0);
                // Release once per base sample, after collecting its four high-rate peaks. A segment peak would
                // hold earlier samples until the segment's end, making the meters depend on host block boundaries.
                peakIn = std::max (sampleIn, peakIn * meterDecay);
                peakLin = std::max (sampleLin, peakLin * meterDecay);
                peakOut = std::max (sampleOut, peakOut * meterDecay);
                sampleIn = sampleLin = sampleOut = 0.0;
            }
            if (fading && ++fadePos >= fadeLen)
            {
                fading = false;
                active = 1 - active;
            }
        }

        os.downsample (ch, n);

        // the base-rate low-frequency sections and the head bump (both lanes' during a crossfade)
        auto low = [] (LowChannel& lc, double v) {
            for (auto& f : lc.low)
                v = f.processSample (v);
            return lc.bump.processSample (v);
        };
        for (int c = 0; c < 2; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
            {
                double y = low (lanes[laneA].lowCh[c], d[i]);
                if (fadeAtStart)
                    y += wBase[i] * (low (lanes[1 - laneA].lowCh[c], d[i]) - y);
                d[i] = (float) y;
            }
        }

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
    m.fadeLen = std::max (kOs, (int) std::lround (kSpeedFadeMs * 0.001 * m.osr));
    m.meterDecay = std::exp (-1.0 / (kMeterReleaseMs * 0.001 * m.sr));
    for (auto& lane : m.lanes)
    {
        for (auto& c : lane.ch)
            for (auto* f : { &c.tiltHigh, &c.tiltLow, &c.emph })
                f->setRampLength (kControl * kOs);
        for (auto& c : lane.lowCh)
        {
            c.bump.setRampLength (kControl);
            for (auto& f : c.low)
                f.setRampLength (kControl);
        }
    }
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
    p.lowEmphasis = (float) finiteOr (p.lowEmphasis, 2.0);
    p.highEmphasis = (float) finiteOr (p.highEmphasis, kHighEmphFlat);
    m.target = p;

    if (! m.haveParams)
    {
        m.haveParams = true;
        m.snapAll();
        return;
    }

    m.drive.setTarget (std::clamp ((double) p.driveDb, kDriveMinDb, kDriveMaxDb));
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
    Proto hp[kNumHigh], lp[kNumLow];
    highSectionsFor (s, hp);
    lowSectionsFor (s, lp);
    const Proto controls[4] { tiltHighFor (b), tiltLowFor (b), bumpFor (s, finiteOr (p.lowEmphasis, 2.0)),
                              highEmphFor (finiteOr (p.highEmphasis, kHighEmphFlat)) };
    // hp[0] (the de-emphasis) cancels the record pre-emphasis: both are left out
    auto mag = [&hp, &lp, &controls] (double f)
    {
        double m = 1.0;
        for (int i = 1; i < kNumHigh; ++i)
            m *= hp[i].magnitude (f);
        for (const auto& c : lp)
            m *= c.magnitude (f);
        for (const auto& c : controls)
            m *= c.magnitude (f);
        return std::max (m, 1.0e-12);
    };
    return 20.0 * std::log10 (mag (hz) / mag (kResponseRefHz));
}
} // namespace ps
