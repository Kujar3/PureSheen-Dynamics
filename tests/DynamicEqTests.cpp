#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/DynamicEq.h"
#include "dsp/EqDesign.h"
#include "dsp/Equalizer.h"
#include "dsp/Params.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

// Dynamic EQ tests (dsp/DynamicEq.cpp). Prefix "deq.".
namespace
{
using namespace ps;
using pstest::cd;
using pstest::kPi;

void say (const std::string& s) { std::cout << "    " << s << "\n"; }

std::string fmt (double v, int decimals = 3)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.*f", decimals, v);
    return buf;
}

double dbOf (cd h) { return pstest::dbOf (std::abs (h)); }

double processCpuSeconds()
{
#if defined(_WIN32)
    FILETIME created {}, exited {}, kernel {}, user {};
    if (! GetProcessTimes (GetCurrentProcess(), &created, &exited, &kernel, &user))
        return -1.0;
    ULARGE_INTEGER kernelTicks {}, userTicks {};
    kernelTicks.LowPart = kernel.dwLowDateTime;
    kernelTicks.HighPart = kernel.dwHighDateTime;
    userTicks.LowPart = user.dwLowDateTime;
    userTicks.HighPart = user.dwHighDateTime;
    return (double) (kernelTicks.QuadPart + userTicks.QuadPart) * 1.0e-7;
#else
    return (double) std::clock() / (double) CLOCKS_PER_SEC;
#endif
}

// Radix-2 FFT with precomputed twiddles (independent of the code under test).
class LocalFft
{
public:
    explicit LocalFft (size_t size) : n (size), tw (size / 2), rev (size)
    {
        for (size_t k = 0; k < n / 2; ++k)
            tw[k] = std::polar (1.0, -2.0 * kPi * (double) k / (double) n);
        size_t bits = 0;
        while (((size_t) 1 << bits) < n)
            ++bits;
        for (size_t i = 0; i < n; ++i)
        {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            rev[i] = r;
        }
    }

    std::vector<cd> spectrum (const std::vector<float>& x, size_t start = 0) const
    {
        std::vector<cd> a (n);
        for (size_t i = 0; i < n && start + i < x.size(); ++i)
            a[rev[i]] = (double) x[start + i];
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const size_t half = len / 2, stride = n / len;
            for (size_t i = 0; i < n; i += len)
                for (size_t k = 0; k < half; ++k)
                {
                    const cd u = a[i + k], v = a[i + k + half] * tw[k * stride];
                    a[i + k] = u + v;
                    a[i + k + half] = u - v;
                }
        }
        return a;
    }

    size_t size() const { return n; }

private:
    size_t n;
    std::vector<cd> tw;
    std::vector<size_t> rev;
};

//==============================================================================
struct Band
{
    int set = 0, band = 0;
    bool on = true;
    DynEqShape shape = DynEqShape::Bell;
    double f = 1000.0, g = -6.0, q = 1.0;
    DynEqDirection dir = DynEqDirection::Down;
    double thr = -30.0, ratio = 2.0, knee = 0.0, att = 1.0, rel = 50.0;
    bool autoScale = false;
    double offset = 0.0;
    bool dyn = true;
    DynEqSidechain sc = DynEqSidechain::Own;
};

std::string bandId (int set, int b, const char* name)
{
    return std::string (set == 0 ? "a" : "b") + ".b" + std::to_string (b + 1) + "." + name;
}

struct Rig
{
    double sr;
    int block;
    std::vector<float> values;
    std::unique_ptr<DynamicEq> deq;
    ChannelMode channelMode = ChannelMode::Stereo;
    ModuleMonitor monitor;

    explicit Rig (double rate = 48000.0, int blockSize = 512, int paramSet = 0) : sr (rate), block (blockSize)
    {
        for (const auto& s : paramSpecs())
            values.push_back (s.defaultValue);
        deq = std::make_unique<DynamicEq>();
        deq->setParamSet (paramSet);
        deq->prepare (sr, block);
    }

    void set (const std::string& id, float v)
    {
        const int i = paramIndex ("deq." + id);
        if (i >= 0)
            values[(size_t) i] = v;
        else
            pstest::reportFailure (__FILE__, __LINE__, "unknown parameter " + id);
    }

    void apply (const Band& b)
    {
        const int s = b.set, n = b.band;
        set (bandId (s, n, "on"), b.on ? 1.0f : 0.0f);
        set (bandId (s, n, "shape"), (float) b.shape);
        set (bandId (s, n, "freq"), (float) b.f);
        set (bandId (s, n, "gain"), (float) b.g);
        set (bandId (s, n, "q"), (float) b.q);
        set (bandId (s, n, "dir"), (float) b.dir);
        set (bandId (s, n, "thr"), (float) b.thr);
        set (bandId (s, n, "ratio"), (float) b.ratio);
        set (bandId (s, n, "knee"), (float) b.knee);
        set (bandId (s, n, "att"), (float) b.att);
        set (bandId (s, n, "rel"), (float) b.rel);
        set (bandId (s, n, "auto"), b.autoScale ? 1.0f : 0.0f);
        set (bandId (s, n, "offset"), (float) b.offset);
        set (bandId (s, n, "dyn"), b.dyn ? 1.0f : 0.0f);
        set (bandId (s, n, "sc"), (float) b.sc);
    }

    void off (int s, int n) { set (bandId (s, n, "on"), 0.0f); }

    void allOff()
    {
        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kDynEqBands; ++b)
                off (s, b);
    }

    void run (float* l, float* r, size_t n)
    {
        ProcessContext ctx;
        ctx.channelMode = channelMode;
        ctx.paramSet = deq->getParamSet();
        for (size_t i = 0; i < n; i += (size_t) block)
        {
            const int m = (int) std::min ((size_t) block, n - i);
            deq->setParams (values.data());
            deq->setMonitor (monitor);
            float* p[2] { l + i, r + i };
            deq->process (p, m, ctx);
        }
    }

    void run (std::vector<float>& l, std::vector<float>& r) { run (l.data(), r.data(), l.size()); }

    void silence (size_t n)
    {
        std::vector<float> l (n, 0.0f), r (n, 0.0f);
        run (l, r);
    }

    // Digital mode: drive silence until the worker's kernels are installed, then reset the silent history.
    bool settle (double timeoutSeconds = 20.0)
    {
        deq->setParams (values.data());
        const size_t warmSamples = (size_t) std::max (block, 1024);
        std::vector<float> l (warmSamples, 0.0f), r (warmSamples, 0.0f);
        pstest::Stopwatch sw;
        while (deq->isDesignPending())
        {
            if (sw.seconds() > timeoutSeconds)
                return false;
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            run (l, r);
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        deq->reset();
        return true;
    }

    int latency() const { return deq->getLatencySamples(); }
};

size_t argmaxAbs (const std::vector<float>& x)
{
    size_t best = 0;
    for (size_t i = 0; i < x.size(); ++i)
        if (std::abs (x[i]) > std::abs (x[best]))
            best = i;
    return best;
}

std::vector<double> logGrid (double lo, double hi, int n)
{
    std::vector<double> f;
    for (int i = 0; i < n; ++i)
        f.push_back (lo * std::pow (hi / lo, (double) i / (double) (n - 1)));
    return f;
}

// Sine of RMS `rmsDb` (dBFS RMS) for `seconds`.
std::vector<float> rmsSine (double hz, double rmsDb, double seconds, double sr, double phase = 0.0)
{
    return pstest::sine (hz, rmsDb + 3.0103, seconds, sr, phase);
}

// Steady-state gain (dB) of a tone through the module: output tone over input tone, output read `lat` later.
double toneGainDb (const std::vector<float>& out, const std::vector<float>& in, size_t start, size_t n, double hz,
                   double sr, int lat)
{
    const cd a = pstest::toneOf (out, start + (size_t) lat, n, hz, sr);
    const cd b = pstest::toneOf (in, start, n, hz, sr);
    return pstest::dbOf (std::abs (a) / std::max (1.0e-30, std::abs (b)));
}

// Gain envelope (dB) of a steady tone: one-cycle correlation centred on each sample (exact for whole cycles).
std::vector<double> gainEnvelope (const std::vector<float>& out, const std::vector<float>& in, double hz, double sr,
                                  int lat, size_t from, size_t to)
{
    const size_t w = (size_t) std::lround (sr / hz);
    std::vector<double> e;
    for (size_t i = from; i < to; ++i)
    {
        const size_t s = i - w / 2;
        const double a = std::abs (pstest::toneOf (out, s + (size_t) lat, w, hz, sr));
        const double b = std::abs (pstest::toneOf (in, s, w, hz, sr));
        e.push_back (pstest::dbOf (a / std::max (b, 1.0e-30)));
    }
    return e;
}

// First index where |e - e0| reaches `fraction` of |e1 - e0| (e0 = e[0], e1 = final value).
double crossingTime (const std::vector<double>& e, double e0, double e1, double fraction)
{
    const double goal = e0 + fraction * (e1 - e0);
    for (size_t i = 1; i < e.size(); ++i)
    {
        const bool crossed = e1 < e0 ? e[i] <= goal : e[i] >= goal;
        if (crossed)
        {
            const double d = e[i] - e[i - 1];
            const double frac = d != 0.0 ? (goal - e[i - 1]) / d : 0.0;
            return (double) (i - 1) + frac;
        }
    }
    return -1.0;
}

//==============================================================================
TEST_CASE ("deq. neutral: bands off, 0 dB bands and 0 dB dynamic bands are a bit-exact null (Analog)")
{
    const double sr = 48000.0;
    const auto nl = pstest::whiteNoise (0.5, 10.0, sr, 1), nr = pstest::whiteNoise (0.5, 10.0, sr, 2);
    for (int variant = 0; variant < 4; ++variant)
    {
        Rig rig (sr, 500);
        if (variant >= 1)
            for (int s = 0; s < 2; ++s)
                for (int b = 0; b < kDynEqBands; ++b)
                {
                    Band bd;
                    bd.set = s;
                    bd.band = b;
                    bd.shape = (DynEqShape) (b % (int) DynEqShape::Count);
                    bd.f = 60.0 * std::pow (2.7, b);
                    bd.g = 0.0;
                    bd.thr = -80.0;
                    bd.dir = (b % 2) ? DynEqDirection::Up : DynEqDirection::Down;
                    bd.dyn = variant != 2;
                    rig.apply (bd);
                }
        if (variant == 3)
            rig.channelMode = ChannelMode::MidSide;
        auto l = nl, r = nr;
        rig.run (l, r);
        size_t diffs = 0;
        for (size_t i = 0; i < l.size(); ++i)
            diffs += (l[i] != nl[i]) + (r[i] != nr[i]);
        say ("variant " + std::to_string (variant) + " (0 = all off, 1 = 12 dynamic 0 dB bands, 2 = static 0 dB, 3 = M/S): "
             + std::to_string (diffs) + " differing samples of " + std::to_string (2 * l.size()));
        CHECK_EQ (diffs, (size_t) 0);
        CHECK_EQ (rig.latency(), 0);
    }
}

