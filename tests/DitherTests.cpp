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
    // Blank on and off run identically seeded: up to the blank point the outputs must be bit-identical (blanking
    // changes nothing before 0.7 s of silence), from the 0.7 s point to the end of the silence the blanked output is
    // exact digital silence while the other one keeps dithering, and both resume when the input does.
    const double sr = 48000.0;
    const auto music = pstest::sine (440.0, -20.0, 0.3, sr);
    std::vector<float> in = music;
    in.resize (music.size() + (size_t) sr, 0.0f);   // 1 s of exact zeros
    in = pstest::concat (in, music);

    std::vector<float> out[2];
    for (int k = 0; k < 2; ++k)
    {
        Dither d;
        d.prepare (sr, 512);
        d.setParams (makeParams (DitherBits::Bits16, DitherAmount::Strong, 50.0f, true, k == 0));
        out[k] = runDither (d, in);
    }
    const auto& on = out[0];
    const auto& off = out[1];

    const size_t zeroStart = music.size();
    const size_t zeroEnd = zeroStart + (size_t) sr;
    const size_t blankSamples = (size_t) std::ceil (0.7 * sr);       // 33600
    const size_t firstBlanked = zeroStart + blankSamples - 1;          // the 33600th silent sample

    size_t firstDifference = on.size();
    for (size_t i = 0; i < on.size() && firstDifference == on.size(); ++i)
        if (on[i] != off[i])
            firstDifference = i;

    bool silentAfter = true, offDithers = false;
    for (size_t i = firstBlanked; i < zeroEnd; ++i)
    {
        silentAfter = silentAfter && on[i] == 0.0f;
        offDithers = offDithers || off[i] != 0.0f;
    }
    bool noiseJustBefore = false;
    for (size_t i = firstBlanked - 480; i < firstBlanked; ++i)
        noiseJustBefore = noiseJustBefore || on[i] != 0.0f;
    bool resumedOn = false, resumedOff = false;
    for (size_t i = zeroEnd; i < on.size(); ++i)
    {
        resumedOn = resumedOn || on[i] != 0.0f;
        resumedOff = resumedOff || off[i] != 0.0f;
    }

    pstest::note ("blank point (samples after the silence starts, expect 33599)", (double) (firstBlanked - zeroStart));
    pstest::note ("first sample where blank on / off differ, after the silence starts", (double) (firstDifference - zeroStart));
    CHECK (firstDifference >= firstBlanked);       // bit-identical before the blank point
    CHECK (firstDifference < firstBlanked + 64);   // and they part right there (off keeps dithering)
    CHECK (noiseJustBefore);                    // still dithering in the last 10 ms before the blank point
    CHECK (silentAfter);
    CHECK (offDithers);
    CHECK (resumedOn);
    CHECK (resumedOff);
    // the first resumed samples are the music again, quantised (within 1 LSB + shaped noise)
    CHECK (std::abs ((double) on[zeroEnd + 100] - (double) in[zeroEnd + 100]) < 8.0 * lsb (DitherBits::Bits16));
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

TEST_CASE ("dither. harmonic suppression removes low-level harmonics, low-frequency floor near plain rounding")
{
    const double sr = 44100.0;
    const double q = lsb (DitherBits::Bits16);
    constexpr int fftOrder = 16;   // fine resolution: harmonic bins hold the harmonics, not the floor
    const double f0 = std::round (1000.0 * 65536.0 / sr) * sr / 65536.0;   // on a bin
    const double binHz = sr / 65536.0;

    // power in the harmonic bins (k = 2.. below 16 kHz) relative to the floor around them (median of +-40 bins)
    auto harmonicExcessDb = [&] (const std::vector<double>& psd)
    {
        double harm = 0.0, floor = 0.0;
        std::vector<double> nb;
        for (int k = 2; k * f0 < 16000.0; ++k)
        {
            const int centre = (int) std::lround (k * f0 / binHz);
            nb.clear();
            for (int b = centre - 40; b <= centre + 40; ++b)
                if (std::abs (b - centre) > 4)
                    nb.push_back (psd[(size_t) b]);
            std::nth_element (nb.begin(), nb.begin() + (long long) nb.size() / 2, nb.end());
            for (int b = centre - 3; b <= centre + 3; ++b)
            {
                harm += psd[(size_t) b];
                floor += nb[nb.size() / 2];
            }
        }
        return 10.0 * std::log10 ((harm + 1.0e-30) / (floor + 1.0e-30));
    };

    auto errorPsd = [&] (const std::vector<float>& in, DitherAmount amount, bool harm)
    {
        Dither d;
        d.prepare (sr, 512);
        auto p = makeParams (DitherBits::Bits16, amount, 0.0f);
        p.harmonicSuppression = harm;
        d.setParams (p);
        return powerSpectrum (errorSignal (runDither (d, in), in), q, fftOrder);
    };

    // reference floor: plain rounding of loud noise (the error is then white, q^2 / 12)
    const auto loud = pstest::whiteNoise (0.5, 4.0, sr, 41u);
    const double whiteFloor = bandPowerDb (errorPsd (loud, DitherAmount::Off, false), sr, 20.0, 6000.0);

    for (double amplitudeLsb : { 0.7, 2.5, 10.0 })
    {
        std::vector<float> in ((size_t) (12.0 * sr));
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = (float) (amplitudeLsb * q * std::sin (2.0 * pstest::kPi * f0 * (double) i / sr));

        const auto plain = errorPsd (in, DitherAmount::Off, false);
        const auto hs = errorPsd (in, DitherAmount::Off, true);
        const auto tpdf = errorPsd (in, DitherAmount::Strong, false);
        const double excessPlain = harmonicExcessDb (plain), excessHs = harmonicExcessDb (hs);
        const double floorHs = bandPowerDb (hs, sr, 20.0, 6000.0);
        note ("sine amplitude (LSB)", amplitudeLsb);
        note ("  harmonics < 16 kHz above the local floor, plain rounding (dB)", excessPlain);
        note ("  harmonics < 16 kHz above the local floor, suppression on (dB)", excessHs);
        note ("  20 Hz..6 kHz floor vs white q^2/12 (dB)", floorHs - whiteFloor);
        note ("  20 Hz..6 kHz floor vs Strong TPDF (dB)", floorHs - bandPowerDb (tpdf, sr, 20.0, 6000.0));
        CHECK (std::abs (excessHs) < 1.0);                  // no harmonics left (estimator spread ~ +-0.5 dB)
        if (amplitudeLsb < 5.0)
            CHECK (excessPlain > 10.0);                     // (plain rounding is full of them)
        CHECK (floorHs - whiteFloor < 1.5);                 // close to undithered rounding below 6 kHz
        CHECK (floorHs - bandPowerDb (tpdf, sr, 20.0, 6000.0) < -3.0);   // well below flat TPDF
    }
}

