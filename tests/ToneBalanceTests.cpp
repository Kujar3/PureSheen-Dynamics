#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "core/Loudness.h"
#include "dsp/Params.h"
#include "dsp/ToneBalance.h"
#include "dsp/ToneTargets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Tone Balance (dsp/ToneBalance.cpp, dsp/ToneTargets.cpp). Prefix "tbal.". 48 kHz stereo unless noted; measurements
// after the detectors have settled. Transfer functions are measured as output / latency-aligned input power in
// fractional-octave bands (Welch, double FFT), the correction curve also from the module's meters.
namespace
{
using namespace ps;
using pstest::note;
using Buf = std::vector<float>;

constexpr double kSr = 48000.0;
constexpr int kHop = 512;

std::vector<float> defaultValues()
{
    std::vector<float> v;
    for (const auto& s : paramSpecs())
        v.push_back (s.defaultValue);
    return v;
}

struct Rig
{
    std::vector<float> values = defaultValues();
    ToneBalance tb;
    double sr;
    ChannelMode mode = ChannelMode::Stereo;

    explicit Rig (double sampleRate = kSr, int paramSet = 0, int maxBlock = 4096) : sr (sampleRate)
    {
        tb.setParamSet (paramSet);
        tb.prepare (sr, maxBlock);
    }

    void set (const std::string& id, float v)
    {
        const int i = paramIndex (id);
        REQUIRE (i >= 0);
        values[(size_t) i] = v;
    }
    void setBoth (const std::string& name, float v)
    {
        set ("tbal.a." + name, v);
        set ("tbal.b." + name, v);
    }
    void common (ToneTarget t, ToneBalanceMode m, float amount)
    {
        set ("tbal.target", (float) (int) t);
        set ("tbal.mode", (float) (int) m);
        setBoth ("amount", amount);
    }

    void block (float* l, float* r, int n)
    {
        tb.setParams (values.data());
        ProcessContext ctx;
        ctx.channelMode = mode;
        ctx.paramSet = tb.getParamSet();
        float* ch[2] { l, r };
        tb.process (ch, n, ctx);
    }

    // processes in place; perBlock (startSample) after every block
    template <typename F>
    void run (Buf& l, Buf& r, int blockSize, F&& perBlock)
    {
        for (size_t pos = 0; pos < l.size(); pos += (size_t) blockSize)
        {
            const int n = (int) std::min<size_t> ((size_t) blockSize, l.size() - pos);
            block (l.data() + pos, r.data() + pos, n);
            perBlock (pos + (size_t) n);
        }
    }
    void run (Buf& l, Buf& r, int blockSize = kHop)
    {
        run (l, r, blockSize, [] (size_t) {});
    }
};

//==============================================================================
// signals and filters
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

Biquad rbj (int type, double f, double gainDb, double q, double sr)   // 0 peak, 1 low shelf, 2 high shelf, 3 band pass
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w = 2.0 * pstest::kPi * f / sr, cw = std::cos (w), sw = std::sin (w);
    const double alpha = sw / (2.0 * q);
    double b0, b1, b2, a0, a1, a2;
    if (type == 0)
    {
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    }
    else if (type == 1)
    {
        const double s = 2 * std::sqrt (A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
    }
    else if (type == 2)
    {
        const double s = 2 * std::sqrt (A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
    }
    else
    {
        b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    }
    Biquad bq;
    bq.b0 = b0 / a0; bq.b1 = b1 / a0; bq.b2 = b2 / a0; bq.a1 = a1 / a0; bq.a2 = a2 / a0;
    return bq;
}

// x filtered by bq from sample `from` on (before: unfiltered, the filter state still runs so the switch is clean)
Buf filtered (const Buf& x, Biquad bq, size_t from = 0)
{
    Buf y (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double f = bq.process (x[i]);
        y[i] = (float) (i >= from ? f : (double) x[i]);
    }
    return y;
}

Buf pink (double seconds, std::uint32_t seed, double amp = 0.5) { return pstest::pinkNoise (amp, seconds, kSr, seed); }

// a deterministic music-like signal: pink bed, a bass line with harmonics, kick and hat hits, a chord pad
Buf musicLike (double seconds, std::uint32_t seed, double sr = kSr)
{
    const size_t n = (size_t) std::llround (seconds * sr);
    Buf y = pstest::pinkNoise (0.08, seconds, sr, seed);
    pstest::Lcg lcg (seed * 7u + 3u);
    const double notes[] { 55.0, 55.0, 73.4, 82.4 };
    const double pad[] { 220.0, 277.2, 329.6 };
    double hat = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const double beat = std::fmod (t, 0.5);
        const int bar = (int) (t / 2.0) % 4;
        double s = 0.0;
        // bass line (4 harmonics)
        for (int h = 1; h <= 4; ++h)
            s += 0.12 / h * std::sin (2.0 * pstest::kPi * notes[bar] * h * t);
        // kick: decaying 60 Hz with a pitch drop
        s += 0.5 * std::exp (-beat / 0.08) * std::sin (2.0 * pstest::kPi * (50.0 + 60.0 * std::exp (-beat / 0.02)) * beat);
        // hat: noise burst every 250 ms
        const double hb = std::fmod (t, 0.25);
        const double white = lcg.next();
        hat = 0.6 * hat + 0.4 * white;
        s += 0.15 * std::exp (-hb / 0.02) * (white - hat);
        // pad (6 harmonics)
        for (double f0 : pad)
            for (int h = 1; h <= 6; ++h)
                s += 0.03 / h * std::sin (2.0 * pstest::kPi * f0 * h * t + h);
        y[i] = (float) (y[i] + s);
    }
    return y;
}

Buf delayed (const Buf& x, int d)
{
    Buf y (x.size(), 0.0f);
    for (size_t i = (size_t) d; i < x.size(); ++i)
        y[i] = x[i - (size_t) d];
    return y;
}

double maxAbsDiff (const Buf& a, const Buf& b, size_t from = 0)
{
    double m = 0.0;
    for (size_t i = from; i < a.size(); ++i)
        m = std::max (m, std::abs ((double) a[i] - (double) b[i]));
    return m;
}

//==============================================================================
// measurement: Welch power spectrum (Hann), scaled so the bins sum to the mean square
std::vector<double> welch (const Buf& x, size_t start, size_t len, size_t nfft = 8192)
{
    std::vector<double> w (nfft), psd (nfft / 2 + 1, 0.0);
    double wsum = 0.0;
    for (size_t j = 0; j < nfft; ++j)
    {
        w[j] = 0.5 - 0.5 * std::cos (2.0 * pstest::kPi * (double) j / (double) nfft);
        wsum += w[j] * w[j];
    }
    int segs = 0;
    std::vector<pstest::cd> a (nfft);
    for (size_t s = start; s + nfft <= start + len && s + nfft <= x.size(); s += nfft / 2)
    {
        for (size_t j = 0; j < nfft; ++j)
            a[j] = (double) x[s + j] * w[j];
        pstest::fftInPlace (a);
        for (size_t k = 0; k <= nfft / 2; ++k)
            psd[k] += std::norm (a[k]) * (k == 0 || k == nfft / 2 ? 1.0 : 2.0) / (wsum * (double) nfft);
        ++segs;
    }
    for (auto& p : psd)
        p /= std::max (segs, 1);
    return psd;
}

double bandPower (const std::vector<double>& psd, double lo, double hi, double sr = kSr)
{
    const size_t nfft = (psd.size() - 1) * 2;
    const double binHz = sr / (double) nfft;
    double s = 0.0;
    for (size_t k = 1; k < psd.size(); ++k)
    {
        const double f = (double) k * binHz;
        if (f >= lo && f < hi)
            s += psd[k];
    }
    return s;
}

double bandDb (const std::vector<double>& psd, double fc, double octaves = 1.0 / 3.0, double sr = kSr)
{
    const double h = std::pow (2.0, octaves / 2.0);
    return 10.0 * std::log10 (bandPower (psd, fc / h, fc * h, sr) + 1.0e-30);
}

struct Transfer
{
    std::vector<double> in, out;
    double db (double fc, double oct = 1.0 / 3.0) const { return bandDb (out, fc, oct) - bandDb (in, fc, oct); }
};

// output vs latency-aligned input over [start, start + len) of the output, averaged over both channels
Transfer transfer (const Buf& inL, const Buf& inR, const Buf& outL, const Buf& outR, int latency, size_t start, size_t len)
{
    const Buf dl = delayed (inL, latency), dr = delayed (inR, latency);
    Transfer t;
    t.in = welch (dl, start, len);
    t.out = welch (outL, start, len);
    const auto inR2 = welch (dr, start, len), outR2 = welch (outR, start, len);
    for (size_t k = 0; k < t.in.size(); ++k)
    {
        t.in[k] += inR2[k];
        t.out[k] += outR2[k];
    }
    return t;
}

const double kThirdOctaves[] { 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250,
                               1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000 };

int displayIndex (double hz)
{
    int best = 0;
    for (int i = 0; i < kToneDisplayPoints; ++i)
        if (std::abs (std::log2 (toneBalanceDisplayHz (i) / hz)) < std::abs (std::log2 (toneBalanceDisplayHz (best) / hz)))
            best = i;
    return best;
}

double meterGain (const ToneBalance& tb, double hz, int set = 0)
{
    // interpolate the 64-point meter in log f
    const double x = std::log10 (std::clamp (hz, 20.0, 20000.0) / 20.0) / 3.0 * (kToneDisplayPoints - 1);
    const int i0 = std::clamp ((int) std::floor (x), 0, kToneDisplayPoints - 2);
    const double t = x - i0;
    const auto& m = tb.getMeters();
    return (1.0 - t) * m.gainDb[set][i0].load() + t * m.gainDb[set][i0 + 1].load();
}

// Unweighted power (dB) of a stereo segment: Shape's level anchor is power-neutral.
double powerDb (const Buf& l, const Buf& r, size_t start, size_t len)
{
    double s = 0.0;
    for (size_t i = start; i < start + len && i < l.size(); ++i)
        s += (double) l[i] * l[i] + (double) r[i] * r[i];
    return 10.0 * std::log10 (s / (double) len + 1.0e-30);
}

double integratedLufs (const Buf& l, const Buf& r, size_t start, size_t len)
{
    return measureIntegratedLufs (l.data() + start, r.data() + start, (long long) len, kSr);
}
} // namespace

