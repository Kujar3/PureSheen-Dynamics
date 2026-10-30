#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/BassShaper.h"
#include "dsp/Quadrature.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Bass Shaper (dsp/BassShaper.cpp). Prefix "bass.".
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
        const int i = paramIndex ("bass." + id);
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

std::unique_ptr<BassShaper> makeShaper (double sr, const Values& vals, int maxBlock = 512)
{
    auto m = std::make_unique<BassShaper>();
    m->prepare (sr, maxBlock);
    m->setParams (vals.data());
    return m;
}

using BlockHook = std::function<void (size_t, Values&, BassShaper&)>;

Stereo run (BassShaper& m, Values& vals, const Stereo& in, int block = 512, const BlockHook& onBlock = {})
{
    Stereo out = in;
    ProcessContext ctx;
    for (size_t start = 0; start < in.size(); start += (size_t) block)
    {
        if (onBlock)
            onBlock (start, vals, m);
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

// Level change (dB) of a tone between the delayed input and the output, over [from, from + len).
double toneGainDb (const std::vector<float>& out, const std::vector<float>& in, int latency, double hz, double sr,
                   size_t from, size_t len)
{
    std::vector<float> inD (in.size(), 0.0f);
    for (size_t i = (size_t) latency; i < in.size(); ++i)
        inD[i] = in[i - (size_t) latency];
    return db (std::abs (pstest::toneOf (out, from, len, hz, sr))) - db (std::abs (pstest::toneOf (inD, from, len, hz, sr)));
}

double rmsDb (const std::vector<float>& x, size_t from, size_t len) { return db (pstest::rmsOf (x, from, len)); }

// Kick: 55 Hz bursts, exponential decay 80 ms, every `period` s, peak `peakDb`.
std::vector<float> kicks (double seconds, double sr, double peakDb = -6.0, double period = 0.5, double hz = 55.0,
                          double decayMs = 80.0)
{
    std::vector<float> x ((size_t) std::llround (seconds * sr), 0.0f);
    const double a = pstest::dbfsToAmplitude (peakDb);
    const auto per = (size_t) std::llround (period * sr);
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) (i % per) / sr;
        x[i] = (float) (a * std::exp (-t / (decayMs * 0.001)) * std::sin (2.0 * kPi * hz * t));
    }
    return x;
}

// Bilinear LR4 low pass (the Butterworth 2 squared the split implements) at hz.
cd lr4Low (double hz, double fc, double sr)
{
    const double g = std::tan (kPi * fc / sr);
    const cd z = std::polar (1.0, 2.0 * kPi * hz / sr);
    const cd s = (z - 1.0) / ((z + 1.0) * g);
    const cd lp = 1.0 / (s * s + std::sqrt (2.0) * s + 1.0);
    return lp * lp;
}

cd lr4AllPass (double hz, double fc, double sr)
{
    const double g = std::tan (kPi * fc / sr);
    const cd z = std::polar (1.0, 2.0 * kPi * hz / sr);
    const cd s = (z - 1.0) / ((z + 1.0) * g);
    return (s * s - std::sqrt (2.0) * s + 1.0) / (s * s + std::sqrt (2.0) * s + 1.0);
}

// Hann-windowed magnitude spectrum (amplitude per bin) of x[from .. from + n), n a power of two.
std::vector<double> amplitudeSpectrum (const std::vector<float>& x, size_t from, size_t n)
{
    std::vector<cd> a (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double wv = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) n);
        a[i] = (double) x[from + i] * wv;
    }
    pstest::fftInPlace (a);
    std::vector<double> mag (n / 2);
    for (size_t k = 0; k < n / 2; ++k)
        mag[k] = 4.0 * std::abs (a[k]) / (double) n;   // Hann coherent gain 0.5
    return mag;
}

// Largest bin (dBFS) that is not within +-guard bins of a multiple of `fundamentalBin` (and above `minBin`).
double worstNonHarmonicDb (const std::vector<double>& mag, size_t fundamentalBin, size_t guard, size_t minBin,
                           size_t* where = nullptr)
{
    double worst = 0.0;
    for (size_t k = minBin; k < mag.size(); ++k)
    {
        const size_t r = k % fundamentalBin;
        if (r <= guard || fundamentalBin - r <= guard)
            continue;
        if (mag[k] > worst)
        {
            worst = mag[k];
            if (where != nullptr)
                *where = k;
        }
    }
    return db (worst);
}

double maxSecondDiff (const std::vector<float>& y, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = std::max<size_t> (from, 2); i < to && i < y.size(); ++i)
        m = std::max (m, std::abs ((double) y[i] - 2.0 * (double) y[i - 1] + (double) y[i - 2]));
    return m;
}
} // namespace

//==============================================================================
TEST_CASE ("bass. neutral defaults are an exact delay, also after a balance excursion")
{
    Values vals;
    auto m = makeShaper (kSr, vals);
    const int lat = m->getLatencySamples();
    CHECK_EQ (lat, 125);

    const Stereo in { pstest::pinkNoise (0.7, 10.0, kSr, 11u), pstest::pinkNoise (0.7, 10.0, kSr, 12u) };
    const auto out = run (*m, vals, in);
    const double d1 = std::max (maxDelayedDiff (out.l, in.l, lat), maxDelayedDiff (out.r, in.r, lat));
    note ("max |out - in delayed| (defaults, 10 s pink)", d1);
    CHECK_EQ (d1, 0.0);
    for (int i = 0; i < lat; ++i)
        CHECK_EQ (out.l[(size_t) i], 0.0f);

    // balance +3 for 0.5 s, then back to 0: exact again after hold (250 ms) + fade (20 ms)
    Values v2;
    auto m2 = makeShaper (kSr, v2);
    const size_t s1 = (size_t) (1.0 * kSr), s2 = (size_t) (1.5 * kSr);
    const auto out2 = run (*m2, v2, in, 512, [&] (size_t start, Values& v, BassShaper&) {
        v.set ("balance", start >= s1 && start < s2 ? 3.0f : 0.0f);
    });
    const double during = maxDelayedDiff (out2.l, in.l, lat, s1 + 4800) ;
    // settle (two-stage smoother, < 150 ms to 1e-9) + neutral hold (250 ms) + transition (20 ms + glide < 0.32 s
    // + 10 ms)
    const double after = std::max (maxDelayedDiff (out2.l, in.l, lat, s2 + (size_t) (0.8 * kSr)),
                                   maxDelayedDiff (out2.r, in.r, lat, s2 + (size_t) (0.8 * kSr)));
    note ("max diff while balance +3 (expected > 0)", during);
    note ("max diff from 800 ms after balance back to 0", after);
    CHECK (during > 1.0e-3);
    CHECK_EQ (after, 0.0);

    // auto gain on alone keeps the exact delay
    Values v3;
    v3.set ("autoGain", 1.0f);
    auto m3 = makeShaper (kSr, v3);
    const auto out3 = run (*m3, v3, in);
    const double d3 = maxDelayedDiff (out3.l, in.l, lat);
    note ("max diff, auto gain on and everything else neutral", d3);
    CHECK_EQ (d3, 0.0);
}

