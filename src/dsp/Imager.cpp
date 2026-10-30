// STUB (skeleton): replaced by dsp-stereo
#include "dsp/Imager.h"

namespace ps
{
struct Imager::Impl
{
    ImagerMeters meters;
};

Imager::Imager() : impl (std::make_unique<Impl>()) {}
Imager::~Imager() = default;

void Imager::prepare (double, int) {}
void Imager::reset() noexcept {}
void Imager::setParams (ParamValues) noexcept {}
void Imager::setMonitor (const ModuleMonitor&) noexcept {}
void Imager::process (float* const*, int, const ProcessContext&) noexcept {}
int Imager::getLatencySamples() const noexcept { return 0; }
const ImagerMeters& Imager::getMeters() const noexcept { return impl->meters; }
} // namespace ps