//==============================================================================
TEST_CASE ("tbal. target curves: anchors, Flat, tolerance corridor, display grid")
{
    for (int t = 0; t < (int) ToneTarget::Count; ++t)
    {
        const auto target = (ToneTarget) t;
        const auto& s = toneTargetShape (target);
        double tol = 0.0;
        if (target == ToneTarget::Flat)
        {
            for (double f = 20.0; f <= 20000.0; f *= 1.1)
                CHECK_EQ (toneTargetDb (target, f), 0.0);
            continue;
        }
        if (toneTargetIsMeasured (target))
        {
            // the reference's measured table: 1 kHz = 0 dB, the tolerance still from the shape, bounded, smooth in log f
            CHECK_NEAR (toneTargetDb (target, 1000.0, &tol), 0.0, 0.05);
            CHECK_NEAR (tol, s.tol, 1.0e-9);
            double prev = toneTargetDb (target, 20.0), worstStep = 0.0;
            for (double f = 20.0 * 1.05; f <= 20000.0; f *= 1.05)
            {
                const double y = toneTargetDb (target, f);
                CHECK (std::isfinite (y) && std::abs (y) < 30.0);
                worstStep = std::max (worstStep, std::abs (y - prev));
                prev = y;
            }
            CHECK (worstStep < 1.5);   // no jumps between neighbouring frequencies (1/14 octave apart)
            CHECK_EQ (toneTargetDb (target, 10.0), toneTargetDb (target, 20.0));   // constant outside the table
            continue;
        }
        CHECK_NEAR (toneTargetDb (target, 30.0), s.sub, 1.0e-9);
        CHECK_NEAR (toneTargetDb (target, 80.0), s.bass, 1.0e-9);
        CHECK_NEAR (toneTargetDb (target, 300.0), s.lowMid, 1.0e-9);
        CHECK_NEAR (toneTargetDb (target, 1000.0, &tol), 0.0, 1.0e-9);
        CHECK_NEAR (tol, s.tol, 1.0e-9);
        CHECK_NEAR (toneTargetDb (target, 3000.0), s.pres + s.slope * std::log2 (3.0), 1.0e-6);
        CHECK_NEAR (toneTargetDb (target, 8000.0), 3.0 * s.slope, 1.0e-6);
        const double v14 = s.slope * std::log2 (14.0) + s.air;
        CHECK_NEAR (toneTargetDb (target, 14000.0), v14, 1.0e-6);
        CHECK_NEAR (toneTargetDb (target, 20000.0), v14 - 6.0, 1.0e-6);
        CHECK_NEAR (toneTargetDb (target, 20.0), s.sub - 6.0, 1.0e-6);
        CHECK_NEAR (toneTargetDb (target, 10.0), s.sub - 6.0, 1.0e-6);   // constant outside 20 Hz .. 20 kHz
        // monotone between anchors (PCHIP): no overshoot beyond the neighbouring anchor values
        const double anchors[] { 20, 30, 80, 300, 1000, 3000, 8000, 14000, 20000 };
        for (int a = 0; a + 1 < 9; ++a)
        {
            const double y0 = toneTargetDb (target, anchors[a]), y1 = toneTargetDb (target, anchors[a + 1]);
            for (int k = 1; k < 16; ++k)
            {
                const double f = anchors[a] * std::pow (anchors[a + 1] / anchors[a], k / 16.0);
                const double y = toneTargetDb (target, f);
                CHECK (y >= std::min (y0, y1) - 1.0e-9 && y <= std::max (y0, y1) + 1.0e-9);
            }
        }
        toneTargetDb (target, 50.0, &tol);
        CHECK_NEAR (tol, 1.5 * s.tol, 1.0e-9);
        toneTargetDb (target, 12000.0, &tol);
        CHECK_NEAR (tol, 1.5 * s.tol, 1.0e-9);
        toneTargetDb (target, 2000.0, &tol);
        CHECK_NEAR (tol, s.tol, 1.0e-9);
        toneTargetDb (target, 160.0, &tol);
        CHECK (tol > s.tol && tol < 1.5 * s.tol);
    }
    CHECK_NEAR (toneBalanceDisplayHz (0), 20.0, 1.0e-9);
    CHECK_NEAR (toneBalanceDisplayHz (kToneDisplayPoints - 1), 20000.0, 1.0e-6);
    note ("Balanced at 50 Hz (dB)", toneTargetDb (ToneTarget::Balanced, 50.0));
    note ("Balanced at 3 kHz (dB)", toneTargetDb (ToneTarget::Balanced, 3000.0));
    note ("Balanced at 10 kHz (dB)", toneTargetDb (ToneTarget::Balanced, 10000.0));
}

TEST_CASE ("tbal. amount 0 is an exact delayed null in every mode, target and channel mode")
{
    const Buf srcL = pstest::added (pink (2.0, 11u), pstest::sine (997.0, -12.0, 2.0, kSr));
    const Buf srcR = pstest::added (pink (2.0, 12u), pstest::sine (3001.0, -18.0, 2.0, kSr));
    double worst = 0.0;
    int runs = 0;
    for (int m = 0; m < 3; ++m)
        for (int t = 0; t < (int) ToneTarget::Count; ++t)
            for (auto cm : { ChannelMode::Stereo, ChannelMode::MidSide, ChannelMode::TransientSustain })
            {
                const int paramSet = cm == ChannelMode::TransientSustain ? 1 : 0;
                Rig rig (kSr, paramSet);
                rig.mode = cm == ChannelMode::TransientSustain ? ChannelMode::Stereo : cm;
                rig.common ((ToneTarget) t, (ToneBalanceMode) m, 0.0f);
                rig.setBoth ("tame", 1.0f);
                Buf l = srcL, r = srcR;
                rig.run (l, r, 333);
                const int lat = rig.tb.getLatencySamples();
                worst = std::max ({ worst, maxAbsDiff (l, delayed (srcL, lat)), maxAbsDiff (r, delayed (srcR, lat)) });
                ++runs;
            }
    note ("runs", runs);
    note ("max |out - in delayed|", worst);
    CHECK_EQ (worst, 0.0);
}

TEST_CASE ("tbal. returns to an exact null after Amount goes back to 0")
{
    Rig rig;
    rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
    const Buf src = filtered (pink (12.0, 21u), rbj (1, 120.0, 8.0, 0.7, kSr));
    Buf l = src, r = src;
    const size_t switchAt = (size_t) (5.0 * kSr);
    bool sawCorrection = false;
    rig.run (l, r, kHop, [&] (size_t pos)
    {
        if (pos == switchAt - (switchAt % kHop))
            sawCorrection = std::abs (meterGain (rig.tb, 60.0)) > 1.0;
        if (pos >= switchAt)
            rig.setBoth ("amount", 0.0f);
    });
    CHECK (sawCorrection);
    const int lat = rig.tb.getLatencySamples();
    const Buf d = delayed (src, lat);
    // first exactly-null sample after the switch
    size_t lastDiff = 0;
    for (size_t i = 0; i < l.size(); ++i)
        if (l[i] != d[i])
            lastDiff = i;
    const double sec = ((double) lastDiff - (double) switchAt) / kSr;
    note ("seconds from Amount 0 to an exact null", sec);
    CHECK (lastDiff > switchAt);
    CHECK (sec < 4.0);
}

TEST_CASE ("tbal. Amount 0 during silence does not restore an old correction when audio returns")
{
    auto coloured = [] (double seconds, std::uint32_t seed)
    {
        return filtered (filtered (pink (seconds, seed), rbj (1, 120.0, 8.0, 0.7, kSr)),
                         rbj (0, 3000.0, -14.0, 1.5, kSr));
    };
    const Buf trainL = coloured (6.0, 24u), trainR = coloured (6.0, 25u);
    const Buf returnL = coloured (2.0, 26u), returnR = coloured (2.0, 27u);
    for (auto mode : { ToneBalanceMode::Shape, ToneBalanceMode::Boost, ToneBalanceMode::Cut })
    {
        Rig rig;
        rig.common (ToneTarget::Flat, mode, 100.0f);
        rig.setBoth ("sens", 100.0f);
        Buf l = trainL, r = trainR;
        rig.run (l, r);
        const int lat = rig.tb.getLatencySamples();
        const auto& meters = rig.tb.getMeters();
        const double before = std::max ((double) meters.maxBoostDb[0].load(), -(double) meters.maxCutDb[0].load());
        REQUIRE (before > 1.0);
        REQUIRE (maxAbsDiff (l, delayed (trainL, lat)) > 1.0e-3);

        // Flush every signal-bearing frame before automating. Detector history may hold on silence, but the
        // controls and gain release must still settle before fresh audio arrives.
        Buf flushL ((size_t) kSr, 0.0f), flushR = flushL;
        rig.run (flushL, flushR);
        rig.setBoth ("amount", 0.0f);
        Buf silenceL ((size_t) (6.0 * kSr), 0.0f), silenceR = silenceL;
        rig.run (silenceL, silenceR);

        l = returnL;
        r = returnR;
        rig.run (l, r);
        const double error = std::max (maxAbsDiff (l, delayed (returnL, lat)), maxAbsDiff (r, delayed (returnR, lat)));
        note ("mode " + std::to_string ((int) mode) + ": correction before silence (dB)", before);
        note ("  max resumed |out - input delayed| after Amount was 0 for 6 s", error);
        CHECK_EQ (error, 0.0);
    }
}

TEST_CASE ("tbal. latency per sample rate equals the measured delay and never changes with parameters")
{
    const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const int expected[] { 2048, 2048, 4096, 4096, 8192, 8192 };
    for (int i = 0; i < 6; ++i)
    {
        Rig rig (rates[i]);
        CHECK_EQ (rig.tb.getLatencySamples(), expected[i]);
        // impulse at Amount 0
        Buf l (20000, 0.0f), r (20000, 0.0f);
        l[100] = 1.0f;
        r[100] = -0.5f;
        rig.run (l, r, 256);
        size_t at = 0;
        for (size_t k = 0; k < l.size(); ++k)
            if (l[k] != 0.0f)
                at = k;
        CHECK_EQ ((int) at - 100, expected[i]);
        // randomised parameter sweep (200 blocks): latency constant
        pstest::Lcg lcg (77u);
        Buf nl = pstest::pinkNoise (0.3, 200.0 * 512.0 / rates[i], rates[i], 3u), nr = nl;
        bool constant = true;
        for (int b = 0; b < 200; ++b)
        {
            rig.set ("tbal.mode", (float) (int) ((lcg.next() + 1.0f) * 1.5f) );
            rig.set ("tbal.target", (float) std::min (35, (int) ((lcg.next() + 1.0f) * 18.0f)));
            for (const char* id : { "amount", "speed", "smooth", "cutSpeed", "sens", "low", "mid", "high" })
                rig.setBoth (id, (lcg.next() + 1.0f) * 50.0f);
            rig.setBoth ("tame", lcg.next() > 0.0f ? 1.0f : 0.0f);
            rig.block (nl.data() + b * 512, nr.data() + b * 512, 512);
            constant = constant && rig.tb.getLatencySamples() == expected[i];
        }
        CHECK (constant);
    }

    // with a real correction running, the output is still the input delayed by exactly N (zero-phase gains): the
    // cross-correlation of output and input peaks at lag N
    Rig rig;
    rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
    const Buf src = filtered (pink (8.0, 5u), rbj (1, 120.0, 8.0, 0.7, kSr));
    Buf l = src, r = src;
    rig.run (l, r);
    int bestLag = 0;
    double best = -1.0;
    for (int lag = 2048 - 64; lag <= 2048 + 64; ++lag)
    {
        double s = 0.0;
        for (size_t k = (size_t) (5.0 * kSr); k < (size_t) (7.5 * kSr); ++k)
            s += (double) l[k] * (double) src[k - (size_t) lag];
        if (s > best)
        {
            best = s;
            bestLag = lag;
        }
    }
    note ("cross-correlation peak lag with Shape at Amount 100", bestLag);
    CHECK_EQ (bestLag, 2048);
}