TEST_CASE ("bass. leaving and returning to the neutral passthrough keeps the level (no dip at the cutoff)")
{
    for (float cutoff : { 65.0f, 120.0f, 300.0f })
        for (double hz : { 0.5 * cutoff, (double) cutoff, 2.0 * cutoff })
        {
            Values vals;
            vals.set ("cutoff", cutoff);
            auto m = makeShaper (kSr, vals);
            const int lat = m->getLatencySamples();
            const auto x = pstest::sine (hz, -12.0, 3.5, kSr);
            const auto on = (size_t) (1.0 * kSr), off = (size_t) (2.0 * kSr);
            const auto out = run (*m, vals, dual (x), 64, [&] (size_t start, Values& v, BassShaper&) {
                v.set ("balance", start >= on && start < off ? 0.01f : 0.0f);   // engage, then back to neutral
            });
            // amplitude of every half cycle of the output (its sample peak between sign changes; exact to 0.01 dB for
            // these tones) against the input amplitude, from 0.9 s to the end. Phase-insensitive: the transition
            // rotates the phase on purpose.
            const double amp = pstest::dbfsToAmplitude (-12.0);
            double lo = 0.0, hi = -300.0, segPeak = 0.0;
            bool started = false;   // skip the partial half cycle at the start
            for (size_t t = (size_t) (0.9 * kSr); t < out.size(); ++t)
            {
                if ((out.l[t] >= 0.0f) != (out.l[t - 1] >= 0.0f))
                {
                    if (started)
                    {
                        lo = std::min (lo, db (segPeak / amp));
                        hi = std::max (hi, db (segPeak / amp));
                    }
                    segPeak = 0.0;
                    started = true;
                }
                segPeak = std::max (segPeak, (double) std::abs (out.l[t]));
            }
            const double exact = maxDelayedDiff (out.l, x, lat, off + (size_t) (0.8 * kSr));
            note ("cutoff " + std::to_string ((int) cutoff) + ", " + std::to_string ((int) hz)
                      + " Hz: level range during engage / disengage (dB) min", lo);
            note ("    max", hi);
            note ("    max diff vs exact delay 800 ms after returning to neutral", exact);
            // the all-pass glide is a (slow) time-varying phase shift: +-0.65 dB at most (a plain crossfade with the
            // all-pass would null a tone at the cutoff and dip 2.75 dB an octave either side)
            CHECK (lo > -0.75);
            CHECK (hi < 0.75);
            CHECK_EQ (exact, 0.0);
        }
}

TEST_CASE ("bass. latency equals the measured impulse delay at 44.1 / 48 / 96 kHz and never changes")
{
    const double rates[] { 44100.0, 48000.0, 96000.0 };
    const int expected[] { 115, 125, 245 };
    for (int r = 0; r < 3; ++r)
    {
        const double sr = rates[r];
        for (int active = 0; active < 2; ++active)
        {
            Values vals;
            if (active == 1)
            {
                vals.set ("balance", 3.0f);
                vals.set ("punch", 3.0f);
                vals.set ("sus.on", 1.0f);
                vals.set ("sus.char", 50.0f);
                vals.set ("peak.on", 1.0f);
                vals.set ("peak.char", 50.0f);
                vals.set ("autoGain", 1.0f);
            }
            auto m = makeShaper (sr, vals);
            const int lat = m->getLatencySamples();
            Stereo in { std::vector<float> ((size_t) sr, 0.0f), std::vector<float> ((size_t) sr, 0.0f) };
            in.l[1000] = in.r[1000] = 0.1f;
            const auto out = run (*m, vals, in, 256);
            size_t arg = 0;
            for (size_t i = 0; i < out.size(); ++i)
                if (std::abs (out.l[i]) > std::abs (out.l[arg]))
                    arg = i;
            const int measured = (int) arg - 1000;
            note (std::string (active ? "active  " : "neutral ") + "latency @" + std::to_string ((int) sr)
                      + " reported", lat);
            note ("    measured impulse delay", measured);
            CHECK_EQ (lat, expected[r]);
            CHECK_EQ (measured, lat);
            if (active == 0)
                CHECK_EQ (out.l[arg], 0.1f);
        }
    }

    // random parameter combinations: the latency never moves
    Values vals;
    auto m = makeShaper (kSr, vals);
    pstest::Lcg lcg (77u);
    bool constant = true;
    for (int k = 0; k < 400; ++k)
    {
        for (const auto& s : paramSpecs())
        {
            if (s.id.rfind ("bass.", 0) != 0)
                continue;
            const float u = 0.5f * (lcg.next() + 1.0f);
            vals.v[(size_t) paramIndex (s.id)] = s.minValue + u * (s.maxValue - s.minValue);
        }
        m->setParams (vals.data());
        constant = constant && m->getLatencySamples() == 125;
    }
    CHECK (constant);
}

TEST_CASE ("bass. split alone sums to a flat all-pass at the cutoff")
{
    for (float cutoff : { 65.0f, 120.0f, 300.0f })
    {
        Values vals;
        vals.set ("cutoff", cutoff);
        vals.set ("sus.on", 1.0f);       // sustain power at amount 0, clean: only the split is active
        vals.set ("sus.amount", 0.0f);
        auto m = makeShaper (kSr, vals);
        const int lat = m->getLatencySamples();
        const size_t n = 1 << 16;
        Stereo in { std::vector<float> (n, 0.0f), std::vector<float> (n, 0.0f) };
        in.l[0] = in.r[0] = 0.05f;
        const auto out = run (*m, vals, in);
        std::vector<double> h (n, 0.0);
        for (size_t i = 0; i + (size_t) lat < n; ++i)
            h[i] = (double) out.l[i + (size_t) lat] / 0.05;
        const auto spec = pstest::spectrumOf (h, n);
        double maxDev = 0.0, maxPhaseErr = 0.0;
        for (size_t k = 1; k < n / 2; ++k)
        {
            const double hz = (double) k * kSr / (double) n;
            if (hz < 10.0 || hz > 20000.0)
                continue;
            maxDev = std::max (maxDev, std::abs (db (std::abs (spec[k]))));
            const double e = std::abs (std::arg (spec[k] / lr4AllPass (hz, cutoff, kSr)));
            maxPhaseErr = std::max (maxPhaseErr, e * 180.0 / kPi);
        }
        note ("cutoff " + std::to_string ((int) cutoff) + " Hz: max |magnitude| dB 10 Hz..20 kHz", maxDev);
        note ("    max phase error vs the 2nd-order all-pass at the cutoff (deg)", maxPhaseErr);
        CHECK (maxDev < 0.01);
        CHECK (maxPhaseErr < 0.1);
    }
}

