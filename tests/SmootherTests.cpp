#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/Smoother.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

// Smoother (dsp/Smoother.cpp): cut-only 32-band harshness tamer. Prefix "smth.".
namespace
{
using namespace ps;
using pstest::note;

constexpr double kSr = 48000.0;
constexpr int kHop = 256;
constexpr int kN = 1024;

// Parameter array at the table defaults, set by id.
struct Values
{
    std::vector<float> v;
    Values()
    {
        for (const auto& s : paramSpecs())
            v.push_back (s.defaultValue);
    }
    Values& set (const std::string& id, float x)
    {
        const int i = paramIndex (id);
        REQUIRE (i >= 0);
        v[(size_t) i] = x;
        return *this;
    }
    // both sets
    Values& both (const std::string& name, float x) { return set ("smth.a." + name, x).set ("smth.b." + name, x); }
    ParamValues data() const { return v.data(); }
};

struct Stereo
{
    std::vector<float> l, r;
};

// Periodic noise of n samples (power of two) whose power spectrum is pink (-3 dB/oct, 20 Hz .. 20 kHz) times
// 10^(shapeDb (f) / 10), random phases; scaled to rmsDbfs.
std::vector<float> shapedNoise (size_t n, std::uint32_t seed, double rmsDbfs, const std::function<double (double)>& shapeDb,
                                double fLo = 20.0, double fHi = 20000.0)
{
    std::vector<pstest::cd> a (n);
    pstest::Lcg lcg (seed);
    for (size_t k = 1; k < n / 2; ++k)
    {
        const double f = (double) k * kSr / (double) n;
        const double ph = pstest::kPi * (double) lcg.next();
        if (f < fLo || f > fHi)
            continue;
        const double mag = std::sqrt (1.0 / f) * std::pow (10.0, shapeDb (f) / 20.0);
        a[k] = std::polar (mag, ph);
        a[n - k] = std::conj (a[k]);
    }
    pstest::fftInPlace (a, true);
    std::vector<float> y (n);
    double s = 0.0;
    for (size_t i = 0; i < n; ++i)
        s += a[i].real() * a[i].real();
    const double g = std::pow (10.0, rmsDbfs / 20.0) / std::sqrt (s / (double) n);
    for (size_t i = 0; i < n; ++i)
        y[i] = (float) (a[i].real() * g);
    return y;
}

std::vector<float> tiled (const std::vector<float>& x, size_t total)
{
    std::vector<float> y (total);
    for (size_t i = 0; i < total; ++i)
        y[i] = x[i % x.size()];
    return y;
}

// Smooth-edged band: +db for |log2 (f / fc)| < width / 2, raised-cosine edges over 1/12 octave.
double bandDb (double f, double fc, double db, double widthOct = 1.0 / 3.0)
{
    const double d = std::abs (std::log2 (f / fc)) - 0.5 * widthOct;
    if (d <= 0.0)
        return db;
    if (d >= 1.0 / 12.0)
        return 0.0;
    return db * 0.5 * (1.0 + std::cos (pstest::kPi * d * 12.0));
}

void prepare (Smoother& s, double sr = kSr, int block = 512)
{
    s.prepare (sr, block);
    s.reset();
}

// Runs the module over x in blocks; perBlock (blockIndex) is called after each block.
void run (Smoother& s, Stereo& x, int block, const ProcessContext& ctx, const std::function<void (size_t)>& perBlock = {})
{
    size_t bi = 0;
    for (size_t pos = 0; pos < x.l.size(); pos += (size_t) block, ++bi)
    {
        const int n = (int) std::min<size_t> ((size_t) block, x.l.size() - pos);
        float* ch[2] { x.l.data() + pos, x.r.data() + pos };
        s.process (ch, n, ctx);
        if (perBlock)
            perBlock (bi);
    }
}

// Welch power of y in [f1, f2] over [start, start + len) (Hann 4096, hop 2048).
double bandPower (const std::vector<float>& y, size_t start, size_t len, double f1, double f2)
{
    const size_t n = 4096;
    std::vector<double> w (n);
    for (size_t i = 0; i < n; ++i)
        w[i] = 0.5 - 0.5 * std::cos (2.0 * pstest::kPi * (double) i / (double) n);
    double p = 0.0;
    std::vector<pstest::cd> a (n);
    for (size_t s = start; s + n <= start + len && s + n <= y.size(); s += n / 2)
    {
        for (size_t i = 0; i < n; ++i)
            a[i] = (double) y[s + i] * w[i];
        pstest::fftInPlace (a);
        for (size_t k = 1; k < n / 2; ++k)
        {
            const double f = (double) k * kSr / (double) n;
            if (f >= f1 && f <= f2)
                p += std::norm (a[k]);
        }
    }
    return p;
}

// Output / input level change (dB) in [f1, f2]: out compared with the input delayed by `latency`.
double changeDb (const std::vector<float>& in, const std::vector<float>& out, int latency, size_t start, size_t len,
                 double f1, double f2)
{
    std::vector<float> d (in.size(), 0.0f);
    for (size_t i = (size_t) latency; i < in.size(); ++i)
        d[i] = in[i - (size_t) latency];
    return 10.0 * std::log10 (bandPower (out, start, len, f1, f2) / std::max (1.0e-30, bandPower (d, start, len, f1, f2)));
}

double third (double fc, int sign) { return fc * std::pow (2.0, sign / 6.0); }

int bandNear (double hz, double sr = kSr)
{
    int best = 0;
    for (int b = 1; b < kSmootherBands; ++b)
        if (std::abs (std::log2 (smootherBandHz (b, sr) / hz)) < std::abs (std::log2 (smootherBandHz (best, sr) / hz)))
            best = b;
    return best;
}

Values harshValues (float amount, SmootherMode mode, float lowHz = 2000.0f, float highHz = 10000.0f)
{
    Values v;
    v.both ("amount", amount).both ("mode", (float) mode).both ("att", 8.0f).both ("rel", 30.0f).both ("tone", 0.0f);
    v.both ("low", lowHz).both ("high", highHz);
    return v;
}

// The UI's Learn rule: the shared helper of Smoother.h (smootherLearnedRegion), as the panel uses it.
void learnedRegion (const SmootherMeters& m, int set, double& lowHz, double& highHz)
{
    const bool learned = smootherLearnedRegion (m, set, kSr, lowHz, highHz);
    CHECK (learned);
}

double meanGr (const std::vector<double>& v) { double s = 0.0; for (double x : v) s += x; return v.empty() ? 0.0 : s / (double) v.size(); }

// The gain computer's detector level (Smoother.cpp) of a band for this material: the band meter X, averaged after the
// first second, + 6.98 - 3.44 log2 (f / 1 kHz). The threshold is -65.1 + (100 - Amount) dB, so amountFor (level - m)
// puts the threshold m dB under that level (e = m).
double detectorLevel (const std::vector<float>& l, const std::vector<float>& r, double hz)
{
    Smoother s;
    prepare (s);
    const auto v = harshValues (1.0f, SmootherMode::Medium, 20.0f, 20000.0f);   // threshold +34 dB: measures only
    s.setParams (v.data());
    Stereo x { l, r };
    const int b = bandNear (hz);
    std::vector<double> xs;
    run (s, x, kHop, {}, [&] (size_t bi) { if ((double) bi * kHop > kSr) xs.push_back (s.getMeters().bandLevelDb[0][b].load()); });
    return meanGr (xs) + 6.98 - 3.44 * std::log2 (smootherBandHz (b, kSr) / 1000.0);
}

float amountFor (double thresholdDb) { return (float) std::clamp (100.0 - (thresholdDb + 65.1), 0.0, 100.0); }
} // namespace