TEST_CASE ("tbal. flat target leaves pink noise alone")
{
    Rig rig;
    rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
    const Buf srcL = pink (14.0, 31u), srcR = pink (14.0, 32u);
    Buf l = srcL, r = srcR;
    std::vector<std::vector<float>> frames;
    rig.run (l, r, kHop, [&] (size_t pos)
    {
        if (pos > (size_t) (6.0 * kSr))
        {
            std::vector<float> g;
            for (int i = 0; i < kToneDisplayPoints; ++i)
                g.push_back (rig.tb.getMeters().gainDb[0][i].load());
            frames.push_back (g);
        }
    });
    const auto tr = transfer (srcL, srcR, l, r, rig.tb.getLatencySamples(), (size_t) (6.0 * kSr), (size_t) (8.0 * kSr));
    double worst = 0.0;
    for (double fc : kThirdOctaves)
        worst = std::max (worst, std::abs (tr.db (fc)));
    double worstStd = 0.0;
    for (int i = 4; i < kToneDisplayPoints; ++i)
    {
        double s = 0.0, s2 = 0.0;
        for (size_t f = 1; f < frames.size(); ++f)
        {
            const double d = frames[f][(size_t) i] - frames[f - 1][(size_t) i];
            s += d;
            s2 += d * d;
        }
        const double n = (double) frames.size() - 1.0;
        worstStd = std::max (worstStd, std::sqrt (std::max (0.0, s2 / n - (s / n) * (s / n))));
    }
    note ("max |1/3-oct output - input| 31.5 Hz .. 16 kHz (dB)", worst);
    note ("max frame-to-frame std of the correction (dB)", worstStd);
    CHECK (worst < 0.75);
    CHECK (worstStd < 0.25);
}

TEST_CASE ("tbal. shape pulls a +6 dB 2 kHz bump toward the target within the documented time")
{
    // pink noise, Flat target, Shape, Amount 100, Speed 50 (tauS 490 ms); a +6 dB one-octave bell at 2 kHz switches
    // on at 6 s. Documented (ToneBalance.h): the bump's 1/3-octave excess drops to <= 2.5 dB (Smoothing 50) and
    // <= 1.5 dB (Smoothing 0); the correction reaches 63 % of its final value within 0.5 .. 1.5 tauS.
    for (float smooth : { 50.0f, 0.0f })
    {
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
        rig.setBoth ("smooth", smooth);
        const size_t onset = (size_t) (6.0 * kSr);
        const Buf pl = pink (14.0, 41u), pr = pink (14.0, 42u);
        const Buf srcL = filtered (pl, rbj (0, 2000.0, 6.0, 1.41, kSr), onset);
        const Buf srcR = filtered (pr, rbj (0, 2000.0, 6.0, 1.41, kSr), onset);
        Buf l = srcL, r = srcR;
        std::vector<std::pair<double, double>> track;   // (seconds after the onset, correction 2 kHz - 500 Hz)
        const int lat = rig.tb.getLatencySamples();
        rig.run (l, r, kHop, [&] (size_t pos)
        {
            // the analysis frame ending at input time pos; its centre passed the onset at pos - N / 2
            track.push_back ({ ((double) pos - (double) lat / 2.0 - (double) onset) / kSr,
                               meterGain (rig.tb, 2000.0) - meterGain (rig.tb, 500.0) });
        });
        double fin = 0.0;
        int nf = 0;
        for (auto& p : track)
            if (p.first > 6.0)
            {
                fin += p.second;
                ++nf;
            }
        fin /= std::max (nf, 1);
        double t63 = -1.0;
        for (auto& p : track)
            if (p.first > 0.0 && p.second <= 0.63 * fin)
            {
                t63 = p.first;
                break;
            }
        // residual bump in the output vs the input bump (1/3 octave around 2 kHz, relative to 500 Hz)
        const size_t m0 = onset + (size_t) (4.0 * kSr), mlen = (size_t) (4.0 * kSr);
        const auto in = welch (delayed (srcL, lat), m0, mlen), out = welch (l, m0, mlen);
        const auto inPink = welch (delayed (pl, lat), m0, mlen);
        const double ref = bandDb (inPink, 2000.0) - bandDb (inPink, 500.0);
        const double inBump = (bandDb (in, 2000.0) - bandDb (in, 500.0)) - ref;
        const double outBump = (bandDb (out, 2000.0) - bandDb (out, 500.0)) - ref;
        note (smooth > 0.0f ? "Smoothing 50: input bump (dB)" : "Smoothing 0: input bump (dB)", inBump);
        note ("  output bump (dB)", outBump);
        note ("  final correction 2 kHz rel. 500 Hz (dB)", fin);
        note ("  time to 63 % (s)", t63);
        CHECK (inBump > 5.0);
        // Shape corrects 0.8 x 0.875 = 70 % of a measured deviation (the reference's law: 2.4 / 3.3 here); the skirted
        // reading (12 dB / octave) softens a one-octave bump a little (Smoothing 0: 2.5 -> 2.8)
        CHECK (outBump <= (smooth > 0.0f ? 3.5 : 2.9));
        CHECK (t63 >= 0.5 * 0.49 && t63 <= 1.5 * 0.49);
    }
}

TEST_CASE ("tbal. shape corrects a bass excess and stays power neutral on several materials")
{
    struct Material
    {
        const char* name;
        ToneTarget target;
        Buf l, r;
    };
    std::vector<Material> mats;
    mats.push_back ({ "pink + 8 dB low shelf", ToneTarget::Flat, filtered (pink (12.0, 51u), rbj (1, 120.0, 8.0, 0.7, kSr)),
                      filtered (pink (12.0, 52u), rbj (1, 120.0, 8.0, 0.7, kSr)) });
    mats.push_back ({ "pink + 6 dB high shelf", ToneTarget::Balanced, filtered (pink (12.0, 53u), rbj (2, 4000.0, 6.0, 0.7, kSr)),
                      filtered (pink (12.0, 54u), rbj (2, 4000.0, 6.0, 0.7, kSr)) });
    mats.push_back ({ "pink + 2 kHz bump", ToneTarget::Pop, filtered (pink (12.0, 55u), rbj (0, 2000.0, 6.0, 1.41, kSr)),
                      filtered (pink (12.0, 56u), rbj (0, 2000.0, 6.0, 1.41, kSr)) });
    mats.push_back ({ "music-like", ToneTarget::HipHop, musicLike (12.0, 57u), musicLike (12.0, 58u) });
    mats.push_back ({ "music-like, dark", ToneTarget::Edm, filtered (musicLike (12.0, 59u), rbj (2, 2000.0, -9.0, 0.7, kSr)),
                      filtered (musicLike (12.0, 60u), rbj (2, 2000.0, -9.0, 0.7, kSr)) });
    for (float amount : { 100.0f, 50.0f })
        for (auto& m : mats)
        {
            Rig rig;
            rig.common (m.target, ToneBalanceMode::Shape, amount);
            Buf l = m.l, r = m.r;
            rig.run (l, r);
            const int lat = rig.tb.getLatencySamples();
            const size_t m0 = (size_t) (5.0 * kSr), len = (size_t) (7.0 * kSr) - (size_t) lat;
            const Buf dl = delayed (m.l, lat), dr = delayed (m.r, lat);
            const double dLu = powerDb (l, r, m0, len) - powerDb (dl, dr, m0, len);
            const double maxCorr = std::max (std::abs ((double) rig.tb.getMeters().maxBoostDb[0].load()),
                                             std::abs ((double) rig.tb.getMeters().maxCutDb[0].load()));
            note (std::string (m.name) + (amount > 50.0f ? " (Amount 100)" : " (Amount 50)") + ": power change (dB)", dLu);
            note ("  largest correction (dB)", maxCorr);
            // the lowest STFT bins are coarse; with unweighted power they carry a bass shelf's energy (K-weighting hid
            // that): within 0.8 dB on shelved noise, <= 0.25 dB on music-like material
            CHECK (std::abs (dLu) < 0.8);
            CHECK (maxCorr > 1.0);
            if (m.target == ToneTarget::Flat && amount == 100.0f)
            {
                const auto tr = transfer (m.l, m.r, l, r, lat, m0, len);
                const double excess = bandDb (tr.out, 63.0, 1.0) - bandDb (tr.out, 1000.0, 1.0);
                const double inExcess = bandDb (tr.in, 63.0, 1.0) - bandDb (tr.in, 1000.0, 1.0);
                note ("  40-100 Hz excess vs 1 kHz, input (dB)", inExcess);
                note ("  40-100 Hz excess vs 1 kHz, output (dB)", excess);
                CHECK (inExcess > 6.0);
                // 0.8 x 0.875 of 7.4 dB leaves ~2.2; the detector's 0.3 s attack reads the narrow bass bands of noise
                // a little high (~0.4 dB), the skirted reading softens the shelf's slope (~0.3 dB)
                CHECK (excess < 3.0);
            }
        }
}

TEST_CASE ("tbal. shape power neutrality follows interpolated bin gains on sparse tonal material")
{
    // Widely spaced low-frequency band centres can straddle tones with very different corrections. Summing the
    // powers after exponentiating each band's gain is not equivalent to the audio path's interpolation in dB:
    // that approximation overestimated the output power and attenuated the 180 / 350 Hz pair by 0.84 LU.
    struct Case
    {
        double hz;
        float amount, smoothing, low, mid, high;
    };
    const Case cases[] {
        { 120.0, 100.0f, 50.0f, 100.0f, 100.0f, 100.0f },
        { 180.0, 100.0f, 50.0f, 100.0f, 100.0f, 100.0f },
        { 120.0,  50.0f, 50.0f, 100.0f, 100.0f, 100.0f },
        { 180.0,  50.0f, 50.0f, 100.0f, 100.0f, 100.0f },
        { 120.0, 100.0f,  0.0f, 100.0f, 100.0f, 100.0f },
        { 180.0, 100.0f,  0.0f, 100.0f, 100.0f, 100.0f },
        { 120.0, 100.0f, 50.0f,   0.0f, 100.0f, 100.0f },
        { 180.0, 100.0f, 50.0f, 100.0f,  50.0f,   0.0f }
    };
    double worst = 0.0;
    for (const auto& c : cases)
    {
        Buf input = pink (6.0, 575u, 0.004);
        for (size_t i = 0; i < input.size(); ++i)
        {
            const double t = (double) i / kSr;
            input[i] += (float) (0.2 * std::sin (2.0 * pstest::kPi * c.hz * t)
                                + 0.3 * std::sin (2.0 * pstest::kPi * 350.0 * t));
        }
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, c.amount);
        rig.setBoth ("smooth", c.smoothing);
        rig.setBoth ("low", c.low);
        rig.setBoth ("mid", c.mid);
        rig.setBoth ("high", c.high);
        Buf l = input, r = input;
        rig.run (l, r);
        const Buf dry = delayed (input, rig.tb.getLatencySamples());
        const size_t start = (size_t) (4.0 * kSr), len = (size_t) (1.9 * kSr);
        const double delta = powerDb (l, r, start, len) - powerDb (dry, dry, start, len);
        worst = std::max (worst, std::abs (delta));
        note ("sparse pair " + std::to_string ((int) c.hz) + "/350 Hz, Amount " + std::to_string ((int) c.amount)
              + ", Smoothing " + std::to_string ((int) c.smoothing) + ": power change (dB)", delta);
        CHECK (std::abs (delta) < 0.5);
    }
    note ("sparse tonal material: worst power change (dB)", worst);
}