TEST_CASE ("bass. balance acts like a shelf below the cutoff")
{
    auto measure = [] (float balance, float cutoff, double hz) {
        Values vals;
        vals.set ("balance", balance);
        vals.set ("cutoff", cutoff);
        auto m = makeShaper (kSr, vals);
        const auto x = pstest::sine (hz, -12.0, 1.5, kSr);
        const auto out = run (*m, vals, dual (x));
        return toneGainDb (out.l, x, m->getLatencySamples(), hz, kSr, (size_t) (0.5 * kSr), (size_t) kSr);
    };
    const double g40 = measure (6.0f, 120.0f, 40.0), g1k = measure (6.0f, 120.0f, 1000.0),
                 g120 = measure (6.0f, 120.0f, 120.0), m40 = measure (-6.0f, 120.0f, 40.0),
                 lo200 = measure (6.0f, 65.0f, 200.0), hi200 = measure (6.0f, 300.0f, 200.0);
    note ("balance +6: 40 Hz", g40);
    note ("balance +6: 120 Hz (cutoff)", g120);
    note ("balance +6: 1 kHz", g1k);
    note ("balance -6: 40 Hz", m40);
    note ("balance +6, 200 Hz with cutoff 65", lo200);
    note ("balance +6, 200 Hz with cutoff 300", hi200);
    CHECK_NEAR (g40, 6.0, 0.1);
    CHECK_NEAR (g120, 3.5, 0.3);
    CHECK_NEAR (g1k, 0.0, 0.02);
    // LR4 at 40 Hz with a 120 Hz cutoff still has |HP^2| = 0.012 in phase: -6 dB of balance reads -5.90 dB there
    const double g = std::pow (10.0, -6.0 / 20.0);
    const cd lp40 = lr4Low (40.0, 120.0, kSr), ap40 = lr4AllPass (40.0, 120.0, kSr);
    const double expectedM40 = db (std::abs (ap40 + (g - 1.0) * lp40));
    note ("balance -6: 40 Hz expected from the LR4 responses", expectedM40);
    CHECK_NEAR (m40, expectedM40, 0.02);
    CHECK_NEAR (m40, -6.0, 0.15);
    CHECK (lo200 < 0.6);
    CHECK (hi200 > 4.5);
}

TEST_CASE ("bass. punch moves the low-end hits and leaves the sustained bass")
{
    const double secs = 6.0;
    const auto k = kicks (secs, kSr);
    const auto bass = pstest::sine (41.0, -18.0, secs, kSr);
    const auto x = pstest::added (k, bass);
    const auto per = (size_t) (0.5 * kSr);

    for (float punch : { 6.0f, -6.0f })
    {
        Values vals;
        vals.set ("punch", punch);
        auto m = makeShaper (kSr, vals);
        const int lat = m->getLatencySamples();
        const auto out = run (*m, vals, dual (x));
        double hitSum = 0.0, gapWorst = 0.0;
        int hits = 0;
        for (size_t start = 2 * per; start + per <= x.size(); start += per)   // skip the first second
        {
            const size_t o = start + (size_t) lat;
            if (o + per > out.size())
                break;
            const double hit = rmsDb (out.l, o, (size_t) (0.030 * kSr)) - rmsDb (x, start, (size_t) (0.030 * kSr));
            const double gap = rmsDb (out.l, o + (size_t) (0.2 * kSr), (size_t) (0.1 * kSr))
                               - rmsDb (x, start + (size_t) (0.2 * kSr), (size_t) (0.1 * kSr));
            hitSum += hit;
            gapWorst = std::max (gapWorst, std::abs (gap));
            ++hits;
        }
        const double hitMean = hitSum / std::max (1, hits);
        note ("punch " + std::to_string ((int) punch) + ": first 30 ms of each hit (dB)", hitMean);
        note ("    worst |bass mid-gap change| 200-300 ms after a hit (dB)", gapWorst);
        note ("    transient share meter", m->getMeters().transientShare.load());
        if (punch > 0.0f)
            CHECK (hitMean >= 4.0 && hitMean <= 6.5);
        else
            CHECK (hitMean <= -4.0 && hitMean >= -6.5);
        CHECK (gapWorst <= 0.5);
    }

    // a steady sine is all sustain: punch does nothing
    for (float punch : { 12.0f, -12.0f })
    {
        Values vals;
        vals.set ("punch", punch);
        auto m = makeShaper (kSr, vals);
        const auto s = pstest::sine (50.0, -12.0, 3.0, kSr);
        const auto out = run (*m, vals, dual (s));
        const double g = toneGainDb (out.l, s, m->getLatencySamples(), 50.0, kSr, (size_t) (1.5 * kSr), (size_t) kSr);
        note ("steady 50 Hz, punch " + std::to_string ((int) punch), g);
        CHECK (std::abs (g) <= 0.3);
    }
}

TEST_CASE ("bass. the transient split behaves the same at 44.1, 48 and 96 kHz")
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const auto x = pstest::added (kicks (6.0, sr), pstest::sine (41.0, -18.0, 6.0, sr));
        Values vals;
        vals.set ("punch", 6.0f);
        auto m = makeShaper (sr, vals);
        const int lat = m->getLatencySamples();
        const auto out = run (*m, vals, dual (x));
        const auto per = (size_t) (0.5 * sr);
        double hitSum = 0.0;
        int hits = 0;
        for (size_t start = 2 * per; start + per + (size_t) lat <= x.size(); start += per, ++hits)
            hitSum += rmsDb (out.l, start + (size_t) lat, (size_t) (0.030 * sr)) - rmsDb (x, start, (size_t) (0.030 * sr));
        const double hit = hitSum / std::max (1, hits);
        note ("punch +6 @" + std::to_string ((int) sr) + ": first 30 ms of each hit (dB)", hit);
        CHECK (hit >= 4.0 && hit <= 5.0);
    }
}

