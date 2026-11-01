#include "dsp/EqDesign.h"

#include <algorithm>
#include <cmath>

// Equalizer band designs (dsp-eq). The behaviour of every shape is documented at the top of Equalizer.cpp.
//
// Brickwall (Analog mode): 8th-order elliptic low-pass prototype (passband ripple 0.1 dB, stopband 96 dB), computed at
// static initialisation with Jacobi elliptic functions (Landen transformations, after S. J. Orfanidis, "Lecture Notes
// on Elliptic Filter Design"), transformed to HP / LP at the corner and discretised with the prewarped bilinear
// transform (the matched method only matches magnitudes and cannot place the stopband zeros on the unit circle). With
// these numbers the stopband (-96 dB) starts 0.74 octave beyond the corner: about 130 dB/oct on average, -28 dB a
// quarter octave from the corner. The corner is the passband edge (-0.1 dB).
namespace ps::eqdesign
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kSqrtHalf = 0.70710678118654752;
    using cd = std::complex<double>;

    //==========================================================================
    // Jacobi elliptic functions with normalised arguments (u in units of the quarter period K), via the descending
    // Landen sequence of the modulus.
    struct Landen
    {
        static constexpr int kSteps = 12;
        double v[kSteps + 1] {};   // v[0] = k, v[n] = k_n

        explicit Landen (double k) noexcept
        {
            v[0] = k;
            for (int n = 1; n <= kSteps; ++n)
            {
                const double kp = v[n - 1];
                const double t = kp / (1.0 + std::sqrt (1.0 - kp * kp));
                v[n] = t * t;
            }
        }
    };

    cd cde (cd u, double k) noexcept
    {
        const Landen l (k);
        cd w = std::cos (u * (kPi * 0.5));
        for (int n = Landen::kSteps; n >= 1; --n)
            w = (1.0 + l.v[n]) * w / (1.0 + l.v[n] * w * w);
        return w;
    }

    cd sne (cd u, double k) noexcept { return cde (u - 1.0, k); }

    cd acde (cd w, double k) noexcept
    {
        const Landen l (k);
        for (int n = 1; n <= Landen::kSteps; ++n)
            w = w / (1.0 + std::sqrt (1.0 - w * w * l.v[n - 1] * l.v[n - 1])) * 2.0 / (1.0 + l.v[n]);
        return std::acos (w) * (2.0 / kPi);
    }

    cd asne (cd w, double k) noexcept { return 1.0 - acde (w, k); }

    // Even-order elliptic low-pass prototype, passband edge at w = 1: per biquad, |zero|^2, |pole|^2 and Re(pole).
    struct EllipticPrototype
    {
        static constexpr int kOrder = 8;
        static constexpr int kBiquads = kOrder / 2;
        double zz[kBiquads] {}, pp[kBiquads] {}, pr[kBiquads] {};
        double dcGain = 1.0;

        EllipticPrototype (double rippleDb, double stopDb) noexcept
        {
            const double ep = std::sqrt (std::pow (10.0, rippleDb / 10.0) - 1.0);
            const double es = std::sqrt (std::pow (10.0, stopDb / 10.0) - 1.0);
            const double k1 = ep / es, k1p = std::sqrt (1.0 - k1 * k1);

            // degree equation solved for the selectivity k
            double kp = std::pow (k1p, (double) kOrder);
            for (int i = 1; i <= kBiquads; ++i)
            {
                const double ui = (2.0 * i - 1.0) / kOrder;
                kp *= std::pow (sne (ui, k1p).real(), 4.0);
            }
            const double k = std::sqrt (1.0 - kp * kp);

            const cd v0 = cd (0.0, -1.0) * asne (cd (0.0, 1.0 / ep), k1) / (double) kOrder;
            for (int i = 1; i <= kBiquads; ++i)
            {
                const double ui = (2.0 * i - 1.0) / kOrder;
                const double zeta = cde (ui, k).real();
                const double zi = 1.0 / (k * zeta);   // zero at j * zi
                const cd p = cd (0.0, 1.0) * cde (cd (ui, 0.0) - cd (0.0, 1.0) * v0, k);
                zz[i - 1] = zi * zi;
                pp[i - 1] = std::norm (p);
                pr[i - 1] = p.real();
            }
            dcGain = std::pow (10.0, -rippleDb / 20.0);   // even order: DC sits at the bottom of the ripple
        }
    };

    const EllipticPrototype kElliptic (0.1, 96.0);

    // Bilinear transform of (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0), s = c (1 - z^-1) / (1 + z^-1).
    BiquadCoeffs bilinear (double n2, double n1, double n0, double d2, double d1, double d0, double c) noexcept
    {
        const double c2 = c * c;
        const double a0 = d2 * c2 + d1 * c + d0;
        BiquadCoeffs r;
        r.b0 = (n2 * c2 + n1 * c + n0) / a0;
        r.b1 = (2.0 * n0 - 2.0 * n2 * c2) / a0;
        r.b2 = (n2 * c2 - n1 * c + n0) / a0;
        r.a1 = (2.0 * d0 - 2.0 * d2 * c2) / a0;
        r.a2 = (d2 * c2 - d1 * c + d0) / a0;
        return r;
    }

    void addSection (BandDesign& d, const BiquadCoeffs& c) noexcept
    {
        if (d.numSections < kMaxSections)
            d.s[d.numSections++] = c;
    }

    void brickwall (BandDesign& d, bool highPass, double sr, double hz) noexcept
    {
        const double f = std::clamp (hz, 1.0, 0.49 * sr);
        const double c = 1.0 / std::tan (kPi * f / sr);   // s_normalised = c (1 - z^-1) / (1 + z^-1)
        const auto& e = kElliptic;
        for (int i = 0; i < EllipticPrototype::kBiquads; ++i)
        {
            BiquadCoeffs s;
            if (highPass)   // s -> 1 / s
                s = bilinear (1.0, 0.0, 1.0 / e.zz[i], 1.0, -2.0 * e.pr[i] / e.pp[i], 1.0 / e.pp[i], c);
            else
                s = bilinear (1.0 / e.zz[i], 0.0, 1.0, 1.0 / e.pp[i], -2.0 * e.pr[i] / e.pp[i], 1.0, c);
            if (i == 0)
            {
                s.b0 *= e.dcGain;
                s.b1 *= e.dcGain;
                s.b2 *= e.dcGain;
            }
            addSection (d, s);
        }
    }

    void butterworthPass (BandDesign& d, bool highPass, double sr, double hz, int order, double lastQScale,
                          bool resonant, double q) noexcept
    {
        if (order % 2 == 1)
            addSection (d, highPass ? design::highPass1 (sr, hz) : design::lowPass1 (sr, hz));

        const int biquads = order / 2;
        for (int k = 0; k < biquads; ++k)
        {
            double sq = design::butterworthQ (order, k);
            if (k == biquads - 1)
                sq *= lastQScale;
            addSection (d, highPass ? design::highPass (sr, hz, sq) : design::lowPass (sr, hz, sq));
        }

        if (resonant && order == 1)
        {
            // a first-order section cannot resonate: the resonance is a peak at the corner
            const double bumpDb = 20.0 * std::log10 (std::max (q / kSqrtHalf, 1.0));
            addSection (d, design::peak (sr, hz, std::max (q, kSqrtHalf), bumpDb));
        }
    }

    int orderOf (EqSlope s) noexcept
    {
        const int i = std::clamp ((int) s, 0, (int) EqSlope::Count - 1);
        return kEqSlopeDb[(size_t) i] / 6;
    }

    double vintageDipQ (double q) noexcept
    {
        return std::clamp (0.9 * std::sqrt (q / kSqrtHalf), 0.3, 3.0);
    }

    // Kaiser window helpers
    double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double h = 0.25 * x * x;
        for (int k = 1; k < 200; ++k)
        {
            term *= h / ((double) k * (double) k);
            sum += term;
            if (term < 1.0e-17 * sum)
                break;
        }
        return sum;
    }

    constexpr double kKaiserBeta = 9.0;
}

