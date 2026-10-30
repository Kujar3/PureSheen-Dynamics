#include "TestFramework.h"
#include "TestSignals.h"

#include "core/Fft.h"
#include "dsp/Dither.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

// Output dither tests (dsp/Dither.cpp). Prefix "dither.".
namespace
{
using namespace ps;
using pstest::note;

DitherParams makeParams (DitherBits bits, DitherAmount amount, float shaping, bool limit = false, bool blank = false)
{
    DitherParams p;
    p.on = true;
    p.bits = bits;
    p.amount = amount;
    p.noiseShaping = shaping;
    p.limitPeaks = limit;
    p.autoBlank = blank;
    return p;
}

double lsb (DitherBits bits) { return std::ldexp (1.0, -(kDitherBitDepth[(size_t) bits] - 1)); }

// Runs the dither over a stereo copy of `in` (both channels the same signal), block size 512.
std::vector<float> runDither (Dither& d, const std::vector<float>& in, std::vector<float>* right = nullptr)
{
    std::vector<float> l (in), r (in);
    for (size_t pos = 0; pos < in.size(); pos += 512)
    {
        const int n = (int) std::min<size_t> (512, in.size() - pos);
        float* ch[2] { l.data() + pos, r.data() + pos };
        d.process (ch, n);
    }
    if (right != nullptr)
        *right = r;
    return l;
}

std::vector<float> errorSignal (const std::vector<float>& out, const std::vector<float>& in)
{
    std::vector<float> e (out.size());
    for (size_t i = 0; i < out.size(); ++i)
        e[i] = (float) ((double) out[i] - (double) in[i]);
    return e;
}

double rms (const std::vector<float>& x, size_t from = 0)
{
    double acc = 0.0;
    for (size_t i = from; i < x.size(); ++i)
        acc += (double) x[i] * x[i];
    return std::sqrt (acc / (double) (x.size() - from));
}

// Welch power spectrum (Hann, 2^order points, 50 % overlap), power per bin, of x / scale.
std::vector<double> powerSpectrum (const std::vector<float>& x, double scale, int order = 12)
{
    const int n = 1 << order;
    Fft fft (order);
    std::vector<double> psd (n / 2 + 1, 0.0), window (n);
    for (int i = 0; i < n; ++i)
        window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * pstest::kPi * i / n);
    std::vector<std::complex<float>> buf ((size_t) n);
    int frames = 0;
    for (size_t start = 0; start + n <= x.size(); start += n / 2, ++frames)
    {
        for (int i = 0; i < n; ++i)
            buf[(size_t) i] = { (float) (x[start + (size_t) i] / scale * window[(size_t) i]), 0.0f };
        fft.forward (buf.data());
        for (int k = 0; k <= n / 2; ++k)
            psd[(size_t) k] += std::norm (buf[(size_t) k]);
    }
    for (auto& p : psd)
        p /= std::max (1, frames);
    return psd;
}

double bandPowerDb (const std::vector<double>& psd, double sr, double lo, double hi)
{
    const double binHz = sr / (2.0 * (double) (psd.size() - 1));
    double acc = 0.0;
    int count = 0;
    for (size_t k = 0; k < psd.size(); ++k)
    {
        const double f = (double) k * binHz;
        if (f >= lo && f <= hi)
        {
            acc += psd[k];
            ++count;
        }
    }
    return 10.0 * std::log10 (acc / std::max (1, count));
}

// Harmonic power (bins around k * f0, k = 2..) below maxHz of a single-sided spectrum, relative units.
double harmonicPower (const std::vector<double>& psd, double sr, double f0, double maxHz)
{
    const double binHz = sr / (2.0 * (double) (psd.size() - 1));
    double acc = 0.0;
    for (int k = 2; k * f0 < maxHz; ++k)
    {
        const int centre = (int) std::lround (k * f0 / binHz);
        for (int b = centre - 3; b <= centre + 3; ++b)
            if (b > 0 && b < (int) psd.size())
                acc += psd[(size_t) b];
    }
    return acc;
}
} // namespace

