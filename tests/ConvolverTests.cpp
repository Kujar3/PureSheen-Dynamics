#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "core/Fft.h"
#include "dsp/Convolver.h"

#include <atomic>
#include <cmath>
#include <complex>
#include <string>
#include <thread>
#include <vector>

// Convolver (dsp/Convolver.cpp) and the FFT additions (core/Fft.cpp). Prefix "conv.".
namespace
{
using namespace ps;

std::vector<double> directConvolution (const std::vector<float>& x, const std::vector<float>& h)
{
    std::vector<double> y (x.size(), 0.0);
    for (size_t n = 0; n < x.size(); ++n)
    {
        double acc = 0.0;
        const size_t kmax = std::min (h.size(), n + 1);
        for (size_t k = 0; k < kmax; ++k)
            acc += (double) h[k] * (double) x[n - k];
        y[n] = acc;
    }
    return y;
}

// Processes in irregular block sizes, in place.
void processIrregular (Convolver& c, std::vector<float>& L, std::vector<float>& R, int maxBlock)
{
    size_t pos = 0;
    int k = 0;
    while (pos < L.size())
    {
        const int n = (int) std::min ((size_t) (1 + (k++ * 131) % maxBlock), L.size() - pos);
        float* ch[2] { L.data() + pos, R.data() + pos };
        c.process (ch, n);
        pos += (size_t) n;
    }
}
} // namespace

TEST_CASE ("conv. fft inverse and packed real transforms are exact")
{
    for (int order : { 4, 9, 12 })
    {
        Fft fft (order);
        const int n = fft.size();
        pstest::Lcg lcg (7u);
        std::vector<std::complex<float>> x ((size_t) n), y;
        for (auto& v : x)
            v = { lcg.next(), lcg.next() };
        y = x;
        fft.forward (y.data());
        fft.inverse (y.data());
        double err = 0.0;
        for (int i = 0; i < n; ++i)
            err = std::max (err, (double) std::abs (y[(size_t) i] - x[(size_t) i]));
        CHECK (err < 1e-5);

        // two real signals in one transform
        std::vector<float> a ((size_t) n), b ((size_t) n);
        for (int i = 0; i < n; ++i) { a[(size_t) i] = lcg.next(); b[(size_t) i] = lcg.next(); }
        std::vector<std::complex<float>> z ((size_t) n), A ((size_t) (n / 2 + 1)), B ((size_t) (n / 2 + 1));
        for (int i = 0; i < n; ++i)
            z[(size_t) i] = { a[(size_t) i], b[(size_t) i] };
        fft.forward (z.data());
        fft.splitPacked (z.data(), A.data(), B.data());

        std::vector<pstest::cd> refA ((size_t) n), refB ((size_t) n);
        for (int i = 0; i < n; ++i) { refA[(size_t) i] = a[(size_t) i]; refB[(size_t) i] = b[(size_t) i]; }
        pstest::fftInPlace (refA);
        pstest::fftInPlace (refB);
        double e2 = 0.0;
        for (int k = 0; k <= n / 2; ++k)
            e2 = std::max (e2, std::max (std::abs (pstest::cd (A[(size_t) k]) - refA[(size_t) k]),
                                         std::abs (pstest::cd (B[(size_t) k]) - refB[(size_t) k])));
        CHECK (e2 < 1e-4 * n);

        fft.mergePacked (A.data(), B.data(), z.data());
        fft.inverse (z.data());
        double e3 = 0.0;
        for (int i = 0; i < n; ++i)
            e3 = std::max (e3, std::max ((double) std::abs (z[(size_t) i].real() - a[(size_t) i]),
                                         (double) std::abs (z[(size_t) i].imag() - b[(size_t) i])));
        CHECK (e3 < 1e-5);
    }
}

