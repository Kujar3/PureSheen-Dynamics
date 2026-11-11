#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "core/Loudness.h"
#include "dsp/Chain.h"
#include "dsp/Definition.h"
#include "dsp/TransientSplit.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <string>
#include <vector>

// Definition (dsp/Definition.cpp): adaptive spectral contour toward pink + tilt above 300 Hz. Prefix "defn.".
// Test material is synthesized (shaped periodic noise from an inverse FFT, LCG phases; Kellet pink; tones).
namespace
{
using namespace ps;
using pstest::note;
using cd = std::complex<double>;

constexpr double kSr = 48000.0;

std::vector<float> defaultValues()
{
    std::vector<float> v;
    for (const auto& s : paramSpecs())
        v.push_back (s.defaultValue);
    return v;
}

// Fast radix-2 FFT with cached twiddles (double precision, independent of the code under test).
struct TestFft
{
    size_t n = 0;
    std::vector<cd> tw;
    std::vector<size_t> rev;

    explicit TestFft (size_t size) : n (size), tw (size / 2), rev (size)
    {
        for (size_t k = 0; k < n / 2; ++k)
            tw[k] = std::polar (1.0, -2.0 * pstest::kPi * (double) k / (double) n);
        size_t bits = 0;
        while (((size_t) 1 << bits) < n)
            ++bits;
        for (size_t i = 0; i < n; ++i)
        {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b)
                if (i & ((size_t) 1 << b))
                    r |= (size_t) 1 << (bits - 1 - b);
            rev[i] = r;
        }
    }

    void forward (std::vector<cd>& a) const
    {
        for (size_t i = 0; i < n; ++i)
            if (i < rev[i])
                std::swap (a[i], a[rev[i]]);
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const size_t step = n / len;
            for (size_t i = 0; i < n; i += len)
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const cd u = a[i + k], v = a[i + k + len / 2] * tw[k * step];
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
        }
    }
};

// Periodic noise (period 2^18, 20 Hz .. 20 kHz) whose octave density is pink + shapeDb (f); RMS = rms. Tiled.
std::vector<float> shapedNoise (double seconds, double rms, std::uint32_t seed, const std::function<double (double)>& shapeDb)
{
    const size_t m = (size_t) 1 << 18;
    static const TestFft fft (m);
    std::vector<cd> x (m);
    pstest::Lcg lcg (seed);
    for (size_t k = 1; k < m / 2; ++k)
    {
        const double f = (double) k * kSr / (double) m;
        const double mag = (f < 20.0 || f > 20000.0) ? 0.0 : std::pow (10.0, shapeDb (f) / 20.0) / std::sqrt (f);
        const double ph = pstest::kPi * (double) lcg.next();
        x[k] = std::polar (mag, ph);
        x[m - k] = std::conj (x[k]);
    }
    fft.forward (x);   // real input spectrum is Hermitian, so the forward transform gives a real (time-reversed) signal
    double s = 0.0;
    for (auto& c : x)
        s += c.real() * c.real();
    const double g = rms / std::sqrt (s / (double) m);
    const size_t len = (size_t) std::llround (seconds * kSr);
    std::vector<float> out (len);
    for (size_t i = 0; i < len; ++i)
        out[i] = (float) (x[i % m].real() * g);
    return out;
}

double flat (double) { return 0.0; }

// Welch power spectrum (Hann, 8192, 50 % overlap) of y[start, start + len).
struct Psd
{
    static constexpr size_t kN = 8192;
    std::vector<double> p = std::vector<double> (kN / 2 + 1, 0.0);

    Psd (const std::vector<float>& y, size_t start, size_t len)
    {
        static const TestFft fft (kN);
        std::vector<cd> a (kN);
        int segs = 0;
        for (size_t s = start; s + kN <= start + len && s + kN <= y.size(); s += kN / 2)
        {
            for (size_t i = 0; i < kN; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (2.0 * pstest::kPi * (double) i / (double) kN);
                a[i] = (double) y[s + i] * w;
            }
            fft.forward (a);
            for (size_t k = 0; k <= kN / 2; ++k)
                p[k] += std::norm (a[k]);
            ++segs;
        }
        for (auto& v : p)
            v /= std::max (segs, 1);
    }

    double energy (double f1, double f2) const
    {
        double e = 0.0;
        for (size_t k = 1; k <= kN / 2; ++k)
        {
            const double f = (double) k * kSr / (double) kN;
            if (f >= f1 && f < f2)
                e += p[k];
        }
        return e;
    }

    double bandDb (double fc, double octaves = 1.0 / 3.0) const
    {
        const double h = std::pow (2.0, octaves / 2.0);
        return 10.0 * std::log10 (energy (fc / h, fc * h) + 1.0e-30);
    }
};

// 1/3-octave centres 315 Hz .. 16 kHz
std::vector<double> thirdOctaves (double from = 315.0, double to = 16000.0)
{
    std::vector<double> f;
    for (int i = -20; i < 20; ++i)
    {
        const double c = 1000.0 * std::pow (2.0, (double) i / 3.0);
        if (c >= from * 0.999 && c <= to * 1.001)
            f.push_back (c);
    }
    return f;
}

// least-squares slope (dB / octave) of band level vs log2 f
double slopeOf (const Psd& p, double from = 315.0, double to = 16000.0)
{
    const auto f = thirdOctaves (from, to);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (double c : f)
    {
        const double x = std::log2 (c), y = p.bandDb (c);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double n = (double) f.size();
    return (n * sxy - sx * sy) / (n * sxx - sx * sx);
}

struct Rig
{
    Definition d;
    std::vector<float> v = defaultValues();
    ProcessContext ctx;

    explicit Rig (double sr = kSr, int maxBlock = 4096, int paramSet = 0)
    {
        d.setParamSet (paramSet);
        d.prepare (sr, maxBlock);
        ctx.paramSet = paramSet;
    }

    void set (const std::string& id, float value)
    {
        const int i = paramIndex ("defn." + id);
        REQUIRE (i >= 0);
        v[(size_t) i] = value;
    }

    void setBoth (const std::string& name, float value)
    {
        set ("a." + name, value);
        set ("b." + name, value);
    }

    // process in place, setParams every block; perBlock (blockEndSample) after each block
    void run (std::vector<float>& l, std::vector<float>& r, int block,
              const std::function<void (size_t)>& perBlock = {}, const std::function<void (size_t)>& before = {})
    {
        for (size_t pos = 0; pos < l.size(); pos += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, l.size() - pos);
            if (before)
                before (pos);
            d.setParams (v.data());
            float* ch[2] { l.data() + pos, r.data() + pos };
            d.process (ch, n, ctx);
            if (perBlock)
                perBlock (pos + (size_t) n);
        }
    }
};

double maxDiffDelayed (const std::vector<float>& in, const std::vector<float>& out, int lat)
{
    double m = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        const double ref = i >= (size_t) lat ? (double) in[i - (size_t) lat] : 0.0;
        m = std::max (m, std::abs ((double) out[i] - ref));
    }
    return m;
}

std::vector<float> delayed (const std::vector<float>& x, int lat)
{
    std::vector<float> y (x.size(), 0.0f);
    for (size_t i = (size_t) lat; i < x.size(); ++i)
        y[i] = x[i - (size_t) lat];
    return y;
}

