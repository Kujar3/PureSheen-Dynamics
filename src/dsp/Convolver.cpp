#include "dsp/Convolver.h"

#include "core/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

// Uniformly partitioned overlap-save FFT convolver (dsp-common).
//
//   - partition P (rounded up to a power of two, >= 16), FFT size 2P; channels are processed in pairs packed into one
//     complex FFT (first = real part, second = imaginary part) and split into their half spectra, so each channel
//     can have its own kernel; one forward and one inverse FFT per channel pair per partition
//   - frequency-domain delay line of K = ceil (maxKernelLength / P) input spectra per channel
//   - kernels: two slots per channel. setKernel() (one non-realtime producer) writes the spectra of the new kernel
//     into the slot that is neither active nor pending and publishes it (release). At the next partition boundary the
//     audio thread computes the block with both kernels, crossfades old -> new (raised cosine) over that partition, then
//     makes the new slot active and clears "pending" (release), which frees the old slot for the producer.
//   - latency = P exactly: the output of a partition is computed when its input is complete and played during the
//     next partition
// Taps beyond maxKernelLength are ignored.
namespace ps
{
namespace
{
    using cf = std::complex<float>;

    inline void cmac (cf* acc, const cf* a, const cf* b, int n) noexcept
    {
        float* r = reinterpret_cast<float*> (acc);
        const float* x = reinterpret_cast<const float*> (a);
        const float* h = reinterpret_cast<const float*> (b);
        for (int i = 0; i < n; ++i)
        {
            const float xr = x[2 * i], xi = x[2 * i + 1], hr = h[2 * i], hi = h[2 * i + 1];
            r[2 * i] += xr * hr - xi * hi;
            r[2 * i + 1] += xr * hi + xi * hr;
        }
    }
}

struct Convolver::Impl
{
    int P = 0, N2 = 0, H = 0, K = 0, numChannels = 0, maxKernel = 0;
    std::unique_ptr<Fft> fft;

    struct Slot
    {
        std::vector<cf> spectra;   // K x H (half spectra)
        int numPartitions = 0;
    };

    struct Channel
    {
        Slot slot[2];
        std::atomic<int> active { 0 };
        std::atomic<int> pending { -1 };
        std::vector<cf> fdl;       // K x H, newest at fdlPos
        std::vector<float> inPrev, inCur, out;
        std::vector<cf> acc, accOld;
    };
    std::vector<std::unique_ptr<Channel>> channels;
    int fdlPos = 0, pos = 0;

    std::vector<cf> work, workOld, half1, zero;   // audio thread
    std::vector<cf> producerWork;                        // producer thread

    void writeKernel (Slot& s, const float* taps, int numTaps) noexcept
    {
        numTaps = std::clamp (numTaps, 0, maxKernel);
        s.numPartitions = std::max (1, (numTaps + P - 1) / P);

        for (int q = 0; q < s.numPartitions; ++q)
        {
            for (int i = 0; i < N2; ++i)
            {
                const int t = q * P + i;
                producerWork[(size_t) i] = cf { (i < P && t < numTaps) ? taps[t] : 0.0f, 0.0f };
            }
            fft->forward (producerWork.data());
            std::copy (producerWork.begin(), producerWork.begin() + H, s.spectra.begin() + (size_t) q * (size_t) H);
        }
    }

    // Half-spectrum output of channel c with slot s into dst (H bins).
    void accumulate (Channel& c, const Slot& s, cf* dst) noexcept
    {
        std::fill (dst, dst + H, cf {});
        for (int q = 0; q < s.numPartitions; ++q)
        {
            const int idx = (fdlPos - q + K) % K;
            cmac (dst, c.fdl.data() + (size_t) idx * (size_t) H, s.spectra.data() + (size_t) q * (size_t) H, H);
        }
    }

