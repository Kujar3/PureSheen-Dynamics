#include "dsp/ToneTargets.h"
#include "dsp/ToneBalance.h"

#include <algorithm>
#include <cmath>

// Tone Balance target curves (see ToneTargets.h for the meaning of the numbers and the anchor points). The data is
// our own starting point, written from general knowledge of long-term music spectra (bass elevated, roughly -2.5 ..
// -4 dB / octave above 1 kHz relative to pink); it is meant to be tuned later by listening and by measuring material
// the user owns.
namespace ps
{
namespace
{
constexpr int kNumTargets = (int) ToneTarget::Count;

//                                     sub   bass  lowMid pres  slope  air   tol
constexpr ToneTargetShape kTargets[kNumTargets] {
    /*  0 Balanced        */ {  3.0f, 6.0f, 2.5f, -0.5f, -3.0f, -1.0f, 2.0f },
    /*  1 Cinematic       */ {  0.0f, 3.0f, 3.0f, -1.0f, -3.8f, -2.0f, 3.0f },
    /*  2 Country         */ {  1.0f, 4.5f, 2.5f,  0.0f, -2.8f, -0.5f, 2.0f },
    /*  3 EDM             */ {  6.0f, 8.0f, 1.5f, -0.5f, -2.6f, -0.5f, 2.5f },
    /*  4 Folk            */ { -1.0f, 3.0f, 3.0f,  0.0f, -3.4f, -1.5f, 2.5f },
    /*  5 Hip-Hop         */ {  7.0f, 9.0f, 2.0f, -1.0f, -3.2f, -1.5f, 2.5f },
    /*  6 Jazz            */ { -1.0f, 3.5f, 3.0f, -1.0f, -3.8f, -2.5f, 3.0f },
    /*  7 Pop             */ {  4.0f, 7.0f, 2.0f,  0.0f, -2.8f, -0.5f, 2.0f },
    /*  8 Reggae          */ {  6.0f, 8.5f, 2.5f, -1.5f, -3.6f, -2.0f, 2.5f },
    /*  9 RnB and Soul    */ {  5.0f, 8.0f, 2.5f, -0.5f, -3.2f, -1.0f, 2.5f },
    /* 10 Rock            */ {  2.0f, 5.0f, 2.0f,  0.5f, -2.6f, -1.0f, 2.0f },
    /* 11 Alt Rock        */ {  2.0f, 5.0f, 2.5f,  0.5f, -2.7f, -1.0f, 2.0f },
    /* 12 Ambient         */ {  1.0f, 3.5f, 2.5f, -1.0f, -3.6f, -1.0f, 3.0f },
    /* 13 Classic Hip-Hop */ {  5.0f, 8.0f, 3.0f, -1.0f, -3.6f, -2.5f, 2.5f },
    /* 14 Classic Rock    */ {  0.0f, 4.0f, 3.0f,  0.5f, -3.2f, -2.0f, 2.5f },
    /* 15 Dance Pop       */ {  5.0f, 7.5f, 1.5f,  0.0f, -2.6f,  0.0f, 2.0f },
    /* 16 Drum and Bass   */ {  6.0f, 7.5f, 1.0f,  0.0f, -2.4f,  0.0f, 2.5f },
    /* 17 Dubstep         */ {  8.0f, 8.5f, 1.0f,  0.0f, -2.4f,  0.0f, 3.0f },
    /* 18 Electropop      */ {  4.0f, 6.5f, 1.5f,  0.0f, -2.6f,  0.0f, 2.0f },
    /* 19 Future Bass     */ {  6.0f, 7.5f, 1.5f,  0.0f, -2.5f,  0.5f, 2.5f },
    /* 20 House           */ {  6.0f, 8.0f, 1.5f, -0.5f, -2.8f, -0.5f, 2.0f },
    /* 21 Hyperpop        */ {  5.0f, 6.5f, 1.0f,  1.0f, -2.2f,  1.0f, 3.0f },
    /* 22 Indie Pop       */ {  2.0f, 5.0f, 2.5f,  0.0f, -3.0f, -1.0f, 2.5f },
    /* 23 Indie Rock      */ {  1.0f, 4.5f, 2.5f,  0.5f, -2.9f, -1.5f, 2.5f },
    /* 24 K-Pop and J-Pop */ {  4.0f, 6.5f, 1.5f,  0.5f, -2.5f,  0.5f, 2.0f },
    /* 25 Latin Pop       */ {  5.0f, 7.5f, 2.0f,  0.0f, -2.8f, -0.5f, 2.0f },
    /* 26 Lo-Fi           */ {  2.0f, 6.0f, 3.5f, -1.5f, -4.5f, -4.0f, 3.0f },
    /* 27 Metal           */ {  1.0f, 4.0f, 1.5f,  1.0f, -2.3f, -1.0f, 2.0f },
    /* 28 Pop Country     */ {  2.0f, 5.5f, 2.0f,  0.0f, -2.7f, -0.5f, 2.0f },
    /* 29 Post Rock       */ {  1.0f, 4.0f, 2.5f,  0.0f, -3.0f, -1.5f, 3.0f },
    /* 30 Punk Rock       */ {  0.0f, 3.5f, 2.0f,  1.0f, -2.4f, -1.0f, 2.5f },
    /* 31 Reggaeton       */ {  7.0f, 9.0f, 2.0f, -0.5f, -3.0f, -1.0f, 2.5f },
    /* 32 Techno          */ {  7.0f, 8.0f, 1.0f, -0.5f, -2.7f, -0.5f, 2.5f },
    /* 33 Trap            */ {  9.0f, 9.5f, 1.5f, -0.5f, -3.0f, -0.5f, 3.0f },
    /* 34 Vocal Jazz      */ { -2.0f, 2.5f, 3.0f,  0.0f, -3.8f, -2.5f, 3.0f },
    /* 35 Flat (pink)     */ {  0.0f, 0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 2.0f },
};

constexpr int kAnchors = 9;
constexpr double kAnchorHz[kAnchors] { 20.0, 30.0, 80.0, 300.0, 1000.0, 3000.0, 8000.0, 14000.0, 20000.0 };

bool isFlat (const ToneTargetShape& s) noexcept
{
    return s.sub == 0.0f && s.bass == 0.0f && s.lowMid == 0.0f && s.pres == 0.0f && s.slope == 0.0f && s.air == 0.0f;
}

// Monotone cubic Hermite interpolation (Fritsch-Carlson / PCHIP) through (x[i], y[i]) at xq (x ascending).
double pchip (const double* x, const double* y, int n, double xq) noexcept
{
    if (xq <= x[0])
        return y[0];
    if (xq >= x[n - 1])
        return y[n - 1];

    double h[kAnchors], delta[kAnchors], d[kAnchors];
    for (int i = 0; i < n - 1; ++i)
    {
        h[i] = x[i + 1] - x[i];
        delta[i] = (y[i + 1] - y[i]) / h[i];
    }
    for (int i = 1; i < n - 1; ++i)
    {
        if (delta[i - 1] * delta[i] <= 0.0)
        {
            d[i] = 0.0;
        }
        else
        {
            const double w1 = 2.0 * h[i] + h[i - 1], w2 = h[i] + 2.0 * h[i - 1];
            d[i] = (w1 + w2) / (w1 / delta[i - 1] + w2 / delta[i]);
        }
    }
    auto endSlope = [] (double h0, double h1, double d0, double d1) noexcept
    {
        double s = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
        if (s * d0 <= 0.0)
            s = 0.0;
        else if (d0 * d1 <= 0.0 && std::abs (s) > 3.0 * std::abs (d0))
            s = 3.0 * d0;
        return s;
    };
    d[0] = endSlope (h[0], h[1], delta[0], delta[1]);
    d[n - 1] = endSlope (h[n - 2], h[n - 3], delta[n - 2], delta[n - 3]);

    int i = 0;
    while (i < n - 2 && xq >= x[i + 1])
        ++i;
    const double t = (xq - x[i]) / h[i];
    const double t2 = t * t, t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * y[i] + (t3 - 2.0 * t2 + t) * h[i] * d[i]
         + (-2.0 * t3 + 3.0 * t2) * y[i + 1] + (t3 - t2) * h[i] * d[i + 1];
}
} // namespace

const ToneTargetShape& toneTargetShape (ToneTarget t) noexcept
{
    const int i = std::clamp ((int) t, 0, kNumTargets - 1);
    return kTargets[i];
}

double toneTargetDb (ToneTarget target, double hz, double* tolDb) noexcept
{
    const auto& s = toneTargetShape (target);
    const double f = std::clamp (std::isfinite (hz) ? hz : 1000.0, 20.0, 20000.0);
    const double lf = std::log2 (f);

    if (tolDb != nullptr)
    {
        // 1.5 x tol below 100 Hz and above 8 kHz, tol in 250 Hz .. 4 kHz, linear in log f in between
        double k = 1.0;
        if (f < 250.0)
            k = 1.0 + 0.5 * std::clamp (std::log2 (250.0 / f) / std::log2 (2.5), 0.0, 1.0);
        else if (f > 4000.0)
            k = 1.0 + 0.5 * std::clamp (std::log2 (f / 4000.0), 0.0, 1.0);
        *tolDb = (double) s.tol * k;
    }

    if (isFlat (s))
        return 0.0;

    const double slope = s.slope;
    const double v14k = slope * std::log2 (14.0) + s.air;
    const double y[kAnchors] { s.sub - 6.0, s.sub, s.bass, s.lowMid, 0.0, s.pres + slope * std::log2 (3.0),
                               slope * 3.0, v14k, v14k - 6.0 };
    double x[kAnchors];
    for (int i = 0; i < kAnchors; ++i)
        x[i] = std::log2 (kAnchorHz[i]);
    return pchip (x, y, kAnchors, lf);
}
} // namespace ps