//==============================================================================
TEST_CASE ("smth. band grid: 32 Mel bands, centres and latency per sample rate")
{
    CHECK_EQ (kSmootherBands, 32);
    double prev = 0.0;
    for (int b = 0; b < kSmootherBands; ++b)
    {
        const double f = smootherBandHz (b, kSr);
        CHECK (f > prev);
        prev = f;
    }
    note ("band 0 centre (Hz)", smootherBandHz (0, kSr));
    note ("band 31 centre (Hz)", smootherBandHz (31, kSr));
    CHECK (smootherBandHz (0, kSr) > 20.0 && smootherBandHz (0, kSr) < 120.0);
    CHECK (smootherBandHz (31, kSr) < 20000.0 && smootherBandHz (31, kSr) > 15000.0);
    CHECK (smootherBandHz (31, 32000.0) < 0.475 * 32000.0);

    const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const int lat[] { 1024, 1024, 2048, 2048, 4096, 4096 };
    for (int i = 0; i < 6; ++i)
    {
        Smoother s;
        prepare (s, rates[i]);
        CHECK_EQ (s.getLatencySamples(), lat[i]);
        CHECK_EQ (s.getSampleRate(), rates[i]);
    }

    // constant under random parameter sweeps, region solo and learn toggles (and every block size)
    Smoother s;
    prepare (s);
    pstest::Lcg lcg (77u);
    Stereo x { pstest::pinkNoise (0.5, 2.0, kSr, 3u), pstest::pinkNoise (0.5, 2.0, kSr, 4u) };
    bool constant = true;
    const auto& specs = paramSpecs();
    Values v;
    ProcessContext ctx;
    for (size_t pos = 0, blk = 0; pos < x.l.size(); ++blk)
    {
        const int n = 1 + (int) ((lcg.next() + 1.0f) * 300.0f);
        for (const auto& sp : specs)
            if (sp.id.rfind ("smth.", 0) == 0)
                v.set (sp.id, sp.minValue + 0.5f * (lcg.next() + 1.0f) * (sp.maxValue - sp.minValue));
        s.setParams (v.data());
        ModuleMonitor m;
        m.soloRegion = lcg.next() > 0.0f;
        m.learnRegion = lcg.next() > 0.0f;
        s.setMonitor (m);
        ctx.channelMode = (blk % 3) == 0 ? ChannelMode::MidSide : ChannelMode::Stereo;
        const int len = (int) std::min<size_t> ((size_t) n, x.l.size() - pos);
        float* ch[2] { x.l.data() + pos, x.r.data() + pos };
        s.process (ch, len, ctx);
        pos += (size_t) len;
        constant = constant && s.getLatencySamples() == 1024;
    }
    CHECK (constant);
}

TEST_CASE ("smth. amount 0 is a bit-exact delay by N (every mode, channel mode, twin, block size)")
{
    const auto l = pstest::pinkNoise (0.7, 1.5, kSr, 11u);
    auto r = pstest::added (pstest::sine (3000.0, -6.0, 1.5, kSr), pstest::pinkNoise (0.3, 1.5, kSr, 12u));
    int worstMismatch = 0;
    for (int mode = 0; mode < 3; ++mode)
        for (auto cm : { ChannelMode::Stereo, ChannelMode::MidSide })
            for (int twin = 0; twin < 2; ++twin)
                for (int block : { 1, 77, 512 })
                {
                    Smoother s;
                    s.setParamSet (twin);
                    prepare (s);
                    Values v;
                    v.both ("amount", 0.0f).both ("mode", (float) mode).both ("tone", 60.0f).both ("att", 1.0f);
                    v.both ("low", 20.0f).both ("high", 20000.0f);
                    s.setParams (v.data());
                    Stereo x { l, r };
                    ProcessContext ctx;
                    ctx.channelMode = cm;
                    ctx.paramSet = twin;
                    run (s, x, block, ctx);
                    int mismatch = 0;
                    for (size_t i = 0; i < l.size(); ++i)
                    {
                        const float el = i >= (size_t) kN ? l[i - kN] : 0.0f, er = i >= (size_t) kN ? r[i - kN] : 0.0f;
                        mismatch += (x.l[i] != el || x.r[i] != er) ? 1 : 0;
                    }
                    worstMismatch = std::max (worstMismatch, mismatch);
                }
    note ("samples differing from the input delayed by 1024 (all cases)", worstMismatch);
    CHECK_EQ (worstMismatch, 0);
}

TEST_CASE ("smth. a +9 dB resonance at 3 kHz over the threshold is cut, its neighbours under it are not")
{
    const size_t period = 1u << 17;
    const auto base = shapedNoise (period, 21u, -20.0, [] (double f) { return bandDb (f, 3000.0, 9.0); });
    const auto baseR = shapedNoise (period, 22u, -20.0, [] (double f) { return bandDb (f, 3000.0, 9.0); });
    const size_t total = (size_t) (8.0 * kSr);
    const auto inL = tiled (base, total), inR = tiled (baseR, total);

    for (auto mode : { SmootherMode::Medium, SmootherMode::Strong })
    {
        Smoother s;
        prepare (s);
        // the threshold 2 dB above the highest-reading neighbour (the detector reads lower bands higher)
        const auto v = harshValues (amountFor (detectorLevel (inL, inR, 1500.0) + 2.0), mode);
        s.setParams (v.data());
        Stereo x { inL, inR };
        const int b3k = bandNear (3000.0), b500 = bandNear (500.0), b15 = bandNear (1500.0), b6 = bandNear (6000.0);
        std::vector<double> gr3, gr15, gr6;
        double gr500 = 0.0;
        run (s, x, kHop, {}, [&] (size_t bi)
        {
            if ((double) bi * kHop < 3.0 * kSr)
                return;
            const auto& m = s.getMeters();
            gr3.push_back (m.grDb[0][b3k].load());
            gr15.push_back (m.grDb[0][b15].load());
            gr6.push_back (m.grDb[0][b6].load());
            gr500 = std::min (gr500, (double) m.grDb[0][b500].load());
        });
        const size_t st = (size_t) (3.0 * kSr), len = (size_t) (5.0 * kSr) - 8192;
        const double c3 = changeDb (inL, x.l, kN, st, len, third (3000.0, -1), third (3000.0, 1));
        const double c15 = changeDb (inL, x.l, kN, st, len, third (1500.0, -1), third (1500.0, 1));
        const double c6 = changeDb (inL, x.l, kN, st, len, third (6000.0, -1), third (6000.0, 1));
        const double c500 = changeDb (inL, x.l, kN, st, len, third (500.0, -1), third (500.0, 1));
        // a narrow probe at the 3 kHz band centre compared with that band's meter
        const double fc = smootherBandHz (b3k, kSr);
        const double probe = changeDb (inL, x.l, kN, st, len, fc - 20.0, fc + 20.0);
        const std::string tag = mode == SmootherMode::Medium ? "Medium" : "Strong";
        note (tag + ": change in the 1/3 oct at 3 kHz (dB)", c3);
        note (tag + ": change at 1.5 kHz (dB)", c15);
        note (tag + ": change at 6 kHz (dB)", c6);
        note (tag + ": change at 500 Hz, outside the region (dB)", c500);
        note (tag + ": mean meter GR at the 3 kHz band (dB)", meanGr (gr3));
        note (tag + ": measured change at that band's centre (dB)", probe);
        note (tag + ": mean meter GR 1.5 kHz (dB)", meanGr (gr15));
        note (tag + ": mean meter GR 6 kHz (dB)", meanGr (gr6));
        CHECK (c3 <= (mode == SmootherMode::Medium ? -1.5 : -1.8));   // the resonance only partly fills its Mel band
        CHECK (std::abs (c15) < 0.5);
        CHECK (std::abs (c6) < 0.5);
        CHECK (std::abs (c500) < 0.05);
        CHECK_EQ (gr500, 0.0);
        CHECK (std::abs (probe - meanGr (gr3)) < 0.75);
    }
}

TEST_CASE ("smth. pink noise under the threshold is untouched, over it the region is cut")
{
    // the reference's law: an absolute threshold that Amount moves (Smoother.h)
    const Stereo pink { pstest::pinkNoise (0.5, 6.0, kSr, 31u), pstest::pinkNoise (0.5, 6.0, kSr, 32u) };
    const double top = detectorLevel (pink.l, pink.r, 2000.0);   // the highest-reading band of the region
    for (auto mode : { SmootherMode::Gentle, SmootherMode::Medium, SmootherMode::Strong })
        for (double margin : { -4.0, 10.0 })   // threshold 4 dB above / 10 dB under the region's top level
        {
            Smoother s;
            prepare (s);
            const auto v = harshValues (amountFor (top - margin), mode, 2000.0f, 8000.0f);
            s.setParams (v.data());
            Stereo x = pink;
            double maxGr = 0.0, sumIn = 0.0, outside = 0.0;
            int countIn = 0;
            run (s, x, kHop, {}, [&] (size_t bi)
            {
                if ((double) bi * kHop < 1.0 * kSr)
                    return;
                const auto& m = s.getMeters();
                for (int band = 0; band < kSmootherBands; ++band)
                {
                    const double g = m.grDb[0][band].load(), hz = smootherBandHz (band, kSr);
                    maxGr = std::min (maxGr, g);
                    if (hz >= 2800.0 && hz <= 5700.0)
                    {
                        sumIn += g;
                        ++countIn;
                    }
                    if (hz < 500.0)
                        outside = std::min (outside, g);
                }
            });
            const std::string tag = std::string (mode == SmootherMode::Gentle ? "Gentle" : (mode == SmootherMode::Medium ? "Medium" : "Strong"))
                                    + (margin < 0.0 ? ", threshold above the material" : ", threshold 10 dB under it");
            note (tag + ": deepest band GR (dB)", maxGr);
            note (tag + ": mean GR inside the region (dB)", sumIn / std::max (1, countIn));
            if (margin < 0.0)
                CHECK (maxGr > -0.05);
            else
            {
                CHECK (sumIn / std::max (1, countIn) < -3.0);
                CHECK_EQ (outside, 0.0);
            }
        }
}