TEST_CASE ("tbal. shape loudness solve stays stable when quiet bands become active")
{
    // Sparse tones crossing the detector gate leave different histories in the power average and the boost
    // presence fade. The neutrality solve must converge across the fade's zero-gain corner. An unbounded Newton
    // step used to create a 5.5 dB applied-curve jump long after the last audio onset, with controls held fixed.
    Rig rig;
    rig.common (ToneTarget::ClassicRock, ToneBalanceMode::Shape, 100.0f);
    rig.setBoth ("speed", 0.0f);
    rig.setBoth ("smooth", 0.0f);
    constexpr int blocks = 516, tones = 12;
    const double hz[tones] { 63, 108, 187, 337, 580, 1000, 1733, 2941, 4837, 6719, 10803, 15901 };
    double amplitude[tones] {}, target[tones] {}, phase[tones] {};
    float previous[kToneDisplayPoints] {};
    const Buf bed = pstest::pinkNoise (1.0e-6, (double) (blocks * kHop) / kSr, kSr, 9268u);
    pstest::Lcg random (7897u);
    Buf l (kHop), r (kHop);
    double maxSteadyStep = 0.0, maxCorrection = 0.0;
    for (int b = 0; b < blocks; ++b)
    {
        if (b == 0 || (b > 400 && b % 47 == 0))
            for (int j = 0; j < tones; ++j)
            {
                target[j] = std::pow (10.0, (-12.0 - 130.0 * (random.next() + 1.0) * 0.5) / 20.0);
                if (b == 0)
                    amplitude[j] = target[j];
            }
        for (int i = 0; i < kHop; ++i)
        {
            double x = bed[(size_t) b * kHop + (size_t) i];
            for (int j = 0; j < tones; ++j)
            {
                amplitude[j] += (target[j] - amplitude[j]) * 0.02;   // 1 ms rise/fall; no discontinuous samples
                phase[j] += 2.0 * pstest::kPi * hz[j] / kSr;
                if (phase[j] > 2.0 * pstest::kPi)
                    phase[j] -= 2.0 * pstest::kPi;
                x += amplitude[j] * std::sin (phase[j]);
            }
            l[(size_t) i] = r[(size_t) i] = (float) x;
        }
        rig.block (l.data(), r.data(), kHop);
        for (int j = 0; j < kToneDisplayPoints; ++j)
        {
            const float gain = rig.tb.getMeters().gainDb[0][j].load();
            // Last tone-level change: hop 470. Next: 517. This checks a steady interval, not the intended
            // immediate cut attack on an onset; the input amplitudes have settled for at least 266 ms.
            if (b >= 495)
                maxSteadyStep = std::max (maxSteadyStep, (double) std::abs (gain - previous[j]));
            maxCorrection = std::max (maxCorrection, (double) std::abs (gain));
            previous[j] = gain;
        }
    }
    note ("largest correction on the sparse changing tones (dB)", maxCorrection);
    note ("largest gain change per hop in the steady interval (dB)", maxSteadyStep);
    CHECK (maxCorrection > 3.0);
    CHECK (maxSteadyStep < 2.0);
}

TEST_CASE ("tbal. boost cap: at most +9 dB x (Amount / 100)^0.84, Boost mode never cuts")
{
    // pink with nearly empty top octaves (a -40 dB high shelf at 4 kHz): a deviation far beyond the cap even through
    // the skirted reading
    auto notched = [] (std::uint32_t seed)
    {
        return filtered (pink (10.0, seed), rbj (2, 4000.0, -40.0, 0.7, kSr));
    };
    const Buf srcL = notched (61u), srcR = notched (62u);
    struct Case { ToneBalanceMode mode; float amount; double cap; const char* name; };
    for (const Case c : { Case { ToneBalanceMode::Shape, 100.0f, 9.0, "Shape, Amount 100" },
                          Case { ToneBalanceMode::Shape, 50.0f, 5.03, "Shape, Amount 50" },
                          Case { ToneBalanceMode::Boost, 100.0f, 9.0, "Boost, Amount 100" } })
    {
        Rig rig;
        rig.common (ToneTarget::Flat, c.mode, c.amount);
        rig.setBoth ("speed", 100.0f);
        rig.setBoth ("smooth", 0.0f);
        Buf l = srcL, r = srcR;
        double maxBoost = 0.0, minGain = 0.0;
        rig.run (l, r, kHop, [&] (size_t)
        {
            maxBoost = std::max (maxBoost, (double) rig.tb.getMeters().maxBoostDb[0].load());
            minGain = std::min (minGain, (double) rig.tb.getMeters().maxCutDb[0].load());
        });
        const auto tr = transfer (srcL, srcR, l, r, rig.tb.getLatencySamples(), (size_t) (5.0 * kSr), (size_t) (4.0 * kSr));
        double measured = -100.0;
        for (double f = 6000.0; f <= 16000.0; f *= std::pow (2.0, 1.0 / 12.0))
            measured = std::max (measured, tr.db (f, 1.0 / 12.0));
        note (std::string (c.name) + ": max boost (meters, dB)", maxBoost);
        note ("  measured max 1/12-oct boost at 6-16 kHz (dB)", measured);
        note ("  most negative gain anywhere (dB)", minGain);
        CHECK (maxBoost <= c.cap + 0.1);
        CHECK (measured <= c.cap + 0.1);
        CHECK (maxBoost >= 0.8 * c.cap);
        if (c.mode == ToneBalanceMode::Boost)
            CHECK (minGain >= -0.01);
    }
}

TEST_CASE ("tbal. cut mode never boosts")
{
    const Buf srcL = musicLike (8.0, 71u), srcR = musicLike (8.0, 72u);
    for (float sens : { 0.0f, 50.0f, 100.0f })
        for (float speed : { 0.0f, 100.0f })
        {
            Rig rig;
            rig.common (ToneTarget::Balanced, ToneBalanceMode::Cut, 100.0f);
            rig.setBoth ("sens", sens);
            rig.setBoth ("cutSpeed", speed);
            rig.setBoth ("tame", speed > 50.0f ? 1.0f : 0.0f);
            Buf l = srcL, r = srcR;
            double maxBoost = 0.0, maxCut = 0.0;
            rig.run (l, r, kHop, [&] (size_t)
            {
                for (int i = 0; i < kToneDisplayPoints; ++i)
                    maxBoost = std::max (maxBoost, (double) rig.tb.getMeters().gainDb[0][i].load());
                maxCut = std::min (maxCut, (double) rig.tb.getMeters().maxCutDb[0].load());
            });
            note ("sens " + std::to_string ((int) sens) + " cutSpeed " + std::to_string ((int) speed) + ": max gain (dB)", maxBoost);
            note ("  deepest cut (dB)", maxCut);
            CHECK (maxBoost <= 0.01);
            if (sens == 100.0f)
                CHECK (maxCut < -1.0);
        }
}

TEST_CASE ("tbal. cut sensitivity: resonance over the target, neighbours untouched")
{
    const size_t n = (size_t) (8.0 * kSr);
    const Buf pl = pink (8.0, 81u), pr = pink (8.0, 82u);
    // the noise power in the 1/12-octave band around 2.5 kHz (the module's band width there)
    const auto psd = welch (pl, 0, n);
    const double bandP = bandPower (psd, 2500.0 * std::pow (2.0, -1.0 / 24.0), 2500.0 * std::pow (2.0, 1.0 / 24.0));
    auto withSine = [&] (const Buf& p, double aboveDb)
    {
        const double amp = std::sqrt (2.0 * bandP * (std::pow (10.0, aboveDb / 10.0) - 1.0));
        Buf y = p;
        for (size_t i = 0; i < y.size(); ++i)
            y[i] += (float) (amp * std::sin (2.0 * pstest::kPi * 2500.0 * (double) i / kSr));
        return y;
    };
    struct Case { float sens; double above; };
    for (const Case c : { Case { 100.0f, 12.0 }, Case { 50.0f, 12.0 }, Case { 0.0f, 4.0 }, Case { 0.0f, 12.0 } })
    {
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Cut, 100.0f);
        rig.setBoth ("sens", c.sens);
        const Buf srcL = withSine (pl, c.above), srcR = withSine (pr, c.above);
        Buf l = srcL, r = srcR;
        double gr = 0.0, grOct = 0.0;
        int frames = 0;
        rig.run (l, r, kHop, [&] (size_t pos)
        {
            if (pos > (size_t) (4.0 * kSr))
            {
                gr += meterGain (rig.tb, 2500.0);
                grOct = std::min ({ grOct, meterGain (rig.tb, 1250.0), meterGain (rig.tb, 5000.0) });
                ++frames;
            }
        });
        gr /= frames;
        const int lat = rig.tb.getLatencySamples();
        const size_t m0 = (size_t) (5.0 * kSr);
        const double toneIn = std::abs (pstest::toneOf (delayed (srcL, lat), m0, 96000, 2500.0, kSr));
        const double toneOut = std::abs (pstest::toneOf (l, m0, 96000, 2500.0, kSr));
        const double grTone = 20.0 * std::log10 (toneOut / toneIn);
        note ("sens " + std::to_string ((int) c.sens) + ", sine " + std::to_string ((int) c.above)
              + " dB above: GR at 2.5 kHz (meters, dB)", gr);
        note ("  measured gain of the sine (dB)", grTone);
        note ("  deepest gain one octave away (dB)", grOct);
        if (c.sens == 100.0f)
        {
            CHECK (grTone <= -5.0);
            CHECK (grOct > -1.0);
        }
        if (c.sens == 0.0f && c.above < 5.0)
            CHECK (grTone > -1.0);
        if (c.sens == 0.0f && c.above > 10.0)
            CHECK (grTone > -2.0);   // sens 0: only resonances more than tol + 10 dB over the target
        if (c.sens == 50.0f)
            CHECK (grTone < -2.0 && grOct > -0.5);
    }
}

