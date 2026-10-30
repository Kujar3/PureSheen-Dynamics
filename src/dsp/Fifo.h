#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

// Lock-free single-producer / single-consumer helpers between the audio thread and the UI. Owner: dsp-common
// (header-only).
namespace ps
{
//==============================================================================
// Relaxed atomic float (meters). Writer: audio thread. Reader: UI.
class AtomicFloat
{
public:
    explicit AtomicFloat (float v = 0.0f) noexcept : value (v) {}
    void store (float v) noexcept { value.store (v, std::memory_order_relaxed); }
    float load() const noexcept { return value.load (std::memory_order_relaxed); }

    // Peak-hold style publish: keeps the max until the reader takes it (readAndReset).
    void storeMax (float v) noexcept
    {
        float cur = value.load (std::memory_order_relaxed);
        while (v > cur && ! value.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }
    float exchange (float v) noexcept { return value.exchange (v, std::memory_order_relaxed); }

private:
    std::atomic<float> value;
};

//==============================================================================
// Stereo sample ring. The audio thread pushes, one UI consumer pulls. When full, push drops the oldest data by
// letting the consumer skip ahead (the UI only ever wants the latest audio).
//
// Race-free (seqlock style): samples are relaxed atomics (plain moves on x86/x64), so a consumer that stalls while
// the producer wraps never reads a torn or undefined value. The producer announces how far it is about to write
// (writeBegin) before storing samples and publishes writeCount after them. The consumer copies, then re-reads
// writeBegin: every copied sample older than writeBegin - capacity may have been overwritten during the copy and is
// discarded, so pull() only ever returns intact, contiguous, newest samples.
class StereoSampleFifo
{
public:
    void prepare (int capacityPowerOfTwo)
    {
        capacity = 1;
        while (capacity < capacityPowerOfTwo) capacity <<= 1;
        left.reset (new std::atomic<float>[(size_t) capacity]);
        right.reset (new std::atomic<float>[(size_t) capacity]);
        for (int i = 0; i < capacity; ++i)
        {
            left[(size_t) i].store (0.0f, std::memory_order_relaxed);
            right[(size_t) i].store (0.0f, std::memory_order_relaxed);
        }
        writeBegin.store (0);
        writeCount.store (0);
        readCount = 0;
    }

    void push (const float* l, const float* r, int n) noexcept
    {
        if (capacity == 0 || n <= 0) return;
        const auto w = writeCount.load (std::memory_order_relaxed);
        const auto end = w + (unsigned long long) n;
        writeBegin.store (end, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);   // writeBegin is visible before any sample store
        const auto mask = (unsigned long long) (capacity - 1);
        for (int i = 0; i < n; ++i)
        {
            const auto idx = (size_t) ((w + (unsigned long long) i) & mask);
            left[idx].store (l[i], std::memory_order_relaxed);
            right[idx].store (r != nullptr ? r[i] : l[i], std::memory_order_relaxed);
        }
        writeCount.store (end, std::memory_order_release);
    }

    // Copies up to maxSamples of the newest unread samples; returns the count. UI thread only.
    int pull (float* l, float* r, int maxSamples) noexcept
    {
        if (capacity == 0 || maxSamples <= 0) return 0;
        const auto cap = (unsigned long long) capacity;
        const auto w = writeCount.load (std::memory_order_acquire);
        if (w - readCount > cap) readCount = w - cap;
        auto available = (int) (w - readCount);
        if (available > maxSamples) { readCount = w - (unsigned long long) maxSamples; available = maxSamples; }

        const auto mask = cap - 1;
        for (int i = 0; i < available; ++i)
        {
            const auto idx = (size_t) ((readCount + (unsigned long long) i) & mask);
            l[i] = left[idx].load (std::memory_order_relaxed);
            if (r != nullptr) r[i] = right[idx].load (std::memory_order_relaxed);
        }

        // samples the producer may have overwritten while we copied
        std::atomic_thread_fence (std::memory_order_acquire);
        const auto begin = writeBegin.load (std::memory_order_relaxed);
        const auto firstValid = begin > cap ? begin - cap : 0ull;
        int skip = 0;
        if (readCount < firstValid)
            skip = (int) std::min<unsigned long long> (firstValid - readCount, (unsigned long long) available);
        if (skip > 0)
        {
            for (int i = skip; i < available; ++i)
            {
                l[i - skip] = l[i];
                if (r != nullptr) r[i - skip] = r[i];
            }
        }
        readCount = w;
        return available - skip;
    }

    unsigned long long getWriteCount() const noexcept { return writeCount.load (std::memory_order_acquire); }

private:
    std::unique_ptr<std::atomic<float>[]> left, right;
    int capacity = 0;
    std::atomic<unsigned long long> writeBegin { 0 }, writeCount { 0 };
    unsigned long long readCount = 0;
};

} // namespace ps