TEST_CASE ("smth. a mode change glides, meters agree with each other")
{
    const size_t period = 1u << 17;
    const auto base = shapedNoise (period, 191u, -20.0, [] (double f) { return bandDb (f, 3000.0, 12.0); });
    const size_t total = (size_t) (5.0 * kSr);
    const auto in = tiled (base, total);
    Smoother s, steady;
    prepare (s);
    prepare (steady);
    auto v = harshValues (100.0f, SmootherMode::Gentle);
    v.both ("att", 1.0f).both ("rel", 2.0f);   // fastest ballistics: only the mode glide smooths the mode step
    auto finalValues = v;
    finalValues.both ("mode", 2.0f);
    steady.setParams (finalValues.data());
    Stereo x { in, in }, control { in, in };
    const int b = bandNear (3000.0);
    std::vector<double> t, ordinary;
    bool metersConsistent = true;
    for (size_t pos = 0, blk = 0; pos + kHop <= total; pos += kHop, ++blk)
    {
        v.both ("mode", pos < (size_t) (2.5 * kSr) ? 0.0f : 2.0f);
        s.setParams (v.data());
        float* ch[2] { x.l.data() + pos, x.r.data() + pos };
        float* refCh[2] { control.l.data() + pos, control.r.data() + pos };
        s.process (ch, kHop, {});
        steady.process (refCh, kHop, {});
        const auto& m = s.getMeters();
        t.push_back (m.grDb[0][b].load());
        ordinary.push_back (steady.getMeters().grDb[0][b].load());
        double mn = 0.0, peak = -1000.0;
        for (int k = 0; k < kSmootherBands; ++k)
        {
            mn = std::min (mn, (double) m.grDb[0][k].load());
            if (smootherBandHz (k, kSr) >= 2000.0 && smootherBandHz (k, kSr) <= 10000.0)
                peak = std::max (peak, (double) m.bandLevelDb[0][k].load());
            metersConsistent = metersConsistent && m.grDb[0][k].load() == m.grDb[1][k].load()
                               && m.bandLevelDb[0][k].load() == m.bandLevelDb[1][k].load();
        }
        metersConsistent = metersConsistent && (float) mn == m.maxGrDb[0].load() && (float) peak == m.regionPeakDb[0].load();
    }
    const size_t sw = (size_t) (2.5 * kSr) / kHop;
    double maxStep = 0.0, maxAddedStep = 0.0;
    for (size_t i = sw; i < sw + 100 && i + 1 < t.size(); ++i)
    {
        maxStep = std::max (maxStep, std::abs (t[i + 1] - t[i]));
        // The noise's detector movement resumes after 300 ms. Only excess movement is a mode-switch artefact.
        maxAddedStep = std::max (maxAddedStep, std::abs (t[i + 1] - t[i]) - std::abs (ordinary[i + 1] - ordinary[i]));
    }
    double before = 0.0, after = 0.0;
    for (size_t i = sw - 40; i < sw; ++i)
        before += t[i] / 40.0;
    for (size_t i = t.size() - 40; i < t.size(); ++i)
        after += t[i] / 40.0;
    note ("GR before (Gentle) / after (Strong) the switch (dB)", before);
    note ("  after", after);
    note ("largest per-hop GR step across the switch and handoff (dB)", maxStep);
    note ("largest added step beyond ordinary detector movement (dB)", maxAddedStep);
    CHECK (after < before - 2.0);
    CHECK (maxAddedStep < 0.5);
    CHECK (metersConsistent);
}

TEST_CASE ("smth. mode glide ends on a continuously changing spectrum")
{
    const size_t total = (size_t) (5.0 * kSr);
    const size_t switchAt = ((size_t) kSr + kHop - 1) / kHop * kHop;
    const size_t glideEnd = switchAt + (size_t) std::ceil (0.3 * kSr / kHop) * kHop;
    auto in = pstest::pinkNoise (0.1, 5.0, kSr, 201u);
    for (size_t i = 0; i < total; ++i)
    {
        const double t = (double) i / kSr;
        const double level = 0.04 * (1.0 + 0.95 * std::sin (2.0 * pstest::kPi * 7.0 * t));
        in[i] += (float) (level * std::sin (2.0 * pstest::kPi * 3000.0 * t));
    }
    Smoother changed, settled;
    prepare (changed);
    prepare (settled);
    auto v = harshValues (amountFor (detectorLevel (in, in, 3000.0)), SmootherMode::Strong);
    v.both ("att", 1.0f).both ("rel", 2.0f);
    settled.setParams (v.data());
    Stereo a { in, in }, b { in, in };
    double maxDifference = 0.0, maxAudioDifference = 0.0;
    double minGr = 0.0, maxGr = -150.0;
    const int peakBand = bandNear (3000.0);
    for (size_t pos = 0; pos < total; pos += kHop)
    {
        v.both ("mode", pos < switchAt ? 0.0f : 2.0f);
        changed.setParams (v.data());
        const int n = (int) std::min<size_t> (kHop, total - pos);
        float* ca[2] { a.l.data() + pos, a.r.data() + pos };
        float* cb[2] { b.l.data() + pos, b.r.data() + pos };
        changed.process (ca, n, {});
        settled.process (cb, n, {});
        // Immediately after the 300 ms glide (rounded to hops), normal ballistics must be restored.
        if (pos >= glideEnd)
        {
            for (int band = 0; band < kSmootherBands; ++band)
                maxDifference = std::max (maxDifference, std::abs ((double) changed.getMeters().grDb[0][band].load()
                                                        - settled.getMeters().grDb[0][band].load()));
            const double g = settled.getMeters().grDb[0][peakBand].load();
            minGr = std::min (minGr, g);
            maxGr = std::max (maxGr, g);
        }
        // Allow the final transition frame to leave the engine's residual overlap-add buffer.
        if (pos >= glideEnd + kN)
            for (int i = 0; i < n; ++i)
                maxAudioDifference = std::max (maxAudioDifference, std::abs ((double) ca[0][i] - cb[0][i]));
    }
    note ("mode glide: ongoing GR variation (dB)", maxGr - minGr);
    note ("mode glide: residual meter difference after 300 ms (dB)", maxDifference);
    note ("mode glide: residual audio difference after 300 ms plus N (dBFS)", pstest::dbOf (maxAudioDifference));
    CHECK (maxGr - minGr > 2.0);
    CHECK (maxDifference < 1.0e-5);
    CHECK (maxAudioDifference < 1.0e-7);
}

TEST_CASE ("smth. same behaviour at 44.1 and 96 kHz")
{
    double tones[2] {};
    int rate = 0;
    for (double sr : { 44100.0, 96000.0 })
    {
        const size_t n = (size_t) (5.0 * sr);
        // pink + a 3 kHz resonance from the time-domain generators: a narrow resonant band from two close sines
        auto l = pstest::pinkNoise (0.5, 5.0, sr, 201u);
        l = pstest::added (l, pstest::sine (3000.0, -22.0, 5.0, sr));
        Smoother s;
        prepare (s, sr);
        const auto v = harshValues (100.0f, SmootherMode::Medium);
        s.setParams (v.data());
        Stereo x { l, l };
        run (s, x, 512, {});
        const int lat = s.getLatencySamples();
        const double tone = 20.0 * std::log10 (std::abs (pstest::toneOf (x.l, n / 2, n / 2 - 8192, 3000.0, sr))
                                                / std::abs (pstest::toneOf (l, n / 2 - (size_t) lat, n / 2 - 8192, 3000.0, sr)));
        note ("sample rate " + std::to_string ((int) sr) + ": latency", lat);
        note ("sample rate " + std::to_string ((int) sr) + ": change of a harsh 3 kHz tone (dB)", tone);
        CHECK_EQ (lat, sr > 50000.0 ? 2048 : 1024);
        CHECK (tone < -3.0);
        tones[rate++] = tone;
        // Amount 0 at this rate: exact delay
        Smoother z;
        prepare (z, sr);
        Values zero;
        z.setParams (zero.data());
        Stereo y { l, l };
        run (z, y, 333, {});
        bool exact = true;
        for (size_t i = (size_t) lat; i < n; ++i)
            exact = exact && y.l[i] == l[i - (size_t) lat];
        CHECK (exact);
    }
    note ("change of the harsh tone, 96 kHz - 44.1 kHz (dB)", tones[1] - tones[0]);
    CHECK (std::abs (tones[1] - tones[0]) < 0.5);
}