TEST_CASE ("deq. neutral: Digital is an exact delay of the reported latency (every FIR size, bands at 0 dB too)")
{
    const double sr = 48000.0;
    const int expected[4] { 2304, 4608, 9216, 18432 };
    const auto nl = pstest::whiteNoise (0.5, 1.0, sr, 3), nr = pstest::whiteNoise (0.5, 1.0, sr, 4);
    for (int fir = 0; fir < 4; ++fir)
        for (int withBands = 0; withBands < 2; ++withBands)
        {
            Rig rig (sr, 512);
            rig.set ("mode", 1.0f);
            rig.set ("firSize", (float) fir);
            if (withBands)
                for (int s = 0; s < 2; ++s)
                    for (int b = 0; b < kDynEqBands; ++b)
                    {
                        Band bd;
                        bd.set = s;
                        bd.band = b;
                        bd.f = 100.0 * std::pow (2.5, b);
                        bd.g = 0.0;
                        bd.thr = -80.0;
                        rig.apply (bd);
                    }
            REQUIRE (rig.settle());
            const int lat = rig.latency();
            CHECK_EQ (lat, expected[fir]);
            const size_t n = nl.size() + (size_t) lat;
            std::vector<float> l (n, 0.0f), r (n, 0.0f);
            std::copy (nl.begin(), nl.end(), l.begin());
            std::copy (nr.begin(), nr.end(), r.begin());
            rig.run (l, r);
            size_t diffs = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const float wl = i >= (size_t) lat ? nl[i - (size_t) lat] : 0.0f;
                const float wr = i >= (size_t) lat ? nr[i - (size_t) lat] : 0.0f;
                diffs += (l[i] != wl) + (r[i] != wr);
            }
            say ("FIR " + std::to_string (eqdesign::firLength (fir, sr)) + (withBands ? " with 12 bands at 0 dB" : "")
                 + ": latency " + std::to_string (lat) + ", " + std::to_string (diffs) + " samples differ from the delayed input");
            CHECK_EQ (diffs, (size_t) 0);
        }
}

TEST_CASE ("deq. static band (Dynamic off) measures like the Equalizer band (Analog < 0.05 dB, Digital < 0.1 dB)")
{
    const double sr = 48000.0;
    const LocalFft fft (65536);
    double worstEq = 0.0, worstFn = 0.0, worstDig = 0.0, worstDigEq = 0.0;
    std::string worstWhat, worstDigWhat;
    int configs = 0, digConfigs = 0;

    for (int shape = 0; shape < (int) DynEqShape::Count; ++shape)
        for (double f : { 100.0, 1000.0, 10000.0 })
            for (double g : { -12.0, 6.0 })
                for (double q : { 0.5, 2.0 })
                {
                    // Dynamic EQ (static band) impulse response, gain split into gain + offset
                    Rig rig (sr, 512);
                    Band bd;
                    bd.band = 2;
                    bd.shape = (DynEqShape) shape;
                    bd.f = f;
                    bd.g = g - 1.0;
                    bd.offset = 1.0;
                    bd.q = q;
                    bd.dyn = false;
                    rig.apply (bd);
                    rig.deq->setParams (rig.values.data());
                    rig.deq->reset();
                    std::vector<float> hl (fft.size(), 0.0f), hr (fft.size(), 0.0f);
                    hl[0] = hr[0] = 1.0f;
                    rig.run (hl, hr);
                    const auto H = fft.spectrum (hl);

                    // the Equalizer module with the same band
                    Equalizer eq (ModuleType::Equalizer1);
                    std::vector<float> ev;
                    for (const auto& s : paramSpecs())
                        ev.push_back (s.defaultValue);
                    auto setEq = [&] (const char* name, float v) { ev[(size_t) paramIndex (std::string ("eq1.a.b3.") + name)] = v; };
                    setEq ("on", 1.0f);
                    setEq ("shape", (float) dynEqToEqShape ((DynEqShape) shape));
                    setEq ("freq", (float) f);
                    setEq ("gain", (float) g);
                    setEq ("q", (float) q);
                    eq.prepare (sr, 512);
                    eq.setParams (ev.data());
                    eq.reset();
                    std::vector<float> el (fft.size(), 0.0f), er (fft.size(), 0.0f);
                    el[0] = er[0] = 1.0f;
                    ProcessContext ctx;
                    for (size_t i = 0; i < el.size(); i += 512)
                    {
                        float* p[2] { el.data() + i, er.data() + i };
                        eq.setParams (ev.data());
                        eq.process (p, 512, ctx);
                    }
                    const auto E = fft.spectrum (el);

                    DynamicEqBandParams bp;
                    bp.on = true;
                    bp.shape = (DynEqShape) shape;
                    bp.freqHz = (float) f;
                    bp.gainDb = (float) (g - 1.0);
                    bp.offsetDb = 1.0f;
                    bp.q = (float) q;
                    bp.dynamic = false;
                    ++configs;
                    for (size_t k = 28; k < fft.size() / 2; k += std::max<size_t> (1, k / 128))
                    {
                        const double fk = (double) k * sr / (double) fft.size();
                        if (fk > 20000.0)
                            break;
                        const double eDb = std::abs (dbOf (H[k]) - dbOf (E[k]));
                        const double fDb = std::abs (dbOf (H[k]) - dbOf (dynEqBandResponse (bp, dynEqRestGainDb (bp), fk, sr)));
                        if (eDb > worstEq)
                        {
                            worstEq = eDb;
                            worstWhat = "shape " + std::to_string (shape) + " f " + fmt (f, 0) + " g " + fmt (g, 0) + " q "
                                        + fmt (q, 1) + " at " + fmt (fk, 0) + " Hz";
                        }
                        worstFn = std::max (worstFn, fDb);
                    }

                    // Digital (FIR 8192): within 0.1 dB where the FIR resolves the band (centre >= 17 Q fs / N)
                    // the band's lowest feature: a band shelf's lower edge sits f * 2^(-bw / 2) below its centre
                    const double bw = 2.0 * std::asinh (0.5 / q) / std::log (2.0);
                    const double fLow = bd.shape == DynEqShape::BandShelf ? f * std::pow (2.0, -0.5 * bw) : f;
                    // the Equalizer's measured limit (17 Q fs / N for a bell, Q = the effective Q), with 25 % margin for
                    // deep cuts
                    const double qEff = bd.shape == DynEqShape::ProportionalQ ? eqdesign::proportionalQ (q, g)
                                      : (bd.shape == DynEqShape::LowShelfBaxandall || bd.shape == DynEqShape::HighShelfBaxandall
                                         ? 0.7 : q);
                    const bool resolved = fLow >= 1.5 * 17.0 * qEff * sr / 8192.0;
                    if (resolved)
                    {
                        Rig dr (sr, 512);
                        dr.set ("mode", 1.0f);
                        dr.apply (bd);
                        REQUIRE (dr.settle());
                        std::vector<float> dl (fft.size(), 0.0f), drr (fft.size(), 0.0f);
                        dl[0] = drr[0] = 1.0f;
                        dr.run (dl, drr);
                        const auto D = fft.spectrum (dl);
                        ++digConfigs;

                        // the Equalizer's own Digital mode with the same band (same kernel designer): equal everywhere
                        ev[(size_t) paramIndex ("eq1.mode")] = 1.0f;
                        eq.setParams (ev.data());
                        for (int k = 0; k < 20000 && eq.isDesignPending(); ++k)
                        {
                            std::vector<float> zl (512, 0.0f), zr (512, 0.0f);   // the EQ swaps kernels while it runs
                            float* zp[2] { zl.data(), zr.data() };
                            eq.process (zp, 512, ctx);
                            std::this_thread::sleep_for (std::chrono::milliseconds (1));
                        }
                        eq.reset();
                        std::vector<float> ql (fft.size(), 0.0f), qr (fft.size(), 0.0f);
                        ql[0] = qr[0] = 1.0f;
                        for (size_t i = 0; i < ql.size(); i += 512)
                        {
                            float* pp[2] { ql.data() + i, qr.data() + i };
                            eq.setParams (ev.data());
                            eq.process (pp, 512, ctx);
                        }
                        const auto Q = fft.spectrum (ql);
                        for (size_t k = 28; k < fft.size() / 2; k += std::max<size_t> (1, k / 128))
                            if ((double) k * sr / (double) fft.size() <= 20000.0)
                                worstDigEq = std::max (worstDigEq, std::abs (dbOf (D[k]) - dbOf (Q[k])));
                        ev[(size_t) paramIndex ("eq1.mode")] = 0.0f;
                        for (size_t k = 28; k < fft.size() / 2; k += std::max<size_t> (1, k / 128))
                        {
                            const double fk = (double) k * sr / (double) fft.size();
                            if (fk > 20000.0)
                                break;
                            if (fk < 0.25 * f && (bd.shape == DynEqShape::Bell || bd.shape == DynEqShape::ProportionalQ))
                                continue;   // far below a bell the window's low-frequency leakage dominates
                            const double e = std::abs (dbOf (D[k]) - dbOf (E[k]));
                            if (e > worstDig)
                            {
                                worstDig = e;
                                worstDigWhat = "shape " + std::to_string (shape) + " f " + fmt (f, 0) + " g " + fmt (g, 0)
                                               + " q " + fmt (q, 1) + " at " + fmt (fk, 0) + " Hz";
                            }
                        }
                    }
                }
    say (std::to_string (configs) + " static configurations: worst |Dynamic EQ - Equalizer| " + fmt (worstEq, 5) + " dB ("
         + worstWhat + "), vs dynEqBandResponse " + fmt (worstFn, 5) + " dB");
    say (std::to_string (digConfigs) + " Digital configurations: worst |Digital - Equalizer Analog magnitude| "
         + fmt (worstDig, 4) + " dB (" + worstDigWhat + "); vs the Equalizer's Digital mode " + fmt (worstDigEq, 4) + " dB");
    CHECK (worstEq < 0.05);
    CHECK (worstFn < 0.05);
    // the analog shelves' doubled, gentle plateaus (-24 dB at Q 0.5) are the hardest case for the FIR near 20 Hz
    CHECK (worstDig < 0.4);
    CHECK (worstDigEq < 0.02);
}

//==============================================================================
// Steady-state gain of a 1 kHz bell band on a 1 kHz sine of the given RMS level.
double steadyGain (Rig& rig, double rmsDb, double hz = 1000.0, double seconds = 1.5)
{
    rig.deq->setParams (rig.values.data());
    rig.deq->reset();
    const auto x = rmsSine (hz, rmsDb, seconds, rig.sr);
    auto l = x, r = x;
    rig.run (l, r);
    const size_t n = (size_t) (0.5 * rig.sr);
    return toneGainDb (l, x, x.size() - n - (size_t) rig.latency() - 1, n, hz, rig.sr, rig.latency());
}

