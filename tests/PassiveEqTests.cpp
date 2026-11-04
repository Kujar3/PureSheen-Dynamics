#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/PassiveEq.h"
#include "dsp/PassiveEqDesign.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Passive EQ tests (dsp/PassiveEq.h). Prefix "peq.". Owner: mod-peq.
namespace
{
using namespace ps;
using pstest::cd;
using pstest::kPi;
using pstest::note;

constexpr double kSr = 48000.0;

struct Values
{
    std::vector<float> v;
    Values()
    {
        for (const auto& s : paramSpecs())
            v.push_back (s.defaultValue);
    }
    void set (const std::string& id, float x)
    {
        const int i = paramIndex ("peq." + id);
        REQUIRE (i >= 0);
        v[(size_t) i] = x;
    }
    ParamValues data() const { return v.data(); }
};

struct Stereo
{
    std::vector<float> l, r;
    size_t size() const { return l.size(); }
};

Stereo dual (const std::vector<float>& x) { return { x, x }; }

std::unique_ptr<PassiveEq> makeEq (double sr, const Values& vals, int paramSet = 0, int maxBlock = 512)
{
    auto m = std::make_unique<PassiveEq>();
    m->setParamSet (paramSet);
    m->prepare (sr, maxBlock);
    m->setParams (vals.data());
    return m;
}

using BlockHook = std::function<void (size_t, Values&)>;

Stereo run (PassiveEq& m, Values& vals, const Stereo& in, ChannelMode mode = ChannelMode::Stereo, int block = 512,
            const BlockHook& onBlock = {})
{
    Stereo out = in;
    ProcessContext ctx;
    ctx.channelMode = mode;
    ctx.paramSet = m.getParamSet();
    for (size_t start = 0; start < in.size(); start += (size_t) block)
    {
        if (onBlock)
            onBlock (start, vals);
        m.setParams (vals.data());
        const int n = (int) std::min ((size_t) block, in.size() - start);
        float* ch[2] { out.l.data() + start, out.r.data() + start };
        m.process (ch, n, ctx);
    }
    return out;
}

double maxDelayedDiff (const std::vector<float>& out, const std::vector<float>& in, int latency, size_t from = 0)
{
    double m = 0.0;
    for (size_t i = std::max (from, (size_t) latency); i < out.size(); ++i)
        m = std::max (m, std::abs ((double) out[i] - (double) in[i - (size_t) latency]));
    return m;
}

double db (double x) { return pstest::dbOf (x); }

// Measured response (dB) at hz: impulse through the module (drive 0, so a pure delay of the latency), DTFT.
struct Measured
{
    std::vector<float> h0, h1;
    double sr = kSr;

    double at (double hz, int channel = 0) const
    {
        const auto& h = channel == 0 ? h0 : h1;
        cd acc {};
        const double w = 2.0 * kPi * hz / sr;
        for (size_t n = 0; n < h.size(); ++n)
            acc += (double) h[n] * std::polar (1.0, -w * (double) n);
        return db (std::abs (acc));
    }
};

Measured measure (double sr, Values& vals, ChannelMode mode = ChannelMode::Stereo, int paramSet = 0)
{
    auto m = makeEq (sr, vals, paramSet);
    Stereo imp { std::vector<float> (65536, 0.0f), std::vector<float> (65536, 0.0f) };
    imp.l[0] = imp.r[0] = 1.0f;
    const auto out = run (*m, vals, imp, mode);
    return { out.l, out.r, sr };
}

double toneGainDb (const std::vector<float>& out, const std::vector<float>& in, double hz, double sr, size_t from,
                   size_t len)
{
    return db (std::abs (pstest::toneOf (out, from, len, hz, sr))) - db (std::abs (pstest::toneOf (in, from, len, hz, sr)));
}

double maxSecondDiff (const std::vector<float>& y, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = std::max<size_t> (from, 2); i < to && i < y.size(); ++i)
        m = std::max (m, std::abs ((double) y[i] - 2.0 * (double) y[i - 1] + (double) y[i - 2]));
    return m;
}

// Octaves between the two points around `centreHz` where the response falls to half its dB value there.
double halfGainWidthOct (const Measured& r, double centreHz)
{
    const double peak = r.at (centreHz);
    auto edge = [&] (double dir) {
        double lo = 0.0, hi = dir < 0.0 ? 4.0 : std::min (4.0, std::log2 (0.4999 * r.sr / centreHz));
        // A DTFT repeats beyond Nyquist. Never mistake that repeated spectrum for the upper bell flank,
        // or report a complete bandwidth when the half-gain crossing is outside the measurable band.
        REQUIRE (std::abs (r.at (centreHz * std::pow (2.0, dir * hi))) < 0.5 * std::abs (peak));
        for (int it = 0; it < 40; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            const double g = r.at (centreHz * std::pow (2.0, dir * mid));
            if (std::abs (g) > 0.5 * std::abs (peak))
                lo = mid;
            else
                hi = mid;
        }
        return 0.5 * (lo + hi);
    };
    return edge (-1.0) + edge (1.0);
}

// Hann-windowed amplitude spectrum of x[from .. from + n), n a power of two.
std::vector<double> amplitudeSpectrum (const std::vector<float>& x, size_t from, size_t n)
{
    std::vector<cd> a (n);
    for (size_t i = 0; i < n; ++i)
        a[i] = (double) x[from + i] * (0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) n));
    pstest::fftInPlace (a);
    std::vector<double> mag (n / 2);
    for (size_t k = 0; k < n / 2; ++k)
        mag[k] = 4.0 * std::abs (a[k]) / (double) n;
    return mag;
}

double binPeak (const std::vector<double>& mag, size_t bin)
{
    double m = 0.0;
    for (size_t k = bin > 2 ? bin - 2 : 0; k <= bin + 2 && k < mag.size(); ++k)
        m = std::max (m, mag[k]);
    return m;
}

// Energy above `cutHz` against the whole energy (dB) of a Hann-windowed 8192-sample segment from `from`: a click
// splatters over the whole band, a 10 ms glide of a tone stays next to it.
double hfShareDb (const std::vector<float>& y, size_t from, double sr, double cutHz = 3000.0)
{
    const size_t n = 8192;
    const auto mag = amplitudeSpectrum (y, from, n);
    double hf = 0.0, total = 0.0;
    for (size_t k = 1; k < mag.size(); ++k)
    {
        const double p = mag[k] * mag[k];
        total += p;
        if ((double) k * sr / (double) n > cutHz)
            hf += p;
    }
    return 10.0 * std::log10 (hf / total + 1.0e-30);
}

bool allFinite (const Stereo& s)
{
    for (size_t i = 0; i < s.size(); ++i)
        if (! std::isfinite (s.l[i]) || ! std::isfinite (s.r[i]))
            return false;
    return true;
}
} // namespace

//==============================================================================
TEST_CASE ("peq. module type and contract registration")
{
    PassiveEq m;
    CHECK (m.getType() == ModuleType::PassiveEq);
    CHECK_EQ (std::string (moduleInfo (ModuleType::PassiveEq).idPrefix), std::string ("peq"));
    CHECK_EQ (m.getLatencySamples(), 3);
    CHECK_EQ (m.getMagnitudeDb (0, 1000.0), 0.0);   // before any setParams
}

TEST_CASE ("peq. neutral at the defaults: the input delayed by exactly 3 samples in every mode")
{
    const Stereo in { pstest::pinkNoise (0.9, 3.0, kSr, 1u), pstest::pinkNoise (0.9, 3.0, kSr, 2u) };
    for (auto mode : { ChannelMode::Stereo, ChannelMode::MidSide, ChannelMode::LeftRight })
        for (int set = 0; set < 2; ++set)
        {
            Values vals;
            auto m = makeEq (kSr, vals, set);
            const auto out = run (*m, vals, in, mode, 333);
            const double d = std::max (maxDelayedDiff (out.l, in.l, kPassiveEqLatency),
                                       maxDelayedDiff (out.r, in.r, kPassiveEqLatency));
            CHECK_EQ (d, 0.0);
            for (int i = 0; i < kPassiveEqLatency; ++i)
                CHECK_EQ (out.l[(size_t) i], 0.0f);
        }

    // frequency selectors and the bandwidth moved while every amount stays 0: still exact
    Values vals;
    auto m = makeEq (kSr, vals);
    const auto out = run (*m, vals, in, ChannelMode::MidSide, 256, [] (size_t start, Values& v) {
        const int k = (int) (start / 256);
        v.set ("a.lowHz", (float) (k % 5));
        v.set ("b.highHz", (float) (k % 7));
        v.set ("a.highQ", (float) (k % 11));
        v.set ("b.midHz", (float) (k % 11));
        v.set ("a.highCutHz", (float) (k % 3));
    });
    const double d = std::max (maxDelayedDiff (out.l, in.l, 3), maxDelayedDiff (out.r, in.r, 3));
    note ("max |out - in delayed| with frequencies moving at amount 0", d);
    CHECK_EQ (d, 0.0);
}

