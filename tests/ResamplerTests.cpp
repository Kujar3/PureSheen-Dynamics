#include "TestFramework.h"
#include "TestSignals.h"

#include "core/Resampler.h"

#include <cmath>
#include <vector>

// Tests for Resampler.h (Track Referencing: reference files are converted to the host rate when they load).

namespace
{
constexpr double kTwoPi = 6.283185307179586;

std::vector<float> sine (double hz, double rate, long long n, double amplitude = 0.5)
{
    std::vector<float> x ((size_t) n);
    for (long long i = 0; i < n; ++i)
        x[(size_t) i] = (float) (amplitude * std::sin (kTwoPi * hz * (double) i / rate));
    return x;
}

std::vector<float> resample (const std::vector<float>& in, double inRate, double outRate)
{
    const ps::SincResampler r (inRate, outRate);
    std::vector<float> out ((size_t) r.outputLength ((long long) in.size()));
    const float* src[] = { in.data() };
    float* dst[] = { out.data() };
    r.process (src, (long long) in.size(), dst, 1, 0, (long long) out.size());
    return out;
}

// Worst error (dB re the amplitude) against the ideal sine over the middle of the output (edges are zero padded).
double errorDb (const std::vector<float>& out, double hz, double rate, double amplitude)
{
    double worst = 0.0;
    const size_t margin = (size_t) (0.05 * rate);
    for (size_t i = margin; i + margin < out.size(); ++i)
        worst = std::max (worst, std::abs ((double) out[i] - amplitude * std::sin (kTwoPi * hz * (double) i / rate)));
    return 20.0 * std::log10 (std::max (worst, 1.0e-12) / amplitude);
}

double rmsDb (const std::vector<float>& x, double rate)
{
    double sum = 0.0;
    size_t count = 0;
    const size_t margin = (size_t) (0.05 * rate);
    for (size_t i = margin; i + margin < x.size(); ++i, ++count)
        sum += (double) x[i] * (double) x[i];
    return 10.0 * std::log10 (std::max (sum / (double) std::max<size_t> (count, 1), 1.0e-30));
}
} // namespace

TEST_CASE ("ref. resampler: 44.1 -> 48 kHz uses exact phases, keeps length and a 1 kHz sine")
{
    const ps::SincResampler r (44100.0, 48000.0);
    CHECK (r.isExact());
    CHECK_EQ (r.getNumPhases(), 160);
    CHECK_EQ (r.outputLength (44100), 48000LL);

    const auto out = resample (sine (1000.0, 44100.0, 44100), 44100.0, 48000.0);
    REQUIRE (out.size() == 48000);
    const double err = errorDb (out, 1000.0, 48000.0, 0.5);
    pstest::note ("error dB", err);
    CHECK (err < -80.0);
}

TEST_CASE ("ref. resampler: 48 -> 44.1 kHz and an irrational ratio stay accurate")
{
    const auto down = resample (sine (5000.0, 48000.0, 48000), 48000.0, 44100.0);
    const double errDown = errorDb (down, 5000.0, 44100.0, 0.5);
    pstest::note ("48 -> 44.1 error dB", errDown);
    CHECK (errDown < -80.0);

    const double odd = 47999.5;   // no small rational ratio: quantised phases
    const ps::SincResampler r (44100.0, odd);
    CHECK (! r.isExact());
    const auto out = resample (sine (1000.0, 44100.0, 44100), 44100.0, odd);
    const double err = errorDb (out, 1000.0, odd, 0.5);
    pstest::note ("irrational error dB", err);
    CHECK (err < -70.0);
}

TEST_CASE ("ref. resampler: DC passes at unity, content above the new Nyquist is removed")
{
    std::vector<float> dc (48000, 0.25f);
    const auto dcOut = resample (dc, 96000.0, 48000.0);
    CHECK (std::abs (dcOut[dcOut.size() / 2] - 0.25f) < 1.0e-4f);

    // 30 kHz in a 96 kHz file: above 24 kHz, must not alias into a 48 kHz session.
    const auto out = resample (sine (30000.0, 96000.0, 96000), 96000.0, 48000.0);
    const double level = rmsDb (out, 48000.0) - 20.0 * std::log10 (0.5 / std::sqrt (2.0));
    pstest::note ("alias level dB", level);
    CHECK (level < -80.0);

    // A 15 kHz tone (inside both bands) keeps its level.
    const auto keep = resample (sine (15000.0, 96000.0, 96000), 96000.0, 48000.0);
    const double keepLevel = rmsDb (keep, 48000.0) - 20.0 * std::log10 (0.5 / std::sqrt (2.0));
    CHECK (std::abs (keepLevel) < 0.05);
}