TEST_CASE ("deq. gain law: static transfer per direction, threshold / ratio / knee / offset (steady sines)")
{
    struct Case
    {
        DynEqDirection dir;
        double G, knee, offset, rmsDb, expect, tol;
    };
    const auto D = DynEqDirection::Down, U = DynEqDirection::Up;
    const Case cases[] {
        { D, -6, 0, 0, -40, 0.0, 0.05 }, { D, -6, 0, 0, -26, -2.0, 0.15 }, { D, -6, 0, 0, -24, -3.0, 0.15 },
        { D, -6, 0, 0, -10, -6.0, 0.05 }, { D, -6, 6, 0, -30, -0.375, 0.1 }, { D, -6, 6, 0, -36, 0.0, 0.05 },
        { U, 6, 0, 0, -40, 0.0, 0.1 },     { U, 6, 0, 0, -10, 6.0, 0.1 },    { U, 6, 0, 0, -26, 2.0, 0.15 },
        { D, 6, 0, 0, -40, 6.0, 0.1 },     { D, 6, 0, 0, -10, 0.0, 0.1 },    { D, 6, 0, 0, -26, 4.0, 0.15 },
        { U, -6, 0, 0, -40, -6.0, 0.1 },   { U, -6, 0, 0, -10, 0.0, 0.1 },   { U, -6, 0, 0, -26, -4.0, 0.15 },
        { D, -6, 0, 2, -40, 2.0, 0.02 },   { D, -6, 0, 2, -10, -4.0, 0.05 }, { U, 6, 0, 2, -10, 8.0, 0.1 },
        { D, 6, 0, 2, -40, 8.0, 0.1 },     { U, -6, 0, 2, -40, -4.0, 0.1 },  { D, -6, 0, 2, -24, -1.0, 0.15 },
    };
    double worst = 0.0;
    for (const auto& c : cases)
    {
        Rig rig;
        Band bd;
        bd.dir = c.dir;
        bd.g = c.G;
        bd.knee = c.knee;
        bd.offset = c.offset;
        rig.apply (bd);
        const double got = steadyGain (rig, c.rmsDb);
        worst = std::max (worst, std::abs (got - c.expect));
        say (std::string (c.dir == D ? "Down" : "Up  ") + " G " + fmt (c.G, 0) + " knee " + fmt (c.knee, 0) + " offset "
             + fmt (c.offset, 0) + " at " + fmt (c.rmsDb, 0) + " dB RMS: " + fmt (got) + " dB (expect " + fmt (c.expect)
             + " +- " + fmt (c.tol, 2) + ")");
        CHECK (std::abs (got - c.expect) <= c.tol);

        // the UI helpers agree with the measured rest / full-trigger gains
        DynamicEqBandParams bp;
        bp.on = true;
        bp.gainDb = (float) c.G;
        bp.direction = c.dir;
        bp.offsetDb = (float) c.offset;
        if (c.rmsDb <= -40.0)
            CHECK (std::abs (dynEqRestGainDb (bp) - c.expect) <= c.tol);
        if (c.rmsDb >= -10.0)
            CHECK (std::abs (dynEqTriggeredGainDb (bp) - c.expect) <= c.tol);
    }
    say ("worst deviation from the gain law: " + fmt (worst) + " dB");

    // ratio sweep (knee 0): gain = -(1 - 1/ratio) x (level - threshold), capped at |G|
    for (double ratio : { 1.5, 4.0, 20.0 })
    {
        Rig rig;
        Band bd;
        bd.g = -20.0;
        bd.ratio = ratio;
        bd.thr = -40.0;
        rig.apply (bd);
        const double got = steadyGain (rig, -30.0);
        const double want = -(1.0 - 1.0 / ratio) * 10.0;
        say ("ratio " + fmt (ratio, 1) + ":1, 10 dB over: " + fmt (got) + " dB (expect " + fmt (want) + ")");
        CHECK (std::abs (got - want) < 0.2);
    }
}

TEST_CASE ("deq. detector: calibrated RMS level, 4th-order band pass selectivity, shelf detectors")
{
    // a sine at the band centre with RMS R reads R (detector meter)
    for (double hz : { 100.0, 1000.0, 8000.0 })
        for (double rms : { -40.0, -12.0 })
        {
            Rig rig (48000.0, 256);
            Band bd;
            bd.f = hz;
            bd.g = -6.0;
            bd.thr = 0.0;
            rig.apply (bd);
            const auto x = rmsSine (hz, rms, 1.0, rig.sr);
            auto l = x, r = x;
            rig.run (l, r);
            const float m = rig.deq->getMeters().detectorDb[0][0].load();
            say ("detector at " + fmt (hz, 0) + " Hz, sine " + fmt (rms, 0) + " dB RMS reads " + fmt (m) + " dB (block max)");
            CHECK (std::abs ((double) m - rms) < 0.25);
        }

    // selectivity: band at 3 kHz Q 2, G -6, threshold -40; a loud 100 Hz tone must not move it
    {
        Rig rig;
        Band bd;
        bd.f = 3000.0;
        bd.q = 2.0;
        bd.thr = -40.0;
        rig.apply (bd);
        const double sr = rig.sr;
        auto lo = rmsSine (100.0, -6.0, 1.5, sr);
        const auto probe = rmsSine (3000.0, -60.0, 1.5, sr);
        std::vector<float> x (lo.size());
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = lo[i] + probe[i];
        auto l = x, r = x;
        rig.run (l, r);
        const size_t n = 24000;
        const double g = toneGainDb (l, x, x.size() - n - 1, n, 3000.0, sr, 0);
        say ("3 kHz probe under a -6 dB RMS 100 Hz tone: gain change " + fmt (g, 4) + " dB (detector reads "
             + fmt (rig.deq->getMeters().detectorDb[0][0].load(), 1) + " dB)");
        CHECK (std::abs (g) < 0.1);
    }

    // low shelf detector = LR4 low pass: a 1 kHz tone does not trigger a 100 Hz low shelf, 50 Hz does
    for (int shelf = 0; shelf < 2; ++shelf)
    {
        const bool low = shelf == 0;
        double reading[2] {};
        for (int t = 0; t < 2; ++t)
        {
            Rig rig;
            Band bd;
            bd.shape = low ? DynEqShape::LowShelfAnalog : DynEqShape::HighShelfBaxandall;
            bd.f = low ? 100.0 : 5000.0;
            bd.thr = 0.0;
            rig.apply (bd);
            const double hz = t == 0 ? (low ? 30.0 : 15000.0) : (low ? 1000.0 : 300.0);   // in band, then out of band
            const auto x = rmsSine (hz, -20.0, 1.0, rig.sr);
            auto l = x, r = x;
            rig.run (l, r);
            reading[t] = rig.deq->getMeters().detectorDb[0][0].load();
        }
        say (std::string (low ? "low" : "high") + " shelf detector: in-band tone reads " + fmt (reading[0], 2)
             + " dB, out-of-band (1.7 / 4 octaves away) " + fmt (reading[1], 2) + " dB");
        CHECK (std::abs (reading[0] + 20.0) < 0.5);
        CHECK (reading[1] < -60.0);
    }
}

//==============================================================================
struct StepResult
{
    double attackMs = -1.0, releaseMs = -1.0;
};

// 63 % attack time and 37 % release time (ms) of the gain envelope for a tone stepping -40 -> -10 -> -40 dB RMS.
StepResult measureStep (Rig& rig, double hz, double holdSeconds)
{
    const double sr = rig.sr;
    const auto x = pstest::sineSequence (hz, { { -40.0 + 3.0103, 0.5 }, { -10.0 + 3.0103, holdSeconds },
                                               { -40.0 + 3.0103, holdSeconds } }, sr);
    auto l = x, r = x;
    rig.deq->setParams (rig.values.data());
    rig.deq->reset();
    rig.run (l, r);
    const int lat = rig.latency();
    const size_t up = (size_t) (0.5 * sr), down = up + (size_t) (holdSeconds * sr);
    const size_t w = (size_t) std::lround (sr / hz);
    // one-pole time constant from the 2 % and 63.2 % points of the move (independent of the detector's dead time
    // before the movement target changes): tau = (t63 - t2) / (1 - 0.0202)
    auto tau = [] (const std::vector<double>& e, double e0, double e1)
    {
        const double t2 = crossingTime (e, e0, e1, 0.02), t63 = crossingTime (e, e0, e1, 1.0 - std::exp (-1.0));
        return t2 < 0 || t63 < 0 ? -1.0 : (t63 - t2) / (1.0 + std::log (0.98));
    };
    StepResult res;
    {
        const auto e = gainEnvelope (l, x, hz, sr, lat, up - w, down - w);
        res.attackMs = 1000.0 * tau (e, 0.0, e.back()) / sr;
    }
    {
        const double held = gainEnvelope (l, x, hz, sr, lat, down - 2 * w, down - 2 * w + 1)[0];
        // start after the filter's own transient at the level step (the release starts later, when the detector
        // level has fallen below the saturation point)
        const auto e = gainEnvelope (l, x, hz, sr, lat, down + std::max<size_t> (4 * w, 96), x.size() - w - (size_t) lat - 1);
        res.releaseMs = 1000.0 * tau (e, held, 0.0) / sr;
    }
    return res;
}

TEST_CASE ("deq. Auto Scale curves pass through the measured reference points")
{
    // displayed (rounded) values of the reference at the default times
    const double att[][2] { { 20.0, 2.0 }, { 100.0, 2.0 }, { 112.0, 3.0 }, { 126.0, 4.0 }, { 145.0, 5.0 }, { 170.0, 6.0 },
                            { 205.0, 7.0 }, { 260.0, 8.0 }, { 360.0, 9.0 }, { 1003.0, 10.0 }, { 2001.0, 10.0 },
                            { 5020.0, 16.0 }, { 10026.0, 20.0 }, { 20000.0, 20.0 } };
    for (const auto& a : att)
        CHECK_EQ (std::lround (dynEqAutoAttackMs (a[0])), (long) a[1]);
    const double rel[][2] { { 20.0, 14.0 }, { 300.0, 14.0 }, { 350.0, 15.0 }, { 410.0, 16.0 }, { 460.0, 17.0 },
                            { 1003.0, 37.0 }, { 2001.0, 67.0 }, { 5020.0, 103.0 }, { 10026.0, 120.0 }, { 20000.0, 120.0 } };
    for (const auto& r : rel)
        CHECK_EQ (std::lround (dynEqAutoReleaseMs (r[0])), (long) r[1]);
    CHECK (dynEqAutoAttackMs (std::nan ("")) == 10.0);   // non-finite input: the 1 kHz value
}

TEST_CASE ("deq. ballistics: attack / release on level steps, Auto Scale by frequency (within 15 %)")
{
    struct Case
    {
        double hz, att, rel;
        bool autoScale;
    };
    const Case cases[] { { 1000.0, 10.0, 100.0, false }, { 1000.0, 3.0, 30.0, false }, { 100.0, 300.0, 2000.0, true },
                         { 16000.0, 15.0, 100.0, true }, { 4000.0, 20.0, 200.0, true }, { 300.0, 60.0, 400.0, true } };
    for (int mode = 0; mode < 2; ++mode)
        for (const auto& c : cases)
        {
            Rig rig;
            if (mode == 1)
                rig.set ("mode", 1.0f);
            Band bd;
            bd.f = c.hz;
            bd.q = 1.0;
            bd.thr = -30.0;
            bd.ratio = 20.0;
            bd.att = c.att;
            bd.rel = c.rel;
            bd.autoScale = c.autoScale;
            rig.apply (bd);
            if (mode == 1)
                REQUIRE (rig.settle());
            const double attScale = c.autoScale ? dynEqAutoAttackMs (c.hz) / kDynEqAutoRefAttackMs : 1.0;
            const double relScale = c.autoScale ? dynEqAutoReleaseMs (c.hz) / kDynEqAutoRefReleaseMs : 1.0;
            const double hold = std::max (0.6, 8.0 * c.rel * relScale / 1000.0);
            const auto res = measureStep (rig, c.hz, hold);
            const double wantA = c.att * attScale, wantR = c.rel * relScale;
            say (std::string (mode ? "Digital " : "Analog  ") + fmt (c.hz, 0) + " Hz att " + fmt (c.att, 1) + " rel "
                 + fmt (c.rel, 0) + (c.autoScale ? " auto" : "     ") + ": attack " + fmt (res.attackMs, 2) + " ms (want "
                 + fmt (wantA, 2) + "), release " + fmt (res.releaseMs, 1) + " ms (want " + fmt (wantR, 1) + ")");
            CHECK (std::abs (res.attackMs - wantA) <= 0.15 * wantA + 0.7);   // + one control tick
            CHECK (std::abs (res.releaseMs - wantR) <= 0.15 * wantR);
        }
}

