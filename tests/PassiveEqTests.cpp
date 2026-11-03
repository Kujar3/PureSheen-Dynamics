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
        v.set ("a.highBw", (float) (k % 11));
        v.set ("b.midHz", (float) (k % 11));
        v.set ("a.highCutHz", (float) (k % 3));
    });
    const double d = std::max (maxDelayedDiff (out.l, in.l, 3), maxDelayedDiff (out.r, in.r, 3));
    note ("max |out - in delayed| with frequencies moving at amount 0", d);
    CHECK_EQ (d, 0.0);
}

TEST_CASE ("peq. latency: exact neutral impulse delay and driven low-frequency delay at 44.1 / 48 / 96 kHz")
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
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

TEST_CASE ("peq. low boost and low cut: shelves, and both together give a boost with a dip above it")
{
    Values boost;
    boost.set ("a.lowHz", 3.0f);   // 60 Hz
    boost.set ("a.lowBoost", 10.0f);
    const auto rb = measure (kSr, boost);
    note ("low boost 10 at 60 Hz: dB at 10 Hz", rb.at (10.0));
    note ("    at 30 Hz", rb.at (30.0));
    note ("    at 60 Hz", rb.at (60.0));
    note ("    at 200 Hz", rb.at (200.0));
    CHECK_NEAR (rb.at (10.0), 13.5, 0.5);
    CHECK_NEAR (rb.at (60.0), 6.75, 0.3);   // half the shelf at the corner
    CHECK_RANGE (rb.at (30.0), 13.5, 15.0);   // the small resonant bump
    CHECK_RANGE (rb.at (1000.0), -0.1, 0.1);

    // half the knob = half the dB (linear knob law until fitted)
    boost.set ("a.lowBoost", 5.0f);
    CHECK_NEAR (measure (kSr, boost).at (10.0), 6.75, 0.3);

    Values cut;
    cut.set ("a.lowHz", 3.0f);
    cut.set ("a.lowCut", 10.0f);
    const auto rc = measure (kSr, cut);
    note ("low cut 10 at 60 Hz: dB at 10 Hz", rc.at (10.0));
    note ("    at 96 Hz (its corner)", rc.at (96.0));
    CHECK_NEAR (rc.at (5.0), -17.5, 0.5);
    CHECK_NEAR (rc.at (96.0), -8.75, 0.4);
    CHECK_RANGE (rc.at (2000.0), -0.1, 0.1);

    Values both;
    both.set ("a.lowHz", 3.0f);
    both.set ("a.lowBoost", 10.0f);
    both.set ("a.lowCut", 5.0f);
    const auto r2 = measure (kSr, both);
    note ("boost 10 + cut 5 at 60 Hz: dB at 30 Hz", r2.at (30.0));
    note ("    at 100 Hz", r2.at (100.0));
    CHECK (r2.at (30.0) > 4.0);
    CHECK (r2.at (100.0) < -2.5);
    CHECK (r2.at (100.0) > -6.0);
    CHECK_RANGE (r2.at (1000.0), -0.3, 0.1);

    // the frequency selector moves the shelf: 20 Hz vs 100 Hz at boost 10, gain at the selected frequency
    for (int i = 0; i < 5; ++i)
    {
        boost.set ("a.lowHz", (float) i);
        boost.set ("a.lowBoost", 10.0f);
        CHECK_NEAR (measure (kSr, boost).at (kPassiveEqLowHz[(size_t) i]), 6.75, 0.3);
    }
}