TEST_CASE ("tbal. speed sets the time constant of the correction")
{
    // a +6 dB high shelf at 3 kHz switches on at 8 s and off again later; time for the 8 kHz correction (relative to
    // 500 Hz) to reach 63 % of its change. The rise of the band is read with min (0.3 s, tauS) at every Speed (the
    // reference's attack); the fall back to plain pink follows Speed.
    struct Case { float speed; double onLo, onHi, offLo, offHi; };
    for (const Case c : { Case { 0.0f, 0.15, 0.6, 2.5, 10.0 }, Case { 50.0f, 0.15, 0.6, 0.3, 1.5 },
                          Case { 100.0f, 0.0, 0.12, 0.0, 0.25 } })
    {
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
        rig.setBoth ("speed", c.speed);
        const size_t onset = (size_t) (8.0 * kSr);
        const double onFor = c.speed == 0.0f ? 14.0 : 8.0, total = 8.0 + 2.0 * onFor;
        const size_t offset = onset + (size_t) (onFor * kSr);
        auto make = [&] (std::uint32_t seed)
        {
            const Buf x = pink (total, seed);
            Buf y = filtered (x, rbj (2, 3000.0, 6.0, 0.7, kSr), onset);
            for (size_t i = offset; i < y.size(); ++i)
                y[i] = x[i];
            return y;
        };
        const Buf srcL = make (91u), srcR = make (92u);
        Buf l = srcL, r = srcR;
        const int lat = rig.tb.getLatencySamples();
        std::vector<std::pair<double, double>> track;
        double before = 0.0;
        rig.run (l, r, kHop, [&] (size_t pos)
        {
            const double t = ((double) pos - (double) lat / 2.0 - (double) onset) / kSr;
            const double g = meterGain (rig.tb, 8000.0) - meterGain (rig.tb, 500.0);
            if (t < 0.0)
                before = g;
            track.push_back ({ t, g });
        });
        double fin = 0.0;
        int nf = 0;
        for (auto& p : track)
            if (p.first > onFor - 2.1 && p.first <= onFor - 0.1)
            {
                fin += p.second;
                ++nf;
            }
        fin /= std::max (nf, 1);
        double tOn = -1.0, tOff = -1.0;
        for (auto& p : track)
            if (p.first > 0.0 && p.second - before <= 0.63 * (fin - before))
            {
                tOn = p.first;
                break;
            }
        for (auto& p : track)
            if (p.first > onFor && p.second - fin >= 0.63 * (before - fin))
            {
                tOff = p.first - onFor;
                break;
            }
        note ("Speed " + std::to_string ((int) c.speed) + ": final correction at 8 kHz (dB)", fin);
        note ("  time to 63 % after the shelf comes on (s)", tOn);
        note ("  time to 63 % after it goes off (s)", tOff);
        // the 0.3 s attack reads noise bands a little above their power mean, the narrow ones most (500 Hz here),
        // so Speed 0 lands ~0.4 dB short of the faster settings on noise
        CHECK (fin < -2.5);
        CHECK (tOn >= c.onLo && tOn <= c.onHi);
        CHECK (tOff >= c.offLo && tOff <= c.offHi);
    }
}

TEST_CASE ("tbal. smoothing sets the width of the correction")
{
    // pink with a +10 dB, 1/6-octave bump at 1 kHz; width of the correction where it falls to half its depth
    // (relative to the curve's median, the loudness-neutral baseline)
    auto hzOf = [] (int i) { return 20.0 * std::pow (1000.0, i / 399.0); };
    for (float smooth : { 0.0f, 100.0f })
    {
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
        rig.setBoth ("smooth", smooth);
        Buf l = filtered (pink (10.0, 101u), rbj (0, 1000.0, 10.0, 8.65, kSr));
        Buf r = filtered (pink (10.0, 102u), rbj (0, 1000.0, 10.0, 8.65, kSr));
        std::vector<double> avg (400, 0.0);
        int frames = 0;
        rig.run (l, r, kHop, [&] (size_t pos)
        {
            if (pos > (size_t) (6.0 * kSr))
            {
                for (int i = 0; i < 400; ++i)
                    avg[(size_t) i] += meterGain (rig.tb, hzOf (i));
                ++frames;
            }
        });
        for (auto& a : avg)
            a /= frames;
        std::vector<double> mid;
        for (int i = 0; i < 400; ++i)
            if (hzOf (i) > 100.0 && hzOf (i) < 10000.0)
                mid.push_back (avg[(size_t) i]);
        std::sort (mid.begin(), mid.end());
        const double base = mid[mid.size() / 2];
        int peak = 0;
        for (int i = 0; i < 400; ++i)
            if (avg[(size_t) i] < avg[(size_t) peak])
                peak = i;
        const double depth = base - avg[(size_t) peak];
        int lo = peak, hi = peak;
        while (lo > 0 && base - avg[(size_t) lo] > 0.5 * depth)
            --lo;
        while (hi < 399 && base - avg[(size_t) hi] > 0.5 * depth)
            ++hi;
        const double width = std::log2 (hzOf (hi) / hzOf (lo));
        note (smooth > 0.0f ? "Smoothing 100: correction depth (dB)" : "Smoothing 0: correction depth (dB)", depth);
        note ("  peak at (Hz)", hzOf (peak));
        note ("  width at half depth (octaves)", width);
        if (smooth == 0.0f)
        {
            CHECK (depth >= 3.4);   // 70 % of a measured deviation (the reference's law)
            CHECK (width <= 0.5);
            CHECK (std::abs (std::log2 (hzOf (peak) / 1000.0)) < 0.1);
        }
        else
        {
            CHECK (width >= 2.0);
        }
    }
}

TEST_CASE ("tbal. region amounts act independently")
{
    // Low = 0 on a bass excess: no correction below 70 Hz, the mid region still corrects
    {
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
        rig.setBoth ("low", 0.0f);
        Buf l = filtered (pink (10.0, 111u), rbj (1, 150.0, 8.0, 0.7, kSr));
        Buf r = filtered (pink (10.0, 112u), rbj (1, 150.0, 8.0, 0.7, kSr));
        double worstLow = 0.0, midCorr = 0.0;
        rig.run (l, r, kHop, [&] (size_t pos)
        {
            if (pos > (size_t) (5.0 * kSr))
            {
                for (double f = 20.0; f < 70.0; f *= 1.05)
                    worstLow = std::max (worstLow, std::abs (meterGain (rig.tb, f)));
                midCorr = std::min (midCorr, meterGain (rig.tb, 150.0) - meterGain (rig.tb, 1000.0));
            }
        });
        note ("Low 0: largest |correction| below 70 Hz (dB)", worstLow);
        note ("  correction at 150 Hz rel. 1 kHz (the mid region still works, dB)", midCorr);
        CHECK (worstLow < 0.3);
        CHECK (midCorr < -1.0);
    }
    // High = 0 on a treble excess: no correction above 8 kHz
    {
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
        rig.setBoth ("high", 0.0f);
        Buf l = filtered (pink (10.0, 113u), rbj (2, 6000.0, 8.0, 0.7, kSr));
        Buf r = filtered (pink (10.0, 114u), rbj (2, 6000.0, 8.0, 0.7, kSr));
        double worstHigh = 0.0, midCorr = 0.0;
        rig.run (l, r, kHop, [&] (size_t pos)
        {
            if (pos > (size_t) (5.0 * kSr))
            {
                for (double f = 8000.0; f < 20000.0; f *= 1.05)
                    worstHigh = std::max (worstHigh, std::abs (meterGain (rig.tb, f)));
                midCorr = std::min (midCorr, meterGain (rig.tb, 4500.0) - meterGain (rig.tb, 1000.0));
            }
        });
        note ("High 0: largest |correction| above 8 kHz (dB)", worstHigh);
        note ("  correction at 4.5 kHz rel. 1 kHz (dB)", midCorr);
        CHECK (worstHigh < 0.3);
        CHECK (midCorr < -1.0);
    }
    // Mid = 0: none between 125 Hz and 5 kHz, the low region still corrects
    {
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
        rig.setBoth ("mid", 0.0f);
        Buf l = filtered (filtered (pink (10.0, 115u), rbj (0, 1000.0, 8.0, 1.0, kSr)), rbj (1, 60.0, 8.0, 0.7, kSr));
        Buf r = filtered (filtered (pink (10.0, 116u), rbj (0, 1000.0, 8.0, 1.0, kSr)), rbj (1, 60.0, 8.0, 0.7, kSr));
        double worstMid = 0.0, lowCorr = 0.0;
        rig.run (l, r, kHop, [&] (size_t pos)
        {
            if (pos > (size_t) (5.0 * kSr))
            {
                for (double f = 125.0; f < 5000.0; f *= 1.05)
                    worstMid = std::max (worstMid, std::abs (meterGain (rig.tb, f)));
                lowCorr = std::min (lowCorr, meterGain (rig.tb, 40.0));
            }
        });
        note ("Mid 0: largest |correction| 125 Hz .. 5 kHz (dB)", worstMid);
        note ("  correction at 40 Hz (the low region still works, dB)", lowCorr);
        CHECK (worstMid < 0.3);
        CHECK (lowCorr < -0.5);   // (the power-neutral anchor sits higher in the lows than the loudness-neutral one)
    }
}

TEST_CASE ("tbal. tame transients: the cut lands on the hit")
{
    // pink bed + 4 kHz noise bursts (+15 dB in their band, 5 ms rise, 100 ms long, every 500 ms), Cut, cutSpeed 30
    const double seconds = 8.0;
    const size_t n = (size_t) (seconds * kSr);
    const Buf bed = pink (seconds, 121u);
    const Buf burst = filtered (pstest::whiteNoise (1.0, seconds, kSr, 122u), rbj (3, 4000.0, 0.0, 2.0, kSr));
    const auto pb = welch (bed, 0, n), pbu = welch (burst, 0, n);
    const double scale = std::sqrt (std::pow (10.0, 1.5) * bandPower (pb, 4000.0 / 1.122, 4000.0 * 1.122)
                                    / bandPower (pbu, 4000.0 / 1.122, 4000.0 * 1.122));
    std::vector<size_t> onsets;
    Buf src = bed;
    const size_t period = (size_t) (0.5 * kSr);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) (i % period) / kSr;
        if (i % period == 0 && i > 0)
            onsets.push_back (i);
        const double env = t < 0.005 ? t / 0.005 : (t < 0.1 ? 1.0 : (t < 0.105 ? (0.105 - t) / 0.005 : 0.0));
        src[i] += (float) (scale * env * burst[i]);
    }
    double ratio[2] {};
    for (int tame = 0; tame < 2; ++tame)
    {
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Cut, 100.0f);
        rig.setBoth ("cutSpeed", 30.0f);
        rig.setBoth ("tame", (float) tame);
        Buf l = src, r = src;
        const int lat = rig.tb.getLatencySamples();
        std::vector<double> gr (n / kHop + 2, 0.0);
        rig.run (l, r, kHop, [&] (size_t pos) { gr[pos / kHop] = meterGain (rig.tb, 4000.0); });
        // per hit after 3 s: GR in the first hop whose frame centre has passed the onset, vs the deepest GR of the hit
        double sum = 0.0;
        int hits = 0;
        for (size_t on : onsets)
        {
            if (on < (size_t) (3.0 * kSr) || on + (size_t) (0.4 * kSr) > n)
                continue;
            const size_t first = (on + (size_t) lat / 2) / kHop + 1;
            double deepest = 0.0;
            for (size_t h = first; h < first + 12; ++h)
                deepest = std::min (deepest, gr[h]);
            sum += gr[first] / std::min (deepest, -1.0e-6);
            ++hits;
        }
        ratio[tame] = sum / std::max (hits, 1);
    }
    note ("without Tame: GR in the onset hop / GR of the hit", ratio[0]);
    note ("with Tame: GR in the onset hop / GR of the hit", ratio[1]);
    CHECK (ratio[1] >= 0.7);
    CHECK (ratio[0] < 0.4);
}