TEST_CASE ("bass. sustain power lifts quiet sustain toward the threshold, not hits or noise")
{
    auto stepRun = [] (float amount, double& quietOut, double& loudGain, float& liftMeter) {
        Values vals;
        vals.set ("sus.on", 1.0f);
        vals.set ("sus.thr", -18.0f);
        vals.set ("sus.amount", amount);
        auto m = makeShaper (kSr, vals);
        const int lat = m->getLatencySamples();
        const auto x = pstest::sineSequence (50.0, { { -30.0, 2.0 }, { -12.0, 2.0 } }, kSr);
        float meterAtQuiet = 0.0f;
        const auto out = run (*m, vals, dual (x), 512, [&] (size_t start, Values&, BassShaper& mm) {
            if (start >= (size_t) (1.9 * kSr) && start < (size_t) (1.92 * kSr))
                meterAtQuiet = mm.getMeters().sustainLiftDb.load();
        });
        quietOut = db (pstest::peakOf (out.l, (size_t) (1.5 * kSr) + (size_t) lat, (size_t) (0.4 * kSr)));
        loudGain = toneGainDb (out.l, x, lat, 50.0, kSr, (size_t) (3.4 * kSr), (size_t) (0.5 * kSr));
        liftMeter = meterAtQuiet;
    };

    double quiet = 0.0, loud = 0.0;
    float lift = 0.0f;
    stepRun (100.0f, quiet, loud, lift);
    note ("amount 100: -30 dBFS part settles at (dBFS peak)", quiet);
    note ("amount 100: -12 dBFS part gain (dB)", loud);
    note ("amount 100: sustain lift meter (dB)", lift);
    CHECK_NEAR (quiet, -18.0, 0.5);
    CHECK (std::abs (loud) <= 0.3);
    CHECK_NEAR (lift, 12.0, 0.2);

    stepRun (50.0f, quiet, loud, lift);
    note ("amount 50: -30 dBFS part lift (dB)", quiet + 30.0);
    CHECK_NEAR (quiet + 30.0, 6.0, 0.5);

    // no lift of the noise floor
    {
        Values vals;
        vals.set ("sus.on", 1.0f);
        vals.set ("sus.amount", 100.0f);
        auto m = makeShaper (kSr, vals);
        const auto s = pstest::sine (50.0, -70.0, 3.0, kSr);
        const auto out = run (*m, vals, dual (s));
        const double g = toneGainDb (out.l, s, m->getLatencySamples(), 50.0, kSr, (size_t) (1.5 * kSr), (size_t) kSr);
        note ("-70 dBFS sine gain (dB)", g);
        CHECK (std::abs (g) <= 0.5);
    }

    // kick peaks are not lifted
    {
        Values vals;
        vals.set ("sus.on", 1.0f);
        vals.set ("sus.amount", 100.0f);
        auto m = makeShaper (kSr, vals);
        const int lat = m->getLatencySamples();
        const auto k = kicks (5.0, kSr);
        const auto out = run (*m, vals, dual (k));
        const auto per = (size_t) (0.5 * kSr);
        double worst = 0.0;
        for (size_t start = 2 * per; start + per + (size_t) lat <= k.size(); start += per)
        {
            const double d = db (pstest::peakOf (out.l, start + (size_t) lat, (size_t) (0.03 * kSr)))
                             - db (pstest::peakOf (k, start, (size_t) (0.03 * kSr)));
            worst = std::max (worst, std::abs (d));
        }
        note ("kick bursts: worst peak change (dB)", worst);
        CHECK (worst <= 0.5);
    }
}

TEST_CASE ("bass. dirty sustain adds 2nd and 3rd harmonics, clean adds none")
{
    for (float character : { 0.0f, 100.0f })
    {
        Values vals;
        vals.set ("sus.on", 1.0f);
        vals.set ("sus.thr", -12.0f);
        vals.set ("sus.amount", 0.0f);
        vals.set ("sus.char", character);
        auto m = makeShaper (kSr, vals);
        // 60 Hz on an exact FFT bin: 82 x 48000 / 65536 = 60.06 Hz
        const size_t n = 1 << 16;
        const double f0 = 82.0 * kSr / (double) n;
        const auto x = pstest::sine (f0, -12.0, 3.0, kSr);
        const auto out = run (*m, vals, dual (x));
        const auto mag = amplitudeSpectrum (out.l, (size_t) kSr, n);
        const double fund = db (mag[82]), h2 = db (mag[164]) - fund, h3 = db (mag[246]) - fund;
        note ("character " + std::to_string ((int) character) + ": 2nd harmonic (dBc)", h2);
        note ("    3rd harmonic (dBc)", h3);
        if (character == 0.0f)
        {
            CHECK (h2 >= -40.0);
            CHECK (h3 >= -40.0);
        }
        else
        {
            CHECK (h2 < -100.0);
            CHECK (h3 < -100.0);
        }
    }

    // a sustain far above the threshold stays bounded (normalised by the held level, |u| <= 1)
    {
        Values vals;
        vals.set ("sus.on", 1.0f);
        vals.set ("sus.thr", -40.0f);
        vals.set ("sus.char", 0.0f);
        vals.set ("cutoff", 300.0f);
        auto m = makeShaper (kSr, vals);
        const auto x = pstest::sine (60.0, -3.0, 3.0, kSr);
        const auto out = run (*m, vals, dual (x));
        const double pk = db (pstest::peakOf (out.l, (size_t) (0.1 * kSr), out.size() - (size_t) (0.1 * kSr)));
        note ("thr -40, -3 dBFS 60 Hz, character 0: output peak (dBFS)", pk);
        CHECK (pk < 3.0);
    }
}

TEST_CASE ("bass. dirty stages alias below -90 dBFS")
{
    struct Case
    {
        double sr;
        size_t bin;          // fundamental bin of a 65536-point FFT
        float susThr, susAmount, peakOn;
        const char* name;
    };
    const Case cases[] {
        { 44100.0, 372, -40.0f, 0.0f, 1.0f, "44.1 kHz 250 Hz -3 dBFS, both sections character 0" },
        { 48000.0, 137, -40.0f, 100.0f, 0.0f, "48 kHz 100 Hz -3 dBFS, dirty sustain at thr -40 (high drive)" },
        { 48000.0, 137, -40.0f, 100.0f, 1.0f, "48 kHz 100 Hz -3 dBFS, dirty sustain + dirty peak thr -12" },
    };
    for (const auto& c : cases)
    {
        Values vals;
        vals.set ("cutoff", 300.0f);
        vals.set ("sus.on", 1.0f);
        vals.set ("sus.thr", c.susThr);
        vals.set ("sus.amount", c.susAmount);
        vals.set ("sus.char", 0.0f);
        vals.set ("peak.on", c.peakOn);
        vals.set ("peak.char", 0.0f);
        vals.set ("peak.thr", c.sr == 44100.0 ? -6.0f : -12.0f);
        auto m = makeShaper (c.sr, vals);
        const size_t n = 1 << 16;
        const double f0 = (double) c.bin * c.sr / (double) n;
        const auto x = pstest::sine (f0, -3.0, 1.0 + (double) n / c.sr + 0.5, c.sr);
        const auto out = run (*m, vals, dual (x));
        const auto mag = amplitudeSpectrum (out.l, (size_t) c.sr, n);
        size_t where = 0;
        const double worst = worstNonHarmonicDb (mag, c.bin, 2, 30, &where);
        double h3 = db (mag[3 * c.bin]) - db (mag[c.bin]);
        note (std::string (c.name) + ": worst non-harmonic (dBFS)", worst);
        note ("    at (Hz)", (double) where * c.sr / (double) n);
        note ("    3rd harmonic (dBc)", h3);
        CHECK (worst < -90.0);
    }
}