//==============================================================================
bool BandDesign::isIdentity() const noexcept
{
    for (int i = 0; i < numSections; ++i)
    {
        const auto& c = s[i];
        if (c.b0 != 1.0 || c.b1 != c.a1 || c.b2 != c.a2)
            return false;
    }
    return true;
}

std::complex<double> BandDesign::response (std::complex<double> z1) const noexcept
{
    const cd z2 = z1 * z1;
    cd h (1.0, 0.0);
    for (int i = 0; i < numSections; ++i)
    {
        const auto& c = s[i];
        h *= (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    }
    return h;
}

void BandDesign::magPhase (std::complex<double> z1, double& mag, double& phase) const noexcept
{
    const cd z2 = z1 * z1;
    mag = 1.0;
    phase = 0.0;
    for (int i = 0; i < numSections; ++i)
    {
        const auto& c = s[i];
        const double sign = c.b0 < 0.0 ? -1.0 : 1.0;
        const cd n = sign * (c.b0 + c.b1 * z1 + c.b2 * z2);
        const cd d = 1.0 + c.a1 * z1 + c.a2 * z2;
        mag *= std::abs (n) / std::abs (d);
        phase += std::arg (n) - std::arg (d) + (sign < 0.0 ? kPi : 0.0);
    }
}

EqShape analogShape (EqShape s) noexcept
{
    return s == EqShape::Surgical ? EqShape::Bell : s;
}

bool isPassShape (EqShape s) noexcept
{
    switch (s)
    {
        case EqShape::HighPassFlat: case EqShape::HighPassResonant: case EqShape::HighPassBrickwall:
        case EqShape::LowPassFlat: case EqShape::LowPassResonant: case EqShape::LowPassBrickwall:
            return true;
        default:
            return false;
    }
}

bool usesSlope (EqShape s) noexcept
{
    return s == EqShape::HighPassFlat || s == EqShape::HighPassResonant || s == EqShape::LowPassFlat
        || s == EqShape::LowPassResonant;
}

int sectionCount (EqShape shape, EqSlope slope) noexcept
{
    switch (analogShape (shape))
    {
        case EqShape::BandShelf:
        case EqShape::LowShelfVintage:
        case EqShape::HighShelfVintage:
            return 2;
        case EqShape::HighPassBrickwall:
        case EqShape::LowPassBrickwall:
            return EllipticPrototype::kBiquads;
        case EqShape::HighPassFlat: case EqShape::LowPassFlat:
        case EqShape::HighPassResonant: case EqShape::LowPassResonant:
        {
            const int n = orderOf (slope);
            const bool res = shape == EqShape::HighPassResonant || shape == EqShape::LowPassResonant;
            if (n == 1)
                return res ? 2 : 1;
            return n / 2 + n % 2;
        }
        default:
            return 1;
    }
}

double scaledGainDb (double gainDb, double amountPercent) noexcept
{
    return std::clamp (gainDb * amountPercent * 0.01, -30.0, 15.0);
}

double proportionalQ (double q, double gainDb) noexcept
{
    // classic console behaviour: wide for small gains, narrower as |gain| grows (x1 at 6 dB, x2 at 15 dB)
    return std::clamp (q * std::pow (2.0, (std::abs (gainDb) - 6.0) / 9.0), 0.05, 60.0);
}

void designAnalog (EqShape shape, EqSlope slope, double sr, double hz, double g, double q, BandDesign& d) noexcept
{
    d.numSections = 0;
    const EqShape a = analogShape (shape);
    const bool flatGain = std::abs (g) < 1.0e-6;

    if (! isPassShape (a) && flatGain)
    {
        // Preserve the limiting poles at unity gain. The output is an exact identity, but keeping these states
        // warm makes automation away from 0 dB continuous with automation away from an arbitrarily small gain.
        // The primitive designs special-case 0 dB, so approach their limit from a negligible positive gain.
        designAnalog (a, slope, sr, hz, 1.0e-5, q, d);
        for (int i = 0; i < d.numSections; ++i)
        {
            d.s[i].b0 = 1.0;
            d.s[i].b1 = d.s[i].a1;
            d.s[i].b2 = d.s[i].a2;
        }
        return;
    }

    switch (a)
    {
        case EqShape::Bell:
        case EqShape::Surgical:
            addSection (d, design::peak (sr, hz, q, g));
            break;
        case EqShape::ProportionalQ:
            addSection (d, design::peak (sr, hz, proportionalQ (q, g), g));
            break;
        case EqShape::BandShelf:
        {
            // flat-topped band between two 2nd-order shelves; the edges (half gain) are where a bell of this Q has
            // its bandwidth: bw = 2 asinh (1 / (2 Q)) / ln 2 octaves
            const double bw = 2.0 * std::asinh (0.5 / std::max (q, 0.01)) / std::log (2.0);
            const double lo = hz * std::pow (2.0, -0.5 * bw), hi = hz * std::pow (2.0, 0.5 * bw);
            addSection (d, design::highShelf (sr, lo, kSqrtHalf, g));
            addSection (d, design::highShelf (sr, hi, kSqrtHalf, -g));
            break;
        }
        case EqShape::LowShelfAnalog:
            addSection (d, design::lowShelf (sr, hz, std::clamp (q, 0.1, 1.0), g));
            break;
        case EqShape::HighShelfAnalog:
            addSection (d, design::highShelf (sr, hz, std::clamp (q, 0.1, 1.0), g));
            break;
        case EqShape::LowShelfBaxandall:
            addSection (d, design::baxandallLow (sr, hz, g));
            break;
        case EqShape::HighShelfBaxandall:
            addSection (d, design::baxandallHigh (sr, hz, g));
            break;
        case EqShape::LowShelfVintage:
            addSection (d, design::lowShelf (sr, hz, 0.6, g));
            addSection (d, design::peak (sr, hz * 3.0, vintageDipQ (q), -0.35 * g));
            break;
        case EqShape::HighShelfVintage:
            addSection (d, design::highShelf (sr, hz, 0.6, g));
            addSection (d, design::peak (sr, hz / 3.0, vintageDipQ (q), -0.35 * g));
            break;
        case EqShape::LowShelfResonant:
            addSection (d, design::lowShelf (sr, hz, std::clamp (2.0 * q, kSqrtHalf, 4.0), g));
            break;
        case EqShape::HighShelfResonant:
            addSection (d, design::highShelf (sr, hz, std::clamp (2.0 * q, kSqrtHalf, 4.0), g));
            break;
        case EqShape::HighPassFlat:
            butterworthPass (d, true, sr, hz, orderOf (slope), 1.0, false, q);
            break;
        case EqShape::LowPassFlat:
            butterworthPass (d, false, sr, hz, orderOf (slope), 1.0, false, q);
            break;
        case EqShape::HighPassResonant:
            butterworthPass (d, true, sr, hz, orderOf (slope), std::clamp (q / kSqrtHalf, 0.1, 40.0), true, q);
            break;
        case EqShape::LowPassResonant:
            butterworthPass (d, false, sr, hz, orderOf (slope), std::clamp (q / kSqrtHalf, 0.1, 40.0), true, q);
            break;
        case EqShape::HighPassBrickwall:
            brickwall (d, true, sr, hz);
            break;
        case EqShape::LowPassBrickwall:
            brickwall (d, false, sr, hz);
            break;
        case EqShape::Count:
            break;
    }
}

//==============================================================================
bool hasDigitalPrototype (EqShape s) noexcept
{
    return s == EqShape::Surgical || s == EqShape::HighPassBrickwall || s == EqShape::LowPassBrickwall;
}

namespace
{
    // Digital brickwall: 128th-order Butterworth magnitude, -0.1 dB at the corner, -3 dB 1.5 % beyond it, -65 dB at
    // 0.1 octave (the FIR resolution, not the prototype, limits the steepness at low frequencies).
    constexpr int kDigitalBrickwallOrder = 128;

    struct BrickwallQs
    {
        double q[kDigitalBrickwallOrder / 2] {};
        double cornerScale = 1.0;   // passband edge / Butterworth -3 dB frequency (HP; LP uses the inverse)

        BrickwallQs() noexcept
        {
            for (int k = 0; k < kDigitalBrickwallOrder / 2; ++k)
                q[k] = design::butterworthQ (kDigitalBrickwallOrder, k);
            cornerScale = std::pow (std::pow (10.0, 0.01) - 1.0, 1.0 / (2.0 * kDigitalBrickwallOrder));
        }
    };

    const BrickwallQs kBrickwall;

    // 1 / sqrt (1 + x^(2n)) without overflow, x > 0
    double butterworthMag (double x) noexcept
    {
        const double t = 2.0 * kDigitalBrickwallOrder * std::log (x);
        if (t > 700.0)
            return std::exp (-0.5 * t);
        return 1.0 / std::sqrt (1.0 + std::exp (t));
    }

    // Digital surgical: LP -> BP transform (p = Q (s / w0 + w0 / s)) of a 4th-order Butterworth-type shelf
    // L(p) = prod (p - r p_k) / (p - p_k), p_k = Butterworth poles, r = G^(1/4):  |H|^2 = (G^2 + y^8) / (1 + y^8),
    // y = Q (f / f0 - f0 / f). Gain G across a flat top, unity beyond y = 1 (bandwidth f0 / Q), minimum phase.
    constexpr int kSurgicalOrder = 4;

    struct SurgicalPoles
    {
        cd p[kSurgicalOrder];

        SurgicalPoles() noexcept
        {
            for (int k = 0; k < kSurgicalOrder; ++k)
                p[k] = std::polar (1.0, kPi * (2.0 * k + kSurgicalOrder + 1.0) / (2.0 * kSurgicalOrder));
        }
    };

    const SurgicalPoles kSurgical;
}

void digitalPrototype (EqShape shape, double hz, double g, double q, double f, double& mag, double& phase,
                       bool wantPhase) noexcept
{
    f = std::max (f, 1.0e-6);
    hz = std::max (hz, 1.0);
    switch (shape)
    {
        case EqShape::Surgical:
        {
            if (std::abs (g) < 1.0e-9)
            {
                mag = 1.0;
                phase = 0.0;
                return;
            }
            const double x = f / hz;
            const double y = q * (x - 1.0 / x);
            const double G = std::pow (10.0, g / 20.0);
            const double y8 = std::pow (y * y, (double) kSurgicalOrder);
            mag = std::sqrt ((G * G + y8) / (1.0 + y8));
            phase = 0.0;
            if (wantPhase)
            {
                const double r = std::pow (G, 1.0 / kSurgicalOrder);
                const cd jy (0.0, y);
                for (const cd& pk : kSurgical.p)
                    phase += std::arg (jy - r * pk) - std::arg (jy - pk);
            }
            return;
        }
        case EqShape::HighPassBrickwall:
        {
            const double x = f / (hz * kBrickwall.cornerScale);
            mag = butterworthMag (1.0 / x);
            phase = 0.0;
            if (wantPhase)
                for (double sq : kBrickwall.q)
                    phase += kPi - std::atan2 (x / sq, 1.0 - x * x);
            return;
        }
        case EqShape::LowPassBrickwall:
        {
            const double x = f * kBrickwall.cornerScale / hz;
            mag = butterworthMag (x);
            phase = 0.0;
            if (wantPhase)
                for (double sq : kBrickwall.q)
                    phase -= std::atan2 (x / sq, 1.0 - x * x);
            return;
        }
        default:
            mag = 1.0;
            phase = 0.0;
            return;
    }
}

//==============================================================================
Svf toSvf (const BiquadCoeffs& c) noexcept
{
    const double A0 = 1.0 + c.a1 + c.a2;   // D at DC
    const double An = 1.0 - c.a1 + c.a2;   // D at Nyquist
    const double Ad = 1.0 - c.a2;
    Svf r;
    if (! (A0 > 0.0 && An > 0.0 && Ad > 0.0) || ! std::isfinite (A0 + An + c.b0 + c.b1 + c.b2))
        return r;   // not stable: identity (never produced by our designs)

    // z^-1 = (1 - g s) / (1 + g s) maps the poles to s^2 + k s + 1
    r.g = std::sqrt (A0 / An);
    const double D2 = r.g * r.g * An;
    r.k = 2.0 * r.g * Ad / D2;
    if (c.b0 == 1.0 && c.b1 == c.a1 && c.b2 == c.a2)
        return r;   // exact unity mix, while retaining the cancellation's pole states
    const double n0 = (c.b0 + c.b1 + c.b2) / D2;
    const double n1 = 2.0 * r.g * (c.b0 - c.b2) / D2;
    const double n2 = r.g * r.g * (c.b0 - c.b1 + c.b2) / D2;
    r.m0 = n2;
    r.m1 = n1 - n2 * r.k;
    r.m2 = n0 - n2;
    return r;
}

//==============================================================================
int firLength (int firSizeIndex, double sampleRate) noexcept
{
    const int mult = sampleRate > 100000.0 ? 4 : (sampleRate > 50000.0 ? 2 : 1);
    return (4096 << std::clamp (firSizeIndex, 0, 3)) * mult;
}

int firPartition (int n) noexcept { return std::max (16, n / 16); }

int firLatency (int firSizeIndex, double sampleRate) noexcept
{
    const int n = firLength (firSizeIndex, sampleRate);
    return n / 2 + firPartition (n);
}

void KernelDesigner::prepare (double sampleRate, int maxFirLength)
{
    sr = sampleRate;
    const size_t L = (size_t) maxFirLength * 2;
    spec.assign (L, cd {});
    twiddle.assign (L / 2, cd {});
    bitrev.assign (L, 0);
    zTable.assign ((size_t) maxFirLength + 1, cd {});
    logMag.assign ((size_t) maxFirLength + 1, 0.0);
    phase.assign ((size_t) maxFirLength + 1, 0.0);
    window.assign ((size_t) maxFirLength, 0.0);
    curN = 0;
}

void KernelDesigner::prepareSize (int n) noexcept
{
    curN = n;
    const int L = 2 * n;
    for (int k = 0; k < L / 2; ++k)
        twiddle[(size_t) k] = std::polar (1.0, 2.0 * kPi * k / L);   // inverse transform
    int bits = 0;
    while ((1 << bits) < L)
        ++bits;
    for (int i = 0; i < L; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            r |= ((i >> b) & 1) << (bits - 1 - b);
        bitrev[(size_t) i] = r;
    }
    for (int k = 0; k <= n; ++k)
        zTable[(size_t) k] = std::polar (1.0, -kPi * k / n);   // e^{-j w_k}, w_k = 2 pi k / L

    const int D = n / 2;
    const double i0b = besselI0 (kKaiserBeta);
    window[0] = 0.0;   // symmetric about D: taps 1 .. N-1
    for (int t = 1; t < n; ++t)
    {
        const double r = (double) (t - D) / (double) D;
        window[(size_t) t] = besselI0 (kKaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
    }
}

bool KernelDesigner::design (const EqBandParams* bands, float amountPercent, int firSizeIndex, float* out) noexcept
{
    const int n = firLength (firSizeIndex, sr);
    if ((size_t) n > window.size())
        return false;
    if (n != curN)
        prepareSize (n);

    const int L = 2 * n, D = n / 2;
    std::fill (logMag.begin(), logMag.begin() + n + 1, 0.0);
    std::fill (phase.begin(), phase.begin() + n + 1, 0.0);
    bool any = false;

    for (int b = 0; b < kEqBands; ++b)
    {
        const auto& bp = bands[b];
        if (! bp.on)
            continue;
        const double g = scaledGainDb (bp.gainDb, amountPercent);
        const double p = std::clamp ((double) bp.phasePercent * 0.01, 0.0, 1.0);

        if (hasDigitalPrototype (bp.shape))
        {
            if (bp.shape == EqShape::Surgical && std::abs (g) < 1.0e-9)
                continue;
            for (int k = 0; k <= n; ++k)
            {
                double m = 1.0, ph = 0.0;
                digitalPrototype (bp.shape, bp.freqHz, g, bp.q, (double) k * sr / L, m, ph, p > 0.0);
                logMag[(size_t) k] += std::log (std::max (m, 1.0e-12));
                phase[(size_t) k] += p * ph;
            }
        }
        else
        {
            BandDesign d;
            designAnalog (bp.shape, bp.slope, sr, bp.freqHz, g, bp.q, d);
            if (d.isIdentity())
                continue;
            for (int k = 0; k <= n; ++k)
            {
                if (p > 0.0)
                {
                    double m = 1.0, ph = 0.0;
                    d.magPhase (zTable[(size_t) k], m, ph);
                    logMag[(size_t) k] += std::log (std::max (m, 1.0e-12));
                    phase[(size_t) k] += p * ph;
                }
                else
                {
                    logMag[(size_t) k] += std::log (std::max (std::abs (d.response (zTable[(size_t) k])), 1.0e-12));
                }
            }
        }
        any = true;
    }

    if (any)
    {
        double worst = 0.0;
        for (int k = 0; k <= n; ++k)
            worst = std::max (worst, std::max (std::abs (logMag[(size_t) k]), std::abs (phase[(size_t) k])));
        any = worst > 1.0e-12;
    }
    if (! any)
    {
        std::fill (out, out + n, 0.0f);
        return false;
    }

    // spectrum with the linear-phase delay D: e^{-j w_k D} = e^{-j pi k / 2} = (-j)^k exactly
    static constexpr double kRe[4] { 1.0, 0.0, -1.0, 0.0 };
    static constexpr double kIm[4] { 0.0, -1.0, 0.0, 1.0 };
    for (int k = 0; k <= n; ++k)
    {
        const cd h = std::polar (std::exp (logMag[(size_t) k]), phase[(size_t) k]);
        const cd dly (kRe[k & 3], kIm[k & 3]);
        spec[(size_t) k] = h * dly;
    }
    spec[0] = cd (spec[0].real(), 0.0);
    spec[(size_t) n] = cd (spec[(size_t) n].real(), 0.0);
    for (int k = 1; k < n; ++k)
        spec[(size_t) (L - k)] = std::conj (spec[(size_t) k]);

    // inverse FFT (radix 2, double)
    for (int i = 0; i < L; ++i)
    {
        const int r = bitrev[(size_t) i];
        if (i < r)
            std::swap (spec[(size_t) i], spec[(size_t) r]);
    }
    for (int len = 2; len <= L; len <<= 1)
    {
        const int half = len / 2, stride = L / len;
        for (int i = 0; i < L; i += len)
            for (int k = 0; k < half; ++k)
            {
                const cd u = spec[(size_t) (i + k)];
                const cd v = spec[(size_t) (i + k + half)] * twiddle[(size_t) (k * stride)];
                spec[(size_t) (i + k)] = u + v;
                spec[(size_t) (i + k + half)] = u - v;
            }
    }

    const double scale = 1.0 / (double) L;
    for (int t = 0; t < n; ++t)
    {
        double v = spec[(size_t) t].real() * scale * window[(size_t) t];
        if (t == D)
            v -= 1.0;
        out[t] = (float) v;
    }
    return true;
}

} // namespace ps::eqdesign
