#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/EqDesign.h"
#include "dsp/Filters.h"
#include "dsp/Equalizer.h"
#include "dsp/Params.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Equalizer tests (dsp/Equalizer.cpp, dsp/EqDesign.cpp). Prefix "eq.".
namespace
{
using namespace ps;
using pstest::cd;
using pstest::kPi;

//==============================================================================
// Radix-2 FFT with precomputed twiddles (independent of the code under test; faster than pstest::fftInPlace for the
// many long transforms here).
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

double dbOf (cd h) { return pstest::dbOf (std::abs (h)); }

void say (const std::string& s) { std::cout << "    " << s << "\n"; }

std::string bandId (int set, int b, const char* name)
{
    return std::string (set == 0 ? "a" : "b") + ".b" + std::to_string (b + 1) + "." + name;
}

const char* shapeName (EqShape s)
{
    static const char* names[] { "Bell", "PropQ", "BandShelf", "LS Analog", "LS Baxandall", "LS Vintage",
                                 "LS Resonant", "HS Analog", "HS Baxandall", "HS Vintage", "HS Resonant", "HP Flat",
                                 "HP Resonant", "HP Brickwall", "LP Flat", "LP Resonant", "LP Brickwall", "Surgical" };
    return names[std::clamp ((int) s, 0, (int) EqShape::Count - 1)];
}

//==============================================================================
struct Rig
{
    double sr;
    int block;
    ModuleType type;
    std::string prefix;
    std::vector<float> values;
    std::unique_ptr<Equalizer> eq;
    ChannelMode channelMode = ChannelMode::Stereo;
    ModuleMonitor monitor;

    explicit Rig (double rate = 48000.0, int blockSize = 512, ModuleType t = ModuleType::Equalizer1)
        : sr (rate), block (blockSize), type (t), prefix (moduleInfo (t).idPrefix)
    {
        for (const auto& s : paramSpecs())
            values.push_back (s.defaultValue);
        eq = std::make_unique<Equalizer> (t);
        eq->prepare (sr, block);
    }

    void set (const std::string& id, float v)
    {
        const int i = paramIndex (prefix + "." + id);
        if (i >= 0)
            values[(size_t) i] = v;
        else
            pstest::reportFailure (__FILE__, __LINE__, "unknown parameter " + id);
    }

    void band (int set, int b, EqShape shape, double f, double g, double q = 0.707, EqSlope slope = EqSlope::Db12,
               double phase = 0.0)
    {
        this->set (bandId (set, b, "on"), 1.0f);
        this->set (bandId (set, b, "shape"), (float) shape);
        this->set (bandId (set, b, "freq"), (float) f);
        this->set (bandId (set, b, "gain"), (float) g);
        this->set (bandId (set, b, "q"), (float) q);
        this->set (bandId (set, b, "slope"), (float) slope);
        this->set (bandId (set, b, "phase"), (float) phase);
    }

    void off (int set, int b) { this->set (bandId (set, b, "on"), 0.0f); }

    void allOff()
    {
        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kEqBands; ++b)
                off (s, b);
    }

    EqParams params() const
    {
        EqParams p;
        readEqParams (values.data(), type, p);
        return p;
    }

    void run (float* l, float* r, size_t n)
    {
        ProcessContext ctx;
        ctx.channelMode = channelMode;
        for (size_t i = 0; i < n; i += (size_t) block)
        {
            const int m = (int) std::min ((size_t) block, n - i);
            eq->setParams (values.data());
            eq->setMonitor (monitor);
            float* p[2] { l + i, r + i };
            eq->process (p, m, ctx);
        }
    }

    void run (std::vector<float>& l, std::vector<float>& r) { run (l.data(), r.data(), l.size()); }

    void silence (size_t n)
    {
        std::vector<float> l (n, 0.0f), r (n, 0.0f);
        run (l, r);
    }