TEST_CASE ("bass. clean peak control is a brickwall on the low part; dirty clips; mix 0 is the aligned input")
{
    // bursts of 50 Hz at -2 dBFS, thr -8, clean
    const double secs = 4.0;
    std::vector<float> burst ((size_t) (secs * kSr), 0.0f);
    for (size_t i = 0; i < burst.size(); ++i)
    {
        const double t = (double) (i % (size_t) (0.4 * kSr)) / kSr;
        if (t < 0.15)
            burst[i] = (float) (pstest::dbfsToAmplitude (-2.0) * std::sin (2.0 * kPi * 50.0 * t));
    }

    for (float character : { 100.0f, 0.0f })
    {
        Values vals;
        vals.set ("peak.on", 1.0f);
        vals.set ("peak.thr", -8.0f);
        vals.set ("peak.char", character);
        auto m = makeShaper (kSr, vals);
        float peakMeter = -200.0f, reduction = 0.0f;
        StereoSampleFifo& scope = m->getScopeFifo();
        std::vector<float> mn (1 << 16), mx (1 << 16);
        float scopeMax = 0.0f;
        run (*m, vals, dual (burst), 512, [&] (size_t, Values&, BassShaper& mm) {
            peakMeter = std::max (peakMeter, mm.getMeters().lowPeakDb.exchange (-200.0f));
            reduction = std::min (reduction, mm.getMeters().peakReductionDb.load());
            const int got = scope.pull (mn.data(), mx.data(), (int) mn.size());
            for (int i = 0; i < got; ++i)
                scopeMax = std::max (scopeMax, std::max (std::abs (mn[(size_t) i]), std::abs (mx[(size_t) i])));
        });
        note (std::string (character > 50.0f ? "clean" : "dirty") + ": low-part output peak (dBFS, meter)", peakMeter);
        note ("    scope max of the low mid (dBFS)", db (scopeMax));
        note ("    deepest clean-branch reduction meter (dB)", reduction);
        CHECK (peakMeter <= -8.0f + 0.05f);
        CHECK (db (scopeMax) <= -8.0 + 0.05);
        if (character > 50.0f)
            CHECK (reduction < -4.0f);
    }

    // the whole output, where the high band carries almost nothing (30 Hz, cutoff 300)
    {
        Values vals;
        vals.set ("cutoff", 300.0f);
        vals.set ("peak.on", 1.0f);
        vals.set ("peak.thr", -8.0f);
        auto m = makeShaper (kSr, vals);
        const auto x = pstest::sine (30.0, -1.0, 3.0, kSr);
        const auto out = run (*m, vals, dual (x));
        const double pk = db (pstest::peakOf (out.l, (size_t) (0.2 * kSr), out.size()));
        note ("clean, 30 Hz -1 dBFS, cutoff 300: whole output peak (dBFS)", pk);
        CHECK (pk <= -8.0 + 0.05);
    }

    // steady 50 Hz 2 dB over the threshold: low distortion (clean), strong 3rd harmonic (dirty)
    for (float character : { 100.0f, 0.0f })
    {
        Values vals;
        vals.set ("peak.on", 1.0f);
        vals.set ("peak.thr", -8.0f);
        vals.set ("peak.char", character);
        auto m = makeShaper (kSr, vals);
        const size_t n = 1 << 16;
        const double f0 = 68.0 * kSr / (double) n;   // 49.8 Hz on a bin
        const double inDb = character > 50.0f ? -6.0 + 0.26 : -2.0;   // low part 2 dB / 6 dB over
        const auto x = pstest::sine (f0, inDb, 3.0, kSr);
        const auto out = run (*m, vals, dual (x));
        const auto mag = amplitudeSpectrum (out.l, (size_t) kSr, n);
        double harm = 0.0;
        for (size_t k = 2; k * 68 < mag.size() && k * 68 * kSr / (double) n < 20000.0; ++k)
            harm += mag[k * 68] * mag[k * 68];
        const double thd = db (std::sqrt (harm)) - db (mag[68]);
        const double h3 = db (mag[204]) - db (mag[68]);
        note (std::string (character > 50.0f ? "clean" : "dirty") + " steady 50 Hz over threshold: THD (dB)", thd);
        note ("    3rd harmonic (dBc)", h3);
        if (character > 50.0f)
            CHECK (thd < -40.0);
        else
            CHECK (h3 >= -30.0);
    }

    // mix 0 == the aligned pre-peak low part exactly (compare against peak off, both with balance active)
    {
        Values a, b;
        a.set ("balance", 2.0f);
        b.set ("balance", 2.0f);
        b.set ("peak.on", 1.0f);
        b.set ("peak.thr", -20.0f);
        b.set ("peak.char", 30.0f);
        b.set ("peak.mix", 0.0f);
        auto ma = makeShaper (kSr, a);
        auto mb = makeShaper (kSr, b);
        const Stereo in { pstest::pinkNoise (0.8, 3.0, kSr, 5u), pstest::pinkNoise (0.8, 3.0, kSr, 6u) };
        const auto oa = run (*ma, a, in), ob = run (*mb, b, in);
        double d = 0.0;
        for (size_t i = 0; i < oa.size(); ++i)
            d = std::max (d, std::max (std::abs ((double) oa.l[i] - ob.l[i]), std::abs ((double) oa.r[i] - ob.r[i])));
        note ("mix 0 vs peak off: max difference", d);
        CHECK_EQ (d, 0.0);
    }

    // stereo: the limiter is linked (channel maximum), so a quiet right channel gets the same reduction
    {
        Values vals;
        vals.set ("peak.on", 1.0f);
        vals.set ("peak.thr", -12.0f);
        auto m = makeShaper (kSr, vals);
        const Stereo in { pstest::sine (40.0, -2.0, 2.0, kSr), pstest::sine (40.0, -20.0, 2.0, kSr) };
        const auto out = run (*m, vals, in);
        const int lat = m->getLatencySamples();
        const double gl = toneGainDb (out.l, in.l, lat, 40.0, kSr, (size_t) kSr, (size_t) (0.5 * kSr));
        const double gr = toneGainDb (out.r, in.r, lat, 40.0, kSr, (size_t) kSr, (size_t) (0.5 * kSr));
        note ("linked limiter: left gain (dB)", gl);
        note ("    right gain (dB)", gr);
        CHECK (gl < -8.0);
        CHECK_NEAR (gl, gr, 0.3);
    }
}

