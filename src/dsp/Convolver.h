#pragma once

#include <atomic>
#include <memory>

// Stereo uniformly-partitioned FFT convolver with lock-free kernel replacement, for the linear-phase EQ (and later
// Match EQ). Owner: dsp-common.
//
//   - prepare (maxKernelLength, partitionSize): allocates everything (kernel slots, FFT buffers). The partition
//     actually used, P, is partitionSize rounded up to a power of two, at least 16 (e.g. 500 -> 512, 8 -> 16).
//   - latency = P samples (input is buffered into partitions): getLatencySamples() returns P, which equals
//     partitionSize only when that is already a power of two >= 16; the kernel's own delay is the caller's
//   - setKernel(): call from ONE non-realtime thread (the EQ's designer). Copies the taps into a free slot and
//     publishes it; the audio thread picks it up at the next partition boundary and crossfades old -> new output over
//     one partition (click-free). Returns false (nothing changed) while a previous kernel is still being installed.
//   - both channels may have different kernels (M/S, L/R): setKernel (channel, ...)
// Realtime: process() is allocation-free and lock-free.
namespace ps
{
class Convolver
{
public:
    Convolver();
    ~Convolver();

    void prepare (int maxKernelLength, int partitionSize, int numChannels = 2);
    void reset() noexcept;

    bool setKernel (int channel, const float* taps, int numTaps);   // non-realtime thread
    bool isKernelPending (int channel) const noexcept;

    int getLatencySamples() const noexcept;                          // == P (partition used, see above); 0 before prepare

    void process (float* const* channels, int numSamples) noexcept;  // in place

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
