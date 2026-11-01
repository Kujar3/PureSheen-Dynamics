#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/LowContrast.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Low Contrast (dsp/LowContrast.cpp). Prefix "lowc.".
namespace
{
using namespace ps;
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
        const int i = paramIndex (id);
        REQUIRE (i >= 0);
        v[(size_t) i] = x;
    }
    void both (const std::string& name, float x)
    {
        set ("lowc.a." + name, x);
        set ("lowc.b." + name, x);
    }
    ParamValues data() const { return v.data(); }
};

struct Stereo
{
    std::vector<float> l, r;
};

Stereo dual (const std::vector<float>& x) { return { x, x }; }

using BlockHook = std::function<void (size_t, Values&, ModuleMonitor&)>;

Stereo run (LowContrast& m, Values& vals, const Stereo& in, int block, ChannelMode mode = ChannelMode::Stereo,
            const BlockHook& onBlock = {})
{
    Stereo out = in;
    ProcessContext ctx;
    ctx.channelMode = mode;
    ctx.paramSet = m.getParamSet();
    ModuleMonitor mon;
    for (size_t start = 0; start < in.l.size(); start += (size_t) block)
    {
        if (onBlock)
        {
            onBlock (start, vals, mon);
            m.setMonitor (mon);
        }
        m.setParams (vals.data());
        const int n = (int) std::min ((size_t) block, in.l.size() - start);
        float* ch[2] { out.l.data() + start, out.r.data() + start };
        m.process (ch, n, ctx);
    }
    return out;
}

std::unique_ptr<LowContrast> make (double sr = kSr, int paramSet = 0)
{
    auto m = std::make_unique<LowContrast>();
    m->setParamSet (paramSet);
    m->prepare (sr, 4096);
    return m;
}

// 50 Hz "kick": burst with a 2 ms raised-cosine onset and a 120 ms exponential decay every 500 ms (peak ~peakDb),
// plus a steady 60 Hz bass sine 12 dB below.
std::vector<float> kickLoop (double sr, double seconds, double peakDb, bool withBass = true, double firstOnset = 0.0)
{
    const size_t n = (size_t) (seconds * sr);
    std::vector<float> y (n, 0.0f);
    const double a = pstest::dbfsToAmplitude (peakDb), bass = pstest::dbfsToAmplitude (peakDb - 12.0);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        double v = withBass ? bass * std::sin (2.0 * kPi * 60.0 * t) : 0.0;
        if (t >= firstOnset)
        {
            const double tb = std::fmod (t - firstOnset, 0.5);
            const double onset = tb < 0.002 ? 0.5 - 0.5 * std::cos (kPi * tb / 0.002) : 1.0;
            v += a * onset * std::sin (2.0 * kPi * 50.0 * tb) * std::exp (-tb / 0.12);
        }
        y[i] = (float) v;
    }
    return y;
}

// Mean (over bursts with onset in [from, to) seconds) of the 20 ms RMS at the burst peak over the 20 ms RMS mid-gap.
double burstGapDb (const std::vector<float>& y, size_t offset, double sr, double from, double to)
{
    double sum = 0.0;
    int count = 0;
    const auto win = (size_t) (0.02 * sr);
    for (double tb = 0.0; tb < to; tb += 0.5)
    {
        if (tb < from)
            continue;
        const size_t p = offset + (size_t) (tb * sr);
        const size_t g = offset + (size_t) ((tb + 0.24) * sr);
        if (g + win > y.size())
            break;
        sum += pstest::dbOf (pstest::rmsOf (y, p, win)) - pstest::dbOf (pstest::rmsOf (y, g, win));
        ++count;
    }
    return count > 0 ? sum / count : 0.0;
}

double maxDelayedDiff (const std::vector<float>& out, const std::vector<float>& in, int latency, size_t from = 0)
{
    double m = 0.0;
    for (size_t i = std::max (from, (size_t) latency); i < out.size(); ++i)
        m = std::max (m, std::abs ((double) out[i] - (double) in[i - (size_t) latency]));
    return m;
}

bool bitExactDelayed (const std::vector<float>& out, const std::vector<float>& in, int latency, size_t from = 0)
{
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (i < from)
            continue;
        const float expect = i >= (size_t) latency ? in[i - (size_t) latency] : 0.0f;
        if (out[i] != expect)
            return false;
    }
    return true;
}

// 8th-order Butterworth high-pass (4 RBJ biquads, double) for measuring what leaks above the region.
std::vector<double> highPass8 (const std::vector<double>& x, double fc, double sr)
{
    std::vector<double> y = x;
    for (const double q : { 0.50979558, 0.60134489, 0.89997622, 2.56291545 })
    {
        const double w0 = 2.0 * kPi * fc / sr, cw = std::cos (w0), al = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + al;
        const double b0 = (1.0 + cw) / 2.0 / a0, b1 = -(1.0 + cw) / a0, b2 = b0;
        const double a1 = -2.0 * cw / a0, a2 = (1.0 - al) / a0;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& v : y)
        {
            const double in = v;
            const double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = in;
            y2 = y1;
            y1 = out;
            v = out;
        }
    }
    return y;
}

std::vector<double> residual (const std::vector<float>& out, const std::vector<float>& in, int latency)
{
    std::vector<double> r (out.size(), 0.0);
    for (size_t i = (size_t) latency; i < out.size(); ++i)
        r[i] = (double) out[i] - (double) in[i - (size_t) latency];
    return r;
}

double maxAbs (const std::vector<double>& x, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = from; i < std::min (to, x.size()); ++i)
        m = std::max (m, std::abs (x[i]));
    return m;
}

double toneDb (const std::vector<float>& y, size_t start, size_t n, double f, double sr)
{
    return pstest::dbOf (std::abs (pstest::toneOf (y, start, n, f, sr)));
}

// whole cycles of f in about `seconds`
size_t wholeCycles (double f, double seconds, double sr) { return (size_t) (std::floor (seconds * f) * sr / f); }

// The contrast effect of the spec's kick test: change of the burst / gap ratio (dB, output minus input).
double contrastEffectDb (LowContrastMode mode, float contrast, double peakDb, bool autoGain = false, double sr = kSr)
{
    auto m = make (sr);
    Values v;
    v.both ("mode", (float) mode);
    v.both ("contrast", contrast);
    if (autoGain)
        v.set ("lowc.autoGain", 1.0f);
    const auto x = kickLoop (sr, 8.0, peakDb);
    const auto y = run (*m, v, dual (x), 512);
    const int lat = m->getLatencySamples();
    return burstGapDb (y.l, (size_t) lat, sr, 3.0, 7.5) - burstGapDb (x, 0, sr, 3.0, 7.5);
}
} // namespace