    // Digital mode: waits for the worker's kernel, then flushes the convolver history.
    bool settle (double timeoutSeconds = 20.0)
    {
        eq->setParams (values.data());
        pstest::Stopwatch sw;
        while (eq->isDesignPending())
        {
            if (sw.seconds() > timeoutSeconds)
                return false;
            silence ((size_t) block);
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        silence ((size_t) (2 * eq->getLatencySamples() + 4 * block));
        return true;
    }

    // Impulse response of both channels (impulse at sample 0 of the returned buffers).
    void impulse (size_t n, std::vector<float>& hl, std::vector<float>& hr, float al = 1.0f, float ar = 1.0f)
    {
        hl.assign (n, 0.0f);
        hr.assign (n, 0.0f);
        hl[0] = al;
        hr[0] = ar;
        run (hl, hr);
    }
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

//==============================================================================
// Analog: the measured response of process() equals eqBandResponse (the realised biquads) for one band.
struct GridResult
{
    double worstDb = 0.0, worstRel = 0.0;
    std::string worstWhat;
    int configs = 0;
};

void checkAnalogBand (Rig& rig, const LocalFft& fftShort, const LocalFft& fftLong, EqShape shape, double f, double g,
                      double q, EqSlope slope, GridResult& res)
{
    rig.allOff();
    rig.set ("mode", 0.0f);
    rig.band (0, 3, shape, f, g, q, slope);
    rig.eq->reset();   // the first parameters after a reset apply without smoothing

    const LocalFft& fft = f < 100.0 ? fftLong : fftShort;
    std::vector<float> hl, hr;
    rig.impulse (fft.size(), hl, hr);
    const auto H = fft.spectrum (hl);
    const auto bp = rig.params().bands[0][3];

    ++res.configs;
    const double binHz = rig.sr / (double) fft.size();
    for (size_t k = 1; k < fft.size() / 2; k += std::max<size_t> (1, k / 64))
    {
        const double fk = (double) k * binHz;
        if (fk < 15.0 || fk > 0.49 * rig.sr)
            continue;
        const cd want = eqBandResponse (bp, EqFilterMode::Analog, fk, rig.sr, 100.0f);
        if (dbOf (want) < -60.0)
            continue;
        const double e = std::abs (dbOf (H[k]) - dbOf (want));
        const double rel = std::abs (H[k] - want) / std::abs (want);
        if (e > res.worstDb)
        {
            res.worstDb = e;
            char buf[200];
            std::snprintf (buf, sizeof (buf), "%s f %.0f g %.1f q %.2f slope %d at %.1f Hz (%.2f dB): %.4f dB",
                           shapeName (shape), f, g, q, kEqSlopeDb[(size_t) slope], fk, dbOf (want), e);
            res.worstWhat = buf;
        }
        if (dbOf (want) > -40.0)
            res.worstRel = std::max (res.worstRel, rel);
    }
}

TEST_CASE ("eq. analog: measured response of every shape equals eqBandResponse (realised biquads)")
{
    for (double rate : { 48000.0, 44100.0, 96000.0 })
    {
        Rig rig (rate, 512);
        const LocalFft fftShort (rate > 50000.0 ? 65536 : 32768), fftLong (rate > 50000.0 ? 262144 : 131072);
        GridResult res;
        const std::vector<double> freqs { 30.0, 250.0, 2000.0, 9000.0, 17000.0 };
        const bool full = rate == 48000.0;

        for (int si = 0; si < (int) EqShape::Count; ++si)
        {
            const auto shape = (EqShape) si;
            if (! full && ! (shape == EqShape::Bell || shape == EqShape::HighShelfAnalog || shape == EqShape::HighPassFlat
                             || shape == EqShape::LowPassBrickwall || shape == EqShape::LowShelfVintage))
                continue;
            for (double f : freqs)
            {
                if (eqdesign::isPassShape (shape))
                {
                    const bool slopes = eqdesign::usesSlope (shape);
                    const bool resonant = shape == EqShape::HighPassResonant || shape == EqShape::LowPassResonant;
                    for (int sl = 0; sl < (slopes ? (int) EqSlope::Count : 1); ++sl)
                        for (double q : resonant ? std::vector<double> { 0.5, 0.707, 3.0 } : std::vector<double> { 0.707 })
                            checkAnalogBand (rig, fftShort, fftLong, shape, f, 0.0, q, (EqSlope) sl, res);
                }
                else
                {
                    for (double g : { -24.0, -6.0, 4.0, 15.0 })
                        for (double q : { 0.3, 1.5, 8.0 })
                            checkAnalogBand (rig, fftShort, fftLong, shape, f, g, q, EqSlope::Db12, res);
                }
            }
        }

        say ("analog response grid @ " + std::to_string ((int) rate) + " Hz: " + std::to_string (res.configs)
                      + " configurations, worst " + std::to_string (res.worstDb) + " dB (" + res.worstWhat
                      + "), worst complex error " + std::to_string (res.worstRel));
        CHECK (res.worstDb < 0.05);
        CHECK (res.worstRel < 0.006);
    }
}

TEST_CASE ("eq. analog: shapes do what they say (spot measurements of eqBandResponse)")
{
    const double sr = 48000.0;
    auto resp = [&] (EqShape s, double f0, double g, double q, double f, EqSlope sl = EqSlope::Db12)
    {
        EqBandParams b;
        b.on = true;
        b.shape = s;
        b.freqHz = (float) f0;
        b.gainDb = (float) g;
        b.q = (float) q;
        b.slope = sl;
        return dbOf (eqBandResponse (b, EqFilterMode::Analog, f, sr, 100.0f));
    };

    // bell: full gain at the centre
    CHECK (std::abs (resp (EqShape::Bell, 1000, 6, 1, 1000) - 6.0) < 0.05);
    // proportional Q: narrower at high gain, wider at low gain than a constant-Q bell (relative to its own peak)
    const double propLow = resp (EqShape::ProportionalQ, 1000, 2, 1, 2000) / 2.0;
    const double bellLow = resp (EqShape::Bell, 1000, 2, 1, 2000) / 2.0;
    const double propHigh = resp (EqShape::ProportionalQ, 1000, 15, 1, 2000) / 15.0;
    const double bellHigh = resp (EqShape::Bell, 1000, 15, 1, 2000) / 15.0;
    CHECK (propLow > bellLow);
    CHECK (propHigh < bellHigh);
    // band shelf: flat top
    CHECK (std::abs (resp (EqShape::BandShelf, 1000, 6, 0.3, 1000) - 6.0) < 0.3);
    CHECK (std::abs (resp (EqShape::BandShelf, 1000, 6, 0.3, 1400) - 6.0) < 0.5);
    // shelves: full gain far away, half at the corner
    for (EqShape s : { EqShape::LowShelfAnalog, EqShape::LowShelfResonant })
    {
        CHECK (std::abs (resp (s, 200, 8, 0.707, 15) - 8.0) < 0.3);
        CHECK (std::abs (resp (s, 200, 8, 0.707, 200) - 4.0) < 0.3);
        CHECK (std::abs (resp (s, 200, 8, 0.707, 18000)) < 0.1);
    }
    // Baxandall shelves follow the reference's measured responses (its renders at 500 Hz / 5 kHz)
    CHECK (std::abs (resp (EqShape::LowShelfBaxandall, 500, -6, 0.707, 500) - (-0.08)) < 0.1);
    CHECK (std::abs (resp (EqShape::LowShelfBaxandall, 500, -6, 0.707, 100) - (-1.97)) < 0.2);
    CHECK (std::abs (resp (EqShape::LowShelfBaxandall, 500, -6, 0.707, 20) - (-10.97)) < 0.3);
    CHECK (std::abs (resp (EqShape::HighShelfBaxandall, 5000, 6, 0.707, 5000) - 2.59) < 0.1);
    CHECK (std::abs (resp (EqShape::HighShelfBaxandall, 5000, -12, 0.707, 5000) - (-3.17)) < 0.1);
    CHECK (std::abs (resp (EqShape::HighShelfBaxandall, 1000, 6, 0.707, 20)) < 0.05);
    // Baxandall is gentler than the analog shelf one octave above the corner
    CHECK (resp (EqShape::HighShelfBaxandall, 4000, 8, 0.707, 2000) > resp (EqShape::HighShelfAnalog, 4000, 8, 0.707, 2000));
    // vintage: boost below, dip above the corner
    CHECK (resp (EqShape::LowShelfVintage, 100, 10, 0.707, 20) > 9.0);
    CHECK (resp (EqShape::LowShelfVintage, 100, 10, 0.707, 300) < -1.5);
    CHECK (resp (EqShape::HighShelfVintage, 8000, 10, 0.707, 8000.0 / 3.0) < -1.5);
    // resonant shelf: bump above the plateau
    double peak = -100.0;
    for (double f : logGrid (20, 400, 200))
        peak = std::max (peak, resp (EqShape::LowShelfResonant, 200, 8, 1.5, f));
    CHECK (peak > 9.0);
    // HP / LP flat: -3 dB at the corner, slope
    for (int sl = 0; sl < (int) EqSlope::Count; ++sl)
    {
        const double slope = kEqSlopeDb[(size_t) sl];
        CHECK (std::abs (resp (EqShape::HighPassFlat, 400, 0, 0.707, 400, (EqSlope) sl) + 3.01) < 0.1);
        const double oct = resp (EqShape::HighPassFlat, 400, 0, 0.707, 25, (EqSlope) sl)
                         - resp (EqShape::HighPassFlat, 400, 0, 0.707, 50, (EqSlope) sl);
        CHECK (std::abs (oct + slope) < 0.6);
        CHECK (std::abs (resp (EqShape::LowPassFlat, 2000, 0, 0.707, 2000, (EqSlope) sl) + 3.01) < 0.1);
        // resonant: Q 3 bumps above 0 dB
        double pk = -100.0;
        for (double f : logGrid (200, 4000, 300))
            pk = std::max (pk, resp (EqShape::LowPassResonant, 1000, 0, 3.0, f, (EqSlope) sl));
        CHECK (pk > 3.0);
    }
    // brickwall: flat to the corner, > 90 dB down 0.75 octave beyond
    CHECK (resp (EqShape::LowPassBrickwall, 2000, 0, 0.707, 2000) > -0.11);
    CHECK (resp (EqShape::LowPassBrickwall, 2000, 0, 0.707, 1000) > -0.11);
    CHECK (resp (EqShape::LowPassBrickwall, 2000, 0, 0.707, 2000 * 1.19) < -25.0);
    CHECK (resp (EqShape::LowPassBrickwall, 2000, 0, 0.707, 2000 * 1.7) < -90.0);
    CHECK (resp (EqShape::HighPassBrickwall, 50, 0, 0.707, 50 / 1.7) < -90.0);
    CHECK (resp (EqShape::HighPassBrickwall, 50, 0, 0.707, 60) > -0.11);
    say ("brickwall LP 2 kHz: -0.1 dB at 2 kHz, " + std::to_string (resp (EqShape::LowPassBrickwall, 2000, 0, 0.707, 2000 * 1.19))
                  + " dB at +1/4 oct, " + std::to_string (resp (EqShape::LowPassBrickwall, 2000, 0, 0.707, 4000)) + " dB at +1 oct");
    // surgical in analog = bell
    CHECK (resp (EqShape::Surgical, 1000, -12, 8, 1100) == resp (EqShape::Bell, 1000, -12, 8, 1100));
}

TEST_CASE ("eq. analog: flat EQ (bands off, 0 dB, amount 0) is a bit-exact null")
{
    Rig rig;
    pstest::Lcg lcg (7);
    std::vector<float> l (48000), r (48000);
    for (size_t i = 0; i < l.size(); ++i)
    {
        l[i] = 0.5f * lcg.next();
        r[i] = 0.5f * lcg.next();
    }

    auto nullOf = [&] (Rig& rr)
    {
        auto a = l, b = r;
        rr.run (a, b);
        bool exact = true;
        for (size_t i = 0; i < l.size(); ++i)
            exact = exact && a[i] == l[i] && b[i] == r[i];
        return exact;
    };

    CHECK (nullOf (rig));   // defaults: all bands off

    // Every gain shape on at 0 dB, including Bell, Proportional Q, Band Shelf and Surgical.
    for (int s = 0; s < (int) EqShape::Count; ++s)
    {
        const auto shape = (EqShape) s;
        if (eqdesign::isPassShape (shape))
            continue;
        rig.allOff();
        for (int b = 0; b < kEqBands; ++b)
            rig.band (0, b, shape, 100.0 * (b + 1), 0.0, 1.0);
        rig.eq->reset();
        CHECK (nullOf (rig));
    }

    // gain bands at +-12 dB with Amount 0
    Rig r2;
    for (int b = 0; b < kEqBands; ++b)
        r2.band (0, b, b % 2 == 0 ? EqShape::Bell : EqShape::HighShelfVintage, 150.0 * (b + 1), b % 2 ? 12.0 : -12.0, 2.0);
    r2.set ("amount", 0.0f);
    CHECK (nullOf (r2));

    // a band turned off fades out to a bit-exact null again
    Rig r3;
    r3.band (0, 0, EqShape::Bell, 1000, 6, 1);
    r3.silence (4800);
    r3.off (0, 0);
    r3.silence (4800);   // > 15 ms crossfade
    CHECK (nullOf (r3));
}

TEST_CASE ("eq. amount scales every gain, clamped to -30..+15 dB")
{
    const double sr = 48000.0;
    EqBandParams b;
    b.on = true;
    b.shape = EqShape::Bell;
    b.freqHz = 1000.0f;
    b.gainDb = 10.0f;
    b.q = 1.0f;
    CHECK (std::abs (dbOf (eqBandResponse (b, EqFilterMode::Analog, 1000, sr, 50.0f)) - 5.0) < 0.02);
    CHECK (std::abs (dbOf (eqBandResponse (b, EqFilterMode::Analog, 1000, sr, 200.0f)) - 15.0) < 0.02);   // clamped
    b.gainDb = -20.0f;
    CHECK (std::abs (dbOf (eqBandResponse (b, EqFilterMode::Analog, 1000, sr, 200.0f)) + 30.0) < 0.05);   // clamped
    CHECK (std::abs (dbOf (eqBandResponse (b, EqFilterMode::Analog, 1000, sr, 0.0f))) < 1.0e-9);

    // measured: Amount 50 % halves the gain in process()
    Rig rig;
    rig.band (0, 0, EqShape::Bell, 1000, 10, 1);
    rig.set ("amount", 50.0f);
    rig.eq->reset();
    auto x = pstest::sine (1000, -20, 0.5, sr);
    auto y = x;
    rig.run (x, y);
    const double gain = pstest::dbOf (std::abs (pstest::toneOf (x, 12000, 12000, 1000, sr)))
                      - pstest::dbOf (pstest::dbfsToAmplitude (-20));
    CHECK (std::abs (gain - 5.0) < 0.02);
}

TEST_CASE ("eq. defaultShapeForNewNode")
{
    EqBandParams bands[kEqBands];
    CHECK_EQ (defaultShapeForNewNode (bands, 60), EqShape::LowShelfBaxandall);
    CHECK_EQ (defaultShapeForNewNode (bands, 100), EqShape::ProportionalQ);
    CHECK_EQ (defaultShapeForNewNode (bands, 8000), EqShape::ProportionalQ);
    CHECK_EQ (defaultShapeForNewNode (bands, 9000), EqShape::HighShelfBaxandall);
    CHECK_EQ (defaultShapeForNewNode (nullptr, 9000), EqShape::HighShelfBaxandall);
    bands[2].on = true;
    bands[2].shape = EqShape::LowShelfBaxandall;
    CHECK_EQ (defaultShapeForNewNode (bands, 60), EqShape::ProportionalQ);
    CHECK_EQ (defaultShapeForNewNode (bands, 12000), EqShape::HighShelfBaxandall);
    bands[2].on = false;   // a disabled band does not count
    CHECK_EQ (defaultShapeForNewNode (bands, 60), EqShape::LowShelfBaxandall);
    bands[5].on = true;
    bands[5].shape = EqShape::HighShelfBaxandall;
    CHECK_EQ (defaultShapeForNewNode (bands, 12000), EqShape::ProportionalQ);
}

TEST_CASE ("eq. M/S and L/R: set A on channel 0, set B on channel 1, independent; Stereo uses set A on both")
{
    for (ChannelMode mode : { ChannelMode::MidSide, ChannelMode::LeftRight })
    {
        Rig rig;
        rig.channelMode = mode;
        rig.band (0, 0, EqShape::Bell, 1000, 6, 1);   // set A only
        rig.eq->reset();
        std::vector<float> hl, hr;
        rig.impulse (16384, hl, hr);
        const LocalFft fft (16384);
        const auto Hl = fft.spectrum (hl);
        CHECK (std::abs (dbOf (Hl[(size_t) std::lround (1000.0 * 16384 / 48000.0)]) - 6.0) < 0.1);
        bool exact = hr[0] == 1.0f;
        for (size_t i = 1; i < hr.size(); ++i)
            exact = exact && hr[i] == 0.0f;
        CHECK (exact);   // set B is flat: channel 1 untouched

        Rig rb;
        rb.channelMode = mode;
        rb.band (1, 4, EqShape::HighShelfAnalog, 3000, -9, 0.707);   // set B only
        rb.eq->reset();
        rb.impulse (16384, hl, hr);
        const auto Hr = fft.spectrum (hr);
        exact = hl[0] == 1.0f;
        for (size_t i = 1; i < hl.size(); ++i)
            exact = exact && hl[i] == 0.0f;
        CHECK (exact);
        CHECK (std::abs (dbOf (Hr[8000]) + 9.0) < 0.1);   // 23.4 kHz: full shelf cut
    }

    Rig st;   // Stereo: set B ignored, set A on both channels
    st.band (0, 0, EqShape::Bell, 1000, 6, 1);
    st.band (1, 0, EqShape::Bell, 1000, -12, 1);
    st.eq->reset();
    std::vector<float> hl, hr;
    st.impulse (16384, hl, hr);
    CHECK (hl == hr);
    const LocalFft fft (16384);
    CHECK (std::abs (dbOf (fft.spectrum (hr)[(size_t) std::lround (1000.0 * 16384 / 48000.0)]) - 6.0) < 0.1);
}

//==============================================================================
TEST_CASE ("eq. fast automation (every block, extreme values) stays bounded and click-free")
{
    const double sr = 48000.0;
    const size_t n = 4 * 65536;
    const double toneHz = 150.0;

    // 1. continuous: band frequency, gain and Q jump between extremes every 64-sample block, 4 bands at once
    Rig rig (sr, 64);
    auto x = pstest::sine (toneHz, -20.0, (double) n / sr, sr);
    auto l = x, r = x;
    bool finite = true;
    double peak = 0.0;
    pstest::Lcg lcg (99);
    for (size_t i = 0; i < n; i += 64)
    {
        const bool hi = ((i / 64) % 2) == 0;
        for (int b = 0; b < 4; ++b)
        {
            const double f = hi ? 20000.0 : 20.0;
            rig.band (0, b, b == 3 ? EqShape::HighShelfResonant : EqShape::Bell, f * (1.0 + 0.1 * b),
                      hi ? 15.0 : -30.0, hi ? 24.0 : 0.1);
        }
        rig.run (l.data() + i, r.data() + i, 64);
    }
    for (size_t i = 0; i < n; ++i)
    {
        finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
        peak = std::max (peak, (double) std::abs (l[i]));
    }
    CHECK (finite);
    say ("extreme every-block automation: output peak " + std::to_string (pstest::dbOf (peak)) + " dBFS (input -20)");
    CHECK (peak < pstest::dbfsToAmplitude (-20.0 + 4 * 15.0 + 3.0));   // never more than the sum of the boosts

    // 2. HF splatter: a 150 Hz tone through a band that is automated every block (frequency 60..300 Hz, gain
    //    -12..+12 dB, Q 0.5..4, random). The spectrum above 2 kHz must stay far below the tone. Reference: a plain
    //    direct-form biquad whose coefficients jump to the new design every block (what clicks sound like).
    auto splatter = [&] (int blockSize, bool shapes, bool naive = false)
    {
        Rig a (sr, blockSize);
        Biquad hard;
        auto yl = x, yr = x;
        pstest::Lcg rnd (1234);
        for (size_t i = 0; i < n; i += (size_t) blockSize)
        {
            const double u1 = 0.5 + 0.5 * rnd.next(), u2 = rnd.next(), u3 = 0.5 + 0.5 * rnd.next();
            EqShape s = EqShape::Bell;
            if (shapes)
            {
                const int k = (int) ((0.5 + 0.5 * rnd.next()) * 4.0) % 4;
                s = k == 0 ? EqShape::Bell : (k == 1 ? EqShape::LowShelfAnalog : (k == 2 ? EqShape::BandShelf : EqShape::ProportionalQ));
            }
            const double f = 60.0 * std::pow (5.0, u1), g = 12.0 * u2, q = 0.5 * std::pow (8.0, u3);
            if (naive)
            {
                hard.setCoeffs (design::peak (sr, f, q, g));
                hard.process (yl.data() + i, blockSize);
                continue;
            }
            a.band (0, 0, s, f, g, q);
            a.run (yl.data() + i, yr.data() + i, (size_t) blockSize);
        }
        const LocalFft fft (65536);
        std::vector<float> seg (65536);
        double hf = 0.0, total = 0.0;
        for (int w = 1; w < 4; ++w)
        {
            for (size_t i = 0; i < 65536; ++i)
                seg[i] = (float) (yl[(size_t) w * 65536 + i] * (0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / 65536.0)));
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
    const double s64 = splatter (64, false), s512 = splatter (512, false), sShapes = splatter (512, true);
    const double h64 = splatter (64, false, true), h512 = splatter (512, false, true);
    say ("HF splatter (> 2 kHz, relative to the 150 Hz tone): random every 64 samples " + std::to_string (s64)
         + " dB (hard-switched biquad " + std::to_string (h64) + " dB), every 512 " + std::to_string (s512)
         + " dB (hard " + std::to_string (h512) + " dB), with random shape changes " + std::to_string (sShapes) + " dB");
    CHECK (s64 < -70.0);
    CHECK (s512 < -75.0);
    CHECK (sShapes < -75.0);
    CHECK (s64 < h64 - 20.0);
    CHECK (s512 < h512 - 20.0);

    // 3. on/off and shape toggled every block: bounded
    Rig t (sr, 128);
    auto tl = x, tr = x;
    finite = true;
    peak = 0.0;
    for (size_t i = 0; i < n / 4; i += 128)
    {
        const int k = (int) (i / 128);
        t.band (0, 0, (EqShape) (k % (int) EqShape::Count), 500, (k % 3) * 5.0, 2.0, (EqSlope) (k % 6));
        if (k % 2)
            t.off (0, 0);
        t.run (tl.data() + i, tr.data() + i, 128);
    }
    for (size_t i = 0; i < n / 4; ++i)
    {
        finite = finite && std::isfinite (tl[i]);
        peak = std::max (peak, (double) std::abs (tl[i]));
    }
    CHECK (finite);
    CHECK (peak < pstest::dbfsToAmplitude (-20.0 + 15.0 + 1.0));
}

//==============================================================================
TEST_CASE ("eq. digital: flat EQ is a pure delay of exactly the reported latency")
{
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (int fir = 0; fir < 4; ++fir)
        {
            Rig rig (rate, 512);
            rig.set ("mode", 1.0f);
            rig.set ("firSize", (float) fir);
            rig.band (0, 2, EqShape::Bell, 1000, 0.0, 1.0);   // on, but 0 dB
            rig.eq->setParams (rig.values.data());
            const int lat = rig.eq->getLatencySamples();
            CHECK_EQ (lat, eqdesign::firLatency (fir, rate));
            CHECK (rig.settle());

            pstest::Lcg lcg (3);
            const size_t n = (size_t) lat + 20000;
            std::vector<float> l (n), r (n);
            for (size_t i = 0; i < n; ++i)
            {
                l[i] = 0.5f * lcg.next();
                r[i] = 0.5f * lcg.next();
            }
            auto a = l, b = r;
            rig.run (a, b);
            double err = 0.0;
            for (size_t i = (size_t) lat; i < n; ++i)
                err = std::max (err, (double) std::max (std::abs (a[i] - l[i - (size_t) lat]), std::abs (b[i] - r[i - (size_t) lat])));
            CHECK (pstest::dbOf (err) < -120.0);
            if (fir == 3)
                say ("digital flat @ " + std::to_string ((int) rate) + ": latency " + std::to_string (lat)
                              + ", max error vs delayed input " + std::to_string (pstest::dbOf (err)) + " dB");
        }
}

TEST_CASE ("eq. digital: latency constant while nodes move, impulse peak at the reported latency, symmetric kernel")
{
    for (int fir = 0; fir < 4; ++fir)
    {
        Rig rig (48000.0, 256);
        rig.set ("mode", 1.0f);
        rig.set ("firSize", (float) fir);
        rig.band (0, 0, EqShape::Bell, 300, 6, 1.0);
        rig.band (0, 1, EqShape::HighShelfAnalog, 6000, -4, 0.707);
        rig.band (0, 2, EqShape::HighPassFlat, 40, 0, 0.707, EqSlope::Db24);
        rig.band (0, 3, EqShape::Surgical, 2500, -8, 6);
        rig.eq->setParams (rig.values.data());
        const int lat = rig.eq->getLatencySamples();
        bool constant = true;
        for (int k = 0; k < 40; ++k)
        {
            rig.band (0, 0, EqShape::Bell, 100.0 + 200.0 * k, 12.0 - k, 0.5 + 0.1 * k);
            rig.band (0, 5, EqShape::LowPassBrickwall, 20000.0 - 300.0 * k, 0, 1);
            rig.silence (256);
            constant = constant && rig.eq->getLatencySamples() == lat;
        }
        CHECK (constant);
        rig.band (0, 0, EqShape::Bell, 300, 6, 1.0);
        rig.off (0, 5);
        CHECK (rig.settle());

        std::vector<float> hl, hr;
        rig.impulse ((size_t) (3 * lat), hl, hr);
        CHECK_EQ ((int) argmaxAbs (hl), lat);
        double lr = 0.0;   // Stereo: both channels use set A (packed in one complex FFT, so equal up to rounding)
        for (size_t i = 0; i < hl.size(); ++i)
            lr = std::max (lr, (double) std::abs (hl[i] - hr[i]));
        CHECK (lr < 1.0e-6);

        // linear phase: symmetric about the peak
        const int half = eqdesign::firLength (fir, 48000.0) / 2;
        double asym = 0.0;
        for (int m = 1; m < half; ++m)
            asym = std::max (asym, (double) std::abs (hl[(size_t) (lat + m)] - hl[(size_t) (lat - m)]));
        say ("FIR " + std::to_string (eqdesign::firLength (fir, 48000.0)) + ": latency " + std::to_string (lat)
                      + ", impulse peak at " + std::to_string (argmaxAbs (hl)) + ", asymmetry "
                      + std::to_string (pstest::dbOf (asym / std::abs (hl[(size_t) lat]))) + " dB");
        CHECK (asym < 1.0e-5 * std::abs (hl[(size_t) lat]));
    }
}

// Measured digital response (delay removed) vs. the product of the bands' target responses.
struct DigitalMeasure
{
    double worstDb = 0.0, worstAt = 0.0, worstPhase = 0.0;
};

DigitalMeasure measureDigital (Rig& rig, double fLow, double fHigh)
{
    REQUIRE (rig.settle());
    const int lat = rig.eq->getLatencySamples();
    const size_t fftN = (size_t) 4 * (size_t) eqdesign::firLength (rig.params().firSizeIndex, rig.sr);
    std::vector<float> hl, hr;
    rig.impulse (fftN, hl, hr);
    const LocalFft fft (fftN);
    const auto H = fft.spectrum (hl);
    const auto p = rig.params();

    DigitalMeasure m;
    const double binHz = rig.sr / (double) fftN;
    for (size_t k = 1; k < fftN / 2; ++k)
    {
        const double f = (double) k * binHz;
        if (f < fLow || f > fHigh)
            continue;
        cd want (1.0, 0.0);
        for (int b = 0; b < kEqBands; ++b)
            want *= eqBandResponse (p.bands[0][b], EqFilterMode::Digital, f, rig.sr, p.amountPercent);
        if (dbOf (want) < -40.0)
            continue;
        const cd got = H[k] * std::polar (1.0, 2.0 * kPi * f * lat / rig.sr);
        const double e = std::abs (dbOf (got) - dbOf (want));
        if (e > m.worstDb)
        {
            m.worstDb = e;
            m.worstAt = f;
        }
        if (dbOf (want) > -20.0)
            m.worstPhase = std::max (m.worstPhase, std::abs (std::arg (got / want)));
    }
    return m;
}

TEST_CASE ("eq. digital realisation matches the target within 0.1 dB above the resolution limit")
{
    // Resolution limit used here: 24 fs / N (281 / 141 / 70 / 35 Hz at 48 kHz for the four sizes), bands with Q <= 2.
    for (int fir = 0; fir < 4; ++fir)
    {
        const int N = eqdesign::firLength (fir, 48000.0);
        const double limit = 24.0 * 48000.0 / N;
        Rig rig (48000.0, 512);
        rig.set ("mode", 1.0f);
        rig.set ("firSize", (float) fir);
        rig.band (0, 0, EqShape::Bell, 4.0 * limit, 6, 1.0);
        rig.band (0, 1, EqShape::LowShelfAnalog, 2.0 * limit, -5, 0.707);
        rig.band (0, 2, EqShape::ProportionalQ, 3000, -9, 2.0);
        rig.band (0, 3, EqShape::HighShelfBaxandall, 9000, 4, 0.707);
        rig.band (0, 4, EqShape::LowPassFlat, 18000, 0, 0.707, EqSlope::Db12);
        rig.band (0, 5, EqShape::BandShelf, 900, 3, 1.0);
        auto m = measureDigital (rig, limit, 20000.0);
        say ("FIR " + std::to_string (N) + " (linear phase): worst " + std::to_string (m.worstDb) + " dB at "
                      + std::to_string (m.worstAt) + " Hz above " + std::to_string (limit) + " Hz, worst phase "
                      + std::to_string (m.worstPhase) + " rad");
        CHECK (m.worstDb < 0.1);
        CHECK (m.worstPhase < 0.02);

        // phase blend: every band at 50 % and 100 % follows the target (incl. its phase) too
        for (double ph : { 50.0, 100.0 })
        {
            for (int b = 0; b < 4; ++b)
                rig.set (bandId (0, b, "phase"), (float) ph);
            m = measureDigital (rig, limit, 20000.0);
            say ("FIR " + std::to_string (N) + " phase " + std::to_string ((int) ph) + " %: worst "
                          + std::to_string (m.worstDb) + " dB at " + std::to_string (m.worstAt) + " Hz, worst phase "
                          + std::to_string (m.worstPhase) + " rad");
            CHECK (m.worstDb < 0.1);
            CHECK (m.worstPhase < 0.02);
        }
    }
}

TEST_CASE ("eq. digital: low-frequency resolution per FIR size (lowest Q 1 / Q 4 bell within 0.1 dB of the target)")
{
    // For each FIR size: the lowest centre frequency (from a 1/3-octave ladder) at which a +6 dB bell still follows its
    // target within 0.1 dB everywhere from f0 / 8 to 20 kHz. Also a digital mode start (prepare, then play) applies
    // the EQ at once, without waiting for a history gate.
    for (int fir = 0; fir < 4; ++fir)
    {
        const int N = eqdesign::firLength (fir, 48000.0);
        std::string line = "FIR " + std::to_string (N) + ": lowest accurate bell";
        for (double q : { 1.0, 4.0 })
        {
            double lowest = 0.0;
            for (double f0 = 1000.0; f0 >= 20.0; f0 /= std::pow (2.0, 1.0 / 3.0))
            {
                Rig rig (48000.0, 512);
                rig.set ("mode", 1.0f);
                rig.set ("firSize", (float) fir);
                rig.band (0, 0, EqShape::Bell, f0, 6, q);
                const auto m = measureDigital (rig, f0 / 8.0, 20000.0);
                if (m.worstDb > 0.1)
                    break;
                lowest = f0;
            }
            line += "  Q " + std::to_string ((int) q) + ": " + std::to_string ((int) std::lround (lowest)) + " Hz";
            CHECK (lowest > 0.0 && lowest <= (q < 2.0 ? 24.0 : 96.0) * 48000.0 / N);
        }
        say (line + " (24 fs / N = " + std::to_string ((int) std::lround (24.0 * 48000.0 / N)) + " Hz)");
    }

    Rig rig (48000.0, 512);
    rig.set ("mode", 1.0f);
    rig.set ("firSize", 3.0f);
    rig.band (0, 0, EqShape::Bell, 1000, 12, 1);
    rig.eq->setParams (rig.values.data());
    pstest::Stopwatch sw;
    while (rig.eq->isDesignPending() && sw.seconds() < 20.0)
    {
        rig.eq->setParams (rig.values.data());
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
        if (! rig.eq->isDesignPending())
            break;
        rig.silence (16);   // the convolver installs the kernel at a partition boundary
    }
    rig.eq->reset();   // e.g. the module joins the chain: playback starts from silence
    const int lat = rig.eq->getLatencySamples();
    auto l = pstest::sine (1000, -20, 1.0, 48000.0), r = l;
    rig.run (l, r);
    const double g = pstest::dbOf (std::abs (pstest::toneOf (l, (size_t) lat + 2400, 4800, 1000, 48000.0))) + 20.0;
    CHECK (std::abs (g - 12.0) < 0.05);
}

TEST_CASE ("eq. digital: Phase 100 % equals the Analog response; Surgical and Brickwall are sharper than Analog")
{
    const double sr = 48000.0;
    EqBandParams b;
    b.on = true;
    b.shape = EqShape::Bell;
    b.freqHz = 2000.0f;
    b.gainDb = 7.0f;
    b.q = 1.5f;
    b.phasePercent = 100.0f;
    double worst = 0.0;
    for (double f : logGrid (20, 20000, 200))
        worst = std::max (worst, std::abs (eqBandResponse (b, EqFilterMode::Digital, f, sr, 100.0f)
                                           - eqBandResponse (b, EqFilterMode::Analog, f, sr, 100.0f)));
    CHECK (worst < 1.0e-9);

    auto att = [&] (EqShape s, EqFilterMode mode, double f0, double f)
    {
        EqBandParams p;
        p.on = true;
        p.shape = s;
        p.freqHz = (float) f0;
        p.gainDb = -20.0f;
        p.q = 4.0f;
        return dbOf (eqBandResponse (p, mode, f, sr, 100.0f));
    };
    // brickwall, a quarter octave beyond the corner
    const double lpA = att (EqShape::LowPassBrickwall, EqFilterMode::Analog, 2000, 2000 * 1.19);
    const double lpD = att (EqShape::LowPassBrickwall, EqFilterMode::Digital, 2000, 2000 * 1.19);
    const double hpD = att (EqShape::HighPassBrickwall, EqFilterMode::Digital, 200, 200 / 1.19);
    CHECK (lpD < lpA - 30.0);
    CHECK (hpD < -60.0);
    CHECK (att (EqShape::LowPassBrickwall, EqFilterMode::Digital, 2000, 1999) > -0.11);
    // surgical: flat top, steeper skirts than the bell of the same Q
    const double sTop = att (EqShape::Surgical, EqFilterMode::Digital, 1000, 1000 * std::pow (2.0, 0.04));
    const double bTop = att (EqShape::Surgical, EqFilterMode::Analog, 1000, 1000 * std::pow (2.0, 0.04));
    const double sSkirt = att (EqShape::Surgical, EqFilterMode::Digital, 1000, 1000 * std::pow (2.0, 0.5));
    const double bSkirt = att (EqShape::Surgical, EqFilterMode::Analog, 1000, 1000 * std::pow (2.0, 0.5));
    say ("sharper in Digital: LP brickwall +1/4 oct analog " + std::to_string (lpA) + " dB, digital "
                  + std::to_string (lpD) + " dB; surgical -20 dB Q 4: +1/25 oct " + std::to_string (sTop) + " vs bell "
                  + std::to_string (bTop) + ", +1/2 oct " + std::to_string (sSkirt) + " vs bell " + std::to_string (bSkirt));
    CHECK (sTop < bTop);
    CHECK (sSkirt > bSkirt);

    // measured: the digital brickwall realisation (FIR 32768) reaches the target's depth
    Rig rig (sr, 512);
    rig.set ("mode", 1.0f);
    rig.set ("firSize", 3.0f);
    rig.band (0, 0, EqShape::LowPassBrickwall, 4000, 0, 1);
    REQUIRE (rig.settle());
    std::vector<float> hl, hr;
    rig.impulse (131072, hl, hr);
    const LocalFft fft (131072);
    const auto H = fft.spectrum (hl);
    const double at3800 = dbOf (H[(size_t) std::lround (3800.0 * 131072 / sr)]);
    const double at4400 = dbOf (H[(size_t) std::lround (4400.0 * 131072 / sr)]);
    const double at6000 = dbOf (H[(size_t) std::lround (6000.0 * 131072 / sr)]);
    say ("measured digital LP brickwall 4 kHz (FIR 32768): 3.8 kHz " + std::to_string (at3800) + " dB, 4.4 kHz "
                  + std::to_string (at4400) + " dB, 6 kHz " + std::to_string (at6000) + " dB");
    CHECK (std::abs (at3800) < 0.1);
    CHECK (at4400 < -60.0);
    CHECK (at6000 < -85.0);
}

//==============================================================================
TEST_CASE ("eq. soft saturation: transparent below -6 dBFS and on cuts, soft-clips boosts, latency reported")
{
    const double sr = 48000.0;
    auto render = [&] (bool sat, double levelDb, double gainDb, std::vector<float>& out, int& lat)
    {
        Rig rig (sr, 512);
        rig.band (0, 0, EqShape::Bell, 1000, gainDb, 0.7);
        rig.set ("softSat", sat ? 1.0f : 0.0f);
        rig.eq->setParams (rig.values.data());
        lat = rig.eq->getLatencySamples();
        out = pstest::sine (1000, levelDb, 1.0, sr);
        auto r = out;
        rig.run (out, r);
    };

    std::vector<float> a, b;
    int la = 0, lb = 0;
    render (false, -20.0, 6.0, a, la);
    render (true, -20.0, 6.0, b, lb);
    CHECK_EQ (la, 0);
    CHECK (lb > 0);
    bool exact = true;
    for (size_t i = 0; i + (size_t) lb < a.size(); ++i)
        exact = exact && b[i + (size_t) lb] == a[i];
    CHECK (exact);   // -14 dBFS out: untouched, just delayed

    render (false, -3.0, -6.0, a, la);
    render (true, -3.0, -6.0, b, lb);
    exact = true;
    for (size_t i = 0; i + (size_t) lb < a.size(); ++i)
        exact = exact && b[i + (size_t) lb] == a[i];
    CHECK (exact);   // hot input but a cut: never saturated

    render (false, -3.0, 9.0, a, la);
    render (true, -3.0, 9.0, b, lb);
    const double pa = pstest::peakOf (a, 24000, 24000), pb = pstest::peakOf (b, 24000, 24000);
    say ("soft saturation: -3 dBFS sine + 9 dB boost peaks at " + std::to_string (pstest::dbOf (pa)) + " dBFS clean, "
                  + std::to_string (pstest::dbOf (pb)) + " dBFS saturated (latency " + std::to_string (lb) + ")");
    CHECK (pb < pa);
    CHECK (pb > pstest::dbfsToAmplitude (-3.0));   // still louder than the dry signal
    CHECK (pb < 1.02);
}

TEST_CASE ("eq. audition band-passes the input around the cursor, on the chosen channel set")
{
    const double sr = 48000.0;
    auto level = [&] (double f, ChannelMode mode, int set, int ch)
    {
        Rig rig (sr, 512);
        rig.channelMode = mode;
        rig.band (0, 0, EqShape::Bell, 300, 12, 1);   // the EQ itself is not heard while auditioning
        rig.monitor.auditionOn = true;
        rig.monitor.auditionHz = 2000.0f;
        rig.monitor.auditionQ = 4.0f;
        rig.monitor.auditionChannelSet = set;
        auto l = pstest::sine (f, -20, 0.5, sr), r = l;
        rig.run (l, r);
        const auto& y = ch == 0 ? l : r;
        return pstest::dbOf (std::abs (pstest::toneOf (y, 12000, 12000, f, sr))) + 20.0;
    };
    CHECK (std::abs (level (2000, ChannelMode::Stereo, 0, 0)) < 0.1);
    CHECK (std::abs (level (2000, ChannelMode::Stereo, 0, 1)) < 0.1);
    CHECK (level (300, ChannelMode::Stereo, 0, 0) < -15.0);
    CHECK (level (8000, ChannelMode::Stereo, 0, 0) < -10.0);
    CHECK (std::abs (level (2000, ChannelMode::MidSide, 1, 1)) < 0.1);
    CHECK (level (2000, ChannelMode::MidSide, 1, 0) < -100.0);
}

TEST_CASE ("eq. audition changing the channel set crossfades without a sample discontinuity")
{
    const double sr = 48000.0, hz = 1000.0, amp = 0.25;
    const size_t switchAt = 24012;   // a positive sine peak, well after the audition fade and filter settling
    const size_t n = switchAt + 2400;
    for (ChannelMode mode : { ChannelMode::MidSide, ChannelMode::LeftRight })
    {
        Rig rig (sr, 256);
        rig.channelMode = mode;
        rig.monitor.auditionOn = true;
        rig.monitor.auditionHz = (float) hz;
        rig.monitor.auditionQ = 4.0f;
        rig.monitor.auditionChannelSet = 0;
        std::vector<float> l (n), r (n);
        for (size_t i = 0; i < n; ++i)
            l[i] = r[i] = (float) (amp * std::sin (2.0 * kPi * hz * (double) i / sr));
        rig.run (l.data(), r.data(), switchAt);
        rig.monitor.auditionChannelSet = 1;
        rig.run (l.data() + switchAt, r.data() + switchAt, n - switchAt);

        const double naturalStep = 2.0 * amp * std::sin (kPi * hz / sr);
        double worstStep = 0.0;
        for (size_t i = switchAt; i < switchAt + 960; ++i)
            worstStep = std::max (worstStep, (double) std::max (std::abs (l[i] - l[i - 1]), std::abs (r[i] - r[i - 1])));
        CHECK (worstStep < 1.2 * naturalStep);
        CHECK (pstest::peakOf (l, switchAt + 960, 960) < 1.0e-6);
        CHECK (std::abs (std::abs (pstest::toneOf (r, switchAt + 960, 960, hz, sr)) - amp) < 1.0e-4);
        say ("audition set change: largest step " + std::to_string (worstStep));
    }
}

TEST_CASE ("eq. soft saturation never distorts phase-shifted cuts")
{
    const double sr = 48000.0;
    for (bool digital : { false, true })
        for (bool dual : { false, true })
            for (EqShape shape : { EqShape::HighPassFlat, EqShape::LowPassFlat, EqShape::Bell })
            {
                const double toneHz = shape == EqShape::LowPassFlat ? 500.0 : (shape == EqShape::Bell ? 3000.0 : 2000.0);
                const double levelDb = shape == EqShape::Bell ? -0.1 : -3.0;
                auto render = [&] (bool sat, int& latency, std::vector<float>& right)
                {
                    Rig rig (sr, 512);
                    rig.channelMode = dual ? ChannelMode::LeftRight : ChannelMode::Stereo;
                    rig.set ("mode", digital ? 1.0f : 0.0f);
                    rig.band (0, 0, shape, 1000.0, shape == EqShape::Bell ? -12.0 : 0.0, 0.707, EqSlope::Db12, 100.0);
                    if (dual)
                        rig.band (1, 0, EqShape::Bell, toneHz, 9.0, 0.707, EqSlope::Db12, 100.0);
                    rig.set ("softSat", sat ? 1.0f : 0.0f);
                    rig.eq->setParams (rig.values.data());
                    latency = rig.eq->getLatencySamples();
                    if (digital)
                    {
                        REQUIRE (rig.settle());
                        rig.eq->reset();   // align the independent FFT partition phases before comparing renders
                    }
                    auto l = pstest::sine (toneHz, levelDb, 0.75, sr);
                    right = l;
                    rig.run (l, right);
                    return l;
                };
                int cleanLatency = 0, saturatedLatency = 0;
                std::vector<float> cleanRight, saturatedRight;
                const auto clean = render (false, cleanLatency, cleanRight), saturated = render (true, saturatedLatency, saturatedRight);
                REQUIRE (saturatedLatency >= cleanLatency);
                const size_t delay = (size_t) (saturatedLatency - cleanLatency);
                double error = 0.0, boostChange = 0.0;
                for (size_t i = 12000; i + delay < clean.size(); ++i)
                {
                    error = std::max (error, (double) std::abs (saturated[i + delay] - clean[i]));
                    boostChange = std::max (boostChange, (double) std::abs (saturatedRight[i + delay] - cleanRight[i]));
                }
                say (std::string ("cut saturation null ") + (digital ? "Digital " : "Analog ") + (dual ? "dual " : "stereo ")
                     + shapeName (shape) + ": " + std::to_string (pstest::dbOf (error)) + " dBFS");
                CHECK (error < 1.0e-7);
                if (dual)
                    CHECK (boostChange > 0.1);   // a boost on the other channel must still saturate
            }
}

TEST_CASE ("eq. mode changes fade instead of clicking")
{
    const double sr = 48000.0;
    Rig rig (sr, 256);
    rig.band (0, 0, EqShape::Bell, 1000, 3, 1);
    auto l = pstest::sine (200, -6, 2.0, sr), r = l;
    double worstJump = 0.0;
    for (size_t i = 0; i < l.size(); i += 256)
    {
        if (i == 256 * 40)
            rig.set ("mode", 1.0f);
        if (i == 256 * 200)
            rig.set ("softSat", 1.0f);
        if (i == 256 * 300)
            rig.set ("mode", 0.0f);
        rig.run (l.data() + i, r.data() + i, 256);
    }
    size_t worstAt = 0;
    for (size_t i = 1; i < l.size(); ++i)
        if (std::abs (l[i] - l[i - 1]) > worstJump)
        {
            worstJump = (double) std::abs (l[i] - l[i - 1]);
            worstAt = i;
        }
    say ("worst step at sample " + std::to_string (worstAt));
    const double sineStep = 2.0 * kPi * 200.0 / sr * pstest::dbfsToAmplitude (-6.0 + 3.0);
    say ("mode switches: largest sample step " + std::to_string (worstJump) + " (sine slope "
                  + std::to_string (sineStep) + ")");
    CHECK (worstJump < 2.0 * sineStep);
}

TEST_CASE ("eq. an inactive FIR-size change leaves Analog audio untouched")
{
    Rig reference (48000.0, 128), changed (48000.0, 128);
    for (Rig* rig : { &reference, &changed })
        rig->band (0, 0, EqShape::Bell, 1000.0, 6.0, 1.0);
    auto expected = pstest::sine (1000.0, -20.0, 0.25, 48000.0), expectedRight = expected;
    auto actual = expected, actualRight = expected;
    constexpr size_t switchAt = 4800;
    reference.run (expected.data(), expectedRight.data(), switchAt);
    changed.run (actual.data(), actualRight.data(), switchAt);
    changed.set ("firSize", 3.0f);
    reference.run (expected.data() + switchAt, expectedRight.data() + switchAt, expected.size() - switchAt);
    changed.run (actual.data() + switchAt, actualRight.data() + switchAt, actual.size() - switchAt);
    double error = 0.0;
    for (size_t i = switchAt; i < actual.size(); ++i)
        error = std::max (error, (double) std::max (std::abs (actual[i] - expected[i]),
                                                  std::abs (actualRight[i] - expectedRight[i])));
    CHECK_EQ (changed.eq->getLatencySamples(), 0);
    CHECK (error < 1.0e-7);
    say ("Analog FIR-size change null: " + std::to_string (pstest::dbOf (error)) + " dBFS");
}

TEST_CASE ("eq. automating an enabled zero-gain band preserves its filter history")
{
    struct Case { double hz, q, gainDb; };
    for (const auto& c : { Case { 20.0, 24.0, -30.0 }, Case { 20.0, 0.1, 15.0 }, Case { 1000.0, 4.0, -30.0 } })
    {
        Rig neutral (48000.0, 64), almostNeutral (48000.0, 64);
        neutral.band (0, 0, EqShape::Bell, c.hz, 0.0, c.q);
        almostNeutral.band (0, 0, EqShape::Bell, c.hz, 0.0001, c.q);
        std::vector<float> input (48000);
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = (float) (0.25 * std::cos (2.0 * kPi * c.hz * (double) i / 48000.0));
        auto warmA = input, warmARight = input, warmB = input, warmBRight = input;
        neutral.run (warmA, warmARight);
        almostNeutral.run (warmB, warmBRight);
        neutral.set (bandId (0, 0, "gain"), (float) c.gainDb);
        almostNeutral.set (bandId (0, 0, "gain"), (float) c.gainDb);
        auto a = input, aRight = input, b = input, bRight = input;
        neutral.run (a, aRight);
        almostNeutral.run (b, bRight);
        double difference = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            difference = std::max (difference, (double) std::abs (a[i] - b[i]));
        say ("zero-gain automation at " + std::to_string (c.hz) + " Hz Q " + std::to_string (c.q)
             + ": difference from +0.0001 dB reference " + std::to_string (difference));
        CHECK (difference < 1.0e-4);
    }
}

TEST_CASE ("eq. enabling sixteen complex bands spreads their designs across control ticks")
{
    Rig rig (48000.0, 64);
    rig.channelMode = ChannelMode::LeftRight;
    std::vector<float> l (64, 1.0f), r (64, 1.0f);
    rig.run (l, r);   // the initial empty EQ is already configured
    for (int set = 0; set < kNumChannelSets; ++set)
        for (int band = 0; band < kEqBands; ++band)
            rig.band (set, band, EqShape::LowPassFlat, 20.0, 0.0, 0.707, EqSlope::Db48);
    std::fill (l.begin(), l.end(), 1.0f);
    std::fill (r.begin(), r.end(), 1.0f);
    rig.run (l, r);
    // Each 48 dB/oct band needs four sections: the 12-section tick budget can start at most three fades.
    // Starting all sixteen at once attenuates both channels at this point to about 0.85 each.
    CHECK ((double) l.back() + r.back() > 1.9);

    l = pstest::sine (1000.0, -20.0, 1.5, 48000.0);
    r = l;
    rig.run (l, r);
    CHECK (pstest::peakOf (l, 60000, 12000) < 1.0e-5);
    CHECK (pstest::peakOf (r, 60000, 12000) < 1.0e-5);
}

TEST_CASE ("eq. digital worker swaps both stereo kernels together during automation")
{
    constexpr double sr = 48000.0, hz = 1000.0;
    constexpr int block = 64;
    Rig rig (sr, block);
    rig.set ("mode", 1.0f);
    rig.set ("firSize", 0.0f);
    rig.band (0, 0, EqShape::Bell, hz, 12.0, 1.0);
    REQUIRE (rig.settle());
    std::vector<float> l ((size_t) block), r ((size_t) block);
    double mismatch = 0.0;
    bool wasPending = false;
    int installs = 0;
    for (int k = 0; k < 150000; ++k)
    {
        if (k % 100 == 0)
            rig.set (bandId (0, 0, "gain"), (k / 100) % 2 == 0 ? -12.0f : 12.0f);
        for (int i = 0; i < block; ++i)
            l[(size_t) i] = r[(size_t) i] = (float) (0.2 * std::sin (2.0 * kPi * hz * (double) (k * block + i) / sr));
        rig.run (l, r);
        for (int i = 0; i < block; ++i)
            mismatch = std::max (mismatch, (double) std::abs (l[(size_t) i] - r[(size_t) i]));
        const bool pending = rig.eq->isDesignPending();
        if (wasPending && ! pending)
            ++installs;
        wasPending = pending;
        if (k % 100 == 0)
            std::this_thread::yield();
    }
    REQUIRE (rig.settle());
    say ("stereo worker swaps: " + std::to_string (installs) + " installs, largest L/R mismatch " + std::to_string (mismatch));
    CHECK (installs >= 3);
    CHECK (mismatch < 1.0e-6);
}

TEST_CASE ("eq. returning to a FIR size waits for its current curve before exposing the residual")
{
    constexpr double sr = 48000.0, hz = 1000.0, amplitude = 0.1;
    constexpr int block = 64;
    Rig rig (sr, block);
    rig.set ("mode", 1.0f);
    auto feedTone = [&]
    {
        auto l = pstest::sine (hz, -20.0, 0.25, sr), r = l;
        rig.run (l, r);
    };
    double peak = 0.0;
    int pendingBlocks = 0;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        rig.set ("firSize", 0.0f);
        rig.band (0, 0, EqShape::Bell, hz, 12.0, 1.0);
        REQUIRE (rig.settle());
        feedTone();
        rig.set ("firSize", 1.0f);
        rig.set (bandId (0, 0, "gain"), -12.0f);
        REQUIRE (rig.settle());
        feedTone();

        rig.set ("firSize", 0.0f);
        rig.allOff();
        std::vector<float> l ((size_t) block), r ((size_t) block);
        for (int k = 0; k < 512; ++k)
        {
            for (int i = 0; i < block; ++i)
                l[(size_t) i] = r[(size_t) i] = (float) (amplitude * std::sin (2.0 * kPi * hz * (double) (k * block + i) / sr));
            rig.run (l, r);   // accelerated rendering crosses the full-history gate before the worker's next poll
            if (rig.eq->isDesignPending())
                ++pendingBlocks;
            for (int i = 0; i < block; ++i)
                peak = std::max (peak, (double) std::max (std::abs (l[(size_t) i]), std::abs (r[(size_t) i])));
        }
        REQUIRE (rig.settle());
    }
    say ("return to FIR size: " + std::to_string (pendingBlocks) + " pending blocks, output peak " + std::to_string (peak));
    CHECK (pendingBlocks > 0);
    CHECK (peak <= amplitude + 1.0e-5);   // neither the previous cut nor the new flat curve can revive the old boost
}

//==============================================================================
TEST_CASE ("eq. no allocation in setParams / process (analog automation, digital while the worker installs kernels)")
{
    Rig rig (48000.0, 512);
    rig.channelMode = ChannelMode::MidSide;
    std::vector<float> l (512), r (512);
    pstest::Lcg lcg (5);
    long long allocs = 0;
    int installs = 0;
    for (int mode = 0; mode < 2; ++mode)
    {
        rig.set ("mode", (float) mode);
        bool wasPending = false;
        for (int k = 0; k < 300; ++k)
        {
            if (mode == 0 || k % 25 == 0)   // digital: a new curve every 25 blocks, so the worker can install it
            for (int s = 0; s < 2; ++s)
                for (int b = 0; b < kEqBands; ++b)
                    rig.band (s, b, (EqShape) ((b + k / 50 + s) % (int) EqShape::Count), 30.0 * std::pow (1.9, b) * (1.0 + 0.01 * (k % 7)),
                              (double) ((k + b) % 12) - 6.0, 0.5 + 0.1 * b, (EqSlope) ((b + k) % 6), (double) ((k * 7) % 100));
            rig.set ("softSat", (k / 100) % 2 ? 1.0f : 0.0f);
            for (size_t i = 0; i < l.size(); ++i)
            {
                l[i] = 0.3f * lcg.next();
                r[i] = 0.3f * lcg.next();
            }
            {
                pstest::AllocationScope scope;
                rig.run (l, r);
                allocs += scope.count();
            }
            const bool pending = rig.eq->isDesignPending();
            if (wasPending && ! pending)
                ++installs;
            wasPending = pending;
            if (mode == 1)
                std::this_thread::sleep_for (std::chrono::milliseconds (2));   // let the worker install kernels
        }
    }
    say ("allocation test: " + std::to_string (installs) + " kernel installs observed while processing");
    CHECK_EQ (allocs, 0LL);
    CHECK (installs > 0);
}

TEST_CASE ("eq. benchmarks (8 bands x 2 sets analog, digital FIR sizes, automation worst case)")
{
    const double sr = 48000.0;
    const int block = 512;
    const double seconds = 10.0;
    const size_t n = (size_t) (seconds * sr);
    pstest::Lcg lcg (11);
    std::vector<float> src (n);
    for (auto& v : src)
        v = 0.25f * lcg.next();

    auto setBands = [] (Rig& rig, int k)
    {
        const EqShape shapes[8] { EqShape::HighPassFlat, EqShape::LowShelfVintage, EqShape::Bell, EqShape::ProportionalQ,
                                  EqShape::BandShelf, EqShape::Bell, EqShape::HighShelfResonant, EqShape::LowPassBrickwall };
        for (int s = 0; s < 2; ++s)
            for (int b = 0; b < kEqBands; ++b)
                rig.band (s, b, shapes[b], 30.0 * std::pow (2.4, b) * (1.0 + 0.002 * k), 4.0 - b + 0.01 * k, 1.0 + 0.2 * b,
                          EqSlope::Db48);
    };

    {
        Rig rig (sr, block);
        rig.channelMode = ChannelMode::MidSide;
        setBands (rig, 0);
        auto l = src, r = src;
        pstest::Stopwatch sw;
        rig.run (l, r);
        const double t = sw.seconds();
        say ("benchmark analog 16 bands (8 x 2 sets, 48 dB HP + elliptic LP), static: " + std::to_string (seconds / t)
                      + " x real time");
        CHECK (seconds / t > 50.0);
    }
    {
        Rig rig (sr, block);
        rig.channelMode = ChannelMode::MidSide;
        auto l = src, r = src;
        pstest::Stopwatch sw;
        for (size_t i = 0; i < n; i += (size_t) block)
        {
            setBands (rig, (int) (i / (size_t) block) % 200);   // every band of both sets moves every block
            rig.run (l.data() + i, r.data() + i, std::min ((size_t) block, n - i));
        }
        const double t = sw.seconds();
        say ("benchmark analog 16 bands all automated every block: " + std::to_string (seconds / t) + " x real time");
        CHECK (seconds / t > 5.0);
    }
    for (int fir = 0; fir < 4; ++fir)
    {
        Rig rig (sr, block);
        rig.channelMode = ChannelMode::MidSide;
        rig.set ("mode", 1.0f);
        rig.set ("firSize", (float) fir);
        setBands (rig, 0);
        REQUIRE (rig.settle());
        auto l = src, r = src;
        pstest::Stopwatch sw;
        rig.run (l, r);
        const double t = sw.seconds();

        // worker design time for both sets
        pstest::Stopwatch dw;
        setBands (rig, 1);
        REQUIRE (rig.settle());
        say ("benchmark digital FIR " + std::to_string (eqdesign::firLength (fir, sr)) + " (M/S, 2 kernels): "
                      + std::to_string (seconds / t) + " x real time; design + install of a new curve "
                      + std::to_string (1000.0 * dw.seconds()) + " ms (incl. settle)");
        CHECK (seconds / t > 20.0);
    }
    {
        pstest::Stopwatch sw;
        EqBandParams b;
        b.on = true;
        b.shape = EqShape::Bell;
        b.gainDb = 6.0f;
        b.q = 1.0f;
        cd acc {};
        int calls = 0;
        for (int k = 0; k < 16; ++k)
        {
            b.freqHz = 100.0f + 900.0f * (float) k;
            for (double f : logGrid (20, 20000, 400))
            {
                acc += eqBandResponse (b, EqFilterMode::Analog, f, sr, 100.0f);
                ++calls;
            }
        }
        say ("eqBandResponse: " + std::to_string (1.0e9 * sw.seconds() / calls) + " ns per point (16 bands x 400 points, cache)"
                      + (std::abs (acc) > 0.0 ? "" : " "));
    }
}
} // namespace
