#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/Crossover.h"
#include "dsp/Filters.h"

#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

// Crossover (dsp/Crossover.cpp). Prefix "xover.".
namespace
{
using namespace ps;
using pstest::cd;
using pstest::kPi;

using Bands = std::array<std::array<std::vector<float>, 2>, kMaxBands>;

// Runs the crossover over L / R in blocks; onBlock (startSample) is called before each block (for setup changes).
Bands run (Crossover& x, const std::vector<float>& L, const std::vector<float>& R, int block,
           const std::function<void (size_t)>& onBlock = {})
{
    Bands out;
    for (auto& b : out)
        for (auto& c : b)
            c.assign (L.size(), 0.0f);

    std::vector<std::vector<float>> buf ((size_t) (kMaxBands * 2), std::vector<float> ((size_t) block));
    float* chans[kMaxBands][2];
    float* const* bandPtrs[kMaxBands];
    for (int b = 0; b < kMaxBands; ++b)
    {
        chans[b][0] = buf[(size_t) (2 * b)].data();
        chans[b][1] = buf[(size_t) (2 * b + 1)].data();
        bandPtrs[b] = chans[b];
    }

    for (size_t start = 0; start < L.size(); start += (size_t) block)
    {
        if (onBlock)
            onBlock (start);
        const int n = (int) std::min ((size_t) block, L.size() - start);
        const float* in[2] { L.data() + start, R.data() + start };
        x.process (in, bandPtrs, n);
        for (int b = 0; b < x.getNumBands(); ++b)
            for (int ch = 0; ch < 2; ++ch)
                std::copy (chans[b][ch], chans[b][ch] + n, out[(size_t) b][(size_t) ch].begin() + (std::ptrdiff_t) start);
    }
    return out;
}

std::vector<float> sumOf (const Bands& b, int numBands, int ch)
{
    std::vector<float> s (b[0][(size_t) ch].size(), 0.0f);
    for (int k = 0; k < numBands; ++k)
        for (size_t i = 0; i < s.size(); ++i)
            s[i] += b[(size_t) k][(size_t) ch][i];
    return s;
}

std::vector<float> impulse (size_t n, size_t at = 0)
{
    std::vector<float> v (n, 0.0f);
    v[at] = 1.0f;
    return v;
}

double binHz (size_t bin, size_t fftSize, double sr) { return (double) bin * sr / (double) fftSize; }

// magnitude (dB) of an impulse response at an exact frequency (direct DFT)
double magAt (const std::vector<float>& h, double hz, double sr)
{
    cd acc {};
    for (size_t n = 0; n < h.size(); ++n)
        if (h[n] != 0.0f)
            acc += (double) h[n] * std::polar (1.0, -2.0 * kPi * hz / sr * (double) n);
    return pstest::dbOf (std::abs (acc));
}

// High-passed copy (8th-order matched Butterworth) for splatter measurements.
std::vector<double> highPassed (const std::vector<float>& x, double sr, double hz)
{
    std::vector<double> y (x.begin(), x.end());
    for (int k = 0; k < 4; ++k)
    {
        Biquad hp;
        hp.setCoeffs (design::highPass (sr, hz, design::butterworthQ (8, k)));
        for (auto& v : y)
            v = hp.processSample (v);
    }
    return y;
}

const char* typeName (CrossoverType t)
{
    return t == CrossoverType::Analog ? "Analog" : (t == CrossoverType::Hybrid ? "Hybrid" : "Digital");
}
} // namespace