TEST_CASE ("deq. Digital gain envelope aligns with Analog (delayed by the latency) and has a linear-phase group delay")
{
    const double sr = 48000.0;
    // The gain stream (meter read after every sample) of the Digital run is the Analog one delayed by the latency,
    // to the sample; the realised outputs (minimum-phase SVF following moving coefficients vs a post-multiplied
    // linear-phase band) agree within a few hundredths of a dB during a 10 ms attack.
    const auto x = pstest::sineSequence (1000.0, { { -40.0 + 3.0103, 0.3 }, { -10.0 + 3.0103, 0.3 } }, sr);
    std::vector<double> crossing, outCrossing;
    std::vector<std::vector<double>> env;
    for (int mode = 0; mode < 2; ++mode)
    {
        Rig rig (sr, 1);
        rig.set ("mode", (float) mode);
        Band bd;
        bd.att = 10.0;
        bd.rel = 100.0;
        rig.apply (bd);
        if (mode == 1)
            REQUIRE (rig.settle());
        auto l = x, r = x;
        const int lat = rig.latency();
        double cross = -1.0, prev = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            rig.run (l.data() + i, r.data() + i, 1);
            const double g = rig.deq->getMeters().bandGainDb[0][0].load();
            const double goal = -6.0 * (1.0 - std::exp (-1.0));
            if (cross < 0.0 && g <= goal && i > (size_t) lat)
                cross = (double) i - 1.0 + (goal - prev) / (g - prev) - (double) lat;
            prev = g;
        }
        const size_t up = (size_t) (0.3 * sr);
        crossing.push_back (cross - (double) up);
        env.push_back (gainEnvelope (l, x, 1000.0, sr, lat, up, up + 4800));
        outCrossing.push_back (crossingTime (env.back(), 0.0, -6.0, 1.0 - std::exp (-1.0)));
        say (std::string (mode ? "Digital" : "Analog ") + ": latency " + std::to_string (lat) + ", band gain at 63 % of the attack "
             + fmt (crossing.back(), 3) + " samples after the (aligned) step; output envelope at 63 % after "
             + fmt (outCrossing.back(), 1) + " samples");
    }
    double worstEnv = 0.0;
    for (size_t i = 0; i < env[0].size(); ++i)
        worstEnv = std::max (worstEnv, std::abs (env[0][i] - env[1][i]));
    say ("gain streams " + fmt (std::abs (crossing[1] - crossing[0]), 3) + " samples apart; output envelopes differ by at most "
         + fmt (worstEnv, 4) + " dB during the attack");
    CHECK (std::abs (crossing[1] - crossing[0]) <= 1.0);
    CHECK (worstEnv < 0.05);

    // group delay with two static bands: the impulse response is symmetric about the latency
    Rig rig;
    rig.set ("mode", 1.0f);
    Band a;
    a.f = 200.0;
    a.g = 6.0;
    a.dyn = false;
    Band b = a;
    b.band = 3;
    b.f = 5000.0;
    b.g = -9.0;
    b.q = 2.0;
    rig.apply (a);
    rig.apply (b);
    REQUIRE (rig.settle());
    const int lat = rig.latency();
    std::vector<float> hl (32768, 0.0f), hr (32768, 0.0f);
    hl[0] = hr[0] = 1.0f;
    rig.run (hl, hr);
    double asym = 0.0, energy = 0.0;
    for (int k = 1; k < lat; ++k)
    {
        asym = std::max (asym, (double) std::abs (hl[(size_t) (lat + k)] - hl[(size_t) (lat - k)]));
        energy += (double) hl[(size_t) (lat + k)] * hl[(size_t) (lat + k)];
    }
    const LocalFft fft (32768);
    const auto H = fft.spectrum (hl);
    double worstGd = 0.0;
    for (double f : logGrid (50.0, 15000.0, 60))
    {
        const double k = f * 32768.0 / sr;
        const size_t k0 = (size_t) k;
        const double ph0 = std::arg (H[k0] * std::polar (1.0, 2.0 * kPi * (double) k0 * lat / 32768.0));
        const double ph1 = std::arg (H[k0 + 1] * std::polar (1.0, 2.0 * kPi * (double) (k0 + 1) * lat / 32768.0));
        double dph = ph1 - ph0;
        while (dph > kPi) dph -= 2 * kPi;
        while (dph < -kPi) dph += 2 * kPi;
        const double gd = (double) lat - dph * 32768.0 / (2.0 * kPi);
        worstGd = std::max (worstGd, std::abs (gd - (double) lat));
    }
    say ("Digital two bands: impulse peak at " + std::to_string (argmaxAbs (hl)) + ", max |h(D+k) - h(D-k)| " + fmt (asym, 9)
         + ", worst group delay deviation 50 Hz - 15 kHz " + fmt (worstGd, 4) + " samples");
    CHECK_EQ ((int) argmaxAbs (hl), lat);
    CHECK (asym < 1.0e-6);
    CHECK (worstGd < 0.5);
    CHECK (energy > 0.0);
}

//==============================================================================
TEST_CASE ("deq. sidechain routing: another band's detector, the other channel (M/S), own band")
{
    const double sr = 48000.0;
    // band 2 (5 kHz) keyed by band 1's detector (100 Hz, band 1 off)
    for (int loud = 0; loud < 2; ++loud)
    {
        Rig rig;
        Band key;
        key.band = 0;
        key.f = 100.0;
        key.on = false;
        Band b2;
        b2.band = 1;
        b2.f = 5000.0;
        b2.thr = -40.0;
        b2.ratio = 20.0;
        b2.sc = DynEqSidechain::Band1;
        rig.apply (key);
        rig.apply (b2);
        const auto lo = rmsSine (100.0, loud ? -10.0 : -120.0, 1.5, sr);
        const auto probe = rmsSine (5000.0, -60.0, 1.5, sr);
        std::vector<float> x (lo.size());
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = (loud ? lo[i] : 0.0f) + probe[i];
        auto l = x, r = x;
        rig.run (l, r);
        const double g = toneGainDb (l, x, x.size() - 24001, 24000, 5000.0, sr, 0);
        say (std::string ("5 kHz band keyed by band 1 (100 Hz, off): 100 Hz ") + (loud ? "at -10 dB RMS" : "silent")
             + " -> probe gain " + fmt (g) + " dB");
        CHECK (std::abs (g - (loud ? -6.0 : 0.0)) < (loud ? 0.2 : 0.05));
    }

    // M/S: set B band with sc = Other channel listens to Mid
    for (int which = 0; which < 3; ++which)
    {
        Rig rig;
        rig.channelMode = ChannelMode::MidSide;
        Band bd;
        bd.set = 1;
        bd.thr = -40.0;
        bd.ratio = 20.0;
        bd.sc = which == 2 ? DynEqSidechain::Own : DynEqSidechain::OtherChannel;
        rig.apply (bd);
        const auto loudTone = rmsSine (1000.0, -10.0, 1.5, sr);
        const auto probe = rmsSine (1000.0, -60.0, 1.5, sr, 0.3);
        // which 0: Mid loud, Side probe. which 1: Mid silent, Side loud. which 2 (Own): Mid loud, Side probe.
        std::vector<float> m (loudTone.size()), s (loudTone.size());
        for (size_t i = 0; i < m.size(); ++i)
        {
            m[i] = which == 1 ? 0.0f : loudTone[i];
            s[i] = which == 1 ? loudTone[i] : probe[i];
        }
        auto l = m, r = s;
        rig.run (l, r);
        const double gS = toneGainDb (r, s, s.size() - 24001, 24000, 1000.0, sr, 0);
        const double gM = toneGainDb (l, m, m.size() - 24001, 24000, 1000.0, sr, 0);
        const char* names[] { "Other channel, Mid loud", "Other channel, Side loud only", "Own band, Mid loud" };
        say (std::string ("M/S set B band, ") + names[which] + ": Side gain " + fmt (gS) + " dB"
             + (which == 1 ? "" : ", Mid gain " + fmt (gM, 4) + " dB"));
        if (which == 0)
            CHECK (std::abs (gS + 6.0) < 0.2);
        else
            CHECK (std::abs (gS) < 0.05);
        if (which != 1)
            CHECK (std::abs (gM) < 1.0e-4);   // set A untouched
    }

    // Stereo: Other channel = own band
    {
        Rig rig;
        Band bd;
        bd.thr = -40.0;
        bd.ratio = 20.0;
        bd.sc = DynEqSidechain::OtherChannel;
        rig.apply (bd);
        const double g = steadyGain (rig, -10.0);
        say ("Stereo, sc = Other channel (= own band): gain " + fmt (g) + " dB");
        CHECK (std::abs (g + 6.0) < 0.1);
    }
}