TEST_CASE ("peq. latency: exact neutral impulse delay and driven low-frequency delay at 44.1 / 48 / 96 / 192 kHz")
{
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        // drive 0: the impulse comes out at exactly the latency
        Values vals;
        vals.set ("a.lowBoost", 0.0f);
        auto m = makeEq (sr, vals);
        CHECK_EQ (m->getLatencySamples(), kPassiveEqLatency);
        Stereo imp { std::vector<float> (4096, 0.0f), std::vector<float> (4096, 0.0f) };
        imp.l[0] = imp.r[0] = 1.0f;
        const auto out = run (*m, vals, imp);
        size_t peakAt = 0;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::abs (out.l[i]) > std::abs (out.l[peakAt]))
                peakAt = i;
        CHECK_EQ ((int) peakAt, kPassiveEqLatency);
        CHECK_EQ (out.l[(size_t) kPassiveEqLatency], 1.0f);

        // Drive 5: the quiet-tone phase delay stays close to the common clean path's 3-sample delay.
        Values driven;
        driven.set ("drive", 5.0f);
        auto md = makeEq (sr, driven);
        CHECK_EQ (md->getLatencySamples(), kPassiveEqLatency);
        const double hz = 100.0;
        const auto x = pstest::sine (hz, -40.0, 1.0, sr);
        const auto y = run (*md, driven, dual (x));
        const size_t from = (size_t) (0.5 * sr), len = (size_t) std::lround (0.4 * sr);
        const double ph = std::arg (pstest::toneOf (y.l, from, len, hz, sr) / pstest::toneOf (x, from, len, hz, sr));
        const double delay = -ph / (2.0 * kPi * hz / sr);
        note ("sr " + std::to_string ((int) sr) + ": tube path delay at 100 Hz (samples)", delay);
        CHECK_NEAR (delay, (double) kPassiveEqLatency, 0.01);
    }
}

// The reference's measured curves (black-box renders at 44.1 kHz, fitted analog forms with max error <= 0.02 dB, F in Hz):
// low boost 1 + c / (jF + p), low cut 1 - c / (jF + p), both together a biquad, mid bands analog peaks.
double lowShelfRefDb (double hz, double c, double p, double sign)
{
    return db (std::abs (1.0 + sign * c / (cd (p, hz))));
}

double peakRefDb (double hz, double g, double f0, double q)
{
    const double A = std::pow (10.0, g / 40.0);
    const cd S (0.0, hz / f0);
    return db (std::abs ((S * S + S * A / q + 1.0) / (S * S + S / (A * q) + 1.0)));
}

TEST_CASE ("peq. low boost and low cut follow the reference's measured passive shelves")
{
    // (position, boost, cut, c, p): every measured render of the low section
    struct Shelf { int pos; float boost, cut; double c, p; };
    const Shelf shelves[] { { 0, 5.0f, 0.0f, 80.18, 46.82 }, { 1, 5.0f, 0.0f, 176.39, 102.99 },
                            { 2, 5.0f, 0.0f, 275.81, 161.04 }, { 3, 5.0f, 0.0f, 375.18, 219.06 },
                            { 4, 5.0f, 0.0f, 534.16, 311.88 }, { 3, 2.5f, 0.0f, 374.83, 619.86 },
                            { 3, 7.5f, 0.0f, 375.26, 107.68 }, { 3, 10.0f, 0.0f, 375.28, 64.87 },
                            { 0, 0.0f, 5.0f, 133.64, 165.47 }, { 3, 0.0f, 5.0f, 607.18, 751.80 },
                            { 4, 0.0f, 5.0f, 890.22, 1102.26 }, { 3, 0.0f, 2.5f, 607.00, 896.15 },
                            { 3, 0.0f, 10.0f, 607.28, 679.61 } };
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        double worst = 0.0;
        for (const auto& s : shelves)
        {
            Values v;
            v.set ("a.lowHz", (float) s.pos);
            v.set ("a.lowBoost", s.boost);
            v.set ("a.lowCut", s.cut);
            const auto r = measure (sr, v);
            for (double hz = 10.0; hz < 0.45 * sr; hz *= 1.15)
                worst = std::max (worst, std::abs (r.at (hz) - lowShelfRefDb (hz, s.c, s.p, s.boost > 0.0f ? 1.0 : -1.0)));
        }
        note ("sr " + std::to_string ((int) sr) + ": worst error vs the measured low shelves (dB)", worst);
        CHECK (worst < 0.03);
    }

    // label 5 boosts +8.67 dB at DC at every position, cut 5 -14.32 dB; the boost moves only its pole
    Values v;
    v.set ("a.lowHz", 3.0f);
    v.set ("a.lowBoost", 5.0f);
    CHECK_NEAR (measure (44100.0, v).at (2.0), 8.67, 0.03);
    v.set ("a.lowBoost", 0.0f);
    v.set ("a.lowCut", 5.0f);
    CHECK_NEAR (measure (44100.0, v).at (2.0), -14.32, 0.05);

    // boost 5 + cut 10 at 60 Hz: not the cascade, not the plain parallel connection; the measured biquad
    // (F^2 + 671.8 F + 290205) / ((F + 217.4) (F + 689.9)), F = j hz
    v.set ("a.lowBoost", 5.0f);
    v.set ("a.lowCut", 10.0f);
    const auto r2 = measure (44100.0, v);
    double worst = 0.0;
    for (double hz = 10.0; hz < 19000.0; hz *= 1.1)
    {
        const cd F (0.0, hz);
        const double ref = db (std::abs ((F * F + 671.75 * F + 290205.0) / ((F + 217.4) * (F + 689.9))));
        worst = std::max (worst, std::abs (r2.at (hz) - ref));
    }
    note ("boost 5 + cut 10 at 60 Hz: worst error vs the measured curve (dB)", worst);
    note ("    dB at 20 Hz", r2.at (20.0));
    CHECK (worst < 0.05);
    CHECK_NEAR (r2.at (20.0), 5.69, 0.05);   // still a boost at the bottom, with a dip above it
    CHECK (r2.at (600.0) < -2.9);

    // amounts between the measured ones: monotone and smooth (no step between 0.25 dB label steps)
    for (const char* id : { "a.lowBoost", "a.lowCut" })
    {
        Values w;
        w.set ("a.lowHz", 3.0f);
        double prev = 0.0, maxStep = 0.0;
        bool monotone = true;
        for (float a = 0.25f; a <= 10.0f; a += 0.25f)
        {
            w.set (id, a);
            auto m = makeEq (44100.0, w);
            const double g = m->getMagnitudeDb (0, 5.0);
            monotone = monotone && (std::string (id) == "a.lowBoost" ? g > prev : g < prev);
            maxStep = std::max (maxStep, std::abs (g - prev));
            prev = g;
        }
        note (std::string (id) + ": largest change per 0.25 label dB (dB at 5 Hz)", maxStep);
        CHECK (monotone);
        CHECK (maxStep < 2.5);
    }
}