TEST_CASE ("tbal. stereo link, M/S sets and the T/S twin")
{
    // Stereo: a tone only in L; the noise in R sees exactly the same curve as the noise in L
    {
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Cut, 100.0f);
        rig.setBoth ("sens", 100.0f);
        const Buf srcL = pstest::added (pink (8.0, 131u), pstest::sine (3000.0, -14.0, 8.0, kSr));
        const Buf srcR = pink (8.0, 132u);
        Buf l = srcL, r = srcR;
        rig.run (l, r);
        const int lat = rig.tb.getLatencySamples();
        const size_t m0 = (size_t) (4.0 * kSr), len = (size_t) (4.0 * kSr) - (size_t) lat;
        const auto inL = welch (delayed (srcL, lat), m0, len), outL = welch (l, m0, len);
        const auto inR = welch (delayed (srcR, lat), m0, len), outR = welch (r, m0, len);
        double worst = 0.0;
        for (double fc : kThirdOctaves)
        {
            if (fc > 2000.0 && fc < 4500.0)
                continue;   // the L band holds the tone
            const double hl = bandDb (outL, fc) - bandDb (inL, fc), hr = bandDb (outR, fc) - bandDb (inR, fc);
            worst = std::max (worst, std::abs (hl - hr));
        }
        const double atTone = bandDb (outR, 3000.0, 1.0 / 12.0) - bandDb (inR, 3000.0, 1.0 / 12.0);
        note ("Stereo: max |H_L - H_R| over 1/3-oct bands (dB)", worst);
        note ("  R noise gain at the L tone (dB)", atTone);
        // the same bin gains on both channels; the Welch estimate differs by a little because the L noise itself
        // drives the detector (its gain correlates with its own short-term level), the R noise does not
        CHECK (worst < 0.15);
        CHECK (atTone < -2.0);   // the R noise is cut where only L has the tone: one linked curve
        for (int i = 0; i < kToneDisplayPoints; ++i)
            CHECK_EQ (rig.tb.getMeters().gainDb[0][i].load(), rig.tb.getMeters().gainDb[1][i].load());
    }
    // M/S: set B Amount 0 -> the Side channel nulls exactly, Mid is processed
    {
        Rig rig;
        rig.mode = ChannelMode::MidSide;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
        rig.set ("tbal.b.amount", 0.0f);
        const Buf srcM = filtered (pink (6.0, 133u), rbj (1, 120.0, 8.0, 0.7, kSr));
        const Buf srcS = filtered (pink (6.0, 134u, 0.2), rbj (2, 4000.0, 8.0, 0.7, kSr));
        Buf m = srcM, s = srcS;
        rig.run (m, s);
        const int lat = rig.tb.getLatencySamples();
        const double sideDiff = maxAbsDiff (s, delayed (srcS, lat));
        const double midDiff = maxAbsDiff (m, delayed (srcM, lat));
        note ("M/S, set B Amount 0: max |Side out - Side in delayed|", sideDiff);
        note ("  max |Mid out - Mid in delayed|", midDiff);
        CHECK (sideDiff < 1.0e-7);   // < -140 dBFS: float rounding of the shared packed FFT (SpectralEngine)
        CHECK (midDiff > 1.0e-3);
        CHECK (std::abs (rig.tb.getMeters().maxCutDb[0].load()) > 1.0f);
        CHECK_EQ (rig.tb.getMeters().maxCutDb[1].load(), 0.0f);
        CHECK_EQ (rig.tb.getMeters().maxBoostDb[1].load(), 0.0f);
    }
    // M/S with both sets on: each channel follows its own measurement (Side treble excess -> Side treble cut only)
    {
        Rig rig;
        rig.mode = ChannelMode::MidSide;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
        Buf m = filtered (pink (8.0, 137u), rbj (1, 120.0, 8.0, 0.7, kSr));
        Buf s = filtered (pink (8.0, 138u, 0.2), rbj (2, 4000.0, 8.0, 0.7, kSr));
        rig.run (m, s);
        const double midLow = meterGain (rig.tb, 50.0, 0) - meterGain (rig.tb, 1000.0, 0);
        const double sideHigh = meterGain (rig.tb, 10000.0, 1) - meterGain (rig.tb, 1000.0, 1);
        const double midHigh = meterGain (rig.tb, 10000.0, 0) - meterGain (rig.tb, 1000.0, 0);
        note ("M/S: Mid correction at 50 Hz (dB)", midLow);
        note ("  Side correction at 10 kHz (dB)", sideHigh);
        note ("  Mid correction at 10 kHz (dB)", midHigh);
        CHECK (midLow < -3.0);
        CHECK (sideHigh < -3.0);
        CHECK (std::abs (midHigh) < 1.0);
    }
    // T/S twin (paramSet 1) reads set B for both channels: set A 100 / set B 0 nulls; set B = X gives exactly what a
    // normal instance gives with set A = X
    {
        const Buf srcL = filtered (pink (6.0, 135u), rbj (1, 120.0, 8.0, 0.7, kSr));
        const Buf srcR = filtered (pink (6.0, 136u), rbj (1, 120.0, 8.0, 0.7, kSr));
        Rig twin (kSr, 1);
        twin.common (ToneTarget::Flat, ToneBalanceMode::Shape, 0.0f);
        twin.set ("tbal.a.amount", 100.0f);
        Buf l = srcL, r = srcR;
        twin.run (l, r);
        const int lat = twin.tb.getLatencySamples();
        const double nullDiff = std::max (maxAbsDiff (l, delayed (srcL, lat)), maxAbsDiff (r, delayed (srcR, lat)));
        note ("twin, set A 100 / set B 0: max |out - in delayed|", nullDiff);
        CHECK_EQ (nullDiff, 0.0);

        Rig twin2 (kSr, 1), normal (kSr, 0);
        twin2.common (ToneTarget::Flat, ToneBalanceMode::Shape, 0.0f);
        twin2.set ("tbal.b.amount", 70.0f);
        twin2.set ("tbal.b.speed", 80.0f);
        twin2.set ("tbal.b.low", 40.0f);
        normal.common (ToneTarget::Flat, ToneBalanceMode::Shape, 0.0f);
        normal.set ("tbal.a.amount", 70.0f);
        normal.set ("tbal.a.speed", 80.0f);
        normal.set ("tbal.a.low", 40.0f);
        Buf l2 = srcL, r2 = srcR, l3 = srcL, r3 = srcR;
        twin2.run (l2, r2);
        normal.run (l3, r3);
        const double same = std::max (maxAbsDiff (l2, l3), maxAbsDiff (r2, r3));
        note ("twin set B vs normal set A with the same values: max diff", same);
        note ("  twin correction (dB)", twin2.tb.getMeters().maxCutDb[0].load());
        CHECK_EQ (same, 0.0);
        CHECK (twin2.tb.getMeters().maxCutDb[0].load() < -0.5f);
        CHECK_EQ (twin2.tb.getLatencySamples(), normal.tb.getLatencySamples());
    }
}

namespace
{
// normalised spectral flux of x over [start, start + len): mean over frames of sum_k (|X_t| - |X_t-1|)^2 / sum_k |X_t|^2
double spectralFlux (const Buf& x, size_t start, size_t len)
{
    const size_t nfft = 2048, hop = 512;
    std::vector<double> w (nfft), prev (nfft / 2 + 1, 0.0), cur (nfft / 2 + 1, 0.0);
    for (size_t j = 0; j < nfft; ++j)
        w[j] = 0.5 - 0.5 * std::cos (2.0 * pstest::kPi * (double) j / (double) nfft);
    std::vector<pstest::cd> a (nfft);
    double num = 0.0, den = 0.0;
    bool first = true;
    for (size_t s = start; s + nfft <= start + len; s += hop)
    {
        for (size_t j = 0; j < nfft; ++j)
            a[j] = (double) x[s + j] * w[j];
        pstest::fftInPlace (a);
        for (size_t k = 0; k <= nfft / 2; ++k)
            cur[k] = std::abs (a[k]);
        if (! first)
            for (size_t k = 1; k <= nfft / 2; ++k)
            {
                num += (cur[k] - prev[k]) * (cur[k] - prev[k]);
                den += cur[k] * cur[k];
            }
        first = false;
        prev.swap (cur);
    }
    return num / std::max (den, 1.0e-30);
}

// energy outside +-guard Hz of the given tones (20 Hz .. 20 kHz) relative to the total, dB (Blackman-Harris, 65536)
double splatterDb (const Buf& x, size_t start, const std::vector<double>& tones, double guard)
{
    const size_t nfft = 65536;
    std::vector<pstest::cd> a (nfft);
    for (size_t j = 0; j < nfft; ++j)
    {
        const double t = 2.0 * pstest::kPi * (double) j / (double) nfft;
        const double w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2 * t) - 0.01168 * std::cos (3 * t);
        a[j] = (double) x[start + j] * w;
    }
    pstest::fftInPlace (a);
    double out = 0.0, total = 0.0;
    for (size_t k = 1; k < nfft / 2; ++k)
    {
        const double f = (double) k * kSr / (double) nfft;
        const double p = std::norm (a[k]);
        total += p;
        if (f < 20.0 || f > 20000.0)
            continue;
        bool near = false;
        for (double t : tones)
            near = near || std::abs (f - t) < guard;
        if (! near)
            out += p;
    }
    return 10.0 * std::log10 (out / total + 1.0e-300);
}
} // namespace

TEST_CASE ("tbal. no musical noise: steady gains on steady noise, no added spectral flux")
{
    const char* names[] { "Shape", "Cut", "Boost" };
    for (int m = 0; m < 3; ++m)
    {
        Rig rig;
        rig.common (ToneTarget::Balanced, (ToneBalanceMode) m, 100.0f);
        const Buf srcL = pink (10.0, 141u), srcR = pink (10.0, 142u);
        Buf l = srcL, r = srcR;
        std::vector<std::vector<double>> frames;
        rig.run (l, r, kHop, [&] (size_t pos)
        {
            if (pos > (size_t) (6.0 * kSr) && pos <= (size_t) (8.0 * kSr))
            {
                std::vector<double> g;
                for (int i = 0; i < kToneDisplayPoints; ++i)
                    g.push_back (rig.tb.getMeters().gainDb[0][i].load());
                frames.push_back (g);
            }
        });
        double worstStd = 0.0, meanStep = 0.0, maxAbs = 0.0;
        int steps = 0;
        for (int i = displayIndex (25.0); i <= displayIndex (16000.0); ++i)
        {
            double s = 0.0, s2 = 0.0;
            for (size_t f = 0; f < frames.size(); ++f)
            {
                s += frames[f][(size_t) i];
                s2 += frames[f][(size_t) i] * frames[f][(size_t) i];
                maxAbs = std::max (maxAbs, std::abs (frames[f][(size_t) i]));
                if (f > 0)
                {
                    meanStep += std::abs (frames[f][(size_t) i] - frames[f - 1][(size_t) i]);
                    ++steps;
                }
            }
            const double n = (double) frames.size();
            worstStd = std::max (worstStd, std::sqrt (std::max (0.0, s2 / n - (s / n) * (s / n))));
        }
        meanStep /= std::max (steps, 1);
        const int lat = rig.tb.getLatencySamples();
        const size_t m0 = (size_t) (6.0 * kSr), len = (size_t) (3.5 * kSr);
        const double fluxRatio = spectralFlux (l, m0, len) / spectralFlux (delayed (srcL, lat), m0, len);
        note (std::string (names[m]) + " on pink: largest gain std over 2 s, 25 Hz .. 16 kHz (dB)", worstStd);
        note ("  mean |gain change| per hop (dB)", meanStep);
        note ("  largest |gain| (dB)", maxAbs);
        note ("  spectral flux out / in", fluxRatio);
        CHECK (worstStd < 0.5);
        CHECK (meanStep < 0.05);
        CHECK (fluxRatio <= 1.1);
    }
    // music-like material: the curve may follow the music, but the output's flux stays that of the input
    for (int m = 0; m < 3; ++m)
    {
        Rig rig;
        rig.common (ToneTarget::Pop, (ToneBalanceMode) m, 100.0f);
        rig.setBoth ("speed", 70.0f);
        const Buf srcL = musicLike (9.0, 143u), srcR = musicLike (9.0, 144u);
        Buf l = srcL, r = srcR;
        rig.run (l, r);
        const int lat = rig.tb.getLatencySamples();
        const double fluxRatio = spectralFlux (l, (size_t) (5.0 * kSr), (size_t) (3.5 * kSr))
                               / spectralFlux (delayed (srcL, lat), (size_t) (5.0 * kSr), (size_t) (3.5 * kSr));
        note (std::string (names[m]) + " on music-like: spectral flux out / in", fluxRatio);
        CHECK (fluxRatio <= 1.1);
    }
}