TEST_CASE ("smth. only cuts: every band gain <= 0 dB and no band gets louder under random sweeps")
{
    Smoother s;
    prepare (s);
    pstest::Lcg lcg (5u);
    const size_t total = (size_t) (6.0 * kSr);
    auto l = pstest::pinkNoise (0.5, 6.0, kSr, 41u);
    auto r = pstest::pinkNoise (0.5, 6.0, kSr, 42u);
    for (double f : { 440.0, 2500.0, 3300.0, 7000.0, 12000.0 })
    {
        l = pstest::added (l, pstest::sine (f, -18.0, 6.0, kSr));
        r = pstest::added (r, pstest::sine (f * 1.01, -20.0, 6.0, kSr));
    }
    Stereo x { l, r };
    double maxMeter = -1.0e9;
    ProcessContext ctx;
    for (size_t pos = 0, blk = 0; pos < total; ++blk)
    {
        Values v;
        for (int set = 0; set < 2; ++set)
        {
            const std::string p = std::string ("smth.") + (set ? "b." : "a.");
            v.set (p + "amount", 50.0f + 50.0f * lcg.next());
            v.set (p + "mode", (float) ((blk / 7) % 3));
            v.set (p + "att", 1.0f + 49.5f * (lcg.next() + 1.0f));
            v.set (p + "rel", 2.0f + 249.0f * (lcg.next() + 1.0f));
            v.set (p + "tone", 100.0f * lcg.next());
            v.set (p + "low", 200.0f + 1500.0f * (lcg.next() + 1.0f));
            v.set (p + "high", 8000.0f + 6000.0f * (lcg.next() + 1.0f));
        }
        s.setParams (v.data());
        ctx.channelMode = (blk / 40) % 2 ? ChannelMode::MidSide : ChannelMode::Stereo;
        const int n = (int) std::min<size_t> (256, total - pos);
        float* ch[2] { x.l.data() + pos, x.r.data() + pos };
        s.process (ch, n, ctx);
        pos += (size_t) n;
        const auto& m = s.getMeters();
        for (int set = 0; set < 2; ++set)
        {
            for (int b = 0; b < kSmootherBands; ++b)
                maxMeter = std::max (maxMeter, (double) m.grDb[set][b].load());
            maxMeter = std::max (maxMeter, (double) m.maxGrDb[set].load());
        }
    }
    note ("highest band gain seen on the meters (dB)", maxMeter);
    CHECK (maxMeter <= 0.0);
    double worst = -100.0;
    for (double fc = 100.0; fc < 16000.0; fc *= std::pow (2.0, 1.0 / 3.0))
    {
        worst = std::max (worst, changeDb (l, x.l, kN, (size_t) kSr, total - (size_t) kSr - 8192, third (fc, -1), third (fc, 1)));
        worst = std::max (worst, changeDb (r, x.r, kN, (size_t) kSr, total - (size_t) kSr - 8192, third (fc, -1), third (fc, 1)));
    }
    note ("largest 1/3-octave level change over the sweep (dB, must be <= ~0)", worst);
    CHECK (worst < 0.05);
}

TEST_CASE ("smth. modes are ordered and respect their maximum cut")
{
    // 1. ordering: a +12 dB third-octave band at 3.2 kHz over pink noise
    const size_t period = 1u << 17;
    double gr[3] {}, grMeter[3] {};
    {
        const auto base = shapedNoise (period, 51u, -24.0, [] (double f) { return bandDb (f, 3200.0, 12.0); });
        const auto baseR = shapedNoise (period, 52u, -24.0, [] (double f) { return bandDb (f, 3200.0, 12.0); });
        const size_t total = (size_t) (6.0 * kSr);
        const auto inL = tiled (base, total), inR = tiled (baseR, total);
        for (int mode = 0; mode < 3; ++mode)
        {
            Smoother s;
            prepare (s);
            const auto v = harshValues (100.0f, (SmootherMode) mode);
            s.setParams (v.data());
            Stereo x { inL, inR };
            const int b = bandNear (3200.0);
            std::vector<double> g;
            run (s, x, kHop, {}, [&] (size_t bi)
            {
                if ((double) bi * kHop > 2.0 * kSr)
                    g.push_back (s.getMeters().grDb[0][b].load());
            });
            gr[mode] = changeDb (inL, x.l, kN, (size_t) (2.0 * kSr), (size_t) (4.0 * kSr) - 8192, third (3200.0, -1), third (3200.0, 1));
            grMeter[mode] = meanGr (g);
        }
        note ("+12 dB band: change at 3.2 kHz, Gentle (dB)", gr[0]);
        note ("+12 dB band: change at 3.2 kHz, Medium (dB)", gr[1]);
        note ("+12 dB band: change at 3.2 kHz, Strong (dB)", gr[2]);
        CHECK (gr[0] > gr[1] && gr[1] > gr[2]);
        CHECK (grMeter[0] > grMeter[1] && grMeter[1] > grMeter[2]);
    }
    // 2. caps: an extreme resonance (a 3.2 kHz sine far above the local pink density) drives every mode to its cap
    {
        const auto bed = pstest::pinkNoise (0.05, 5.0, kSr, 53u);
        const auto in = pstest::added (bed, pstest::sine (3200.0, -6.0, 5.0, kSr));
        for (int mode = 0; mode < 3; ++mode)
        {
            Smoother s;
            prepare (s);
            const auto v = harshValues (100.0f, (SmootherMode) mode);
            s.setParams (v.data());
            Stereo x { in, in };
            const int b = bandNear (3200.0);
            double deepest = 0.0;
            std::vector<double> g;
            run (s, x, kHop, {}, [&] (size_t bi)
            {
                deepest = std::min (deepest, (double) s.getMeters().maxGrDb[0].load());
                if ((double) bi * kHop > 2.0 * kSr)
                    g.push_back (s.getMeters().grDb[0][b].load());
            });
            const double tone = 20.0 * std::log10 (std::abs (pstest::toneOf (x.l, (size_t) (2.0 * kSr), (size_t) (2.0 * kSr), 3200.0, kSr))
                                                    / std::abs (pstest::toneOf (in, (size_t) (2.0 * kSr) - kN, (size_t) (2.0 * kSr), 3200.0, kSr)));
            const double cap = mode == 0 ? 18.1 : (mode == 1 ? 17.6 : 19.8);   // M of each style (Smoother.h)
            const std::string tag = mode == 0 ? "Gentle" : (mode == 1 ? "Medium" : "Strong");
            note (tag + ": extreme sine, change of the tone (dB)", tone);
            note (tag + ": deepest reduction on the meters (dB)", deepest);
            note (tag + ": mean meter GR at the tone's band (dB)", meanGr (g));
            CHECK (deepest >= -cap - 1.0e-4);
            CHECK (tone >= -cap - 0.1);
            CHECK (meanGr (g) < -0.75 * cap);   // a tone this far over the threshold sits near the cap
        }
    }
}

TEST_CASE ("smth. tone: + less reduction of the highs, - more")
{
    const size_t period = 1u << 17;
    const auto base = shapedNoise (period, 61u, -22.0, [] (double f) { return bandDb (f, 8000.0, 10.0); });
    const auto baseR = shapedNoise (period, 62u, -22.0, [] (double f) { return bandDb (f, 8000.0, 10.0); });
    const size_t total = (size_t) (5.0 * kSr);
    const auto inL = tiled (base, total), inR = tiled (baseR, total);
    double g[3] {};
    const float tones[3] { -100.0f, 0.0f, 100.0f };
    for (int i = 0; i < 3; ++i)
    {
        Smoother s;
        prepare (s);
        auto v = harshValues (amountFor (detectorLevel (inL, inR, 8000.0) - 6.0), SmootherMode::Medium, 3000.0f, 16000.0f);
        v.both ("tone", tones[i]);
        s.setParams (v.data());
        Stereo x { inL, inR };
        run (s, x, kHop, {});
        g[i] = changeDb (inL, x.l, kN, (size_t) (2.0 * kSr), (size_t) (3.0 * kSr) - 8192, third (8000.0, -1), third (8000.0, 1));
    }
    note ("change at 8 kHz, Tone -100 (dB)", g[0]);
    note ("change at 8 kHz, Tone 0 (dB)", g[1]);
    note ("change at 8 kHz, Tone +100 (dB)", g[2]);
    CHECK (g[2] - g[1] >= 1.5);
    CHECK (g[1] - g[0] >= 1.5);
}