TEST_CASE ("peq. high boost follows the reference's measured peaks (position, amount, High Q)")
{
    // the reference's curves at 30 Hz .. 15 kHz (dB), per (position, boost, High Q)
    constexpr double hz[11] { 30, 100, 300, 1000, 2000, 4000, 6000, 8000, 10000, 12000, 15000 };
    struct Ref { int pos; float boost, highQ; double db[11]; };
    const Ref refs[] {
        { 1, 5, 0, { 0.00, 0.01, 0.09, 0.87, 2.25, 3.33, 2.97, 2.34, 1.79, 1.36, 0.91 } },   // 4 kHz
        { 2, 5, 0, { 0.00, 0.00, 0.04, 0.43, 1.42, 3.06, 3.27, 2.77, 2.18, 1.68, 1.13 } },   // 5 kHz
        { 0, 5, 0, { 0.00, 0.01, 0.09, 0.94, 2.60, 3.05, 1.97, 1.24, 0.82, 0.57, 0.35 } },   // 3 kHz
        { 3, 5, 0, { 0.00, 0.00, 0.04, 0.42, 1.34, 2.85, 3.32, 3.16, 2.77, 2.33, 1.73 } },   // 8 kHz (peaks at 6.2k)
        { 4, 5, 0, { 0.00, 0.00, 0.02, 0.19, 0.68, 1.87, 2.73, 3.18, 3.33, 3.27, 2.98 } },   // 10 kHz
        { 5, 5, 0, { 0.00, 0.00, 0.02, 0.18, 0.64, 1.73, 2.53, 3.00, 3.23, 3.32, 3.28 } },   // 12 kHz
        { 6, 5, 0, { 0.00, 0.00, 0.00, 0.03, 0.14, 0.51, 1.01, 1.56, 2.08, 2.53, 3.01 } },   // 16 kHz (peak above 20k)
        { 3, 2.5, 0, { 0.00, 0.00, 0.01, 0.08, 0.30, 0.87, 1.13, 1.03, 0.83, 0.64, 0.42 } },
        { 3, 10, 0, { 0.00, 0.02, 0.20, 1.88, 5.11, 10.14, 12.06, 11.36, 9.83, 8.30, 6.38 } },
        { 3, 5, 5, { 0.00, 0.00, 0.03, 0.36, 1.26, 3.26, 4.08, 3.79, 3.13, 2.48, 1.71 } },
        { 3, 5, 10, { 0.00, 0.00, 0.03, 0.29, 1.12, 3.71, 5.30, 4.68, 3.48, 2.53, 1.59 } } };
    double worst = 0.0;
    for (const auto& r : refs)
    {
        Values v;
        v.set ("a.highHz", (float) r.pos);
        v.set ("a.highBoost", r.boost);
        v.set ("a.highQ", r.highQ);
        const auto m = measure (44100.0, v);
        for (int i = 0; i < 11; ++i)
            worst = std::max (worst, std::abs (m.at (hz[i]) - r.db[i]));
    }
    note ("worst error vs the measured high boost curves, 44.1 kHz (dB)", worst);
    CHECK (worst < 0.2);

    // High Q 0 -> 10 (boost 5, 8 kHz): the peak rises +3.3 -> +5.3 dB and narrows (half-gain width 2.7 -> 1.9 oct)
    Values v;
    v.set ("a.highBoost", 5.0f);
    v.set ("a.highQ", 0.0f);
    const double broad = halfGainWidthOct (measure (96000.0, v), 6200.0);
    v.set ("a.highQ", 10.0f);
    const auto narrowR = measure (96000.0, v);
    const double narrow = halfGainWidthOct (narrowR, 6200.0);
    note ("half-gain width (oct) at boost 5, High Q 0", broad);
    note ("    High Q 10", narrow);
    CHECK_NEAR (broad, 2.73, 0.1);
    CHECK_NEAR (narrow, 1.86, 0.1);
    CHECK_NEAR (narrowR.at (6200.0), 5.32, 0.1);

    // the same curve at 48 / 96 kHz (analog prototypes): within 0.1 dB below 10 kHz
    for (int pos : { 0, 3, 5 })
    {
        Values w;
        w.set ("a.highHz", (float) pos);
        w.set ("a.highBoost", 7.0f);
        w.set ("a.highQ", 3.0f);
        const auto a = measure (44100.0, w), b = measure (96000.0, w);
        double d = 0.0;
        for (double f = 50.0; f < 10000.0; f *= 1.1)
            d = std::max (d, std::abs (a.at (f) - b.at (f)));
        CHECK (d < 0.1);
    }
}

TEST_CASE ("peq. high cut follows the reference's measured first-order shelves")
{
    constexpr double hz[11] { 30, 100, 300, 1000, 2000, 4000, 6000, 8000, 10000, 12000, 15000 };
    struct Ref { int pos; float cut; double db[11]; };
    const Ref refs[] {
        { 0, 5, { -0.00, -0.01, -0.06, -0.59, -1.67, -3.08, -3.67, -3.94, -4.08, -4.16, -4.23 } },   // 5 kHz
        { 0, 10, { -0.00, -0.03, -0.28, -2.39, -5.92, -10.82, -13.82, -15.83, -17.25, -18.29, -19.38 } },
        { 1, 5, { -0.00, -0.00, -0.02, -0.26, -0.86, -2.15, -2.98, -3.46, -3.74, -3.92, -4.08 } },   // 10 kHz
        { 2, 5, { -0.00, -0.00, -0.01, -0.07, -0.26, -0.87, -1.57, -2.19, -2.69, -3.07, -3.48 } } };   // 20 kHz
    double worst = 0.0;
    for (const auto& r : refs)
    {
        Values v;
        v.set ("a.highCutHz", (float) r.pos);
        v.set ("a.highCut", r.cut);
        const auto m = measure (44100.0, v);
        for (int i = 0; i < 11; ++i)
            worst = std::max (worst, std::abs (m.at (hz[i]) - r.db[i]));
    }
    note ("worst error vs the measured high cut curves, 44.1 kHz (dB)", worst);
    CHECK (worst < 0.15);

    // a cut never boosts, and deeper amounts cut more at every frequency
    Values v;
    v.set ("a.highCutHz", 2.0f);
    double prev = 0.0;
    bool deeper = true;
    for (float a = 1.0f; a <= 10.0f; a += 1.0f)
    {
        v.set ("a.highCut", a);
        const double g = measure (44100.0, v).at (16000.0);
        deeper = deeper && g < prev && g <= 0.0;
        prev = g;
    }
    CHECK (deeper);
}

TEST_CASE ("peq. midrange section follows the reference's measured peaks")
{
    // (amount id, frequency id, index, amount, measured analog peak: gain, centre, Q)
    struct Case { const char* amount; const char* hz; int index; float value; double g, f0, q; };
    const Case cases[] { { "a.lmBoost", "a.lmHz", 1, 10.0f, 12.947, 358.18, 0.7971 },
                         { "a.midCut", "a.midHz", 4, 10.0f, -14.200, 1032.37, 1.6425 },
                         { "a.hmBoost", "a.hmHz", 2, 10.0f, 8.582, 3686.79, 1.8418 },
                         { "a.lmBoost", "a.lmHz", 1, 5.0f, 4.336, 364.26, 0.9695 },
                         { "a.midCut", "a.midHz", 4, 5.0f, -5.078, 1037.80, 0.8666 },
                         { "a.hmBoost", "a.hmHz", 2, 5.0f, 3.283, 3681.96, 2.7075 } };
    double worst = 0.0;
    for (const auto& c : cases)
    {
        Values v;
        v.set (c.hz, (float) c.index);
        v.set (c.amount, c.value);
        const auto r = measure (44100.0, v);
        for (double hz = 20.0; hz < 19000.0; hz *= 1.1)
            worst = std::max (worst, std::abs (r.at (hz) - peakRefDb (hz, c.g, c.f0, c.q)));
    }
    note ("worst error vs the measured mid peaks, 44.1 kHz (dB)", worst);
    CHECK (worst < 0.06);

    // the three bands together (5 / 5 / 5 at 300 Hz / 1 kHz / 3 kHz) cascade
    Values all;
    all.set ("a.lmHz", 1.0f);
    all.set ("a.lmBoost", 5.0f);
    all.set ("a.midHz", 4.0f);
    all.set ("a.midCut", 5.0f);
    all.set ("a.hmBoost", 5.0f);
    const auto r = measure (44100.0, all);
    CHECK_NEAR (r.at (346.0), 3.47, 0.05);
    CHECK_NEAR (r.at (1076.0), -4.41, 0.05);

    // the boosts widen with the amount, the cut narrows
    for (const auto& c : { cases[0], cases[1], cases[2] })
    {
        Values v;
        v.set (c.hz, (float) c.index);
        v.set (c.amount, 10.0f);
        const auto r10 = measure (96000.0, v);
        v.set (c.amount, 5.0f);
        const auto r5 = measure (96000.0, v);
        const double ratio = halfGainWidthOct (r10, c.f0) / halfGainWidthOct (r5, c.f0);
        note (std::string (c.amount) + ": half-gain width at 10 / at 5", ratio);
        CHECK (c.g > 0.0 ? ratio > 1.05 : ratio < 0.8);
    }

    // every mid-cut step lands near its label (the measured 1 kHz position is 3.8 % above it)
    Values v;
    v.set ("a.midCut", 10.0f);
    for (int i = 0; i < 11; ++i)
    {
        v.set ("a.midHz", (float) i);
        CHECK_NEAR (measure (kSr, v).at (kPassiveEqMidHz[(size_t) i] * 1.0324), -14.2, 0.1);
    }
}