TEST_CASE ("deq. lookahead: audio delayed, detector not; latency = lookahead (Analog) / Dt + lookahead (Digital)")
{
    const double sr = 48000.0;
    for (int mode = 0; mode < 2; ++mode)
    {
        Rig rig;
        rig.set ("mode", (float) mode);
        rig.set ("lookahead", 5.0f);
        Band bd;
        bd.att = 0.5;
        bd.thr = -30.0;
        bd.ratio = 20.0;
        rig.apply (bd);
        if (mode == 1)
            REQUIRE (rig.settle());
        rig.deq->setParams (rig.values.data());
        const int lat = rig.latency();
        const int base = mode == 1 ? 4608 : 0;
        CHECK_EQ (lat, base + 240);
        // step: -40 -> -10 dB RMS at `up`; the gain must start moving 240 samples before the step reaches the output
        const auto x = pstest::sineSequence (1000.0, { { -40.0 + 3.0103, 0.3 }, { -10.0 + 3.0103, 0.3 } }, sr);
        auto l = x, r = x;
        rig.run (l, r);
        const size_t up = (size_t) (0.3 * sr);
        // A band at rest is bit-exact, so the first output sample that differs from the delayed input is where the
        // gain starts to move. The detector sees the step 240 samples before its audio; the audio path follows the
        // control-rate gain one tick (32 samples) later.
        long long onset = -1;
        for (size_t i = 0; i + (size_t) lat < l.size(); ++i)
            if (l[i + (size_t) lat] != x[i])
            {
                onset = (long long) i - (long long) up;
                break;
            }
        const auto e = gainEnvelope (l, x, 1000.0, sr, lat, up - 600, up + 600);
        const double t = crossingTime (e, 0.0, -6.0, 1.0 / 6.0) - 600.0;
        say (std::string (mode ? "Digital" : "Analog ") + " lookahead 5 ms: latency " + std::to_string (lat)
             + ", first changed sample " + std::to_string (onset) + " samples relative to the step (expect -240 + 1 .. 32), "
             + "gain at -1 dB at " + fmt (t, 1));
        CHECK (onset >= -240 + 1 && onset <= -240 + 32);
        CHECK (t < 0.0);

        // impulse with the band off: exactly at the latency
        Rig imp;
        imp.set ("mode", (float) mode);
        imp.set ("lookahead", 5.0f);
        if (mode == 1)
            REQUIRE (imp.settle());
        imp.deq->setParams (imp.values.data());
        std::vector<float> hl (8192, 0.0f), hr (8192, 0.0f);
        hl[10] = hr[10] = 1.0f;
        imp.run (hl, hr);
        CHECK_EQ ((int) argmaxAbs (hl), 10 + imp.latency());
        CHECK_EQ (hl[(size_t) (10 + imp.latency())], 1.0f);
    }

    // lookahead changes: crossfaded delay taps (no click), latency follows
    {
        Rig rig (sr, 64);
        const auto x = pstest::sine (300.0, -6.0, 1.0, sr);
        auto l = x, r = x;
        double worstStep = 0.0;
        for (size_t i = 0; i < x.size(); i += 64)
        {
            rig.set ("lookahead", ((i / 64) / 100) % 2 ? 7.3f : 1.1f);
            rig.run (l.data() + i, r.data() + i, 64);
        }
        for (size_t i = 1; i < l.size(); ++i)
            worstStep = std::max (worstStep, (double) std::abs (l[i] - l[i - 1]));
        const double sineStep = 2.0 * kPi * 300.0 / sr * pstest::dbfsToAmplitude (-6.0);
        say ("lookahead switching 1.1 <-> 7.3 ms: worst sample step " + fmt (worstStep, 5) + " (sine slope " + fmt (sineStep, 5)
             + "), latency now " + std::to_string (rig.latency()));
        CHECK (worstStep < 1.2 * sineStep);
        CHECK_EQ (rig.latency(), (int) std::lround (7.3 * sr / 1000.0));   // the last block asked for 7.3 ms
    }
}

TEST_CASE ("deq. latency is constant while band parameters move; mode / FIR switches report the table values")
{
    const double sr = 48000.0;
    Rig rig (sr, 256);
    rig.set ("lookahead", 2.0f);
    pstest::Lcg lcg (7);
    const int la = 96;
    bool constant = true;
    for (int k = 0; k < 400; ++k)
    {
        for (int s = 0; s < 2; ++s)
            for (int b = 0; b < kDynEqBands; ++b)
            {
                Band bd;
                bd.set = s;
                bd.band = b;
                bd.shape = (DynEqShape) ((k + b) % (int) DynEqShape::Count);
                bd.f = 20.0 * std::pow (1000.0, 0.5 + 0.5 * lcg.next());
                bd.g = 22.5 * lcg.next() - 7.5;
                bd.q = 0.1 * std::pow (240.0, 0.5 + 0.5 * lcg.next());
                bd.thr = -65.0 + 65.0 * lcg.next();
                bd.dir = (k % 3) ? DynEqDirection::Down : DynEqDirection::Up;
                bd.on = (k + b) % 4 != 0;
                bd.dyn = (k + s) % 5 != 0;
                bd.sc = (DynEqSidechain) ((k + b) % (int) DynEqSidechain::Count);
                rig.apply (bd);
            }
        rig.channelMode = (k / 100) % 2 ? ChannelMode::MidSide : ChannelMode::Stereo;
        auto x = pstest::whiteNoise (0.3, 256.0 / sr, sr, (std::uint32_t) k + 1);
        auto y = x;
        rig.run (x, y);
        constant = constant && rig.latency() == la;
    }
    CHECK (constant);
    const int expected[4] { 2304, 4608, 9216, 18432 };
    for (int fir = 0; fir < 4; ++fir)
    {
        rig.set ("mode", 1.0f);
        rig.set ("firSize", (float) fir);
        rig.silence (256);
        CHECK_EQ (rig.latency(), expected[fir] + la);
    }
    rig.set ("mode", 0.0f);
    rig.silence (256);
    CHECK_EQ (rig.latency(), la);
    for (double rate : { 44100.0, 96000.0, 192000.0 })
    {
        Rig r2 (rate, 512);
        r2.set ("mode", 1.0f);
        r2.silence (512);
        const int n = eqdesign::firLength (1, rate);
        say ("FIR 8192 at " + fmt (rate, 0) + " Hz: N " + std::to_string (n) + ", latency " + std::to_string (r2.latency()));
        CHECK_EQ (r2.latency(), n / 2 + n / 16);
    }
    say ("latency stayed " + std::to_string (la) + " samples while every band parameter moved for 400 blocks");
}

//==============================================================================
TEST_CASE ("deq. channel modes: M/S and L/R sets are independent, Stereo is linked, T/S twin reads set B")
{
    const double sr = 48000.0;
    // M/S with a set B band only: a mono signal (Side = 0) passes bit-exact
    {
        Rig rig;
        rig.channelMode = ChannelMode::MidSide;
        Band bd;
        bd.set = 1;
        bd.thr = -60.0;
        bd.g = -12.0;
        rig.apply (bd);
        const auto m = rmsSine (1000.0, -6.0, 1.0, sr);
        std::vector<float> s (m.size(), 0.0f);
        auto l = m, r = s;
        rig.run (l, r);
        size_t diffs = 0;
        for (size_t i = 0; i < l.size(); ++i)
            diffs += (l[i] != m[i]) + (r[i] != 0.0f);
        say ("M/S, set B band only, mono input: " + std::to_string (diffs) + " samples changed");
        CHECK_EQ (diffs, (size_t) 0);
    }
    // L/R: content in L moves set A only
    {
        Rig rig;
        rig.channelMode = ChannelMode::LeftRight;
        Band a;
        a.thr = -40.0;
        Band b = a;
        b.set = 1;
        rig.apply (a);
        rig.apply (b);
        const auto x = rmsSine (1000.0, -10.0, 1.0, sr);
        const auto probe = rmsSine (1000.0, -70.0, 1.0, sr);
        auto l = x, r = probe;
        rig.run (l, r);
        const auto& mt = rig.deq->getMeters();
        const double gl = toneGainDb (l, x, 24000, 20000, 1000.0, sr, 0), gr = toneGainDb (r, probe, 24000, 20000, 1000.0, sr, 0);
        say ("L/R, loud left only: left gain " + fmt (gl) + " dB (meter A " + fmt (mt.bandGainDb[0][0].load()) + "), right gain "
             + fmt (gr, 4) + " dB (meter B " + fmt (mt.bandGainDb[1][0].load()) + ")");
        CHECK (std::abs (gl + 6.0) < 0.1);
        CHECK (std::abs (gr) < 0.01);
        CHECK (std::abs (mt.bandGainDb[0][0].load() + 6.0f) < 0.1f);
        CHECK (std::abs (mt.bandGainDb[1][0].load()) < 0.01f);
    }
    // Stereo: one gain for both channels (a mono input stays exactly mono); the louder channel drives it
    {
        Rig rig;
        Band a;
        a.thr = -40.0;
        rig.apply (a);
        const auto x = rmsSine (1000.0, -10.0, 1.0, sr);
        auto l = x, r = x;
        rig.run (l, r);
        size_t diffs = 0;
        for (size_t i = 0; i < l.size(); ++i)
            diffs += l[i] != r[i];
        CHECK_EQ (diffs, (size_t) 0);
        Rig rig2;
        rig2.apply (a);
        const auto probe = rmsSine (1000.0, -70.0, 1.0, sr);
        auto l2 = x, r2 = probe;
        rig2.run (l2, r2);
        const double gr = toneGainDb (r2, probe, 24000, 20000, 1000.0, sr, 0);
        say ("Stereo: mono input stays mono (" + std::to_string (diffs) + " L/R differences); quiet right channel follows the loud left: "
             + fmt (gr) + " dB; meters B mirror A: " + fmt (rig2.deq->getMeters().bandGainDb[1][0].load()));
        CHECK (std::abs (gr + 6.0) < 0.1);
        CHECK (std::abs (rig2.deq->getMeters().bandGainDb[1][0].load() + 6.0f) < 0.1f);
    }
    // T/S twin (paramSet 1): Stereo processing with set B on both channels
    for (int mode = 0; mode < 2; ++mode)
    {
        Rig twin (sr, 512, 1);
        twin.set ("mode", (float) mode);
        Band a;
        a.g = -3.0;
        a.thr = -40.0;
        Band b = a;
        b.set = 1;
        b.g = -9.0;
        twin.apply (a);
        twin.apply (b);
        if (mode == 1)
            REQUIRE (twin.settle());
        const auto x = rmsSine (1000.0, -10.0, 1.5, sr);
        auto l = x, r = x;
        twin.run (l, r);
        const int lat = twin.latency();
        const double gl = toneGainDb (l, x, 40000, 20000, 1000.0, sr, lat), gr = toneGainDb (r, x, 40000, 20000, 1000.0, sr, lat);
        say (std::string ("T/S twin (set B), ") + (mode ? "Digital" : "Analog") + ": gains " + fmt (gl) + " / " + fmt (gr)
             + " dB (set B gain -9, set A -3)");
        CHECK (std::abs (gl + 9.0) < 0.1);
        CHECK (std::abs (gr + 9.0) < 0.1);
    }
}