//==============================================================================
TEST_CASE ("lowc. FFT size and latency per sample rate; latency equals the impulse delay for any block size")
{
    const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const int sizes[] { 4096, 4096, 8192, 8192, 16384, 16384 };
    for (int r = 0; r < 6; ++r)
    {
        for (const int block : { 1, 37, 512, 4096 })
        {
            if (block == 1 && rates[r] > 100000.0)
                continue;   // (block 1 is covered at the lower rates; keeps the test fast)
            for (const bool active : { false, true })
            {
                auto m = make (rates[r]);
                CHECK_EQ (m->getLatencySamples(), sizes[r]);
                Values v;
                if (active)
                {
                    v.both ("contrast", 60.0f);
                    v.both ("gain", 3.0f);
                }
                const size_t len = (size_t) (3 * sizes[r] + 1000);
                std::vector<float> x (len, 0.0f);
                x[700] = 1.0f;
                const auto y = run (*m, v, dual (x), block);
                size_t arg = 0;
                for (size_t i = 0; i < len; ++i)
                    if (std::abs (y.l[i]) > std::abs (y.l[arg]))
                        arg = i;
                CHECK_EQ ((int) arg - 700, m->getLatencySamples());
                if (! active)
                    CHECK (bitExactDelayed (y.l, x, m->getLatencySamples()) && bitExactDelayed (y.r, x, m->getLatencySamples()));
                else
                    CHECK (std::abs (y.l[arg] - 1.0f) < 0.01f);   // the low bins hold a tiny part of an impulse
            }
        }
        note ("latency at " + std::to_string ((int) rates[r]) + " Hz (samples)", sizes[r]);
    }
}

TEST_CASE ("lowc. neutral is the input delayed by N, bit-exact, in every mode (also after processing)")
{
    const auto noise = pstest::pinkNoise (0.3, 10.0, kSr, 77u);
    const auto noiseR = pstest::pinkNoise (0.3, 10.0, kSr, 78u);
    for (const auto chMode : { ChannelMode::Stereo, ChannelMode::MidSide })
        for (const int mode : { 0, 1 })
            for (const bool autoGain : { false, true })
            {
                auto m = make();
                Values v;
                v.both ("mode", (float) mode);
                v.set ("lowc.autoGain", autoGain ? 1.0f : 0.0f);
                v.set ("lowc.low", 35.0f);
                v.set ("lowc.high", 180.0f);
                const auto y = run (*m, v, { noise, noiseR }, 512, chMode);
                const int lat = m->getLatencySamples();
                CHECK (bitExactDelayed (y.l, noise, lat));
                CHECK (bitExactDelayed (y.r, noiseR, lat));
            }

    // processing, then the region moves and the contrast / gain return to 0: exact again once the glides end
    auto m = make();
    Values v;
    const auto y = run (*m, v, { noise, noiseR }, 333, ChannelMode::Stereo,
                        [] (size_t start, Values& vv, ModuleMonitor&)
                        {
                            const bool on = start < (size_t) (3.0 * kSr);
                            vv.both ("contrast", on ? 70.0f : 0.0f);
                            vv.both ("gain", on ? -4.0f : 0.0f);
                            vv.set ("lowc.low", on ? 25.0f : 60.0f);
                            vv.set ("lowc.high", on ? 140.0f : 290.0f);
                        });
    const int lat = m->getLatencySamples();
    const double during = maxDelayedDiff (y.l, noise, lat, (size_t) (1.0 * kSr));
    note ("max |out - delayed in| while processing", during);
    CHECK (during > 1.0e-3);
    size_t firstExact = y.l.size();
    for (size_t i = y.l.size(); i-- > (size_t) (3.0 * kSr);)
    {
        if (y.l[i] != noise[i - (size_t) lat] || y.r[i] != noiseR[i - (size_t) lat])
            break;
        firstExact = i;
    }
    note ("bit-exact again after the controls returned to 0 (s)", ((double) firstExact - 3.0 * kSr) / kSr);
    CHECK (firstExact < (size_t) (5.0 * kSr));
}

TEST_CASE ("lowc. contrast changes the low-band envelope modulation depth by documented amounts")
{
    const double punchyUp = contrastEffectDb (LowContrastMode::Punchy, 100.0f, -6.0);
    const double punchyDown = contrastEffectDb (LowContrastMode::Punchy, -100.0f, -6.0);
    const double smoothUp = contrastEffectDb (LowContrastMode::Smooth, 100.0f, -6.0);
    const double smoothDown = contrastEffectDb (LowContrastMode::Smooth, -100.0f, -6.0);
    const double punchyHalf = contrastEffectDb (LowContrastMode::Punchy, 50.0f, -6.0);
    note ("burst/gap ratio change, Punchy +100 (dB)", punchyUp);
    note ("burst/gap ratio change, Punchy +50 (dB)", punchyHalf);
    note ("burst/gap ratio change, Punchy -100 (dB)", punchyDown);
    note ("burst/gap ratio change, Smooth +100 (dB)", smoothUp);
    note ("burst/gap ratio change, Smooth -100 (dB)", smoothDown);
    CHECK (punchyUp >= 4.0);
    CHECK (punchyDown <= -2.0);
    CHECK (punchyDown > -punchyUp);   // the negative side acts about half as strongly
    // Smooth works on steady notes (see "Punchy lifts note onsets ..."); a decaying kick over a bass barely moves it
    CHECK (std::abs (smoothUp) < 1.0);
    CHECK (std::abs (smoothDown) < 1.0);
    CHECK (punchyHalf > 0.3 * punchyUp && punchyHalf < 0.75 * punchyUp);
}

TEST_CASE ("lowc. the contrast effect is independent of the input level and the sample rate")
{
    const double loud = contrastEffectDb (LowContrastMode::Punchy, 100.0f, -6.0);
    const double quiet = contrastEffectDb (LowContrastMode::Punchy, 100.0f, -26.0);
    const double loudNeg = contrastEffectDb (LowContrastMode::Punchy, -100.0f, -6.0);
    const double quietNeg = contrastEffectDb (LowContrastMode::Punchy, -100.0f, -26.0);
    note ("ratio change at -6 dBFS (dB)", loud);
    note ("ratio change at -26 dBFS (dB)", quiet);
    // within 0.5 dB: Punchy's fitted envelope floor (-41 dB of band power) makes quiet material a little gentler
    CHECK_NEAR (loud, quiet, 0.5);
    CHECK_NEAR (loudNeg, quietNeg, 0.5);
    // and of the sample rate (the FFT size scales with it: same bin width and time constants)
    for (const double sr : { 44100.0, 96000.0, 192000.0 })
    {
        const double e = contrastEffectDb (LowContrastMode::Punchy, 100.0f, -6.0, false, sr);
        note ("ratio change at " + std::to_string ((int) sr) + " Hz (dB)", e);
        CHECK_NEAR (e, loud, 0.6);
    }
}