TEST_CASE ("peq. high boost: gain, bandwidth knob and the boost / bandwidth interaction")
{
    Values v;
    v.set ("a.highHz", 3.0f);   // 8 kHz
    v.set ("a.highBoost", 10.0f);
    v.set ("a.highBw", 5.0f);
    const auto r = measure (kSr, v);
    note ("high boost 10 at 8 kHz: dB at 8 kHz", r.at (8000.0));
    CHECK_NEAR (r.at (8000.0), 16.0, 0.3);
    CHECK_RANGE (r.at (500.0), -0.1, 0.3);

    v.set ("a.highBw", 0.0f);
    const double sharp = halfGainWidthOct (measure (96000.0, v), 8000.0);
    v.set ("a.highBw", 10.0f);
    const double broad = halfGainWidthOct (measure (96000.0, v), 8000.0);
    note ("half-gain width at 96 kHz (oct), bandwidth 0 (sharp)", sharp);
    note ("    bandwidth 10 (broad)", broad);
    CHECK (broad > 3.0 * sharp);

    // the peak narrows as the boost rises (passive circuit interaction)
    v.set ("a.highBw", 5.0f);
    v.set ("a.highBoost", 3.0f);
    const double w3 = halfGainWidthOct (measure (kSr, v), 8000.0);
    v.set ("a.highBoost", 10.0f);
    const double w10 = halfGainWidthOct (measure (kSr, v), 8000.0);
    note ("half-gain width (oct), boost 3", w3);
    note ("    boost 10", w10);
    CHECK (w10 < 0.9 * w3);

    // every frequency step, at 44.1 kHz too (16 kHz near Nyquist: matched design, no cramping)
    for (double sr : { 44100.0, 96000.0 })
        for (int i = 0; i < 7; ++i)
        {
            v.set ("a.highHz", (float) i);
            const double g = measure (sr, v).at (kPassiveEqHighHz[(size_t) i]);
            if (i == 6)
                note ("sr " + std::to_string ((int) sr) + ": boost 10 at 16 kHz, dB there", g);
            CHECK_NEAR (g, 16.0, i == 6 && sr < 48000.0 ? 1.0 : 0.4);
        }
}

TEST_CASE ("peq. high cut: high shelf at 5 / 10 / 20 kHz")
{
    Values v;
    v.set ("a.highCutHz", 0.0f);   // 5 kHz
    v.set ("a.highCut", 10.0f);
    const auto r = measure (kSr, v);
    note ("high cut 10 at 5 kHz: dB at 5 kHz", r.at (5000.0));
    note ("    at 20 kHz", r.at (20000.0));
    CHECK_NEAR (r.at (5000.0), -8.0, 0.4);
    CHECK (r.at (20000.0) < -13.0);
    CHECK_RANGE (r.at (300.0), -0.2, 0.05);

    v.set ("a.highCut", 5.0f);
    CHECK_NEAR (measure (kSr, v).at (5000.0), -4.0, 0.3);

    // 20 kHz at 44.1 kHz is clamped to 0.45 x the rate: still a gentle cut, nothing above 0 dB
    v.set ("a.highCutHz", 2.0f);
    v.set ("a.highCut", 10.0f);
    const auto r44 = measure (44100.0, v);
    note ("high cut 10 at 20 kHz (44.1 kHz): dB at 10 kHz", r44.at (10000.0));
    note ("    at 19.8 kHz", r44.at (19845.0));
    CHECK (r44.at (19845.0) < -6.0);
    CHECK_RANGE (r44.at (2000.0), -0.6, 0.05);
}