TEST_CASE ("tbal. fast automation of every control is smooth (no splatter)")
{
    // two tones (200 Hz at -12 dBFS, 3 kHz at -24 dBFS) give a real correction curve; the automated run changes
    // controls every block (amount 0 / 100, mode, target, both speeds, smoothing, all regions, sensitivity, Tame)
    const double seconds = 6.0;
    const Buf src = pstest::added (pstest::sine (200.0, -12.0, seconds, kSr), pstest::sine (3000.0, -24.0, seconds, kSr));
    double splat[2] {}, corr[2] {};
    for (int automated = 0; automated < 2; ++automated)
    {
        Rig rig;
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
        Buf l = src, r = src;
        int blockIndex = 0;
        double maxCorr = 0.0;
        for (size_t pos = 0; pos < l.size(); pos += kHop, ++blockIndex)
        {
            if (automated != 0)
            {
                const int b = blockIndex;
                rig.setBoth ("amount", (b / 4) % 2 == 0 ? 100.0f : 0.0f);
                rig.set ("tbal.mode", (float) ((b / 7) % 3));
                rig.set ("tbal.target", (float) ((b / 5) % 36));
                rig.setBoth ("speed", (b / 3) % 2 == 0 ? 0.0f : 100.0f);
                rig.setBoth ("cutSpeed", (b / 5) % 2 == 0 ? 100.0f : 0.0f);
                rig.setBoth ("smooth", (b / 2) % 2 == 0 ? 0.0f : 100.0f);
                rig.setBoth ("sens", (b / 3) % 2 == 0 ? 100.0f : 0.0f);
                rig.setBoth ("low", (b / 6) % 2 == 0 ? 100.0f : 0.0f);
                rig.setBoth ("mid", (b / 8) % 2 == 0 ? 100.0f : 0.0f);
                rig.setBoth ("high", (b / 9) % 2 == 0 ? 0.0f : 100.0f);
                rig.setBoth ("tame", (b / 11) % 2 == 0 ? 1.0f : 0.0f);
            }
            const int n = (int) std::min<size_t> (kHop, l.size() - pos);
            rig.block (l.data() + pos, r.data() + pos, n);
            if (pos >= (size_t) (2.0 * kSr))
                maxCorr = std::max ({ maxCorr, (double) rig.tb.getMeters().maxBoostDb[0].load(),
                                      -(double) rig.tb.getMeters().maxCutDb[0].load() });
        }
        splat[automated] = splatterDb (l, (size_t) (2.0 * kSr), { 200.0, 3000.0 }, 500.0);
        corr[automated] = maxCorr;
    }
    const double inputSplat = splatterDb (src, (size_t) (2.0 * kSr), { 200.0, 3000.0 }, 500.0);
    note ("input: energy > 500 Hz away from the tones (dB re total)", inputSplat);
    note ("static Amount 100: (dB re total)", splat[0]);
    note ("automated every block: (dB re total)", splat[1]);
    note ("largest correction while automating (dB)", corr[1]);
    // bare tones with an 11 dB cut are the worst case of a residual STFT (its reconstruction error is not masked by
    // anything); -90 dB re the total is about -102 dBFS here
    CHECK (corr[0] > 3.0);
    CHECK (corr[1] > 0.5);
    CHECK (splat[0] < -85.0);
    CHECK (splat[1] < -95.0);
}

TEST_CASE ("tbal. mode and target changes glide the curve")
{
    Rig rig;
    rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
    const Buf src = filtered (pink (12.0, 151u), rbj (1, 120.0, 8.0, 0.7, kSr));
    Buf l = src, r = src;
    std::vector<double> g60;
    rig.run (l, r, kHop, [&] (size_t pos)
    {
        if (pos >= (size_t) (5.0 * kSr))
            rig.set ("tbal.target", (float) (int) ToneTarget::Trap);
        if (pos >= (size_t) (8.0 * kSr))
            rig.set ("tbal.mode", (float) (int) ToneBalanceMode::Boost);
        g60.push_back (meterGain (rig.tb, 60.0));
    });
    double maxStep = 0.0;
    const size_t from = (size_t) (4.5 * kSr) / kHop;
    for (size_t i = from + 1; i < g60.size(); ++i)
        maxStep = std::max (maxStep, std::abs (g60[i] - g60[i - 1]));
    note ("60 Hz correction before the target change (dB)", g60[from]);
    note ("  after the change to Trap (dB)", g60[(size_t) (7.5 * kSr) / kHop]);
    note ("  after the change to Boost mode (dB)", g60.back());
    note ("largest change of the 60 Hz gain per hop (dB)", maxStep);
    CHECK (g60[from] < -1.7);   // an 8 dB, 120 Hz shelf: 70 % of the measured deviation, smoothed
    CHECK (g60[(size_t) (7.5 * kSr) / kHop] > g60[from] + 2.0);   // Trap wants much more bass
    CHECK (g60.back() >= 0.0);
    CHECK (maxStep < 1.2);
}

TEST_CASE ("tbal. stable under extreme input and random automation; silence and denormals")
{
    // random automation of everything each block on loud material
    {
        Rig rig;
        pstest::Lcg lcg (999u);
        Buf l = pstest::scaled (musicLike (6.0, 161u), 2.0f), r = pstest::scaled (musicLike (6.0, 162u), 2.0f);
        for (size_t i = 0; i < l.size(); i += 9973)
            l[i] = 1.0f;   // spikes
        bool finite = true;
        double peak = 0.0;
        for (size_t pos = 0; pos < l.size(); pos += 300)
        {
            rig.set ("tbal.mode", (float) std::min (2, (int) ((lcg.next() + 1.0f) * 1.5f)));
            rig.set ("tbal.target", (float) std::min (35, (int) ((lcg.next() + 1.0f) * 18.0f)));
            for (const char* id : { "amount", "speed", "smooth", "cutSpeed", "sens", "low", "mid", "high" })
                rig.setBoth (id, lcg.next() > 0.0f ? 100.0f : (lcg.next() + 1.0f) * 50.0f);
            rig.setBoth ("tame", lcg.next() > 0.0f ? 1.0f : 0.0f);
            rig.mode = lcg.next() > 0.9f ? ChannelMode::MidSide : ChannelMode::Stereo;
            const int n = (int) std::min<size_t> (300, l.size() - pos);
            rig.block (l.data() + pos, r.data() + pos, n);
        }
        for (size_t i = 0; i < l.size(); ++i)
        {
            finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
            peak = std::max ({ peak, (double) std::abs (l[i]), (double) std::abs (r[i]) });
        }
        note ("random automation: output peak (input peak ~2)", peak);
        CHECK (finite);
        CHECK (peak < 8.0);
    }
    // full-scale square wave, DC, tiny values
    struct Case { const char* name; float value; bool square; };
    for (const Case c : { Case { "full-scale square 100 Hz", 1.0f, true }, Case { "DC 0.9", 0.9f, false },
                          Case { "1e-30", 1.0e-30f, true }, Case { "denormal 1e-40", 1.0e-40f, true } })
    {
        Rig rig;
        rig.common (ToneTarget::Balanced, ToneBalanceMode::Shape, 100.0f);
        Buf l ((size_t) (3.0 * kSr)), r;
        for (size_t i = 0; i < l.size(); ++i)
            l[i] = c.square ? ((i / 240) % 2 == 0 ? c.value : -c.value) : c.value;
        r = l;
        pstest::Stopwatch sw;
        rig.run (l, r);
        const double sec = sw.seconds();
        bool finite = true;
        double peak = 0.0;
        for (size_t i = 0; i < l.size(); ++i)
        {
            finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
            peak = std::max (peak, (double) std::abs (l[i]));
        }
        note (std::string (c.name) + ": output peak / input level", peak / std::abs ((double) c.value));
        note ("  seconds to process 3 s", sec);
        CHECK (finite);
        CHECK (peak <= 4.0 * std::abs ((double) c.value) + 1.0e-35);
        CHECK (sec < 0.5);
    }
    // silence after noise: the correction is exactly 0 once no frame holds signal (2 N after the end), and inaudible
    // one frame earlier
    {
        Rig rig;
        rig.common (ToneTarget::Balanced, ToneBalanceMode::Shape, 100.0f);
        Buf l = pink (8.0, 171u), r = pink (8.0, 172u);
        const size_t end = (size_t) (3.0 * kSr);
        std::fill (l.begin() + (long long) end, l.end(), 0.0f);
        std::fill (r.begin() + (long long) end, r.end(), 0.0f);
        rig.run (l, r);
        const int lat = rig.tb.getLatencySamples();
        const double after2N = std::max (pstest::peakOf (l, end + 2 * (size_t) lat, l.size() - end - 2 * (size_t) lat),
                                         pstest::peakOf (r, end + 2 * (size_t) lat, r.size() - end - 2 * (size_t) lat));
        const double afterNH = pstest::peakOf (l, end + (size_t) lat + kHop, (size_t) lat);
        note ("peak after end + 2N", after2N);
        note ("peak in the frame after end + N + H (dBFS)", pstest::dbOf (afterNH));
        CHECK_EQ (after2N, 0.0);
        CHECK (pstest::dbOf (afterNH) < -60.0);
        // and it recovers when signal returns
        Buf l2 = pink (3.0, 173u), r2 = pink (3.0, 174u);
        rig.run (l2, r2);
        bool finite = true;
        for (float v : l2)
            finite = finite && std::isfinite (v);
        CHECK (finite);
    }
}