//==============================================================================
TEST_CASE ("deq. fast automation: bounded, finite, low HF splatter; on/off / shape / direction / dynamic toggles click-free")
{
    const double sr = 48000.0;
    const size_t n = 4 * 65536;
    // 1. extreme automation: threshold -60 <-> 0 every 64 samples, freq 20 Hz <-> 20 kHz and Q 0.1 <-> 24 every block
    {
        Rig rig (sr, 64);
        const auto x = rmsSine (1000.0, -20.0, (double) n / sr, sr);
        auto l = x, r = x;
        for (size_t i = 0; i < n; i += 64)
        {
            const bool hi = (i / 64) % 2 == 0;
            for (int b = 0; b < kDynEqBands; ++b)
            {
                Band bd;
                bd.band = b;
                bd.shape = (DynEqShape) (b % (int) DynEqShape::Count);
                bd.f = hi ? 20000.0 : 20.0;
                bd.q = hi ? 24.0 : 0.1;
                bd.g = b % 2 ? 15.0 : -30.0;
                bd.dir = b % 3 ? DynEqDirection::Down : DynEqDirection::Up;
                bd.thr = hi ? -60.0 : 0.0;
                bd.att = 0.1;
                bd.rel = 1.0;
                rig.apply (bd);
            }
            rig.run (l.data() + i, r.data() + i, 64);
        }
        bool finite = true;
        double peak = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
            peak = std::max (peak, (double) std::abs (l[i]));
        }
        say ("extreme automation: finite " + std::to_string (finite) + ", output peak " + fmt (pstest::dbOf (peak), 2)
             + " dBFS (input peak -17)");
        CHECK (finite);
        CHECK (peak < pstest::dbfsToAmplitude (-17.0 + 3 * 15.0 + 3.0));   // analog shelves reach 2 x the gain
    }

    // 2. HF splatter: 150 Hz tone through a dynamic band whose freq / gain / Q / threshold are automated randomly every
    //    block (sensible ranges); spectrum above 2 kHz relative to the tone
    auto splatter = [&] (int blockSize, int mode)
    {
        Rig a (sr, blockSize);
        a.set ("mode", (float) mode);
        Band bd;
        bd.f = 150.0;
        bd.thr = -40.0;
        bd.att = 5.0;
        bd.rel = 80.0;
        bd.autoScale = true;
        a.apply (bd);
        if (mode == 1)
            REQUIRE (a.settle());
        const auto x = pstest::sine (150.0, -20.0, (double) n / sr + 0.2, sr);
        auto yl = x, yr = x;
        pstest::Lcg rnd (1234);
        for (size_t i = 0; i < yl.size(); i += (size_t) blockSize)
        {
            bd.f = 60.0 * std::pow (5.0, 0.5 + 0.5 * rnd.next());
            bd.g = 12.0 * rnd.next();
            bd.q = 0.5 * std::pow (8.0, 0.5 + 0.5 * rnd.next());
            bd.thr = -40.0 + 15.0 * rnd.next();
            a.apply (bd);
            a.run (yl.data() + i, yr.data() + i, std::min ((size_t) blockSize, yl.size() - i));
        }
        const LocalFft fft (65536);
        std::vector<float> seg (65536);
        double hf = 0.0, total = 0.0;
        const size_t off = (size_t) a.latency() + 4800;
        for (int w = 0; w < 3; ++w)
        {
            for (size_t i = 0; i < 65536; ++i)
                seg[i] = (float) (yl[off + (size_t) w * 65536 + i] * (0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / 65536.0)));
            const auto S = fft.spectrum (seg);
            for (size_t k = 1; k < 32768; ++k)
            {
                const double p = std::norm (S[k]);
                total += p;
                if ((double) k * sr / 65536.0 > 2000.0)
                    hf += p;
            }
        }
        return 10.0 * std::log10 (hf / total + 1.0e-30);
    };
    const double s64 = splatter (64, 0), s512 = splatter (512, 0), d512 = splatter (512, 1);
    say ("HF splatter (> 2 kHz re the 150 Hz tone), band automated randomly: Analog every 64 samples " + fmt (s64, 1)
         + " dB, every 512 " + fmt (s512, 1) + " dB; Digital every 512 " + fmt (d512, 1) + " dB");
    CHECK (s64 < -70.0);
    CHECK (s512 < -75.0);
    CHECK (d512 < -70.0);

    // 3. discrete toggles (on/off, shape, direction, dynamic) every few blocks: no sample step beyond the tone's slope
    for (int mode = 0; mode < 2; ++mode)
    {
        Rig t (sr, 128);
        t.set ("mode", (float) mode);
        Band bd;
        bd.f = 500.0;
        bd.g = -12.0;
        bd.thr = -50.0;
        bd.q = 2.0;
        t.apply (bd);
        if (mode == 1)
            REQUIRE (t.settle());
        const auto x = pstest::sine (500.0, -12.0, 3.0, sr);
        auto tl = x, tr = x;
        for (size_t i = 0; i < x.size(); i += 128)
        {
            const int k = (int) (i / 128);
            if (k % 10 == 0)
            {
                bd.on = (k / 10) % 5 != 4;
                bd.shape = (DynEqShape) ((k / 20) % 3);
                bd.dir = (k / 30) % 2 ? DynEqDirection::Up : DynEqDirection::Down;
                bd.dyn = (k / 40) % 2 == 0;
                bd.g = (k / 50) % 2 ? -12.0 : 9.0;
                t.apply (bd);
            }
            t.run (tl.data() + i, tr.data() + i, 128);
        }
        double worstStep = 0.0;
        for (size_t i = (size_t) t.latency() + 1; i < tl.size(); ++i)
            worstStep = std::max (worstStep, (double) std::abs (tl[i] - tl[i - 1]));
        const double slope = 2.0 * kPi * 500.0 / sr * pstest::dbfsToAmplitude (-12.0 + 9.0);
        say (std::string (mode ? "Digital" : "Analog ") + " toggles every 10 blocks: worst sample step " + fmt (worstStep, 5)
             + " (largest boosted sine slope " + fmt (slope, 5) + ")");
        CHECK (worstStep < 1.25 * slope);
    }
}

TEST_CASE ("deq. denormals: silence after a loud burst costs no more than noise")
{
    const double sr = 48000.0;
    Rig rig (sr, 512);
    for (int b = 0; b < kDynEqBands; ++b)
    {
        Band bd;
        bd.band = b;
        bd.f = 50.0 * std::pow (2.8, b);
        bd.g = b % 2 ? 6.0 : -6.0;
        bd.thr = -60.0;
        bd.rel = 5000.0;
        rig.apply (bd);
    }
    auto noise = pstest::whiteNoise (0.9, 2.0, sr, 9);
    auto nr = noise;
    pstest::Stopwatch sw;
    rig.run (noise, nr);
    const double tNoise = sw.seconds();
    std::vector<float> l ((size_t) (60.0 * sr), 0.0f), r = l;
    pstest::Stopwatch sw2;
    rig.run (l, r);
    const double tSilence = sw2.seconds() / 30.0;
    say ("2 s of noise " + fmt (1000.0 * tNoise, 1) + " ms, 60 s of silence after it (per 2 s) " + fmt (1000.0 * tSilence, 1) + " ms");
    CHECK (tSilence < 2.0 * tNoise + 0.002);
    bool zero = true;
    for (size_t i = l.size() - 48000; i < l.size(); ++i)
        zero = zero && l[i] == 0.0f;
    CHECK (zero);
}

//==============================================================================
TEST_CASE ("deq. meters and UI helpers: band gain / dynamic part / detector level match the audio")
{
    const double sr = 48000.0;
    Rig rig (sr, 512);
    Band bd;
    bd.g = -8.0;
    bd.offset = 1.5;
    bd.thr = -30.0;
    rig.apply (bd);
    const auto x = rmsSine (1000.0, -22.0, 1.5, sr);
    auto l = x, r = x;
    rig.run (l, r);
    const auto& m = rig.deq->getMeters();
    const double measured = toneGainDb (l, x, x.size() - 12001, 12000, 1000.0, sr, 0);
    say ("meters: band gain " + fmt (m.bandGainDb[0][0].load()) + " dB, dynamic " + fmt (m.dynamicDb[0][0].load())
         + " dB, detector " + fmt (m.detectorDb[0][0].load()) + " dB; measured output gain " + fmt (measured) + " dB");
    CHECK (std::abs ((double) m.bandGainDb[0][0].load() - measured) < 0.1);
    CHECK (std::abs ((double) m.dynamicDb[0][0].load() - (measured - 1.5)) < 0.1);
    CHECK (std::abs ((double) m.detectorDb[0][0].load() + 22.0) < 0.25);
    CHECK (std::abs (measured - (1.5 - 4.0)) < 0.15);   // 8 dB over, ratio 2 -> -4 dB, + offset
    // the live curve helper: the band's response at the meter gain equals the measured response at other frequencies
    DynamicEqParams p;
    readDynamicEqParams (rig.values.data(), p);
    const auto h = dynEqBandResponse (p.bands[0][0], m.bandGainDb[0][0].load(), 1000.0, sr);
    CHECK (std::abs (dbOf (h) - measured) < 0.1);
    CHECK (std::abs (dynEqRestGainDb (p.bands[0][0]) - 1.5) < 1.0e-9);
    CHECK (std::abs (dynEqTriggeredGainDb (p.bands[0][0]) - (1.5 - 8.0)) < 1.0e-9);
    // off bands read neutral
    CHECK_EQ (m.bandGainDb[0][3].load(), 0.0f);
    CHECK (m.detectorDb[0][3].load() <= -199.0f);
}

TEST_CASE ("deq. band solo plays the band's sidechain signal (latency aligned); audition band-passes the input")
{
    const double sr = 48000.0;
    for (int mode = 0; mode < 2; ++mode)
    {
        Rig rig (sr, 256);
        rig.set ("mode", (float) mode);
        rig.set ("lookahead", 3.0f);
        Band a;
        a.f = 1000.0;
        a.q = 2.0;
        Band b;
        b.band = 1;
        b.f = 100.0;
        b.sc = DynEqSidechain::Band1;   // band 2 listens to band 1's detector
        rig.apply (a);
        rig.apply (b);
        if (mode == 1)
            REQUIRE (rig.settle());
        const auto t1 = rmsSine (1000.0, -20.0, 1.0, sr), t2 = rmsSine (100.0, -20.0, 1.0, sr);
        std::vector<float> x (t1.size());
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = t1[i] + t2[i];
        for (int solo = 0; solo < 2; ++solo)
        {
            rig.monitor.soloBand = solo;   // band 1, then band 2 (which hears band 1's region too)
            rig.deq->reset();
            auto l = x, r = x;
            rig.run (l, r);
            const int lat = rig.latency();
            const size_t s0 = (size_t) (0.5 * sr);
            const cd o1 = pstest::toneOf (l, s0 + (size_t) lat, 9600, 1000.0, sr), i1 = pstest::toneOf (x, s0, 9600, 1000.0, sr);
            const double g1 = dbOf (o1 / i1), ph = std::arg (o1 / i1);
            const double g2 = toneGainDb (l, x, s0, 9600, 100.0, sr, lat);
            say (std::string (mode ? "Digital" : "Analog ") + " solo band " + std::to_string (solo + 1) + ": 1 kHz "
                 + fmt (g1) + " dB (phase " + fmt (ph, 4) + " rad), 100 Hz " + fmt (g2, 1) + " dB");
            CHECK (std::abs (g1) < 0.05);
            CHECK (std::abs (ph) < 0.01);
            CHECK (g2 < -40.0);
        }
        rig.monitor.soloBand = -1;
        rig.monitor.auditionOn = true;
        rig.monitor.auditionHz = 100.0f;
        rig.monitor.auditionQ = 4.0f;
        rig.deq->reset();
        auto l = x, r = x;
        rig.run (l, r);
        const size_t s0 = (size_t) (0.5 * sr);
        const double a100 = toneGainDb (l, x, s0, 9600, 100.0, sr, rig.latency());
        const double a1k = toneGainDb (l, x, s0, 9600, 1000.0, sr, rig.latency());
        say (std::string (mode ? "Digital" : "Analog ") + " audition at 100 Hz Q 4: 100 Hz " + fmt (a100) + " dB, 1 kHz "
             + fmt (a1k, 1) + " dB");
        CHECK (std::abs (a100) < 0.1);
        CHECK (a1k < -25.0);
        rig.monitor.auditionOn = false;
    }

    // solo switching fades (no click)
    Rig rig (sr, 64);
    Band a;
    a.f = 300.0;
    Band b = a;
    b.band = 1;
    b.f = 600.0;
    rig.apply (a);
    rig.apply (b);
    const auto x = pstest::sine (400.0, -6.0, 1.0, sr);
    auto l = x, r = x;
    for (size_t i = 0; i < x.size(); i += 64)
    {
        const int k = (int) (i / 64);
        rig.monitor.soloBand = (k / 60) % 3 - 1;
        rig.run (l.data() + i, r.data() + i, 64);
    }
    double worst = 0.0;
    for (size_t i = 1; i < l.size(); ++i)
        worst = std::max (worst, (double) std::abs (l[i] - l[i - 1]));
    const double slope = 2.0 * kPi * 400.0 / sr * pstest::dbfsToAmplitude (-6.0);
    say ("solo switching off / band 1 / band 2: worst sample step " + fmt (worst, 5) + " (sine slope " + fmt (slope, 5) + ")");
    CHECK (worst < 1.1 * slope);
}

