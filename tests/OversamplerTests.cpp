#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/Oversampler.h"

#include <cmath>
#include <string>
#include <vector>

// Oversampler (dsp/Oversampler.cpp). Prefix "os.".
namespace
{
using namespace ps;
using Quality = Oversampler::Quality;

const char* qName (Quality q) { return q == Quality::LinearPhase ? "linear" : "minimum"; }

// Interpolator impulse response at the top rate (normalised so the passband is 0 dB).
std::vector<double> upsampledImpulse (int factorLog2, Quality q, int n)
{
    Oversampler os;
    os.prepare (factorLog2, q, n, 2);
    std::vector<float> in ((size_t) n, 0.0f);
    in[0] = 1.0f;
    const float* chans[2] { in.data(), in.data() };
    float* const* up = os.upsample (chans, n);
    const int F = os.getFactor();
    std::vector<double> h ((size_t) (n * F));
    for (size_t i = 0; i < h.size(); ++i)
        h[i] = up[0][i] / (double) F;
    return h;
}

// Up -> down round trip of x in blocks of varying size.
std::vector<float> roundTrip (Oversampler& os, const std::vector<float>& x, int maxBlock)
{
    std::vector<float> y (x.size());
    std::vector<float> outL ((size_t) maxBlock), outR ((size_t) maxBlock);
    size_t pos = 0;
    int k = 0;
    while (pos < x.size())
    {
        const int n = (int) std::min ((size_t) (1 + (k++ * 97) % maxBlock), x.size() - pos);
        const float* in[2] { x.data() + pos, x.data() + pos };
        os.upsample (in, n);
        float* out[2] { outL.data(), outR.data() };
        os.downsample (out, n);
        std::copy (outL.begin(), outL.begin() + n, y.begin() + (std::ptrdiff_t) pos);
        pos += (size_t) n;
    }
    return y;
}

// Level (dB) of what comes out at the base rate when a top-rate tone at `hzTop` is decimated.
double decimatedLevelDb (int factorLog2, Quality q, double sr, double hzTop)
{
    Oversampler os;
    const int block = 256;
    os.prepare (factorLog2, q, block, 2);
    const int F = os.getFactor();
    const double srTop = sr * F;
    std::vector<float> zeros ((size_t) block, 0.0f), out ((size_t) block), out2 ((size_t) block);
    std::vector<float> y;
    size_t t = 0;
    for (int b = 0; b < 200; ++b)
    {
        const float* in[2] { zeros.data(), zeros.data() };
        float* const* up = os.upsample (in, block);
        for (int i = 0; i < block * F; ++i, ++t)
            up[0][i] = up[1][i] = (float) (0.5 * std::sin (2.0 * pstest::kPi * hzTop * (double) t / srTop));
        float* o[2] { out.data(), out2.data() };
        os.downsample (o, block);
        y.insert (y.end(), out.begin(), out.end());
    }
    const double inRms = 0.5 / std::sqrt (2.0);
    return pstest::dbOf (pstest::rmsOf (y, y.size() / 2, y.size() / 2) / inRms);
}
} // namespace