TEST_CASE ("dither. harmonic suppression adds no spectral lines (DC, sub-LSB DC and low-level sines)")
{
    // Regression: the deterministic pattern turned 0.25 LSB of DC at 8 bits into a ~723 Hz tone (-52 dBFS).
    // The total error (output - input) must be line-free: every bin of an averaged spectrum (Welch, 4096 points,
    // ~8 s) within 3 dB of the median of its +-24 neighbours. Plain rounding is checked to fail this (it does create
    // lines), so the detector is known to work.
    const double sr = 44100.0;
    const double q = lsb (DitherBits::Bits8);
    const double fBin = 44100.0 / 4096.0;
    struct Case { const char* name; double dcLsb, sineLsb, sineHz; };
    const Case cases[] { { "DC 0.25 LSB", 0.25, 0.0, 0.0 }, { "DC 0.37 LSB", 0.37, 0.0, 0.0 },
                         { "DC 3.5 LSB", 3.5, 0.0, 0.0 }, { "sine 0.4 LSB", 0.0, 0.4, 93.0 * fBin },
                         { "sine 3 LSB + 0.2 LSB DC", 0.2, 3.0, 93.0 * fBin }, { "sine 20 LSB", 0.0, 20.0, 371.0 * fBin } };

    auto worstLineDb = [&] (const std::vector<double>& psd)
    {
        double worst = -100.0;
        std::vector<double> nb;
        for (int k = 4; k < (int) psd.size() - 2; ++k)
        {
            nb.clear();
            for (int j = std::max (1, k - 24); j <= std::min ((int) psd.size() - 1, k + 24); ++j)
                if (std::abs (j - k) > 2)
                    nb.push_back (psd[(size_t) j]);
            std::nth_element (nb.begin(), nb.begin() + (long long) nb.size() / 2, nb.end());
            const double med = nb[nb.size() / 2];
            if (med > 0.0)
                worst = std::max (worst, 10.0 * std::log10 (psd[(size_t) k] / med));
        }
        return worst;
    };

    for (const auto& cs : cases)
    {
        std::vector<float> in ((size_t) (8.0 * sr));
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = (float) (q * (cs.dcLsb + cs.sineLsb * std::sin (2.0 * pstest::kPi * cs.sineHz * (double) i / sr)));

        double lines[2] {};
        for (int hs = 0; hs < 2; ++hs)
        {
            Dither d;
            d.prepare (sr, 512);
            auto p = makeParams (DitherBits::Bits8, DitherAmount::Off, 0.0f);
            p.harmonicSuppression = hs == 1;
            d.setParams (p);
            const auto psd = powerSpectrum (errorSignal (runDither (d, in), in), q, 12);
            lines[hs] = worstLineDb (psd);
        }
        note (std::string (cs.name) + ": strongest line above the local floor, suppression on (dB)", lines[1]);
        note (std::string (cs.name) + ":   plain rounding (dB)", lines[0]);
        CHECK (lines[1] < 3.0);
        if (cs.sineLsb > 0.0 || cs.dcLsb != std::floor (cs.dcLsb))
            CHECK (lines[0] > 10.0 || cs.sineLsb == 0.0);   // rounding of a tone is full of harmonics
    }

    // and with noise shaping on top (error feedback of the shaped total error): still no lines
    {
        std::vector<float> in ((size_t) (8.0 * sr), (float) (0.25 * q));
        Dither d;
        d.prepare (sr, 512);
        auto p = makeParams (DitherBits::Bits8, DitherAmount::Off, 100.0f, true);
        p.harmonicSuppression = true;
        d.setParams (p);
        const double worst = worstLineDb (powerSpectrum (errorSignal (runDither (d, in), in), q, 12));
        note ("DC 0.25 LSB, shaping 100, Limit Peaks: strongest line (dB)", worst);
        CHECK (worst < 3.0);
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
