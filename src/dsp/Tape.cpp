// STUB (contracts): replaced by mod-tape
#include "dsp/Tape.h"

#include "dsp/Envelope.h"

// Passthrough stub of the Tape contract (Tape.h): process() delays the audio by kTapeLatency samples (the contract's
// constant latency), meters at rest, the response view is flat.
namespace ps
{
struct Tape::Impl
{
    TapeMeters meters;
    DelayLine delay[2];
};

Tape::Tape() : impl (std::make_unique<Impl>()) {}
Tape::~Tape() = default;

void Tape::prepare (double, int)
{
    for (auto& d : impl->delay)
    {
        d.prepare (kTapeLatency);
        d.setDelay (kTapeLatency);
    }
}

void Tape::reset() noexcept
{
    for (auto& d : impl->delay)
        d.reset();
}

void Tape::setParams (ParamValues) noexcept {}

void Tape::process (float* const* channels, int numSamples, const ProcessContext&) noexcept
{
    for (int c = 0; c < 2; ++c)
        impl->delay[c].process (channels[c], numSamples);
}

int Tape::getLatencySamples() const noexcept { return kTapeLatency; }
const TapeMeters& Tape::getMeters() const noexcept { return impl->meters; }
double Tape::responseDb (const TapeParams&, double) noexcept { return 0.0; }
} // namespace ps
