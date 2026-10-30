#pragma once

#include "dsp/Module.h"

#include <memory>

// Multiband harmonic Exciter (reference parity, seven saturation models). Owner: dsp-stereo. Implementation: Exciter.cpp.
//   - per band: wet = saturate_mode (band * driveGain), level-compensated so drive changes harmonics more than level;
//     out = dry * (1 - mix) + wet * mix
//   - models (our own static + dynamic curves):
//       Analog     odd harmonics, gritty (asymmetric-free tanh-like with hard-ish knee)
//       Retro      transistor-style odd harmonics with slow decay (level-dependent bias, slow envelope)
//       Tape       brighter odd harmonics (pre-emphasis -> soft clip -> de-emphasis, mild hysteresis)
//       Tube       asymmetric (even + odd), emphasises attacks (fast bias envelope)
//       Warm       even harmonics only (full-wave rectifier-style mix), quick decay
//       Triode     single triode stage: subtle asymmetric overdrive
//       DualTriode two stages (push-pull): stronger, warmer, more even harmonics
//   - oversampling: ps::Oversampler 4x around the nonlinear stage (linear phase), latency reported
//   - post filter: high shelf on the wet signal only (postFilterHz / postFilterGainDb)
//   - Stereo or M/S (sets A / B)
namespace ps
{
struct ExciterMeters
{
    AtomicFloat harmonicsDb[kNumChannelSets][kMaxBands];   // RMS of (wet - dry) per band (saturation histogram)
};

class Exciter final : public Module
{
public:
    Exciter();
    ~Exciter() override;

    ModuleType getType() const noexcept override { return ModuleType::Exciter; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const ExciterMeters& getMeters() const noexcept;

    // Static transfer curve of a model (for the UI's curve thumbnail): output for input x at the given drive.
    static float transferCurve (ExciterMode, float x, float drivePercent) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