// music-like test mix: pink bed + harmonic tones (chord changes) + kick-like bursts, stereo-ish
void musicMix (std::vector<float>& l, std::vector<float>& r, double seconds, double lpHz = 0.0)
{
    l = pstest::pinkNoise (0.25, seconds, kSr, 11u);
    r = pstest::pinkNoise (0.25, seconds, kSr, 12u);
    const double roots[] { 110.0, 146.83, 130.81, 98.0 };
    for (size_t i = 0; i < l.size(); ++i)
    {
        const double t = (double) i / kSr;
        const double root = roots[(size_t) (t / 2.0) % 4];
        double s = 0.0;
        for (int h = 1; h <= 12; ++h)
            s += 0.06 / h * std::sin (2.0 * pstest::kPi * root * h * t + h);
        const double beat = std::fmod (t, 0.5);
        const double kick = 0.5 * std::exp (-beat * 30.0) * std::sin (2.0 * pstest::kPi * (50.0 + 80.0 * std::exp (-beat * 40.0)) * beat);
        l[i] += (float) (s + kick);
        r[i] += (float) (0.9 * s + kick);
    }
    if (lpHz > 0.0)
    {
        const double a = 1.0 - std::exp (-2.0 * pstest::kPi * lpHz / kSr);
        double yl = 0.0, yr = 0.0;
        for (size_t i = 0; i < l.size(); ++i)
        {
            yl += a * ((double) l[i] - yl);
            yr += a * ((double) r[i] - yr);
            l[i] = (float) yl;
            r[i] = (float) yr;
        }
    }
}

double lufsOf (const std::vector<float>& l, const std::vector<float>& r, size_t start, size_t len)
{
    LoudnessMeter m;
    m.prepare (kSr);
    m.process (l.data() + start, r.data() + start, (int) len);
    return m.integratedLufs();
}

int displayIndexNear (double hz)
{
    int best = 0;
    for (int i = 0; i < kDefinitionDisplayPoints; ++i)
        if (std::abs (std::log2 (definitionDisplayHz (i) / hz)) < std::abs (std::log2 (definitionDisplayHz (best) / hz)))
            best = i;
    return best;
}
} // namespace

TEST_CASE ("defn. latency per sample rate, constant under parameter sweeps, equals the impulse delay")
{
    const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const int expected[] { 1024, 1024, 2048, 2048, 4096, 4096 };
    for (int i = 0; i < 6; ++i)
    {
        Rig rig (rates[i], 512);
        CHECK_EQ (rig.d.getLatencySamples(), expected[i]);

        // impulse at Amount 0: exactly one delayed impulse
        std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
        l[100] = 1.0f;
        r[100] = -0.5f;
        rig.run (l, r, 333);
        int at = -1;
        for (size_t k = 0; k < l.size(); ++k)
            if (l[k] != 0.0f)
            {
                CHECK (at < 0);
                at = (int) k;
            }
        CHECK_EQ (at - 100, expected[i]);
        CHECK_EQ (l[(size_t) at], 1.0f);
        CHECK_EQ (r[(size_t) at], -0.5f);
    }

    // randomised sweep of every parameter (both sets), M/S and Stereo: latency never moves
    Rig rig;
    pstest::Lcg lcg (77u);
    auto x = pstest::pinkNoise (0.3, 2.0, kSr, 3u);
    auto y = pstest::pinkNoise (0.3, 2.0, kSr, 4u);
    bool constant = true;
    rig.run (x, y, 480, [&] (size_t) { constant = constant && rig.d.getLatencySamples() == 1024; },
             [&] (size_t pos) {
                 for (const char* s : { "a.", "b." })
                 {
                     const std::string p (s);
                     rig.set (p + "amount", 50.0f + 50.0f * lcg.next());
                     rig.set (p + "tilt", 6.0f * lcg.next());
                     rig.set (p + "att", 5.0f * std::pow (200.0f, 0.5f + 0.5f * lcg.next()));
                     rig.set (p + "rel", 10.0f * std::pow (100.0f, 0.5f + 0.5f * lcg.next()));
                     rig.set (p + "low", 300.0f * std::pow (66.0f, 0.5f + 0.5f * lcg.next()));
                     rig.set (p + "high", 300.0f * std::pow (66.0f, 0.5f + 0.5f * lcg.next()));
                 }
                 rig.ctx.channelMode = (pos / 9600) % 2 ? ChannelMode::MidSide : ChannelMode::Stereo;
             });
    CHECK (constant);
    bool finite = true;
    for (size_t i = 0; i < x.size(); ++i)
        finite = finite && std::isfinite (x[i]) && std::isfinite (y[i]) && std::abs (x[i]) < 4.0f;
    CHECK (finite);
}

TEST_CASE ("defn. Amount 0 is a bit-exact delayed null in every channel mode, tilt, region and block size")
{
    const auto inL = pstest::added (pstest::pinkNoise (0.4, 3.0, kSr, 21u), pstest::sine (1000.0, -12.0, 3.0, kSr));
    const auto inR = pstest::added (pstest::pinkNoise (0.4, 3.0, kSr, 22u), pstest::sine (3100.0, -18.0, 3.0, kSr));
    double worst = 0.0;
    int cases = 0;
    for (int paramSet : { 0, 1 })
        for (auto mode : { ChannelMode::Stereo, ChannelMode::MidSide })
            for (float tilt : { -6.0f, 0.0f, 6.0f })
                for (int block : { 1, 77, 512 })
                {
                    Rig rig (kSr, 512, paramSet);
                    rig.ctx.channelMode = mode;
                    rig.setBoth ("tilt", tilt);
                    rig.setBoth ("low", tilt > 0 ? 1000.0f : 300.0f);
                    rig.setBoth ("high", tilt < 0 ? 4000.0f : 20000.0f);
                    rig.setBoth ("att", 5.0f);
                    auto l = inL, r = inR;
                    rig.run (l, r, block);
                    worst = std::max (worst, std::max (maxDiffDelayed (inL, l, 1024), maxDiffDelayed (inR, r, 1024)));
                    ++cases;
                }
    note ("cases", cases);
    note ("max |out - in delayed| over every case", worst);
    CHECK_EQ (worst, 0.0);

    // from Amount 100 back to 0: the curve releases, snaps to unity and the output is bit-exact again
    Rig rig;
    rig.setBoth ("amount", 100.0f);
    rig.setBoth ("tilt", 3.0f);
    auto l = pstest::pinkNoise (0.4, 4.0, kSr, 5u), r = pstest::pinkNoise (0.4, 4.0, kSr, 6u);
    const auto l0 = l, r0 = r;
    rig.run (l, r, 512, {}, [&] (size_t pos) { if (pos >= (size_t) (1.5 * kSr)) rig.setBoth ("amount", 0.0f); });
    double tail = 0.0, active = 0.0;
    for (size_t i = (size_t) (3.0 * kSr); i < l.size(); ++i)
        tail = std::max (tail, std::abs ((double) l[i] - l0[i - 1024]) + std::abs ((double) r[i] - r0[i - 1024]));
    for (size_t i = (size_t) (0.5 * kSr); i < (size_t) (1.5 * kSr); ++i)
        active = std::max (active, std::abs ((double) l[i] - l0[i - 1024]));
    note ("max diff while active (Amount 100)", active);
    note ("max diff 1.5 s after Amount -> 0", tail);
    CHECK (active > 1.0e-3);
    CHECK_EQ (tail, 0.0);
}

