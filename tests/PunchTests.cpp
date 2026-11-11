#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/Crossover.h"
#include "dsp/Punch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Punch (dsp/Punch.cpp). Prefix "pnch.".
namespace
{
using namespace ps;
using pstest::cd;
using pstest::kPi;

constexpr double kSr = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

struct Rig
{
    explicit Rig (double sampleRate = kSr, int blockSize = 512) : sr (sampleRate), block (blockSize)
    {
        for (const auto& s : paramSpecs())
            values.push_back (s.defaultValue);
        punch = std::make_unique<Punch>();
    }

    void set (const std::string& id, float v)
    {
        const int i = paramIndex ("pnch." + id);
        REQUIRE (i >= 0);
        values[(size_t) i] = v;
    }

    void setBand (int set, int band, const std::string& name, float v)
    {
        this->set (std::string (set == 0 ? "a" : "b") + ".b" + std::to_string (band) + "." + name, v);
    }

    void setAmount (int band, float v)   // both sets
    {
        setBand (0, band, "amount", v);
        setBand (1, band, "amount", v);
    }

    void prepare()
    {
        punch->prepare (sr, block);
        punch->setParams (values.data());
    }

    Stereo run (const std::vector<float>& l, const std::vector<float>& r,
                const std::function<void (size_t)>& onBlock = {})
    {
        Stereo out { l, r };
        for (size_t start = 0; start < l.size(); start += (size_t) block)
        {
            if (onBlock)
                onBlock (start);
            punch->setParams (values.data());
            const int n = (int) std::min ((size_t) block, l.size() - start);
            float* ch[2] { out.l.data() + start, out.r.data() + start };
            punch->process (ch, n, ctx);
        }
        return out;
    }

    Stereo run (const std::vector<float>& mono) { return run (mono, mono); }

    double sr;
    int block;
    std::vector<float> values;
    std::unique_ptr<Punch> punch;
    ProcessContext ctx;
};

size_t at (double seconds, double sr = kSr) { return (size_t) std::llround (seconds * sr); }

// Quadrature test signal: L = e(t) sin, R = e(t) cos. With Stereo Link on, both channels get the same gain, so
// |out| / |in| is the exact per-sample gain.
Stereo quadTone (const std::vector<double>& env, double hz, double sr = kSr)
{
    Stereo s { std::vector<float> (env.size()), std::vector<float> (env.size()) };
    for (size_t i = 0; i < env.size(); ++i)
    {
        const double ph = 2.0 * kPi * hz * (double) i / sr;
        s.l[i] = (float) (env[i] * std::sin (ph));
        s.r[i] = (float) (env[i] * std::cos (ph));
    }
    return s;
}

std::vector<double> gainTraceDb (const Stereo& in, const Stereo& out)
{
    std::vector<double> g (in.l.size(), 0.0);
    for (size_t i = 0; i < g.size(); ++i)
    {
        const double a = std::hypot ((double) in.l[i], (double) in.r[i]);
        const double b = std::hypot ((double) out.l[i], (double) out.r[i]);
        g[i] = a > 1.0e-12 ? 20.0 * std::log10 (std::max (b, 1.0e-30) / a) : 0.0;
    }
    return g;
}

// burst train envelope: attack 1 ms, hold, exponential decay, repeated
std::vector<double> burstEnvelope (double seconds, double periodS, double peak, double decayMs, double floorAmp,
                                   double sr = kSr)
{
    std::vector<double> e (at (seconds, sr));
    const auto period = at (periodS, sr);
    for (size_t i = 0; i < e.size(); ++i)
    {
        const double t = (double) (i % period) / sr;
        const double att = std::min (1.0, t / 0.001);
        e[i] = floorAmp + peak * att * std::exp (-t * 1000.0 / decayMs);
    }
    return e;
}

double crestDb (const std::vector<float>& x, size_t from, size_t to)
{
    double pk = 0.0, ss = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        pk = std::max (pk, std::abs ((double) x[i]));
        ss += (double) x[i] * x[i];
    }
    const double rms = std::sqrt (ss / (double) std::max<size_t> (1, to - from));
    return 20.0 * std::log10 (pk / std::max (rms, 1e-30));
}

// mean crest factor over 1 s windows
double meanCrestDb (const std::vector<float>& x, size_t from, double sr = kSr)
{
    double acc = 0.0;
    int n = 0;
    for (size_t s = from; s + at (1.0, sr) <= x.size(); s += at (1.0, sr), ++n)
        acc += crestDb (x, s, s + at (1.0, sr));
    return n > 0 ? acc / n : 0.0;
}

// Independent micro-dynamics measure: std dev of 10 log10 (short / long) with attack 1 ms, release 10 ms, long
// 100 ms on x^2 (plain one-poles, not the module's detector).
double microStdDb (const std::vector<float>& x, size_t from, double sr = kSr)
{
    const double ca = 1.0 - std::exp (-1.0 / (0.001 * sr)), cr = 1.0 - std::exp (-1.0 / (0.010 * sr));
    const double cl = 1.0 - std::exp (-1.0 / (0.100 * sr));
    double as = 0.0, al = 0.0, m = 0.0, m2 = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double p = (double) x[i] * x[i];
        as += (p > as ? ca : cr) * (p - as);
        al += cl * (p - al);
        if (i >= from)
        {
            const double rel = 10.0 * std::log10 ((as + 1e-12) / (al + 1e-12));
            m += rel;
            m2 += rel * rel;
            ++n;
        }
    }
    m /= (double) n;
    return std::sqrt (std::max (0.0, m2 / (double) n - m * m));
}

// Kick + bass loop at 120 BPM: pitch-swept kick (150 -> 48 Hz, 180 ms decay) every beat, sustained bass notes
// (55 / 41 Hz, a sine plus a soft second harmonic) under it, a hat-like noise tick on the off-beats.
std::vector<float> kickBassLoop (double seconds, double sr = kSr)
{
    std::vector<float> x (at (seconds, sr));
    const auto beat = at (0.5, sr);
    pstest::Lcg lcg (99);
    double kickPhase = 0.0, bassPhase = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double tb = (double) (i % beat) / sr;
        if (i % beat == 0)
            kickPhase = 0.0;
        const double kf = 48.0 + 102.0 * std::exp (-tb / 0.03);
        kickPhase += 2.0 * kPi * kf / sr;
        const double kick = 0.55 * std::exp (-tb / 0.18) * std::sin (kickPhase) * std::min (1.0, tb / 0.0005);
        const double bassHz = ((i / (4 * beat)) % 2 == 0) ? 55.0 : 41.2;
        bassPhase += 2.0 * kPi * bassHz / sr;
        const double bass = 0.18 * (std::sin (bassPhase) + 0.25 * std::sin (2.0 * bassPhase));
        const double toff = std::fmod (tb + 0.25, 0.5);
        const double hat = 0.05 * lcg.next() * std::exp (-toff / 0.015);
        x[i] = (float) (kick + bass + hat);
    }
    return x;
}

// "Drum" signal of the spec: decaying 150 Hz + 3 kHz bursts every 250 ms over pink noise 30 dB below.
std::vector<float> drumSignal (double seconds, double sr = kSr)
{
    auto x = pstest::pinkNoise (pstest::dbfsToAmplitude (-36.0), seconds, sr, 4242);
    const auto period = at (0.25, sr);
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) (i % period) / sr;
        const double e = std::min (1.0, t / 0.0005);
        x[i] += (float) (e * (0.35 * std::exp (-t / 0.06) * std::sin (2.0 * kPi * 150.0 * t)
                              + 0.15 * std::exp (-t / 0.02) * std::sin (2.0 * kPi * 3000.0 * t)));
    }
    return x;
}

// Max |a[i] - ref[i - delay]| in dBFS over [from, a.size()).
double nullDb (const std::vector<float>& a, const std::vector<float>& ref, int delay, size_t from)
{
    double m = 0.0;
    for (size_t i = from; i < a.size(); ++i)
    {
        const double r = i >= (size_t) delay ? (double) ref[i - (size_t) delay] : 0.0;
        m = std::max (m, std::abs ((double) a[i] - r));
    }
    return pstest::dbOf (m);
}

// THD+N of a sine: everything but the fundamental (fitted by correlation), relative to the fundamental.
double thdNDb (const std::vector<float>& y, size_t start, size_t n, double hz, double sr = kSr)
{
    const cd t = pstest::toneOf (y, start, n, hz, sr);
    double res = 0.0, sig = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double ph = 2.0 * kPi * hz * (double) (start + i) / sr;
        const double fit = t.real() * std::cos (ph) - t.imag() * std::sin (ph);
        const double d = (double) y[start + i] - fit;
        res += d * d;
        sig += fit * fit;
    }
    return 10.0 * std::log10 (std::max (res, 1e-30) / std::max (sig, 1e-30));
}

// Harmonic distortion: energy at 2f .. 6f relative to f, from correlation (whole-period window).
double harmonicsDb (const std::vector<float>& y, size_t start, size_t n, double hz, double sr = kSr)
{
    const double f = std::abs (pstest::toneOf (y, start, n, hz, sr));
    double h = 0.0;
    for (int k = 2; k <= 6; ++k)
    {
        const double a = std::abs (pstest::toneOf (y, start, n, hz * k, sr));
        h += a * a;
    }
    return 10.0 * std::log10 (std::max (h, 1e-30) / std::max (f * f, 1e-30));
}

