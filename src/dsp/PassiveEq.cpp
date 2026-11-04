#include "dsp/PassiveEq.h"

#include "dsp/Envelope.h"
#include "dsp/PassiveEqDesign.h"
#include "dsp/Oversampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_PEQ_HAS_SSE 1
#endif

// Passive EQ (mod-peq). Contract and signal flow: PassiveEq.h. Choices made here (the header leaves them open):
//
// - Sections: analog prototypes measured from the reference's renders (out-of-tree measurement notes; every number
//   below comes from a fit of its curves, 1/24 octave, 20 Hz - 19 kHz; F = s / 2 pi, in Hz):
//     low boost   H = 1 + cB / (F + pB): the input plus a first-order low-passed copy (a passive parallel network).
//                 cB is fixed per frequency position (kLowBoostC); the amount moves only the pole, pB = cB / gB with
//                 gB = 10^(plateau / 20) - 1, the plateau (dB at DC) a monotone cubic (PCHIP) over kLowBoostDb.
//     low cut     H = 1 - cC / (F + pC) = (F + zC) / (F + pC), zC = pC - cC; cC per position (kLowCutC), the depth
//                 d = cC / pC from the plateau 20 log10 (1 - d) over kLowCutDb.
//     both        H = 1 + cB / (F + pB) - cC / (F + pC) + delta / ((F + pB) (F + pC)): the parallel connection with a
//                 measured cross term delta = kLowCross cB cC (the same at cut 5 and 10, so independent of both
//                 amounts; it vanishes with either section, whose pole then goes to infinity). Realised as the cut
//                 (section 2) times H / cut = 1 + (cB (F + pC) + delta) / ((F + pB) (F + zC)) (section 1), so each
//                 section is continuous through its own amount 0. Both bilinear (prewarped at kLowWarpHz: their poles
//                 and zeros are below 3 kHz, where the bilinear map is exact to < 0.001 dB).
//     high boost  an analog peak, H = (S^2 + S A / Q + 1) / (S^2 + S / (A Q) + 1), S = F / f0, A = 10^(g / 40), i.e. the
//                 input plus a band-passed copy, 1 + c BP (c = A^2 - 1, band-pass Q Qp = A Q). Per position f0 and Q
//                 at boost 5 (kHighBoostF0 / kHighBoostQ; the 8k position peaks near 6.2 kHz), the peak dB a PCHIP
//                 over kHighBoostDb (the same at every position), Q and f0 scaled by the amount (kHighBoostQMul,
//                 kHighBoostF0Mul). High Q scales Qp by kHighQPoleMul and c by kHighQPoleMul x kHighQGainMul (c / Qp
//                 stays almost constant, as when a series resistance of an LC branch changes), f0 by kHighQF0Mul. The
//                 reference's peak is not exactly this shape (a sharper top, a different upper skirt); a first-order
//                 high shelf of kHighTailShare x g^2 / 5 dB centred at kHighTailRatio x f0 brings every measured
//                 setting within 0.12 dB of it (analog fit, magnitude plus a light phase weight; without the shelf
//                 the 10 dB boost is 0.48 dB off). The tail is part of section 4.
//     high cut    first-order high shelf, H = (G F + p) / (F + p): DC 1, far gain G = 1 - kHighCutDepth x
//                 kHighCutPosDepth[pos] x (amount / 10)^kHighCutExp; its pole p = kHighCutPole[pos] x
//                 kHighCutPoleStep^((amount - 5) / 5) (two amounts measured: 5 and 10 at 5 kHz).
//     mid bands   analog peaks (as the high boost), f0 = the label x kMidF0Ratio, dB a PCHIP over (0, 5, 10) and Q
//                 linear between 5 and 10 (held below 5): the boosts widen with the amount, the cut narrows.
//   Positions and amounts the renders do not show (low cut at 30 / 45 Hz, high cut at 10 kHz with other amounts, high
//   boost at other amounts per position, High Q at other amounts, mid bands at other positions, every amount between
//   the measured ones) follow the most plausible smooth law: monotone cubic interpolation over the measured points, a
//   position ratio carried over unchanged, a multiplier held outside its measured span.
// - Digital realisation: high boost, high cut, tail and mid peaks use the matched designs of design:: (Filters.h),
//   which follow the analog magnitude up to Nyquist (no bilinear cramping); a peak centred above Nyquist (the 16k
//   position, f0 21 kHz) is clamped to 0.49 x the sample rate there.
// - The reference also rolls off above 17 kHz in every render (-0.64 dB at 19 kHz, -2.1 dB at 20 kHz at 44.1 kHz,
//   most likely the anti-alias filter of an oversampler); it is not modelled (inaudible, unknown at other rates).
// - A section at amount 0 keeps its poles and gets numerator = denominator (exactly unity: the SVF output weights are
//   exactly 0 and 1), so its state keeps running and engaging it never starts from a cold state; a frequency step at
//   amount 0 glides its poles without touching the audio (bit-exact). Section 4 is unity only when the high cut AND
//   the high boost are 0 (it carries the boost's tail). Its two first-order factors glide independently, so changing
//   either control near zero cannot change the other shelf's realisation during the glide.
// - Designs are recomputed in setParams only for the sections whose inputs changed; each channel picks up a changed
//   set at the start of the next process() and glides there over kGlideMs (SmoothedBiquad). The first parameters
//   after prepare / reset are applied at once (no glide from the defaults).
// - Tube stage (at 2x, ps::Oversampler MinimumPhase), our addition (the reference has none; at drive 0 it is
//   bypassed): a triode-like operating point on a tanh curve,
//   y = (tanh (k (x + b)) - tanh (k b)) / (k sech^2 (k b)), k = kDriveMax x drive / 10, b = kTubeBias: slope 1 and
//   no offset at x = 0, the asymmetry gives mostly 2nd harmonic (drive 10, -18 dBFS: about -32 dBc H2, -53 dBc H3).
//   Only the residual y - x goes through a DC blocker (kResidualHpHz). The downsampler receives the compensated
//   residual, gain * h + (gain - 1) * x; it is added to the exact delayed clean path. Thus switching Drive cannot
//   crossfade two full-level copies with different phase and notch the treble. Make-up: a table over drive
//   (101 points, built in prepare) of the fundamental gain of a -18 dBFS sine, inverted. Drive glides linearly over
//   kDriveSmoothMs.
// - Drive 0: the output is the input delayed by kPassiveEqLatency in a plain delay line (bit-exact); the oversampled
//   path keeps running unheard (warm), and leaving / returning to 0 fades its residual (raised cosine, kCrossfadeMs).
//   The residual retains the minimum-phase round trip's phase; the clean signal always has exactly 3 samples' delay.
// - Denormals: FTZ / DAZ while processing (and the filter states flush tiny values). Non-finite or corrupt input
//   samples are rejected before entering any recursive state.
namespace ps
{
namespace
{
    //==========================================================================
    // Measured coefficients (fitted to the reference's renders; see the model above). Amounts are the labelled dB
    // (0..10). Frequency positions follow kPassiveEq*Hz (Types.h).
    // top section, low: positions 20, 30, 45, 60, 100 Hz
    constexpr double kLowBoostC[5] { 80.18, 176.39, 275.81, 375.14, 534.16 };      // cB (Hz)
    constexpr double kLowCutC[5] { 133.64, 290.9, 449.9, 607.15, 890.22 };         // cC (Hz); 30 / 45 Hz interpolated
    constexpr double kLowAmount[5] { 0.0, 2.5, 5.0, 7.5, 10.0 };
    constexpr double kLowBoostDb[5] { 0.0, 4.1079, 8.668, 13.0352, 16.6306 };      // plateau at DC
    constexpr double kLowCutAmount[4] { 0.0, 2.5, 5.0, 10.0 };
    constexpr double kLowCutDb[4] { 0.0, -9.8251, -14.3175, -19.4597 };            // plateau at DC
    constexpr double kLowCross = 0.0755;          // boost + cut cross term delta = this x cB x cC (Hz^2)
    constexpr double kLowWarpHz = 1000.0;         // bilinear prewarp of the low sections
    // top section, high boost: positions 3, 4, 5, 8, 10, 12, 16 kHz (at boost 5, High Q 0)
    constexpr double kHighBoostF0[7] { 3126.21, 4103.15, 5271.94, 6161.34, 10155.49, 12791.20, 21019.72 };
    constexpr double kHighBoostQ[7] { 0.57360, 0.44250, 0.52108, 0.44947, 0.41390, 0.33511, 0.47461 };
    constexpr double kHighBoostAmount[4] { 0.0, 2.5, 5.0, 10.0 };
    constexpr double kHighBoostDb[4] { 0.0, 1.13590, 3.35281, 12.44202 };          // peak gain g
    constexpr double kHighBoostMulAmount[3] { 2.5, 5.0, 10.0 };
    constexpr double kHighBoostQMul[3] { 1.36010, 1.0, 0.93941 };                  // Q re boost 5 (held outside)
    constexpr double kHighBoostF0Mul[3] { 0.99857, 1.0, 1.04010 };                 // f0 re boost 5 (held outside)
    constexpr double kHighQAmount[3] { 0.0, 5.0, 10.0 };
    constexpr double kHighQPoleMul[3] { 1.0, 1.28535, 1.78723 };                   // band-pass Q multiplier
    constexpr double kHighQGainMul[3] { 1.0, 1.00793, 1.02198 };                   // extra c multiplier
    constexpr double kHighQF0Mul[3] { 1.0, 1.00585, 1.01230 };
    constexpr double kHighTailRatio = 1.85940;     // tail shelf centre = this x f0
    constexpr double kHighTailShare = -0.051177;   // tail shelf dB = this x g^2 / 5 (g = the peak dB)
    // top section, high cut: positions 5, 10, 20 kHz
    constexpr double kHighCutDepth = 0.925014;     // 1 - far gain at cut 10 (5 kHz position)
    constexpr double kHighCutExp = 1.2312;         // depth law exponent
    constexpr double kHighCutPosDepth[3] { 1.0, 1.00718, 1.04102 };
    constexpr double kHighCutPole[3] { 1987.0, 3178.0, 6432.0 };                    // pole at cut 5 (Hz)
    constexpr double kHighCutPoleStep = 0.58430;   // pole ratio cut 10 / cut 5 (log-linear in the amount)
    constexpr double kHighCutMinGain = 0.01;       // far-gain floor (-40 dB)
    // bottom section: low-mid boost, mid cut, high-mid boost (amounts 5 and 10 measured)
    constexpr double kMidAmount[3] { 0.0, 5.0, 10.0 };
    constexpr double kMidDb[3][3] { { 0.0, 4.3356, 12.9474 }, { 0.0, -5.0783, -14.2001 }, { 0.0, 3.2830, 8.5816 } };
    constexpr double kMidQ[3][2] { { 0.9695, 0.7971 }, { 0.8666, 1.6425 }, { 2.7075, 1.8418 } };        // at 5, 10
    constexpr double kMidF0Ratio[3][2] { { 1.21420, 1.19393 }, { 1.03780, 1.03237 }, { 1.22732, 1.22893 } };
    constexpr double kMinAmount = 1.0e-4;          // below this a section is off (exactly unity)
    constexpr double kGlideMs = 10.0;              // coefficient glide
    constexpr double kNeutralProbe = 1.0;          // a neutral section keeps the poles of its design at this amount
    // tube make-up stage (our addition)
    constexpr double kDriveMax = 1.55;             // curve gain k at drive 10
    constexpr double kTubeBias = 0.21;             // operating-point offset b (the asymmetry: even harmonics)
    constexpr double kResidualHpHz = 10.0;         // DC blocker on the saturator's residual
    constexpr double kMakeupRefDbfs = -18.0;       // level compensation reference (sine peak)
    constexpr double kDriveSmoothMs = 20.0;        // drive glide
    constexpr double kCrossfadeMs = 20.0;          // drive 0 <-> tube path