TEST_CASE ("defn. pink is left alone at tilt 0")
{
    Rig rig;
    rig.setBoth ("amount", 100.0f);
    auto l = shapedNoise (10.0, 0.2, 31u, flat), r = shapedNoise (10.0, 0.2, 32u, flat);
    const auto inL = delayed (l, 1024);
    rig.run (l, r, 512);
    const size_t s = (size_t) (3.0 * kSr), n = (size_t) (7.0 * kSr);
    const Psd pin (inL, s, n), pout (l, s, n);
    double worst = 0.0;
    for (double fc : thirdOctaves (315.0, 16000.0))
        worst = std::max (worst, std::abs (pout.bandDb (fc) - pin.bandDb (fc)));
    note ("max |1/3-oct change| 315 Hz..16 kHz (dB)", worst);
    CHECK (worst < 0.75);
    const auto& m = rig.d.getMeters();
    note ("meter max boost", m.maxBoostDb[0].load());
    note ("meter max cut", m.maxCutDb[0].load());
}

TEST_CASE ("defn. moves white toward pink, and the tilt sets the target")
{
    {
        Rig rig;
        rig.setBoth ("amount", 100.0f);
        auto l = shapedNoise (10.0, 0.15, 41u, [] (double f) { return 10.0 * std::log10 (f / 1000.0); });   // white
        auto r = shapedNoise (10.0, 0.15, 42u, [] (double f) { return 10.0 * std::log10 (f / 1000.0); });
        const auto in = delayed (l, 1024);
        rig.run (l, r, 512);
        const size_t s = (size_t) (3.0 * kSr), n = (size_t) (7.0 * kSr);
        const double before = slopeOf (Psd (in, s, n)), after = slopeOf (Psd (l, s, n));
        note ("white input slope (dB/oct, 1/3-oct levels)", before);
        note ("output slope", after);
        CHECK (before - after >= 1.5);
        CHECK (after > 0.0);
    }
    for (float tilt : { 3.0f, -3.0f })
    {
        Rig rig;
        rig.setBoth ("amount", 100.0f);
        rig.setBoth ("tilt", tilt);
        auto l = shapedNoise (10.0, 0.2, 43u, flat), r = shapedNoise (10.0, 0.2, 44u, flat);
        const auto in = delayed (l, 1024);
        rig.run (l, r, 512);
        const size_t s = (size_t) (3.0 * kSr), n = (size_t) (7.0 * kSr);
        const double before = slopeOf (Psd (in, s, n)), after = slopeOf (Psd (l, s, n));
        note (tilt > 0 ? "pink, tilt +3: output slope" : "pink, tilt -3: output slope", after);
        note ("  (input slope)", before);
        if (tilt > 0)
            CHECK (after > 1.0);
        else
            CHECK (after < -1.0);
    }
}

TEST_CASE ("defn. dull mix: highs rise by a documented amount, loudness stays within 0.5 LU")
{
    std::vector<float> l, r;
    musicMix (l, r, 12.0, 3000.0);   // one-pole low-pass at 3 kHz: a dull, dark mix
    const auto inL = delayed (l, 1024), inR = delayed (r, 1024);
    double rise50 = 0.0, rise100 = 0.0;
    for (float amount : { 50.0f, 100.0f })
    {
        Rig rig;
        rig.setBoth ("amount", amount);
        auto ol = l, orr = r;
        rig.run (ol, orr, 512);
        const size_t s = (size_t) (3.0 * kSr), n = (size_t) (9.0 * kSr);
        const Psd pin (inL, s, n), pout (ol, s, n);
        const double hf = 10.0 * std::log10 (pout.energy (6000.0, 16000.0) / pin.energy (6000.0, 16000.0));
        const double air = 10.0 * std::log10 (pout.energy (10000.0, 16000.0) / pin.energy (10000.0, 16000.0));
        const double mid = 10.0 * std::log10 (pout.energy (300.0, 2000.0) / pin.energy (300.0, 2000.0));
        const double low = 10.0 * std::log10 (pout.energy (20.0, 200.0) / pin.energy (20.0, 200.0));
        const double dl = lufsOf (ol, orr, s, n) - lufsOf (inL, inR, s, n);
        note (amount > 60 ? "Amount 100: 6-16 kHz energy change (dB)" : "Amount 50: 6-16 kHz energy change (dB)", hf);
        note ("  10-16 kHz change (dB)", air);
        note ("  300 Hz - 2 kHz change (dB)", mid);
        note ("  below 200 Hz change (dB)", low);
        note ("  integrated loudness change (LU)", dl);
        CHECK (std::abs (dl) < 0.5);
        CHECK (std::abs (low) < 0.1);
        if (amount > 60)
        {
            rise100 = hf;
            CHECK (hf > 3.0);
            CHECK (mid < 0.0);
        }
        else
        {
            rise50 = hf;
            CHECK (hf > 1.5);
        }
    }
    note ("Amount 50 / Amount 100 high-frequency lift ratio", rise50 / rise100);
    CHECK (rise50 < rise100);
    CHECK_RANGE (rise50 / rise100, 0.4, 0.65);
}

TEST_CASE ("defn. loudness neutral on a music-like mix")
{
    std::vector<float> l, r;
    musicMix (l, r, 12.0);
    const auto inL = delayed (l, 1024), inR = delayed (r, 1024);
    for (float tilt : { 0.0f, 3.0f, -3.0f, 6.0f })
    {
        Rig rig;
        rig.setBoth ("amount", 100.0f);
        rig.setBoth ("tilt", tilt);
        auto ol = l, orr = r;
        rig.run (ol, orr, 512);
        const size_t s = (size_t) (2.0 * kSr), n = (size_t) (10.0 * kSr);
        const double dl = lufsOf (ol, orr, s, n) - lufsOf (inL, inR, s, n);
        note ("tilt " + std::to_string ((int) tilt) + ": dLUFS integrated", dl);
        CHECK (std::abs (dl) < 0.5);
    }
}

TEST_CASE ("defn. gain limits: never more than 6 dB (Amount 100) / 3 dB (Amount 50) per band")
{
    // pink with a -30 dB hole (2..3 kHz) and a +30 dB peak (7..8 kHz)
    auto shape = [] (double f) { return (f > 2000.0 && f < 3000.0) ? -30.0 : ((f > 7000.0 && f < 8000.0) ? 30.0 : 0.0); };
    for (float amount : { 100.0f, 50.0f })
    {
        Rig rig;
        rig.setBoth ("amount", amount);
        rig.setBoth ("att", 5.0f);
        rig.setBoth ("rel", 10.0f);
        auto l = shapedNoise (8.0, 0.2, 51u, shape), r = shapedNoise (8.0, 0.2, 52u, shape);
        const auto in = delayed (l, 1024);
        double maxB = 0.0, maxC = 0.0, maxPoint = 0.0;
        rig.run (l, r, 256, [&] (size_t) {
            const auto& m = rig.d.getMeters();
            maxB = std::max (maxB, (double) m.maxBoostDb[0].load());
            maxC = std::min (maxC, (double) m.maxCutDb[0].load());
            for (int i = 0; i < kDefinitionDisplayPoints; ++i)
                maxPoint = std::max (maxPoint, std::abs ((double) m.gainDb[0][i].load()));
        });
        const size_t s = (size_t) (3.0 * kSr), n = (size_t) (5.0 * kSr);
        const Psd pin (in, s, n), pout (l, s, n);
        double measured = 0.0;
        for (double fc : thirdOctaves (315.0, 16000.0))
            measured = std::max (measured, std::abs (pout.bandDb (fc, 1.0 / 6.0) - pin.bandDb (fc, 1.0 / 6.0)));
        const double lim = definitionMaxGainDb (amount / 100.0) + 0.05;
        note (amount > 60 ? "Amount 100: max boost (meter, every hop)" : "Amount 50: max boost (meter, every hop)", maxB);
        note ("  max cut (meter)", maxC);
        note ("  max |change| measured in 1/6-oct bands of the audio", measured);
        CHECK (maxB <= lim);
        CHECK (-maxC <= lim);
        CHECK (maxPoint <= lim);
        CHECK (measured <= lim + 0.1);
        CHECK (maxB > lim - 0.5);   // the extreme input does drive the limit
        // the cut of the +30 dB peak is held back by the loudness shift (bounded at 3 dB x Amount... see .cpp)
        CHECK (-maxC > 0.4 * lim);
    }
}