TEST_CASE ("os. linear phase: stopband >= 100 dB, passband ripple < 0.01 dB up to 0.45 fs")
{
    const double sr = 44100.0;
    for (int f = 1; f <= 3; ++f)
    {
        const int n = 4096;
        const auto h = upsampledImpulse (f, Quality::LinearPhase, n);
        const size_t N = h.size();
        const auto H = pstest::spectrumOf (h, N);
        const double srTop = sr * (1 << f);

        double ripple = 0.0, stop = -400.0;
        for (size_t k = 0; k <= N / 2; ++k)
        {
            const double hz = (double) k * srTop / (double) N;
            const double db = pstest::dbOf (std::abs (H[k]));
            if (hz <= 0.45 * sr)
                ripple = std::max (ripple, std::abs (db));
            if (hz >= 0.55 * sr)
                stop = std::max (stop, db);
        }
        pstest::note (std::to_string (1 << f) + "x interpolation: passband deviation (dB)", ripple);
        pstest::note (std::to_string (1 << f) + "x interpolation: worst stopband (dB)", stop);
        CHECK (ripple < 0.01);
        CHECK (stop <= -100.0);

        // decimation: stopband tones alias at >= 100 dB down, passband tones pass at 0 dB
        double worstAlias = -400.0;
        for (double rel : { 0.56, 0.7, 0.95, 1.3, 1.8, 2.6, 3.5 })
            if (rel * sr < 0.5 * sr * (1 << f))
                worstAlias = std::max (worstAlias, decimatedLevelDb (f, Quality::LinearPhase, sr, rel * sr));
        const double pass = decimatedLevelDb (f, Quality::LinearPhase, sr, 0.43 * sr);
        pstest::note (std::to_string (1 << f) + "x decimation: worst alias (dB)", worstAlias);
        pstest::note (std::to_string (1 << f) + "x decimation: 0.43 fs gain (dB)", pass);
        CHECK (worstAlias <= -100.0);
        CHECK (std::abs (pass) < 0.01);
    }
}

TEST_CASE ("os. round trip is the input delayed by exactly getLatencySamples()")
{
    const double sr = 44100.0;
    for (int f = 1; f <= 3; ++f)
    {
        Oversampler os;
        os.prepare (f, Quality::LinearPhase, 512, 2);
        const int L = os.getLatencySamples();
        pstest::note (std::to_string (1 << f) + "x linear phase latency (samples)", L);

        // impulse: peak exactly at L, symmetric around it
        std::vector<float> imp (4096, 0.0f);
        imp[100] = 1.0f;
        const auto y = roundTrip (os, imp, 512);
        size_t peak = 0;
        for (size_t i = 0; i < y.size(); ++i)
            if (std::abs (y[i]) > std::abs (y[peak]))
                peak = i;
        CHECK_EQ ((int) peak, 100 + L);
        double asym = 0.0;
        for (int k = 1; k < 90; ++k)
            asym = std::max (asym, (double) std::abs (y[peak + (size_t) k] - y[peak - (size_t) k]));
        CHECK (asym < 1e-6);

        for (double hz : { 1000.0, 10000.0, 19800.0 })
        {
            os.reset();
            const auto x = pstest::sine (hz, -1.0, 0.5, sr);
            const auto z = roundTrip (os, x, 512);
            double e2 = 0.0, s2 = 0.0;
            for (size_t i = 2000; i < x.size(); ++i)
            {
                const double e = (double) z[i] - (double) x[i - (size_t) L];
                e2 += e * e;
                s2 += (double) x[i - (size_t) L] * x[i - (size_t) L];
            }
            const double rel = 10.0 * std::log10 (e2 / s2);
            pstest::note (std::to_string (1 << f) + "x round trip error at " + std::to_string ((int) hz) + " Hz (dB)", rel);
            CHECK (rel < -95.0);
        }
    }
}

TEST_CASE ("os. minimum phase: low latency, 100 dB images, flat passband")
{
    const double sr = 44100.0;
    for (int f = 1; f <= 3; ++f)
    {
        Oversampler os;
        os.prepare (f, Quality::MinimumPhase, 512, 2);
        const int L = os.getLatencySamples();
        pstest::note (std::to_string (1 << f) + "x minimum phase latency (samples)", L);
        CHECK (L <= 6);

        const auto h = upsampledImpulse (f, Quality::MinimumPhase, 8192);
        const auto H = pstest::spectrumOf (h, h.size());
        const double srTop = sr * (1 << f);
        double ripple = 0.0, stop = -400.0;
        for (size_t k = 0; k <= h.size() / 2; ++k)
        {
            const double hz = (double) k * srTop / (double) h.size();
            const double db = pstest::dbOf (std::abs (H[k]));
            if (hz <= 0.45 * sr) ripple = std::max (ripple, std::abs (db));
            if (hz >= 0.55 * sr) stop = std::max (stop, db);
        }
        pstest::note (std::to_string (1 << f) + "x minimum phase: passband deviation (dB)", ripple);
        pstest::note (std::to_string (1 << f) + "x minimum phase: worst stopband (dB)", stop);
        CHECK (ripple < 0.01);
        CHECK (stop <= -100.0);

        // round trip of a 1 kHz sine keeps its level
        const auto x = pstest::sine (1000.0, -1.0, 0.5, sr);
        const auto z = roundTrip (os, x, 512);
        const double g = std::abs (pstest::toneOf (z, 4410, 17640, 1000.0, sr)) / std::abs (pstest::toneOf (x, 4410, 17640, 1000.0, sr));
        CHECK_NEAR (20.0 * std::log10 (g), 0.0, 0.001);
    }
}