    constexpr double kPi = 3.14159265358979323846;
    constexpr float kMaxInput = 1.0e6f;            // reject corrupt audio before the EQ and float oversampler
    constexpr int kChunk = 64;                     // oversampler block
    constexpr int kMakeupPoints = 101;             // make-up table over drive 0..10
    constexpr int kMakeupCycle = 512;              // samples per cycle of its measurement

    using namespace peq;   // the Section enum

    inline void flushTiny (double& v) noexcept
    {
        if (std::abs (v) < 1.0e-30)
            v = 0.0;
    }

    // Monotone cubic (PCHIP, Fritsch-Carlson slopes with the three-point end rule) through (x[i], y[i]), x
    // increasing, n >= 2; held at the end values outside [x[0], x[n-1]].
    double pchip (const double* x, const double* y, int n, double v) noexcept
    {
        if (! (v > x[0]))
            return y[0];
        if (! (v < x[n - 1]))
            return y[n - 1];
        int i = 0;
        while (i < n - 2 && v >= x[i + 1])
            ++i;
        auto delta = [&] (int k) { return (y[k + 1] - y[k]) / (x[k + 1] - x[k]); };
        auto slope = [&] (int k) -> double
        {
            if (n == 2)
                return delta (0);
            if (k == 0 || k == n - 1)
            {
                const int a = k == 0 ? 0 : n - 2, b = k == 0 ? 1 : n - 3;   // the edge interval and its neighbour
                const double h0 = x[a + 1] - x[a], h1 = x[b + 1] - x[b];
                const double d0 = delta (a), d1 = delta (b);
                double d = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
                if (d * d0 <= 0.0)
                    d = 0.0;
                else if (d0 * d1 <= 0.0 && std::abs (d) > 3.0 * std::abs (d0))
                    d = 3.0 * d0;
                return d;
            }
            const double dl = delta (k - 1), dr = delta (k);
            if (dl * dr <= 0.0)
                return 0.0;
            const double hl = x[k] - x[k - 1], hr = x[k + 1] - x[k];
            const double w1 = 2.0 * hr + hl, w2 = hr + 2.0 * hl;
            return (w1 + w2) / (w1 / dl + w2 / dr);
        };
        const double h = x[i + 1] - x[i], t = (v - x[i]) / h;
        const double t2 = t * t, t3 = t2 * t;
        return (2.0 * t3 - 3.0 * t2 + 1.0) * y[i] + (t3 - 2.0 * t2 + t) * h * slope (i)
               + (-2.0 * t3 + 3.0 * t2) * y[i + 1] + (t3 - t2) * h * slope (i + 1);
    }