// HF splatter: max over 5 ms windows of the RMS of the 4th difference (1 kHz is attenuated by ~70 dB, a click is not)
double maxSplatter (const std::vector<float>& y, size_t from, double sr = kSr)
{
    const auto w = at (0.005, sr);
    double worst = 0.0, acc = 0.0;
    size_t cnt = 0;
    for (size_t i = std::max<size_t> (from, 4); i < y.size(); ++i)
    {
        const double d = (double) y[i] - 4.0 * y[i - 1] + 6.0 * y[i - 2] - 4.0 * y[i - 3] + (double) y[i - 4];
        acc += d * d;
        if (++cnt == w)
        {
            worst = std::max (worst, std::sqrt (acc / (double) w));
            acc = 0.0;
            cnt = 0;
        }
    }
    return worst;
}
} // namespace

//==============================================================================
TEST_CASE ("pnch. neutral default is a bit-exact passthrough (Stereo, M/S, odd blocks)")
{
    const auto l = pstest::pinkNoise (0.5, 5.0, kSr, 11);
    const auto r = pstest::pinkNoise (0.5, 5.0, kSr, 12);
    for (auto mode : { ChannelMode::Stereo, ChannelMode::MidSide })
        for (int block : { 1, 64, 512, 333 })
        {
            Rig rig (kSr, block);
            rig.ctx.channelMode = mode;
            rig.prepare();
            const auto out = rig.run (l, r);
            CHECK (out.l == l);
            CHECK (out.r == r);
            CHECK_EQ (rig.punch->getLatencySamples(), 0);
        }
    // every amount 0 but the other controls moved: still exact (1 band)
    Rig rig;
    rig.set ("env", 13.0f);
    rig.set ("stLink", 1.0f);
    rig.set ("amount", 37.0f);
    rig.set ("sync", 1.0f);
    rig.prepare();
    const auto out = rig.run (l, r);
    CHECK (out.l == l);
    CHECK (out.r == r);
}

TEST_CASE ("pnch. output does not depend on the host block size")
{
    const auto l = drumSignal (3.0);
    const auto r = pstest::added (pstest::scaled (drumSignal (3.0), 0.5f), pstest::pinkNoise (0.05, 3.0, kSr, 3));
    std::vector<Stereo> outs;
    for (int block : { 512, 64, 333, 1 })
    {
        Rig rig (kSr, block);
        rig.set ("bands", 3.0f);
        rig.set ("autoGain", 1.0f);
        rig.set ("stLink", 1.0f);
        rig.setAmount (1, 60.0f);
        rig.setAmount (2, -40.0f);
        rig.setAmount (3, 80.0f);
        rig.prepare();
        outs.push_back (rig.run (l, r));
    }
    for (size_t k = 1; k < outs.size(); ++k)
    {
        CHECK (outs[k].l == outs[0].l);
        CHECK (outs[k].r == outs[0].r);
    }
    // a host block larger than the prepared maximum is processed in chunks, with the same result
    Rig big (kSr, 512);
    big.set ("bands", 3.0f);
    big.set ("autoGain", 1.0f);
    big.set ("stLink", 1.0f);
    big.setAmount (1, 60.0f);
    big.setAmount (2, -40.0f);
    big.setAmount (3, 80.0f);
    big.prepare();
    big.block = 2048;
    const auto ob = big.run (l, r);
    CHECK (ob.l == outs[0].l);
}

TEST_CASE ("pnch. N bands with amounts 0 equal the crossover (Analog / Hybrid sum, Digital delay)")
{
    const auto l = pstest::pinkNoise (0.5, 3.0, kSr, 21);
    const auto r = pstest::pinkNoise (0.5, 3.0, kSr, 22);
    for (int type = 0; type < 3; ++type)
    {
        Rig rig;
        rig.set ("bands", 4.0f);
        rig.set ("xoverType", (float) type);
        rig.prepare();
        const auto out = rig.run (l, r);

        Crossover x;
        x.prepare (kSr, 512);
        const float hz[3] { 120.0f, 1000.0f, 6000.0f };
        std::vector<float> buf (4 * 2 * 512);
        float* ptr[4][2];
        float* const* bands[4];
        for (int b = 0; b < 4; ++b)
        {
            ptr[b][0] = buf.data() + (size_t) (b * 2) * 512;
            ptr[b][1] = buf.data() + (size_t) (b * 2 + 1) * 512;
            bands[b] = ptr[b];
        }
        Stereo ref { l, r };
        for (size_t s = 0; s < l.size(); s += 512)
        {
            x.setup (4, hz, (CrossoverType) type);
            const int n = (int) std::min<size_t> (512, l.size() - s);
            const float* in[2] { l.data() + s, r.data() + s };
            x.process (in, bands, n);
            for (int i = 0; i < n; ++i)
            {
                float a = 0.0f, c = 0.0f;
                for (int b = 0; b < 4; ++b)
                {
                    a += ptr[b][0][i];
                    c += ptr[b][1][i];
                }
                ref.l[s + (size_t) i] = a;
                ref.r[s + (size_t) i] = c;
            }
        }
        const double d = std::max (nullDb (out.l, ref.l, 0, 0), nullDb (out.r, ref.r, 0, 0));
        pstest::note ("type " + std::to_string (type) + ": |punch - crossover sum| max (dBFS)", d);
        CHECK (d < -140.0);
        if (type == (int) CrossoverType::Digital)
        {
            const int lat = rig.punch->getLatencySamples();
            CHECK_EQ (lat, x.getLatencySamples (CrossoverType::Digital));
            const double dd = std::max (nullDb (out.l, l, lat, at (0.5)), nullDb (out.r, r, lat, at (0.5)));
            pstest::note ("Digital: |out - input delayed by latency| (dBFS)", dd);
            CHECK (dd < -90.0);
        }
    }
}

TEST_CASE ("pnch. latency equals the measured impulse delay per crossover type")
{
    for (int type = 0; type < 3; ++type)
    {
        Rig rig;
        rig.set ("bands", 3.0f);
        rig.set ("xoverType", (float) type);
        rig.setAmount (1, 40.0f);
        rig.setAmount (2, -40.0f);
        rig.setAmount (3, 60.0f);
        rig.prepare();
        std::vector<float> imp (at (2.0), 0.0f);
        imp[at (1.0)] = 0.5f;
        const auto out = rig.run (imp, imp);
        size_t peak = 0;
        for (size_t i = 0; i < out.l.size(); ++i)
            if (std::abs (out.l[i]) > std::abs (out.l[peak]))
                peak = i;
        const int lat = rig.punch->getLatencySamples();
        pstest::note ("type " + std::to_string (type) + " latency", lat);
        if (type == (int) CrossoverType::Digital)
        {
            CHECK (lat > 0);
            CHECK_EQ ((int) (peak - at (1.0)), lat);
        }
        else
        {
            CHECK_EQ (lat, 0);
            CHECK (peak >= at (1.0) && peak < at (1.0) + 40);   // all-pass sum: energy right at the impulse
        }
    }
    // latency is reported from the moment of a type switch
    Rig rig;
    rig.prepare();
    CHECK_EQ (rig.punch->getLatencySamples(), 0);
    rig.set ("xoverType", (float) CrossoverType::Digital);
    rig.punch->setParams (rig.values.data());
    CHECK (rig.punch->getLatencySamples() > 0);
}

TEST_CASE ("pnch. step response follows the documented ballistics")
{
    auto stepRun = [] (float amount, float env, std::vector<double>& g)
    {
        Rig rig;
        rig.set ("stLink", 1.0f);
        rig.set ("env", env);
        rig.setAmount (1, amount);
        rig.prepare();
        std::vector<double> e (at (3.5));
        for (size_t i = 0; i < e.size(); ++i)
            e[i] = pstest::dbfsToAmplitude (i < at (2.0) ? -30.0 : -10.0);
        const auto in = quadTone (e, 1000.0);
        const auto out = rig.run (in.l, in.r);
        g = gainTraceDb (in, out);
    };
    std::vector<double> gp, gn, g25;
    stepRun (50.0f, 100.0f, gp);
    stepRun (-50.0f, 100.0f, gn);
    stepRun (50.0f, 25.0f, g25);
    const size_t s = at (2.0);
    double peak = -100.0;
    size_t peakAt = 0;
    for (size_t i = s; i < s + at (0.05); ++i)
        if (gp[i] > peak)
        {
            peak = gp[i];
            peakAt = i - s;
        }
    const double ms = 1000.0 / kSr;
    pstest::note ("before step (dB)", gp[s - 10]);
    pstest::note ("peak gain (dB)", peak);
    pstest::note ("peak at (ms)", (double) peakAt * ms);
    pstest::note ("gain at 1 / 2 / 5 ms", gp[s + at (0.001)]);
    pstest::note ("  2 ms", gp[s + at (0.002)]);
    pstest::note ("  5 ms", gp[s + at (0.005)]);
    pstest::note ("gain at 100 ms", gp[s + at (0.1)]);
    pstest::note ("gain at 230 ms", gp[s + at (0.23)]);
    pstest::note ("gain at 300 ms", gp[s + at (0.3)]);
    // the reference (one-band renders, a 1 kHz step of +12 dB at +50): about +4 dB right after a step to -12 dBFS,
    // still +2.4 dB after 1 s (its long envelope rises over seconds); this step from -30 starts near the level floor,
    // so it gets a little less (+2.7 dB)
    CHECK (std::abs (gp[s - 10]) < 0.05);
    CHECK (peak >= 2.2 && peak <= 3.4);
    CHECK (peakAt * ms <= 20.0);
    CHECK (gp[s + at (0.3)] >= 2.0 && gp[s + at (0.3)] < gp[s + at (0.1)]);

    // negative amounts: the same trace scaled by -0.578 (no clamp at 50 %)
    double mirror = 0.0;
    for (size_t i = s - at (0.1); i < s + at (1.0); ++i)
        mirror = std::max (mirror, std::abs (gn[i] + 0.578 * gp[i]));
    pstest::note ("|gain(-50) + 0.578 gain(+50)| max (dB)", mirror);
    CHECK (mirror < 0.05);

    // every time constant scales with E: E = 25 ms at 75 ms = E = 100 ms at 300 ms
    pstest::note ("E = 25 ms: gain at 75 ms (dB)", g25[s + at (0.075)]);
    CHECK (std::abs (g25[s + at (0.075)] - gp[s + at (0.3)]) < 0.1);
}