TEST_CASE ("dither. output is quantised exactly to the bit depth")
{
    const auto in = pstest::whiteNoise (0.5, 0.5, 44100.0, 7u);
    int checkedConfigs = 0;
    for (int b = 0; b < 5; ++b)
        for (int a = 0; a < 4; ++a)
            for (float shaping : { 0.0f, 50.0f, 100.0f })
                for (bool harm : { false, true })
                {
                    Dither d;
                    d.prepare (44100.0, 512);
                    auto p = makeParams ((DitherBits) b, (DitherAmount) a, shaping, true);
                    p.harmonicSuppression = harm;
                    d.setParams (p);
                    const auto out = runDither (d, in);
                    const double scale = 1.0 / lsb ((DitherBits) b);
                    bool exact = true;
                    for (float y : out)
                    {
                        const double v = (double) y * scale;
                        exact = exact && v == std::floor (v);
                    }
                    CHECK (exact);
                    ++checkedConfigs;
                }
    note ("configurations checked", checkedConfigs);

    // Already-quantised input with plain rounding, no shaping: unchanged.
    Dither d;
    d.prepare (48000.0, 512);
    d.setParams (makeParams (DitherBits::Bits16, DitherAmount::Off, 0.0f));
    std::vector<float> grid (4800);
    for (size_t i = 0; i < grid.size(); ++i)
        grid[i] = (float) (std::lround (20000.0 * std::sin ((double) i * 0.01)) * lsb (DitherBits::Bits16));
    CHECK (runDither (d, grid) == grid);
}

TEST_CASE ("dither. noise level per amount")
{
    const double sr = 44100.0;
    const auto in = pstest::sine (997.0, -40.0, 4.0, sr);
    const double q = lsb (DitherBits::Bits16);
    // TPDF of +-scale LSB (variance scale^2 / 6) plus uniform rounding error (1 / 12), in LSB rms.
    const double expected[4] { std::sqrt (1.0 / 6 + 1.0 / 12), std::sqrt (0.49 / 6 + 1.0 / 12),
                               std::sqrt (0.25 / 6 + 1.0 / 12), std::sqrt (1.0 / 12) };
    const char* names[4] { "Strong noise rms (LSB)", "Medium noise rms (LSB)", "Low noise rms (LSB)", "Off noise rms (LSB)" };
    for (int a = 0; a < 4; ++a)
    {
        Dither d;
        d.prepare (sr, 512);
        d.setParams (makeParams (DitherBits::Bits16, (DitherAmount) a, 0.0f));
        const double measured = rms (errorSignal (runDither (d, in), in)) / q;
        note (names[a], measured);
        CHECK_NEAR (measured, expected[a], 0.02 * expected[a] + 0.003);
    }
}