    double amountOf (float a) noexcept
    {
        return std::isfinite (a) ? std::clamp ((double) a, 0.0, 10.0) : 0.0;
    }

    int indexOf (int index, int n) noexcept { return std::clamp (index, 0, n - 1); }

    template <size_t N>
    double pick (const std::array<double, N>& hz, int index) noexcept
    {
        return hz[(size_t) indexOf (index, (int) N)];
    }

    // The inputs of one section (a redesign happens only when they change).
    struct SectionKey
    {
        int hzIndex = -1, hzIndex2 = -1;
        float amount = -1.0f, amount2 = -1.0f, extra = -1.0f;

        bool operator== (const SectionKey& o) const noexcept
        {
            return hzIndex == o.hzIndex && hzIndex2 == o.hzIndex2 && amount == o.amount && amount2 == o.amount2
                   && extra == o.extra;
        }
    };

    SectionKey keyOf (int section, const PassiveEqSetParams& p) noexcept
    {
        switch (section)
        {
            case kLowBoost:
            case kLowCut:    return { p.lowHzIndex, 0, p.lowBoost, p.lowCut, 0.0f };
            case kHighBoost: return { p.highHzIndex, 0, p.highBoost, 0.0f, p.highQ };
            case kHighCut:   return { p.highCutHzIndex, p.highHzIndex, p.highCut, p.highBoost, p.highQ };
            case kLowMid:    return { p.lowMidHzIndex, 0, p.lowMidBoost, 0.0f, 0.0f };
            case kMidCut:    return { p.midHzIndex, 0, p.midCut, 0.0f, 0.0f };
            default:         return { p.highMidHzIndex, 0, p.highMidBoost, 0.0f, 0.0f };
        }
    }

    //==========================================================================
    // The measured laws.
    struct LowNet
    {
        double cB = 0.0, pB = 0.0, cC = 0.0, pC = 0.0, zC = 0.0;   // Hz; a section is off when its c is 0
    };

    LowNet lowNet (int position, double boost, double cut) noexcept
    {
        const int i = indexOf (position, 5);
        LowNet n;
        if (boost >= kMinAmount)
        {
            const double g = std::pow (10.0, pchip (kLowAmount, kLowBoostDb, 5, boost) / 20.0) - 1.0;
            n.cB = kLowBoostC[i];
            n.pB = n.cB / std::max (g, 1.0e-9);
        }
        if (cut >= kMinAmount)
        {
            const double d = 1.0 - std::pow (10.0, pchip (kLowCutAmount, kLowCutDb, 4, cut) / 20.0);
            n.cC = kLowCutC[i];
            n.pC = n.cC / std::max (d, 1.0e-9);
            n.zC = n.pC - n.cC;
        }
        return n;
    }