TEST_CASE ("bass. meters: balance, crest, share and the scope FIFOs follow the audio")
{
    // pink noise: balance equals the expected LR4 low-band share of the input (same 3 s weighting)
    {
        Values vals;
        auto m = makeShaper (kSr, vals);
        const size_t n = 1 << 19;   // 10.9 s
        const Stereo in { pstest::pinkNoise (0.5, (double) n / kSr, kSr, 21u), pstest::pinkNoise (0.5, (double) n / kSr, kSr, 22u) };
        run (*m, vals, in);
        double expectedLow = 0.0, expectedTot = 0.0;
        for (const auto* ch : { &in.l, &in.r })
        {
            std::vector<cd> a (n);
            for (size_t i = 0; i < n; ++i)
                a[i] = (double) (*ch)[i];
            pstest::fftInPlace (a);
            for (size_t k = 0; k < n; ++k)
            {
                const size_t kk = k <= n / 2 ? k : n - k;
                const cd hk = lr4Low ((double) kk * kSr / (double) n, 120.0, kSr);
                a[k] *= k <= n / 2 ? hk : std::conj (hk);   // H (-f) = conj (H (f)): the filtered signal stays real
            }
            pstest::fftInPlace (a, true);
            const double c3 = std::exp (-1.0 / (3.0 * kSr));
            double wsum = 1.0;
            for (size_t i = n; i-- > 0;)
            {
                expectedLow += wsum * a[i].real() * a[i].real();
                expectedTot += wsum * (double) (*ch)[i] * (double) (*ch)[i];
                wsum *= c3;
            }
        }
        const double expected = 10.0 * std::log10 (expectedLow / expectedTot);
        const double got = m->getMeters().balanceDb.load(), gotOut = m->getMeters().balanceOutDb.load();
        note ("pink noise: balance meter (dB)", got);
        note ("    expected from the input's own FFT under the LR4 low pass", expected);
        note ("    output balance meter (neutral)", gotOut);
        note ("    target zone centre at 120 Hz", bassShaperBalanceTargetDb (120.0));
        CHECK_NEAR (got, expected, 0.3);
        CHECK_NEAR (gotOut, got, 0.1);
        CHECK_NEAR (bassShaperBalanceTargetDb (240.0), -1.0, 1.0e-9);

        Values v2;
        v2.set ("balance", 6.0f);
        auto m2 = makeShaper (kSr, v2);
        run (*m2, v2, in);
        // +6 dB on the low band: P_low x 4, the rest unchanged (the bands overlap little in power)
        const double r = std::pow (10.0, m2->getMeters().balanceDb.load() / 10.0);
        const double expectedOut = 10.0 * std::log10 (4.0 * r / (1.0 - r + 4.0 * r));
        note ("balance +6: output balance meter (dB)", m2->getMeters().balanceOutDb.load());
        note ("    expected", expectedOut);
        CHECK_NEAR (m2->getMeters().balanceOutDb.load(), expectedOut, 0.5);
    }

    // crest: sine 3 dB, kicks > 10 dB; share: kicks > 0.05, a steady sine ~0
    {
        Values vals;
        auto m = makeShaper (kSr, vals);
        run (*m, vals, dual (pstest::sine (50.0, -12.0, 8.0, kSr)));   // the 1 s RMS has settled to 0.03 %
        const float crestSine = m->getMeters().crestDb.load(), shareSine = m->getMeters().transientShare.load();
        auto m2 = makeShaper (kSr, vals);
        run (*m2, vals, dual (kicks (4.0, kSr)));
        const float crestKick = m2->getMeters().crestDb.load(), shareKick = m2->getMeters().transientShare.load();
        note ("sine crest (dB)", crestSine);
        note ("kick bursts crest (dB)", crestKick);
        note ("sine transient share", shareSine);
        note ("kick transient share", shareKick);
        CHECK_NEAR (crestSine, 3.0, 0.2);
        CHECK_NEAR (m->getMeters().crestOutDb.load(), crestSine, 0.2);
        CHECK (crestKick > 10.0f);
        CHECK (shareSine < 0.01f);
        CHECK (shareKick > 0.05f);
    }

    // scope FIFO: one (min, max) pair per 16 samples of the low mid; mask FIFO: w in [0, 1] and an envelope
    {
        Values vals;
        auto m = makeShaper (kSr, vals);
        const auto x = pstest::sine (50.0, -6.0, 1.0, kSr);
        run (*m, vals, dual (x));
        std::vector<float> a (1 << 16), b (1 << 16), c (1 << 16), d (1 << 16);
        const int ns = m->getScopeFifo().pull (a.data(), b.data(), (int) a.size());
        const int nm = m->getMaskFifo().pull (c.data(), d.data(), (int) c.size());
        float mn = 0.0f, mx = 0.0f, wMax = 0.0f, envLast = d[(size_t) std::max (0, nm - 1)];
        for (int i = ns / 2; i < ns; ++i)
        {
            mn = std::min (mn, a[(size_t) i]);
            mx = std::max (mx, b[(size_t) i]);
        }
        for (int i = nm / 2; i < nm; ++i)
            wMax = std::max (wMax, c[(size_t) i]);
        const double lowAmp = pstest::dbfsToAmplitude (-6.0) * std::abs (lr4Low (50.0, 120.0, kSr));
        note ("scope pairs for 1 s", ns);
        note ("scope max (dB re expected low amplitude)", db (mx / lowAmp));
        note ("mask envelope (dB re expected)", db (envLast / lowAmp));
        CHECK_EQ (ns, (int) (kSr / 16));
        CHECK_EQ (nm, ns);
        CHECK_NEAR (db (mx / lowAmp), 0.0, 0.1);
        CHECK_NEAR (db (-mn / lowAmp), 0.0, 0.1);
        CHECK (wMax < 0.01f);
        CHECK_NEAR (db (envLast / lowAmp), 0.0, 0.5);
    }
}

TEST_CASE ("bass. auto gain levels the output to the input")
{
    Values vals;
    vals.set ("balance", 6.0f);
    vals.set ("autoGain", 1.0f);
    auto m = makeShaper (kSr, vals);
    const Stereo in { pstest::pinkNoise (0.5, 10.0, kSr, 31u), pstest::pinkNoise (0.5, 10.0, kSr, 32u) };
    const auto out = run (*m, vals, in);
    const auto from = (size_t) (6.0 * kSr), len = (size_t) (4.0 * kSr);
    const double d = rmsDb (out.l, from, len) - rmsDb (in.l, from - (size_t) m->getLatencySamples(), len);
    Values v2;
    v2.set ("balance", 6.0f);
    auto m2 = makeShaper (kSr, v2);
    const auto out2 = run (*m2, v2, in);
    note ("balance +6 without auto gain: RMS change (dB)", rmsDb (out2.l, from, len) - rmsDb (in.l, from, len));
    note ("balance +6 with auto gain: RMS change after 6 s (dB)", d);
    CHECK (std::abs (d) <= 0.5);

    // back to neutral with auto gain on: the gain returns to unity and the output is the exact delay again
    Values v3;
    v3.set ("autoGain", 1.0f);
    auto m3 = makeShaper (kSr, v3);
    const auto out3 = run (*m3, v3, in, 512, [&] (size_t start, Values& v, BassShaper&) {
        v.set ("balance", start < (size_t) (5.0 * kSr) ? 6.0f : 0.0f);
    });
    const double exact = maxDelayedDiff (out3.l, in.l, m3->getLatencySamples(), (size_t) (7.0 * kSr));
    note ("auto gain on, balance +6 -> 0 at 5 s: max diff vs exact delay from 7 s", exact);
    CHECK_EQ (exact, 0.0);
}

