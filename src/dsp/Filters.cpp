#include "dsp/Filters.h"

#include <algorithm>
#include <cmath>

// Biquad designs (dsp-common).
//
// Analog prototypes (s normalised to the corner / centre frequency, A = 10^(gainDb / 40), G = A^2):
//   peak       (s^2 + s A/Q + 1) / (s^2 + s/(A Q) + 1)          gain G at the centre; boost and cut are mirror images
//   lowShelf   A (s^2 + s sqrt(A)/Q + A) / (A s^2 + s sqrt(A)/Q + 1)   G at DC, 1 at HF, half the dB gain at hz
//   highShelf  A (A s^2 + s sqrt(A)/Q + 1) / (s^2 + s sqrt(A)/Q + A)   1 at DC, G at HF, half the dB gain at hz
//   lowPass    1 / (s^2 + s/Q + 1)          highPass  s^2 / (s^2 + s/Q + 1)
//   lowShelf1  (s + sqrt(G)) / (s + 1/sqrt(G))                  highShelf1  G (s + 1/sqrt(G)) / (s + sqrt(G))
//   lowPass1   1 / (s + 1)                                      highPass1   s / (s + 1)
// (Q = 1/sqrt(2) gives the steepest shelf without overshoot.)
//
// Matched designs (M. Vicanek, "Matched Second Order Digital Filters", 2016): the poles are the exact images of the
// analog poles (impulse invariance, z = e^(sT)); the numerator is chosen so that the digital magnitude equals the
// analog magnitude at DC, at the centre frequency and at Nyquist. With
//   phi1 = sin^2(w/2), phi0 = 1 - phi1, phi2 = 4 phi0 phi1
// every second-order polynomial satisfies |c0 + c1 z^-1 + c2 z^-2|^2 = C0 phi0 + C1 phi1 + C2 phi2 with
//   C0 = (c0 + c1 + c2)^2, C1 = (c0 - c1 + c2)^2, C2 = -4 c0 c2,
// so matching three magnitudes is a linear problem in (B0, B1, B2), from which b0, b1, b2 follow in closed form
// (minimum-phase root). Low / high pass use Vicanek's dedicated forms (b2 = 0 for the low pass; a double zero at DC
// for the high pass). First-order matched designs use the exact pole and match DC plus Nyquist or the corner.
// Cuts are the exact inverses of the matching boosts (peak, low shelf) and high-shelf boosts the inverse of the cut,
// so the directly designed filter always has its poles below the corner. A single biquad cannot follow every analog
// shape up to Nyquist, so the closed-form candidates are scored on a frequency grid and, when the worst error is above
// 0.05 dB, refined by a small Levenberg-Marquardt fit (see refine()). Gain 0 dB returns the exact identity.
//
// All-passes are bilinear (prewarped): the matched method only matches magnitudes, which for an all-pass would give
// H = 1. Crossovers build their own bilinear Butterworth / Linkwitz-Riley sections (exact power complementarity).
namespace ps
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;

    double clampFreq (double sr, double hz) noexcept
    {
        return std::clamp (hz, 1.0, 0.49 * sr);
    }

    double clampQ (double q) noexcept
    {
        return std::clamp (q, 0.025, 100.0);
    }

    // Normalised analog biquad H(s) = (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0).
    struct Analog
    {
        double n0, n1, n2, d0, d1, d2;

        double magSq (double omega) const noexcept   // omega normalised to the design frequency
        {
            const double w2 = omega * omega;
            const double nr = n0 - n2 * w2, ni = n1 * omega;
            const double dr = d0 - d2 * w2, di = d1 * omega;
            return (nr * nr + ni * ni) / (dr * dr + di * di);
        }
    };

    struct Phis
    {
        double p0, p1, p2;
    };

    Phis phis (double w) noexcept
    {
        const double s = std::sin (0.5 * w);
        const double p1 = s * s;
        const double p0 = 1.0 - p1;
        return { p0, p1, 4.0 * p0 * p1 };
    }

    // Digital pole pair from an analog pole pair (natural frequency wp rad/sample, damping zeta).
    void matchedPoles (double wp, double zeta, double& a1, double& a2) noexcept
    {
        const double r = std::exp (-zeta * wp);
        a2 = r * r;

        if (zeta < 1.0)
        {
            const double wd = std::min (wp * std::sqrt (1.0 - zeta * zeta), 0.995 * kPi);
            a1 = -2.0 * r * std::cos (wd);
        }
        else
        {
            a1 = -2.0 * r * std::cosh (wp * std::sqrt (zeta * zeta - 1.0));
        }
    }

    // Numerator from three magnitude matches (DC, w0, Nyquist) for the poles a1, a2.
    BiquadCoeffs matchThreePoint (const Analog& h, double w0, double a1, double a2) noexcept
    {
        const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2);
        const double A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2);
        const double A2 = -4.0 * a2;

        const Phis p = phis (w0);
        const double denW0 = A0 * p.p0 + A1 * p.p1 + A2 * p.p2;

        const double B0 = A0 * h.magSq (0.0);
        const double B1 = A1 * h.magSq (kPi / w0);
        double B2 = (h.magSq (1.0) * denW0 - B0 * p.p0 - B1 * p.p1) / p.p2;

        const double sB0 = std::sqrt (B0), sB1 = std::sqrt (B1);
        const double W = 0.5 * (sB0 + sB1);
        B2 = std::max (B2, -W * W);

        BiquadCoeffs c;
        c.b0 = 0.5 * (W + std::sqrt (W * W + B2));
        c.b1 = 0.5 * (sB0 - sB1);
        c.b2 = c.b0 != 0.0 ? -B2 / (4.0 * c.b0) : 0.0;
        c.a1 = a1;
        c.a2 = a2;
        return c;
    }

    // Poles of d2 s^2 + d1 s + d0 (normalised to w0).
    void polesOf (const Analog& h, double w0, double& a1, double& a2) noexcept
    {
        const double wn = std::sqrt (h.d0 / h.d2);           // natural frequency (normalised)
        const double zeta = h.d1 / (2.0 * std::sqrt (h.d0 * h.d2));
        matchedPoles (wn * w0, zeta, a1, a2);
    }

    BiquadCoeffs matched (const Analog& h, double w0) noexcept
    {
        double a1 = 0.0, a2 = 0.0;
        polesOf (h, w0, a1, a2);
        return matchThreePoint (h, w0, a1, a2);
    }

    //--------------------------------------------------------------------------
    // Refinement: a single biquad cannot follow every analog shape exactly up to Nyquist (wide bells and shelves
    // centred high up lose up to ~1-2 dB with the closed forms). The candidates are scored on a grid (DC, 24
    // log-spaced points up to ~22 kHz, the centre and 12 points around it); if the worst error exceeds 0.05 dB, a
    // bounded Levenberg-Marquardt fit of log|H|^2 (10 iterations, stability enforced) refines the best candidate,
    // which is kept only if its worst error is lower. Cost: ~1.5 us closed form, ~20 us with the refinement.
    constexpr double kDeepLog = -9.2103403719761836;   // log of -40 dB in power

    struct GridPoint
    {
        std::complex<double> z1, z2;
        double target, weight;
    };

    struct Grid
    {
        static constexpr int kMax = 40;
        GridPoint p[kMax];
        int n = 0;

        void add (const Analog& h, double w, double w0, double weight) noexcept
        {
            if (n >= kMax)
                return;
            auto& g = p[n++];
            g.z1 = std::polar (1.0, -w);
            g.z2 = g.z1 * g.z1;
            g.target = std::log (std::max (h.magSq (w / w0), 1.0e-300));
            // the deep stop band of low / high passes (below -40 dB) is fitted only loosely and not scored
            g.weight = g.target < kDeepLog ? 0.01 * weight : weight;
        }

        void build (const Analog& h, double sr, double w0, double q, bool includeDc) noexcept
        {
            n = 0;
            if (includeDc)
                add (h, 0.0, w0, 5.0);
            const double whi = std::min (0.98 * kPi, 2.0 * kPi * 20000.0 / sr);
            const double wlo = std::min (2.0 * kPi * 10.0 / sr, 0.5 * whi);
            for (int i = 0; i < 24; ++i)
                add (h, wlo * std::pow (whi / wlo, i / 23.0), w0, 1.0);
            add (h, std::min (w0, whi), w0, 4.0);
            for (int k = 1; k <= 6; ++k)
                for (double sgn : { -1.0, 1.0 })
                {
                    const double w = w0 * (1.0 + sgn * k * 0.35 / std::max (q, 0.1));
                    if (w > wlo && w < whi)
                        add (h, w, w0, 1.0);
                }
        }
    };

    // Which coefficients the refinement may move: all five; a high pass b0 (1 - z^-1)^2 with free poles; a first-order
    // section (b0, b1, a1); a first-order high pass b0 (1 - z^-1) with a free pole.
    enum class FitMode { Full, HighPass2, FirstOrder, HighPass1 };

    inline double residual (const BiquadCoeffs& c, const GridPoint& g, double* J, FitMode mode) noexcept
    {
        const std::complex<double> N = c.b0 + c.b1 * g.z1 + c.b2 * g.z2;
        const std::complex<double> D = 1.0 + c.a1 * g.z1 + c.a2 * g.z2;
        const double n2 = std::norm (N), d2 = std::norm (D);
        if (J != nullptr)
        {
            const std::complex<double> cn = std::conj (N), cdn = std::conj (D);
            if (mode == FitMode::HighPass2 || mode == FitMode::HighPass1)
            {
                J[0] = 2.0 / c.b0;   // N = b0 (1 - z^-1)^k
                J[1] = J[2] = 0.0;
            }
            else
            {
                J[0] = 2.0 * std::real (cn) / n2;
                J[1] = 2.0 * std::real (cn * g.z1) / n2;
                J[2] = 2.0 * std::real (cn * g.z2) / n2;
            }
            J[3] = -2.0 * std::real (cdn * g.z1) / d2;
            J[4] = -2.0 * std::real (cdn * g.z2) / d2;
        }
        return std::log (std::max (n2, 1.0e-300) / std::max (d2, 1.0e-300)) - g.target;
    }

    bool isStable (const BiquadCoeffs& c) noexcept
    {
        return std::isfinite (c.b0) && std::isfinite (c.b1) && std::isfinite (c.b2) && std::isfinite (c.a1)
            && std::isfinite (c.a2) && std::abs (c.a2) < 0.99999 && std::abs (c.a1) < 1.0 + c.a2 - 1.0e-9;
    }

    // Largest pole radius of 1 + a1 z^-1 + a2 z^-2.
    double maxPoleRadius (const BiquadCoeffs& c) noexcept
    {
        const double disc = c.a1 * c.a1 - 4.0 * c.a2;
        if (disc < 0.0)
            return std::sqrt (std::max (0.0, c.a2));
        const double s = std::sqrt (disc);
        return 0.5 * std::max (std::abs (-c.a1 + s), std::abs (-c.a1 - s));
    }

    double worstDb (const BiquadCoeffs& c, const Grid& g) noexcept
    {
        if (! isStable (c))
            return 1.0e9;
        double worst = 0.0;
        for (int i = 0; i < g.n; ++i)
            if (g.p[i].target >= kDeepLog)
                worst = std::max (worst, std::abs (residual (c, g.p[i], nullptr, FitMode::Full)));
        return worst * (10.0 / std::log (10.0));   // log |H|^2 -> dB
    }

    double gridCost (const BiquadCoeffs& c, const Grid& g) noexcept
    {
        double s = 0.0;
        for (int i = 0; i < g.n; ++i)
        {
            const double r = residual (c, g.p[i], nullptr, FitMode::Full);
            s += g.p[i].weight * r * r;
        }
        return s;
    }

    bool solveLinear (double (&m)[5][6], double* x, int n) noexcept
    {
        for (int col = 0; col < n; ++col)
        {
            int piv = col;
            for (int r = col + 1; r < n; ++r)
                if (std::abs (m[r][col]) > std::abs (m[piv][col]))
                    piv = r;
            if (std::abs (m[piv][col]) < 1.0e-300)
                return false;
            for (int k = 0; k <= 5; ++k)
                std::swap (m[col][k], m[piv][k]);
            for (int r = 0; r < n; ++r)
                if (r != col)
                {
                    const double f = m[r][col] / m[col][col];
                    for (int k = col; k <= 5; ++k)
                        m[r][k] -= f * m[col][k];
                }
        }
        for (int i = 0; i < n; ++i)
            x[i] = m[i][5] / m[i][i];
        return true;
    }

    BiquadCoeffs refine (BiquadCoeffs c, const Grid& g, FitMode mode) noexcept
    {
        // parameters: (b0, b1, b2, a1, a2), the subset given by the mode
        static constexpr int kAll[5] { 0, 1, 2, 3, 4 };
        static constexpr int kHp2[3] { 0, 3, 4 };
        static constexpr int kFo[3] { 0, 1, 3 };
        static constexpr int kHp1[2] { 0, 3 };
        const int* use = mode == FitMode::Full ? kAll : (mode == FitMode::HighPass2 ? kHp2
                                                       : (mode == FitMode::FirstOrder ? kFo : kHp1));
        const int np = mode == FitMode::Full ? 5 : (mode == FitMode::HighPass1 ? 2 : 3);

        double lambda = 1.0e-2;
        double cost = gridCost (c, g);

        // The fit may move the poles, but never much closer to the unit circle than the closed-form (impulse-
        // invariant) poles: at most half their distance, i.e. at most twice the ringing time. Without this bound a
        // narrow bell or resonant pass near Nyquist could be "fitted" with a pole within 1e-6 of |z| = 1 (rings for
        // seconds).
        const double radiusLimit = 1.0 - 0.5 * (1.0 - std::min (maxPoleRadius (c), 1.0));

        for (int it = 0; it < 16; ++it)
        {
            double m[5][6] {};
            for (int i = 0; i < g.n; ++i)
            {
                double J[5];
                const double r = residual (c, g.p[i], J, mode);
                const double w = g.p[i].weight;
                for (int a = 0; a < np; ++a)
                {
                    m[a][5] -= w * J[use[a]] * r;
                    for (int b = 0; b < np; ++b)
                        m[a][b] += w * J[use[a]] * J[use[b]];
                }
            }
            for (int a = 0; a < np; ++a)
                m[a][a] *= 1.0 + lambda;

            double d[5] {};
            if (! solveLinear (m, d, np))
                break;

            double v[5] { c.b0, c.b1, c.b2, c.a1, c.a2 };
            for (int a = 0; a < np; ++a)
                v[use[a]] += d[a];
            BiquadCoeffs t { v[0], v[1], v[2], v[3], v[4] };
            if (mode == FitMode::HighPass2)
            {
                t.b1 = -2.0 * t.b0;
                t.b2 = t.b0;
            }
            else if (mode == FitMode::HighPass1)
            {
                t.b1 = -t.b0;
            }

            const double tc = (isStable (t) && maxPoleRadius (t) <= radiusLimit) ? gridCost (t, g) : 1.0e300;
            if (tc < cost)
            {
                c = t;
                cost = tc;
                lambda = std::max (1.0e-9, lambda * 0.3);
            }
            else
            {
                lambda *= 10.0;
            }
        }
        return c;
    }

    BiquadCoeffs invert (const BiquadCoeffs& c) noexcept
    {
        // 1 / H: swap numerator and denominator (the numerators are minimum phase, so this stays stable)
        const double g = 1.0 / c.b0;
        return { g, c.a1 * g, c.a2 * g, c.b1 * g, c.b2 * g };
    }

    // Peak with Vicanek's own numerator conditions: DC, value and zero slope at the centre.
    BiquadCoeffs slopeMatchedPeak (double w0, double q, double A) noexcept
    {
        const double G = A * A;
        BiquadCoeffs c;
        matchedPoles (w0, 0.5 / (A * q), c.a1, c.a2);
        const double A0 = (1.0 + c.a1 + c.a2) * (1.0 + c.a1 + c.a2);
        const double A1 = (1.0 - c.a1 + c.a2) * (1.0 - c.a1 + c.a2);
        const double A2 = -4.0 * c.a2;
        const Phis p = phis (w0);
        const double R1 = (A0 * p.p0 + A1 * p.p1 + A2 * p.p2) * G * G;
        const double R2 = (-A0 + A1 + 4.0 * (p.p0 - p.p1) * A2) * G * G;
        const double B0 = A0;
        const double B2 = (R1 - R2 * p.p1 - B0) / (4.0 * p.p1 * p.p1);
        const double B1 = R2 + B0 + 4.0 * (p.p1 - p.p0) * B2;

        const double sB0 = std::sqrt (std::max (0.0, B0)), sB1 = std::sqrt (std::max (0.0, B1));
        const double W = 0.5 * (sB0 + sB1);
        const double B2c = std::max (B2, -W * W);
        c.b0 = 0.5 * (W + std::sqrt (W * W + B2c));
        c.b1 = 0.5 * (sB0 - sB1);
        c.b2 = c.b0 != 0.0 ? -B2c / (4.0 * c.b0) : 0.0;
        return c;
    }

    BiquadCoeffs pickAndRefine (const BiquadCoeffs* candidates, int num, const Analog& target, double sr, double w0,
                                double q, FitMode mode) noexcept
    {
        const bool hasDc = mode == FitMode::Full || mode == FitMode::FirstOrder;
        Grid g;
        g.build (target, sr, w0, q, hasDc);

        BiquadCoeffs best = candidates[0];
        double bestErr = worstDb (best, g);
        for (int i = 1; i < num; ++i)
        {
            const double e = worstDb (candidates[i], g);
            if (e < bestErr)
            {
                bestErr = e;
                best = candidates[i];
            }
        }

        if (bestErr > 0.05)
        {
            for (int i = 0; i < num; ++i)
            {
            auto r = refine (candidates[i], g, mode);
            if (hasDc)
            {
                // keep the DC gain exact
                const double dc = std::abs ((r.b0 + r.b1 + r.b2) / (1.0 + r.a1 + r.a2));
                const double want = std::sqrt (target.magSq (0.0));
                if (dc > 0.0)
                {
                    const double k = want / dc;
                    r.b0 *= k;
                    r.b1 *= k;
                    r.b2 *= k;
                }
            }
            const double e = worstDb (r, g);
            if (e < bestErr)
            {
                best = r;
                bestErr = e;
            }
            }
        }
        return best;
    }

    // First order: H(s) = (n1 s + n0) / (s + p) normalised to w0, exact pole. The numerator matches DC and either
    // Nyquist or the corner, whichever follows the analog curve better; refined like the biquads (b2 = a2 = 0 kept).
    BiquadCoeffs matchedFirstOrder (double n0, double n1, double p, double w0, double sr) noexcept
    {
        BiquadCoeffs c;
        c.a1 = -std::exp (-p * w0);
        c.a2 = 0.0;
        c.b2 = 0.0;

        auto magAt = [&] (double omega)
        {
            return std::sqrt ((n0 * n0 + n1 * n1 * omega * omega) / (p * p + omega * omega));
        };

        const double sumB = (1.0 + c.a1) * magAt (0.0);        // b0 + b1 (DC)

        // candidate 1: Nyquist
        BiquadCoeffs c1 = c;
        const double difNyq = (1.0 - c.a1) * magAt (kPi / w0);
        c1.b0 = 0.5 * (sumB + difNyq);
        c1.b1 = 0.5 * (sumB - difNyq);

        // candidate 2: corner, |b0 + b1 z^-1|^2 = (b0 + b1)^2 phi0 + (b0 - b1)^2 phi1
        BiquadCoeffs c2 = c;
        const Phis ph = phis (w0);
        const double den = (1.0 + c.a1) * (1.0 + c.a1) * ph.p0 + (1.0 - c.a1) * (1.0 - c.a1) * ph.p1;
        const double m = magAt (1.0);
        const double dif2 = (m * m * den - sumB * sumB * ph.p0) / ph.p1;
        const double difW0 = std::sqrt (std::max (0.0, dif2));
        c2.b0 = 0.5 * (sumB + difW0);
        c2.b1 = 0.5 * (sumB - difW0);

        const BiquadCoeffs cands[2] { c1, c2 };
        const Analog target { n0, n1, 0.0, p, 1.0, 0.0 };   // (n1 s + n0) / (s + p)
        return pickAndRefine (cands, 2, target, sr, w0, 1.0, n0 == 0.0 ? FitMode::HighPass1 : FitMode::FirstOrder);
    }
}