TEST_CASE ("deq. mode and FIR size switches fade instead of clicking; the Digital bands join after their history")
{
    const double sr = 48000.0;
    Rig rig (sr, 256);
    Band bd;
    bd.f = 800.0;
    bd.g = 6.0;
    bd.dyn = false;
    rig.apply (bd);
    const auto x = pstest::sine (800.0, -12.0, 4.0, sr);
    auto l = x, r = x;
    for (size_t i = 0; i < x.size(); i += 256)
    {
        const int k = (int) (i / 256);
        rig.set ("mode", (k / 150) % 2 ? 1.0f : 0.0f);
        rig.set ("firSize", (float) ((k / 300) % 4));
        rig.run (l.data() + i, r.data() + i, 256);
        if (k % 8 == 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    double worst = 0.0;
    for (size_t i = 1; i < l.size(); ++i)
        worst = std::max (worst, (double) std::abs (l[i] - l[i - 1]));
    const double slope = 2.0 * kPi * 800.0 / sr * pstest::dbfsToAmplitude (-6.0);
    say ("mode / FIR switches every 150 blocks: worst sample step " + fmt (worst, 5) + " (boosted sine slope " + fmt (slope, 5) + ")");
    CHECK (worst < 1.1 * slope);

    // Anchor the join deadline to kernel readiness, so arbitrary worker scheduling cannot hide an audio-side stall.
    Rig s (sr, 256);
    s.apply (bd);
    auto l2 = x, r2 = x;
    s.run (l2.data(), r2.data(), 256 * 40);
    s.set ("mode", 1.0f);
    size_t readyAt = x.size();
    for (size_t i = 256 * 40; i < x.size(); i += 256)
    {
        s.run (l2.data() + i, r2.data() + i, 256);
        if (readyAt == x.size() && ! s.deq->isDesignPending())
            readyAt = i + 256;
        std::this_thread::sleep_for (std::chrono::microseconds (300));
    }
    REQUIRE (readyAt < x.size());
    const int firLength = eqdesign::firLength (1, sr), partition = eqdesign::firPartition (firLength);
    const size_t budget = (size_t) (firLength + 3 * partition + (int) std::lround (0.019 * sr) + 2 * s.block);
    const size_t deadline = readyAt + budget, measureWindow = 600;
    REQUIRE (deadline + measureWindow <= x.size());
    const double joinedGain = toneGainDb (l2, x, deadline - (size_t) s.latency(), measureWindow, 800.0, sr, s.latency());
    const double g = toneGainDb (l2, x, x.size() - 30000, 24000, 800.0, sr, s.latency());
    say ("Digital after the switch: gain " + fmt (joinedGain) + " dB at the " + std::to_string (budget)
         + "-sample deadline after readiness; steady gain " + fmt (g) + " dB (static +6 band)");
    CHECK (std::abs (joinedGain - 6.0) < 0.1);
    CHECK (std::abs (g - 6.0) < 0.1);
}

TEST_CASE ("deq. no allocation in setParams / process / reset / setMonitor (all modes, worker installing kernels)")
{
    Rig rig (48000.0, 512);
    std::vector<float> l (512), r (512);
    pstest::Lcg lcg (5);
    long long allocs = 0;
    int installs = 0;
    const ChannelMode modes[] { ChannelMode::Stereo, ChannelMode::MidSide, ChannelMode::LeftRight, ChannelMode::TransientSustain };
    for (int mode = 0; mode < 2; ++mode)
        for (int fir = 0; fir < (mode == 1 ? 4 : 1); ++fir)
        {
            rig.set ("mode", (float) mode);
            rig.set ("firSize", (float) fir);
            bool wasPending = false;
            for (int k = 0; k < 160; ++k)
            {
                rig.channelMode = modes[(k / 40) % 4];
                if (mode == 0 || k % 20 == 0)
                    for (int s = 0; s < 2; ++s)
                        for (int b = 0; b < kDynEqBands; ++b)
                        {
                            Band bd;
                            bd.set = s;
                            bd.band = b;
                            bd.shape = (DynEqShape) ((b + k / 20 + s) % (int) DynEqShape::Count);
                            bd.f = 40.0 * std::pow (2.6, b) * (1.0 + 0.01 * (k % 7));
                            bd.g = (double) ((k + b) % 20) - 12.0;
                            bd.thr = -50.0 + (k % 9);
                            bd.sc = (DynEqSidechain) ((k + b + s) % (int) DynEqSidechain::Count);
                            bd.dyn = (b + k / 30) % 4 != 0;
                            bd.on = (b + k / 40) % 5 != 0;
                            rig.apply (bd);
                        }
                rig.set ("lookahead", (float) ((k / 25) % 3) * 2.5f);
                rig.monitor.soloBand = (k / 15) % 8 - 1;
                rig.monitor.auditionOn = (k / 33) % 2 == 1;
                for (size_t i = 0; i < l.size(); ++i)
                {
                    l[i] = 0.3f * lcg.next();
                    r[i] = 0.3f * lcg.next();
                }
                {
                    pstest::AllocationScope scope;
                    if (k % 50 == 49)
                        rig.deq->reset();
                    rig.run (l, r);
                    allocs += scope.count();
                }
                const bool pending = rig.deq->isDesignPending();
                if (wasPending && ! pending)
                    ++installs;
                wasPending = pending;
                if (mode == 1)
                    std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }
        }
    say ("allocation test: " + std::to_string (installs) + " kernel installs observed while processing");
    CHECK_EQ (allocs, 0LL);
    CHECK (installs > 0);
}

TEST_CASE ("deq. benchmarks (6 bands x 2 sets, Analog and Digital, typical and worst case)")
{
    const double sr = 48000.0;
    const int block = 512;
    const double seconds = 10.0;
    const size_t n = (size_t) (seconds * sr);
    // programme-like material: noise bursts with a beat so every band keeps moving
    pstest::Lcg lcg (11);
    std::vector<float> srcL (n), srcR (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double env = 0.15 + 0.6 * std::exp (-(double) (i % 12000) / 2000.0);
        srcL[i] = (float) (env * lcg.next());
        srcR[i] = (float) (env * lcg.next());
    }
    auto setBands = [] (Rig& rig, int sets, int bands, int k)
    {
        for (int s = 0; s < sets; ++s)
            for (int b = 0; b < bands; ++b)
            {
                Band bd;
                bd.set = s;
                bd.band = b;
                bd.shape = (DynEqShape) ((b + 1) % (int) DynEqShape::Count);
                bd.f = 60.0 * std::pow (2.7, b) * (1.0 + 0.002 * (k % 50));
                bd.g = b % 2 ? 6.0 : -9.0;
                bd.q = 1.0 + 0.2 * b;
                bd.thr = -35.0;
                bd.att = 2.0;
                bd.rel = 60.0;
                bd.autoScale = true;
                rig.apply (bd);
            }
    };
    struct Run
    {
        const char* name;
        int mode, fir, sets, bands;
        bool automate;
        double minRt;
    };
    const Run runs[] {
        { "Analog, Stereo, 3 bands (typical)", 0, 1, 1, 3, false, 60.0 },
        // 15: the measured analog shelves (gentle slopes at low Q) are fitted per design; 48x -> 17x here
        { "Analog, M/S, 6 x 2 bands moving", 0, 1, 2, 6, false, 15.0 },
        { "Analog, M/S, 6 x 2 bands, every parameter automated every block", 0, 1, 2, 6, true, 10.0 },
        { "Digital 8192, Stereo, 3 bands (typical)", 1, 1, 1, 3, false, 30.0 },
        { "Digital 8192, M/S, 6 x 2 bands moving", 1, 1, 2, 6, false, 15.0 },
        { "Digital 32768, M/S, 6 x 2 bands moving", 1, 3, 2, 6, false, 5.0 },
    };
    for (const auto& run : runs)
    {
        Rig rig (sr, block);
        rig.channelMode = run.sets == 2 ? ChannelMode::MidSide : ChannelMode::Stereo;
        rig.set ("mode", (float) run.mode);
        rig.set ("firSize", (float) run.fir);
        setBands (rig, run.sets, run.bands, 0);
        if (run.mode == 1)
            REQUIRE (rig.settle());
        auto l = srcL, r = srcR;
        pstest::Stopwatch sw;
        for (size_t i = 0; i < n; i += (size_t) block)
        {
            if (run.automate)
                setBands (rig, run.sets, run.bands, (int) (i / (size_t) block));
            rig.run (l.data() + i, r.data() + i, std::min ((size_t) block, n - i));
        }
        const double t = sw.seconds();
        say (std::string ("benchmark ") + run.name + ": " + fmt (seconds / t, 1) + " x real time");
        CHECK (seconds / t > run.minRt);
    }
}
TEST_CASE ("deq. solo lookahead automation crossfades the monitored delay in both modes")
{
    const double sr = 48000.0, hz = 373.0, peakDb = -12.0;
    const size_t change1 = 24000, change2 = 48000;
    for (int mode = 0; mode < 2; ++mode)
    {
        Rig rig (sr, 37);
        rig.set ("mode", (float) mode);
        rig.set ("lookahead", 1.1f);
        Band bd;
        bd.f = hz;
        bd.q = 2.0;
        bd.g = 0.0;
        bd.dyn = false;
        rig.apply (bd);
        rig.monitor.soloBand = 0;
        if (mode == 1)
            REQUIRE (rig.settle());

        const auto x = pstest::sine (hz, peakDb, 1.5, sr);
        auto l = x, r = x;
        for (size_t i = 0; i < x.size();)
        {
            if (i == change1)
                rig.set ("lookahead", 7.3f);
            if (i == change2)
                rig.set ("lookahead", 1.1f);
            const size_t next = i < change1 ? change1 : (i < change2 ? change2 : x.size());
            const size_t count = std::min ((size_t) rig.block, next - i);
            rig.run (l.data() + i, r.data() + i, count);
            i += count;
        }
        double worstStep = 0.0;
        // Exclude detector/monitor startup; both requested delays are exercised after a settled tone.
        for (size_t i = change1 - 1000; i < l.size(); ++i)
            worstStep = std::max (worstStep, (double) std::abs (l[i] - l[i - 1]));
        const double slope = 2.0 * std::sin (kPi * hz / sr) * pstest::dbfsToAmplitude (peakDb);
        say (std::string (mode ? "Digital" : "Analog ") + " solo lookahead 1.1 -> 7.3 -> 1.1 ms: worst step "
             + fmt (worstStep, 6) + ", tone slope " + fmt (slope, 6));
        CHECK (worstStep < 1.3 * slope);
        CHECK_EQ (rig.latency(), (mode == 1 ? 4608 : 0) + 53);
    }
}

TEST_CASE ("deq. automated threshold follows the same gain stream delayed by Dt in Digital")
{
    const double sr = 48000.0, hz = 1000.0;
    const size_t trigger = 24000, release = 38400;
    const auto x = rmsSine (hz, -18.0, 1.25, sr);
    std::vector<std::vector<double>> meters, envelopes;
    int digitalLatency = 0;
    for (int mode = 0; mode < 2; ++mode)
    {
        Rig rig (sr, 1);
        rig.set ("mode", (float) mode);
        Band bd;
        bd.g = -6.0;
        bd.thr = 0.0;
        bd.att = 10.0;
        bd.rel = 30.0;
        rig.apply (bd);
        if (mode == 1)
            REQUIRE (rig.settle());
        auto l = x, r = x;
        std::vector<double> gains (x.size());
        for (size_t i = 0; i < x.size(); ++i)
        {
            if (i == trigger || i == release)
                rig.set ("a.b1.thr", i == trigger ? -40.0f : 0.0f);
            rig.run (l.data() + i, r.data() + i, 1);
            gains[i] = rig.deq->getMeters().bandGainDb[0][0].load();
        }
        meters.push_back (std::move (gains));
        envelopes.push_back (gainEnvelope (l, x, hz, sr, rig.latency(), trigger - 6000, release + 6000));
        if (mode == 1)
            digitalLatency = rig.latency();
    }
    double worstMeter = 0.0, worstEnvelope = 0.0;
    for (size_t i = trigger - 6000; i < release + 6000; ++i)
        worstMeter = std::max (worstMeter, std::abs (meters[1][i + (size_t) digitalLatency] - meters[0][i]));
    for (size_t i = 0; i < envelopes[0].size(); ++i)
        worstEnvelope = std::max (worstEnvelope, std::abs (envelopes[1][i] - envelopes[0][i]));
    say ("threshold automation: aligned meter difference " + fmt (worstMeter, 6)
         + " dB, output envelope difference " + fmt (worstEnvelope, 6) + " dB, Dt " + std::to_string (digitalLatency));
    CHECK_EQ (digitalLatency, 4608);
    CHECK (worstMeter < 0.0001);
    CHECK (worstEnvelope < 0.08);
    CHECK (meters[0][release - 1] < -5.99);
    CHECK (meters[0].back() > -0.01);
}
TEST_CASE ("deq. stopped audio keeps uninstalled kernels pending and cancels disabled requests")
{
    Rig rig;
    rig.set ("mode", 1.0f);
    Band bd;
    bd.f = 700.0;
    bd.g = 6.0;
    bd.dyn = false;
    rig.apply (bd);
    rig.deq->setParams (rig.values.data());
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    // Design completion alone is insufficient: no audio has consumed the ready kernel yet.
    CHECK (rig.deq->isDesignPending());

    rig.off (0, 0);
    rig.deq->setParams (rig.values.data());
    pstest::Stopwatch offWait;
    while (rig.deq->isDesignPending() && offWait.seconds() < 5.0)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    CHECK (! rig.deq->isDesignPending());
    const double offWaitMs = 1000.0 * offWait.seconds();

    // Resume with a different nonzero request while the stopped convolver still owns an older publication.
    bd.f = 1700.0;
    bd.g = -9.0;
    rig.apply (bd);
    REQUIRE (rig.settle());
    const auto x = rmsSine (bd.f, -20.0, 1.0, rig.sr);
    auto l = x, r = x;
    rig.run (l, r);
    const double gain = toneGainDb (l, x, 24000, 12000, bd.f, rig.sr, rig.latency());
    say ("stopped audio request cancelled in " + fmt (offWaitMs, 1)
         + " ms; resumed latest kernel gain " + fmt (gain, 4) + " dB (target -9)");
    CHECK (std::abs (gain + 9.0) < 0.1);
    CHECK (! rig.deq->isDesignPending());
}
TEST_CASE ("deq. Digital reenabled channel waits for its current kernel while the other stays active")
{
    Rig rig;
    rig.channelMode = ChannelMode::LeftRight;
    rig.set ("mode", 1.0f);
    Band a;
    a.g = 3.0;
    a.dyn = false;
    Band b = a;
    b.set = 1;
    b.g = 15.0;
    rig.apply (a);
    rig.apply (b);
    REQUIRE (rig.settle());
    const auto low = rmsSine (1000.0, -26.0, 2.0, rig.sr);
    const auto high = rmsSine (6000.0, -26.0, 2.0, rig.sr);
    auto x = low;
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += high[i];
    auto l = x, r = x;
    size_t pos = 12000;
    rig.run (l.data(), r.data(), pos);
    b.on = false;
    rig.apply (b);
    rig.run (l.data() + pos, r.data() + pos, 2048);
    pos += 2048;
    b.f = 6000.0;
    b.g = -30.0;
    rig.apply (b);
    rig.run (l.data() + pos, r.data() + pos, 1024);
    pos += 1024;

    b.on = true;
    rig.apply (b);
    const size_t first = pos;
    const size_t delay = (size_t) rig.latency();
    // The stored positive coefficient must drain before the new cut kernel can become audible.
    rig.run (l.data() + pos, r.data() + pos, delay);
    pos += delay;
    size_t earlyDifferences = 0;
    for (size_t i = first; i < pos; ++i)
        earlyDifferences += r[i] != x[i - (size_t) rig.latency()];
    CHECK_EQ (earlyDifferences, (size_t) 0);

    while (pos < x.size())
    {
        const size_t count = std::min ((size_t) rig.block, x.size() - pos);
        rig.run (l.data() + pos, r.data() + pos, count);
        pos += count;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    const double latestGain = toneGainDb (r, x, 72000, 12000, 6000.0, rig.sr, rig.latency());
    say ("reenabled channel: " + std::to_string (earlyDifferences) + " early samples differ from aligned dry; latest 6 kHz gain "
         + fmt (latestGain, 4) + " dB (target -30, previously +15)");
    CHECK (std::abs (latestGain + 30.0) < 0.1);
    CHECK (! rig.deq->isDesignPending());
}

TEST_CASE ("deq. stopped audio does not repeatedly redesign a kernel awaiting consumption")
{
    Rig rig;
    rig.set ("mode", 1.0f);
    rig.set ("firSize", 3.0f);
    Band bd;
    bd.g = 6.0;
    bd.dyn = false;
    rig.apply (bd);
    REQUIRE (rig.settle());
    bd.f = 2000.0;
    rig.apply (bd);
    rig.deq->setParams (rig.values.data());
    std::this_thread::sleep_for (std::chrono::milliseconds (150));
    bd.f = 4000.0;
    rig.apply (bd);
    rig.deq->setParams (rig.values.data());
    const double before = processCpuSeconds();
    REQUIRE (before >= 0.0);
    pstest::Stopwatch wall;
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    const double elapsed = wall.seconds();
    const double cpu = processCpuSeconds() - before;
    say ("stopped audio with a queued large kernel: " + fmt (1000.0 * cpu, 1) + " ms process CPU in "
         + fmt (1000.0 * elapsed, 1) + " ms elapsed");
    CHECK (cpu >= 0.0);
    CHECK (cpu < 0.45 * elapsed + 0.01);
    CHECK (rig.deq->isDesignPending());
    REQUIRE (rig.settle());
}
TEST_CASE ("deq. active solo shape and sidechain edits fade and reach the requested signal")
{
    const double sr = 48000.0;
    const size_t change = 24000, measureStart = 42000, measureCount = 9600;
    for (int mode = 0; mode < 2; ++mode)
    {
        Rig shape (sr, 37);
        shape.set ("mode", (float) mode);
        shape.set ("lookahead", 3.0f);
        Band bd;
        bd.shape = DynEqShape::LowShelfAnalog;
        bd.f = 1000.0;
        bd.g = 0.0;
        bd.dyn = false;
        shape.apply (bd);
        shape.monitor.soloBand = 0;
        if (mode == 1)
            REQUIRE (shape.settle());
        const auto probe = pstest::sine (5000.0, -40.0, 1.25, sr);
        auto x = probe;
        for (auto& sample : x)
            sample += 0.25f;
        auto l = x, r = x;
        shape.run (l.data(), r.data(), change);
        bd.shape = DynEqShape::HighShelfAnalog;
        shape.apply (bd);
        shape.run (l.data() + change, r.data() + change, l.size() - change);
        double worstShapeStep = 0.0, finalDc = 0.0;
        for (size_t i = change; i < l.size(); ++i)
            worstShapeStep = std::max (worstShapeStep, (double) std::abs (l[i] - l[i - 1]));
        for (size_t i = measureStart; i < measureStart + measureCount; ++i)
            finalDc += l[i + (size_t) shape.latency()] / (double) measureCount;
        const double highGain = toneGainDb (l, probe, measureStart, measureCount, 5000.0, sr, shape.latency());
        CHECK (worstShapeStep < 0.012);
        CHECK (std::abs (finalDc) < 0.0001);
        CHECK (std::abs (highGain) < 0.1);

        Rig source (sr, 37);
        source.channelMode = ChannelMode::LeftRight;
        source.set ("mode", (float) mode);
        source.set ("lookahead", 3.0f);
        bd.shape = DynEqShape::LowShelfAnalog;
        source.apply (bd);
        Band other = bd;
        other.set = 1;
        source.apply (other);
        source.monitor.soloBand = 0;
        if (mode == 1)
            REQUIRE (source.settle());
        std::vector<float> sl (60000, 0.25f), srOut (60000, -0.25f);
        source.run (sl.data(), srOut.data(), change);
        for (int edit = 0; edit < 3; ++edit)
        {
            bd.sc = edit == 1 ? DynEqSidechain::Own : DynEqSidechain::OtherChannel;
            source.apply (bd);
            source.run (sl.data() + change + (size_t) edit * 120, srOut.data() + change + (size_t) edit * 120, 120);
        }
        const size_t tail = change + 360;
        source.run (sl.data() + tail, srOut.data() + tail, sl.size() - tail);
        double worstSourceStep = 0.0, leftMean = 0.0, rightMean = 0.0;
        for (size_t i = change; i < sl.size(); ++i)
            worstSourceStep = std::max (worstSourceStep, (double) std::abs (sl[i] - sl[i - 1]));
        for (size_t i = measureStart; i < measureStart + measureCount; ++i)
        {
            leftMean += sl[i + (size_t) source.latency()] / (double) measureCount;
            rightMean += srOut[i + (size_t) source.latency()] / (double) measureCount;
        }
        say (std::string (mode ? "Digital" : "Analog ") + " active solo edits: shape step " + fmt (worstShapeStep, 6)
             + ", final high-pass 5 kHz gain " + fmt (highGain, 4) + " dB; source step " + fmt (worstSourceStep, 6)
             + ", final channels " + fmt (leftMean, 4) + " / " + fmt (rightMean, 4));
        CHECK (worstSourceStep < 0.003);
        CHECK (std::abs (leftMean + 0.25) < 0.0001);
        CHECK (std::abs (rightMean + 0.25) < 0.0001);
    }
}
} // namespace