TEST_CASE ("pnch. a train of hits: positive amounts lift it, negative amounts lower it")
{
    // decaying 1 kHz hits (5 ms attack portion vs the 60..250 ms tail), measured with the gain trace
    auto ratio = [] (float amount, double& attackDb, double& tailDb)
    {
        Rig rig;
        rig.set ("stLink", 1.0f);
        rig.set ("env", 100.0f);
        rig.setAmount (1, amount);
        rig.prepare();
        const auto e = burstEnvelope (6.0, 0.5, 0.5, 80.0, 0.002);
        const auto in = quadTone (e, 1000.0);
        const auto out = rig.run (in.l, in.r);
        const auto g = gainTraceDb (in, out);
        double a = 0.0, t = 0.0;
        int na = 0, nt = 0;
        for (size_t p = at (2.0); p + at (0.5) <= g.size(); p += at (0.5))
        {
            for (size_t i = p; i < p + at (0.005); ++i, ++na)
                a += g[i];
            for (size_t i = p + at (0.06); i < p + at (0.25); ++i, ++nt)
                t += g[i];
        }
        attackDb = a / na;
        tailDb = t / nt;
    };
    double ap, tp, an, tn;
    ratio (60.0f, ap, tp);
    ratio (-60.0f, an, tn);
    pstest::note ("+60: mean gain in the first 5 ms of a hit (dB)", ap);
    pstest::note ("+60: mean gain in the tail 60..250 ms (dB)", tp);
    pstest::note ("-60: hit (dB)", an);
    pstest::note ("-60: tail (dB)", tn);
    // the long envelope falls fast and rises slowly, so it sits near the troughs of a hit train: the train is lifted
    // (hit and tail alike) at positive amounts, lowered at negative ones (like the reference on its kick test and
    // on music, where its gain follows the band level rather than single onsets)
    CHECK (ap > 2.0);
    CHECK (tp > 2.0);
    CHECK (an < -1.0);
    CHECK (tn < -1.0);
    CHECK (std::abs (an / ap + 0.578) < 0.05);
}

TEST_CASE ("pnch. level floor: quiet bands get little gain")
{
    std::vector<double> traces[2];
    int idx = 0;
    std::vector<double> envRef;
    for (double peakDb : { -6.0, -46.0 })
    {
        Rig rig;
        rig.set ("stLink", 1.0f);
        rig.setAmount (1, 60.0f);
        rig.prepare();
        const double pk = pstest::dbfsToAmplitude (peakDb);
        const auto e = burstEnvelope (4.0, 0.4, pk, 30.0, pk * pstest::dbfsToAmplitude (-24.0));
        if (idx == 0)
            envRef = e;
        const auto in = quadTone (e, 1000.0);
        const auto out = rig.run (in.l, in.r);
        traces[idx++] = gainTraceDb (in, out);
    }
    double loud = 0.0, quiet = 0.0;
    for (size_t i = at (1.0); i < traces[0].size(); ++i)
        if (envRef[i] > pstest::dbfsToAmplitude (-70.0))
        {
            loud = std::max (loud, std::abs (traces[0][i]));
            quiet = std::max (quiet, std::abs (traces[1][i]));
        }
    pstest::note ("max |gain| at -6 dBFS (dB)", loud);
    pstest::note ("max |gain| at -46 dBFS (dB)", quiet);
    CHECK (loud > 2.0);
    CHECK (quiet < 0.1 * loud);
}

TEST_CASE ("pnch. crest factor and micro-dynamics of a kick + bass loop and a drum signal")
{
    const auto loop = kickBassLoop (8.0);
    const double crestIn = meanCrestDb (loop, at (2.0));
    const double microIn = microStdDb (loop, at (2.0));
    pstest::note ("kick+bass loop: input crest (dB)", crestIn);
    double crest[5] {}, micro[5] {};
    const float amounts[5] { -100.0f, -50.0f, 0.0f, 50.0f, 100.0f };
    for (int k = 0; k < 5; ++k)
    {
        Rig rig;
        rig.setAmount (1, amounts[k]);
        rig.prepare();
        const auto out = rig.run (loop);
        crest[k] = meanCrestDb (out.l, at (2.0)) - crestIn;
        pstest::note ("loop: crest change at amount " + std::to_string ((int) amounts[k]) + " (dB)", crest[k]);
        micro[k] = microStdDb (out.l, at (2.0)) / microIn;
        pstest::note ("   micro std out / in", micro[k]);
    }
    CHECK (micro[0] < micro[1] && micro[1] < micro[2] && micro[2] < micro[3] && micro[3] < micro[4]);
    CHECK (std::abs (crest[2]) < 1e-9);
    // measured with the fitted law: micro std 0.93 / 0.96 / 1.07 / 1.16 at -100 / -50 / +50 / +100; the crest moves by
    // +1.5 / +0.7 / +0.4 / +1.1 dB (the whole loop is lifted or lowered over seconds, so the crest changes little;
    // the reference's crest change on music is small too)
    CHECK (micro[4] >= 1.08 && micro[4] <= 1.25);
    CHECK (micro[0] >= 0.88 && micro[0] <= 0.97);
    for (int k = 0; k < 5; ++k)
        CHECK (std::abs (crest[k]) < 2.5);

    // the spec's drum signal with the independent micro-dynamics detector
    const auto drum = drumSignal (8.0);
    const double dIn = microStdDb (drum, at (2.0));
    const double dCrest = meanCrestDb (drum, at (2.0));
    double ratio[2] {}, dc[2] {};
    for (int k = 0; k < 2; ++k)
    {
        Rig rig;
        rig.setAmount (1, k == 0 ? 100.0f : -100.0f);
        rig.prepare();
        const auto out = rig.run (drum);
        ratio[k] = microStdDb (out.l, at (2.0)) / dIn;
        Rig rig50;
        rig50.setAmount (1, k == 0 ? 50.0f : -50.0f);
        rig50.prepare();
        const auto out50 = rig50.run (drum);
        dc[k] = meanCrestDb (out50.l, at (2.0)) - dCrest;
    }
    pstest::note ("drum: micro std ratio at +100", ratio[0]);
    pstest::note ("drum: micro std ratio at -100", ratio[1]);
    pstest::note ("drum: crest change at +50 (dB)", dc[0]);
    pstest::note ("drum: crest change at -50 (dB)", dc[1]);
    // measured with the fitted law: 1.13 / 0.92, crest -1.0 / +1.0 dB
    CHECK (ratio[0] >= 1.05 && ratio[0] <= 1.2);
    CHECK (ratio[1] >= 0.85 && ratio[1] <= 0.97);
    CHECK (std::abs (dc[0]) < 1.5);
    CHECK (std::abs (dc[1]) < 1.5);
}

TEST_CASE ("pnch. steady sines are unaffected after settling, without distortion")
{
    for (double hz : { 40.0, 1000.0 })
        for (float env : { 10.0f, 100.0f, 1000.0f })
            for (float amount : { 100.0f, -100.0f })
            {
                Rig rig;
                rig.set ("env", env);
                rig.setAmount (1, amount);
                rig.prepare();
                const auto x = pstest::sine (hz, -10.0, 4.0, kSr);
                const auto out = rig.run (x);
                const size_t s = at (3.0), n = at (1.0);
                const double lvl = 20.0 * std::log10 (pstest::rmsOf (out.l, s, n) / pstest::rmsOf (x, s, n));
                const double thd = thdNDb (out.l, s, n, hz);
                if (env == 10.0f || env == 100.0f)
                {
                    pstest::note (std::to_string ((int) hz) + " Hz, env " + std::to_string ((int) env) + ", amount "
                                      + std::to_string ((int) amount) + ": level (dB)", lvl);
                    pstest::note ("   THD+N (dB)", thd);
                }
                // at E = 1000 ms the long envelope rises over 10 s: a start-up remainder is still decaying at 3 s
                CHECK (std::abs (lvl) < (env < 1000.0f ? 0.1 : 1.0));
                CHECK (thd < (hz < 100.0 ? -55.0 : -60.0));
            }
}

// Level (dB rel. the 40 Hz fundamental) of the strongest of the given lines; window = whole periods of everything.
double linesDb (const std::vector<float>& y, size_t start, size_t n, std::initializer_list<double> freqs)
{
    const double f = std::abs (pstest::toneOf (y, start, n, 40.0, kSr));
    double worst = 0.0;
    for (double hz : freqs)
        worst = std::max (worst, std::abs (pstest::toneOf (y, start, n, hz, kSr)));
    return pstest::dbOf (worst / f);
}