    struct Peak
    {
        double hz = 1000.0, q = 0.707, db = 0.0;
    };

    Peak highBoost (int position, double boost, double highQ) noexcept
    {
        const int i = indexOf (position, 7);
        const double g = pchip (kHighBoostAmount, kHighBoostDb, 4, boost);
        const double q = kHighBoostQ[i] * pchip (kHighBoostMulAmount, kHighBoostQMul, 3, boost);
        const double f0 = kHighBoostF0[i] * pchip (kHighBoostMulAmount, kHighBoostF0Mul, 3, boost);
        const double hq = std::clamp (highQ, 0.0, 10.0);
        const double rq = pchip (kHighQAmount, kHighQPoleMul, 3, hq);
        // 1 + c BP form: c = A^2 - 1, Qp = A Q; High Q scales Qp by rq and c by rq x the gain multiplier
        const double A = std::pow (10.0, g / 40.0);
        const double c = (A * A - 1.0) * rq * pchip (kHighQAmount, kHighQGainMul, 3, hq);
        const double qp = A * q * rq;
        const double A2 = std::sqrt (1.0 + c);
        return { f0 * pchip (kHighQAmount, kHighQF0Mul, 3, hq), qp / A2, 40.0 * std::log10 (A2) };
    }

    struct Shelf
    {
        double pole = 1000.0, gain = 1.0;   // first-order high shelf: DC 1, far gain `gain`, pole `pole` (Hz)
    };

    Shelf highCut (int position, double cut) noexcept
    {
        const int i = indexOf (position, 3);
        const double depth = kHighCutDepth * kHighCutPosDepth[i] * std::pow (cut / 10.0, kHighCutExp);
        return { kHighCutPole[i] * std::pow (kHighCutPoleStep, (cut - 5.0) / 5.0), std::max (kHighCutMinGain, 1.0 - depth) };
    }

    Peak midBand (int band, double hzLabel, double amount) noexcept
    {
        const double t = std::clamp ((amount - 5.0) / 5.0, 0.0, 1.0);   // Q and f0 ratio: linear 5..10, held below 5
        return { hzLabel * (kMidF0Ratio[band][0] + (kMidF0Ratio[band][1] - kMidF0Ratio[band][0]) * t),
                 kMidQ[band][0] + (kMidQ[band][1] - kMidQ[band][0]) * t, pchip (kMidAmount, kMidDb[band], 3, amount) };
    }

    //==========================================================================
    // Digital realisations.
    BiquadCoeffs unityWithPoles (const BiquadCoeffs& c) noexcept
    {
        return { 1.0, c.a1, c.a2, c.a1, c.a2 };   // exactly unity, poles kept
    }

    // Bilinear transform (prewarped at kLowWarpHz) of the analog section (F^2 n2 + F n1 + n0) / (F^2 d2 + F d1 + d0)
    // in F = s / 2 pi (Hz); first order when n2 = d2 = 0.
    BiquadCoeffs bilinear (double n2, double n1, double n0, double d2, double d1, double d0, double sr) noexcept
    {
        const double fw = std::min (kLowWarpHz, 0.25 * sr);
        const double K = fw / std::tan (kPi * fw / sr);
        if (n2 == 0.0 && d2 == 0.0)   // first order: b2 = a2 = 0 (no pole at Nyquist)
        {
            const double a0 = d1 * K + d0;
            return { (n1 * K + n0) / a0, (n0 - n1 * K) / a0, 0.0, (d0 - d1 * K) / a0, 0.0 };
        }
        const double K2 = K * K;
        const double B0 = n2 * K2 + n1 * K + n0, B1 = 2.0 * (n0 - n2 * K2), B2 = n2 * K2 - n1 * K + n0;
        const double A0 = d2 * K2 + d1 * K + d0, A1 = 2.0 * (d0 - d2 * K2), A2 = d2 * K2 - d1 * K + d0;
        return { B0 / A0, B1 / A0, B2 / A0, A1 / A0, A2 / A0 };
    }

    // Section 1: the low boost relative to the cut (see the model above); first order without the cut.
    BiquadCoeffs lowBoostDesign (const LowNet& n, double sr) noexcept
    {
        if (n.cC <= 0.0)   // (F + pB + cB) / (F + pB)
            return bilinear (0.0, 1.0, n.pB + n.cB, 0.0, 1.0, n.pB, sr);
        // 1 + (cB (F + pC) + delta) / ((F + pB) (F + zC))
        const double delta = kLowCross * n.cB * n.cC;
        const double d1 = n.pB + n.zC, d0 = n.pB * n.zC;
        return bilinear (1.0, d1 + n.cB, d0 + n.cB * n.pC + delta, 1.0, d1, d0, sr);
    }

    // Section 2: the low cut (F + zC) / (F + pC).
    BiquadCoeffs lowCutDesign (const LowNet& n, double sr) noexcept
    {
        return bilinear (0.0, 1.0, n.zC, 0.0, 1.0, n.pC, sr);
    }

    BiquadCoeffs peakDesign (const Peak& pk, double sr) noexcept
    {
        return design::peak (sr, pk.hz, pk.q, pk.db);
    }

    // First-order high shelf with DC gain 1, far gain G and pole p: centre sqrt (p z), z = p / G.
    BiquadCoeffs highShelfDesign (double pole, double gain, double sr) noexcept
    {
        return design::highShelf1 (sr, pole / std::sqrt (gain), 20.0 * std::log10 (gain));
    }

    BiquadCoeffs multiplyFirstOrder (const BiquadCoeffs& x, const BiquadCoeffs& y) noexcept
    {
        return { x.b0 * y.b0, x.b0 * y.b1 + x.b1 * y.b0, x.b1 * y.b1, x.a1 + y.a1, x.a1 * y.a1 };
    }