TEST_CASE ("peq. midrange section: low-mid / high-mid boosts and the mid cut with proportional Q")
{
    struct Case { const char* amount; const char* hz; int index; double centre, gain; };
    const Case cases[] { { "a.lmBoost", "a.lmHz", 1, 300.0, 10.0 }, { "a.midCut", "a.midHz", 4, 1000.0, -10.0 },
                         { "a.hmBoost", "a.hmHz", 2, 3000.0, 10.0 } };
    for (const auto& c : cases)
    {
        Values v;
        v.set (c.hz, (float) c.index);
        v.set (c.amount, 10.0f);
        const auto r = measure (kSr, v);
        CHECK_NEAR (r.at (c.centre), c.gain, 0.2);
        const double w10 = halfGainWidthOct (r, c.centre);
        v.set (c.amount, 3.0f);
        const auto r3 = measure (kSr, v);
        CHECK_NEAR (r3.at (c.centre), 0.3 * c.gain, 0.15);
        const double w3 = halfGainWidthOct (r3, c.centre);
        note (std::string (c.amount) + ": half-gain width (oct) at 3", w3);
        note ("    at 10", w10);
        CHECK (w10 < 0.8 * w3);   // proportional Q: narrower as the amount rises
    }

    // every mid-cut step lands on its frequency
    Values v;
    v.set ("a.midCut", 10.0f);
    for (int i = 0; i < 11; ++i)
    {
        v.set ("a.midHz", (float) i);
        CHECK_NEAR (measure (kSr, v).at (kPassiveEqMidHz[(size_t) i]), -10.0, 0.25);
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
        v.set ("a.highBw", 2.0f);
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
        CHECK_NEAR (m->getMagnitudeDb (1, 8000.0), 16.0, 0.3);
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
    CHECK_NEAR (st.at (30.0, 0), 13.5, 1.0);
    CHECK_NEAR (st.at (30.0, 1), 13.5, 1.0);
    CHECK_NEAR (st.at (8000.0, 1), 0.0, 0.1);

    for (auto mode : { ChannelMode::MidSide, ChannelMode::LeftRight })
    {
        const auto ms = measure (kSr, v, mode);
        CHECK_NEAR (ms.at (30.0, 0), 13.5, 1.0);
        CHECK_NEAR (ms.at (8000.0, 0), 0.0, 0.1);
        CHECK_NEAR (ms.at (30.0, 1), 0.0, 0.1);
        CHECK_NEAR (ms.at (8000.0, 1), 16.0, 0.3);
    }

    const auto twin = measure (kSr, v, ChannelMode::Stereo, 1);
    CHECK_NEAR (twin.at (8000.0, 0), 16.0, 0.3);
    CHECK_NEAR (twin.at (8000.0, 1), 16.0, 0.3);
    CHECK_NEAR (twin.at (30.0, 0), 0.0, 0.1);

    auto a = makeEq (kSr, v, 0);
    auto b = makeEq (kSr, v, 1);
    CHECK_NEAR (a->getMagnitudeDb (0, 30.0), 13.5, 1.0);   // instance A answers both sets
    CHECK_NEAR (a->getMagnitudeDb (1, 8000.0), 16.0, 0.3);
    CHECK_NEAR (b->getMagnitudeDb (0, 8000.0), 16.0, 0.3);   // the twin answers set B for both
    CHECK_NEAR (b->getMagnitudeDb (1, 8000.0), 16.0, 0.3);
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
        CHECK_NEAR (g12, 16.0, 0.3);
    }

    // clicks: a step of every control on a 220 Hz tone. Second difference against the steady state (<= 4, as the other
    // modules), and the HF splatter (> 3 kHz) of the transition against a hard switch (the two steady outputs spliced
    // at the step: what a click sounds like).
    struct Step { const char* id; float from, to; };
    const Step steps[] { { "a.lowBoost", 0.0f, 10.0f }, { "a.lowHz", 0.0f, 4.0f }, { "a.lowCut", 10.0f, 0.0f },
                         { "a.highBoost", 10.0f, 0.0f }, { "a.highHz", 0.0f, 6.0f }, { "a.highBw", 0.0f, 10.0f },
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
            v.set (p + "highBw", 0.0f);
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
                                            "highBw" })
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
    std::atomic<bool> stop { false };
    std::atomic<int> torn { 0 }, reads { 0 };
    std::thread reader ([&] {
        while (! stop.load())
        {
            const double g = m->getMagnitudeDb (0, 1000.0);
            if (std::abs (g) > 1.0e-6 && std::abs (g + 10.0) > 0.25)   // 0 dB or the -10 dB cut, nothing in between
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
    CHECK_NEAR (peq::sectionHz (peq::kLowCut, p.sets[0], 44100.0), 60.0 * 1.6, 1.0e-9);
    p.sets[1].highCutHzIndex = 2;   // 20 kHz: clamped to 0.45 x the rate
    CHECK_NEAR (peq::sectionHz (peq::kHighCut, p.sets[1], 44100.0), 0.45 * 44100.0, 1.0e-9);
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