TEST_CASE ("dither. noise shaping moves noise out of 1..6 kHz and above 15 kHz")
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const auto in = pstest::sine (997.0, -30.0, 6.0, sr);
        const double q = lsb (DitherBits::Bits16);

        auto measure = [&] (float shaping, bool limit)
        {
            Dither d;
            d.prepare (sr, 512);
            d.setParams (makeParams (DitherBits::Bits16, DitherAmount::Strong, shaping, limit));
            return powerSpectrum (errorSignal (runDither (d, in), in), q);
        };

        const auto flat = measure (0.0f, false);
        const auto shaped = measure (100.0f, false);
        const auto shapedLimited = measure (100.0f, true);
        const auto half = measure (50.0f, false);

        const double audible = bandPowerDb (shaped, sr, 1000.0, 6000.0) - bandPowerDb (flat, sr, 1000.0, 6000.0);
        const double audibleLimited = bandPowerDb (shapedLimited, sr, 1000.0, 6000.0) - bandPowerDb (flat, sr, 1000.0, 6000.0);
        const double audibleHalf = bandPowerDb (half, sr, 1000.0, 6000.0) - bandPowerDb (flat, sr, 1000.0, 6000.0);
        const double top = std::min (20000.0, sr * 0.5 * 0.98);
        const double high = bandPowerDb (shaped, sr, 15000.0, top) - bandPowerDb (flat, sr, 15000.0, top);

        note ("sample rate", sr);
        note ("  1..6 kHz change at 100 (dB)", audible);
        note ("  1..6 kHz change at 100, Limit Peaks on (dB)", audibleLimited);
        note ("  1..6 kHz change at 50 (dB)", audibleHalf);
        note ("  15..20 kHz change at 100 (dB)", high);

        CHECK (audible < -12.0);
        CHECK (audibleLimited < -9.0);
        CHECK (audibleHalf < -3.0 && audibleHalf > audible);
        CHECK (high > 3.0);
    }

    // Flat above 96 kHz.
    Dither d;
    d.prepare (192000.0, 512);
    d.setParams (makeParams (DitherBits::Bits16, DitherAmount::Strong, 100.0f));
    const auto in = pstest::sine (997.0, -30.0, 1.0, 192000.0);
    const auto e = errorSignal (runDither (d, in), in);
    CHECK_NEAR (rms (e) / lsb (DitherBits::Bits16), 0.5, 0.02);
}

TEST_CASE ("dither. limit peaks keeps the shaped noise within 4 LSB")
{
    const double sr = 44100.0;
    const auto in = pstest::whiteNoise (0.3, 3.0, sr, 99u);
    const double q = lsb (DitherBits::Bits16);
    for (bool limit : { true, false })
    {
        Dither d;
        d.prepare (sr, 512);
        d.setParams (makeParams (DitherBits::Bits16, DitherAmount::Strong, 100.0f, limit));
        const auto e = errorSignal (runDither (d, in), in);
        double peak = 0.0;
        for (float v : e)
            peak = std::max (peak, std::abs ((double) v) / q);
        note (limit ? "peak noise, Limit Peaks on (LSB)" : "peak noise, Limit Peaks off (LSB)", peak);
        if (limit)
            CHECK (peak <= 4.0 + 1.0e-6);
        else
            CHECK (peak > 4.0);
    }
}

TEST_CASE ("dither. auto-blank mutes after 0.7 s of digital silence")
{
    const double sr = 48000.0;
    const auto music = pstest::sine (440.0, -20.0, 0.3, sr);
    std::vector<float> in = music;
    in.resize (music.size() + (size_t) sr, 0.0f);   // 1 s of exact zeros
    in = pstest::concat (in, music);

    for (bool blank : { true, false })
    {
        Dither d;
        d.prepare (sr, 512);
        d.setParams (makeParams (DitherBits::Bits16, DitherAmount::Strong, 50.0f, true, blank));
        const auto out = runDither (d, in);
        const size_t zeroStart = music.size();
        const size_t blankAt = zeroStart + (size_t) std::ceil (0.7 * sr);

        bool noiseBefore = false, silentAfter = true;
        for (size_t i = zeroStart; i < blankAt - 1; ++i)
            noiseBefore = noiseBefore || out[i] != 0.0f;
        for (size_t i = blankAt; i < zeroStart + (size_t) sr; ++i)
            silentAfter = silentAfter && out[i] == 0.0f;
        bool resumed = false;
        for (size_t i = zeroStart + (size_t) sr; i < out.size(); ++i)
            resumed = resumed || out[i] != 0.0f;

        CHECK (noiseBefore);
        CHECK (resumed);
        if (blank)
            CHECK (silentAfter);
        else
            CHECK (! silentAfter);
    }
}

