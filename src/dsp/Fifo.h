#pragma once

#include <atomic>
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
class StereoSampleFifo
{
public:
    void prepare (int capacityPowerOfTwo)
    {
        capacity = 1;
        while (capacity < capacityPowerOfTwo) capacity <<= 1;
        left.assign ((size_t) capacity, 0.0f);
        right.assign ((size_t) capacity, 0.0f);
        writeCount.store (0);
        readCount = 0;
    }

    void push (const float* l, const float* r, int n) noexcept
    {
        if (capacity == 0) return;
        auto w = writeCount.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            const auto idx = (size_t) ((w + (unsigned long long) i) & (unsigned long long) (capacity - 1));
            left[idx] = l[i];
            right[idx] = r != nullptr ? r[i] : l[i];
        }
        writeCount.store (w + (unsigned long long) n, std::memory_order_release);
    }

    // Copies up to maxSamples of the newest unread samples; returns the count. UI thread only.
    int pull (float* l, float* r, int maxSamples) noexcept
    {
        const auto w = writeCount.load (std::memory_order_acquire);
        if (w - readCount > (unsigned long long) capacity) readCount = w - (unsigned long long) capacity;
        auto available = (int) (w - readCount);
        if (available > maxSamples) { readCount = w - (unsigned long long) maxSamples; available = maxSamples; }
        for (int i = 0; i < available; ++i)
        {
            const auto idx = (size_t) ((readCount + (unsigned long long) i) & (unsigned long long) (capacity - 1));
            l[i] = left[idx];
            if (r != nullptr) r[i] = right[idx];
        }
        readCount += (unsigned long long) available;
        return available;
    }

    unsigned long long getWriteCount() const noexcept { return writeCount.load (std::memory_order_acquire); }

private:
    std::vector<float> left, right;
    int capacity = 0;
    std::atomic<unsigned long long> writeCount { 0 };
    unsigned long long readCount = 0;
};

} // namespace ps