TEST_CASE ("pnch. no low-frequency distortion from fast envelopes (40 Hz sine with a kick on top)")
{
    // A 40 Hz sine at -12 dBFS plus a kick every T = 0.50625 s whose energy sits above 1 kHz (a 2.5 kHz click). The gain
    // moves with every hit (that is the effect) and modulates the sine: those products sit at 40 + k / T. With 80 T =
    // 40.5, the lines 80, 120, 160 and 240 Hz are never such products over a 4 T window: they can only come from a
    // detector rippling at 2f (and its products). A plain x^2 detector with the same ballistics is measured as the
    // sensitivity check.
    const size_t period = 24300, win = 4 * period;
    std::vector<float> x (3 * win);
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) (i % period) / kSr;
        x[i] = (float) (0.25 * std::sin (2.0 * kPi * 40.0 * (double) i / kSr)
                        + 0.4 * std::exp (-t / 0.008) * std::sin (2.0 * kPi * 2500.0 * t) * std::min (1.0, t / 0.0003));
    }
    const auto lines = { 80.0, 120.0, 160.0, 240.0 };
    const double inDb = linesDb (x, win, win, lines);
    pstest::note ("input: strongest of 80 / 120 / 160 / 240 Hz (dB rel. 40 Hz)", inDb);
    for (float env : { 10.0f, 100.0f })
        for (float amount : { 100.0f, -100.0f })
        {
            Rig rig;
            rig.set ("env", env);
            rig.setAmount (1, amount);
            rig.prepare();
            const auto out = rig.run (x);
            const double h = linesDb (out.l, win, win, lines);

            // the same gain law on (1) a plain x^2 detector and (2) the exact analytic envelope of the synthetic signal
            // |A e^{j w1 t} + B(t) e^{j w2 t}|^2 (an ideal, delay-free envelope: it still beats with the bass phase
            // at every hit, so it sets the floor any envelope detector has on this signal)
            const double e = env, k = amount * 0.01;
            const double ca = 1.0 - std::exp (-1.0 / (std::clamp (e / 100.0, 0.2, 2.0) * 0.001 * kSr));
            const double cr = 1.0 - std::exp (-1.0 / (std::max (1.0, e / 10.0) * 0.001 * kSr));
            const double cl = 1.0 - std::exp (-1.0 / (e * 0.001 * kSr));
            auto law = [&] (const std::function<double (size_t)>& power)
            {
                double as = 0.0, al = 0.0;
                std::vector<float> y (x.size());
                for (size_t i = 0; i < x.size(); ++i)
                {
                    const double p = power (i);
                    as += (p > as ? ca : cr) * (p - as);
                    al += cl * (p - al);
                    const double rel = std::clamp (10.0 * std::log10 ((as + 1e-9) / (al + 1e-9)), -40.0, 24.0);
                    y[i] = (float) (x[i] * std::pow (10.0, std::clamp (k * rel, -24.0, 12.0) / 20.0));
                }
                return y;
            };
            const double hn = linesDb (law ([&] (size_t i) { return 2.0 * (double) x[i] * x[i]; }), win, win, lines);
            const double hi = linesDb (law ([&] (size_t i) {
                                           const double t = (double) (i % period) / kSr;
                                           const double bAmp = 0.4 * std::exp (-t / 0.008) * std::min (1.0, t / 0.0003);
                                           const cd z = 0.25 * std::polar (1.0, 2.0 * kPi * 40.0 * (double) i / kSr)
                                                        + bAmp * std::polar (1.0, 2.0 * kPi * 2500.0 * t);
                                           return std::norm (z);
                                       }),
                                       win, win, lines);
            pstest::note ("env " + std::to_string ((int) env) + " amount " + std::to_string ((int) amount)
                              + ": distortion lines (dB rel. 40 Hz)", h);
            pstest::note ("   ideal analytic envelope, same law", hi);
            pstest::note ("   plain x^2 detector, same law", hn);
            CHECK (h < hi + 6.0);      // as clean as an ideal envelope (within 6 dB)
            CHECK (h < hn - 30.0);     // and far from what a squaring detector does
            CHECK (h < -50.0);
        }
}

TEST_CASE ("pnch. no distortion on an amplitude-modulated 40 Hz tone")
{
    // 40 Hz with a 3 Hz +-6 dB (9.5 dB p-p) modulation: Punch reshapes the modulation (lines 40 + 3 k), while 80, 120,
    // 200 and 240 Hz are never such lines and stay clean (the onset floor of the detector must not ripple here).
    std::vector<float> x (at (4.0));
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / kSr;
        x[i] = (float) (0.2 * (1.0 + 0.5 * std::sin (2.0 * kPi * 3.0 * t)) * std::sin (2.0 * kPi * 40.0 * t));
    }
    for (float env : { 10.0f, 100.0f })
        for (float amount : { 100.0f, -100.0f })
        {
            Rig rig;
            rig.set ("env", env);
            rig.setAmount (1, amount);
            rig.prepare();
            const auto out = rig.run (x);
            const double h = linesDb (out.l, at (2.0), at (2.0), { 80.0, 120.0, 200.0, 240.0 });
            pstest::note ("env " + std::to_string ((int) env) + " amount " + std::to_string ((int) amount)
                              + ": distortion lines (dB rel. 40 Hz)", h);
            CHECK (h < -60.0);
        }
}

TEST_CASE ("pnch. sync: note values at 120 and 87 BPM equal the ms mode")
{
    PunchParams p;
    p.sync = true;
    const char* names[18] { "1/64T", "1/64", "1/64D", "1/32T", "1/32", "1/32D", "1/16T", "1/16", "1/16D",
                            "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2T", "1/2", "1/2D" };
    for (double bpm : { 120.0, 87.0 })
        for (int k = 0; k < kPunchSyncNotes; ++k)
        {
            p.syncNote = k;
            const double expect = std::clamp (kPunchSyncFractions[(size_t) k] * 240000.0 / bpm, 10.0, 1000.0);
            CHECK_NEAR (punchEnvelopeMs (p, bpm), expect, 1e-9);
            if (k == 7 || k == 13 || k == 17)
                pstest::note (std::string (names[k]) + " at " + std::to_string ((int) bpm) + " BPM (ms)",
                              punchEnvelopeMs (p, bpm));
        }
    p.syncNote = 7;
    CHECK_NEAR (punchEnvelopeMs (p, 120.0), 125.0, 1e-9);
    CHECK_NEAR (punchEnvelopeMs (p, 87.0), 60000.0 / 87.0 / 4.0, 1e-9);
    CHECK_NEAR (punchEnvelopeMs (p, 60.0), 250.0, 1e-9);
    CHECK_NEAR (punchEnvelopeMs (p, 0.0), 125.0, 1e-9);
    CHECK_NEAR (punchEnvelopeMs (p, std::nan ("")), 125.0, 1e-9);
    p.syncNote = 17;
    CHECK_NEAR (punchEnvelopeMs (p, 120.0), 1000.0, 1e-9);
    p.syncNote = 0;
    CHECK_NEAR (punchEnvelopeMs (p, 120.0), 20.8333333333, 1e-6);
    p.sync = false;
    p.envelopeMs = 3.0f;
    CHECK_NEAR (punchEnvelopeMs (p, 120.0), 10.0, 1e-9);

    // audio: synced 1/16 equals the ms mode at the same time, at 120 and 87 BPM
    const auto drum = drumSignal (3.0);
    for (double bpm : { 120.0, 87.0 })
    {
        Rig a, b;
        a.ctx.bpm = bpm;
        b.ctx.bpm = bpm;
        a.set ("sync", 1.0f);
        a.set ("syncNote", 7.0f);
        b.set ("env", (float) (60000.0 / bpm * 0.25));
        a.setAmount (1, 70.0f);
        b.setAmount (1, 70.0f);
        a.prepare();
        b.prepare();
        const auto oa = a.run (drum);
        const auto ob = b.run (drum);
        double d = 0.0;
        for (size_t i = 0; i < oa.l.size(); ++i)
            d = std::max (d, (double) std::abs (oa.l[i] - ob.l[i]));
        pstest::note ("sync vs ms max |diff| at " + std::to_string ((int) bpm) + " BPM", d);
        CHECK (d < 1e-6);
    }
}

TEST_CASE ("pnch. stereo link and M/S sets")
{
    // L = bursts, R = steady sine (in quadrature pairs so the gains are exact)
    const auto e = burstEnvelope (4.0, 0.4, 0.4, 40.0, 0.004);
    std::vector<float> l (e.size()), r (e.size());
    for (size_t i = 0; i < e.size(); ++i)
    {
        l[i] = (float) (e[i] * std::sin (2.0 * kPi * 900.0 * (double) i / kSr));
        r[i] = (float) (0.1 * std::sin (2.0 * kPi * 1000.0 * (double) i / kSr));
    }
    auto gainOf = [] (const std::vector<float>& in, const std::vector<float>& out, size_t from, double thr)
    {
        std::vector<double> g;
        for (size_t i = from; i < in.size(); ++i)
            g.push_back (std::abs (in[i]) > thr ? 20.0 * std::log10 (std::abs (out[i] / in[i])) : 1e9);
        return g;
    };
    {
        Rig rig;
        rig.setAmount (1, 80.0f);
        rig.prepare();
        const auto out = rig.run (l, r);
        double worst = 0.0;
        for (double g : gainOf (r, out.r, at (1.0), 0.05))
            if (g < 1e8)
                worst = std::max (worst, std::abs (g));
        pstest::note ("link off: steady R max |gain| (dB)", worst);
        CHECK (worst < 0.05);
    }
    {
        Rig rig;
        rig.set ("stLink", 1.0f);
        rig.setAmount (1, 80.0f);
        rig.prepare();
        const auto out = rig.run (l, r);
        const auto gl = gainOf (l, out.l, at (1.0), 0.02);
        const auto gr = gainOf (r, out.r, at (1.0), 0.05);
        double worst = 0.0, swing = 0.0;
        for (size_t i = 0; i < gl.size(); ++i)
            if (gl[i] < 1e8 && gr[i] < 1e8)
            {
                worst = std::max (worst, std::abs (gl[i] - gr[i]));
                swing = std::max (swing, std::abs (gr[i]));
            }
        pstest::note ("link on: max |gain L - gain R| (dB)", worst);
        pstest::note ("link on: R gain swing (dB)", swing);
        CHECK (worst < 0.01);
        CHECK (swing > 3.0);
    }
    {
        // M/S: set A (Mid) active, set B (Side) amount 0 -> the Side channel passes exactly
        Rig rig;
        rig.ctx.channelMode = ChannelMode::MidSide;
        rig.setBand (0, 1, "amount", 80.0f);
        rig.prepare();
        const auto out = rig.run (l, r);
        CHECK (out.r == r);
        CHECK (! (out.l == l));
        // set B drives ch1 only
        Rig rig2;
        rig2.ctx.channelMode = ChannelMode::MidSide;
        rig2.setBand (1, 1, "amount", 80.0f);
        rig2.prepare();
        const auto out2 = rig2.run (r, l);
        CHECK (out2.l == r);
        CHECK (! (out2.r == l));
    }
}