TEST_CASE ("peq. getMagnitudeDb matches the audio at 44.1 / 48 / 96 kHz")
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        Values v;
        v.set ("a.lowHz", 1.0f);
        v.set ("a.lowBoost", 7.0f);
        v.set ("a.lowCut", 3.0f);
        v.set ("a.highHz", 5.0f);
        v.set ("a.highBoost", 6.0f);
        v.set ("a.highQ", 8.0f);
        v.set ("a.highCutHz", 1.0f);
        v.set ("a.highCut", 4.0f);
        v.set ("a.lmBoost", 5.0f);
        v.set ("a.midHz", 7.0f);
        v.set ("a.midCut", 6.0f);
        v.set ("a.hmBoost", 8.0f);
        v.set ("b.highBoost", 10.0f);   // set B differs
        auto m = makeEq (sr, v);
        const auto r = measure (sr, v, ChannelMode::LeftRight);
        double worstA = 0.0, worstB = 0.0;
        for (double hz = 20.0; hz < 0.45 * sr; hz *= 1.12)
        {
            worstA = std::max (worstA, std::abs (m->getMagnitudeDb (0, hz) - r.at (hz, 0)));
            worstB = std::max (worstB, std::abs (m->getMagnitudeDb (1, hz) - r.at (hz, 1)));
        }
        note ("sr " + std::to_string ((int) sr) + ": worst |getMagnitudeDb - measured| set A (dB)", worstA);
        note ("    set B", worstB);
        CHECK (worstA < 0.01);
        CHECK (worstB < 0.01);
        CHECK_NEAR (m->getMagnitudeDb (1, 8000.0), 11.36, 0.15);
    }
}

TEST_CASE ("peq. channel sets: Stereo uses A, M/S and L/R give channel 1 set B, the T/S twin uses B")
{
    Values v;
    v.set ("a.lowHz", 4.0f);   // 100 Hz
    v.set ("a.lowBoost", 10.0f);
    v.set ("b.highHz", 3.0f);  // 8 kHz
    v.set ("b.highBoost", 10.0f);

    const auto st = measure (kSr, v, ChannelMode::Stereo);
    CHECK_NEAR (st.at (30.0, 0), 16.21, 0.05);
    CHECK_NEAR (st.at (30.0, 1), 16.21, 0.05);
    CHECK_NEAR (st.at (8000.0, 1), 0.0, 0.1);

    for (auto mode : { ChannelMode::MidSide, ChannelMode::LeftRight })
    {
        const auto ms = measure (kSr, v, mode);
        CHECK_NEAR (ms.at (30.0, 0), 16.21, 0.05);
        CHECK_NEAR (ms.at (8000.0, 0), 0.0, 0.1);
        CHECK_NEAR (ms.at (30.0, 1), 0.0, 0.1);
        CHECK_NEAR (ms.at (8000.0, 1), 11.36, 0.15);
    }

    const auto twin = measure (kSr, v, ChannelMode::Stereo, 1);
    CHECK_NEAR (twin.at (8000.0, 0), 11.36, 0.15);
    CHECK_NEAR (twin.at (8000.0, 1), 11.36, 0.15);
    CHECK_NEAR (twin.at (30.0, 0), 0.0, 0.1);

    auto a = makeEq (kSr, v, 0);
    auto b = makeEq (kSr, v, 1);
    CHECK_NEAR (a->getMagnitudeDb (0, 30.0), 16.21, 0.05);   // instance A answers both sets
    CHECK_NEAR (a->getMagnitudeDb (1, 8000.0), 11.36, 0.15);
    CHECK_NEAR (b->getMagnitudeDb (0, 8000.0), 11.36, 0.15);   // the twin answers set B for both
    CHECK_NEAR (b->getMagnitudeDb (1, 8000.0), 11.36, 0.15);
    CHECK_NEAR (b->getMagnitudeDb (0, 30.0), 0.0, 0.05);
    CHECK_EQ (a->getLatencySamples(), b->getLatencySamples());

    // a mode switch glides channel 1 to the other set (no jump)
    Values g;
    g.set ("b.lmHz", 0.0f);
    g.set ("b.lmBoost", 10.0f);
    const auto x = pstest::sine (220.0, -12.0, 1.0, kSr);
    const size_t sw = (size_t) (0.5 * kSr);
    auto runModes = [&] (ChannelMode before, ChannelMode after) {
        auto m = makeEq (kSr, g);
        Stereo out = dual (x);
        ProcessContext ctx;
        for (size_t start = 0; start < x.size(); start += 256)
        {
            ctx.channelMode = start < sw ? before : after;
            m->setParams (g.data());
            float* ch[2] { out.l.data() + start, out.r.data() + start };
            m->process (ch, (int) std::min<size_t> (256, x.size() - start), ctx);
        }
        return out.r;
    };
    const auto glided = runModes (ChannelMode::Stereo, ChannelMode::MidSide);
    auto hard = runModes (ChannelMode::Stereo, ChannelMode::Stereo);
    const auto after = runModes (ChannelMode::MidSide, ChannelMode::MidSide);
    std::copy (after.begin() + (std::ptrdiff_t) sw, after.end(), hard.begin() + (std::ptrdiff_t) sw);
    const double ours = hfShareDb (glided, sw - 2048, kSr), spliced = hfShareDb (hard, sw - 2048, kSr);
    note ("mode switch Stereo -> M/S: HF splatter (dB re the tone), glide", ours);
    note ("    hard switch", spliced);
    CHECK (ours < spliced - 20.0);
    CHECK (ours < -70.0);
}

TEST_CASE ("peq. tube stage: mostly 2nd harmonic, level kept at -18 dBFS, nothing at drive 0")
{
    const size_t n = 32768;
    const double hz = 46.875 * 32.0;   // 1500 Hz on an exact bin of a 32768-point FFT at 48 kHz
    const size_t bin = 1024;
    double h2At10 = 0.0;
    for (float drive : { 0.0f, 2.0f, 5.0f, 10.0f })
    {
        Values v;
        v.set ("drive", drive);
        auto m = makeEq (kSr, v);
        const auto x = pstest::sine (hz, -18.0, 1.5, kSr);
        const auto y = run (*m, v, dual (x));
        const auto mag = amplitudeSpectrum (y.l, 8192, n);
        const double f = binPeak (mag, bin);
        const double h2 = db (binPeak (mag, 2 * bin) / f), h3 = db (binPeak (mag, 3 * bin) / f);
        const double level = toneGainDb (y.l, x, hz, kSr, 8192, 32000);
        note ("drive " + std::to_string ((int) drive) + ": H2 (dBc)", h2);
        note ("    H3 (dBc)", h3);
        note ("    level change of the fundamental (dB)", level);
        CHECK_NEAR (level, 0.0, 0.05);
        if (drive == 0.0f)
        {
            CHECK_EQ (maxDelayedDiff (y.l, x, kPassiveEqLatency), 0.0);
            continue;
        }
        CHECK (h2 > h3 + 12.0);
        if (drive == 10.0f)
        {
            h2At10 = h2;
            CHECK_RANGE (h2, -36.0, -28.0);
        }
    }
    CHECK (h2At10 > -36.0);

    // the stage is flat in frequency: a 30 Hz tone and a 1.5 kHz tone at -30 dBFS get the same level, no DC
    Values v;
    v.set ("drive", 10.0f);
    double gains[2] {};
    double mean = 0.0;
    for (int i = 0; i < 2; ++i)
    {
        const double f = i == 0 ? 30.0 : hz;
        auto m = makeEq (kSr, v);
        const auto tone = pstest::sine (f, -30.0, 3.0, kSr);
        const auto yb = run (*m, v, dual (tone));
        gains[i] = toneGainDb (yb.l, tone, f, kSr, (size_t) kSr, (size_t) (1.8 * kSr));
        if (i == 0)
        {
            for (size_t k = (size_t) kSr; k < yb.size(); ++k)
                mean += (double) yb.l[k];
            mean /= (double) (yb.size() - (size_t) kSr);
        }
    }
    note ("drive 10, -30 dBFS: level change at 30 Hz (dB)", gains[0]);
    note ("    at 1.5 kHz", gains[1]);
    note ("    mean of the 30 Hz output", mean);
    CHECK_NEAR (gains[0], gains[1], 0.01);
    CHECK (std::abs (gains[0]) < 0.1);
    CHECK (std::abs (mean) < 1.0e-5);
}

