// STUB (skeleton): replaced by dsp-stereo
#include "dsp/Exciter.h"

namespace ps
{
struct Exciter::Impl
{
    ExciterMeters meters;
};

Exciter::Exciter() : impl (std::make_unique<Impl>()) {}
Exciter::~Exciter() = default;

void Exciter::prepare (double, int) {}
void Exciter::reset() noexcept {}
void Exciter::setParams (ParamValues) noexcept {}
void Exciter::setMonitor (const ModuleMonitor&) noexcept {}
void Exciter::process (float* const*, int, const ProcessContext&) noexcept {}
int Exciter::getLatencySamples() const noexcept { return 0; }
const ExciterMeters& Exciter::getMeters() const noexcept { return impl->meters; }
float Exciter::transferCurve (ExciterMode, float x, float) noexcept { return x; }
} // namespace ps