TEST_CASE ("pnch. T/S rule: a paramSet 1 instance reads set B")
{
    const auto drum = drumSignal (2.0);
    Rig a, b;
    b.punch->setParamSet (1);
    a.setBand (1, 1, "amount", 70.0f);   // set B only
    b.setBand (1, 1, "amount", 70.0f);
    a.prepare();
    b.prepare();
    const auto oa = a.run (drum);
    const auto ob = b.run (drum);
    CHECK (oa.l == drum);                 // set A instance: nothing
    CHECK (! (ob.l == drum));             // twin: set B drives both channels
    CHECK (ob.l == ob.r);
}

TEST_CASE ("pnch. bands are independent")
{
    // 4 bands (120 / 1k / 6k), only band 3 at +80: a steady 60 Hz tone keeps its level, 2.5 kHz bursts expand
    const auto tone = pstest::sine (60.0, -12.0, 5.0, kSr);
    std::vector<float> bursts (tone.size());
    const auto e = burstEnvelope (5.0, 0.4, 0.3, 30.0, 0.003);
    for (size_t i = 0; i < bursts.size(); ++i)
        bursts[i] = (float) (e[i] * std::sin (2.0 * kPi * 2500.0 * (double) i / kSr));
    const auto mix = pstest::added (tone, bursts);

    Rig rig;
    rig.set ("bands", 4.0f);
    rig.setAmount (3, 80.0f);
    rig.prepare();
    const auto out = rig.run (mix);

    const size_t s = at (2.0), n = at (3.0);
    const double t60 = pstest::dbOf (std::abs (pstest::toneOf (out.l, s, n, 60.0, kSr)))
                       - pstest::dbOf (std::abs (pstest::toneOf (tone, s, n, 60.0, kSr)));
    pstest::note ("60 Hz level change (dB)", t60);
    CHECK (std::abs (t60) < 0.05);

    // crest of the high band: isolate it by subtracting the processed tone-only run
    Rig toneOnly;
    toneOnly.set ("bands", 4.0f);
    toneOnly.setAmount (3, 80.0f);
    toneOnly.prepare();
    const auto outTone = toneOnly.run (tone);
    Rig dry;
    dry.set ("bands", 4.0f);
    dry.prepare();
    const auto outDry = dry.run (mix);
    double pkIn = 0.0, pkOut = 0.0;
    for (size_t i = s; i < s + n; ++i)
    {
        pkIn = std::max (pkIn, std::abs ((double) outDry.l[i]));
        pkOut = std::max (pkOut, std::abs ((double) out.l[i]));
    }
    const double crestChange = meanCrestDb (out.l, s) - meanCrestDb (outDry.l, s);
    pstest::note ("mix crest change with band 3 at +80 (dB)", crestChange);
    pstest::note ("peak change (dB)", 20.0 * std::log10 (pkOut / pkIn));
    CHECK (crestChange > 1.0);
    (void) outTone;
}

TEST_CASE ("pnch. auto gain keeps the long-term level per band")
{
    const auto e = burstEnvelope (12.0, 0.4, 0.4, 30.0, 0.01);
    const auto in = quadTone (e, 1000.0);
    // 2 bands: low = steady 80 Hz sine, high = the bursts
    Stereo mix = in;
    for (size_t i = 0; i < mix.l.size(); ++i)
    {
        const float lo = (float) (0.1 * std::sin (2.0 * kPi * 80.0 * (double) i / kSr));
        mix.l[i] += lo;
        mix.r[i] += lo;
    }
    double diff[2] {};
    for (int ag = 0; ag < 2; ++ag)
    {
        Rig rig;
        rig.set ("bands", 2.0f);
        rig.set ("xover1", 300.0f);
        rig.set ("autoGain", (float) ag);
        rig.setAmount (1, 60.0f);
        rig.setAmount (2, 60.0f);
        rig.prepare();
        const auto out = rig.run (mix.l, mix.r);
        const size_t s = at (6.0), n = mix.l.size() - at (6.0);
        diff[ag] = 20.0 * std::log10 (pstest::rmsOf (out.l, s, n) / pstest::rmsOf (mix.l, s, n));
        pstest::note (std::string ("auto gain ") + (ag ? "on" : "off") + ": long-term RMS out - in (dB)", diff[ag]);
        if (ag == 1)
        {
            const auto& m = rig.punch->getMeters();
            pstest::note ("   makeup band 2 (dB)", m.makeupDb[0][1].load());
            pstest::note ("   makeup band 1 (dB)", m.makeupDb[0][0].load());
            CHECK (std::abs (m.makeupDb[0][1].load()) > 0.3);   // it really corrects
            CHECK (std::abs (m.makeupDb[0][1].load()) < 12.0);
            // per band: the high band's RMS through a processed / dry comparison of the burst-only signal
            Rig hb;
            hb.set ("bands", 2.0f);
            hb.set ("xover1", 300.0f);
            hb.set ("autoGain", 1.0f);
            hb.setAmount (2, 60.0f);
            hb.prepare();
            const auto ob = hb.run (in.l, in.r);
            const double band = 20.0 * std::log10 (pstest::rmsOf (ob.l, s, n) / pstest::rmsOf (in.l, s, n));
            pstest::note ("   burst band alone: RMS out - in (dB)", band);
            CHECK (std::abs (band) < 1.0);
        }
    }
    CHECK (std::abs (diff[1]) < 0.5);
    CHECK (std::abs (diff[0]) > std::abs (diff[1]));
}

TEST_CASE ("pnch. parameter changes are click-free (HF splatter)")
{
    // 1 kHz tone with a slow 12 dB amplitude modulation, so the module has work to do
    const size_t len = at (6.0);
    std::vector<double> env (len);
    for (size_t i = 0; i < len; ++i)
        env[i] = 0.25 * (0.625 + 0.375 * std::sin (2.0 * kPi * 3.0 * (double) i / kSr));
    std::vector<float> x (len);
    for (size_t i = 0; i < len; ++i)
        x[i] = (float) (env[i] * std::sin (2.0 * kPi * 1000.0 * (double) i / kSr));

    auto base = [] (Rig& r)
    {
        r.set ("bands", 2.0f);
        r.set ("xover1", 300.0f);
        r.setAmount (1, 50.0f);
        r.setAmount (2, 50.0f);
    };
    Rig ref;
    base (ref);
    ref.prepare();
    const double refSplat = maxSplatter (ref.run (x).l, at (1.0));

    struct Case
    {
        const char* name;
        std::function<void (Rig&, size_t)> automate;
    };
    const double blockS = 512.0 / kSr;
    const std::vector<Case> cases {
        { "amount sweep -100 -> +100 in 50 ms", [blockS] (Rig& r, size_t s) {
              const double t = (double) s / kSr;
              const double ph = std::fmod (t, 0.1) / 0.05;
              const float a = (float) (ph < 1.0 ? -100.0 + 200.0 * ph : 100.0 - 200.0 * (ph - 1.0));
              r.setAmount (2, a);
              (void) blockS;
          } },
        { "global amount 0 <-> 100", [] (Rig& r, size_t s) { r.set ("amount", (s / 4800) % 2 ? 0.0f : 100.0f); } },
        { "envelope 10 <-> 1000 ms", [] (Rig& r, size_t s) { r.set ("env", (s / 9600) % 2 ? 10.0f : 1000.0f); } },
        { "sync toggles", [] (Rig& r, size_t s) { r.set ("sync", (float) ((s / 9600) % 2)); } },
        { "band on / off", [] (Rig& r, size_t s) { r.setBand (0, 2, "on", (float) ((s / 9600) % 2)); } },
        { "stereo link toggles", [] (Rig& r, size_t s) { r.set ("stLink", (float) ((s / 9600) % 2)); } },
        { "auto gain toggles", [] (Rig& r, size_t s) { r.set ("autoGain", (float) ((s / 48000) % 2)); } },
        { "band count 2 <-> 3", [] (Rig& r, size_t s) { r.set ("bands", (s / 24000) % 2 ? 3.0f : 2.0f); } },
        { "crossover type Analog <-> Hybrid", [] (Rig& r, size_t s) {
              r.set ("xoverType", (float) ((s / 24000) % 2 ? CrossoverType::Hybrid : CrossoverType::Analog));
          } },
        { "crossover sweep", [] (Rig& r, size_t s) { r.set ("xover1", (float) (200.0 + 600.0 * ((s / 4800) % 2))); } },
        { "band solo", [] (Rig& r, size_t s) {
              ModuleMonitor m;
              m.soloBand = (s / 24000) % 2 ? 1 : -1;
              r.punch->setMonitor (m);
          } },
    };
    pstest::note ("reference splatter (no automation)", refSplat);
    for (const auto& c : cases)
    {
        Rig rig;
        base (rig);
        rig.prepare();
        const auto out = rig.run (x, x, [&] (size_t s) { c.automate (rig, s); });
        const double sp = maxSplatter (out.l, at (1.0));
        pstest::note (std::string (c.name) + ": splatter / reference", sp / refSplat);
        CHECK (sp < 4.0 * refSplat);
        bool finite = true;
        for (float v : out.l)
            finite = finite && std::isfinite (v);
        CHECK (finite);
    }
}