TEST_CASE ("smth. attack and release timing")
{
    // 3.2 kHz 1/3-octave noise bursts (+15 dB over a pink bed): per-hop meter of the burst band
    const size_t period = 1u << 17;
    const auto bed = shapedNoise (period, 71u, -24.0, [] (double) { return 0.0; });
    const auto burst = shapedNoise (period, 73u, -24.0, [] (double f) { return std::abs (std::log2 (f / 3200.0)) < 1.0 / 6.0 ? 0.0 : -200.0; });
    const double bedBand = 10.0 * std::log10 (bandPower (bed, 0, period, third (3200.0, -1), third (3200.0, 1)));
    const double burstBand = 10.0 * std::log10 (bandPower (burst, 0, period, third (3200.0, -1), third (3200.0, 1)));
    const float burstGain = (float) std::pow (10.0, (bedBand + 15.0 - burstBand) / 20.0);
    const size_t onset = (size_t) (2.0 * kSr) + 100, offset = onset + (size_t) (1.5 * kSr), total = offset + (size_t) (2.0 * kSr);
    std::vector<float> in (total);
    for (size_t i = 0; i < total; ++i)
        in[i] = bed[i % period] + (i >= onset && i < offset ? burstGain * burst[i % period] : 0.0f);
    const int b = bandNear (3200.0);
    const std::vector<float> steadyBurst (in.begin() + (std::ptrdiff_t) onset, in.begin() + (std::ptrdiff_t) offset);
    const float amount = amountFor (detectorLevel (steadyBurst, steadyBurst, 3200.0) - 6.0);

    // per-hop trace of the band's GR for given attack / release
    auto trace = [&] (float att, float rel)
    {
        Smoother s;
        prepare (s);
        auto v = harshValues (amount, SmootherMode::Medium);
        v.both ("att", att).both ("rel", rel);
        s.setParams (v.data());
        Stereo x { in, in };
        std::vector<double> t;
        run (s, x, kHop, {}, [&] (size_t) { t.push_back (s.getMeters().grDb[0][b].load()); });
        return t;
    };
    // hop index of the first frame ending after sample n
    auto hopAt = [] (size_t n) { return (int) (n / kHop); };
    auto firstAt = [] (const std::vector<double>& t, int from, const std::function<bool (double)>& pred)
    {
        for (int i = std::max (0, from); i < (int) t.size(); ++i)
            if (pred (t[(size_t) i]))
                return i;
        return (int) t.size();
    };

    const auto ref = trace (1.0f, 2.0f);
    const double finalRef = ref[(size_t) hopAt (offset) - 2];
    note ("steady GR of the burst band (dB)", finalRef);
    REQUIRE (finalRef < -3.0);
    const int a63Ref = firstAt (ref, hopAt (onset) - 8, [&] (double g) { return g <= 0.63 * finalRef; });
    const int r37Ref = firstAt (ref, hopAt (offset), [&] (double g) { return g >= 0.37 * finalRef; });
    const double hopMs = 1000.0 * kHop / kSr;
    for (float att : { 1.0f, 10.0f, 50.0f })
    {
        const auto t = trace (att, 30.0f);
        const double fin = t[(size_t) hopAt (offset) - 2];
        const int i63 = firstAt (t, hopAt (onset) - 8, [&] (double g) { return g <= 0.63 * fin; });
        const double ms = (i63 - a63Ref) * hopMs;
        note ("attack " + std::to_string ((int) att) + " ms: 63 % reached after (ms, relative to the 1 ms trace)", ms);
        CHECK (ms <= att + hopMs + 1.0e-9);
        if (att >= 50.0f)
            CHECK (ms >= 0.5 * att);
    }
    for (float rel : { 10.0f, 100.0f, 400.0f })
    {
        const auto t = trace (1.0f, rel);
        const double fin = t[(size_t) hopAt (offset) - 2];
        const int i37 = firstAt (t, hopAt (offset), [&] (double g) { return g >= 0.37 * fin; });
        const double ms = (i37 - r37Ref) * hopMs;
        note ("release " + std::to_string ((int) rel) + " ms: back to 37 % after (ms, relative to the 2 ms trace)", ms);
        CHECK (ms <= rel + hopMs + 1.0e-9);
        if (rel >= 100.0f)
            CHECK (ms >= 0.5 * rel);
    }
}

TEST_CASE ("smth. hits under the threshold pass; the attack decides how much of a short hit is caught")
{
    const size_t period = 1u << 17;
    const auto bed = shapedNoise (period, 81u, -26.0, [] (double) { return 0.0; });
    const auto hit = shapedNoise (period, 83u, -26.0, [] (double) { return 0.0; });
    const size_t total = (size_t) (6.0 * kSr);

    // 1. broadband hits (+15 dB, 30 ms, every 500 ms) with the threshold above them: untouched
    {
        std::vector<float> in (total);
        for (size_t i = 0; i < total; ++i)
        {
            const size_t ph = i % (size_t) (0.5 * kSr);
            const float env = ph < (size_t) (0.03 * kSr) ? (float) std::exp (-(double) ph / (0.01 * kSr)) : 0.0f;
            in[i] = bed[i % period] + 5.62f * env * hit[i % period];
        }
        Smoother s;
        prepare (s);
        // the region covers everything: the threshold 20 dB above the highest-reading band (the lowest)
        const auto v = harshValues (amountFor (detectorLevel (bed, bed, 100.0) + 20.0), SmootherMode::Medium, 20.0f, 20000.0f);
        s.setParams (v.data());
        Stereo x { in, in };
        double deepest = 0.0;
        run (s, x, kHop, {}, [&] (size_t bi) { if (bi > 100) deepest = std::min (deepest, (double) s.getMeters().maxGrDb[0].load()); });
        // peak level of each hit, output vs delayed input
        double worst = 0.0;
        for (size_t h = 2; h < 11; ++h)
        {
            const size_t st = h * (size_t) (0.5 * kSr);
            const double pin = pstest::peakOf (in, st, 2048), pout = pstest::peakOf (x.l, st + kN, 2048);
            worst = std::min (worst, 20.0 * std::log10 (pout / pin));
        }
        note ("broadband hits: deepest band GR (dB)", deepest);
        note ("broadband hits: worst peak change of a hit (dB)", worst);
        CHECK (deepest > -1.5);
        CHECK (worst > -1.0);
    }
    // 2. a narrow 3.2 kHz hit (+20 dB, 15 ms): a long attack lets it through, a short one catches it
    {
        const auto ring = shapedNoise (period, 85u, -26.0, [] (double f) { return std::abs (std::log2 (f / 3200.0)) < 1.0 / 6.0 ? 0.0 : -200.0; });
        const double g = std::pow (10.0, (10.0 * std::log10 (bandPower (bed, 0, period, third (3200.0, -1), third (3200.0, 1))
                                                   / bandPower (ring, 0, period, third (3200.0, -1), third (3200.0, 1))) + 20.0) / 20.0);
        std::vector<float> in (total);
        for (size_t i = 0; i < total; ++i)
        {
            const size_t ph = i % (size_t) (0.5 * kSr);
            in[i] = bed[i % period] + (ph < (size_t) (0.015 * kSr) ? (float) g * ring[i % period] : 0.0f);
        }
        double peakGr[2] {};
        const float atts[2] { 1.0f, 50.0f };
        std::vector<float> steadyHit ((size_t) (2.0 * kSr));
        for (size_t i = 0; i < steadyHit.size(); ++i)
            steadyHit[i] = bed[i % period] + (float) g * ring[i % period];
        const float amount = amountFor (detectorLevel (steadyHit, steadyHit, 3200.0) - 8.0);
        for (int i = 0; i < 2; ++i)
        {
            Smoother s;
            prepare (s);
            auto v = harshValues (amount, SmootherMode::Medium);
            v.both ("att", atts[i]).both ("rel", 30.0f);
            s.setParams (v.data());
            Stereo x { in, in };
            run (s, x, kHop, {}, [&] (size_t bi) { if (bi > 100) peakGr[i] = std::min (peakGr[i], (double) s.getMeters().grDb[0][bandNear (3200.0)].load()); });
        }
        note ("narrow 15 ms hit: peak GR, attack 1 ms (dB)", peakGr[0]);
        note ("narrow 15 ms hit: peak GR, attack 50 ms (dB)", peakGr[1]);
        CHECK (peakGr[0] < -3.0);
        CHECK (peakGr[1] > 0.6 * peakGr[0]);
    }
}