TEST_CASE ("conv. matches direct convolution (< -100 dB) with latency == partition size")
{
    for (int partition : { 64, 256, 1000 })
    {
        Convolver c;
        c.prepare (5000, partition, 2);
        const int P = c.getLatencySamples();
        pstest::note ("partition request " + std::to_string (partition) + " -> latency", P);
        CHECK (P >= partition && (P & (P - 1)) == 0);

        // different kernels per channel (M/S use)
        auto hL = pstest::whiteNoise (0.2, 4100.0 / 48000.0, 48000.0, 31u);
        auto hR = pstest::whiteNoise (0.3, 1234.0 / 48000.0, 48000.0, 32u);
        for (size_t i = 0; i < hL.size(); ++i)
            hL[i] *= (float) std::exp (-(double) i / 900.0);
        REQUIRE (c.setKernel (0, hL.data(), (int) hL.size()));
        REQUIRE (c.setKernel (1, hR.data(), (int) hR.size()));

        // the kernels are installed at the first partition boundary: feed one partition of silence first
        std::vector<float> zL ((size_t) P, 0.0f), zR ((size_t) P, 0.0f);
        float* zc[2] { zL.data(), zR.data() };
        c.process (zc, P);
        CHECK (! c.isKernelPending (0));
        CHECK (! c.isKernelPending (1));

        const auto xL = pstest::whiteNoise (0.5, 0.5, 48000.0, 41u);
        const auto xR = pstest::pinkNoise (0.5, 0.5, 48000.0, 42u);
        auto yL = xL, yR = xR;
        processIrregular (c, yL, yR, 700);

        const auto rL = directConvolution (xL, hL), rR = directConvolution (xR, hR);
        double e2 = 0.0, s2 = 0.0;
        for (size_t n = (size_t) P; n < xL.size(); ++n)
        {
            const double eL = (double) yL[n] - rL[n - (size_t) P], eR = (double) yR[n] - rR[n - (size_t) P];
            e2 += eL * eL + eR * eR;
            s2 += rL[n - (size_t) P] * rL[n - (size_t) P] + rR[n - (size_t) P] * rR[n - (size_t) P];
        }
        const double rel = 10.0 * std::log10 (e2 / s2);
        pstest::note ("partition " + std::to_string (P) + ": error vs direct convolution (dB)", rel);
        CHECK (rel < -100.0);
    }
}

TEST_CASE ("conv. kernel swaps crossfade over exactly one partition")
{
    Convolver c;
    c.prepare (1024, 128, 2);
    const int P = c.getLatencySamples();
    const float one = 1.0f, half = 0.5f;
    c.setKernel (0, &one, 1);
    c.setKernel (1, &one, 1);

    std::vector<float> dcL (4096, 1.0f), dcR (4096, 1.0f);
    float* ch[2] { dcL.data(), dcR.data() };
    c.process (ch, 1024);   // settles at gain 1

    CHECK (c.setKernel (0, &half, 1));
    CHECK (c.isKernelPending (0));
    CHECK (! c.setKernel (0, &one, 1));   // refused while the previous kernel is still pending

    std::fill (dcL.begin(), dcL.end(), 1.0f);
    std::fill (dcR.begin(), dcR.end(), 1.0f);
    // 1024 is a multiple of P: the next sample starts a new partition; the swap is picked up at the end of it
    c.process (ch, 2048);
    CHECK (! c.isKernelPending (0));

    // channel 0: P samples at 1 (the partition already computed), then a raised ramp over P samples to 0.5, then 0.5
    int firstChange = -1, lastChange = -1;
    double maxStep = 0.0;
    for (int i = 1; i < 2048; ++i)
    {
        const double d = (double) dcL[(size_t) i] - (double) dcL[(size_t) i - 1];
        if (std::abs (d) > 1e-7)
        {
            if (firstChange < 0) firstChange = i;
            lastChange = i;
        }
        maxStep = std::max (maxStep, std::abs (d));
    }
    pstest::note ("crossfade starts at sample", firstChange);
    pstest::note ("crossfade ends at sample", lastChange);
    pstest::note ("largest per-sample gain step", maxStep);
    CHECK_EQ (lastChange - firstChange, P);   // P faded samples = P + 1 steps (into and out of the fade)
    CHECK (maxStep < 1.0 / P);
    CHECK_NEAR (dcL[2047], 0.5, 1e-6);
    CHECK_NEAR (dcR[2047], 1.0, 1e-6);   // the other channel is untouched
}