TEST_CASE ("defn. region: nothing outside, unity bins below 238 Hz")
{
    // white noise drives big corrections everywhere; region 1 kHz .. 4 kHz
    auto white = [] (double f) { return 10.0 * std::log10 (f / 1000.0); };
    Rig rig;
    rig.setBoth ("amount", 100.0f);
    rig.setBoth ("tilt", -6.0f);
    rig.setBoth ("low", 1000.0f);
    rig.setBoth ("high", 4000.0f);
    auto l = shapedNoise (8.0, 0.2, 61u, white), r = shapedNoise (8.0, 0.2, 62u, white);
    const auto in = delayed (l, 1024);
    double outsideMeter = 0.0, insideMeter = 0.0;
    rig.run (l, r, 512, [&] (size_t pos) {
        if (pos < (size_t) kSr)
            return;
        const auto& m = rig.d.getMeters();
        for (int i = 0; i < kDefinitionDisplayPoints; ++i)
        {
            const double hz = definitionDisplayHz (i), g = std::abs ((double) m.gainDb[0][i].load());
            if (hz < 700.0 || hz > 5600.0)
                outsideMeter = std::max (outsideMeter, g);
            if (hz > 1200.0 && hz < 3500.0)
                insideMeter = std::max (insideMeter, g);
        }
    });
    const size_t s = (size_t) (3.0 * kSr), n = (size_t) (5.0 * kSr);
    const Psd pin (in, s, n), pout (l, s, n);
    double outside = 0.0;
    for (double fc : thirdOctaves (315.0, 16000.0))
        if (fc * std::pow (2.0, 1.0 / 6.0) < 700.0 || fc / std::pow (2.0, 1.0 / 6.0) > 5600.0)
            outside = std::max (outside, std::abs (pout.bandDb (fc) - pin.bandDb (fc)));
    note ("max |gain| outside 700 Hz..5.6 kHz (meter)", outsideMeter);
    note ("max |change| outside, measured 1/3-oct", outside);
    note ("max |gain| inside (meter)", insideMeter);
    CHECK (outsideMeter < 0.2);
    CHECK (outside < 0.2);
    CHECK (insideMeter > 2.0);

    // default region (300 Hz up), extreme settings: tones at 60, 120 and 200 Hz pass bit-identically in level
    for (float tilt : { -6.0f, 6.0f })
    {
        Rig low;
        low.setBoth ("amount", 100.0f);
        low.setBoth ("tilt", tilt);
        low.setBoth ("att", 5.0f);
        std::vector<float> a = shapedNoise (6.0, 0.1, 63u, white), b = shapedNoise (6.0, 0.1, 64u, white);
        for (size_t i = 0; i < a.size(); ++i)
        {
            const double t = (double) i / kSr;
            const float tones = (float) (0.1 * std::sin (2 * pstest::kPi * 60 * t) + 0.1 * std::sin (2 * pstest::kPi * 120 * t)
                                         + 0.1 * std::sin (2 * pstest::kPi * 200 * t));
            a[i] += tones;
            b[i] += tones;
        }
        const auto ain = delayed (a, 1024);
        low.run (a, b, 512);
        const size_t s0 = (size_t) (2.0 * kSr), n0 = (size_t) (4.0 * kSr);
        double worst = 0.0;
        for (double f : { 60.0, 120.0, 200.0 })
        {
            const double gi = std::abs (pstest::toneOf (ain, s0, n0, f, kSr)), go = std::abs (pstest::toneOf (a, s0, n0, f, kSr));
            worst = std::max (worst, std::abs (20.0 * std::log10 (go / gi)));
        }
        note (tilt > 0 ? "tilt +6: max level change of 60/120/200 Hz tones (dB)" : "tilt -6: max level change of 60/120/200 Hz tones (dB)", worst);
        CHECK (worst < 0.02);
        // the meters agree: nothing applied at the lowest display point (250 Hz is inside the 238..300 Hz fade)
        const auto& m = low.d.getMeters();
        note ("  meter gain at 250 Hz", m.gainDb[0][0].load());
        CHECK (std::abs (m.gainDb[0][0].load()) < 3.5f);
    }
}

TEST_CASE ("defn. attack and release times (2 kHz band noise steps +12 dB)")
{
    // pink bed + 1/3-octave band noise at 2 kHz (about the bed's own level there) that steps up by 12 dB at 5 s and
    // back at 9 s. The 2 kHz meter point is read after every hop and stamped with the frame centre (output-aligned
    // time); the timelines of 4 noise seeds are averaged so the 63 % crossings are not decided by detector noise.
    auto band = [] (double f) { return (f > 1780.0 && f < 2245.0) ? 0.0 : -200.0; };
    const size_t up = (size_t) (5.0 * kSr), down = (size_t) (9.0 * kSr);
    const int idx = displayIndexNear (2000.0);
    const int hop = 256;
    constexpr int kSeeds = 4;
    std::vector<float> beds[kSeeds][2], bands[kSeeds];
    for (int k = 0; k < kSeeds; ++k)
    {
        beds[k][0] = shapedNoise (12.0, 0.1, 71u + 10u * (std::uint32_t) k, flat);
        beds[k][1] = shapedNoise (12.0, 0.1, 72u + 10u * (std::uint32_t) k, flat);
        bands[k] = shapedNoise (12.0, 0.1 * std::sqrt ((1.0 / 3.0) / 9.966), 73u + 10u * (std::uint32_t) k, band);
    }

    struct Case { float att, rel; };
    for (Case c : { Case { 50.0f, 50.0f }, Case { 150.0f, 150.0f }, Case { 500.0f, 500.0f }, Case { 5.0f, 300.0f },
                    Case { 20.0f, 1000.0f } })
    {
        std::vector<double> t, g;
        for (int k = 0; k < kSeeds; ++k)
        {
            Rig rig;
            rig.setBoth ("amount", 100.0f);
            rig.setBoth ("att", c.att);
            rig.setBoth ("rel", c.rel);
            std::vector<float> l (beds[k][0].size()), r (l.size());
            for (size_t i = 0; i < l.size(); ++i)
            {
                const float gs = (i >= up && i < down) ? 4.0f : 1.0f;   // +12 dB
                l[i] = beds[k][0][i] + gs * bands[k][i];
                r[i] = beds[k][1][i] + gs * bands[k][i];
            }
            size_t frame = 0;
            rig.run (l, r, hop, [&] (size_t end) {
                // the frame that just completed ended at input time `end`; its centre is end - N / 2
                if (k == 0)
                {
                    t.push_back ((double) end - 512.0);
                    g.push_back (0.0);
                }
                g[frame++] += (double) rig.d.getMeters().gainDb[0][idx].load() / kSeeds;
            });
        }
        auto avg = [&] (double from, double to) {
            double s = 0; int n = 0;
            for (size_t i = 0; i < t.size(); ++i)
                if (t[i] >= from * kSr && t[i] < to * kSr) { s += g[i]; ++n; }
            return s / std::max (n, 1);
        };
        const double g0 = avg (4.0, 5.0), g1 = avg (8.0, 9.0), g2 = avg (11.0, 12.0);
        auto cross = [&] (double from, double a, double b) {
            const double level = a + 0.632 * (b - a);
            for (size_t i = 0; i < t.size(); ++i)
                if (t[i] >= from && ((b < a) ? g[i] <= level : g[i] >= level))
                    return (t[i] - from) / kSr * 1000.0;
            return 1.0e9;
        };
        const double ta = cross ((double) up, g0, g1), tr = cross ((double) down, g1, g2);
        note ("attack " + std::to_string ((int) c.att) + " ms / release " + std::to_string ((int) c.rel) + " ms: gain before (dB)", g0);
        note ("  gain during the step (dB)", g1);
        note ("  measured attack (ms, 63 %)", ta);
        note ("  measured release (ms, 63 %)", tr);
        CHECK (std::abs (g2 - g0) < 0.2);
        if (c.att == c.rel)
        {
            CHECK (g1 < g0 - 2.0);
            // +-10 %, but never tighter than 1.5 hops (the gain is measured once per hop)
            const double tol = std::max (0.1 * c.att, 1.5 * hop / kSr * 1000.0);
            CHECK_RANGE (ta, c.att - tol, c.att + tol);
            CHECK_RANGE (tr, c.rel - tol, c.rel + tol);
        }
        else
        {
            // fast attack / slow release: the attack also catches the detector's noise dips, so the resting cut is
            // deeper (peak-hold bias) and the release lands on it a little sooner
            CHECK (g1 < g0 - 1.5);
            if (c.att < 10.0f)
                CHECK (ta <= 2.0 * hop / kSr * 1000.0 + 0.01);
            else
                CHECK_RANGE (ta, 0.7 * c.att, 1.3 * c.att);
            CHECK_RANGE (tr, 0.5 * c.rel, 1.3 * c.rel);
        }
    }
}