TEST_CASE ("pnch. extreme automation stays stable and bounded")
{
    const auto drum = drumSignal (6.0);
    auto noise = pstest::whiteNoise (0.9, 6.0, kSr, 5);
    const auto in = pstest::added (drum, noise);
    Rig rig (kSr, 256);
    rig.prepare();
    pstest::Lcg lcg (77);
    auto u = [&lcg] { return 0.5f * (lcg.next() + 1.0f); };
    const auto out = rig.run (in, pstest::scaled (in, -1.0f), [&] (size_t) {
        rig.set ("bands", (float) (1 + (int) (u() * 3.99f)));
        rig.set ("xoverType", (float) (int) (u() * 2.99f));
        rig.set ("env", 10.0f + 990.0f * u());
        rig.set ("sync", (float) (u() > 0.5f));
        rig.set ("syncNote", (float) (int) (u() * 17.99f));
        rig.set ("stLink", (float) (u() > 0.5f));
        rig.set ("autoGain", (float) (u() > 0.5f));
        for (int b = 1; b <= 4; ++b)
        {
            rig.setBand (0, b, "amount", -100.0f + 200.0f * u());
            rig.setBand (1, b, "amount", -100.0f + 200.0f * u());
            rig.setBand (0, b, "on", (float) (u() > 0.2f));
        }
        rig.ctx.channelMode = u() > 0.9f ? ChannelMode::MidSide : ChannelMode::Stereo;
        rig.ctx.bpm = 60.0 + 120.0 * u();
    });
    double pk = 0.0, pkIn = pstest::peakOf (in, 0, in.size());
    bool finite = true;
    for (size_t i = 0; i < out.l.size(); ++i)
    {
        finite = finite && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
        pk = std::max ({ pk, std::abs ((double) out.l[i]), std::abs ((double) out.r[i]) });
    }
    pstest::note ("input peak", pkIn);
    pstest::note ("output peak under random automation", pk);
    CHECK (finite);
    CHECK (pk < pkIn * 16.0);   // +12 dB band gain + 12 dB make-up at the very most
}

TEST_CASE ("pnch. denormal safe: long silence after a signal")
{
    Rig rig;
    rig.set ("bands", 4.0f);
    rig.set ("autoGain", 1.0f);
    for (int b = 1; b <= 4; ++b)
        rig.setAmount (b, 70.0f);
    rig.prepare();
    auto x = drumSignal (2.0);
    x.resize (at (40.0), 0.0f);
    pstest::Stopwatch sw;
    const auto out = rig.run (x);
    const double total = sw.seconds();
    bool finite = true, subnormal = false;
    for (size_t i = at (2.0); i < out.l.size(); ++i)
    {
        finite = finite && std::isfinite (out.l[i]);
        subnormal = subnormal || std::fpclassify (out.l[i]) == FP_SUBNORMAL;
    }
    CHECK (finite);
    CHECK (! subnormal);
    CHECK_EQ (out.l.back(), 0.0f);
    pstest::note ("40 s (38 s silence), 4 bands: x realtime", 40.0 / total);
    CHECK (40.0 / total > 30.0);
}

TEST_CASE ("pnch. no allocation in setParams / process / reset / setMonitor")
{
    const auto drum = drumSignal (0.5);
    for (int type = 0; type < 3; ++type)
        for (int bands = 1; bands <= 4; ++bands)
            for (auto mode : { ChannelMode::Stereo, ChannelMode::MidSide })
            {
                Rig rig (kSr, 256);
                rig.set ("bands", (float) bands);
                rig.set ("xoverType", (float) type);
                rig.set ("autoGain", 1.0f);
                for (int b = 1; b <= 4; ++b)
                    rig.setAmount (b, 50.0f);
                rig.ctx.channelMode = mode;
                rig.prepare();
                Stereo buf { drum, drum };
                pstest::AllocationScope scope;
                for (size_t s = 0; s + 256 <= buf.l.size(); s += 256)
                {
                    rig.set ("stLink", (float) ((s / 2560) % 2));
                    rig.set ("bands", (float) (1 + (s / 4096) % 4));
                    rig.set ("sync", (float) ((s / 3000) % 2));
                    rig.punch->setParams (rig.values.data());
                    ModuleMonitor m;
                    m.soloBand = (int) ((s / 5000) % 3) - 1;
                    rig.punch->setMonitor (m);
                    float* ch[2] { buf.l.data() + s, buf.r.data() + s };
                    rig.punch->process (ch, 256, rig.ctx);
                    if (s == 256 * 40)
                        rig.punch->reset();
                }
                (void) rig.punch->getLatencySamples();
                CHECK_EQ (scope.count(), 0LL);
            }
}

TEST_CASE ("pnch. meters: gain extremes bracket the applied gain, micro meters and makeup are real")
{
    Rig rig;
    rig.set ("stLink", 1.0f);
    rig.setAmount (1, 60.0f);
    rig.prepare();
    const auto e = burstEnvelope (5.0, 0.4, 0.4, 30.0, 0.004);
    const auto in = quadTone (e, 1000.0);
    auto& m = rig.punch->getMeters();
    Stereo out { in.l, in.r };
    double worstBracket = 0.0, worstLast = 0.0;
    for (size_t s = 0; s + 512 <= in.l.size(); s += 512)
    {
        rig.punch->setParams (rig.values.data());
        float* ch[2] { out.l.data() + s, out.r.data() + s };
        rig.punch->process (ch, 512, rig.ctx);
        const float lo = m.gainMinDb[0][0].exchange (200.0f);
        const float hi = m.gainMaxDb[0][0].exchange (-200.0f);
        if (s > at (1.0))
        {
            double glo = 1e9, ghi = -1e9;
            for (size_t i = s; i < s + 512; ++i)
            {
                const double a = std::hypot ((double) in.l[i], (double) in.r[i]);
                const double b = std::hypot ((double) out.l[i], (double) out.r[i]);
                const double g = 20.0 * std::log10 (b / a);
                glo = std::min (glo, g);
                ghi = std::max (ghi, g);
            }
            worstBracket = std::max ({ worstBracket, std::abs (glo - lo), std::abs (ghi - hi) });
            const size_t i = s + 511;
            const double g = 20.0 * std::log10 (std::hypot ((double) out.l[i], (double) out.r[i])
                                                / std::hypot ((double) in.l[i], (double) in.r[i]));
            worstLast = std::max (worstLast, std::abs (g - m.gainDb[0][0].load()));
            CHECK (m.gainDb[1][0].load() == m.gainDb[0][0].load());   // set B mirrors A in Stereo
        }
    }
    pstest::note ("max |meter min/max - measured block min/max| (dB)", worstBracket);
    pstest::note ("max |gainDb - measured gain at block end| (dB)", worstLast);
    CHECK (worstBracket < 0.02);
    CHECK (worstLast < 0.02);
    pstest::note ("burst train: micro in (dB)", m.microInDb[0][0].load());
    pstest::note ("burst train: micro out (dB)", m.microOutDb[0][0].load());
    CHECK (m.microInDb[0][0].load() > 2.0);
    CHECK (m.microOutDb[0][0].load() > m.microInDb[0][0].load() * 1.3);

    Rig steady;
    steady.setAmount (1, 60.0f);
    steady.prepare();
    steady.run (pstest::sine (1000.0, -10.0, 5.0, kSr));
    pstest::note ("steady sine: micro in (dB)", steady.punch->getMeters().microInDb[0][0].load());
    CHECK (steady.punch->getMeters().microInDb[0][0].load() < 0.3);
    CHECK (steady.punch->getMeters().microOutDb[0][0].load() < 0.3);
    CHECK (std::abs (steady.punch->getMeters().gainDb[0][0].load()) < 0.1);

    Rig drum;
    drum.setAmount (1, 60.0f);
    drum.prepare();
    drum.run (drumSignal (5.0));
    pstest::note ("drum signal: micro in (dB)", drum.punch->getMeters().microInDb[0][0].load());
    CHECK (drum.punch->getMeters().microInDb[0][0].load() > 3.0);

    // M/S: set B meters follow ch1
    Rig ms;
    ms.ctx.channelMode = ChannelMode::MidSide;
    ms.setBand (1, 1, "amount", 60.0f);
    ms.prepare();
    const auto d = drumSignal (3.0);
    ms.run (d, d);
    CHECK (std::abs (ms.punch->getMeters().gainMaxDb[0][0].load()) < 1e-6);
    CHECK (ms.punch->getMeters().gainMaxDb[1][0].load() > 1.0);
}

TEST_CASE ("pnch. band solo plays that band only")
{
    const auto x = pstest::pinkNoise (0.4, 2.0, kSr, 31);
    Rig rig;
    rig.set ("bands", 3.0f);
    rig.prepare();
    ModuleMonitor m;
    m.soloBand = 1;
    rig.punch->setMonitor (m);
    const auto out = rig.run (x);

    Crossover xo;
    xo.prepare (kSr, 512);
    const float hz[3] { 120.0f, 1000.0f, 6000.0f };
    std::vector<float> buf (4 * 2 * 512);
    float* ptr[4][2];
    float* const* bands[4];
    for (int b = 0; b < 4; ++b)
    {
        ptr[b][0] = buf.data() + (size_t) (b * 2) * 512;
        ptr[b][1] = buf.data() + (size_t) (b * 2 + 1) * 512;
        bands[b] = ptr[b];
    }
    std::vector<float> ref (x.size());
    for (size_t s = 0; s < x.size(); s += 512)
    {
        xo.setup (3, hz, CrossoverType::Analog);
        const int n = (int) std::min<size_t> (512, x.size() - s);
        const float* in[2] { x.data() + s, x.data() + s };
        xo.process (in, bands, n);
        for (int i = 0; i < n; ++i)
            ref[s + (size_t) i] = ptr[1][0][i];
    }
    const double d = nullDb (out.l, ref, 0, 0);
    pstest::note ("solo band 2 vs crossover band 2 (dBFS)", d);
    CHECK (d < -140.0);
}

