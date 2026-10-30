// STUB (skeleton): replaced by dsp-eq
#include "dsp/Equalizer.h"

namespace ps
{
std::complex<double> eqBandResponse (const EqBandParams&, EqFilterMode, double, double, float) noexcept
{
    return { 1.0, 0.0 };
}

EqShape defaultShapeForNewNode (const EqBandParams*, double) noexcept
{
    return EqShape::ProportionalQ;
}

struct Equalizer::Impl
{
    ModuleType type = ModuleType::Equalizer1;
};

Equalizer::Equalizer (ModuleType eq1or2) : impl (std::make_unique<Impl>())
{
    impl->type = eq1or2;
}

Equalizer::~Equalizer() = default;

ModuleType Equalizer::getType() const noexcept { return impl->type; }
void Equalizer::prepare (double, int) {}
void Equalizer::reset() noexcept {}
void Equalizer::setParams (ParamValues) noexcept {}
void Equalizer::setMonitor (const ModuleMonitor&) noexcept {}
void Equalizer::process (float* const*, int, const ProcessContext&) noexcept {}
int Equalizer::getLatencySamples() const noexcept { return 0; }
bool Equalizer::isDesignPending() const noexcept { return false; }
} // namespace ps