    double highTailDb (double peakDb) noexcept
    {
        return kHighTailShare * peakDb * peakDb / 5.0;
    }

    struct HighShelves
    {
        BiquadCoeffs cut, tail;
    };

    HighShelves highShelves (const PassiveEqSetParams& p, double sr) noexcept
    {
        const double cut = amountOf (p.highCut), boost = amountOf (p.highBoost);
        const bool cutOn = cut >= kMinAmount, tailOn = boost >= kMinAmount;
        const auto s = highCut (p.highCutHzIndex, cutOn ? cut : 0.0);
        auto c = highShelfDesign (s.pole, s.gain, sr);
        const auto pk = highBoost (p.highHzIndex, tailOn ? boost : 0.0, amountOf (p.highQ));
        auto t = design::highShelf1 (sr, kHighTailRatio * pk.hz, highTailDb (pk.db));
        if (! cutOn)
            c = unityWithPoles (c);
        if (! tailOn)
            t = unityWithPoles (t);
        return { c, t };
    }

    bool sameSet (const PassiveEqSetParams& a, const PassiveEqSetParams& b) noexcept
    {
        return a.lowHzIndex == b.lowHzIndex && a.lowBoost == b.lowBoost && a.lowCut == b.lowCut
               && a.highHzIndex == b.highHzIndex && a.highBoost == b.highBoost && a.highQ == b.highQ
               && a.highCutHzIndex == b.highCutHzIndex && a.highCut == b.highCut && a.lowMidHzIndex == b.lowMidHzIndex
               && a.lowMidBoost == b.lowMidBoost && a.midHzIndex == b.midHzIndex && a.midCut == b.midCut
               && a.highMidHzIndex == b.highMidHzIndex && a.highMidBoost == b.highMidBoost;
    }
    // The tube curve with slope 1 and no offset at 0 (k > 0).
    struct TubeCurve
    {
        double k = 0.0, kb = 0.0, t0 = 0.0, inv = 0.0;

        void set (double driveNorm) noexcept
        {
            k = kDriveMax * driveNorm;
            kb = k * kTubeBias;
            t0 = std::tanh (kb);
            inv = k > 1.0e-9 ? 1.0 / (k * (1.0 - t0 * t0)) : 0.0;
        }

        bool active() const noexcept { return k > 1.0e-9; }
        double operator() (double x) const noexcept { return (std::tanh (k * x + kb) - t0) * inv; }
    };
} // namespace

//==============================================================================
namespace peq
{
BiquadCoeffs designSection (int section, const PassiveEqSetParams& p, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    switch (section)
    {
        case kLowBoost:
        {
            const auto n = lowNet (p.lowHzIndex, amountOf (p.lowBoost), amountOf (p.lowCut));
            if (n.cB > 0.0)
                return lowBoostDesign (n, sr);
            return unityWithPoles (lowBoostDesign (lowNet (p.lowHzIndex, kNeutralProbe, 0.0), sr));
        }
        case kLowCut:
        {
            const auto n = lowNet (p.lowHzIndex, amountOf (p.lowBoost), amountOf (p.lowCut));
            if (n.cC > 0.0)
                return lowCutDesign (n, sr);
            return unityWithPoles (lowCutDesign (lowNet (p.lowHzIndex, 0.0, kNeutralProbe), sr));
        }
        case kHighBoost:
        {
            const double boost = amountOf (p.highBoost), hq = amountOf (p.highQ);
            if (boost >= kMinAmount)
                return peakDesign (highBoost (p.highHzIndex, boost, hq), sr);
            return unityWithPoles (peakDesign (highBoost (p.highHzIndex, kNeutralProbe, hq), sr));
        }
        case kHighCut:
        {
            const auto shelves = highShelves (p, sr);
            return multiplyFirstOrder (shelves.cut, shelves.tail);
        }
        case kLowMid:
        case kMidCut:
        default:
        {
            const int band = section == kLowMid ? 0 : (section == kMidCut ? 1 : 2);
            const double label = band == 0 ? pick (kPassiveEqLowMidHz, p.lowMidHzIndex)
                                           : (band == 1 ? pick (kPassiveEqMidHz, p.midHzIndex)
                                                        : pick (kPassiveEqHighMidHz, p.highMidHzIndex));
            const double amount = amountOf (band == 0 ? p.lowMidBoost : (band == 1 ? p.midCut : p.highMidBoost));
            if (amount >= kMinAmount)
                return peakDesign (midBand (band, label, amount), sr);
            return unityWithPoles (peakDesign (midBand (band, label, kNeutralProbe), sr));
        }
    }
}

bool isSectionActive (int section, const PassiveEqSetParams& p) noexcept
{
    switch (section)
    {
        case kLowBoost:  return amountOf (p.lowBoost) >= kMinAmount;
        case kLowCut:    return amountOf (p.lowCut) >= kMinAmount;
        case kHighBoost: return amountOf (p.highBoost) >= kMinAmount;
        case kHighCut:   return amountOf (p.highCut) >= kMinAmount;
        case kLowMid:    return amountOf (p.lowMidBoost) >= kMinAmount;
        case kMidCut:    return amountOf (p.midCut) >= kMinAmount;
        default:         return amountOf (p.highMidBoost) >= kMinAmount;
    }
}

double sectionHz (int section, const PassiveEqSetParams& p, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    double hz = 1000.0;
    switch (section)
    {
        case kLowBoost:
        {
            const auto n = lowNet (p.lowHzIndex, (isSectionActive (section, p) ? amountOf (p.lowBoost) : kNeutralProbe), 0.0);
            hz = std::sqrt (n.pB * (n.pB + n.cB));
            break;
        }
        case kLowCut:
        {
            const auto n = lowNet (p.lowHzIndex, 0.0, (isSectionActive (section, p) ? amountOf (p.lowCut) : kNeutralProbe));
            hz = std::sqrt (n.pC * n.zC);
            break;
        }
        case kHighBoost:
            hz = highBoost (p.highHzIndex, (isSectionActive (section, p) ? amountOf (p.highBoost) : kNeutralProbe), amountOf (p.highQ)).hz;
            break;
        case kHighCut:
        {
            const auto s = highCut (p.highCutHzIndex, (isSectionActive (section, p) ? amountOf (p.highCut) : kNeutralProbe));
            hz = s.pole / std::sqrt (s.gain);
            break;
        }
        case kLowMid:
            hz = midBand (0, pick (kPassiveEqLowMidHz, p.lowMidHzIndex), amountOf (p.lowMidBoost)).hz;
            break;
        case kMidCut:
            hz = midBand (1, pick (kPassiveEqMidHz, p.midHzIndex), amountOf (p.midCut)).hz;
            break;
        default:
            hz = midBand (2, pick (kPassiveEqHighMidHz, p.highMidHzIndex), amountOf (p.highMidBoost)).hz;
            break;
    }
    if (section == kLowBoost || section == kLowCut)
    {
        // Map the analog half-gain centre through the same prewarped bilinear transform as the audio.
        const double fw = std::min (kLowWarpHz, 0.25 * sr);
        hz = sr / kPi * std::atan (hz * std::tan (kPi * fw / sr) / fw);
    }
    return std::min (hz, 0.49 * sr);
}
} // namespace peq

//==============================================================================
struct PassiveEq::Impl
{
    struct SetDesign
    {
        BiquadCoeffs c[kNumSections];
        HighShelves shelves;
        unsigned cutVersion = 0, tailVersion = 0;
        SectionKey key[kNumSections];
        unsigned sectionVersion[kNumSections] {};
        unsigned version = 1;
    };

