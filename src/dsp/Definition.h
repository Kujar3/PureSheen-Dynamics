#pragma once

#include "dsp/Module.h"

#include <memory>

// Definition: adaptive spectral processing that moves the average response toward pink noise plus a tilt, above
// 300 Hz (channel modes Stereo, M/S, T/S). Owner: dsp-defn. Implementation: Definition.cpp (on ps::SpectralEngine,
// base size 1024). Tests: tests/DefinitionTests.cpp, prefix "defn.".
//
// Engine: residual STFT, N = 1024 at 44.1 / 48 kHz (x2 / x4 above), hop N / 4 (5.3 ms at 48 kHz); latency N, constant.
// Band grid: 1/6 octave from 250 Hz to min (20 kHz, 0.45 fs), at least 2 bins (about 32 bands). Per hop, per lane:
//   1. detector L (b): octave-density band level after a 10 ms power one-pole per band
//   2. target line (b) = tilt x log2 (f_b / 1 kHz); offset o = octave-width weighted mean of L - line over the bands
//      inside the region (gated bands excluded); D = line + o - L
//   3. desired gain g* = A x clamp (0.6 D, -L / A, +L / A), A = amount / 100, L = 6 x A^0.91 dB (+6 at Amount 100,
//      +1.7 at 25, as measured), Gaussian smoothing over log2 f with sigma 1/6 octave
//   4. loudness neutral: g* -= the K-weighted power-weighted mean dB change over the region (our choice: Definition
//      redistributes energy instead of mostly brightening, so A/B comparisons stay fair)
//   5. region mask: 1 inside [low, high], raised-cosine to 0 over 1/3 octave outside, exactly 0 below
//      300 Hz x 2^(-1/3)
//   6. ballistics per band (dB, per-hop coefficients): attack tau while |g*| grows with the same sign, release tau
//      toward 0 or on a sign change; boosts slew-limited to +1 dB per hop; release >= 2 hops
//   7. band -> bin gains, residual synthesis
// Stereo / T/S: one curve for both channels (power-sum detector). M/S: Mid with set A, Side with set B.
// Neutral: Amount 0 (the default) -> exact null against the input delayed by N.
// Monitors: soloRegion = the input inside the region only (ISTFT of mask x X, no dry path, same latency).
// T/S: the twin (getParamSet() == 1) reads set B for both channels (Module.h).
namespace ps
{
// Largest boost / cut (dB) at amount = Amount / 100 (0..1): 6 x amount^0.91 (+6 at 100, +1.7 at 25).
double definitionMaxGainDb (double amount) noexcept;

inline constexpr int kDefinitionDisplayPoints = 64;
double definitionDisplayHz (int index) noexcept;     // 64 log-spaced points 250 Hz .. 20 kHz

struct DefinitionMeters
{
    AtomicFloat gainDb[kNumChannelSets][kDefinitionDisplayPoints];    // current correction at the display points
    AtomicFloat levelDb[kNumChannelSets][kDefinitionDisplayPoints];   // detector level (octave density)
    AtomicFloat maxBoostDb[kNumChannelSets], maxCutDb[kNumChannelSets];
};                                                                    // Stereo / T/S: set B mirrors set A

class Definition final : public Module
{
public:
    Definition();
    ~Definition() override;

    ModuleType getType() const noexcept override { return ModuleType::Definition; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const DefinitionMeters& getMeters() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