TEST_CASE ("pnch. other sample rates keep the time constants")
{
    for (double sr : { 44100.0, 96000.0 })
    {
        Rig rig (sr, 512);
        rig.set ("stLink", 1.0f);
        rig.setAmount (1, 50.0f);
        rig.prepare();
        std::vector<double> e (at (3.0, sr));
        for (size_t i = 0; i < e.size(); ++i)
            e[i] = pstest::dbfsToAmplitude (i < at (2.0, sr) ? -30.0 : -10.0);
        const auto in = quadTone (e, 1000.0, sr);
        const auto out = rig.run (in.l, in.r);
        const auto g = gainTraceDb (in, out);
        double pk = -100.0;
        for (size_t i = at (2.0, sr); i < at (2.05, sr); ++i)
            pk = std::max (pk, g[i]);
        pstest::note ("peak gain at " + std::to_string ((int) sr), pk);
        pstest::note ("   gain at 100 ms", g[at (2.1, sr)]);
        CHECK (pk >= 2.2 && pk <= 3.4);
        CHECK (g[at (2.1, sr)] >= 2.2 && g[at (2.1, sr)] <= 3.1);
    }
}

TEST_CASE ("pnch. cpu benchmark")
{
    const auto noise = pstest::whiteNoise (0.5, 10.0, kSr, 9);
    struct Bench
    {
        const char* name;
        int bands;
        CrossoverType type;
        ChannelMode mode;
        bool link;
    };
    for (const auto& b : { Bench { "1 band Stereo (typical)", 1, CrossoverType::Analog, ChannelMode::Stereo, false },
                           Bench { "3 bands Analog Stereo linked", 3, CrossoverType::Analog, ChannelMode::Stereo, true },
                           Bench { "4 bands Analog M/S", 4, CrossoverType::Analog, ChannelMode::MidSide, false },
                           Bench { "4 bands Hybrid Stereo", 4, CrossoverType::Hybrid, ChannelMode::Stereo, false },
                           Bench { "4 bands Digital M/S (worst)", 4, CrossoverType::Digital, ChannelMode::MidSide, false } })
    {
        Rig rig;
        rig.set ("bands", (float) b.bands);
        rig.set ("xoverType", (float) b.type);
        rig.set ("stLink", b.link ? 1.0f : 0.0f);
        rig.set ("autoGain", 1.0f);
        for (int k = 1; k <= 4; ++k)
            rig.setAmount (k, 40.0f);
        rig.ctx.channelMode = b.mode;
        rig.prepare();
        double best = 1e9;
        for (int rep = 0; rep < 3; ++rep)   // best of 3 (the build machine is shared)
        {
            pstest::Stopwatch sw;
            rig.run (noise);
            best = std::min (best, sw.seconds());
        }
        const double x = 10.0 / best;
        pstest::note (std::string (b.name) + ": x realtime", x);
        CHECK_SPEED (x > 30.0);
    }
}

TEST_CASE ("pnch. gain meters follow the rendered gain through fades and live makeup")
{
    Rig rig (kSr, 48);
    rig.set ("stLink", 1.0f);
    rig.set ("autoGain", 1.0f);
    rig.setAmount (1, 80.0f);
    rig.prepare();
    const auto in = quadTone (burstEnvelope (6.5, 0.4, 0.4, 30.0, 0.004), 1000.0);
    Stereo out { in.l, in.r };
    auto& meters = rig.punch->getMeters();
    double worstLast = 0.0, worstExtrema = 0.0, learnedMakeup = 0.0;
    for (size_t s = 0; s < in.l.size(); s += 48)
    {
        if (s == at (5.28))
            rig.setBand (0, 1, "on", 0.0f);
        if (s == at (5.68))
            rig.setBand (0, 1, "on", 1.0f);
        rig.punch->setParams (rig.values.data());
        const int n = (int) std::min<size_t> (48, in.l.size() - s);
        float* ch[2] { out.l.data() + s, out.r.data() + s };
        rig.punch->process (ch, n, rig.ctx);
        const double lo = meters.gainMinDb[0][0].exchange (200.0f);
        const double hi = meters.gainMaxDb[0][0].exchange (-200.0f);
        if (s < at (4.0))
            continue;
        learnedMakeup = std::max (learnedMakeup, std::abs ((double) meters.makeupDb[0][0].load()));
        double measuredMin = 1e9, measuredMax = -1e9, measuredLast = 0.0;
        for (size_t i = s; i < s + (size_t) n; ++i)
        {
            const double input = std::hypot ((double) in.l[i], (double) in.r[i]);
            const double output = std::hypot ((double) out.l[i], (double) out.r[i]);
            measuredLast = 20.0 * std::log10 (output / input);
            measuredMin = std::min (measuredMin, measuredLast);
            measuredMax = std::max (measuredMax, measuredLast);
        }
        worstLast = std::max (worstLast, std::abs (measuredLast - meters.gainDb[0][0].load()));
        worstExtrema = std::max ({ worstExtrema, std::abs (measuredMin - lo), std::abs (measuredMax - hi) });
    }
    pstest::note ("learned makeup during meter fade test (dB)", learnedMakeup);
    pstest::note ("fade/live makeup: largest gain meter error (dB)", worstLast);
    pstest::note ("fade/live makeup: largest extrema error (dB)", worstExtrema);
    CHECK (learnedMakeup > 0.5);
    CHECK (worstLast < 0.00002);
    CHECK (worstExtrema < 0.00002);
}

TEST_CASE ("pnch. disabled band micro meters describe its unchanged output")
{
    const auto in = quadTone (burstEnvelope (8.0, 0.4, 0.4, 30.0, 0.004), 1000.0);
    for (bool linked : { false, true })
    {
        Rig rig;
        rig.set ("stLink", linked ? 1.0f : 0.0f);
        rig.setAmount (1, 80.0f);
        rig.setBand (0, 1, "on", 0.0f);
        rig.prepare();
        const auto out = rig.run (in.l, in.r);
        CHECK (out.l == in.l);
        CHECK (out.r == in.r);
        const auto& meters = rig.punch->getMeters();
        const double input = meters.microInDb[0][0].load();
        const double output = meters.microOutDb[0][0].load();
        pstest::note (std::string (linked ? "linked" : "independent") + " disabled band: |micro out - in| (dB)",
                      std::abs (output - input));
        CHECK (input > 2.0);
        CHECK (std::abs (output - input) < 0.01);
    }
}

TEST_CASE ("pnch. stereo link also joins previously independent auto gain within its ramp")
{
    Rig rig (kSr, 48);
    rig.set ("autoGain", 1.0f);
    rig.setAmount (1, 80.0f);
    rig.prepare();
    const auto env = burstEnvelope (12.5, 0.4, 0.4, 30.0, 0.004);
    Stereo in { std::vector<float> (env.size()), std::vector<float> (env.size()) };
    for (size_t i = 0; i < env.size(); ++i)
    {
        // This phase keeps every 48 kHz sample away from a zero, allowing exact gain measurements on both channels.
        const double carrier = std::sin (2.0 * kPi * 1000.0 * (double) i / kSr + 0.17);
        in.l[i] = (float) (env[i] * carrier);
        in.r[i] = (float) (0.1 * carrier);
    }
    const size_t change = at (12.2), linkedAt = change + at (0.02);
    const auto out = rig.run (in.l, in.r, [&] (size_t s) {
        if (s == change)
            rig.set ("stLink", 1.0f);
    });
    double independentDifference = 0.0, linkedDifference = 0.0, worstGainStep = 0.0;
    double previousL = 0.0, previousR = 0.0;
    for (size_t i = change - at (0.01); i < change + at (0.08); ++i)
    {
        const double gainL = 20.0 * std::log10 ((double) out.l[i] / in.l[i]);
        const double gainR = 20.0 * std::log10 ((double) out.r[i] / in.r[i]);
        if (i < change)
            independentDifference = std::max (independentDifference, std::abs (gainL - gainR));
        if (i >= linkedAt)
            linkedDifference = std::max (linkedDifference, std::abs (gainL - gainR));
        if (i > change - at (0.01))
            worstGainStep = std::max ({ worstGainStep, std::abs (gainL - previousL), std::abs (gainR - previousR) });
        previousL = gainL;
        previousR = gainR;
    }
    pstest::note ("trained independent L/R gain difference before link (dB)", independentDifference);
    pstest::note ("L/R gain difference after the 20 ms link ramp (dB)", linkedDifference);
    pstest::note ("largest adjacent gain step across link change (dB)", worstGainStep);
    CHECK (independentDifference > 1.0);
    CHECK (linkedDifference < 0.00002);
    CHECK (worstGainStep < 0.15);
}

TEST_CASE ("pnch. band-count automation has identical sample timing across host blocks")
{
    const auto l = drumSignal (3.0);
    const auto r = pstest::added (pstest::scaled (l, 0.5f), pstest::pinkNoise (0.03, 3.0, kSr, 123));
    const std::array<size_t, 5> events { at (0.75), at (1.25), at (1.75), at (2.25), l.size() };
    const std::array<int, 4> counts { 2, 3, 1, 4 };
    Stereo reference;
    for (int block : { 1, 64, 333, 512, 2048 })
    {
        Rig rig (kSr, 512);
        rig.set ("bands", 3.0f);
        rig.setAmount (1, 80.0f);
        rig.setAmount (2, -60.0f);
        rig.setAmount (3, 35.0f);
        rig.setAmount (4, -80.0f);
        rig.prepare();
        Stereo out { l, r };
        size_t event = 0;
        for (size_t s = 0; s < l.size();)
        {
            if (s == events[event])
            {
                rig.set ("bands", (float) counts[event]);
                ++event;
            }
            rig.punch->setParams (rig.values.data());
            const int n = (int) std::min ((size_t) block, events[event] - s);
            float* ch[2] { out.l.data() + s, out.r.data() + s };
            rig.punch->process (ch, n, rig.ctx);
            s += (size_t) n;
        }
        if (block == 1)
            reference = out;
        else
        {
            const double difference = std::max (nullDb (out.l, reference.l, 0, 0), nullDb (out.r, reference.r, 0, 0));
            pstest::note ("band automation, block " + std::to_string (block) + ": max difference vs block 1 (dBFS)",
                          difference);
            CHECK (out.l == reference.l);
            CHECK (out.r == reference.r);
        }
    }
}