TEST_CASE ("lowc. outside the region is untouched")
{
    // region 20-120 Hz, contrast +100, gain +6: 50 Hz bursts + steady 1 kHz at -12 dBFS
    auto m = make();
    Values v;
    v.set ("lowc.low", 20.0f);
    v.set ("lowc.high", 120.0f);
    v.both ("contrast", 100.0f);
    v.both ("gain", 6.0f);
    const auto kick = kickLoop (kSr, 6.0, -6.0, false);
    const auto tone = pstest::sine (1000.0, -12.0, 6.0, kSr);
    const auto x = pstest::added (kick, tone);
    const auto y = run (*m, v, dual (x), 512);
    const int lat = m->getLatencySamples();
    const auto r = residual (y.l, x, lat);
    const auto hp = highPass8 (r, 400.0, kSr);
    const double leak = pstest::dbOf (maxAbs (hp, (size_t) (2.0 * kSr), r.size()));
    note ("residual above 400 Hz, peak (dBFS)", leak);
    // the onset gain moves within the first ms of each hit: its modulation of the region band reaches above the
    // region, at the onset only (an onset shaper's own sidebands), at least 18 dB under the kick peak (-6 dBFS)
    CHECK (leak < -24.0);
    const size_t n = wholeCycles (1000.0, 3.0, kSr), s = (size_t) (2.5 * kSr);
    const double d1k = toneDb (y.l, s, n, 1000.0, kSr) - toneDb (x, s - (size_t) lat, n, 1000.0, kSr);
    note ("1 kHz level change (dB)", d1k);
    CHECK (std::abs (d1k) < 0.01);

    // beyond the 1.1-octave taper above the high cutoff: tones change < 0.1 dB (region 40-150 Hz, +100, +6 dB)
    for (const double f : { 360.0, 420.0 })
    {
        auto m2 = make();
        Values v2;
        v2.set ("lowc.low", 40.0f);
        v2.set ("lowc.high", 150.0f);
        v2.both ("contrast", 100.0f);
        v2.both ("gain", 6.0f);
        const auto x2 = pstest::added (kickLoop (kSr, 5.0, -6.0), pstest::sine (f, -12.0, 5.0, kSr));
        const auto y2 = run (*m2, v2, dual (x2), 512);
        const size_t n2 = wholeCycles (f, 2.5, kSr), s2 = (size_t) (2.0 * kSr);
        const double d = toneDb (y2.l, s2, n2, f, kSr) - toneDb (x2, s2 - (size_t) lat, n2, f, kSr);
        note ("tone " + std::to_string ((int) f) + " Hz level change (dB)", d);
        CHECK (std::abs (d) < 0.1);
    }
}

TEST_CASE ("lowc. gain inside the region and the 1.1-octave edges")
{
    // contrast 0, gain +6, region 40-160 Hz, steady sines
    struct Probe { double f, lo, hi; };
    // 20 Hz is one octave under the low edge, inside the taper (and between two 11.7 Hz bins)
    for (const Probe pr : { Probe { 100.0, 5.85, 6.15 }, Probe { 60.0, 5.85, 6.15 }, Probe { 20.0, 0.0, 2.0 },
                            Probe { 400.0, -0.05, 0.05 }, Probe { 1000.0, -0.001, 0.001 } })
    {
        auto m = make();
        Values v;
        v.set ("lowc.low", 40.0f);
        v.set ("lowc.high", 160.0f);
        v.both ("gain", 6.0f);
        const auto x = pstest::sine (pr.f, -12.0, 4.0, kSr);
        const auto y = run (*m, v, dual (x), 512);
        const int lat = m->getLatencySamples();
        const size_t n = wholeCycles (pr.f, 2.0, kSr), s = (size_t) (1.5 * kSr);
        const double d = toneDb (y.l, s, n, pr.f, kSr) - toneDb (x, s - (size_t) lat, n, pr.f, kSr);
        note ("gain +6, tone " + std::to_string ((int) pr.f) + " Hz change (dB)", d);
        CHECK (d >= pr.lo && d <= pr.hi);
        if (pr.f >= 1000.0)
        {
            const double res = pstest::dbOf (maxDelayedDiff (y.l, x, lat, (size_t) (1.0 * kSr)));
            note ("1 kHz residual peak (dBFS)", res);
            CHECK (res < -120.0);
        }
        // the difference meter shows what the audio gets
        if (pr.f == 100.0)
        {
            const auto& mt = m->getMeters();
            int i100 = 0;
            for (int i = 0; i < LowContrastMeters::kPoints; ++i)
                if (std::abs (std::log (lowContrastDisplayHz (i) / 100.0)) < std::abs (std::log (lowContrastDisplayHz (i100) / 100.0)))
                    i100 = i;
            note ("diffDb near 100 Hz", mt.diffDb[0][i100].load());
            note ("diffDb at 400 Hz", mt.diffDb[0][LowContrastMeters::kPoints - 1].load());
            CHECK_NEAR (mt.diffDb[0][i100].load(), 6.0, 0.05);
            CHECK_NEAR (mt.diffDb[0][LowContrastMeters::kPoints - 1].load(), 0.0, 0.01);
            CHECK_NEAR (mt.diffDb[1][i100].load(), 6.0, 0.05);   // Stereo: set B mirrors set A
            const double inDb = mt.regionInDb[0].load(), outDb = mt.regionOutDb[0].load();
            note ("region in (dB)", inDb);
            note ("region out (dB)", outDb);
            CHECK_NEAR (inDb, -15.05, 0.3);   // -12 dBFS peak sine = -15.05 dB RMS, W = 1
            CHECK_NEAR (outDb - inDb, 6.0, 0.05);
        }
    }
    CHECK_NEAR (lowContrastDisplayHz (0), 16.0, 1.0e-9);
    CHECK_NEAR (lowContrastDisplayHz (LowContrastMeters::kPoints - 1), 400.0, 1.0e-9);
}