TEST_CASE ("defn. stereo link, M/S sets and the T/S twin")
{
    // Stereo: L carries pink + a +15 dB 3 kHz band, R a quiet flat probe. R gets L's cut (one linked curve).
    auto peak = [] (double f) { return (f > 2670.0 && f < 3370.0) ? 15.0 : 0.0; };
    {
        Rig rig;
        rig.setBoth ("amount", 100.0f);
        auto l = shapedNoise (8.0, 0.2, 81u, peak), r = shapedNoise (8.0, 0.002, 82u, flat);
        const auto inL = delayed (l, 1024), inR = delayed (r, 1024);
        rig.run (l, r, 512);
        const size_t s = (size_t) (3.0 * kSr), n = (size_t) (5.0 * kSr);
        const Psd pl (inL, s, n), ql (l, s, n), pr (inR, s, n), qr (r, s, n);
        const double gl = ql.bandDb (3000.0) - pl.bandDb (3000.0), gr = qr.bandDb (3000.0) - pr.bandDb (3000.0);
        note ("Stereo: change at 3 kHz, L (loud band)", gl);
        note ("Stereo: change at 3 kHz, R (quiet flat probe)", gr);
        CHECK (gl < -1.5);
        CHECK (std::abs (gl - gr) < 0.15);
    }
    // M/S: set B Amount 0 -> channel 1 (Side) is a bit-exact delayed null; set A processes Mid, and the reverse
    for (int processed : { 0, 1 })
    {
        Rig rig;
        rig.ctx.channelMode = ChannelMode::MidSide;
        rig.set (processed == 0 ? "a.amount" : "b.amount", 100.0f);
        rig.set (processed == 0 ? "a.tilt" : "b.tilt", 4.0f);
        auto l = shapedNoise (4.0, 0.2, 83u, flat), r = shapedNoise (4.0, 0.1, 84u, flat);
        const auto inL = l, inR = r;
        rig.run (l, r, 512);
        const double d0 = maxDiffDelayed (inL, l, 1024), d1 = maxDiffDelayed (inR, r, 1024);
        note (processed == 0 ? "M/S set A only: max diff ch0 (processed)" : "M/S set B only: max diff ch0 (null)", d0);
        note ("  max diff ch1", d1);
        // the unprocessed channel: the engine's packed two-channel IFFT leaks float rounding (< -140 dBFS)
        CHECK ((processed == 0 ? d1 : d0) < 1.0e-7);
        CHECK ((processed == 0 ? d0 : d1) > 1.0e-3);
        const auto& m = rig.d.getMeters();
        note ("  meter max boost, set A", m.maxBoostDb[0].load());
        note ("  meter max boost, set B", m.maxBoostDb[1].load());
        CHECK ((processed == 0 ? m.maxBoostDb[1].load() : m.maxBoostDb[0].load()) == 0.0f);
        CHECK ((processed == 0 ? m.maxBoostDb[0].load() : m.maxBoostDb[1].load()) > 0.5f);
    }
    // T/S twin (paramSet 1) reads set B for both channels: bit-identical to a normal instance given set B as set A
    {
        Rig twin (kSr, 512, 1), ref;
        twin.set ("a.amount", 0.0f);
        twin.set ("b.amount", 80.0f);
        twin.set ("b.tilt", 2.5f);
        twin.set ("b.att", 40.0f);
        twin.set ("b.low", 800.0f);
        ref.set ("a.amount", 80.0f);
        ref.set ("a.tilt", 2.5f);
        ref.set ("a.att", 40.0f);
        ref.set ("a.low", 800.0f);
        ref.set ("b.amount", 0.0f);
        auto l = shapedNoise (4.0, 0.2, 85u, flat), r = shapedNoise (4.0, 0.2, 86u, flat);
        auto l2 = l, r2 = r;
        const auto in = l;
        twin.run (l, r, 512);
        ref.run (l2, r2, 512);
        double d = 0.0;
        for (size_t i = 0; i < l.size(); ++i)
            d = std::max (d, (double) std::abs (l[i] - l2[i]) + std::abs (r[i] - r2[i]));
        note ("twin vs reference (set B as set A): max diff", d);
        note ("twin: max diff vs delayed input (it processes)", maxDiffDelayed (in, l, 1024));
        CHECK_EQ (d, 0.0);
        CHECK (maxDiffDelayed (in, l, 1024) > 1.0e-3);
        CHECK (twin.d.getMeters().maxBoostDb[1].load() == twin.d.getMeters().maxBoostDb[0].load());

        // twin with set B at Amount 0 (set A at 100): exact null
        Rig quiet (kSr, 512, 1);
        quiet.set ("a.amount", 100.0f);
        auto q = shapedNoise (3.0, 0.2, 87u, flat), qr = shapedNoise (3.0, 0.2, 88u, flat);
        const auto q0 = q;
        quiet.run (q, qr, 512);
        CHECK_EQ (maxDiffDelayed (q0, q, 1024), 0.0);
    }
}