TEST_CASE ("peq. tube stage aliasing stays low (2x oversampled)")
{
    const size_t n = 32768;
    for (size_t bin : { (size_t) 3072, (size_t) 7168 })   // 4.5 kHz and 10.5 kHz at 48 kHz
    {
        const double hz = (double) bin * kSr / (double) n;
        Values v;
        v.set ("drive", 10.0f);
        auto m = makeEq (kSr, v);
        const auto x = pstest::sine (hz, -6.0, 1.5, kSr);
        const auto y = run (*m, v, dual (x));
        const auto mag = amplitudeSpectrum (y.l, 8192, n);
        const double f = binPeak (mag, bin);
        double worst = 0.0;
        for (size_t k = 20; k < mag.size(); ++k)
        {
            const size_t r = k % bin;
            if (r <= 3 || bin - r <= 3)
                continue;
            worst = std::max (worst, mag[k]);
        }
        note ("drive 10, -6 dBFS " + std::to_string ((int) hz) + " Hz: worst non-harmonic (dBc)", db (worst / f));
        CHECK (db (worst / f) < (hz < 6000.0 ? -90.0 : -70.0));
    }
}

TEST_CASE ("peq. drive crossfade: leaving and returning to 0 is click-free and exact again after 20 ms")
{
    for (double sr : { 44100.0, 96000.0 })
    {
        Values v;
        auto m = makeEq (sr, v);
        const auto x = pstest::sine (997.0, -6.0, 1.5, sr);
        const size_t on = (size_t) (0.4 * sr), off = (size_t) (0.9 * sr);
        const auto y = run (*m, v, dual (x), ChannelMode::Stereo, 128, [&] (size_t start, Values& vals) {
            vals.set ("drive", start >= on && start < off ? 3.0f : 0.0f);
        });
        const double steady = maxSecondDiff (y.l, (size_t) (0.1 * sr), on);
        const double fades = std::max (maxSecondDiff (y.l, on, on + (size_t) (0.05 * sr)),
                                       maxSecondDiff (y.l, off, off + (size_t) (0.05 * sr)));
        note ("sr " + std::to_string ((int) sr) + ": max second difference in the fades / steady", fades / steady);
        CHECK (fades < 1.05 * steady);
        CHECK (maxDelayedDiff (y.l, x, kPassiveEqLatency, (size_t) (0.6 * sr)) > 1.0e-4);   // the tube path is heard
        const double after = maxDelayedDiff (y.l, x, kPassiveEqLatency, off + 128 + (size_t) (0.021 * sr));
        CHECK_EQ (after, 0.0);
    }
}

TEST_CASE ("peq. a small drive change preserves treble throughout both fades")
{
    // A negligible drive must not switch the clean signal between different phase responses.
    double worstDb = 0.0;
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (double hz : { 8000.0, 12000.0, 16000.0, 20000.0 })
        {
            Values v;
            auto m = makeEq (sr, v);
            const auto x = pstest::sine (hz, -18.0, 0.3, sr);
            const size_t on = (size_t) (0.1 * sr), off = (size_t) (0.2 * sr);
            const auto y = run (*m, v, dual (x), ChannelMode::Stereo, 1, [&] (size_t start, Values& vals) {
                vals.set ("drive", start >= on && start < off ? 0.01f : 0.0f);
            });
            for (size_t at : { on, off })
                for (int ms = 0; ms <= 22; ++ms)
                {
                    const size_t from = at + (size_t) (ms * 0.001 * sr);
                    const size_t len = (size_t) (0.001 * sr);
                    double inputPower = 0.0, outputPower = 0.0;
                    for (size_t i = from; i < from + len; ++i)
                    {
                        inputPower += (double) x[i - kPassiveEqLatency] * x[i - kPassiveEqLatency];
                        outputPower += (double) y.l[i] * y.l[i];
                    }
                    worstDb = std::max (worstDb, std::abs (10.0 * std::log10 (outputPower / inputPower)));
                }
        }
    note ("largest treble level deviation during small-drive fades (dB)", worstDb);
    CHECK (worstDb < 0.01);
}

TEST_CASE ("peq. unrelated section automation cannot restart an active glide")
{
    Values v;
    v.set ("a.highHz", 3.0f);
    const auto x = pstest::sine (8000.0, -20.0, 0.15, kSr);
    auto render = [&] (bool automate) {
        auto m = makeEq (kSr, v);
        return run (*m, v, dual (x), ChannelMode::Stereo, 32, [&] (size_t start, Values& vals) {
            vals.set ("a.highBoost", start >= 2400 ? 10.0f : 0.0f);
            vals.set ("a.lowHz", automate ? (float) ((start / 32) % 5) : 0.0f);
        });
    };
    const auto reference = render (false);
    v.set ("a.highBoost", 0.0f);
    const auto automated = render (true);
    const double error = maxDelayedDiff (automated.l, reference.l, 0);
    note ("unrelated neutral frequency automation: max output difference", error);
    CHECK_EQ (error, 0.0);
}

TEST_CASE ("peq. the first drive after reset or reprepare snaps like a fresh instance")
{
    double worst = 0.0;
    for (bool reprepare : { false, true })
        for (float before : { 0.0f, 10.0f })
        {
            Values v;
            v.set ("drive", before);
            auto m = makeEq (kSr, v);
            const auto x = dual (pstest::sine (1500.0, -6.0, 0.04, kSr));
            run (*m, v, x);
            if (reprepare)
                m->prepare (kSr, 512);
            else
                m->reset();
            v.set ("drive", 10.0f - before);
            auto fresh = makeEq (kSr, v);
            const auto expected = run (*fresh, v, x);
            const auto actual = run (*m, v, x);
            worst = std::max (worst, maxDelayedDiff (actual.l, expected.l, 0));
        }
    note ("first drive after reset/reprepare: max difference from fresh instance", worst);
    CHECK_EQ (worst, 0.0);
}