TEST_CASE ("smth. no musical noise")
{
    const size_t period = 1u << 17;
    const auto base = shapedNoise (period, 91u, -20.0, [] (double f) { return bandDb (f, 3000.0, 9.0) + bandDb (f, 7000.0, 6.0); });
    const auto baseR = shapedNoise (period, 92u, -20.0, [] (double f) { return bandDb (f, 3000.0, 9.0) + bandDb (f, 7000.0, 6.0); });
    const size_t total = (size_t) (5.0 * kSr);
    const auto inL = tiled (base, total), inR = tiled (baseR, total);
    for (int mode = 0; mode < 3; ++mode)
    {
        Smoother s;
        prepare (s);
        const auto v = harshValues (100.0f, (SmootherMode) mode, 1000.0f, 16000.0f);
        s.setParams (v.data());
        Stereo x { inL, inR };
        std::vector<std::vector<double>> g (kSmootherBands);
        run (s, x, kHop, {}, [&] (size_t bi)
        {
            if ((double) bi * kHop >= 2.0 * kSr)
                for (int b = 0; b < kSmootherBands; ++b)
                    g[(size_t) b].push_back (s.getMeters().grDb[0][b].load());
        });
        double worstStd = 0.0;
        for (const auto& t : g)
        {
            const double m = meanGr (t);
            double var = 0.0;
            for (double y : t)
                var += (y - m) * (y - m);
            worstStd = std::max (worstStd, std::sqrt (var / std::max<size_t> (1, t.size())));
        }
        // spectral flux (mean |dB difference| of consecutive 2048-point frames, 1 .. 16 kHz), output vs delayed input
        auto flux = [] (const std::vector<float>& y, size_t start, size_t len)
        {
            const size_t n = 2048;
            std::vector<pstest::cd> a (n);
            std::vector<double> prev, cur (n / 2);
            double sum = 0.0;
            int count = 0;
            for (size_t st = start; st + n <= start + len; st += n / 2)
            {
                for (size_t i = 0; i < n; ++i)
                    a[i] = (double) y[st + i] * (0.5 - 0.5 * std::cos (2.0 * pstest::kPi * (double) i / (double) n));
                pstest::fftInPlace (a);
                for (size_t k = 0; k < n / 2; ++k)
                    cur[k] = 10.0 * std::log10 (std::norm (a[k]) + 1.0e-20);
                if (! prev.empty())
                    for (size_t k = (size_t) (1000.0 * n / kSr); k < (size_t) (16000.0 * n / kSr); ++k, ++count)
                        sum += std::abs (cur[k] - prev[k]);
                prev = cur;
            }
            return sum / std::max (1, count);
        };
        std::vector<float> delayed (total, 0.0f);
        for (size_t i = kN; i < total; ++i)
            delayed[i] = inL[i - kN];
        const double fIn = flux (delayed, (size_t) (2.0 * kSr), (size_t) (3.0 * kSr) - 4096);
        const double fOut = flux (x.l, (size_t) (2.0 * kSr), (size_t) (3.0 * kSr) - 4096);
        const std::string tag = mode == 0 ? "Gentle" : (mode == 1 ? "Medium" : "Strong");
        note (tag + ": worst per-band GR std over 3 s (dB)", worstStd);
        note (tag + ": spectral flux out / in", fOut / fIn);
        CHECK (worstStd < 0.75);
        CHECK (fOut / fIn < 1.1);
    }
}

TEST_CASE ("smth. region solo: hear only the input inside the region, same latency")
{
    const size_t period = 1u << 17;
    const auto base = shapedNoise (period, 101u, -20.0, [] (double) { return 0.0; });
    const auto baseR = shapedNoise (period, 102u, -20.0, [] (double) { return 0.0; });
    const size_t total = (size_t) (4.0 * kSr);
    const auto inL = tiled (base, total), inR = tiled (baseR, total);
    for (float amount : { 0.0f, 80.0f })
    {
        Smoother s;
        prepare (s);
        const auto v = harshValues (amount, SmootherMode::Medium, 1000.0f, 4000.0f);
        s.setParams (v.data());
        ModuleMonitor m;
        m.soloRegion = true;
        s.setMonitor (m);
        Stereo x { inL, inR };
        run (s, x, 512, {});
        const size_t st = (size_t) kSr, len = (size_t) (3.0 * kSr) - 8192;
        double inside = 0.0;
        for (double fc = 1260.0; fc < 3200.0; fc *= std::pow (2.0, 1.0 / 6.0))
            inside = std::max (inside, std::abs (changeDb (inL, x.l, kN, st, len, fc / 1.06, fc * 1.06)));
        const double below = changeDb (inL, x.l, kN, st, len, 40.0, 500.0);
        const double above = changeDb (inL, x.l, kN, st, len, 8000.0, 20000.0);
        const std::string tag = "Amount " + std::to_string ((int) amount);
        note (tag + ": worst change inside 1.2 .. 3.3 kHz (dB)", inside);
        note (tag + ": level below 500 Hz (dB)", below);
        note (tag + ": level above 8 kHz (dB)", above);
        CHECK (inside < 0.5);
        CHECK (below < -30.0);
        CHECK (above < -30.0);
        CHECK_EQ (s.getLatencySamples(), kN);
        // the latency is unchanged: the solo output correlates best with the input delayed by exactly N
        double best = -1.0;
        int bestLag = -1;
        for (int lag = kN - 8; lag <= kN + 8; ++lag)
        {
            double c = 0.0;
            for (size_t i = st; i < st + 48000; ++i)
                c += (double) x.l[i] * inL[i - (size_t) lag];
            if (c > best)
            {
                best = c;
                bestLag = lag;
            }
        }
        CHECK_EQ (bestLag, kN);
    }
}

TEST_CASE ("smth. learn finds the harsh region and does not touch the audio")
{
    const size_t period = 1u << 17;
    const auto base = shapedNoise (period, 111u, -20.0, [] (double f) { return bandDb (f, 5000.0, 10.0); });
    const auto baseR = shapedNoise (period, 112u, -20.0, [] (double f) { return bandDb (f, 5000.0, 10.0); });
    const size_t total = (size_t) (7.0 * kSr);
    const auto inL = tiled (base, total), inR = tiled (baseR, total);

    auto runOnce = [&] (bool learn, Smoother& s, Stereo& x)
    {
        prepare (s);
        const auto v = harshValues (40.0f, SmootherMode::Medium, 200.0f, 1000.0f);   // a region far from the peak
        s.setParams (v.data());
        ModuleMonitor m;
        m.learnRegion = learn;
        long long allocs = 0;
        {
            pstest::AllocationScope scope;
            s.setMonitor (m);
            run (s, x, 512, {});
            allocs = scope.count();
        }
        return allocs;
    };
    Smoother a, b;
    Stereo xa { inL, inR }, xb { inL, inR };
    const long long allocs = runOnce (true, a, xa);
    runOnce (false, b, xb);
    CHECK_EQ (allocs, 0LL);
    CHECK (xa.l == xb.l && xa.r == xb.r);

    double lo = 0.0, hi = 0.0;
    learnedRegion (a.getMeters(), 0, lo, hi);
    note ("learned region low (Hz)", lo);
    note ("learned region high (Hz)", hi);
    note ("learned span (octaves)", std::log2 (hi / lo));
    CHECK (lo <= 5000.0 && hi >= 5000.0);
    CHECK (std::log2 (hi / lo) <= 2.0);
    // Stereo: set B mirrors set A
    bool mirror = true;
    for (int band = 0; band < kSmootherBands; ++band)
        mirror = mirror && a.getMeters().learnScore[0][band].load() == a.getMeters().learnScore[1][band].load();
    CHECK (mirror);
    double total0 = 0.0;
    for (int band = 0; band < kSmootherBands; ++band)
        total0 += b.getMeters().learnScore[0][band].load();
    CHECK_EQ (total0, 0.0);   // without learn nothing accumulates

    // restarting learn clears the scores
    ModuleMonitor off, on;
    on.learnRegion = true;
    a.setMonitor (off);
    a.setMonitor (on);
    double after = 0.0;
    for (int band = 0; band < kSmootherBands; ++band)
        after += a.getMeters().learnScore[0][band].load();
    CHECK_EQ (after, 0.0);
}