    struct ChannelEq
    {
        SmoothedBiquad s[kNumSections];
        // Section 4 is two independent first-order glides. Interpolating their product changes the audible
        // shelf's SVF coordinates when the other enters/leaves unity, or its inactive frequency moves.
        SmoothedBiquad highTail;
        unsigned cutVersion = 0, tailVersion = 0;
        int set = -1;
        unsigned version = 0;
        unsigned sectionVersion[kNumSections] {};
        bool snap = true;
    };

    // Seqlock for getMagnitudeDb: the audio thread writes (never waits), readers retry while a write is in progress.
    struct Published
    {
        std::atomic<std::uint32_t> seq { 0 };
        std::atomic<double> sampleRate { 0.0 };
        std::atomic<bool> valid { false };
        std::atomic<double> c[kNumChannelSets][kNumSections][5];
    };

    Impl()
    {
        for (auto& set : pub.c)
            for (auto& sec : set)
                for (auto& v : sec)
                    v.store (0.0, std::memory_order_relaxed);
    }

    //==========================================================================
    void prepare (double rate, int set)
    {
        sr = rate > 0.0 ? rate : 48000.0;
        paramSet = set;

        const int glide = std::max (1, (int) std::lround (kGlideMs * 0.001 * sr));
        for (auto& ch : eq)
        {
            for (auto& s : ch.s)
                s.setRampLength (glide);
            ch.highTail.setRampLength (glide);
        }

        os.prepare (1, Oversampler::Quality::MinimumPhase, kChunk, 2);
        wetDelaySamples = std::max (0, kPassiveEqLatency - os.getLatencySamples());
        for (int c = 0; c < 2; ++c)
        {
            dryDelay[c].prepare (kPassiveEqLatency);
            dryDelay[c].setDelay (kPassiveEqLatency);
            wetDelay[c].prepare (std::max (1, wetDelaySamples));
            wetDelay[c].setDelay (wetDelaySamples);
        }

        driveSmoother.prepare (2.0 * sr, kDriveSmoothMs);
        xfadeStep = 1.0 / std::max (1.0, kCrossfadeMs * 0.001 * sr);
        residualPole = std::exp (-2.0 * kPi * kResidualHpHz / (2.0 * sr));
        buildMakeup();

        // designs of the current parameters at the new rate
        for (auto& d : designs)
            for (auto& k : d.key)
                k = SectionKey {};
        redesign (0, current.sets[0], true);
        redesign (1, current.sets[1], true);
        if (haveParams)
            publish();
        reset();
    }

    void reset() noexcept
    {
        for (auto& ch : eq)
        {
            for (auto& s : ch.s)
                s.reset();
            ch.highTail.reset();
            ch.snap = true;
            ch.set = -1;
        }
        os.reset();
        for (int c = 0; c < 2; ++c)
        {
            dryDelay[c].reset();
            wetDelay[c].reset();
            residualIn[c] = residualOut[c] = 0.0;
        }
        driveSmoother.snap (current.drive);
        snapDrive = true;
        curveDrive = -1.0;   // the curve is set again on the next sample
        xfade = current.drive > 0.0f ? 1.0 : 0.0;
    }

    // Redesigns the changed sections of set `set` (all with force); publishes when anything changed.
    void redesign (int set, const PassiveEqSetParams& p, bool force) noexcept
    {
        auto& d = designs[set];
        bool changed = false;
        for (int k = 0; k < kNumSections; ++k)
        {
            const auto key = keyOf (k, p);
            if (! force && key == d.key[k])
                continue;
            if (k == kHighCut)
            {
                const auto& old = d.key[k];
                if (force || key.hzIndex != old.hzIndex || key.amount != old.amount)
                    ++d.cutVersion;
                if (force || key.hzIndex2 != old.hzIndex2 || key.amount2 != old.amount2 || key.extra != old.extra)
                    ++d.tailVersion;
                d.shelves = highShelves (p, sr);
                d.c[k] = multiplyFirstOrder (d.shelves.cut, d.shelves.tail);
            }
            else
                d.c[k] = peq::designSection (k, p, sr);
            d.key[k] = key;
            ++d.sectionVersion[k];
            changed = true;
        }
        if (changed)
        {
            ++d.version;
            publishPending = true;
        }
    }

