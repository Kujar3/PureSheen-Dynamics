// STUB (contracts): replaced by mod-peq
#include "dsp/PassiveEq.h"

#include "dsp/Envelope.h"

// Passthrough stub of the Passive EQ contract (PassiveEq.h): process() delays the audio by kPassiveEqLatency samples
// (the contract's constant latency), the response query is flat.
namespace ps
{
struct PassiveEq::Impl
{
    DelayLine delay[2];
};

PassiveEq::PassiveEq() : impl (std::make_unique<Impl>()) {}
PassiveEq::~PassiveEq() = default;

void PassiveEq::prepare (double, int)
{
    for (auto& d : impl->delay)
    {
        d.prepare (kPassiveEqLatency);
        d.setDelay (kPassiveEqLatency);
    }
}

void PassiveEq::reset() noexcept
{
    for (auto& d : impl->delay)
        d.reset();
}

void PassiveEq::setParams (ParamValues) noexcept {}

void PassiveEq::process (float* const* channels, int numSamples, const ProcessContext&) noexcept
{
    for (int c = 0; c < 2; ++c)
        impl->delay[c].process (channels[c], numSamples);
}

int PassiveEq::getLatencySamples() const noexcept { return kPassiveEqLatency; }
double PassiveEq::getMagnitudeDb (int, double) const noexcept { return 0.0; }
} // namespace ps