TEST_CASE ("dither. deterministic")
{
    const auto in = pstest::pinkNoise (0.4, 1.0, 44100.0, 3u);
    Dither a, b;
    a.prepare (44100.0, 512);
    b.prepare (44100.0, 512);
    const auto p = makeParams (DitherBits::Bits16, DitherAmount::Medium, 70.0f, true, true);
    a.setParams (p);
    b.setParams (p);
    std::vector<float> ra, rb;
    const auto la = runDither (a, in, &ra);
    const auto lb = runDither (b, in, &rb);
    CHECK (la == lb);
    CHECK (ra == rb);
    CHECK (la != ra);   // channels use different seeds

    a.reset();
    CHECK (runDither (a, in) == la);
}

TEST_CASE ("dither. harmonic suppression moves low-level harmonics out of the audible band")
{
    const double sr = 44100.0;
    const double q = lsb (DitherBits::Bits16);
    constexpr int fftOrder = 16;   // fine resolution: harmonic bins hold the harmonics, not the floor
    const double f0 = std::round (1000.0 * 65536.0 / sr) * sr / 65536.0;   // on a bin
    for (double amplitudeLsb : { 0.7, 2.5, 10.0 })
    {
        std::vector<float> in ((size_t) (4.0 * sr));
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = (float) (amplitudeLsb * q * std::sin (2.0 * pstest::kPi * f0 * (double) i / sr));

        auto measure = [&] (bool harm, double& floorDb)
        {
            Dither d;
            d.prepare (sr, 512);
            auto p = makeParams (DitherBits::Bits16, DitherAmount::Off, 0.0f);
            p.harmonicSuppression = harm;
            d.setParams (p);
            const auto psd = powerSpectrum (errorSignal (runDither (d, in), in), q, fftOrder);
            floorDb = bandPowerDb (psd, sr, 20.0, 16000.0);
            return 10.0 * std::log10 (harmonicPower (psd, sr, f0, 16000.0) + 1.0e-30);
        };

        double floorPlain = 0.0, floorHs = 0.0;
        const double plain = measure (false, floorPlain);
        const double hs = measure (true, floorHs);
        note ("sine amplitude (LSB)", amplitudeLsb);
        note ("  harmonics < 16 kHz, suppression on vs plain rounding (dB)", hs - plain);
        note ("  20 Hz..16 kHz error floor change (dB)", floorHs - floorPlain);
        CHECK (hs - plain < -15.0);
        CHECK (floorHs - floorPlain < 1.0);
    }
}

TEST_CASE ("dither. bit meter, DC meter and off is passthrough")
{
    const double sr = 48000.0;
    const auto in = pstest::sine (440.0, -10.0, 0.1, sr);

    Dither d;
    d.prepare (sr, 512);
    d.setParams (makeParams (DitherBits::Bits16, DitherAmount::Strong, 50.0f, true));
    runDither (d, in);
    const auto active = d.getMeters().activeBits[0].load();
    CHECK ((active & 0xFFFFu) == 0xFFFFu);   // bits 0 (MSB) .. 15 all toggle on a -10 dBFS dithered sine
    CHECK ((active & 0xFF0000u) == 0u);      // bits 16..23 unused at 16 bits
    CHECK ((d.getMeters().peakBits[1].load() & 0xFF0000u) == 0u);
    d.resetPeakBits();
    CHECK (d.getMeters().peakBits[0].load() == 0u);

    DitherParams off;
    off.on = false;
    d.setParams (off);
    std::vector<float> dc (in.size() * 50);
    for (size_t i = 0; i < dc.size(); ++i)
        dc[i] = 0.25f + 0.1f * (float) std::sin ((double) i * 0.05);
    const auto out = runDither (d, dc);
    CHECK (out == dc);
    note ("DC meter after 5 s of 0.25 DC", d.getMeters().dcOffset[0].load());
    CHECK_NEAR (d.getMeters().dcOffset[0].load(), 0.25, 0.01);
    CHECK ((d.getMeters().activeBits[0].load() & 0xFFFFFFu) != 0u);
}