    void setParams (ParamValues values) noexcept
    {
        PassiveEqParams p;
        readPassiveEqParams (values, p);
        if (paramSet == 1)
            p.sets[0] = p.sets[1];   // T/S twin: set B for both channels (Module.h rule 1)

        p.drive = std::isfinite (p.drive) ? std::clamp (p.drive, 0.0f, 10.0f) : 0.0f;
        current.drive = p.drive;
        driveSmoother.setTarget (p.drive);

        for (int set = 0; set < kNumChannelSets; ++set)
            if (! haveParams || ! sameSet (p.sets[set], current.sets[set]))
            {
                current.sets[set] = p.sets[set];
                redesign (set, p.sets[set], false);
            }

        if (! haveParams)
        {
            haveParams = true;
            publishPending = true;
        }
        if (snapDrive)
        {
            driveSmoother.snap (p.drive);
            xfade = p.drive > 0.0f ? 1.0 : 0.0;
            snapDrive = false;
        }

        if (publishPending)
            publish();
    }

    void publish() noexcept
    {
        publishPending = false;
        const auto s = pub.seq.load (std::memory_order_relaxed);
        pub.seq.store (s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        pub.sampleRate.store (sr, std::memory_order_relaxed);
        for (int set = 0; set < kNumChannelSets; ++set)
            for (int k = 0; k < kNumSections; ++k)
            {
                const auto& c = designs[set].c[k];
                const double v[5] { c.b0, c.b1, c.b2, c.a1, c.a2 };
                for (int i = 0; i < 5; ++i)
                    pub.c[set][k][i].store (v[i], std::memory_order_relaxed);
            }
        pub.valid.store (haveParams, std::memory_order_relaxed);
        pub.seq.store (s + 2u, std::memory_order_release);
    }

    double magnitudeDb (int set, double hz) const noexcept
    {
        set = set == 1 ? 1 : 0;
        double c[kNumSections][5] {};
        double rate = 0.0;
        bool valid = false;
        for (int attempt = 0; attempt < 10000; ++attempt)
        {
            const auto s1 = pub.seq.load (std::memory_order_acquire);
            if ((s1 & 1u) != 0u)
            {
                std::this_thread::yield();
                continue;
            }
            rate = pub.sampleRate.load (std::memory_order_relaxed);
            valid = pub.valid.load (std::memory_order_relaxed);
            for (int k = 0; k < kNumSections; ++k)
                for (int i = 0; i < 5; ++i)
                    c[k][i] = pub.c[set][k][i].load (std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_acquire);
            if (pub.seq.load (std::memory_order_relaxed) == s1)
                break;
            valid = false;
        }

        if (! valid || ! (rate > 0.0) || ! std::isfinite (hz))
            return 0.0;

        const double f = std::clamp (hz, 0.0, 0.4999 * rate);
        double total = 0.0;
        for (int k = 0; k < kNumSections; ++k)
        {
            const BiquadCoeffs b { c[k][0], c[k][1], c[k][2], c[k][3], c[k][4] };
            total += b.magnitudeDb (f, rate);
        }
        return std::isfinite (total) ? total : 0.0;
    }

    //==========================================================================
    void buildMakeup()
    {
        const double a = std::pow (10.0, kMakeupRefDbfs / 20.0);
        for (int i = 0; i < kMakeupPoints; ++i)
        {
            TubeCurve tube;
            tube.set ((double) i / (double) (kMakeupPoints - 1));
            if (! tube.active())
            {
                makeup[(size_t) i] = 1.0;
                continue;
            }
            double acc = 0.0;
            for (int n = 0; n < kMakeupCycle; ++n)
            {
                const double s = std::sin (2.0 * kPi * (double) n / (double) kMakeupCycle);
                acc += tube (a * s) * s;
            }
            const double fundamental = 2.0 * acc / (double) kMakeupCycle;
            makeup[(size_t) i] = fundamental > 0.0 ? a / fundamental : 1.0;
        }
    }

    double makeupFor (double driveNorm) const noexcept
    {
        const double x = std::clamp (driveNorm, 0.0, 1.0) * (double) (kMakeupPoints - 1);
        const int i = std::min ((int) x, kMakeupPoints - 2);
        const double f = x - (double) i;
        return makeup[(size_t) i] + (makeup[(size_t) i + 1] - makeup[(size_t) i]) * f;
    }

    // Brings channel c's filters to the design of the set it plays now.
    void applyTargets (int c, int set) noexcept
    {
        auto& ch = eq[c];
        const auto& d = designs[set];
        if (ch.set == set && ch.version == d.version && ! ch.snap)
            return;
        for (int k = 0; k < kNumSections; ++k)
        {
            // SmoothedBiquad restarts a running ramp even when its target is unchanged.
            // An edit to another section must not extend this section's settling time.
            if (! ch.snap && ch.set == set && ch.sectionVersion[k] == d.sectionVersion[k])
                continue;
            if (k == kHighCut)
            {
                if (ch.snap || ch.set != set || ch.cutVersion != d.cutVersion)
                    ch.s[k].setTarget (d.shelves.cut);
                if (ch.snap || ch.set != set || ch.tailVersion != d.tailVersion)
                    ch.highTail.setTarget (d.shelves.tail);
                ch.cutVersion = d.cutVersion;
                ch.tailVersion = d.tailVersion;
                if (ch.snap)
                    ch.highTail.snapToTarget();
            }
            else
                ch.s[k].setTarget (d.c[k]);
            if (ch.snap)
                ch.s[k].snapToTarget();
            ch.sectionVersion[k] = d.sectionVersion[k];
        }
        ch.set = set;
        ch.version = d.version;
        ch.snap = false;
    }

    void process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
    {
        const bool dual = ctx.channelMode == ChannelMode::MidSide || ctx.channelMode == ChannelMode::LeftRight;
        applyTargets (0, 0);
        applyTargets (1, dual ? 1 : 0);

        // 1 - 7: the passive sections, per channel
        bool finite = true;
        for (int c = 0; c < 2; ++c)
        {
            auto& ch = eq[c];
            float* x = channels[c];
            for (int i = 0; i < numSamples; ++i)
            {
                double v = std::isfinite (x[i]) && std::abs (x[i]) <= kMaxInput ? (double) x[i] : 0.0;
                for (int k = 0; k < kNumSections; ++k)
                {
                    v = ch.s[k].processSample (v);
                    if (k == kHighCut)
                        v = ch.highTail.processSample (v);
                }
                x[i] = (float) v;
            }
            if (numSamples > 0 && ! std::isfinite (x[numSamples - 1]))
                finite = false;
        }

        // 8: the tube make-up stage
        for (int start = 0; start < numSamples; start += kChunk)
            tubeChunk (channels, start, std::min (kChunk, numSamples - start));

        if (! finite)
        {
            for (auto& ch : eq)
            {
                for (auto& s : ch.s)
                    s.reset();
                ch.highTail.reset();
            }
            os.reset();
            residualIn[0] = residualIn[1] = residualOut[0] = residualOut[1] = 0.0;
        }
    }

    void tubeChunk (float* const* channels, int start, int len) noexcept
    {
        float* ch[2] { channels[0] + start, channels[1] + start };

        for (int c = 0; c < 2; ++c)
        {
            std::copy (ch[c], ch[c] + len, dry[c].data());
            dryDelay[c].process (dry[c].data(), len);
        }

        // the oversampled path runs always (warm), heard only while xfade > 0
        const float* in[2] { ch[0], ch[1] };
        float* const* up = os.upsample (in, len);
        const int n2 = 2 * len;
        for (int i = 0; i < n2; ++i)
        {
            const double driveNorm = (double) driveSmoother.next() * 0.1;
            if (driveNorm != curveDrive)
            {
                curveDrive = driveNorm;
                curve.set (driveNorm);
                curveGain = makeupFor (driveNorm);
            }
            const double gain = curveGain;
            for (int c = 0; c < 2; ++c)
            {
                const double x = (double) up[c][i];
                const double r = curve.active() ? curve (x) - x : 0.0;
                double h = residualPole * (residualOut[c] + r - residualIn[c]);
                residualIn[c] = r;
                flushTiny (h);
                residualOut[c] = h;
                up[c][i] = (float) (h * gain + x * (gain - 1.0));
            }
        }
        float* wetOut[2] { wet[0].data(), wet[1].data() };
        os.downsample (wetOut, len);
        if (wetDelaySamples > 0)
            for (int c = 0; c < 2; ++c)
                wetDelay[c].process (wet[c].data(), len);

        // The same exact delayed clean signal is present throughout; only the tube correction fades.
        const double target = current.drive > 0.0f ? 1.0 : 0.0;
        if (xfade == target)
        {
            for (int c = 0; c < 2; ++c)
                if (target == 0.0)
                    std::copy (dry[c].data(), dry[c].data() + len, ch[c]);
                else
                    for (int i = 0; i < len; ++i)
                        ch[c][i] = dry[c][(size_t) i] + wet[c][(size_t) i];
            return;
        }

        for (int i = 0; i < len; ++i)
        {
            xfade = target > xfade ? std::min (target, xfade + xfadeStep) : std::max (target, xfade - xfadeStep);
            const double w = 0.5 - 0.5 * std::cos (kPi * xfade);
            for (int c = 0; c < 2; ++c)
                ch[c][i] = (float) ((double) dry[c][(size_t) i] + w * (double) wet[c][(size_t) i]);
        }
    }

    //==========================================================================
    double sr = 48000.0;
    int paramSet = 0;
    bool haveParams = false, publishPending = false, snapDrive = true;
    PassiveEqParams current;
    SetDesign designs[kNumChannelSets];
    ChannelEq eq[2];
    Published pub;

    Oversampler os;
    DelayLine dryDelay[2], wetDelay[2];
    int wetDelaySamples = 0;
    std::array<std::array<float, kChunk>, 2> dry {}, wet {};
    LinearSmoother driveSmoother;
    TubeCurve curve;
    double curveDrive = 0.0, curveGain = 1.0;
    double residualPole = 0.0, residualIn[2] {}, residualOut[2] {};
    double xfade = 0.0, xfadeStep = 0.001;
    std::array<double, kMakeupPoints> makeup {};
};

//==============================================================================
PassiveEq::PassiveEq() : impl (std::make_unique<Impl>()) {}
PassiveEq::~PassiveEq() = default;

void PassiveEq::prepare (double sampleRate, int)
{
    impl->prepare (sampleRate, getParamSet());
}

void PassiveEq::reset() noexcept { impl->reset(); }

void PassiveEq::setParams (ParamValues values) noexcept { impl->setParams (values); }

void PassiveEq::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
#if PS_PEQ_HAS_SSE
    const unsigned int savedCsr = _mm_getcsr();
    _mm_setcsr (savedCsr | 0x8040u);   // FTZ | DAZ
#endif
    impl->process (channels, numSamples, ctx);
#if PS_PEQ_HAS_SSE
    _mm_setcsr (savedCsr);
#endif
}

int PassiveEq::getLatencySamples() const noexcept { return kPassiveEqLatency; }

double PassiveEq::getMagnitudeDb (int set, double hz) const noexcept { return impl->magnitudeDb (set, hz); }
} // namespace ps