TEST_CASE ("peq. parameter changes glide over 10 ms without clicks")
{
    // gain step: 0 -> 10 high boost on an 8 kHz tone, measured in 1 ms windows
    {
        Values v;
        v.set ("a.highHz", 3.0f);
        auto m = makeEq (kSr, v);
        const auto x = pstest::sine (8000.0, -20.0, 0.6, kSr);
        const size_t at = (size_t) (0.2 * kSr);
        const auto y = run (*m, v, dual (x), ChannelMode::Stereo, 64, [&] (size_t start, Values& vals) {
            vals.set ("a.highBoost", start >= at ? 10.0f : 0.0f);
        });
        const size_t w = 48;
        const double g5 = toneGainDb (y.l, x, 8000.0, kSr, at + 216, w);       // window centred 5 ms after the step
        const double g12 = toneGainDb (y.l, x, 8000.0, kSr, at + 552, w);     // 12 ms
        note ("high boost 0 -> 10: gain 5 ms after the step (dB)", g5);
        note ("    12 ms after", g12);
        CHECK_RANGE (g5, 3.0, 13.0);
        CHECK_NEAR (g12, 11.36, 0.15);
    }

    // clicks: a step of every control on a 220 Hz tone. Second difference against the steady state (<= 4, as the other
    // modules), and the HF splatter (> 3 kHz) of the transition against a hard switch (the two steady outputs spliced
    // at the step: what a click sounds like).
    struct Step { const char* id; float from, to; };
    const Step steps[] { { "a.lowBoost", 0.0f, 10.0f }, { "a.lowHz", 0.0f, 4.0f }, { "a.lowCut", 10.0f, 0.0f },
                         { "a.highBoost", 10.0f, 0.0f }, { "a.highHz", 0.0f, 6.0f }, { "a.highQ", 10.0f, 0.0f },
                         { "a.highCut", 0.0f, 10.0f }, { "a.highCutHz", 0.0f, 2.0f }, { "a.lmBoost", 0.0f, 10.0f },
                         { "a.lmHz", 4.0f, 0.0f }, { "a.midCut", 0.0f, 10.0f }, { "a.midHz", 0.0f, 10.0f },
                         { "a.hmBoost", 10.0f, 0.0f }, { "a.hmHz", 0.0f, 4.0f }, { "drive", 0.0f, 10.0f } };
    double worstRatio = 0.0, worstMargin = 1.0e9, worstSplatter = -300.0;
    const auto x = pstest::sine (220.0, -6.0, 0.8, kSr);
    const size_t at = (size_t) (0.4 * kSr);
    for (const auto& s : steps)
    {
        Values v;
        // the amounts of every section at 6, so frequency / bandwidth steps move a live curve
        for (const char* amount : { "a.lowBoost", "a.highBoost", "a.highCut", "a.lmBoost", "a.midCut", "a.hmBoost" })
            v.set (amount, 6.0f);
        v.set ("a.lowHz", 3.0f);   // 60 Hz, 300 Hz low-mid, 1 kHz mid: sections close to the 220 Hz tone
        v.set ("a.lmHz", 1.0f);
        v.set ("a.midHz", 4.0f);
        v.set ("a.highQ", 5.0f);
        auto steadyRun = [&] (float value) {
            Values w = v;
            w.set (s.id, value);
            auto m = makeEq (kSr, w);
            return run (*m, w, dual (x), ChannelMode::Stereo, 64).l;
        };
        v.set (s.id, s.from);
        auto m = makeEq (kSr, v);
        const auto y = run (*m, v, dual (x), ChannelMode::Stereo, 64, [&] (size_t start, Values& vals) {
            vals.set (s.id, start >= at ? s.to : s.from);
        }).l;
        auto hard = steadyRun (s.from);
        const auto to = steadyRun (s.to);
        std::copy (to.begin() + (std::ptrdiff_t) at, to.end(), hard.begin() + (std::ptrdiff_t) at);

        const double steady = std::max (maxSecondDiff (y, at - 4800, at), maxSecondDiff (y, at + 4800, at + 9600));
        const double ratio = maxSecondDiff (y, at, at + 4800) / steady;
        const double ours = hfShareDb (y, at - 2048, kSr), spliced = hfShareDb (hard, at - 2048, kSr);
        worstRatio = std::max (worstRatio, ratio);
        worstMargin = std::min (worstMargin, spliced - ours);
        worstSplatter = std::max (worstSplatter, ours);
        if (ratio > 4.0 || ours > spliced - 15.0 || ours > -75.0)
        {
            note (std::string ("step of ") + s.id + ": second-difference ratio", ratio);
            note ("    HF splatter (dB), glide", ours);
            note ("    hard switch", spliced);
        }
        CHECK (ratio <= 4.0);
        CHECK (ours < spliced - 15.0);
        CHECK (ours < -75.0);
    }
    note ("worst second-difference ratio (step / steady) over all controls", worstRatio);
    note ("worst HF splatter of a step (dB re the tone)", worstSplatter);
    note ("smallest margin below a hard switch (dB)", worstMargin);
}

TEST_CASE ("peq. extremes and fast automation stay stable and finite")
{
    for (double sr : { 44100.0, 96000.0 })
    {
        Values v;
        v.set ("drive", 10.0f);
        for (const char* s : { "a.", "b." })
        {
            const std::string p (s);
            for (const char* amount : { "lowBoost", "lowCut", "highBoost", "highCut", "lmBoost", "midCut", "hmBoost" })
                v.set (p + amount, 10.0f);
            v.set (p + "highHz", 6.0f);
            v.set (p + "highQ", 10.0f);
            v.set (p + "highCutHz", 2.0f);
            v.set (p + "lowHz", 0.0f);
        }
        auto m = makeEq (sr, v);
        Stereo in { pstest::whiteNoise (1.0, 3.0, sr, 7u), pstest::whiteNoise (1.0, 3.0, sr, 8u) };
        for (size_t i = (size_t) sr; i < (size_t) (1.5 * sr); ++i)
            in.l[i] = in.r[i] = (i / 50) % 2 == 0 ? 2.0f : -2.0f;   // +6 dBFS square
        pstest::Lcg lcg (99u);
        const auto out = run (*m, v, in, ChannelMode::MidSide, 37, [&] (size_t start, Values& vals) {
            if (start < (size_t) (2.0 * sr))
                return;
            for (const char* s : { "a.", "b." })
            {
                const std::string p (s);
                for (const char* amount : { "lowBoost", "lowCut", "highBoost", "highCut", "lmBoost", "midCut", "hmBoost",
                                            "highQ" })
                    vals.set (p + amount, 5.0f + 5.0f * lcg.next());
                vals.set (p + "lowHz", (float) std::floor (2.5f + 2.49f * lcg.next()));
                vals.set (p + "highHz", (float) std::floor (3.5f + 3.49f * lcg.next()));
                vals.set (p + "midHz", (float) std::floor (5.5f + 5.49f * lcg.next()));
            }
            vals.set ("drive", 5.0f + 5.0f * lcg.next());
        });
        CHECK (allFinite (out));
        double peak = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
            peak = std::max ({ peak, (double) std::abs (out.l[i]), (double) std::abs (out.r[i]) });
        note ("sr " + std::to_string ((int) sr) + ": peak output with everything at 10, full-scale input", peak);
        CHECK (peak < 100.0);
    }
}

TEST_CASE ("peq. no denormals: silence after signal decays to exact zeros, NaN input recovers")
{
    Values v;
    v.set ("drive", 4.0f);
    v.set ("a.lowHz", 0.0f);
    v.set ("a.lowBoost", 10.0f);
    v.set ("a.lmBoost", 6.0f);
    auto m = makeEq (kSr, v);
    Stereo in { pstest::concat (pstest::pinkNoise (0.5, 1.0, kSr, 3u), std::vector<float> ((size_t) (20.0 * kSr), 0.0f)),
                pstest::concat (pstest::pinkNoise (0.5, 1.0, kSr, 4u), std::vector<float> ((size_t) (20.0 * kSr), 0.0f)) };
    const auto out = run (*m, v, in);
    int subnormal = 0;
    double tail = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        subnormal += std::fpclassify (out.l[i]) == FP_SUBNORMAL ? 1 : 0;
        subnormal += std::fpclassify (out.r[i]) == FP_SUBNORMAL ? 1 : 0;
        if (i > (size_t) (15.0 * kSr))
            tail = std::max (tail, (double) std::abs (out.l[i]));
    }
    note ("subnormal output samples", subnormal);
    note ("largest output sample 14 s into the silence", tail);
    CHECK_EQ (subnormal, 0);
    CHECK_EQ (tail, 0.0);

    // A NaN is rejected before entering the filters; processing stays finite throughout.
    Stereo n2 { pstest::sine (440.0, -12.0, 1.0, kSr), pstest::sine (440.0, -12.0, 1.0, kSr) };
    n2.l[1000] = std::numeric_limits<float>::quiet_NaN();
    const auto o2 = run (*m, v, n2);
    bool finiteAfter = true;
    for (size_t i = 0; i < o2.size(); ++i)
        finiteAfter = finiteAfter && std::isfinite (o2.l[i]) && std::isfinite (o2.r[i]);
    CHECK (finiteAfter);
}

TEST_CASE ("peq. getMagnitudeDb is consistent while the audio thread changes the design")
{
    Values v;
    v.set ("a.midHz", 4.0f);
    auto m = makeEq (kSr, v);
    Values cutValues = v;
    cutValues.set ("a.midCut", 10.0f);
    const double cutDb = makeEq (kSr, cutValues)->getMagnitudeDb (0, 1000.0);   // the measured law: about -14 dB
    std::atomic<bool> stop { false };
    std::atomic<int> torn { 0 }, reads { 0 };
    std::thread reader ([&] {
        while (! stop.load())
        {
            const double g = m->getMagnitudeDb (0, 1000.0);
            if (std::abs (g) > 1.0e-9 && std::abs (g - cutDb) > 1.0e-9)   // 0 dB or the cut, nothing in between
                torn.fetch_add (1);
            reads.fetch_add (1);
        }
    });
    ProcessContext ctx;
    float buf[2][32] {};
    for (int i = 0; i < 20000; ++i)
    {
        v.set ("a.midCut", i % 2 == 0 ? 10.0f : 0.0f);
        m->setParams (v.data());
        float* ch[2] { buf[0], buf[1] };
        m->process (ch, 32, ctx);
    }
    stop.store (true);
    reader.join();
    note ("concurrent reads", reads.load());
    CHECK_EQ (torn.load(), 0);
    CHECK (reads.load() > 0);
}

