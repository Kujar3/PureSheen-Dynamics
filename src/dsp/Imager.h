#pragma once

#include "dsp/Module.h"

#include <memory>

// Multiband stereo Imager (reference parity). Owner: dsp-stereo. Implementation: Imager.cpp.
//   - per band: S *= 1 + width/100 * amount/100 (width -100 = mono), M untouched
//   - Stereoize (bands with width > 0 only, scaled by that band's width):
//       Mode I : Haas-style: a delayed (~ 8..20 ms, frequency-dependent) and filtered copy of M added to S
//       Mode II: decorrelated M (allpass network / short diffusion) added to S, transient-friendly
//     Both mono compatible (the injected component is in S only, so it cancels in L+R)
//   - Recover Sides (bands with width < 0 only): the S energy removed by narrowing is re-introduced into M with
//     recoverSidesGainDb (keeps the level of hard-panned elements when narrowing)
//   - Prevent antiphase: per band, limits |S| so the band's correlation stays >= 0 (smoothed)
//   - Crossover: ps::Crossover (default Analog: 0 latency, compensated tree so every band carries the same phase;
//     Digital = linear phase)
// Meters are computed by the UI from the chain's output FIFO (vectorscope always shows the final output, reference
// parity); the module publishes per-band correlation for its band display.
namespace ps
{
struct ImagerMeters
{
    AtomicFloat correlation[kMaxBands];   // -1..+1 per band of the module's output
    AtomicFloat widthRatio[kMaxBands];    // S / M RMS ratio per band of the output
};

class Imager final : public Module
{
public:
    Imager();
    ~Imager() override;

    ModuleType getType() const noexcept override { return ModuleType::Imager; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    const ImagerMeters& getMeters() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