TEST_CASE ("lowc. Punchy lifts note onsets, Smooth lowers them; the body is left alone")
{
    // 50 Hz notes of 200 ms every 500 ms at -6 dBFS (5 ms raised-cosine edges); gain in the onset (2 .. 17 ms) and in
    // the body (60 .. 180 ms) of the notes after the first second, output re input (the reference: Punchy +100 lifts
    // the first ~20 ms by ~+5.5 dB and leaves the body within ~0.4 dB)
    const double sr = kSr;
    const size_t n = (size_t) (6.0 * sr);
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr, tc = std::fmod (t, 0.5);
        double g = tc < 0.2 ? 1.0 : 0.0;
        if (tc < 0.005)
            g = 0.5 - 0.5 * std::cos (kPi * tc / 0.005);
        else if (tc >= 0.195 && tc < 0.2)
            g = 0.5 + 0.5 * std::cos (kPi * (tc - 0.195) / 0.005);
        x[i] = (float) (pstest::dbfsToAmplitude (-6.0) * g * std::sin (2.0 * kPi * 50.0 * t));
    }
    const auto partDb = [sr] (const std::vector<float>& y, size_t offset, const std::vector<float>& in, double from, double to)
    {
        double ey = 0.0, ex = 0.0;
        for (double t0 = 1.0; t0 < 5.5; t0 += 0.5)
            for (size_t i = (size_t) ((t0 + from) * sr); i < (size_t) ((t0 + to) * sr); ++i)
            {
                ey += (double) y[offset + i] * y[offset + i];
                ex += (double) in[i] * in[i];
            }
        return 10.0 * std::log10 (ey / ex);
    };
    struct Setting { LowContrastMode mode; float contrast; double onset, body; };
    Setting settings[] { { LowContrastMode::Punchy, 100.0f, 0, 0 }, { LowContrastMode::Punchy, 50.0f, 0, 0 },
                         { LowContrastMode::Punchy, -100.0f, 0, 0 }, { LowContrastMode::Smooth, 100.0f, 0, 0 } };
    for (auto& st : settings)
    {
        auto m = make();
        Values v;
        v.both ("mode", (float) st.mode);
        v.both ("contrast", st.contrast);
        const auto y = run (*m, v, dual (x), 512);
        const auto lat = (size_t) m->getLatencySamples();
        st.onset = partDb (y.l, lat, x, 0.002, 0.017);
        st.body = partDb (y.l, lat, x, 0.06, 0.18);
        note (std::string (st.mode == LowContrastMode::Punchy ? "Punchy " : "Smooth ") + std::to_string ((int) st.contrast)
                  + ": onset / body gain (dB)", st.onset);
        note ("    body", st.body);
    }
    CHECK (settings[0].onset >= 4.0);
    CHECK (std::abs (settings[0].body) < 1.5);
    CHECK (settings[1].onset > 0.3 * settings[0].onset && settings[1].onset < 0.75 * settings[0].onset);
    CHECK (settings[2].onset <= -1.0);
    // Smooth: much less onset change than Punchy, body untouched. (The reference's Smooth +100 lowers the onset of a
    // note after silence by ~0.5-1 dB; ours still holds the gap's lift ~5 ms longer: +2.5 dB here. Open, round 8.)
    CHECK (std::abs (settings[3].onset) < 0.5 * settings[0].onset);
    CHECK (std::abs (settings[3].body) < 1.0);
}

TEST_CASE ("lowc. steady tones are not modulated (no warble)")
{
    for (const int mode : { 0, 1 })
        for (const double f : { 45.0, 90.0, 200.0 })
        {
            auto m = make();
            Values v;
            v.both ("mode", (float) mode);
            v.both ("contrast", 100.0f);
            const auto x = pstest::sine (f, -10.0, 9.0, kSr);
            const auto y = run (*m, v, dual (x), 512);
            double lo = 1.0e9, hi = -1.0e9;
            const size_t win = (size_t) std::round (std::ceil (0.02 * f) * kSr / f);   // whole cycles, >= 20 ms
            for (size_t s = (size_t) (5.0 * kSr); s + win < y.l.size(); s += win)
            {
                const double d = pstest::dbOf (pstest::rmsOf (y.l, s, win));
                lo = std::min (lo, d);
                hi = std::max (hi, d);
            }
            const size_t nt = wholeCycles (f, 2.0, kSr), st = (size_t) (6.0 * kSr);
            const double gain = toneDb (y.l, st, nt, f, kSr) - toneDb (x, st - (size_t) m->getLatencySamples(), nt, f, kSr);
            const std::string tag = std::string (mode == 0 ? "Punchy" : "Smooth") + " +100, steady "
                                    + std::to_string ((int) f) + " Hz";
            note (tag + ": RMS swing over 4 s (dB)", hi - lo);
            note (tag + ": static level change (spectral contrast vs. its neighbourhood, dB)", gain);
            CHECK (hi - lo < 0.05);
        }
}

TEST_CASE ("lowc. pre-echo before a kick is bounded")
{
    // The change of the output that a kick causes BEFORE its onset (run with and without the kick, onset at 2 s),
    // RMS in 5 ms windows re the kick peak. The 85 ms analysis window (11 Hz bins) lets a gain change start up to
    // about N / 2 before the hit; it stays in the premasking range: documented levels below.
    struct Limit { double ms, silence, overBass; };
    const Limit limits[] { { 60.0, -75.0, -45.0 }, { 40.0, -60.0, -33.0 }, { 20.0, -35.0, -15.0 }, { 10.0, -20.0, -10.0 } };
    for (const bool withBass : { false, true })
        for (const float contrast : { 100.0f, -100.0f, 40.0f })
        {
            const double onset = 2.0;
            const auto with = kickLoop (kSr, 2.5, -6.0, withBass, onset);
            const auto without = withBass ? kickLoop (kSr, 2.5, -6.0, true, 10.0) : std::vector<float> (with.size(), 0.0f);
            Values v;
            v.both ("contrast", contrast);
            auto m1 = make();
            auto m2 = make();
            const auto y1 = run (*m1, v, dual (with), 512);
            const auto y2 = run (*m2, v, dual (without), 512);
            const size_t t0 = (size_t) (onset * kSr) + (size_t) m1->getLatencySamples();
            const double kickPeak = pstest::peakOf (y1.l, t0, (size_t) (0.05 * kSr));
            const auto win = (size_t) (0.005 * kSr);
            for (const auto& lim : limits)
            {
                const size_t s = t0 - (size_t) (lim.ms * 0.001 * kSr);
                double e = 0.0;
                for (size_t i = s; i < s + win; ++i)
                    e += ((double) y1.l[i] - y2.l[i]) * ((double) y1.l[i] - y2.l[i]);
                const double db = pstest::dbOf (std::sqrt (e / (double) win) / kickPeak);
                note (std::string (withBass ? "kick over bass" : "kick after silence") + ", contrast "
                          + std::to_string ((int) contrast) + ": change " + std::to_string ((int) lim.ms)
                          + " ms before the onset re kick peak (dB)", db);
                CHECK (db < (withBass ? lim.overBass : lim.silence));
            }
            // nothing audible earlier than 70 ms
            double early = 0.0;
            for (size_t i = t0 - (size_t) m1->getLatencySamples(); i < t0 - (size_t) (0.07 * kSr); ++i)
                early = std::max (early, std::abs ((double) y1.l[i] - y2.l[i]));
            note ("  earlier than 70 ms, peak re kick peak (dB)", pstest::dbOf (early / kickPeak));
            CHECK (pstest::dbOf (early / kickPeak) < -45.0);
        }
}