TEST_CASE ("xover. analog and hybrid sums are flat within 0.01 dB and every band carries the same phase")
{
    const double sr = 48000.0;
    const size_t N = 65536;
    const float hz[3] { 100.0f, 1000.0f, 8000.0f };

    for (auto type : { CrossoverType::Analog, CrossoverType::Hybrid })
        for (int numBands = 2; numBands <= 4; ++numBands)
        {
            Crossover x;
            x.prepare (sr, 512);
            x.setup (numBands, hz, type);
            CHECK_EQ (x.getLatencySamples (type), 0);

            const auto L = impulse (N), R = impulse (N, 100);
            const auto bands = run (x, L, R, 512);
            const auto sumL = sumOf (bands, numBands, 0);
            const auto sumR = sumOf (bands, numBands, 1);

            const auto S = pstest::spectrumOf (sumL, N);
            double worst = 0.0;
            for (size_t k = 1; k < N / 2; ++k)
            {
                const double f = binHz (k, N, sr);
                if (f >= 10.0 && f <= 20000.0)
                    worst = std::max (worst, std::abs (pstest::dbOf (std::abs (S[k]))));
            }
            pstest::note (std::string (typeName (type)) + " " + std::to_string (numBands) + " bands: sum flatness (dB)", worst);
            CHECK (worst < 0.01);

            // the right channel is the same filter (delayed impulse)
            double chanDiff = 0.0;
            for (size_t i = 0; i + 100 < N; ++i)
                chanDiff = std::max (chanDiff, (double) std::abs (sumR[i + 100] - sumL[i]));
            CHECK (chanDiff < 1e-6);

            // phase alignment: every band's phase equals the phase of the sum wherever the band matters
            double worstDeg = 0.0;
            for (int b = 0; b < numBands; ++b)
            {
                const auto B = pstest::spectrumOf (bands[(size_t) b][0], N);
                for (size_t k = 1; k < N / 2; ++k)
                {
                    const double f = binHz (k, N, sr);
                    if (f < 10.0 || f > 20000.0 || std::abs (B[k]) < 0.01)
                        continue;
                    const double d = std::abs (std::arg (B[k] / S[k])) * 180.0 / kPi;
                    worstDeg = std::max (worstDeg, d);
                }
            }
            pstest::note (std::string (typeName (type)) + " " + std::to_string (numBands) + " bands: worst band phase offset (deg)", worstDeg);
            CHECK (worstDeg < 0.05);

            if (numBands == 4)
            {
                const auto& B0 = bands[0][0];
                const auto& B3 = bands[3][0];
                CHECK_NEAR (magAt (B0, 100.0, sr), -6.0206, 0.05);
                CHECK_NEAR (magAt (B3, 8000.0, sr), -6.0206, 0.05);
                // slope: LR4 = 1 / (1 + 2^4) one octave above, LR8 = 1 / (1 + 2^8)
                const double expect = type == CrossoverType::Analog ? -10.0 * std::log10 (17.0 * 17.0)
                                                                    : -10.0 * std::log10 (257.0 * 257.0);
                CHECK_NEAR (magAt (B0, 200.0, sr), expect, 0.3);
            }
        }
}

TEST_CASE ("xover. digital bands sum to the delayed input (error < -90 dB), also while crossovers move")
{
    const double sr = 48000.0;
    const auto L = pstest::pinkNoise (0.5, 3.0, sr, 11u);
    const auto R = pstest::pinkNoise (0.5, 3.0, sr, 12u);

    for (int numBands = 1; numBands <= 4; ++numBands)
    {
        Crossover x;
        x.prepare (sr, 512);
        float hz[3] { 120.0f, 1000.0f, 6000.0f };
        x.setup (numBands, hz, CrossoverType::Digital);
        const int D = x.getLatencySamples (CrossoverType::Digital);

        const auto bands = run (x, L, R, 333, [&] (size_t start)
        {
            if (start > 48000 && start < 120000)   // keep the crossovers moving for 1.5 s
            {
                const double t = (double) start / sr;
                hz[0] = (float) (60.0 * std::pow (2.0, 3.0 * std::sin (5.0 * t)));
                hz[1] = (float) (900.0 * std::pow (2.0, std::sin (7.0 * t)));
                hz[2] = (float) (7000.0 * std::pow (2.0, 0.5 * std::sin (3.0 * t)));
                x.setup (numBands, hz, CrossoverType::Digital);
            }
        });

        double errSq = 0.0, sigSq = 0.0, peakErr = 0.0;
        bool exact = true;
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto& in = ch == 0 ? L : R;
            const auto s = sumOf (bands, numBands, ch);
            for (size_t i = (size_t) D; i < s.size(); ++i)
            {
                const double e = (double) s[i] - (double) in[i - (size_t) D];
                errSq += e * e;
                sigSq += (double) in[i - (size_t) D] * in[i - (size_t) D];
                peakErr = std::max (peakErr, std::abs (e));
                exact = exact && e == 0.0;
            }
        }
        const double relDb = 10.0 * std::log10 (std::max (errSq, 1e-300) / sigSq);
        pstest::note ("Digital " + std::to_string (numBands) + " bands: sum error vs delayed input (dB rel)", relDb);
        pstest::note ("Digital " + std::to_string (numBands) + " bands: peak sum error (dBFS)", pstest::dbOf (peakErr));
        CHECK (relDb < -90.0);
        CHECK (pstest::dbOf (peakErr) < -90.0);
        if (numBands == 1)
            CHECK (exact);   // one band = the delayed input, bit-exact
    }
}