TEST_CASE ("tbal. block size does not change the result")
{
    const Buf srcL = musicLike (4.0, 181u), srcR = musicLike (4.0, 182u);
    Buf ref[2];
    bool first = true;
    double worst = 0.0;
    for (int bs : { 4096, 1, 7, 64, 512 })
    {
        Rig rig;
        rig.common (ToneTarget::Rock, ToneBalanceMode::Shape, 80.0f);
        rig.setBoth ("tame", 1.0f);
        Buf l = srcL, r = srcR;
        rig.run (l, r, bs);
        if (first)
        {
            ref[0] = l;
            ref[1] = r;
            first = false;
        }
        else
        {
            worst = std::max ({ worst, maxAbsDiff (l, ref[0]), maxAbsDiff (r, ref[1]) });
        }
    }
    note ("max difference between block sizes 1, 7, 64, 512, 4096", worst);
    CHECK_EQ (worst, 0.0);
}

TEST_CASE ("tbal. meters match the audio")
{
    Rig rig;
    rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
    const Buf srcL = filtered (filtered (pink (10.0, 191u), rbj (1, 120.0, 8.0, 0.7, kSr)), rbj (2, 5000.0, -6.0, 0.7, kSr));
    const Buf srcR = filtered (filtered (pink (10.0, 192u), rbj (1, 120.0, 8.0, 0.7, kSr)), rbj (2, 5000.0, -6.0, 0.7, kSr));
    Buf l = srcL, r = srcR;
    std::vector<double> avg (std::size (kThirdOctaves), 0.0), level (kToneDisplayPoints, 0.0);
    int frames = 0;
    const size_t m0 = (size_t) (6.0 * kSr), len = (size_t) (3.5 * kSr);
    const int lat = rig.tb.getLatencySamples();
    rig.run (l, r, kHop, [&] (size_t pos)
    {
        // the meters of the frame that ends at input time pos describe the output around pos - N / 2 + N
        const double outTime = (double) pos + (double) lat / 2.0;
        if (outTime >= (double) m0 && outTime < (double) (m0 + len))
        {
            for (size_t i = 0; i < avg.size(); ++i)
                avg[i] += meterGain (rig.tb, kThirdOctaves[i]);
            for (int i = 0; i < kToneDisplayPoints; ++i)
                level[(size_t) i] += rig.tb.getMeters().levelDb[0][i].load();
            ++frames;
        }
    });
    const auto tr = transfer (srcL, srcR, l, r, lat, m0, len);
    double worst = 0.0;
    for (size_t i = 0; i < avg.size(); ++i)
    {
        if (kThirdOctaves[i] < 63.0 || kThirdOctaves[i] > 12500.0)
            continue;
        worst = std::max (worst, std::abs (avg[i] / frames - tr.db (kThirdOctaves[i])));
    }
    note ("max |meter gain - measured 1/3-oct gain| 63 Hz .. 12.5 kHz (dB)", worst);
    note ("  meter / measured at 63 Hz (dB)", avg[3] / frames);
    note ("  measured at 63 Hz (dB)", tr.db (63.0));
    CHECK (worst < 0.5);

    // the level meter shows the input's shape (octave density): pink + 8 dB below 120 Hz, -6 dB above 5 kHz
    const double l40 = level[(size_t) displayIndex (40.0)] / frames, l1k = level[(size_t) displayIndex (1000.0)] / frames;
    const double l12k = level[(size_t) displayIndex (12000.0)] / frames;
    note ("level meter 40 Hz rel. 1 kHz (dB)", l40 - l1k);
    note ("level meter 12 kHz rel. 1 kHz (dB)", l12k - l1k);
    note ("target offset (dB)", rig.tb.getMeters().targetOffsetDb[0].load());
    CHECK_RANGE (l40 - l1k, 6.5, 9.0);
    CHECK_RANGE (l12k - l1k, -7.0, -4.5);
    CHECK (std::abs (rig.tb.getMeters().targetOffsetDb[0].load() - l1k) < 1.5);
}

TEST_CASE ("tbal. works the same at 44.1, 96 and 192 kHz")
{
    for (double sr : { 44100.0, 96000.0, 192000.0 })
    {
        Rig rig (sr);
        rig.common (ToneTarget::Flat, ToneBalanceMode::Shape, 100.0f);
        Buf l = filtered (pstest::pinkNoise (0.5, 8.0, sr, 201u), rbj (1, 120.0, 8.0, 0.7, sr));
        Buf r = filtered (pstest::pinkNoise (0.5, 8.0, sr, 202u), rbj (1, 120.0, 8.0, 0.7, sr));
        rig.run (l, r, 1024);
        const double g60 = meterGain (rig.tb, 60.0), g1k = meterGain (rig.tb, 1000.0), g10k = meterGain (rig.tb, 10000.0);
        note ("fs " + std::to_string ((int) sr) + ": correction at 60 Hz rel. 1 kHz (dB)", g60 - g1k);
        note ("  at 10 kHz rel. 1 kHz (dB)", g10k - g1k);
        CHECK (g60 - g1k < -3.5);
        CHECK (std::abs (g10k - g1k) < 1.0);
    }
}

TEST_CASE ("tbal. realtime: no allocation in setParams, process and reset")
{
    Rig rig (kSr, 0, 4096);
    Rig twin (kSr, 1, 4096);
    Buf l = musicLike (3.0, 211u), r = musicLike (3.0, 212u);
    long long count = 0;
    {
        pstest::AllocationScope scope;
        size_t pos = 0;
        int step = 0;
        for (int mode = 0; mode < 3; ++mode)
            for (int t : { 0, 7, 35 })
                for (auto cm : { ChannelMode::Stereo, ChannelMode::MidSide })
                    for (int bs : { 1, 7, 64, 512, 4096 })
                    {
                        auto& rg = (step % 2 == 0) ? rig : twin;
                        rg.mode = rg.tb.getParamSet() == 1 ? ChannelMode::Stereo : cm;
                        rg.common ((ToneTarget) t, (ToneBalanceMode) mode, 100.0f);
                        rg.setBoth ("smooth", (float) ((step * 37) % 101));
                        rg.setBoth ("tame", step % 3 == 0 ? 1.0f : 0.0f);
                        for (int rep = 0; rep < 4; ++rep)
                        {
                            if (pos + (size_t) bs > l.size())
                                pos = 0;
                            rg.block (l.data() + pos, r.data() + pos, bs);
                            pos += (size_t) bs;
                        }
                        if (step % 17 == 0)
                            rg.tb.reset();
                        ++step;
                    }
        count = scope.count();
    }
    note ("allocations", (double) count);
    CHECK_EQ (count, 0LL);
}

TEST_CASE ("tbal. cpu benchmark (48 kHz stereo)")
{
    const double seconds = 20.0;
    const Buf srcL = musicLike (seconds, 221u), srcR = musicLike (seconds, 222u);
    struct Case { const char* name; ToneBalanceMode mode; ChannelMode cm; bool automate; };
    double worst = 1.0e9;
    for (const Case c : { Case { "Shape, Stereo (typical)", ToneBalanceMode::Shape, ChannelMode::Stereo, false },
                          Case { "Cut, Stereo", ToneBalanceMode::Cut, ChannelMode::Stereo, false },
                          Case { "Shape, M/S, Smoothing automated every block (worst)", ToneBalanceMode::Shape,
                                 ChannelMode::MidSide, true } })
    {
        Rig rig;
        rig.mode = c.cm;
        rig.common (ToneTarget::Pop, c.mode, 100.0f);
        rig.setBoth ("tame", 1.0f);
        Buf l = srcL, r = srcR;
        int b = 0;
        pstest::Stopwatch sw;
        for (size_t pos = 0; pos < l.size(); pos += 512, ++b)
        {
            if (c.automate)
                rig.setBoth ("smooth", (float) (b % 100));
            rig.block (l.data() + pos, r.data() + pos, (int) std::min<size_t> (512, l.size() - pos));
        }
        const double x = seconds / sw.seconds();
        note (std::string (c.name) + ": x realtime", x);
        worst = std::min (worst, x);
    }
    CHECK (worst >= 80.0);
}

TEST_CASE ("tbal. proposed factory presets behave on music-like material")
{
    struct Preset
    {
        const char* name;
        ToneTarget target;
        ToneBalanceMode mode;
        float amount, speed, smooth, cutSpeed, sens, tame, low, mid, high;
    };
    const Preset presets[] {
        { "Soft Sheen", ToneTarget::Balanced, ToneBalanceMode::Shape, 35, 40, 70, 50, 50, 0, 100, 100, 100 },
        { "Steady Low End", ToneTarget::HipHop, ToneBalanceMode::Shape, 50, 55, 60, 50, 50, 0, 100, 30, 0 },
        { "Resonance Sweep", ToneTarget::Balanced, ToneBalanceMode::Cut, 60, 50, 50, 70, 55, 0, 100, 100, 100 },
        { "Calm the Hits", ToneTarget::Balanced, ToneBalanceMode::Cut, 30, 50, 50, 85, 40, 1, 0, 100, 100 },
    };
    const Buf srcL = musicLike (10.0, 231u), srcR = musicLike (10.0, 232u);
    for (const auto& p : presets)
    {
        Rig rig;
        rig.common (p.target, p.mode, p.amount);
        rig.setBoth ("speed", p.speed);
        rig.setBoth ("smooth", p.smooth);
        rig.setBoth ("cutSpeed", p.cutSpeed);
        rig.setBoth ("sens", p.sens);
        rig.setBoth ("tame", p.tame);
        rig.setBoth ("low", p.low);
        rig.setBoth ("mid", p.mid);
        rig.setBoth ("high", p.high);
        Buf l = srcL, r = srcR;
        double maxBoost = 0.0, maxCut = 0.0;
        rig.run (l, r, kHop, [&] (size_t pos)
        {
            if (pos > (size_t) (4.0 * kSr))
            {
                maxBoost = std::max (maxBoost, (double) rig.tb.getMeters().maxBoostDb[0].load());
                maxCut = std::min (maxCut, (double) rig.tb.getMeters().maxCutDb[0].load());
            }
        });
        const int lat = rig.tb.getLatencySamples();
        const size_t m0 = (size_t) (4.0 * kSr), len = (size_t) (6.0 * kSr) - (size_t) lat;
        const double dLu = integratedLufs (l, r, m0, len)
                         - integratedLufs (delayed (srcL, lat), delayed (srcR, lat), m0, len);
        const double dPow = powerDb (l, r, m0, len) - powerDb (delayed (srcL, lat), delayed (srcR, lat), m0, len);
        bool finite = true;
        for (size_t i = 0; i < l.size(); ++i)
            finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
        note (std::string (p.name) + ": loudness change (LU)", dLu);
        note ("  largest boost / cut (dB)", maxBoost);
        note ("  ", maxCut);
        CHECK (finite);
        CHECK (maxBoost <= toneBalanceMaxBoostDb (p.amount / 100.0) + 0.01);
        if (p.mode == ToneBalanceMode::Shape)
            CHECK (std::abs (dPow) < 0.5);
        else
            CHECK (maxBoost == 0.0 && dLu <= 0.01);
        CHECK (maxCut < -0.4);
    }
}