TEST_CASE ("smth. stereo link and M/S sets")
{
    const size_t period = 1u << 17;
    const auto harsh = shapedNoise (period, 121u, -20.0, [] (double f) { return bandDb (f, 3000.0, 12.0); });
    const auto plain = shapedNoise (period, 122u, -20.0, [] (double) { return 0.0; });
    const size_t total = (size_t) (5.0 * kSr);
    const auto inL = tiled (harsh, total), inR = tiled (plain, total);
    const size_t st = (size_t) (2.0 * kSr), len = (size_t) (3.0 * kSr) - 8192;

    // Stereo: L-only harshness, R receives the same band gains
    {
        Smoother s;
        prepare (s);
        const auto v = harshValues (100.0f, SmootherMode::Strong);
        s.setParams (v.data());
        Stereo x { inL, inR };
        run (s, x, 512, {});
        const double cl = changeDb (inL, x.l, kN, st, len, 2900.0, 3100.0);
        const double cr = changeDb (inR, x.r, kN, st, len, 2900.0, 3100.0);
        note ("Stereo: change at 3 kHz, L (harsh) (dB)", cl);
        note ("Stereo: change at 3 kHz, R (plain probe) (dB)", cr);
        CHECK (cl < -2.0);
        CHECK (std::abs (cl - cr) < 0.3);
    }
    // M/S: set A (Mid) acts, set B (Side) at Amount 0 passes (within float rounding of the shared FFT)
    {
        Smoother s;
        prepare (s);
        auto v = harshValues (100.0f, SmootherMode::Strong);
        v.set ("smth.b.amount", 0.0f);
        s.setParams (v.data());
        Stereo x { inL, inL };
        ProcessContext ctx;
        ctx.channelMode = ChannelMode::MidSide;
        run (s, x, 512, ctx);
        double err = 0.0;
        for (size_t i = kN; i < total; ++i)
            err = std::max (err, (double) std::abs (x.r[i] - inL[i - kN]));
        const double cm = changeDb (inL, x.l, kN, st, len, 2900.0, 3100.0);
        note ("M/S: change at 3 kHz on ch0 / set A (dB)", cm);
        note ("M/S: ch1 / set B at Amount 0, max error vs delayed input (dBFS)", pstest::dbOf (err));
        CHECK (cm < -2.0);
        CHECK (err < 1.0e-6);
        // meters: set A row reduces, set B row is at rest
        CHECK (s.getMeters().maxGrDb[0].load() < -1.0f);
        CHECK_EQ (s.getMeters().maxGrDb[1].load(), 0.0f);
    }
    // M/S with the sets swapped: ch1 acts, ch0 passes
    {
        Smoother s;
        prepare (s);
        auto v = harshValues (100.0f, SmootherMode::Strong);
        v.set ("smth.a.amount", 0.0f);
        s.setParams (v.data());
        Stereo x { inR, inL };
        ProcessContext ctx;
        ctx.channelMode = ChannelMode::MidSide;
        run (s, x, 512, ctx);
        const double c1 = changeDb (inL, x.r, kN, st, len, 2900.0, 3100.0);
        const double c0 = changeDb (inR, x.l, kN, st, len, 2900.0, 3100.0);
        note ("M/S swapped: change at 3 kHz ch1 / ch0 (dB)", c1);
        CHECK (c1 < -2.0);
        CHECK (std::abs (c0) < 0.01);
    }
}

TEST_CASE ("smth. T/S twin reads set B for both channels")
{
    const size_t period = 1u << 17;
    const auto harsh = shapedNoise (period, 131u, -20.0, [] (double f) { return bandDb (f, 3000.0, 12.0); });
    const size_t total = (size_t) (3.0 * kSr);
    const auto in = tiled (harsh, total);

    Values v;   // set A neutral, set B active
    v.set ("smth.b.amount", 90.0f).set ("smth.b.mode", 2.0f).set ("smth.b.low", 1500.0f).set ("smth.b.high", 9000.0f);
    Values ref;   // the same settings as set A
    ref.set ("smth.a.amount", 90.0f).set ("smth.a.mode", 2.0f).set ("smth.a.low", 1500.0f).set ("smth.a.high", 9000.0f);

    Smoother main, twin, expected;
    twin.setParamSet (1);
    CHECK_EQ (twin.getParamSet(), 1);
    prepare (main);
    prepare (twin);
    prepare (expected);
    main.setParams (v.data());
    twin.setParams (v.data());
    expected.setParams (ref.data());
    Stereo xm { in, in }, xt { in, in }, xe { in, in };
    ProcessContext c0, c1;
    c1.paramSet = 1;
    run (main, xm, 256, c0);
    run (twin, xt, 256, c1);
    run (expected, xe, 256, c0);
    bool mainNull = true;
    for (size_t i = kN; i < total; ++i)
        mainNull = mainNull && xm.l[i] == in[i - kN] && xm.r[i] == in[i - kN];
    CHECK (mainNull);
    CHECK (xt.l == xe.l && xt.r == xe.r);
    const double c = changeDb (in, xt.l, kN, (size_t) kSr, (size_t) (2.0 * kSr) - 8192, 2900.0, 3100.0);
    note ("twin (set B active): change at 3 kHz (dB)", c);
    CHECK (c < -2.0);
    CHECK (twin.getMeters().maxGrDb[1].load() == twin.getMeters().maxGrDb[0].load());
}

TEST_CASE ("smth. parameter automation is smooth: no HF splatter, no NaN")
{
    // band-limited input (< 5 kHz): anything above 12 kHz in the output is splatter from the gain changes
    const size_t period = 1u << 17;
    const auto base = shapedNoise (period, 141u, -18.0, [] (double f) { return bandDb (f, 3000.0, 12.0); }, 20.0, 5000.0);
    const size_t total = (size_t) (6.0 * kSr);
    const auto in = tiled (base, total);
    const double inLevel = 10.0 * std::log10 (bandPower (in, (size_t) kSr, (size_t) (4.0 * kSr), 20.0, 5000.0));
    for (int block : { 64, 512 })
    {
        Smoother s;
        prepare (s);
        pstest::Lcg lcg (9u);
        Stereo x { in, in };
        ProcessContext ctx;
        bool finite = true;
        for (size_t pos = 0, blk = 0; pos < total; ++blk)
        {
            Values v;
            v.both ("amount", (blk / 8) % 2 ? 100.0f : 0.0f);
            v.both ("mode", (float) ((blk / 5) % 3));
            v.both ("tone", 100.0f * lcg.next());
            v.both ("att", 1.0f + 49.0f * (lcg.next() + 1.0f));
            v.both ("rel", 2.0f + 100.0f * (lcg.next() + 1.0f));
            v.both ("low", 1000.0f + 800.0f * (lcg.next() + 1.0f));
            v.both ("high", 4000.0f + 3000.0f * (lcg.next() + 1.0f));
            s.setParams (v.data());
            ModuleMonitor m;
            m.soloRegion = (blk / 50) % 4 == 3;
            s.setMonitor (m);
            const int n = (int) std::min<size_t> ((size_t) block, total - pos);
            float* ch[2] { x.l.data() + pos, x.r.data() + pos };
            s.process (ch, n, ctx);
            for (int i = 0; i < n; ++i)
                finite = finite && std::isfinite (ch[0][i]) && std::isfinite (ch[1][i]);
            pos += (size_t) n;
        }
        const double hf = 10.0 * std::log10 (bandPower (x.l, (size_t) kSr, (size_t) (4.0 * kSr), 12000.0, 23000.0) + 1.0e-30);
        Stereo still { in, in };
        Smoother t;
        prepare (t);
        const auto v = harshValues (100.0f, SmootherMode::Strong, 1000.0f, 7000.0f);
        t.setParams (v.data());
        run (t, still, block, ctx);
        const double hfStill = 10.0 * std::log10 (bandPower (still.l, (size_t) kSr, (size_t) (4.0 * kSr), 12000.0, 23000.0) + 1.0e-30);
        note ("block " + std::to_string (block) + ": HF (> 12 kHz) under random automation, relative to the signal (dB)", hf - inLevel);
        note ("block " + std::to_string (block) + ": HF with static settings, relative (dB)", hfStill - inLevel);
        CHECK (finite);
        CHECK (hf - inLevel < -80.0);
    }
}