TEST_CASE ("xover. digital latency is fixed per sample rate, bands are linear phase and cross at -6 dB")
{
    for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        Crossover x;
        x.prepare (sr, 512);
        const float a[3] { 120.0f, 1000.0f, 6000.0f };
        const float b[3] { 25.0f, 400.0f, 15000.0f };
        x.setup (4, a, CrossoverType::Digital);
        const int D = x.getLatencySamples (CrossoverType::Digital);
        x.setup (2, b, CrossoverType::Digital);
        CHECK_EQ (x.getLatencySamples (CrossoverType::Digital), D);
        x.setup (4, a, CrossoverType::Digital);
        pstest::note ("latency at " + std::to_string ((int) sr) + " Hz (samples)", D);
        pstest::note ("latency at " + std::to_string ((int) sr) + " Hz (ms)", 1000.0 * D / sr);
        CHECK (1000.0 * D / sr < 80.0);

        if (sr > 50000.0)
            continue;

        const size_t N = 65536;
        const auto bands = run (x, impulse (N), impulse (N), 512);
        const auto s = sumOf (bands, 4, 0);
        CHECK_NEAR (s[(size_t) D], 1.0, 1e-6);   // the sum is a pure delay of D

        double asym = 0.0;
        for (int band = 0; band < 4; ++band)
            for (int k = 1; k < D; ++k)
                asym = std::max (asym, (double) std::abs (bands[(size_t) band][0][(size_t) (D + k)]
                                                          - bands[(size_t) band][0][(size_t) (D - k)]));
        pstest::note ("worst impulse-response asymmetry around D", asym);
        CHECK (asym < 1e-6);

        const auto& B0 = bands[0][0];
        const auto& B1 = bands[1][0];
        const auto& B3 = bands[3][0];
        CHECK_NEAR (magAt (B0, 120.0, sr), -6.0206, 0.05);
        CHECK_NEAR (magAt (B1, 1000.0, sr), -6.0206, 0.05);
        CHECK_NEAR (magAt (B3, 6000.0, sr), -6.0206, 0.05);
        // constant-Q transition: from 0.4 fc (passband) to 1.6 fc (70 dB down)
        CHECK (magAt (B0, 0.35 * 120.0, sr) > -0.01);
        CHECK (magAt (B0, 1.65 * 120.0, sr) < -65.0);
        CHECK (magAt (B3, 0.35 * 6000.0, sr) < -65.0);
        CHECK (magAt (B3, 1.65 * 6000.0, sr) > -0.01);
    }
}

TEST_CASE ("xover. digital 20 Hz crossover is still well defined")
{
    for (double sr : { 44100.0, 48000.0 })
    {
        Crossover x;
        x.prepare (sr, 512);
        const float hz[1] { 20.0f };
        x.setup (2, hz, CrossoverType::Digital);
        const size_t N = 1 << 17;
        const auto bands = run (x, impulse (N), impulse (N), 512);
        const auto B0 = pstest::spectrumOf (bands[0][0], N);
        const auto& b0 = bands[0][0];
        const auto& b1 = bands[1][0];

        double edge60 = 0.0, edgePass = 0.0;   // where the low band reaches -60 dB / leaves -0.1 dB
        for (size_t k = 1; k < N / 2; ++k)
        {
            const double f = binHz (k, N, sr), m = pstest::dbOf (std::abs (B0[k]));
            if (edgePass == 0.0 && m < -0.1) edgePass = f;
            if (edge60 == 0.0 && m < -60.0) edge60 = f;
        }
        pstest::note ("20 Hz crossover at " + std::to_string ((int) sr) + ": low band leaves -0.1 dB at (Hz)", edgePass);
        pstest::note ("20 Hz crossover at " + std::to_string ((int) sr) + ": low band reaches -60 dB at (Hz)", edge60);
        CHECK (std::abs (pstest::dbOf (std::abs (B0[0]))) < 0.001);   // DC passes the low band at 0 dB
        CHECK (edgePass > 2.0);
        CHECK (edge60 < 40.0);
        CHECK_NEAR (magAt (b0, 20.0, sr), -6.0206, 0.05);
        CHECK_NEAR (magAt (b1, 20.0, sr), -6.0206, 0.05);
        CHECK (magAt (b1, 45.0, sr) > -0.01);
        CHECK (magAt (b1, 1.0, sr) < -40.0);
    }
}