TEST_CASE ("lowc. Stereo applies one gain to both channels; M/S keeps set B independent")
{
    // a panned region signal: L/R ratio unchanged
    {
        auto m = make();
        Values v;
        v.both ("contrast", 100.0f);
        v.both ("gain", 2.0f);
        const auto x = kickLoop (kSr, 6.0, -6.0);
        Stereo in { x, pstest::scaled (x, 0.4f) };
        const auto y = run (*m, v, in, 512);
        const int lat = m->getLatencySamples();
        double worst = 0.0;
        const size_t win = (size_t) (0.02 * kSr);
        for (size_t s = (size_t) (2.0 * kSr); s + win < y.l.size(); s += win)
        {
            const double d = pstest::dbOf (pstest::rmsOf (y.l, s, win)) - pstest::dbOf (pstest::rmsOf (y.r, s, win));
            worst = std::max (worst, std::abs (d - pstest::dbOf (1.0 / 0.4)));
        }
        note ("Stereo: worst L/R ratio change (dB)", worst);
        CHECK (worst < 0.01);
        CHECK (maxDelayedDiff (y.l, x, lat, (size_t) kSr) > 1.0e-3);
    }
    // M/S: set A processes, set B neutral -> channel 1 bit-exact delayed
    {
        auto m = make();
        Values v;
        v.set ("lowc.a.contrast", 100.0f);
        v.set ("lowc.a.gain", 3.0f);
        const auto mid = kickLoop (kSr, 5.0, -6.0);
        const auto side = pstest::pinkNoise (0.1, 5.0, kSr, 5u);
        const auto y = run (*m, v, { mid, side }, 512, ChannelMode::MidSide);
        const int lat = m->getLatencySamples();
        CHECK (bitExactDelayed (y.r, side, lat));
        CHECK (maxDelayedDiff (y.l, mid, lat, (size_t) kSr) > 1.0e-2);
        // and the other way round: set B processes the side only
        auto m2 = make();
        Values v2;
        v2.set ("lowc.b.contrast", -100.0f);
        v2.set ("lowc.b.gain", -3.0f);
        const auto y2 = run (*m2, v2, { side, mid }, 512, ChannelMode::MidSide);
        CHECK (bitExactDelayed (y2.l, side, lat));
        CHECK (maxDelayedDiff (y2.r, mid, lat, (size_t) kSr) > 1.0e-2);
        const auto& mt = m2->getMeters();
        note ("M/S set B region out - in (dB)", mt.regionOutDb[1].load() - mt.regionInDb[1].load());
        CHECK (mt.regionOutDb[1].load() - mt.regionInDb[1].load() < -1.0);
        CHECK_NEAR (mt.regionOutDb[0].load() - mt.regionInDb[0].load(), 0.0, 1.0e-4);
    }
}

TEST_CASE ("lowc. Stereo detector follows activity in the other channel")
{
    // Proportional L/R signals alone cannot prove linking: independent normalized detectors give the same gains.
    // Keep the right input identical and steady, then excite only the left input with low-frequency kicks.
    const auto kicks = kickLoop (kSr, 6.0, -6.0, false);
    const auto steady = pstest::sine (60.0, -24.0, 6.0, kSr);
    Values v;
    v.both ("contrast", 100.0f);
    auto linked = make();
    auto reference = make();
    const auto y = run (*linked, v, { kicks, steady }, 512);
    const auto ref = run (*reference, v, { std::vector<float> (kicks.size(), 0.0f), steady }, 512);
    const size_t latency = (size_t) linked->getLatencySamples();
    const double linkedModulation = burstGapDb (y.r, latency, kSr, 3.0, 5.5);
    const double referenceModulation = burstGapDb (ref.r, latency, kSr, 3.0, 5.5);
    note ("steady right: burst/gap ratio change caused only by left kicks (dB)",
          linkedModulation - referenceModulation);
    CHECK (linkedModulation - referenceModulation > 1.0);
}

TEST_CASE ("lowc. T/S twin (paramSet 1) reads set B for both channels")
{
    const auto x = kickLoop (kSr, 4.0, -6.0);
    const Stereo in { x, pstest::scaled (x, 0.7f) };

    Values v;   // set A neutral, set B active
    v.set ("lowc.b.mode", 1.0f);
    v.set ("lowc.b.contrast", 80.0f);
    v.set ("lowc.b.gain", 2.0f);
    auto twin = make (kSr, 1);
    const auto yt = run (*twin, v, in, 512);

    Values ref;   // a normal instance with those values in set A
    ref.set ("lowc.a.mode", 1.0f);
    ref.set ("lowc.a.contrast", 80.0f);
    ref.set ("lowc.a.gain", 2.0f);
    auto normal = make();
    const auto yn = run (*normal, ref, in, 512);
    CHECK (yt.l == yn.l && yt.r == yn.r);
    CHECK (maxDelayedDiff (yt.l, x, twin->getLatencySamples(), (size_t) kSr) > 1.0e-3);

    // the normal instance with the same array stays neutral (set A), the twin with set B neutral too
    auto a = make();
    const auto ya = run (*a, v, in, 512);
    CHECK (bitExactDelayed (ya.l, in.l, a->getLatencySamples()) && bitExactDelayed (ya.r, in.r, a->getLatencySamples()));
    Values onlyA;
    onlyA.set ("lowc.a.contrast", 100.0f);
    auto twin2 = make (kSr, 1);
    const auto yt2 = run (*twin2, onlyA, in, 512);
    CHECK (bitExactDelayed (yt2.l, in.l, twin2->getLatencySamples()) && bitExactDelayed (yt2.r, in.r, twin2->getLatencySamples()));
    CHECK_EQ (twin->getLatencySamples(), normal->getLatencySamples());
}

