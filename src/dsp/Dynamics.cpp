// STUB (skeleton): replaced by dsp-dynamics
#include "dsp/Dynamics.h"

namespace ps
{
struct Dynamics::Impl
{
    DynamicsMeters meters;
};

Dynamics::Dynamics() : impl (std::make_unique<Impl>()) {}
Dynamics::~Dynamics() = default;

void Dynamics::prepare (double, int) {}
void Dynamics::reset() noexcept {}
void Dynamics::setParams (ParamValues) noexcept {}
void Dynamics::setMonitor (const ModuleMonitor&) noexcept {}
void Dynamics::process (float* const*, int, const ProcessContext&) noexcept {}
int Dynamics::getLatencySamples() const noexcept { return 0; }
const DynamicsMeters& Dynamics::getMeters() const noexcept { return impl->meters; }
} // namespace ps