TEST_CASE ("defn. no musical noise on steady noise")
{
    struct Case { const char* what; float att, rel; double limit; };
    for (Case c : { Case { "defaults (att 150, rel 120)", 150.0f, 120.0f, 0.5 }, Case { "fast (att 20, rel 60)", 20.0f, 60.0f, 0.5 } })
    {
        Rig rig;
        rig.setBoth ("amount", 100.0f);
        rig.setBoth ("att", c.att);
        rig.setBoth ("rel", c.rel);
        // steady pink + white mix (white makes the module work hard everywhere)
        auto mixShape = [] (double f) { return 10.0 * std::log10 (1.0 + 0.5 * f / 1000.0); };
        auto l = shapedNoise (8.0, 0.2, 91u, mixShape), r = shapedNoise (8.0, 0.2, 92u, mixShape);
        const auto in = delayed (l, 1024);
        std::vector<double> sum (kDefinitionDisplayPoints, 0.0), sum2 (kDefinitionDisplayPoints, 0.0);
        int frames = 0;
        rig.run (l, r, 256, [&] (size_t end) {
            if (end < (size_t) (4.0 * kSr))
                return;
            const auto& m = rig.d.getMeters();
            for (int i = 0; i < kDefinitionDisplayPoints; ++i)
            {
                const double g = m.gainDb[0][i].load();
                sum[(size_t) i] += g;
                sum2[(size_t) i] += g * g;
            }
            ++frames;
        });
        double worstStd = 0.0, meanAbs = 0.0;
        for (int i = 0; i < kDefinitionDisplayPoints; ++i)
        {
            const double mu = sum[(size_t) i] / frames;
            worstStd = std::max (worstStd, std::sqrt (std::max (0.0, sum2[(size_t) i] / frames - mu * mu)));
            meanAbs = std::max (meanAbs, std::abs (mu));
        }
        // spectral flux (mean |dB change| per bin between 1024-sample frames, hop 256), 300 Hz .. 16 kHz
        auto flux = [] (const std::vector<float>& y, size_t start, size_t len) {
            const size_t n = 1024;
            static const TestFft fft (n);
            std::vector<cd> a (n);
            std::vector<double> prev (n / 2 + 1, 0.0);
            double acc = 0.0;
            int count = 0;
            for (size_t s = start; s + n <= start + len; s += 256)
            {
                for (size_t i = 0; i < n; ++i)
                    a[i] = (double) y[s + i] * (0.5 - 0.5 * std::cos (2 * pstest::kPi * (double) i / (double) n));
                fft.forward (a);
                for (size_t k = 7; k <= 341; ++k)
                {
                    const double db = 10.0 * std::log10 (std::norm (a[k]) + 1.0e-20);
                    if (s > start)
                        acc += std::abs (db - prev[k]);
                    prev[k] = db;
                }
                if (s > start)
                    ++count;
            }
            return acc / std::max (count, 1);
        };
        const double fin = flux (in, (size_t) (4.0 * kSr), (size_t) (3.0 * kSr));
        const double fout = flux (l, (size_t) (4.0 * kSr), (size_t) (3.0 * kSr));
        note (std::string (c.what) + ": worst per-point gain std over 4 s (dB)", worstStd);
        note ("  largest mean correction (dB)", meanAbs);
        note ("  spectral flux out / in", fout / fin);
        CHECK (worstStd < c.limit);
        CHECK (meanAbs > 1.0);
        CHECK (fout / fin <= 1.1);
    }
}

TEST_CASE ("defn. fast automation is smooth (no clicks, no HF splatter), region solo")
{
    // two steady tones (+ optionally a low pink bed so every band has signal); every control moves as fast as a
    // host could automate it (amount 0 <-> 100, tilt -6 <-> +6, low 300 <-> 2500 Hz, high 4 k <-> 20 kHz, region solo)
    for (bool bed : { false, true })
    {
        std::vector<float> l = pstest::added (pstest::sine (1000.0, -12.0, 6.0, kSr), pstest::sine (3100.0, -18.0, 6.0, kSr));
        if (bed)
            l = pstest::added (l, pstest::pinkNoise (0.002, 6.0, kSr, 99u));
        std::vector<float> r = l;
        const auto in = delayed (l, 1024);
        Rig rig;
        rig.setBoth ("att", 5.0f);
        rig.setBoth ("rel", 10.0f);
        long long blocks = 0;
        rig.run (l, r, 64, {}, [&] (size_t) {
            ++blocks;
            rig.setBoth ("amount", (blocks / 37) % 2 ? 100.0f : 0.0f);
            rig.setBoth ("tilt", (blocks / 23) % 2 ? 6.0f : -6.0f);
            rig.setBoth ("low", (blocks / 53) % 2 ? 2500.0f : 300.0f);
            rig.setBoth ("high", (blocks / 41) % 2 ? 4000.0f : 20000.0f);
            ModuleMonitor m;
            m.soloRegion = (blocks / 300) % 2 == 1;
            rig.d.setMonitor (m);
        });
        const size_t s = (size_t) (1.0 * kSr), n = (size_t) (5.0 * kSr);
        const Psd pin (in, s, n), pout (l, s, n);
        const double tone = pin.energy (900.0, 1100.0);
        const double far = 10.0 * std::log10 (pout.energy (8000.0, 20000.0) / tone);
        const double farIn = 10.0 * std::log10 (pin.energy (8000.0, 20000.0) / tone);
        const double near = 10.0 * std::log10 (pout.energy (1300.0, 2700.0) / tone);
        // largest second difference (click detector) relative to the input's
        auto d2 = [] (const std::vector<float>& y, size_t a, size_t b) {
            double m = 0.0;
            for (size_t i = a + 2; i < b; ++i)
                m = std::max (m, std::abs ((double) y[i] - 2.0 * y[i - 1] + y[i - 2]));
            return m;
        };
        const double clicks = d2 (l, s, s + n) / d2 (in, s, s + n);
        if (! bed)
        {
            note ("tones only, automation: 8-20 kHz energy re the 1 kHz tone (dB)", far);
            note ("  1.3-2.7 kHz (between the tones) re the tone (dB)", near);
            note ("  max 2nd difference out / in", clicks);
            CHECK (far < -100.0);
            CHECK (near < -60.0);
        }
        else
        {
            note ("tones + pink bed, automation: 8-20 kHz out re tone (dB)", far);
            note ("  input (dB)", farIn);
            note ("  max 2nd difference out / in", clicks);
            CHECK (far < farIn + 6.5);
        }
        CHECK (clicks < 1.5);
    }

    // region solo: only the input inside [1 kHz, 4 kHz] is heard, same latency
    {
        Rig solo;
        solo.setBoth ("low", 1000.0f);
        solo.setBoth ("high", 4000.0f);
        ModuleMonitor m;
        m.soloRegion = true;
        solo.d.setMonitor (m);
        std::vector<float> a (3 * (size_t) kSr), b;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const double t = (double) i / kSr;
            a[i] = (float) (0.2 * std::sin (2 * pstest::kPi * 312.5 * t) + 0.2 * std::sin (2 * pstest::kPi * 2000.0 * t)
                            + 0.2 * std::sin (2 * pstest::kPi * 10000.0 * t));
        }
        b = a;
        const auto ain = delayed (a, 1024);
        solo.run (a, b, 512);
        const size_t s0 = (size_t) kSr, n0 = (size_t) kSr;
        auto lvl = [&] (const std::vector<float>& y, double f) { return 20.0 * std::log10 (std::abs (pstest::toneOf (y, s0, n0, f, kSr)) + 1e-12); };
        const double in2 = lvl (ain, 2000.0), out2 = lvl (a, 2000.0);
        const double out312 = lvl (a, 312.5) - lvl (ain, 312.5), out10k = lvl (a, 10000.0) - lvl (ain, 10000.0);
        note ("solo region 1-4 kHz: 2 kHz tone change (dB)", out2 - in2);
        note ("  312.5 Hz tone (dB)", out312);
        note ("  10 kHz tone (dB)", out10k);
        // phase / latency: the 2 kHz tone is the delayed input
        const auto ci = pstest::toneOf (ain, s0, n0, 2000.0, kSr), co = pstest::toneOf (a, s0, n0, 2000.0, kSr);
        note ("  2 kHz phase difference (rad)", std::arg (co / ci));
        CHECK (std::abs (out2 - in2) < 0.05);
        CHECK (out312 < -60.0);
        CHECK (out10k < -60.0);
        CHECK (std::abs (std::arg (co / ci)) < 1.0e-3);
    }
}