TEST_CASE ("bass. parameter changes are click-free")
{
    // a 60 Hz sine has a tiny second difference: any step in gain or filter state shows up at once
    struct Move
    {
        const char* name;
        std::function<void (Values&)> before, after;
        double rampSeconds;   // 0 = jump
        std::function<void (Values&, double)> ramp;
        double inputDb = -12.0;
    };
    const std::vector<Move> moves {
        { "cutoff 65 -> 300 in 100 ms (balance +6)", [] (Values& v) { v.set ("balance", 6.0f); v.set ("cutoff", 65.0f); },
          [] (Values& v) { v.set ("balance", 6.0f); v.set ("cutoff", 300.0f); }, 0.1,
          [] (Values& v, double a) { v.set ("cutoff", (float) (65.0 * std::pow (300.0 / 65.0, a))); } },
        { "balance -12 -> +12 jump", [] (Values& v) { v.set ("balance", -12.0f); },
          [] (Values& v) { v.set ("balance", 12.0f); }, 0.0, {} },
        { "balance 0 -> +6 jump (leaves the neutral passthrough)", [] (Values&) {},
          [] (Values& v) { v.set ("balance", 6.0f); }, 0.0, {} },
        { "punch -12 -> +12 jump", [] (Values& v) { v.set ("punch", -12.0f); },
          [] (Values& v) { v.set ("punch", 12.0f); }, 0.0, {} },
        { "sustain on (thr -6, amount 100)", [] (Values& v) { v.set ("sus.thr", -6.0f); v.set ("sus.amount", 100.0f); },
          [] (Values& v) { v.set ("sus.on", 1.0f); v.set ("sus.thr", -6.0f); v.set ("sus.amount", 100.0f); }, 0.0, {} },
        { "sustain character 100 -> 0", [] (Values& v) { v.set ("sus.on", 1.0f); v.set ("sus.thr", -12.0f); },
          [] (Values& v) { v.set ("sus.on", 1.0f); v.set ("sus.thr", -12.0f); v.set ("sus.char", 0.0f); }, 0.0, {} },
        { "sustain threshold -40 -> 0 (character 0)", [] (Values& v) { v.set ("sus.on", 1.0f); v.set ("sus.thr", -40.0f); v.set ("sus.char", 0.0f); },
          [] (Values& v) { v.set ("sus.on", 1.0f); v.set ("sus.thr", 0.0f); v.set ("sus.char", 0.0f); }, 0.0, {} },
        { "sustain amount 100 -> 0 on quiet sustain",
          [] (Values& v) { v.set ("sus.on", 1.0f); v.set ("sus.thr", -18.0f); v.set ("sus.amount", 100.0f); },
          [] (Values& v) { v.set ("sus.on", 1.0f); v.set ("sus.thr", -18.0f); v.set ("sus.amount", 0.0f); }, 0.0, {}, -36.0 },
        { "sustain off on quiet lifted sustain",
          [] (Values& v) { v.set ("sus.on", 1.0f); v.set ("sus.thr", -18.0f); v.set ("sus.amount", 100.0f); },
          [] (Values& v) { v.set ("sus.on", 0.0f); v.set ("sus.thr", -18.0f); v.set ("sus.amount", 100.0f); }, 0.0, {}, -36.0 },
        { "peak on (thr -20, clean)", [] (Values& v) { v.set ("peak.thr", -20.0f); },
          [] (Values& v) { v.set ("peak.on", 1.0f); v.set ("peak.thr", -20.0f); }, 0.0, {} },
        { "peak threshold 0 -> -30", [] (Values& v) { v.set ("peak.on", 1.0f); v.set ("peak.thr", 0.0f); },
          [] (Values& v) { v.set ("peak.on", 1.0f); v.set ("peak.thr", -30.0f); }, 0.0, {} },
        { "peak character 100 -> 0 (thr -18)", [] (Values& v) { v.set ("peak.on", 1.0f); v.set ("peak.thr", -18.0f); },
          [] (Values& v) { v.set ("peak.on", 1.0f); v.set ("peak.thr", -18.0f); v.set ("peak.char", 0.0f); }, 0.0, {} },
        { "peak mix 100 -> 0 (thr -20)", [] (Values& v) { v.set ("peak.on", 1.0f); v.set ("peak.thr", -20.0f); },
          [] (Values& v) { v.set ("peak.on", 1.0f); v.set ("peak.thr", -20.0f); v.set ("peak.mix", 0.0f); }, 0.0, {} },
        { "auto gain on (balance +12)", [] (Values& v) { v.set ("balance", 12.0f); },
          [] (Values& v) { v.set ("balance", 12.0f); v.set ("autoGain", 1.0f); }, 0.0, {} },
        { "peak on, dirty (character 0, thr -18; the oversampler wakes up)", [] (Values& v) { v.set ("peak.thr", -18.0f); v.set ("peak.char", 0.0f); },
          [] (Values& v) { v.set ("peak.on", 1.0f); v.set ("peak.thr", -18.0f); v.set ("peak.char", 0.0f); }, 0.0, {} },
        { "sustain on, dirty (character 0, thr -12)", [] (Values& v) { v.set ("sus.thr", -12.0f); v.set ("sus.char", 0.0f); },
          [] (Values& v) { v.set ("sus.on", 1.0f); v.set ("sus.thr", -12.0f); v.set ("sus.char", 0.0f); }, 0.0, {} },
        { "everything off at once (back toward the passthrough)",
          [] (Values& v) { v.set ("balance", 4.0f); v.set ("punch", 4.0f); v.set ("sus.on", 1.0f); v.set ("sus.char", 30.0f);
                           v.set ("peak.on", 1.0f); v.set ("peak.thr", -14.0f); v.set ("peak.char", 30.0f); v.set ("autoGain", 1.0f); },
          [] (Values& v) { v.set ("autoGain", 1.0f); }, 0.0, {} },
    };

    const auto tSwitch = (size_t) (3.0 * kSr);
    for (const auto& mv : moves)
    {
        const auto x = pstest::sine (60.0, mv.inputDb, 6.0, kSr);
        Values vals;
        mv.before (vals);
        auto m = makeShaper (kSr, vals);
        const auto out = run (*m, vals, dual (x), 64, [&] (size_t start, Values& v, BassShaper&) {
            if (start < tSwitch)
                return;
            v = Values();
            mv.after (v);
            if (mv.rampSeconds > 0.0 && mv.ramp)
            {
                const double a = std::min (1.0, (double) (start - tSwitch) / (mv.rampSeconds * kSr));
                mv.ramp (v, a);
            }
        });
        const auto lat = (size_t) m->getLatencySamples();
        // Mix, character and auto gain act on output-time samples, so a transition can start immediately. Keep
        // every post-switch sample out of the steady reference, or an early click would raise its own threshold.
        const double steadyBefore = maxSecondDiff (out.l, (size_t) (2.0 * kSr), tSwitch);
        const double steadyAfter = maxSecondDiff (out.l, (size_t) (5.0 * kSr), out.size());
        const double during = maxSecondDiff (out.l, tSwitch, tSwitch + lat + (size_t) (1.0 * kSr));
        const double ratio = during / std::max (steadyBefore, steadyAfter);
        note (std::string (mv.name) + ": transition / steady second difference", ratio);
        CHECK (ratio <= 4.0);
    }
}