std::complex<double> BiquadCoeffs::response (double freqHz, double sampleRate) const noexcept
{
    const double w = 2.0 * kPi * freqHz / sampleRate;
    const std::complex<double> z1 = std::polar (1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
}

double BiquadCoeffs::magnitudeDb (double freqHz, double sampleRate) const noexcept
{
    const double m = std::abs (response (freqHz, sampleRate));
    return m > 1.0e-10 ? 20.0 * std::log10 (m) : -200.0;
}

namespace design
{
    BiquadCoeffs peak (double sr, double hz, double q, double gainDb) noexcept
    {
        if (std::abs (gainDb) < 1.0e-6)
            return {};
        const double w0 = 2.0 * kPi * clampFreq (sr, hz) / sr;
        q = clampQ (q);

        // boost designed directly (its poles carry the resonance), cut = exact inverse of the boost
        const double Ab = std::pow (10.0, std::abs (gainDb) / 40.0);
        BiquadCoeffs cands[2] { matched ({ 1.0, Ab / q, 1.0, 1.0, 1.0 / (Ab * q), 1.0 }, w0),
                                slopeMatchedPeak (w0, q, Ab) };
        if (gainDb < 0.0)
            for (auto& c : cands)
                c = invert (c);

        const double A = std::pow (10.0, gainDb / 40.0);
        return pickAndRefine (cands, 2, { 1.0, A / q, 1.0, 1.0, 1.0 / (A * q), 1.0 }, sr, w0, q, FitMode::Full);
    }

    // Shelves: the variant whose poles sit below the corner is designed directly (low shelf boost, high shelf cut);
    // the other one is its exact inverse, so no pole ever has to be placed above Nyquist.
    BiquadCoeffs lowShelf (double sr, double hz, double q, double gainDb) noexcept
    {
        if (std::abs (gainDb) < 1.0e-6)
            return {};
        const double w0 = 2.0 * kPi * clampFreq (sr, hz) / sr;
        q = clampQ (q);

        const double Ab = std::pow (10.0, std::abs (gainDb) / 40.0), sAb = std::sqrt (Ab);
        BiquadCoeffs c = matched ({ Ab * Ab, Ab * sAb / q, Ab, 1.0, sAb / q, Ab }, w0);
        if (gainDb < 0.0)
            c = invert (c);

        const double A = std::pow (10.0, gainDb / 40.0), sA = std::sqrt (A);
        return pickAndRefine (&c, 1, { A * A, A * sA / q, A, 1.0, sA / q, A }, sr, w0, q, FitMode::Full);
    }

    BiquadCoeffs highShelf (double sr, double hz, double q, double gainDb) noexcept
    {
        if (std::abs (gainDb) < 1.0e-6)
            return {};
        const double w0 = 2.0 * kPi * clampFreq (sr, hz) / sr;
        q = clampQ (q);

        const double Ac = std::pow (10.0, -std::abs (gainDb) / 40.0), sAc = std::sqrt (Ac);
        BiquadCoeffs c = matched ({ Ac, Ac * sAc / q, Ac * Ac, Ac, sAc / q, 1.0 }, w0);
        if (gainDb > 0.0)
            c = invert (c);

        const double A = std::pow (10.0, gainDb / 40.0), sA = std::sqrt (A);
        return pickAndRefine (&c, 1, { A, A * sA / q, A * A, A, sA / q, 1.0 }, sr, w0, q, FitMode::Full);
    }

    BiquadCoeffs highPass (double sr, double hz, double q) noexcept
    {
        const double w0 = 2.0 * kPi * clampFreq (sr, hz) / sr;
        q = clampQ (q);
        BiquadCoeffs c;
        matchedPoles (w0, 0.5 / q, c.a1, c.a2);

        const double A0 = (1.0 + c.a1 + c.a2) * (1.0 + c.a1 + c.a2);
        const double A1 = (1.0 - c.a1 + c.a2) * (1.0 - c.a1 + c.a2);
        const double A2 = -4.0 * c.a2;
        const Phis p = phis (w0);

        c.b0 = q * std::sqrt (A0 * p.p0 + A1 * p.p1 + A2 * p.p2) / (4.0 * p.p1);
        c.b1 = -2.0 * c.b0;
        c.b2 = c.b0;
        return pickAndRefine (&c, 1, { 0.0, 0.0, 1.0, 1.0, 1.0 / q, 1.0 }, sr, w0, q, FitMode::HighPass2);
    }

    BiquadCoeffs lowPass (double sr, double hz, double q) noexcept
    {
        const double w0 = 2.0 * kPi * clampFreq (sr, hz) / sr;
        q = clampQ (q);
        BiquadCoeffs c;
        matchedPoles (w0, 0.5 / q, c.a1, c.a2);

        const double A0 = (1.0 + c.a1 + c.a2) * (1.0 + c.a1 + c.a2);
        const double A1 = (1.0 - c.a1 + c.a2) * (1.0 - c.a1 + c.a2);
        const double A2 = -4.0 * c.a2;
        const Phis p = phis (w0);

        const double R1 = (A0 * p.p0 + A1 * p.p1 + A2 * p.p2) * q * q;
        const double B0 = A0;
        const double B1 = std::max (0.0, (R1 - B0 * p.p0) / p.p1);

        c.b0 = 0.5 * (std::sqrt (B0) + std::sqrt (B1));
        c.b1 = std::sqrt (B0) - c.b0;
        c.b2 = 0.0;
        return pickAndRefine (&c, 1, { 1.0, 0.0, 0.0, 1.0, 1.0 / q, 1.0 }, sr, w0, q, FitMode::Full);
    }

    BiquadCoeffs highPass1 (double sr, double hz) noexcept
    {
        const double w0 = 2.0 * kPi * clampFreq (sr, hz) / sr;
        return matchedFirstOrder (0.0, 1.0, 1.0, w0, sr);
    }

    BiquadCoeffs lowPass1 (double sr, double hz) noexcept
    {
        const double w0 = 2.0 * kPi * clampFreq (sr, hz) / sr;
        return matchedFirstOrder (1.0, 0.0, 1.0, w0, sr);
    }

    BiquadCoeffs allPass (double sr, double hz, double q) noexcept
    {
        const double K = std::tan (kPi * clampFreq (sr, hz) / sr);
        q = clampQ (q);
        const double norm = 1.0 / (1.0 + K / q + K * K);

        BiquadCoeffs c;
        c.a1 = 2.0 * (K * K - 1.0) * norm;
        c.a2 = (1.0 - K / q + K * K) * norm;
        c.b0 = c.a2;
        c.b1 = c.a1;
        c.b2 = 1.0;
        return c;
    }

    BiquadCoeffs allPass1 (double sr, double hz) noexcept
    {
        const double K = std::tan (kPi * clampFreq (sr, hz) / sr);
        BiquadCoeffs c;
        c.a1 = (K - 1.0) / (K + 1.0);
        c.b0 = c.a1;
        c.b1 = 1.0;
        return c;
    }

    BiquadCoeffs lowShelf1 (double sr, double hz, double gainDb) noexcept
    {
        const double w0 = 2.0 * kPi * clampFreq (sr, hz) / sr;
        const double sG = std::pow (10.0, gainDb / 40.0);   // sqrt (G)
        return matchedFirstOrder (sG, 1.0, 1.0 / sG, w0, sr);
    }

    BiquadCoeffs highShelf1 (double sr, double hz, double gainDb) noexcept
    {
        const double w0 = 2.0 * kPi * clampFreq (sr, hz) / sr;
        const double sG = std::pow (10.0, gainDb / 40.0);
        return matchedFirstOrder (sG, sG * sG, sG, w0, sr);    // G (s + 1/sqrt(G)) = G s + sqrt(G)
    }

    // Baxandall high shelf (measured: the reference's renders at 1 and 5 kHz, gains +-3 / 6 / 12 dB, fit rms <= 0.01 dB):
    // an unwarped analog first-order shelf whose far gain is exactly the set gain; its lower corner (the zero of a
    // boost, the pole of a cut) sits at r f with r = 0.7659 + 0.00456 |g| + 0.000493 g^2 (0.784 / 0.811 / 0.892 at
    // 3 / 6 / 12 dB), the other corner at G times that; a cut is the exact inverse of the boost.
    BiquadCoeffs baxandallHigh (double sr, double hz, double gainDb) noexcept
    {
        const double g = std::abs (gainDb);
        const double r = 0.7659 + 0.00456 * g + 0.000493 * g * g;
        const double G = std::pow (10.0, gainDb / 20.0);
        const double lower = r * hz;
        const double w0 = 2.0 * kPi * clampFreq (sr, gainDb >= 0.0 ? lower * G : lower) / sr;
        return matchedFirstOrder (1.0, G, 1.0, w0, sr);   // (G s + 1) / (s + 1) at w0: DC 1, far gain G
    }

    // Baxandall low shelf (measured: the reference's renders at 500 Hz, -3 / -6 / +6 dB, and at 100 Hz, -6 dB):
    // H = (s + wz) / (s + wp) with the depth at DC D (g) and the pole fp (g) x f / 500 Hz taken from the fitted
    // anchors (g, D, fp) = (-6, -16.66, 77.79), (-3, -6.27, 53.0), (0, 0, 65.1), (+6, +12.11, 98.86), linear in g between
    // them (fp in log) and extrapolated beyond. The reference's curve has a little second-order overshoot (a -0.5 dB
    // dip at 4 f for +6) that this first-order form leaves out (max error 0.75 dB); its -12 dB setting (only reachable by
    // typing it in) is not a shelf at all any more (a -15.6 dB dip near f / 9 that returns toward 0 dB below).
    BiquadCoeffs baxandallLow (double sr, double hz, double gainDb) noexcept
    {
        static constexpr double gs[4] { -6.0, -3.0, 0.0, 6.0 };
        static constexpr double ds[4] { -16.66, -6.27, 0.0, 12.11 };
        static constexpr double fps[4] { 77.79, 53.0, 65.1, 98.86 };
        const int i = gainDb < -3.0 ? 0 : (gainDb < 0.0 ? 1 : 2);
        const double t = (gainDb - gs[i]) / (gs[i + 1] - gs[i]);
        const double depthDb = ds[i] + t * (ds[i + 1] - ds[i]);
        const double fp = std::exp (std::log (fps[i]) + t * (std::log (fps[i + 1]) - std::log (fps[i]))) * hz / 500.0;
        const double w0 = 2.0 * kPi * clampFreq (sr, fp) / sr;
        return matchedFirstOrder (std::pow (10.0, depthDb / 20.0), 1.0, 1.0, w0, sr);   // (s + D) / (s + 1) at the pole
    }

    double butterworthQ (int order, int sectionIndex) noexcept
    {
        order = std::max (order, 1);
        if (order < 2)
            return 0.7071067811865476;

        const int numBiquads = order / 2;
        const int k = std::clamp (sectionIndex, 0, numBiquads - 1);

        // Pole angles from the negative real axis: even N: (2k + 1) pi / (2N); odd N: (k + 1) pi / N.
        const double theta = (order % 2 == 0) ? (2.0 * k + 1.0) * kPi / (2.0 * order)
                                              : (k + 1.0) * kPi / order;
        return 1.0 / (2.0 * std::cos (theta));
    }
}

//==============================================================================
void Biquad::process (float* data, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
        data[i] = (float) processSample ((double) data[i]);

    // No denormals: a state this small can never reach the denormal range within one block.
    if (std::abs (z1) < 1.0e-30) z1 = 0.0;
    if (std::abs (z2) < 1.0e-30) z2 = 0.0;
}

//==============================================================================
// SmoothedBiquad: trapezoidal state-variable filter (A. Simper's form), states ic1, ic2:
//     v3 = x - ic2,  v1 = a1 ic1 + a2 v3,  v2 = ic2 + a2 ic1 + a3 v3,  ic1' = 2 v1 - ic1,  ic2' = 2 v2 - ic2
//     a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2;   y = m0 x + m1 v1 + m2 v2
// With no input the states map through M = [[1 - g^2 - g k, -2 g], [2 g, 1 - g^2 + g k]] / (1 + g^2 + g k), whose
// singular values are 1 and |1 + g^2 - g k| / (1 + g^2 + g k): ||M|| = 1 for every g > 0, k > 0, so the state norm
// can never grow, whatever sequence of (g, k) the glide produces (unconditional stability under modulation).
void SmoothedBiquad::updateGains() noexcept
{
    const double g = current.g;
    a1 = 1.0 / (1.0 + g * (g + current.k));
    a2 = g * a1;
    a3 = g * a2;
}

void SmoothedBiquad::setTarget (const BiquadCoeffs& c) noexcept
{
    if (! (std::isfinite (c.b0) && std::isfinite (c.b1) && std::isfinite (c.b2) && std::isfinite (c.a1)
           && std::isfinite (c.a2)))
        return;

    // Denominator -> (g, k): the bilinear SVF has 1 + a1 + a2 = 4 g^2 / D, 1 - a1 + a2 = 4 / D, 1 - a2 = 2 g k / D
    // (D = 1 + g k + g^2). A stable denominator has all three > 0.
    const double pDc = 1.0 + c.a1 + c.a2, pNy = 1.0 - c.a1 + c.a2, pMid = 1.0 - c.a2;
    if (! (pDc > 1.0e-30 && pNy > 1.0e-30 && pMid > 1.0e-30))
        return;

    Svf t;
    t.g = std::sqrt (pDc / pNy);
    t.k = 2.0 * pMid / (t.g * pNy);
    const double D = 4.0 / pNy;
    // Numerator: B = m0 A + (m1 g / D) (1 - z^-2) + (m2 g^2 / D) (1 + z^-1)^2
    t.m0 = (c.b0 - c.b1 + c.b2) / pNy;               // response at Nyquist
    const double q = 0.5 * (c.b1 - t.m0 * c.a1);      // m2 g^2 / D
    const double p = c.b0 - t.m0 - q;                 // m1 g / D
    t.m1 = p * D / t.g;
    t.m2 = q * D / (t.g * t.g);
    if (! (std::isfinite (t.g) && std::isfinite (t.k) && std::isfinite (t.m1) && std::isfinite (t.m2)))
        return;

    target = t;
    if (remaining == 0 && t.g == current.g && t.k == current.k && t.m0 == current.m0 && t.m1 == current.m1
        && t.m2 == current.m2)
        return;   // unchanged design (callers may re-send it every block): no ramp, no per-sample work

    const double inv = 1.0 / (double) rampLength;
    step.g = (target.g - current.g) * inv;
    step.k = (target.k - current.k) * inv;
    step.m0 = (target.m0 - current.m0) * inv;
    step.m1 = (target.m1 - current.m1) * inv;
    step.m2 = (target.m2 - current.m2) * inv;
    remaining = rampLength;
}

void SmoothedBiquad::snapToTarget() noexcept
{
    current = target;
    remaining = 0;
    updateGains();
}

void SmoothedBiquad::reset() noexcept
{
    ic1 = ic2 = 0.0;
}

double SmoothedBiquad::processSample (double x) noexcept
{
    if (remaining > 0)
    {
        if (--remaining == 0)
        {
            current = target;
        }
        else
        {
            // convex steps: g > 0 and k > 0 hold all the way
            current.g += step.g;
            current.k += step.k;
            current.m0 += step.m0;
            current.m1 += step.m1;
            current.m2 += step.m2;
        }
        updateGains();
    }

    const double v3 = x - ic2;
    const double v1 = a1 * ic1 + a2 * v3;
    const double v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;

    if (std::abs (ic1) < 1.0e-30) ic1 = 0.0;
    if (std::abs (ic2) < 1.0e-30) ic2 = 0.0;
    return current.m0 * x + current.m1 * v1 + current.m2 * v2;
}
} // namespace ps
