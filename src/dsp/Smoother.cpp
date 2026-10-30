// STUB (contracts): replaced by dsp-smth
#include "dsp/Smoother.h"

#include <algorithm>
#include <cmath>

// Passthrough stub of the Smoother contract (Smoother.h): process() leaves the audio untouched, latency 0, meters at
// rest. The Mel band helper is real.
namespace ps
{
double smootherBandHz (int band, double sampleRate) noexcept
{
    auto mel = [] (double hz) { return 2595.0 * std::log10 (1.0 + hz / 700.0); };
    auto hzOf = [] (double m) { return 700.0 * (std::pow (10.0, m / 2595.0) - 1.0); };
    const double top = std::min (20000.0, 0.475 * (sampleRate > 0.0 ? sampleRate : 48000.0));
    const double lo = mel (20.0), hi = mel (top);
    const double centre = lo + (hi - lo) * ((double) std::clamp (band, 0, kSmootherBands - 1) + 1.0) / (kSmootherBands + 1.0);
    return hzOf (centre);
}

struct Smoother::Impl
{
    SmootherMeters meters;
    double sampleRate = 48000.0;
};

Smoother::Smoother() : impl (std::make_unique<Impl>()) {}
Smoother::~Smoother() = default;

void Smoother::prepare (double sampleRate, int) { impl->sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0; }
void Smoother::reset() noexcept {}
void Smoother::setParams (ParamValues) noexcept {}
void Smoother::setMonitor (const ModuleMonitor&) noexcept {}
void Smoother::process (float* const*, int, const ProcessContext&) noexcept {}
int Smoother::getLatencySamples() const noexcept { return 0; }
const SmootherMeters& Smoother::getMeters() const noexcept { return impl->meters; }
double Smoother::getSampleRate() const noexcept { return impl->sampleRate; }
} // namespace ps