TEST_CASE ("bass. cutoff automation during the neutral transition is independent of host block size")
{
    // Switch at exact sample positions regardless of the host's block partition. Changing the cutoff while the
    // return-to-neutral all-pass is audible must use the same per-sample coefficient trajectory as the split.
    const auto x = pstest::sine (120.0, -12.0, 2.0, kSr);
    const size_t neutralAt = (size_t) kSr, cutoffAt = (size_t) (1.36 * kSr);
    auto render = [&] (int block) {
        Values vals;
        vals.set ("balance", 0.01f);
        vals.set ("cutoff", 65.0f);
        auto m = makeShaper (kSr, vals, block);
        Stereo out = dual (x);
        ProcessContext ctx;
        for (size_t start = 0; start < out.size();)
        {
            vals.set ("balance", start < neutralAt ? 0.01f : 0.0f);
            vals.set ("cutoff", start < cutoffAt ? 65.0f : 300.0f);
            m->setParams (vals.data());
            size_t end = std::min (start + (size_t) block, out.size());
            if (start < neutralAt)
                end = std::min (end, neutralAt);
            if (start < cutoffAt)
                end = std::min (end, cutoffAt);
            float* ch[2] { out.l.data() + start, out.r.data() + start };
            m->process (ch, (int) (end - start), ctx);
            start = end;
        }
        return out;
    };
    const auto reference = render (1);
    const double referenceSecond = maxSecondDiff (reference.l, cutoffAt, cutoffAt + (size_t) (0.050 * kSr));
    for (const int block : { 17, 64, 512 })
    {
        const auto out = render (block);
        double error = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
            error = std::max (error, std::abs ((double) out.l[i] - reference.l[i]));
        const double second = maxSecondDiff (out.l, cutoffAt, cutoffAt + (size_t) (0.050 * kSr));
        note ("neutral transition + cutoff jump, block " + std::to_string (block) + ": max error vs block 1", error);
        note ("    second difference / block 1", second / referenceSecond);
        CHECK (error < 1.0e-6);
        CHECK (second <= referenceSecond * 1.05);
    }
}

TEST_CASE ("bass. stable and denormal-safe under extreme automation")
{
    Values vals;
    auto m = makeShaper (kSr, vals);
    pstest::Lcg lcg (99u);
    const Stereo in { pstest::added (pstest::pinkNoise (0.6, 10.0, kSr, 41u), kicks (10.0, kSr, -3.0)),
                      pstest::added (pstest::pinkNoise (0.6, 10.0, kSr, 42u), kicks (10.0, kSr, -3.0)) };
    const auto out = run (*m, vals, in, 64, [&] (size_t, Values& v, BassShaper&) {
        for (const auto& s : paramSpecs())
        {
            if (s.id.rfind ("bass.", 0) != 0)
                continue;
            const float u = lcg.next() > 0.0f ? 1.0f : 0.0f;   // extremes only
            v.v[(size_t) paramIndex (s.id)] = s.minValue + u * (s.maxValue - s.minValue);
        }
    });
    bool finite = true;
    double pk = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        finite = finite && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
        pk = std::max (pk, (double) std::max (std::abs (out.l[i]), std::abs (out.r[i])));
    }
    note ("random extreme values every 64 samples: output peak (dBFS)", db (pk));
    CHECK (finite);
    CHECK (pk < pstest::dbfsToAmplitude (14.0));

    // silence after loud material: decays to exact zeros, and runs as fast as with signal
    Values v2;
    v2.set ("balance", 6.0f);
    v2.set ("punch", 6.0f);
    v2.set ("sus.on", 1.0f);
    v2.set ("sus.char", 0.0f);
    v2.set ("peak.on", 1.0f);
    v2.set ("peak.char", 50.0f);
    auto m2 = makeShaper (kSr, v2);
    run (*m2, v2, dual (pstest::pinkNoise (0.8, 2.0, kSr, 7u)));
    const Stereo silence { std::vector<float> ((size_t) (40.0 * kSr), 0.0f), std::vector<float> ((size_t) (40.0 * kSr), 0.0f) };
    pstest::Stopwatch sw;
    const auto quiet = run (*m2, v2, silence);
    const double tSilence = sw.seconds();
    double tail = 0.0;
    for (size_t i = (size_t) (30.0 * kSr); i < quiet.size(); ++i)
        tail = std::max (tail, (double) std::abs (quiet.l[i]));
    note ("40 s of silence after noise: processing time (s)", tSilence);
    note ("    largest output sample after 30 s", tail);
    CHECK (tail < 1.0e-20);
}

TEST_CASE ("bass. no allocation in setParams / process / reset")
{
    Values vals;
    auto m = makeShaper (kSr, vals);
    const Stereo in { pstest::pinkNoise (0.6, 2.0, kSr, 51u), pstest::pinkNoise (0.6, 2.0, kSr, 52u) };
    Stereo buf = in;
    long long count = 0;
    {
        pstest::AllocationScope scope;
        ProcessContext ctx;
        int k = 0;
        for (size_t start = 0; start + 512 <= buf.size(); start += 512, ++k)
        {
            vals.set ("balance", (float) ((k % 7) - 3));
            vals.set ("punch", (float) ((k % 5) - 2));
            vals.set ("sus.on", (float) (k % 2));
            vals.set ("peak.on", (float) ((k / 3) % 2));
            vals.set ("cutoff", 65.0f + (float) (k % 11) * 20.0f);
            vals.set ("sus.char", (float) ((k * 13) % 100));
            vals.set ("peak.char", (float) ((k * 7) % 100));
            vals.set ("autoGain", (float) ((k / 5) % 2));
            m->setParams (vals.data());
            float* ch[2] { buf.l.data() + start, buf.r.data() + start };
            m->process (ch, 512, ctx);
            if (k == 40)
                m->reset();
        }
        count = scope.count();
    }
    note ("allocations", (double) count);
    CHECK_EQ (count, 0LL);
}

TEST_CASE ("bass. cpu benchmark (48 kHz stereo)")
{
    const Stereo in { pstest::pinkNoise (0.6, 20.0, kSr, 61u), pstest::pinkNoise (0.6, 20.0, kSr, 62u) };
    auto bench = [&] (Values& vals, const char* name) {
        auto m = makeShaper (kSr, vals);
        Stereo buf = in;
        pstest::Stopwatch sw;
        run (*m, vals, buf, 512);
        const double x = 20.0 / sw.seconds();
        note (std::string (name) + ": x realtime", x);
        return x;
    };
    Values neutral;
    Values typical;
    typical.set ("balance", 2.0f);
    typical.set ("punch", 3.0f);
    typical.set ("peak.on", 1.0f);
    Values worst;
    worst.set ("balance", 3.0f);
    worst.set ("punch", 4.0f);
    worst.set ("sus.on", 1.0f);
    worst.set ("sus.char", 50.0f);
    worst.set ("peak.on", 1.0f);
    worst.set ("peak.char", 50.0f);
    worst.set ("autoGain", 1.0f);
    bench (neutral, "neutral (defaults)");
    const double t = bench (typical, "typical (balance, punch, clean peak control)");
    const double w = bench (worst, "worst (all sections, characters 50, auto gain)");
    CHECK (t > 60.0);
    CHECK (w > 60.0);
}