TEST_CASE ("conv. kernel replacement from another thread is lock-free and never tears")
{
    Convolver c;
    const int P = 256;
    c.prepare (2048, P, 2);

    // kernels: gain g at tap 0 (+ a long tail of exact zeros, so every partition is in use)
    std::vector<float> kA (2048, 0.0f), kB (2048, 0.0f);
    kA[0] = 1.0f;
    kB[0] = 0.25f;
    c.setKernel (0, kA.data(), 2048);
    c.setKernel (1, kA.data(), 2048);

    std::atomic<bool> stop { false };
    std::atomic<int> installs { 0 };
    std::thread producer ([&]
    {
        int i = 0;
        while (! stop.load())
        {
            const auto& k = ((i / 2) & 1) ? kA : kB;
            if (c.setKernel (i % 2, k.data(), 2048))
            {
                ++i;
                installs.fetch_add (1);
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });

    std::vector<float> L (512), R (512);
    float* ch[2] { L.data(), R.data() };
    bool ok = true;
    double maxStep = 0.0;
    float prevL = 1.0f, prevR = 1.0f;
    int blocks = 0;
    for (; blocks < 4000; ++blocks)
    {
        std::fill (L.begin(), L.end(), 1.0f);
        std::fill (R.begin(), R.end(), 1.0f);
        c.process (ch, 512);
        if (blocks < 2)
        {
            prevL = L[511];
            prevR = R[511];
            continue;
        }
        for (int i = 0; i < 512; ++i)
        {
            ok = ok && std::isfinite (L[(size_t) i]) && L[(size_t) i] >= 0.25f - 1e-5f && L[(size_t) i] <= 1.0f + 1e-5f;
            ok = ok && std::isfinite (R[(size_t) i]) && R[(size_t) i] >= 0.25f - 1e-5f && R[(size_t) i] <= 1.0f + 1e-5f;
            maxStep = std::max (maxStep, (double) std::max (std::abs (L[(size_t) i] - prevL), std::abs (R[(size_t) i] - prevR)));
            prevL = L[(size_t) i];
            prevR = R[(size_t) i];
        }
    }
    stop = true;
    producer.join();

    pstest::note ("kernels installed while processing", installs.load());
    pstest::note ("largest per-sample gain step", maxStep);
    CHECK (installs.load() > 100);
    CHECK (ok);
    CHECK (maxStep <= 0.75 * 1.6 / P);   // raised-cosine / linear fade over one partition, no jumps
}

TEST_CASE ("conv. process does not allocate")
{
    Convolver c;
    c.prepare (8192, 512, 2);
    std::vector<float> h (8000);
    pstest::Lcg lcg (3u);
    for (auto& v : h) v = 0.01f * lcg.next();
    c.setKernel (0, h.data(), (int) h.size());
    c.setKernel (1, h.data(), (int) h.size());
    auto L = pstest::whiteNoise (0.5, 1.0, 48000.0, 5u), R = L;

    pstest::AllocationScope scope;
    processIrregular (c, L, R, 1024);
    c.reset();
    CHECK_EQ (scope.count(), 0LL);

    // setKernel (producer side) does not allocate either
    pstest::AllocationScope s2;
    float* ch[2] { L.data(), R.data() };
    c.process (ch, 1024);
    c.setKernel (0, h.data(), 4000);
    CHECK_EQ (s2.count(), 0LL);
}

TEST_CASE ("conv. benchmark (16384 taps, partition 512, stereo, 48 kHz)")
{
    Convolver c;
    c.prepare (16384, 512, 2);
    auto h = pstest::whiteNoise (0.01, 16384.0 / 48000.0, 48000.0, 8u);
    c.setKernel (0, h.data(), (int) h.size());
    c.setKernel (1, h.data(), (int) h.size());
    const double seconds = 10.0;
    auto L = pstest::whiteNoise (0.5, seconds, 48000.0, 9u), R = L;
    pstest::Stopwatch sw;
    processIrregular (c, L, R, 512);
    const double t = sw.seconds();
    pstest::note ("x real time", seconds / t);
    CHECK (seconds / t > 20.0);
}