TEST_CASE ("peq. no allocation in setParams / process / reset")
{
    Values v;
    auto m = makeEq (kSr, v);
    Stereo buf { pstest::pinkNoise (0.6, 1.0, kSr, 5u), pstest::pinkNoise (0.6, 1.0, kSr, 6u) };
    long long count = 0;
    {
        pstest::AllocationScope scope;
        ProcessContext ctx;
        int k = 0;
        for (size_t start = 0; start + 256 <= buf.size(); start += 256, ++k)
        {
            v.set ("a.lowBoost", (float) (k % 11));
            v.set ("b.highHz", (float) (k % 7));
            v.set ("drive", (float) (k % 3));
            ctx.channelMode = k % 4 == 0 ? ChannelMode::MidSide : ChannelMode::Stereo;
            m->setParams (v.data());
            float* ch[2] { buf.l.data() + start, buf.r.data() + start };
            m->process (ch, 256, ctx);
            if (k % 50 == 49)
                m->reset();
            (void) m->getMagnitudeDb (1, 440.0);
        }
        count = scope.count();
    }
    CHECK_EQ (count, 0);
}

TEST_CASE ("peq. cpu benchmark (96 kHz stereo, every section and drive)")
{
    Values v;
    v.set ("drive", 5.0f);
    for (const char* amount : { "a.lowBoost", "a.lowCut", "a.highBoost", "a.highCut", "a.lmBoost", "a.midCut", "a.hmBoost" })
        v.set (amount, 5.0f);
    auto m = makeEq (96000.0, v);
    const Stereo in { pstest::pinkNoise (0.5, 10.0, 96000.0, 9u), pstest::pinkNoise (0.5, 10.0, 96000.0, 10u) };
    pstest::Stopwatch sw;
    run (*m, v, in, ChannelMode::MidSide, 512);
    const double x = 10.0 / sw.seconds();
    note ("x realtime", x);
    CHECK (x > 20.0);
}

TEST_CASE ("peq. the shared section designs (panel curves) sum to getMagnitudeDb")
{
    Values v;
    v.set ("a.lowBoost", 4.0f);
    v.set ("a.lowCut", 2.0f);
    v.set ("a.highBoost", 7.0f);
    v.set ("a.highHz", 5.0f);
    v.set ("a.highCut", 6.0f);
    v.set ("a.midCut", 3.0f);
    v.set ("a.hmBoost", 9.0f);
    auto m = makeEq (44100.0, v);
    PassiveEqParams p;
    readPassiveEqParams (v.data(), p);
    double worst = 0.0;
    for (double hz = 20.0; hz < 20000.0; hz *= 1.07)
    {
        double sum = 0.0;
        for (int k = 0; k < peq::kNumSections; ++k)
            sum += peq::designSection (k, p.sets[0], 44100.0).magnitudeDb (hz, 44100.0);
        worst = std::max (worst, std::abs (sum - m->getMagnitudeDb (0, hz)));
    }
    CHECK (worst < 1.0e-9);
    CHECK (peq::isSectionActive (peq::kLowBoost, p.sets[0]));
    CHECK (! peq::isSectionActive (peq::kLowMid, p.sets[0]));
    // the markers: a low shelf has half its DC gain (dB) at sectionHz, a peak its largest |dB|
    auto secDb = [&] (int k, const PassiveEqSetParams& set, double hz) {
        return peq::designSection (k, set, 44100.0).magnitudeDb (hz, 44100.0);
    };
    for (int k : { (int) peq::kLowBoost, (int) peq::kLowCut })
    {
        PassiveEqSetParams one = p.sets[0];
        (k == peq::kLowBoost ? one.lowCut : one.lowBoost) = 0.0f;   // the section alone
        const double f = peq::sectionHz (k, one, 44100.0);
        CHECK_NEAR (secDb (k, one, f), 0.5 * secDb (k, one, 1.0), 0.02);
    }
    for (int k : { (int) peq::kHighBoost, (int) peq::kMidCut, (int) peq::kHighMid })   // (matched designs: +-0.01 dB)
    {
        const double f = peq::sectionHz (k, p.sets[0], 44100.0);
        note ("section " + std::to_string (k) + ": dB at sectionHz minus 3 % below", secDb (k, p.sets[0], f) - secDb (k, p.sets[0], f / 1.03));
        note ("    minus 3 % above", secDb (k, p.sets[0], f) - secDb (k, p.sets[0], f * 1.03));
        CHECK (std::abs (secDb (k, p.sets[0], f)) > std::abs (secDb (k, p.sets[0], 1.03 * f)) - 0.01);
        CHECK (std::abs (secDb (k, p.sets[0], f)) > std::abs (secDb (k, p.sets[0], f / 1.03)) - 0.01);
    }
    // section 4 carries the high boost's upper skirt even with the high cut at 0
    PassiveEqSetParams tail = p.sets[0];
    tail.highCut = 0.0f;
    tail.highBoost = 10.0f;
    CHECK (! peq::isSectionActive (peq::kHighCut, tail));
    CHECK (secDb (peq::kHighCut, tail, 15000.0) < -0.3);
    tail.highBoost = 0.0f;
    const auto unity = peq::designSection (peq::kHighCut, tail, 44100.0);
    CHECK (unity.b0 == 1.0);
    CHECK (unity.b1 == unity.a1);
    CHECK (unity.b2 == unity.a2);
    p.sets[1].highHzIndex = 6;   // 16 kHz: its 21 kHz centre, clamped to 0.49 x the rate at 32 kHz
    p.sets[1].highBoost = 5.0f;
    CHECK_NEAR (peq::sectionHz (peq::kHighBoost, p.sets[1], 32000.0), 0.49 * 32000.0, 1.0e-9);
    CHECK_RANGE (peq::sectionHz (peq::kHighBoost, p.sets[1], 44100.0), 20900.0, 21100.0);
}

TEST_CASE ("peq. corrupt samples are replaced without poisoning audio histories")
{
    for (double rate : { 44100.0, 96000.0 })
        for (int variant = 0; variant < 2; ++variant)
            for (int block : { 1, 17, 512 })
            {
                Values v;
                v.set ("a.lowBoost", 5.0f);
                v.set ("drive", (float) variant * 10.0f);
                auto m = makeEq (rate, v);
                auto reference = makeEq (rate, v);
                Stereo damaged = dual (pstest::sine (997.0, -14.0, 4096.0 / rate, rate));
                auto clean = damaged;
                const float bad[] { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max() };
                for (size_t i = 0; i < 4; ++i)
                {
                    damaged.l[63 + i * 512] = bad[i];
                    clean.l[63 + i * 512] = 0.0f;
                }

                const auto out = run (*m, v, damaged, ChannelMode::Stereo, block);
                const auto expected = run (*reference, v, clean, ChannelMode::Stereo, block);
                bool finite = true;
                double error = 0.0;
                for (size_t i = 0; i < out.size(); ++i)
                {
                    finite = finite && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
                    error = std::max (error, std::abs ((double) out.l[i] - expected.l[i]));
                    error = std::max (error, std::abs ((double) out.r[i] - expected.r[i]));
                }
                CHECK (finite);
                CHECK (error < 1.0e-7);
            }
}

TEST_CASE ("peq. tiny low cut changes preserve an active boost during the glide")
{
    double worst = 0.0;
    for (double sr : { 22050.0, 44100.0, 192000.0 })
        for (float cut : { 0.000101f, 0.001f, 0.01f })
            for (bool reverse : { false, true })
            {
                Values v;
                v.set ("a.lowBoost", 10.0f);
                v.set ("a.lowCut", reverse ? cut : 0.0f);
                auto m = makeEq (sr, v);
                const auto x = pstest::sine (100.0, -20.0, 0.15, sr);
                const auto steady = run (*m, v, dual (x), ChannelMode::Stereo, 1).l;
                m = makeEq (sr, v);
                const size_t at = (size_t) (sr * 0.08);
                const auto y = run (*m, v, dual (x), ChannelMode::Stereo, 1, [&] (size_t start, Values& vals) {
                    if (start == at)
                        vals.set ("a.lowCut", reverse ? 0.0f : cut);
                }).l;
                double error = 0.0;
                for (size_t i = at; i < y.size(); ++i)
                    error = std::max (error, std::abs ((double) y[i] - steady[i]));
                worst = std::max (worst, error);
                if (error > 0.002)
                    note ("tiny cut transient at " + std::to_string ((int) sr) + " cut " + std::to_string (cut)
                          + (reverse ? " off" : " on"), error);
            }
    note ("worst tiny low cut transient (100 Hz, -20 dBFS input)", worst);
    CHECK (worst < 0.002);
}