TEST_CASE ("pnch. a soloed band fades when reducing the band count merges a neighbour")
{
    Rig rig;
    rig.set ("bands", 3.0f);
    rig.prepare();
    ModuleMonitor monitor;
    monitor.soloBand = 1;
    rig.punch->setMonitor (monitor);
    const auto in = quadTone (std::vector<double> (at (1.5), 0.25), 5000.0);
    bool changed = false;
    const auto out = rig.run (in.l, in.r, [&] (size_t s) {
        if (! changed && s >= at (1.0))
        {
            rig.set ("bands", 2.0f);
            changed = true;
        }
    });
    double worstAmplitudeStep = 0.0;
    for (size_t i = at (0.99); i < at (1.12); ++i)
    {
        const double before = std::hypot ((double) out.l[i - 1], (double) out.r[i - 1]);
        const double after = std::hypot ((double) out.l[i], (double) out.r[i]);
        worstAmplitudeStep = std::max (worstAmplitudeStep, std::abs (after - before));
    }
    const double finalAmplitude = std::hypot ((double) out.l.back(), (double) out.r.back());
    pstest::note ("soloed band merge: largest one-sample amplitude change", worstAmplitudeStep);
    pstest::note ("soloed band merge: final amplitude", finalAmplitude);
    CHECK (finalAmplitude > 0.24);
    CHECK (worstAmplitudeStep < 0.0025);
}

TEST_CASE ("pnch. output micro meter agrees with a fresh detector on the rendered audio")
{
    // A neutral instance is an independent measurement of the actual output stream, including changing makeup.
    const auto in = quadTone (burstEnvelope (8.0, 0.4, 0.4, 30.0, 0.004), 40.0);
    for (bool linked : { false, true })
    {
        Rig processed;
        processed.set ("stLink", linked ? 1.0f : 0.0f);
        processed.set ("autoGain", 1.0f);
        processed.set ("env", 10.0f);
        processed.setAmount (1, 80.0f);
        processed.prepare();
        const auto out = processed.run (in.l, in.r);

        Rig measured;
        measured.set ("stLink", linked ? 1.0f : 0.0f);
        measured.set ("env", 10.0f);
        measured.prepare();
        const auto neutralOut = measured.run (out.l, out.r);
        CHECK (neutralOut.l == out.l);
        CHECK (neutralOut.r == out.r);
        const double reported = processed.punch->getMeters().microOutDb[0][0].load();
        const double observed = measured.punch->getMeters().microInDb[0][0].load();
        pstest::note (std::string (linked ? "linked" : "independent") + " output micro vs rendered-audio detector (dB)",
                      std::abs (reported - observed));
        CHECK (observed > 0.5);
        CHECK (std::abs (reported - observed) < 0.005);
    }
}

TEST_CASE ("pnch. removed solos and newly added bands keep smooth audition routing")
{
    const auto in = quadTone (std::vector<double> (at (1.5), 0.25), 5000.0);
    for (bool removeSolo : { true, false })
    {
        Rig rig (kSr, 48);
        rig.set ("bands", removeSolo ? 3.0f : 2.0f);
        rig.prepare();
        ModuleMonitor monitor;
        monitor.soloBand = removeSolo ? 2 : 0;
        const auto out = rig.run (in.l, in.r, [&] (size_t s) {
            // Set solo after playback starts: inactive bands still have their previous ramp values.
            if (s == at (0.5))
                rig.punch->setMonitor (monitor);
            if (s == at (1.0))
                rig.set ("bands", removeSolo ? 2.0f : 3.0f);
        });
        double largestStep = 0.0, peakDuringChange = 0.0;
        for (size_t i = at (0.99); i < at (1.12); ++i)
        {
            const double before = std::hypot ((double) out.l[i - 1], (double) out.r[i - 1]);
            const double after = std::hypot ((double) out.l[i], (double) out.r[i]);
            largestStep = std::max (largestStep, std::abs (after - before));
            peakDuringChange = std::max (peakDuringChange, after);
        }

        Rig reference (kSr, 48);
        reference.set ("bands", removeSolo ? 2.0f : 3.0f);
        reference.prepare();
        reference.punch->setMonitor (monitor);
        const auto expected = reference.run (in.l, in.r);
        const double finalDifference = std::max (nullDb (out.l, expected.l, 0, at (1.25)),
                                                 nullDb (out.r, expected.r, 0, at (1.25)));
        const std::string label = removeSolo ? "removed solo" : "new band while low band is soloed";
        pstest::note (label + ": largest one-sample amplitude change", largestStep);
        pstest::note (label + ": final routing null (dBFS)", finalDifference);
        CHECK (largestStep < 0.0025);
        CHECK (finalDifference < -120.0);
        if (! removeSolo)
        {
            // A newly activated high band must never leak through its stale, previously unused solo ramp.
            pstest::note ("new band: high-tone amplitude while the low band remains soloed", peakDuringChange);
            CHECK (peakDuringChange < 0.00001);
        }
    }
}

TEST_CASE ("pnch. solo topology requests during hold and fade-in remain smooth")
{
    const auto in = quadTone (std::vector<double> (at (1.5), 0.25), 5000.0);
    ModuleMonitor monitor;
    monitor.soloBand = 1;
    Rig reference (kSr, 48);
    reference.set ("bands", 3.0f);
    reference.prepare();
    reference.punch->setMonitor (monitor);
    const auto expected = reference.run (in.l, in.r);

    // First request at 1 s applies after the 10 ms fade. Its hold ends at 1.04 s and fade-in at 1.06 s.
    for (double secondRequest : { 1.025, 1.05 })
    {
        Rig rig (kSr, 48);
        rig.set ("bands", 3.0f);
        rig.prepare();
        rig.punch->setMonitor (monitor);
        const auto out = rig.run (in.l, in.r, [&] (size_t s) {
            if (s == at (1.0))
                rig.set ("bands", 2.0f);
            if (s == at (secondRequest))
                rig.set ("bands", 3.0f);
        });
        double largestStep = 0.0, heardMergedBand = 0.0;
        for (size_t i = at (0.99); i < at (1.15); ++i)
        {
            const double before = std::hypot ((double) out.l[i - 1], (double) out.r[i - 1]);
            const double after = std::hypot ((double) out.l[i], (double) out.r[i]);
            largestStep = std::max (largestStep, std::abs (after - before));
            heardMergedBand = std::max (heardMergedBand, after);
        }
        const double finalDifference = std::max (nullDb (out.l, expected.l, 0, at (1.25)),
                                                 nullDb (out.r, expected.r, 0, at (1.25)));
        const std::string label = secondRequest < 1.04 ? "request during hold" : "request during fade-in";
        pstest::note (label + ": largest one-sample amplitude change", largestStep);
        pstest::note (label + ": final routing null (dBFS)", finalDifference);
        CHECK (heardMergedBand > 0.2);
        CHECK (largestStep < 0.0025);
        CHECK (finalDifference < -120.0);
    }
}

TEST_CASE ("pnch. gain extrema hold across multiple audio blocks until exchanged")
{
    Rig rig (kSr, 64);
    rig.set ("stLink", 1.0f);
    rig.setAmount (1, 80.0f);
    rig.prepare();
    const auto in = quadTone (burstEnvelope (4.0, 0.4, 0.4, 30.0, 0.004), 1000.0);
    Stereo out { in.l, in.r };
    auto& meters = rig.punch->getMeters();
    double expectedMin = 1e9, expectedMax = -1e9, worstError = 0.0;
    double globalMin = 1e9, globalMax = -1e9;
    int pendingBlocks = 0, reads = 0;
    for (size_t s = 0; s < in.l.size(); s += 64)
    {
        rig.punch->setParams (rig.values.data());
        const int n = (int) std::min<size_t> (64, in.l.size() - s);
        float* ch[2] { out.l.data() + s, out.r.data() + s };
        rig.punch->process (ch, n, rig.ctx);
        for (size_t i = s; i < s + (size_t) n; ++i)
        {
            const double input = std::hypot ((double) in.l[i], (double) in.r[i]);
            const double output = std::hypot ((double) out.l[i], (double) out.r[i]);
            const double gain = 20.0 * std::log10 (output / input);
            expectedMin = std::min (expectedMin, gain);
            expectedMax = std::max (expectedMax, gain);
        }
        if (++pendingBlocks == 37 || s + (size_t) n == in.l.size())
        {
            const double measuredMin = meters.gainMinDb[0][0].exchange (200.0f);
            const double measuredMax = meters.gainMaxDb[0][0].exchange (-200.0f);
            worstError = std::max ({ worstError, std::abs (measuredMin - expectedMin),
                                     std::abs (measuredMax - expectedMax) });
            globalMin = std::min (globalMin, measuredMin);
            globalMax = std::max (globalMax, measuredMax);
            expectedMin = 1e9;
            expectedMax = -1e9;
            pendingBlocks = 0;
            ++reads;
        }
    }
    pstest::note ("37-block held gain extrema: largest measurement error (dB)", worstError);
    CHECK (reads > 50);
    CHECK (globalMin < -3.0);
    CHECK (globalMax > 3.0);
    CHECK (worstError < 0.0001);
}