TEST_CASE ("xover. crossover frequency changes are click-free")
{
    const double sr = 48000.0;
    const auto tone = pstest::sine (1000.0, -6.0, 2.0, sr);

    for (auto type : { CrossoverType::Analog, CrossoverType::Hybrid, CrossoverType::Digital })
    {
        Crossover x;
        x.prepare (sr, 512);
        float hz[1] { 300.0f };
        x.setup (2, hz, type);
        const auto bands = run (x, tone, tone, 256, [&] (size_t start)
        {
            if (start >= 48000 && start < 48256) { hz[0] = 3000.0f; x.setup (2, hz, type); }
            if (start >= 72000 && start < 72256) { hz[0] = 300.0f; x.setup (2, hz, type); }
        });

        const auto hp = highPassed (bands[0][0], sr, 8000.0);
        const double floorDb = pstest::dbOf (pstest::peakOf (hp, 20000, 20000));
        const double moveDb = pstest::dbOf (std::max (pstest::peakOf (hp, 47000, 12000), pstest::peakOf (hp, 71000, 12000)));
        pstest::note (std::string (typeName (type)) + ": HF floor of band 1 before the move (dBFS)", floorDb);
        pstest::note (std::string (typeName (type)) + ": HF splatter of band 1 during the moves (dBFS)", moveDb);
        CHECK (moveDb < -80.0);

        // the band really moved: the 1 kHz tone is in band 1 after the first move, in band 2 after the second
        const int D = x.getLatencySamples (type);
        const double inLow = pstest::rmsOf (bands[0][0], 62000, 4800);
        const double inLowAfter = pstest::rmsOf (bands[0][0], 85000, 4800);
        CHECK (pstest::dbOf (inLow) > -9.5);         // -6 dBFS sine = -9.03 dB RMS
        CHECK (pstest::dbOf (inLowAfter) < -30.0);

        // the sum keeps the level of the tone (IIR: time-varying all-pass, RMS over 10 ms; Digital: exact delay)
        const auto s = sumOf (bands, 2, 0);
        double worstLevel = 0.0;
        for (size_t start = 4800 + (size_t) D; start + 480 < s.size(); start += 240)
            worstLevel = std::max (worstLevel, std::abs (pstest::dbOf (pstest::rmsOf (s, start, 480)) + 9.0103));
        pstest::note (std::string (typeName (type)) + ": worst tone level change of the sum (dB)", worstLevel);
        // a decade jump glides in ~60 ms; while the tree is modulated LP^2 + HP^2 = AP holds only approximately
        CHECK (worstLevel < (type == CrossoverType::Analog ? 0.25 : (type == CrossoverType::Hybrid ? 0.6 : 0.01)));
    }
}