TEST_CASE ("lowc. auto gain holds the region level")
{
    for (const float contrast : { -60.0f, 80.0f })
    {
        auto m = make();
        Values v;
        v.both ("contrast", contrast);
        v.set ("lowc.autoGain", 1.0f);
        const auto x = kickLoop (kSr, 10.0, -6.0);
        const auto y = run (*m, v, dual (x), 512);
        const int lat = m->getLatencySamples();
        const size_t s = (size_t) (7.0 * kSr), n = (size_t) (3.0 * kSr) - (size_t) lat;
        const double d = pstest::dbOf (pstest::rmsOf (y.l, s, n)) - pstest::dbOf (pstest::rmsOf (x, s - (size_t) lat, n));
        // without auto gain, for comparison
        auto m2 = make();
        v.set ("lowc.autoGain", 0.0f);
        const auto y2 = run (*m2, v, dual (x), 512);
        const double d2 = pstest::dbOf (pstest::rmsOf (y2.l, s, n)) - pstest::dbOf (pstest::rmsOf (x, s - (size_t) lat, n));
        note ("contrast " + std::to_string ((int) contrast) + ": RMS change with auto gain (dB)", d);
        note ("contrast " + std::to_string ((int) contrast) + ": RMS change without auto gain (dB)", d2);
        note ("auto gain meter (dB)", m->getMeters().autoGainDb[0].load());
        CHECK (std::abs (d) <= 0.7);
        CHECK (std::abs (d) < std::abs (d2));
        CHECK (std::abs (m->getMeters().autoGainDb[0].load()) > 0.1);
        CHECK_EQ (m2->getMeters().autoGainDb[0].load(), 0.0f);
    }
}

TEST_CASE ("lowc. auto gain releases to exact neutral after active processing")
{
    const Stereo in { kickLoop (kSr, 12.0, -6.0), kickLoop (kSr, 12.0, -12.0, true, 0.1) };
    for (const auto chMode : { ChannelMode::Stereo, ChannelMode::MidSide })
        for (const float contrast : { -60.0f, 80.0f })
        {
            auto m = make();
            Values v;
            v.set ("lowc.autoGain", 1.0f);
            double activeAuto[2] {};
            const auto y = run (*m, v, in, 480, chMode,
                                [&] (size_t start, Values& vv, ModuleMonitor&)
                                {
                                    const bool active = start < (size_t) (8.0 * kSr);
                                    vv.set ("lowc.a.contrast", active ? contrast : 0.0f);
                                    vv.set ("lowc.b.contrast", active ? -contrast : 0.0f);
                                    if (start == (size_t) (8.0 * kSr))
                                        for (int set = 0; set < 2; ++set)
                                            activeAuto[set] = m->getMeters().autoGainDb[set].load();
                                });
            const int latency = m->getLatencySamples();
            const size_t settled = (size_t) (10.0 * kSr);
            const std::string tag = std::string (chMode == ChannelMode::Stereo ? "Stereo" : "M/S")
                                    + ", contrast " + std::to_string ((int) contrast);
            const double peak = std::max (maxDelayedDiff (y.l, in.l, latency, settled),
                                          maxDelayedDiff (y.r, in.r, latency, settled));
            size_t firstExact = y.l.size();
            for (size_t i = y.l.size(); i-- > (size_t) (8.0 * kSr);)
            {
                if (y.l[i] != in.l[i - (size_t) latency] || y.r[i] != in.r[i - (size_t) latency])
                    break;
                firstExact = i;
            }
            note (tag + ": neutral residual peak after 2 s (dBFS)", pstest::dbOf (peak));
            note (tag + ": bit-exact after controls return to 0 (s)", ((double) firstExact - 8.0 * kSr) / kSr);
            CHECK (bitExactDelayed (y.l, in.l, latency, settled));
            CHECK (bitExactDelayed (y.r, in.r, latency, settled));
            CHECK (firstExact <= settled);
            for (int set = 0; set < 2; ++set)
            {
                note (tag + ": active auto gain set " + std::to_string (set) + " (dB)", activeAuto[set]);
                note (tag + ": final auto gain set " + std::to_string (set) + " (dB)",
                      m->getMeters().autoGainDb[set].load());
                CHECK (std::abs (activeAuto[set]) > 0.1);
                CHECK_EQ (m->getMeters().autoGainDb[set].load(), 0.0f);
            }
        }
}

TEST_CASE ("lowc. auto gain releases to exact neutral while its detector is frozen")
{
    auto m = make();
    Values v;
    v.set ("lowc.autoGain", 1.0f);
    auto x = kickLoop (kSr, 8.0, -6.0);
    x.resize ((size_t) (40.0 * kSr), 0.0f);
    x = pstest::concat (x, pstest::sine (100.0, -90.0, 4.0, kSr));
    double frozenAuto = 0.0, beforeNeutralAuto = 0.0;
    const auto y = run (*m, v, dual (x), 480, ChannelMode::Stereo,
                        [&] (size_t start, Values& vv, ModuleMonitor&)
                        {
                            vv.both ("contrast", start < (size_t) (38.0 * kSr) ? 80.0f : 0.0f);
                            if (start == (size_t) (37.0 * kSr))
                                frozenAuto = m->getMeters().autoGainDb[0].load();
                            if (start == (size_t) (38.0 * kSr))
                                beforeNeutralAuto = m->getMeters().autoGainDb[0].load();
                        });
    const int latency = m->getLatencySamples();
    const size_t settled = (size_t) (40.0 * kSr);
    const double peak = std::max (maxDelayedDiff (y.l, x, latency, settled),
                                  maxDelayedDiff (y.r, x, latency, settled));
    const size_t toneStart = (size_t) (41.0 * kSr), toneLength = (size_t) (2.0 * kSr);
    const double quietGain = toneDb (y.l, toneStart, toneLength, 100.0, kSr)
                              - toneDb (x, toneStart - (size_t) latency, toneLength, 100.0, kSr);
    note ("frozen auto gain before neutral (dB)", beforeNeutralAuto);
    note ("auto gain change in the last silent second before neutral (dB)", beforeNeutralAuto - frozenAuto);
    note ("quiet tone after neutral: residual peak (dBFS)", pstest::dbOf (peak));
    note ("quiet tone after neutral: level change (dB)", quietGain);
    note ("quiet tone after neutral: auto gain meter (dB)", m->getMeters().autoGainDb[0].load());
    CHECK (std::abs (beforeNeutralAuto) > 0.1);
    CHECK (std::abs (beforeNeutralAuto - frozenAuto) < 1.0e-4);
    CHECK (bitExactDelayed (y.l, x, latency, settled));
    CHECK (bitExactDelayed (y.r, x, latency, settled));
    CHECK_EQ (m->getMeters().autoGainDb[0].load(), 0.0f);
    CHECK (std::abs (quietGain) < 1.0e-6);
}

