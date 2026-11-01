#pragma once

#include "dsp/Module.h"

#include <memory>

// Multiband harmonic Exciter (reference-suite parity, seven saturation models). Owner: dsp-stereo. Implementation: Exciter.cpp.
//   - per band: wet = HP14 (curve_mode,drive (band)), out = dry * (1 - mix) + wet * mix; no level compensation
//   - models (static curves measured from the reference at drive 20 / 50 / 100 %, see Exciter.cpp):
//       Analog     asymmetric soft curve: mostly 2nd harmonic, clips at full drive
//       Retro      symmetric hard-ish clipper (odd harmonics), the level drops on loud input
//       Tape       symmetric soft clip (odd harmonics)
//       Tube       nearly clean at low drive, a hard clip (square-like) at full drive
//       Warm       even harmonics only (rectifier-like)
//       Triode     asymmetric, adds level at low drive, clips one half at full drive
//       DualTriode clean on quiet input, then a strong asymmetric clip
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
