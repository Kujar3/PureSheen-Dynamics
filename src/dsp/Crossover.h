#pragma once

#include "dsp/Types.h"

#include <memory>

// Stereo N-band splitter for the multiband modules (Dynamics, Imager, Exciter). Owner: dsp-common.
//
// Guarantees (tested in CoreTests):
//   - Analog : LR4 tree; sum of the bands = input passed through an allpass (flat magnitude, +-0.01 dB), 0 latency
//   - Hybrid : LR8 (48 dB/oct) tree with allpass compensation so every band carries the same phase; sum = allpass of
//              input; 0 latency. (The compensated LR4 tree of Analog is phase-aligned too; Hybrid differs by its steeper,
//              Digital-like band separation at zero latency.)
//   - Digital: linear-phase FIR bands (complementary: sum of bands = input delayed by getLatencySamples(), error
//              < -90 dB); latency = (fir length - 1) / 2, fixed for a given sample rate (independent of the crossover
//              frequencies, so moving a crossover never changes the plug-in latency): 3327 samples x sample-rate family
//              (1 up to 50 kHz, 2 up to 100 kHz, 4, 8), i.e. 75.4 ms at 44.1 kHz, 69.3 ms at 48 kHz
//   - crossover frequency changes are click-free (Analog/Hybrid: smoothed coefficients; Digital: crossfaded kernels)
//   - one band = passthrough (Digital still delays by its latency so a band-count change never shifts latency)
//   - type changes are click-free: the new engine is warmed and crossfaded in (20 ms raised cosine). Digital's input
//     history always runs (one FFT per partition), so it has valid output at once; an Analog / Hybrid engine is run
//     over the last D input samples first (plus a silent settle of up to ~0.2 s for crossovers below ~45 Hz when
//     coming from the other IIR type; meanwhile the old type stays audible). A switch to or from Digital changes the
//     latency: during the 20 ms fade the old type (old latency) and the new one (new latency) are mixed, after that
//     only the new one plays; the caller reports getLatencySamples (new type) from the moment of the switch.
//     A switch back during a fade reverses it; any other request waits for the fade to end.
//
// Realtime: prepare() allocates; setup() and process() are allocation-free and lock-free. Digital kernels are
// designed inside process() incrementally or from precomputed tables - never with allocation.
// The input buffers may alias band buffers. The first setup() after prepare() / reset() applies its frequencies
// immediately (no glide).
namespace ps
{
class Crossover
{
public:
    Crossover();
    ~Crossover();

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    // numBands 1..kMaxBands, freqs ascending (numBands - 1 values used). Cheap when nothing changed.
    void setup (int numBands, const float* crossoverHz, CrossoverType) noexcept;

    // in[2][n] -> bands[b][ch][n] for b < numBands (the caller owns kMaxBands x 2 buffers of maxBlockSize).
    void process (const float* const* input, float* const* const* bandsOut, int numSamples) noexcept;

    int getLatencySamples (CrossoverType) const noexcept;   // Digital: fixed per sample rate; others 0
    int getNumBands() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