TEST_CASE ("lowc. auto gain neutral return preserves manual gain")
{
    const auto x = kickLoop (kSr, 12.0, -12.0);
    auto m = make();
    Values v;
    v.set ("lowc.autoGain", 1.0f);
    v.both ("gain", 6.0f);
    double activeAuto = 0.0;
    const auto y = run (*m, v, dual (x), 480, ChannelMode::Stereo,
                        [&] (size_t start, Values& vv, ModuleMonitor&)
                        {
                            vv.both ("contrast", start < (size_t) (8.0 * kSr) ? 80.0f : 0.0f);
                            if (start == (size_t) (8.0 * kSr))
                                activeAuto = m->getMeters().autoGainDb[0].load();
                        });
    auto reference = make();
    Values refValues;
    refValues.set ("lowc.autoGain", 1.0f);
    refValues.both ("gain", 6.0f);
    const auto ref = run (*reference, refValues, dual (x), 480);
    const size_t settled = (size_t) (10.0 * kSr);
    const double residualPeak = std::max (maxDelayedDiff (y.l, ref.l, 0, settled),
                                          maxDelayedDiff (y.r, ref.r, 0, settled));
    const size_t length = (size_t) kSr;
    const double gain = toneDb (y.l, settled, length, 60.0, kSr)
                        - toneDb (x, settled - (size_t) m->getLatencySamples(), length, 60.0, kSr);
    note ("manual +6 dB: active auto gain (dB)", activeAuto);
    note ("manual +6 dB: settled residual versus static +6 reference (dBFS)", pstest::dbOf (residualPeak));
    note ("manual +6 dB: settled 60 Hz (bass) gain (dB)", gain);
    CHECK (std::abs (activeAuto) > 0.1);
    CHECK (bitExactDelayed (y.l, ref.l, 0, settled));
    CHECK (bitExactDelayed (y.r, ref.r, 0, settled));
    CHECK_EQ (m->getMeters().autoGainDb[0].load(), 0.0f);
    CHECK_NEAR (gain, 6.0, 0.01);
}

TEST_CASE ("lowc. parameter automation is click-free (no HF splatter, no second-difference spikes)")
{
    const auto x = kickLoop (kSr, 6.0, -6.0);
    const double t0 = 2.0;
    struct Case { const char* name; BlockHook hook; };
    const Case cases[] {
        { "contrast -100 -> +100 in 100 ms",
          [t0] (size_t s, Values& v, ModuleMonitor&)
          {
              const double t = (double) s / kSr;
              v.both ("contrast", (float) std::clamp (-100.0 + 2000.0 * (t - t0), -100.0, 100.0));
          } },
        { "contrast +80 -> 0 immediately with auto gain",
          [t0] (size_t s, Values& v, ModuleMonitor&)
          {
              v.set ("lowc.autoGain", 1.0f);
              v.both ("contrast", (double) s / kSr < t0 ? 80.0f : 0.0f);
          } },
        { "region drag 20 -> 300 Hz in 200 ms",
          [t0] (size_t s, Values& v, ModuleMonitor&)
          {
              const double t = std::clamp (((double) s / kSr - t0) / 0.2, 0.0, 1.0);
              v.both ("contrast", 100.0f);
              v.set ("lowc.low", (float) (20.0 * std::pow (8.0, t)));
              v.set ("lowc.high", (float) (40.0 * std::pow (7.5, t)));
          } },
        { "mode toggles every 150 ms",
          [t0] (size_t s, Values& v, ModuleMonitor&)
          {
              const double t = (double) s / kSr;
              v.both ("contrast", 100.0f);
              v.both ("mode", t > t0 && ((int) ((t - t0) / 0.15) & 1) == 0 ? 1.0f : 0.0f);
          } },
        { "gain -12 -> +12 in 50 ms",
          [t0] (size_t s, Values& v, ModuleMonitor&)
          {
              const double t = (double) s / kSr;
              v.both ("gain", (float) std::clamp (-12.0 + 480.0 * (t - t0), -12.0, 12.0));
          } },
        { "region solo on / off",
          [t0] (size_t s, Values& v, ModuleMonitor& mon)
          {
              const double t = (double) s / kSr;
              v.both ("contrast", 50.0f);
              mon.soloRegion = t > t0 && t < t0 + 1.0;
          } },
    };
    // reference: the most extreme static setting
    auto mr = make();
    Values vr;
    vr.both ("contrast", 100.0f);
    vr.both ("gain", 12.0f);
    const auto yr = run (*mr, vr, dual (x), 64);
    const int lat = mr->getLatencySamples();
    const double refHf = maxAbs (highPass8 (residual (yr.l, x, lat), 2000.0, kSr), (size_t) lat + (size_t) kSr, x.size());
    double refD2 = 0.0;
    for (size_t i = (size_t) lat + (size_t) kSr; i < yr.l.size(); ++i)
        refD2 = std::max (refD2, std::abs ((double) yr.l[i] - 2.0 * yr.l[i - 1] + yr.l[i - 2]));
    note ("static +100 / +12 dB: residual above 2 kHz, peak (dBFS)", pstest::dbOf (refHf));

    for (const auto& c : cases)
    {
        auto m = make();
        Values v;
        const auto y = run (*m, v, dual (x), 64, ChannelMode::Stereo, c.hook);
        // the solo removes everything above the region (the input's own HF too), so it is measured on the output:
        // it must not hold more HF than the input
        const bool solo = std::string (c.name).find ("solo") != std::string::npos;
        std::vector<double> yd (y.l.begin(), y.l.end()), xd (x.begin(), x.end());
        const double hf = solo ? maxAbs (highPass8 (yd, 2000.0, kSr), (size_t) lat + (size_t) kSr, x.size())
                                     / std::max (1.0e-30, maxAbs (highPass8 (xd, 2000.0, kSr), (size_t) kSr, x.size()))
                               : maxAbs (highPass8 (residual (y.l, x, lat), 2000.0, kSr), (size_t) lat + (size_t) kSr, x.size());
        double d2 = 0.0;
        for (size_t i = (size_t) lat + (size_t) kSr; i < y.l.size(); ++i)
            d2 = std::max (d2, std::abs ((double) y.l[i] - 2.0 * y.l[i - 1] + y.l[i - 2]));
        bool finite = true;
        for (const float s : y.l)
            finite = finite && std::isfinite (s);
        note (std::string (c.name) + (solo ? ": output HF re input HF (dB)" : ": residual above 2 kHz, peak (dBFS)"),
              pstest::dbOf (hf));
        note (std::string (c.name) + ": second-difference peak / static", d2 / refD2);
        CHECK (finite);
        // the onset shaping itself puts sidebands above the region (static +100 / +12: ~-66 dBFS); automation must
        // not add more than 6 dB to that
        CHECK (pstest::dbOf (hf) < (solo ? 0.5 : pstest::dbOf (refHf) + 6.0));
        CHECK (d2 <= 4.0 * refD2);
    }
}