TEST_CASE ("smth. silence, extremes and denormals")
{
    Smoother s;
    prepare (s);
    const auto v = harshValues (100.0f, SmootherMode::Strong, 20.0f, 20000.0f);
    s.setParams (v.data());
    ProcessContext ctx;
    // noise, then silence: exactly 0 once every frame holding the noise has left the output
    {
        auto l = pstest::concat (pstest::pinkNoise (0.8, 1.0, kSr, 151u), std::vector<float> ((size_t) kSr, 0.0f));
        Stereo x { l, l };
        run (s, x, 300, ctx);
        double tail = 0.0;
        for (size_t i = (size_t) kSr + 2 * kN + kHop; i < x.l.size(); ++i)
            tail = std::max (tail, (double) std::abs (x.l[i]) + std::abs (x.r[i]));
        note ("max |output| after noise -> silence (2N + H later)", tail);
        CHECK_EQ (tail, 0.0);
    }
    // full-scale square, DC, 1e-30 and denormal inputs
    {
        const size_t n = (size_t) (2.0 * kSr);
        std::vector<float> sq (n), dc (n, 1.0f), tiny (n, 1.0e-30f), den (n);
        for (size_t i = 0; i < n; ++i)
        {
            sq[i] = (i / 60) % 2 ? 1.0f : -1.0f;
            den[i] = (i % 2 ? 1.0f : -1.0f) * 1.0e-40f;
        }
        for (auto* sig : { &sq, &dc, &tiny, &den })
        {
            Smoother t;
            prepare (t);
            t.setParams (v.data());
            Stereo x { *sig, *sig };
            run (t, x, 512, ctx);
            bool finite = true;
            double peak = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                finite = finite && std::isfinite (x.l[i]) && std::isfinite (x.r[i]);
                peak = std::max (peak, (double) std::abs (x.l[i]));
            }
            CHECK (finite);
            CHECK (peak <= 4.0);
            for (int b = 0; b < kSmootherBands; ++b)
                CHECK (std::isfinite (t.getMeters().grDb[0][b].load()) && t.getMeters().grDb[0][b].load() <= 0.0f);
        }
        note ("square wave: no NaN / Inf, |y| <= 4", 1.0);
    }
}

TEST_CASE ("smth. amount back to 0 returns to an exact null after the release")
{
    const size_t period = 1u << 17;
    const auto base = shapedNoise (period, 161u, -20.0, [] (double f) { return bandDb (f, 3000.0, 12.0); });
    const size_t total = (size_t) (6.0 * kSr);
    const auto in = tiled (base, total);
    Smoother s;
    prepare (s);
    auto v = harshValues (100.0f, SmootherMode::Strong);
    v.both ("rel", 50.0f);
    Stereo x { in, in };
    ProcessContext ctx;
    for (size_t pos = 0; pos < total; pos += 512)
    {
        v.both ("amount", pos < (size_t) (2.0 * kSr) ? 100.0f : 0.0f);
        s.setParams (v.data());
        float* ch[2] { x.l.data() + pos, x.r.data() + pos };
        s.process (ch, (int) std::min<size_t> (512, total - pos), ctx);
    }
    // the reduction is gone within ~20 release time constants + one frame
    size_t firstExact = total;
    for (size_t i = total; i-- > (size_t) (2.0 * kSr);)
    {
        if (x.l[i] != in[i - kN] || x.r[i] != in[i - kN])
            break;
        firstExact = i;
    }
    note ("exact null again after Amount -> 0 (ms)", 1000.0 * ((double) firstExact - 2.0 * kSr) / kSr);
    CHECK (firstExact < (size_t) (3.5 * kSr));
    CHECK_EQ (s.getMeters().maxGrDb[0].load(), 0.0f);
}

TEST_CASE ("smth. no allocation in setParams / setMonitor / process / reset")
{
    for (auto cm : { ChannelMode::Stereo, ChannelMode::MidSide })
        for (int twin = 0; twin < 2; ++twin)
        {
            Smoother s;
            s.setParamSet (twin);
            s.prepare (kSr, 4096);
            auto v = harshValues (80.0f, SmootherMode::Strong);
            auto x = pstest::added (pstest::pinkNoise (0.5, 1.0, kSr, 171u), pstest::sine (3000.0, -12.0, 1.0, kSr));
            Stereo st { x, x };
            ProcessContext ctx;
            ctx.channelMode = cm;
            ctx.paramSet = twin;
            long long count = 0;
            {
                pstest::AllocationScope scope;
                size_t pos = 0;
                int i = 0;
                for (int block : { 1, 7, 64, 512, 4096, 333, 1, 4096 })
                {
                    for (int rep = 0; rep < 3 && pos + (size_t) block <= x.size(); ++rep, ++i)
                    {
                        v.both ("mode", (float) (i % 3)).both ("tone", (float) (i * 13 % 200 - 100));
                        s.setParams (v.data());
                        ModuleMonitor m;
                        m.soloRegion = i % 4 == 1;
                        m.learnRegion = i % 3 == 0;
                        s.setMonitor (m);
                        float* ch[2] { st.l.data() + pos, st.r.data() + pos };
                        s.process (ch, block, ctx);
                        pos += (size_t) block;
                    }
                }
                s.reset();
                float* ch[2] { st.l.data(), st.r.data() };
                s.process (ch, 512, ctx);
                count = scope.count();
            }
            CHECK_EQ (count, 0LL);
        }
}

TEST_CASE ("smth. cpu (x realtime, 48 kHz stereo)")
{
    const double seconds = 30.0;
    const auto l = pstest::pinkNoise (0.5, seconds, kSr, 181u);
    const auto r = pstest::pinkNoise (0.5, seconds, kSr, 182u);
    struct Case { const char* name; ChannelMode cm; bool solo; float amount; };
    for (const Case& c : { Case { "typical (Stereo, Amount 50, Medium)", ChannelMode::Stereo, false, 50.0f },
                           Case { "worst (M/S, Amount 100, Strong, full range, learn)", ChannelMode::MidSide, false, 100.0f } })
    {
        Smoother s;
        prepare (s, kSr, 512);
        auto v = harshValues (c.amount, c.cm == ChannelMode::MidSide ? SmootherMode::Strong : SmootherMode::Medium,
                              c.cm == ChannelMode::MidSide ? 20.0f : 2000.0f, c.cm == ChannelMode::MidSide ? 20000.0f : 10000.0f);
        s.setParams (v.data());
        ModuleMonitor m;
        m.learnRegion = c.cm == ChannelMode::MidSide;
        s.setMonitor (m);
        Stereo x { l, r };
        ProcessContext ctx;
        ctx.channelMode = c.cm;
        pstest::Stopwatch w;
        run (s, x, 512, ctx);
        const double rt = seconds / std::max (1.0e-9, w.seconds());
        note (std::string (c.name) + ": x realtime", rt);
        CHECK (rt >= 80.0);
    }
}

TEST_CASE ("smth. learned region helper: empty scores, a single band, the 75 % span")
{
    SmootherMeters m;
    double lo = -1.0, hi = -1.0;
    CHECK (! smootherLearnedRegion (m, 0, kSr, lo, hi));   // nothing learned: unchanged
    CHECK_EQ (lo, -1.0);
    CHECK_EQ (hi, -1.0);

    // One band: its centre +- half a band (geometric means with the neighbours)
    m.learnScore[1][20].store (3.0f);
    CHECK (smootherLearnedRegion (m, 1, kSr, lo, hi));
    CHECK (std::abs (lo - (std::sqrt (smootherBandHz (19, kSr) * smootherBandHz (20, kSr)))) < 1.0e-6);
    CHECK (std::abs (hi - (std::sqrt (smootherBandHz (20, kSr) * smootherBandHz (21, kSr)))) < 1.0e-6);
    CHECK (! smootherLearnedRegion (m, 0, kSr, lo, hi));   // the other set is still empty

    // 80 % of the score in bands 10..12, the rest spread: the span is 10..12; the top band mirrors its ratio
    for (int b = 0; b < kSmootherBands; ++b)
        m.learnScore[0][b].store (b >= 10 && b <= 12 ? 8.0f : 6.0f / 29.0f);
    CHECK (smootherLearnedRegion (m, 0, kSr, lo, hi));
    CHECK (std::abs (lo - (std::sqrt (smootherBandHz (9, kSr) * smootherBandHz (10, kSr)))) < 1.0e-6);
    CHECK (std::abs (hi - (std::sqrt (smootherBandHz (12, kSr) * smootherBandHz (13, kSr)))) < 1.0e-6);

    SmootherMeters top;
    top.learnScore[0][kSmootherBands - 1].store (1.0f);
    CHECK (smootherLearnedRegion (top, 0, kSr, lo, hi));
    const double c = smootherBandHz (kSmootherBands - 1, kSr), below = smootherBandHz (kSmootherBands - 2, kSr);
    CHECK (std::abs (hi - (c * std::sqrt (c / below))) < 1.0e-6);
}