TEST_CASE ("os. minimum phase latency = measured low-frequency group delay of the round trip, rounded")
{
    // Regression: the decimator's one-sample advance (per stage, at that stage's high rate) was not counted, so 4x
    // measured ~4.9 samples but reported 6. Convention: latency = round (group delay at DC, base-rate samples).
    for (int f = 1; f <= 3; ++f)
    {
        Oversampler os;
        os.prepare (f, Quality::MinimumPhase, 512, 2);
        const int L = os.getLatencySamples();

        // round-trip impulse response -> phase slope at low frequencies (independent double FFT)
        std::vector<float> imp (16384, 0.0f);
        imp[0] = 1.0f;
        const auto h = roundTrip (os, imp, 512);
        const auto H = pstest::spectrumOf (h, h.size());
        const double binW = 2.0 * pstest::kPi / (double) h.size();
        double worstAway = 0.0, gdDc = 0.0;
        for (int k : { 1, 4, 16, 64 })   // 2.7 Hz .. 172 Hz at 44.1 kHz
        {
            const double dphi = std::arg (H[(size_t) k + 1] / H[(size_t) k]);
            const double gd = -dphi / binW;
            if (k == 1)
                gdDc = gd;
            worstAway = std::max (worstAway, std::abs (gd - L));
        }
        // alignment check: a 50 Hz sine through the round trip vs the same sine delayed by L
        const double sr = 44100.0;
        const auto x = pstest::sine (50.0, -6.0, 1.0, sr);
        os.reset();
        const auto z = roundTrip (os, x, 512);
        double err = 0.0;
        for (size_t i = 22050; i < x.size(); ++i)
            err = std::max (err, std::abs ((double) z[i] - (double) x[i - (size_t) L]));

        pstest::note (std::to_string (1 << f) + "x minimum phase: reported latency", L);
        pstest::note (std::to_string (1 << f) + "x minimum phase: measured group delay at DC (samples)", gdDc);
        pstest::note (std::to_string (1 << f) + "x minimum phase: 50 Hz sine vs input delayed by the latency (dBFS)",
                      pstest::dbOf (err));
        CHECK (std::abs (gdDc - L) <= 0.5);
        CHECK (worstAway <= 0.5);
        CHECK (std::lround (gdDc) == L);
    }
}

TEST_CASE ("os. upsample and downsample do not allocate")
{
    for (auto q : { Quality::LinearPhase, Quality::MinimumPhase })
        for (int f = 1; f <= 3; ++f)
        {
            Oversampler os;
            os.prepare (f, q, 512, 2);
            const auto x = pstest::whiteNoise (0.5, 0.2, 48000.0, 9u);
            std::vector<float> outL (512), outR (512);
            pstest::AllocationScope scope;
            const long long start = scope.count();
            for (size_t pos = 0; pos + 512 <= x.size(); pos += 512)
            {
                const float* in[2] { x.data() + pos, x.data() + pos };
                os.upsample (in, 512);
                float* out[2] { outL.data(), outR.data() };
                os.downsample (out, 512);
            }
            os.reset();
            CHECK_EQ (scope.count() - start, 0LL);
            pstest::note (std::string (qName (q)) + " " + std::to_string (1 << f) + "x: allocations", (double) (scope.count() - start));
        }
}