TEST_CASE ("lowc. region solo plays the unprocessed region")
{
    for (const auto pr : { std::pair<double, double> { 100.0, 0.1 }, std::pair<double, double> { 1000.0, -80.0 } })
    {
        auto m = make();
        Values v;
        v.set ("lowc.low", 40.0f);
        v.set ("lowc.high", 160.0f);
        v.both ("contrast", 100.0f);   // the solo ignores the processing
        v.both ("gain", 6.0f);
        const auto x = pstest::sine (pr.first, -6.0, 4.0, kSr);
        const auto y = run (*m, v, dual (x), 512, ChannelMode::Stereo,
                            [] (size_t, Values&, ModuleMonitor& mon) { mon.soloRegion = true; });
        const int lat = m->getLatencySamples();
        const size_t n = wholeCycles (pr.first, 2.0, kSr), s = (size_t) (1.5 * kSr);
        const double d = toneDb (y.l, s, n, pr.first, kSr) - toneDb (x, s - (size_t) lat, n, pr.first, kSr);
        note ("solo: tone " + std::to_string ((int) pr.first) + " Hz change (dB)", d);
        if (pr.first < 500.0)
            CHECK (std::abs (d) < pr.second);
        else
            CHECK (d < pr.second);
    }
}

TEST_CASE ("lowc. silence, denormals and extremes stay finite and quiet")
{
    auto m = make();
    Values v;
    v.both ("contrast", 100.0f);
    v.both ("gain", 12.0f);
    v.set ("lowc.autoGain", 1.0f);
    // loud burst, then 20 s of silence, then tiny values, then full-scale noise
    std::vector<float> x = kickLoop (kSr, 1.0, 0.0);
    x.resize ((size_t) (21.0 * kSr), 0.0f);
    for (size_t i = (size_t) (21.0 * kSr) - 2000; i < x.size(); ++i)
        x[i] = 1.0e-38f;
    x = pstest::concat (x, pstest::whiteNoise (1.0, 1.0, kSr, 9u));
    const auto y = run (*m, v, dual (x), 512);
    bool finite = true;
    double peak = 0.0;
    for (const float s : y.l)
    {
        finite = finite && std::isfinite (s);
        peak = std::max (peak, (double) std::abs (s));
    }
    note ("peak with contrast +100, gain +12 on 0 dBFS material", peak);
    CHECK (finite);
    CHECK (peak < 32.0);   // at most +24 dB (contrast +12 and gain +12) on a 0 dBFS kick
    const double silent = pstest::peakOf (y.l, (size_t) (12.0 * kSr), (size_t) (8.0 * kSr));
    CHECK (silent < 1.0e-30);
}

TEST_CASE ("lowc. no allocation in setParams, setMonitor, process and reset")
{
    auto m = make();
    Values v;
    v.both ("contrast", 70.0f);
    v.set ("lowc.autoGain", 1.0f);
    const auto x = kickLoop (kSr, 1.0, -6.0);
    Stereo buf = dual (x);
    ProcessContext ctx;
    ModuleMonitor mon;
    pstest::AllocationScope scope;
    for (int pass = 0; pass < 4; ++pass)
    {
        ctx.channelMode = pass == 1 ? ChannelMode::MidSide : ChannelMode::Stereo;
        mon.soloRegion = pass == 2;
        v.both ("mode", (float) (pass & 1));
        for (size_t s = 0; s + 480 <= buf.l.size(); s += 480)
        {
            m->setParams (v.data());
            m->setMonitor (mon);
            float* ch[2] { buf.l.data() + s, buf.r.data() + s };
            m->process (ch, 480, ctx);
        }
        m->reset();
    }
    CHECK_EQ (scope.count(), 0LL);
}

TEST_CASE ("lowc. cpu")
{
    struct Bench { const char* name; double sr; float contrast; bool solo; ChannelMode mode; bool autoGain; };
    for (const Bench b : { Bench { "48 kHz neutral", 48000.0, 0.0f, false, ChannelMode::Stereo, false },
                           Bench { "48 kHz typical (Stereo, +40)", 48000.0, 40.0f, false, ChannelMode::Stereo, false },
                           Bench { "48 kHz worst (M/S, +100, auto gain)", 48000.0, 100.0f, false, ChannelMode::MidSide, true },
                           Bench { "48 kHz region solo", 48000.0, 100.0f, true, ChannelMode::Stereo, false },
                           Bench { "192 kHz worst (M/S, +100, auto gain)", 192000.0, 100.0f, false, ChannelMode::MidSide, true } })
    {
        auto m = make (b.sr);
        Values v;
        v.both ("contrast", b.contrast);
        v.set ("lowc.autoGain", b.autoGain ? 1.0f : 0.0f);
        const double seconds = 10.0;
        const auto x = kickLoop (b.sr, seconds, -6.0);
        Stereo buf = dual (x);
        ModuleMonitor mon;
        mon.soloRegion = b.solo;
        m->setMonitor (mon);
        ProcessContext ctx;
        ctx.channelMode = b.mode;
        pstest::Stopwatch sw;
        for (size_t s = 0; s + 512 <= buf.l.size(); s += 512)
        {
            m->setParams (v.data());
            float* ch[2] { buf.l.data() + s, buf.r.data() + s };
            m->process (ch, 512, ctx);
        }
        const double xr = seconds / std::max (1.0e-9, sw.seconds());
        note (std::string (b.name) + " (x realtime)", xr);
        // 4 linear-phase bands (one forward and four inverse FFTs per hop) and per-band detectors: ~55x at 48 kHz
        CHECK (xr > (b.sr > 100000.0 ? 12.0 : 45.0));
    }
}
