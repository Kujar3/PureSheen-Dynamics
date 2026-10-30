#include "dsp/SpectralEngine.h"

#include "core/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

// Residual STFT engine (see SpectralEngine.h). Implemented with the contracts so the three spectral modules can be
// built on it in parallel.
//
// Streaming layout: one ring of N samples per channel serves as both the input history (the last N samples, oldest at
// `pos`) and the dry delay (reading ring[pos] before overwriting it gives x[n - N]). A second ring accumulates the
// overlap-added correction for the output times n .. n + N - 1 (slot = time mod N, cleared when read). Every H samples
// the last N inputs are windowed, transformed (both channels in one complex FFT), the client's gains applied as
// (G - 1) X, transformed back, windowed again and added to the correction ring. Latency is exactly N.
namespace ps
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

int orderOf (int n) noexcept
{
    int o = 0;
    while ((1 << o) < n)
        ++o;
    return o;
}
} // namespace

struct SpectralEngine::Impl
{
    int fftSize = 1024, hop = 256, numBins = 513;
    double binHz = 46.875, sampleRate = 48000.0;
    std::unique_ptr<Fft> fft;
    std::vector<float> window;
    std::vector<float> ring[2], acc[2];
    std::vector<std::complex<float>> z, a, b;
    std::vector<float> power[2], gain[2];
    int pos = 0, hopCount = 0;
    float powerScale = 1.0f;
    Client* client = nullptr;

    void frame (bool linked) noexcept
    {
        const int n = fftSize;
        double energy = 0.0;
        for (int j = 0; j < n; ++j)
        {
            const int r = (pos + j) & (n - 1);
            const float w = window[(size_t) j];
            const float l = ring[0][(size_t) r] * w, rr = ring[1][(size_t) r] * w;
            z[(size_t) j] = { l, rr };
            energy += (double) l * (double) l + (double) rr * (double) rr;
        }

        fft->forward (z.data());
        fft->splitPacked (z.data(), a.data(), b.data());

        for (int k = 0; k < numBins; ++k)
        {
            power[0][(size_t) k] = std::norm (a[(size_t) k]) * powerScale;
            power[1][(size_t) k] = std::norm (b[(size_t) k]) * powerScale;
        }
        std::fill (gain[0].begin(), gain[0].end(), 1.0f);
        std::fill (gain[1].begin(), gain[1].end(), 1.0f);

        Frame f;
        f.numBins = numBins;
        f.binHz = binHz;
        f.hop = hop;
        f.power[0] = power[0].data();
        f.power[1] = power[1].data();
        f.gain[0] = gain[0].data();
        f.gain[1] = linked ? gain[0].data() : gain[1].data();
        f.linked = linked;
        // -150 dBFS mean power of the windowed frame (window power N / 2 per channel)
        f.silent = energy < 1.0e-15 * (double) n;

        const bool unity = client == nullptr || client->computeGains (f);
        if (unity)
            return;

        const float* g0 = gain[0].data();
        const float* g1 = linked ? gain[0].data() : gain[1].data();
        bool any = false;
        for (int k = 0; k < numBins; ++k)
        {
            const float c0 = g0[k] - 1.0f, c1 = g1[k] - 1.0f;
            any = any || c0 != 0.0f || c1 != 0.0f;
            a[(size_t) k] *= c0;
            b[(size_t) k] *= c1;
        }
        if (! any)
            return;

        fft->mergePacked (a.data(), b.data(), z.data());
        fft->inverse (z.data());

        for (int j = 0; j < n; ++j)
        {
            const int r = (pos + j) & (n - 1);
            const float w = 0.5f * window[(size_t) j];   // synthesis window x OLA scale (sum of w^2 over hops = 2)
            acc[0][(size_t) r] += z[(size_t) j].real() * w;
            acc[1][(size_t) r] += z[(size_t) j].imag() * w;
        }
    }
};

SpectralEngine::SpectralEngine() : impl (std::make_unique<Impl>()) {}
SpectralEngine::~SpectralEngine() = default;

void SpectralEngine::prepare (double sampleRate, int baseFftSize, int)
{
    auto& d = *impl;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int k = d.sampleRate <= 50000.0 ? 1 : (d.sampleRate <= 100000.0 ? 2 : 4);
    const int base = 1 << orderOf (std::clamp (baseFftSize, 256, 8192));
    d.fftSize = base * k;
    d.hop = d.fftSize / 4;
    d.numBins = d.fftSize / 2 + 1;
    d.binHz = d.sampleRate / d.fftSize;
    d.fft = std::make_unique<Fft> (orderOf (d.fftSize));
    d.window.resize ((size_t) d.fftSize);
    for (int j = 0; j < d.fftSize; ++j)
        d.window[(size_t) j] = (float) std::sin (kPi * (double) j / (double) d.fftSize);   // sqrt of a periodic Hann
    for (int c = 0; c < 2; ++c)
    {
        d.ring[c].assign ((size_t) d.fftSize, 0.0f);
        d.acc[c].assign ((size_t) d.fftSize, 0.0f);
        d.power[c].assign ((size_t) d.numBins, 0.0f);
        d.gain[c].assign ((size_t) d.numBins, 1.0f);
    }
    d.z.assign ((size_t) d.fftSize, {});
    d.a.assign ((size_t) d.numBins, {});
    d.b.assign ((size_t) d.numBins, {});
    // |X|^2 of a full-scale sine summed over its main lobe = N sum (w^2) / 4 = N^2 / 8
    d.powerScale = 8.0f / ((float) d.fftSize * (float) d.fftSize);
    reset();
}

void SpectralEngine::reset() noexcept
{
    auto& d = *impl;
    for (int c = 0; c < 2; ++c)
    {
        std::fill (d.ring[c].begin(), d.ring[c].end(), 0.0f);
        std::fill (d.acc[c].begin(), d.acc[c].end(), 0.0f);
    }
    d.pos = 0;
    d.hopCount = 0;
}

void SpectralEngine::setClient (Client* c) noexcept { impl->client = c; }

void SpectralEngine::process (float* const* channels, int numSamples, bool linked) noexcept
{
    auto& d = *impl;
    if (d.ring[0].empty() || numSamples <= 0)
        return;

    const int mask = d.fftSize - 1;
    for (int i = 0; i < numSamples; ++i)
    {
        for (int c = 0; c < 2; ++c)
        {
            auto& ring = d.ring[c];
            auto& acc = d.acc[c];
            const float x = channels[c][i];
            const float correction = acc[(size_t) d.pos];
            channels[c][i] = correction == 0.0f ? ring[(size_t) d.pos] : ring[(size_t) d.pos] + correction;
            acc[(size_t) d.pos] = 0.0f;
            ring[(size_t) d.pos] = x;
        }
        d.pos = (d.pos + 1) & mask;

        if (++d.hopCount >= d.hop)
        {
            d.hopCount = 0;
            d.frame (linked);
        }
    }
}

int SpectralEngine::getFftSize() const noexcept { return impl->fftSize; }
int SpectralEngine::getHopSize() const noexcept { return impl->hop; }
int SpectralEngine::getLatencySamples() const noexcept { return impl->fftSize; }
double SpectralEngine::getBinHz() const noexcept { return impl->binHz; }
} // namespace ps