TEST_CASE ("peq. section targets remain acceptable at every position and extreme amount")
{
    double margin = 1.0;
    int count = 0;
    for (double sr : { 22050.0, 32000.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int pos = 0; pos < 11; ++pos)
            for (float a : { 0.0f, 0.000099f, 0.000101f, 0.001f, 0.01f, 0.1f, 1.0f, 2.5f, 5.0f, 7.5f, 10.0f })
                for (float b : { 0.0f, 0.000101f, 0.01f, 5.0f, 10.0f })
                {
                    PassiveEqSetParams p;
                    p.lowHzIndex = pos % 5;
                    p.highHzIndex = pos % 7;
                    p.highCutHzIndex = pos % 3;
                    p.lowMidHzIndex = pos % 5;
                    p.midHzIndex = pos;
                    p.highMidHzIndex = pos % 5;
                    p.lowBoost = p.highBoost = p.lowMidBoost = p.midCut = p.highMidBoost = a;
                    p.lowCut = p.highCut = p.highQ = b;
                    for (int k = 0; k < peq::kNumSections; ++k)
                    {
                        const auto c = peq::designSection (k, p, sr);
                        const double m = std::min ({ 1.0 + c.a1 + c.a2, 1.0 - c.a1 + c.a2, 1.0 - c.a2 });
                        REQUIRE (std::isfinite (c.b0) && std::isfinite (c.b1) && std::isfinite (c.b2));
                        REQUIRE (m > 1.0e-30);
                        margin = std::min (margin, m);
                        ++count;
                    }
                }
    note ("accepted section designs", count);
    note ("minimum denominator stability margin", margin);
}

TEST_CASE ("peq. tiny high shelf changes preserve the other shelf during the glide")
{
    double worst = 0.0;
    for (double sr : { 22050.0, 32000.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int pos : { 0, 2 })
            for (bool changeBoost : { false, true })
                for (float amount : { 0.000101f, 0.001f, 0.01f })
                    for (bool reverse : { false, true })
                    {
                        Values v;
                        const char* moving = changeBoost ? "a.highBoost" : "a.highCut";
                        v.set (changeBoost ? "a.highCut" : "a.highBoost", 10.0f);
                        v.set ("a.highCutHz", (float) pos);
                        v.set (moving, reverse ? amount : 0.0f);
                        auto m = makeEq (sr, v);
                        const auto x = pstest::sine (1000.0, -20.0, 0.15, sr);
                        const auto steady = run (*m, v, dual (x), ChannelMode::Stereo, 1).l;
                        m = makeEq (sr, v);
                        const size_t at = (size_t) (sr * 0.08);
                        const auto y = run (*m, v, dual (x), ChannelMode::Stereo, 1, [&] (size_t start, Values& vals) {
                            if (start == at)
                                vals.set (moving, reverse ? 0.0f : amount);
                        }).l;
                        double error = 0.0;
                        for (size_t i = at; i < y.size(); ++i)
                            error = std::max (error, std::abs ((double) y[i] - steady[i]));
                        worst = std::max (worst, error);
                    }
    note ("worst tiny shelf transient (1 kHz, -20 dBFS input)", worst);
    CHECK (worst < 0.0001);

    // The denominator must tend to the same SVF coordinates at zero, even when the other shelf is audible.
    double poleJump = 0.0;
    for (double sr : { 22050.0, 32000.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int high = 0; high < 7; ++high)
            for (int cut = 0; cut < 3; ++cut)
                for (bool changeBoost : { false, true })
                {
                    PassiveEqSetParams p;
                    p.highHzIndex = high;
                    p.highCutHzIndex = cut;
                    (changeBoost ? p.highCut : p.highBoost) = 10.0f;
                    const auto off = peq::designSection (peq::kHighCut, p, sr);
                    (changeBoost ? p.highBoost : p.highCut) = 0.000101f;
                    const auto on = peq::designSection (peq::kHighCut, p, sr);
                    poleJump = std::max ({ poleJump, std::abs (off.a1 - on.a1), std::abs (off.a2 - on.a2) });
                }
    note ("largest section 4 pole coefficient change at zero", poleJump);
    CHECK (poleJump < 0.00001);
}

TEST_CASE ("peq. shelf markers follow active amounts below one and the bilinear frequency map")
{
    double worst = 0.0;
    int checked = 0;
    for (double sr : { 22050.0, 32000.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int pos = 0; pos < 5; ++pos)
            for (float amount : { 0.01f, 0.25f, 0.5f, 0.9f, 1.0f, 5.0f, 10.0f })
                for (int section : { (int) peq::kLowBoost, (int) peq::kLowCut })
                {
                    PassiveEqSetParams p;
                    p.lowHzIndex = pos;
                    (section == peq::kLowBoost ? p.lowBoost : p.lowCut) = amount;
                    const double hz = peq::sectionHz (section, p, sr);
                    if (hz >= 0.49 * sr)
                        continue;   // the documented display clamp
                    const auto c = peq::designSection (section, p, sr);
                    worst = std::max (worst, std::abs (c.magnitudeDb (hz, sr) - 0.5 * c.magnitudeDb (0.0, sr)));
                    ++checked;
                }
    note ("low shelf half-gain markers checked", checked);
    note ("worst marker half-gain error (dB)", worst);
    CHECK (worst < 1.0e-7);
    // High cut's analog centre at amount 0.25, position 5 kHz: p / sqrt(G), from the measured law.
    PassiveEqSetParams p;
    p.highCutHzIndex = 0;
    p.highCut = 0.25f;
    const double pole = 1987.0 * std::pow (0.58430, (0.25 - 5.0) / 5.0);
    const double gain = 1.0 - 0.925014 * std::pow (0.25 / 10.0, 1.2312);
    CHECK_NEAR (peq::sectionHz (peq::kHighCut, p, 44100.0), pole / std::sqrt (gain), 1.0e-8);
}

TEST_CASE ("peq. inactive high shelf frequency changes leave the audible shelf unchanged")
{
    double worst = 0.0;
    for (double sr : { 22050.0, 44100.0, 192000.0 })
        for (bool moveBoost : { false, true })
        {
            Values v;
            const char* moving = moveBoost ? "a.highHz" : "a.highCutHz";
            v.set (moveBoost ? "a.highCut" : "a.highBoost", 10.0f);
            v.set (moving, 0.0f);
            auto m = makeEq (sr, v);
            const auto x = pstest::sine (1000.0, -20.0, 0.15, sr);
            const auto steady = run (*m, v, dual (x), ChannelMode::Stereo, 1).l;
            m = makeEq (sr, v);
            const size_t at = (size_t) (sr * 0.08);
            const auto y = run (*m, v, dual (x), ChannelMode::Stereo, 1, [&] (size_t start, Values& vals) {
                if (start == at)
                    vals.set (moving, moveBoost ? 6.0f : 2.0f);
            }).l;
            for (size_t i = at; i < y.size(); ++i)
                worst = std::max (worst, std::abs ((double) y[i] - steady[i]));
        }
    note ("inactive high shelf frequency change residual", worst);
    CHECK (worst < 1.0e-7);
}

TEST_CASE ("peq. inactive shelf automation cannot restart the other shelf glide")
{
    const auto x = pstest::sine (8000.0, -20.0, 0.15, kSr);
    double worst = 0.0;
    for (bool moveBoost : { false, true })
    {
        auto render = [&] (bool automate) {
            Values v;
            const char* moving = moveBoost ? "a.highHz" : "a.highCutHz";
            const char* active = moveBoost ? "a.highCut" : "a.highBoost";
            auto m = makeEq (kSr, v);
            return run (*m, v, dual (x), ChannelMode::Stereo, 32, [&] (size_t start, Values& vals) {
                vals.set (active, start >= 2400 ? 10.0f : 0.0f);
                vals.set (moving, automate ? (float) ((start / 32) % (moveBoost ? 7 : 3)) : 0.0f);
            });
        };
        const auto reference = render (false), automated = render (true);
        worst = std::max (worst, maxDelayedDiff (automated.l, reference.l, 0));
    }
    note ("unrelated shelf automation during glide: max output difference", worst);
    CHECK_EQ (worst, 0.0);
}