TEST_CASE ("defn. meters match the audio")
{
    auto dull = [] (double f) { return -10.0 * std::log10 (1.0 + (f / 2500.0) * (f / 2500.0)); };   // -6 dB/oct above 2.5 kHz
    Rig rig;
    rig.setBoth ("amount", 100.0f);
    auto l = shapedNoise (10.0, 0.2, 101u, dull), r = shapedNoise (10.0, 0.2, 102u, dull);
    const auto in = delayed (l, 1024);
    std::vector<double> meanGain (kDefinitionDisplayPoints, 0.0), meanLevel (kDefinitionDisplayPoints, 0.0);
    int frames = 0;
    rig.run (l, r, 256, [&] (size_t end) {
        if (end < (size_t) (3.0 * kSr))
            return;
        for (int i = 0; i < kDefinitionDisplayPoints; ++i)
        {
            meanGain[(size_t) i] += rig.d.getMeters().gainDb[0][i].load();
            meanLevel[(size_t) i] += rig.d.getMeters().levelDb[0][i].load();
        }
        ++frames;
    });
    const size_t s = (size_t) (3.0 * kSr), n = (size_t) (7.0 * kSr);
    const Psd pin (in, s, n), pout (l, s, n);
    double worst = 0.0;
    for (int i = 0; i < kDefinitionDisplayPoints; ++i)
    {
        const double hz = definitionDisplayHz (i);
        if (hz < 400.0 || hz > 16000.0)
            continue;
        const double measured = pout.bandDb (hz, 1.0 / 6.0) - pin.bandDb (hz, 1.0 / 6.0);
        worst = std::max (worst, std::abs (measured - meanGain[(size_t) i] / frames));
    }
    note ("max |meter gain - measured 1/6-oct change| 400 Hz..16 kHz (dB)", worst);
    note ("meter gain at 10 kHz (dB)", meanGain[(size_t) displayIndexNear (10000.0)] / frames);
    note ("meter gain at 1 kHz (dB)", meanGain[(size_t) displayIndexNear (1000.0)] / frames);
    CHECK (worst < 0.5);
    // the level meter is the detector (octave density): the dull input reads ~6 dB/oct down above 2.5 kHz
    const double l1 = meanLevel[(size_t) displayIndexNear (1000.0)] / frames;
    const double l10 = meanLevel[(size_t) displayIndexNear (10000.0)] / frames;
    note ("meter level 1 kHz - 10 kHz (dB)", l1 - l10);
    CHECK_RANGE (l1 - l10, 10.0, 16.0);

    // pink reads flat
    Rig p;
    auto a = shapedNoise (4.0, 0.2, 103u, flat), b = shapedNoise (4.0, 0.2, 104u, flat);
    double lo = 1e9, hi = -1e9;
    bool taken = false;
    p.run (a, b, 256, [&] (size_t end) {
        if (end < (size_t) (3.0 * kSr) || taken)
            return;
        taken = true;
        for (int i = 0; i < kDefinitionDisplayPoints; ++i)
        {
            const double hz = definitionDisplayHz (i);
            if (hz < 400.0 || hz > 16000.0)
                continue;
            lo = std::min (lo, (double) p.d.getMeters().levelDb[0][i].load());
            hi = std::max (hi, (double) p.d.getMeters().levelDb[0][i].load());
        }
    });
    note ("pink: level meter spread 400 Hz..16 kHz in one hop (dB)", hi - lo);
    CHECK (hi - lo < 6.0);
}

TEST_CASE ("defn. controls settle during silence before the next onset")
{
    double worstAmount = 0.0, worstSolo = 0.0;
    int cases = 0;
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int set : { 0, 1 })
            for (bool solo : { false, true })
            {
                Rig rig (sr, 512, set);
                // Set B belongs to the twin in Stereo, and to the second lane in M/S.
                if (set == 0)
                    rig.ctx.channelMode = ChannelMode::MidSide;
                rig.setBoth ("amount", solo ? 0.0f : 100.0f);
                rig.setBoth ("tilt", 6.0f);
                rig.setBoth ("low", 1000.0f);
                rig.setBoth ("high", 4000.0f);
                ModuleMonitor monitor;
                monitor.soloRegion = solo;
                rig.d.setMonitor (monitor);
                auto warmL = pstest::pinkNoise (0.3, 1.0, sr, 151u);
                auto warmR = pstest::pinkNoise (0.3, 1.0, sr, 152u);
                rig.run (warmL, warmR, 77);
                if (! solo)
                    CHECK (rig.d.getMeters().maxBoostDb[0].load() > 0.5f);

                // Drain the STFT before touching the controls, so no non-silent hop can advance them.
                std::vector<float> silentL ((size_t) (0.25 * sr), 0.0f), silentR = silentL;
                rig.run (silentL, silentR, 77);
                rig.setBoth ("amount", 0.0f);
                monitor.soloRegion = false;
                rig.d.setMonitor (monitor);
                silentL.assign ((size_t) (2.0 * sr), 0.0f);
                silentR = silentL;
                rig.run (silentL, silentR, 77);

                auto l = pstest::pinkNoise (0.3, 0.5, sr, 153u);
                auto r = pstest::pinkNoise (0.3, 0.5, sr, 154u);
                const auto inL = l, inR = r;
                rig.run (l, r, 77);
                const int latency = rig.d.getLatencySamples();
                const double diff = std::max (maxDiffDelayed (inL, l, latency), maxDiffDelayed (inR, r, latency));
                (solo ? worstSolo : worstAmount) = std::max (solo ? worstSolo : worstAmount, diff);
                CHECK_EQ (diff, 0.0);
                ++cases;
            }
    note ("silent control transitions across rates / M/S / twin: cases", cases);
    note ("Amount disabled during silence: next onset max delayed-null error", worstAmount);
    note ("solo disabled during silence: next onset max delayed-null error", worstSolo);
}