    void blockBoundary() noexcept
    {
        for (int c0 = 0; c0 < numChannels; c0 += 2)
        {
            Channel& a = *channels[(size_t) c0];
            Channel* b = c0 + 1 < numChannels ? channels[(size_t) c0 + 1].get() : nullptr;

            // forward FFT of [prev, cur] for both channels
            for (int i = 0; i < P; ++i)
            {
                work[(size_t) i] = cf { a.inPrev[(size_t) i], b != nullptr ? b->inPrev[(size_t) i] : 0.0f };
                work[(size_t) (P + i)] = cf { a.inCur[(size_t) i], b != nullptr ? b->inCur[(size_t) i] : 0.0f };
            }
            fft->forward (work.data());
            fft->splitPacked (work.data(), a.fdl.data() + (size_t) fdlPos * (size_t) H,
                              b != nullptr ? b->fdl.data() + (size_t) fdlPos * (size_t) H : half1.data());

            Channel* pair[2] { &a, b };
            int fadeFrom[2] { -1, -1 };
            bool anyFade = false;

            for (int k = 0; k < 2; ++k)
            {
                Channel* c = pair[k];
                if (c == nullptr)
                    continue;

                const int p = c->pending.load (std::memory_order_acquire);
                int act = c->active.load (std::memory_order_relaxed);
                if (p >= 0)
                {
                    fadeFrom[k] = act;
                    act = p;
                    anyFade = true;
                }
                accumulate (*c, c->slot[act], c->acc.data());
                if (fadeFrom[k] >= 0)
                    accumulate (*c, c->slot[fadeFrom[k]], c->accOld.data());
            }

            // new outputs (and old ones for a crossfade) through one inverse FFT per pair each
            fft->mergePacked (a.acc.data(), b != nullptr ? b->acc.data() : zero.data(), work.data());
            fft->inverse (work.data());

            if (anyFade)
            {
                fft->mergePacked (fadeFrom[0] >= 0 ? a.accOld.data() : a.acc.data(),
                                  b == nullptr ? zero.data() : (fadeFrom[1] >= 0 ? b->accOld.data() : b->acc.data()),
                                  workOld.data());
                fft->inverse (workOld.data());
            }

            const double inv = 3.14159265358979323846 / (double) P;
            for (int i = 0; i < P; ++i)
            {
                const cf y = work[(size_t) (P + i)];
                float ya = y.real(), yb = y.imag();
                if (anyFade)
                {
                    const float r = (float) (0.5 - 0.5 * std::cos (((double) i + 0.5) * inv));   // raised cosine
                    const cf o = workOld[(size_t) (P + i)];
                    if (fadeFrom[0] >= 0) ya = o.real() + (ya - o.real()) * r;
                    if (fadeFrom[1] >= 0) yb = o.imag() + (yb - o.imag()) * r;
                }
                a.out[(size_t) i] = ya;
                if (b != nullptr)
                    b->out[(size_t) i] = yb;
            }

            for (int k = 0; k < 2; ++k)
            {
                Channel* c = pair[k];
                if (c == nullptr || fadeFrom[k] < 0)
                    continue;
                c->active.store (c->pending.load (std::memory_order_relaxed), std::memory_order_release);
                c->pending.store (-1, std::memory_order_release);
            }

            std::swap (a.inPrev, a.inCur);
            if (b != nullptr)
                std::swap (b->inPrev, b->inCur);
        }

        fdlPos = (fdlPos + 1) % K;
    }
};

Convolver::Convolver() : impl (std::make_unique<Impl>()) {}
Convolver::~Convolver() = default;

void Convolver::prepare (int maxKernelLength, int partitionSize, int numChannels)
{
    auto& m = *impl;
    int P = 16;
    while (P < partitionSize)
        P <<= 1;

    m.P = P;
    m.N2 = 2 * P;
    m.H = P + 1;
    m.maxKernel = std::max (1, maxKernelLength);
    m.K = (m.maxKernel + P - 1) / P;
    m.numChannels = std::max (1, numChannels);

    int order = 0;
    while ((1 << order) < m.N2)
        ++order;
    m.fft = std::make_unique<Fft> (order);

    m.work.assign ((size_t) m.N2, cf {});
    m.workOld.assign ((size_t) m.N2, cf {});
    m.producerWork.assign ((size_t) m.N2, cf {});
    m.half1.assign ((size_t) m.H, cf {});
    m.zero.assign ((size_t) m.H, cf {});

    m.channels.clear();
    for (int ch = 0; ch < m.numChannels; ++ch)
    {
        auto c = std::make_unique<Impl::Channel>();
        for (auto& s : c->slot)
            s.spectra.assign ((size_t) (m.K * m.H), cf {});
        c->fdl.assign ((size_t) (m.K * m.H), cf {});
        c->inPrev.assign ((size_t) P, 0.0f);
        c->inCur.assign ((size_t) P, 0.0f);
        c->out.assign ((size_t) P, 0.0f);
        c->acc.assign ((size_t) m.H, cf {});
        c->accOld.assign ((size_t) m.H, cf {});

        const float identity = 1.0f;
        m.writeKernel (c->slot[0], &identity, 1);
        c->active.store (0);
        c->pending.store (-1);
        m.channels.push_back (std::move (c));
    }
    reset();
}

void Convolver::reset() noexcept
{
    auto& m = *impl;
    for (auto& c : m.channels)
    {
        // a pending kernel is installed immediately (nothing is playing through the old one any more)
        const int p = c->pending.load (std::memory_order_acquire);
        if (p >= 0)
        {
            c->active.store (p, std::memory_order_release);
            c->pending.store (-1, std::memory_order_release);
        }
        std::fill (c->fdl.begin(), c->fdl.end(), cf {});
        std::fill (c->inPrev.begin(), c->inPrev.end(), 0.0f);
        std::fill (c->inCur.begin(), c->inCur.end(), 0.0f);
        std::fill (c->out.begin(), c->out.end(), 0.0f);
    }
    m.fdlPos = 0;
    m.pos = 0;
}

bool Convolver::setKernel (int channel, const float* taps, int numTaps)
{
    auto& m = *impl;
    if (channel < 0 || channel >= (int) m.channels.size() || (taps == nullptr && numTaps > 0))
        return false;

    auto& c = *m.channels[(size_t) channel];
    if (c.pending.load (std::memory_order_acquire) >= 0)
        return false;

    const int target = 1 - c.active.load (std::memory_order_acquire);
    const float zeroTap = 0.0f;
    m.writeKernel (c.slot[target], numTaps > 0 ? taps : &zeroTap, std::max (numTaps, 1));
    c.pending.store (target, std::memory_order_release);
    return true;
}

bool Convolver::isKernelPending (int channel) const noexcept
{
    if (channel < 0 || channel >= (int) impl->channels.size())
        return false;
    return impl->channels[(size_t) channel]->pending.load (std::memory_order_acquire) >= 0;
}

int Convolver::getLatencySamples() const noexcept { return impl->P; }

void Convolver::process (float* const* data, int numSamples) noexcept
{
    auto& m = *impl;
    if (m.channels.empty())
        return;

    int done = 0;
    while (done < numSamples)
    {
        const int n = std::min (numSamples - done, m.P - m.pos);
        for (int ch = 0; ch < m.numChannels; ++ch)
        {
            auto& c = *m.channels[(size_t) ch];
            float* x = data[ch] + done;
            for (int i = 0; i < n; ++i)
            {
                c.inCur[(size_t) (m.pos + i)] = x[i];
                x[i] = c.out[(size_t) (m.pos + i)];
            }
        }

        m.pos += n;
        done += n;
        if (m.pos == m.P)
        {
            m.pos = 0;
            m.blockBoundary();
        }
    }
}
} // namespace ps