TEST_CASE ("xover. one band is a passthrough and band-count changes keep the sum continuous")
{
    const double sr = 44100.0;
    const auto L = pstest::pinkNoise (0.5, 1.0, sr, 5u);
    const auto R = pstest::pinkNoise (0.5, 1.0, sr, 6u);

    for (auto type : { CrossoverType::Analog, CrossoverType::Hybrid, CrossoverType::Digital })
    {
        Crossover x;
        x.prepare (sr, 512);
        const float hz[3] { 200.0f, 2000.0f, 9000.0f };
        x.setup (1, hz, type);
        const int D = x.getLatencySamples (type);
        const auto bands = run (x, L, R, 500);
        bool exact = true;
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto& in = ch == 0 ? L : R;
            for (size_t i = (size_t) D; i < in.size(); ++i)
                exact = exact && bands[0][(size_t) ch][i] == in[i - (size_t) D];
        }
        CHECK (exact);
    }

    // band count 4 -> 2 -> 1 -> 3 with a steady tone: the sum stays at the tone's level, no splatter
    const auto tone = pstest::sine (700.0, -6.0, 2.0, sr);
    for (auto type : { CrossoverType::Analog, CrossoverType::Hybrid, CrossoverType::Digital })
    {
        Crossover x;
        x.prepare (sr, 512);
        const float hz[3] { 200.0f, 2000.0f, 9000.0f };
        int nb = 4;
        x.setup (nb, hz, type);
        std::vector<float> sum (tone.size(), 0.0f);
        Bands bands;
        {
            auto out = run (x, tone, tone, 441, [&] (size_t start)
            {
                if (start == 441 * 40) nb = 2;
                if (start == 441 * 80) nb = 1;
                if (start == 441 * 120) nb = 3;
                x.setup (nb, hz, type);
            });
            // run() only copies x.getNumBands() bands per block: sum what was written
            for (int b = 0; b < 4; ++b)
                for (size_t i = 0; i < sum.size(); ++i)
                    sum[i] += out[(size_t) b][0][i];
        }
        const int D = x.getLatencySamples (type);
        const auto hp = highPassed (sum, sr, 6000.0);
        const double splat = pstest::dbOf (pstest::peakOf (hp, 4410, sum.size() - 4410));
        // level (RMS over 10 cycles), outside the 20 ms crossfades to / from the 1-band passthrough
        double worstLevel = 0.0;
        for (size_t start = 4410 + (size_t) D; start + 630 < sum.size(); start += 315)
        {
            const bool nearPass = (start + 630 > 441 * 80 && start < 441 * 80 + 1800)
                               || (start + 630 > 441 * 120 && start < 441 * 120 + 1800);
            if (! nearPass || type == CrossoverType::Digital)
                worstLevel = std::max (worstLevel, std::abs (pstest::dbOf (pstest::rmsOf (sum, start, 630)) + 9.0103));
        }
        pstest::note (std::string (typeName (type)) + ": band-count changes, sum HF splatter (dBFS)", splat);
        pstest::note (std::string (typeName (type)) + ": band-count changes, worst sum level change (dB)", worstLevel);
        CHECK (splat < -80.0);
        CHECK (worstLevel < 0.05);
    }
}

TEST_CASE ("xover. setup and process do not allocate")
{
    const double sr = 48000.0;
    const auto L = pstest::whiteNoise (0.3, 0.5, sr, 3u);
    for (auto type : { CrossoverType::Analog, CrossoverType::Hybrid, CrossoverType::Digital })
    {
        Crossover x;
        x.prepare (sr, 512);
        float hz[3] { 120.0f, 1000.0f, 6000.0f };
        x.setup (4, hz, type);

        std::vector<std::vector<float>> buf (8, std::vector<float> (512));
        float* chans[4][2];
        float* const* ptrs[4];
        for (int b = 0; b < 4; ++b)
        {
            chans[b][0] = buf[(size_t) (2 * b)].data();
            chans[b][1] = buf[(size_t) (2 * b + 1)].data();
            ptrs[b] = chans[b];
        }

        pstest::AllocationScope scope;
        for (size_t start = 0; start + 512 <= L.size(); start += 512)
        {
            hz[0] = 50.0f + (float) (start % 4000);
            hz[1] = 1000.0f + (float) (start % 3000);
            x.setup (1 + (int) ((start / 512) % 4), hz, type);
            const float* in[2] { L.data() + start, L.data() + start };
            x.process (in, ptrs, 512);
        }
        x.reset();
        CHECK_EQ (scope.count(), 0LL);
    }
}

TEST_CASE ("xover. benchmark (4 bands, 48 kHz stereo)")
{
    const double sr = 48000.0;
    const double seconds = 20.0;
    const auto L = pstest::whiteNoise (0.3, seconds, sr, 21u);
    const auto R = pstest::whiteNoise (0.3, seconds, sr, 22u);

    for (auto type : { CrossoverType::Analog, CrossoverType::Hybrid, CrossoverType::Digital })
    {
        Crossover x;
        x.prepare (sr, 512);
        const float hz[3] { 120.0f, 1000.0f, 6000.0f };
        x.setup (4, hz, type);

        pstest::Stopwatch sw;
        run (x, L, R, 512);
        const double t = sw.seconds();
        pstest::note (std::string (typeName (type)) + ": x real time", seconds / t);
        CHECK (seconds / t > 20.0);
    }
}