TEST_CASE ("defn. silence, extremes and denormals")
{
    Rig rig;
    rig.setBoth ("amount", 100.0f);
    rig.setBoth ("tilt", 6.0f);
    auto l = pstest::concat (pstest::pinkNoise (0.5, 2.0, kSr, 111u), std::vector<float> ((size_t) (2.0 * kSr), 0.0f));
    auto r = l;
    const size_t zeroFrom = (size_t) (2.0 * kSr);
    rig.run (l, r, 333);
    double tail = 0.0;
    for (size_t i = zeroFrom + 2 * 1024 + 256; i < l.size(); ++i)
        tail = std::max (tail, (double) std::abs (l[i]) + std::abs (r[i]));
    note ("max |out| from 2N + H after the input went silent", tail);
    CHECK_EQ (tail, 0.0);

    // full-scale square, DC, 1e-30 and denormal-range input
    std::vector<float> sq ((size_t) (2.0 * kSr));
    for (size_t i = 0; i < sq.size(); ++i)
        sq[i] = (i / 60) % 2 ? 1.0f : -1.0f;
    for (const auto& sig : { sq, std::vector<float> ((size_t) kSr, 1.0f), std::vector<float> ((size_t) kSr, 1.0e-30f),
                             std::vector<float> ((size_t) kSr, 1.0e-39f) })
    {
        Rig x;
        x.setBoth ("amount", 100.0f);
        x.setBoth ("tilt", -6.0f);
        x.setBoth ("att", 5.0f);
        auto a = sig, b = sig;
        x.run (a, b, 512);
        double peak = 0.0;
        bool finite = true;
        for (size_t i = 0; i < a.size(); ++i)
        {
            finite = finite && std::isfinite (a[i]) && std::isfinite (b[i]);
            peak = std::max (peak, (double) std::abs (a[i]));
        }
        CHECK (finite);
        CHECK (peak <= 4.0);
    }
    note ("square / DC / tiny inputs: finite, |y| <= 4", 1.0);

    // reset() clears everything (back to an exact delayed null at Amount 0)
    Rig z;
    z.setBoth ("amount", 100.0f);
    auto a = pstest::pinkNoise (0.4, 1.0, kSr, 112u), b = a;
    z.run (a, b, 512);
    z.d.reset();
    z.setBoth ("amount", 0.0f);
    auto c = pstest::pinkNoise (0.4, 1.0, kSr, 113u), e = c;
    const auto c0 = c;
    z.run (c, e, 512);
    CHECK_EQ (maxDiffDelayed (c0, c, 1024), 0.0);
}

TEST_CASE ("defn. realtime: no allocation in setParams / setMonitor / process / reset")
{
    for (int paramSet : { 0, 1 })
        for (int block : { 1, 7, 64, 512, 4096 })
        {
            Rig rig (kSr, 4096, paramSet);
            auto l = pstest::pinkNoise (0.3, 1.0, kSr, 121u), r = pstest::pinkNoise (0.3, 1.0, kSr, 122u);
            long long count = 0;
            {
                pstest::AllocationScope scope;
                long long k = 0;
                rig.run (l, r, block, {}, [&] (size_t) {
                    ++k;
                    rig.setBoth ("amount", (k / 5) % 2 ? 100.0f : 30.0f);
                    rig.setBoth ("tilt", (k / 3) % 2 ? 4.0f : -2.0f);
                    rig.setBoth ("low", (k / 7) % 2 ? 300.0f : 2000.0f);
                    rig.ctx.channelMode = (k / 11) % 2 ? ChannelMode::MidSide : ChannelMode::Stereo;
                    ModuleMonitor m;
                    m.soloRegion = (k / 13) % 2 == 1;
                    rig.d.setMonitor (m);
                    if (k % 97 == 0)
                        rig.d.reset();
                });
                count = scope.count();
            }
            CHECK_EQ (count, 0LL);
        }
}

TEST_CASE ("defn. cpu (x realtime, 48 kHz stereo)")
{
    const double seconds = 20.0;
    const auto srcL = pstest::pinkNoise (0.3, seconds, kSr, 131u), srcR = pstest::pinkNoise (0.3, seconds, kSr, 132u);
    struct Case { const char* what; ChannelMode mode; bool solo; bool ts; float amount; };
    for (Case c : { Case { "Stereo, Amount 0 (default)", ChannelMode::Stereo, false, false, 0.0f },
                    Case { "Stereo, Amount 100 (typical)", ChannelMode::Stereo, false, false, 100.0f },
                    Case { "M/S, Amount 100 both sets", ChannelMode::MidSide, false, false, 100.0f },
                    Case { "T/S (two instances), Amount 100 (worst)", ChannelMode::Stereo, false, true, 100.0f } })
    {
        Rig a, b (kSr, 4096, 1);
        a.setBoth ("amount", c.amount);
        b.setBoth ("amount", c.amount);
        a.setBoth ("tilt", 2.0f);
        a.ctx.channelMode = c.mode;
        auto l = srcL, r = srcR, l2 = srcL, r2 = srcR;
        pstest::Stopwatch sw;
        a.run (l, r, 512);
        if (c.ts)
            b.run (l2, r2, 512);
        const double x = seconds / sw.seconds();
        note (std::string (c.what) + ": x realtime", x);
        CHECK_SPEED (x >= (c.ts ? 40.0 : 80.0));
    }
}

TEST_CASE ("defn. in the chain: T/S twin, M/S and Stereo slots")
{
    // music-like input; the chain slot in each channel mode with Amount 0 is the input delayed by the slot latency
    // (T/S: + the splitter's L, exact up to the splitter's float rounding), and with Amount 100 it processes
    const auto x = pstest::added (pstest::pinkNoise (0.3, 4.0, kSr, 141u), pstest::sine (2500.0, -14.0, 4.0, kSr));
    const auto y = pstest::added (pstest::pinkNoise (0.3, 4.0, kSr, 142u), pstest::sine (700.0, -16.0, 4.0, kSr));
    for (float mode : { 0.0f, 1.0f, 2.0f })   // Stereo, Mid/Side, Transient/Sustain
        for (float amount : { 0.0f, 100.0f })
        {
            Chain chain;
            chain.setOrder ({ ModuleType::Definition });
            chain.prepare (kSr, 480);
            auto v = defaultValues();
            v[(size_t) paramIndex ("defn.chMode")] = mode;
            v[(size_t) paramIndex ("defn.a.amount")] = amount;
            v[(size_t) paramIndex ("defn.b.amount")] = amount;
            v[(size_t) paramIndex ("defn.b.tilt")] = 3.0f;
            auto l = x, r = y;
            for (size_t pos = 0; pos < l.size(); pos += 480)
            {
                const int n = (int) std::min<size_t> (480, l.size() - pos);
                float* ch[2] { l.data() + pos, r.data() + pos };
                chain.process (ch, n, v.data(), HostInfo {});
            }
            const int lat = chain.getLatencySamples();
            const double d = std::max (maxDiffDelayed (x, l, lat), maxDiffDelayed (y, r, lat));
            const std::string what = mode == 0.0f ? "Stereo" : (mode == 1.0f ? "M/S" : "T/S");
            note (what + ", Amount " + std::to_string ((int) amount) + ": slot latency", lat);
            note ("  max |out - in delayed|", d);
            CHECK_EQ (lat, mode == 2.0f ? 1024 + TransientSplitter::latencyFor (kSr) : 1024);
            if (amount == 0.0f)
                CHECK (d < (mode == 2.0f ? 1.0e-6 : 1.0e-7));
            else
                CHECK (d > 1.0e-3);
            if (mode == 2.0f)
            {
                CHECK (chain.hasTwin (ModuleType::Definition));
                CHECK_EQ (chain.getModule (ModuleType::Definition, 1).getParamSet(), 1);
                if (amount > 0.0f)
                {
                    // both instances processed their part and their meters mirror their own set
                    const auto& ma = static_cast<Definition&> (chain.getModule (ModuleType::Definition, 0)).getMeters();
                    const auto& mb = static_cast<Definition&> (chain.getModule (ModuleType::Definition, 1)).getMeters();
                    note ("  transient instance max boost", ma.maxBoostDb[0].load());
                    note ("  sustain twin max boost", mb.maxBoostDb[0].load());
                    CHECK (ma.maxBoostDb[0].load() > 0.1f);
                    CHECK (mb.maxBoostDb[0].load() > 0.1f);
                    CHECK (mb.maxBoostDb[1].load() == mb.maxBoostDb[0].load());
                }
            }
        }
}
