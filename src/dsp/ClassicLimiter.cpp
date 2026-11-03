// STUB (contracts): replaced by mod-clim
#include "dsp/ClassicLimiter.h"

#include "dsp/Envelope.h"
#include "dsp/MaximizerStages.h"

#include <cmath>
#include <cstring>

// Passthrough stub of the Classic Limiter contract (ClassicLimiter.h): process() delays the audio by the contract's
// latency D (latencyFor is the real rule), the Delta output is silent, meters at rest, the history FIFO stays empty.
namespace ps
{
int ClassicLimiter::latencyFor (double sampleRate) noexcept
{
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int lookahead = (int) std::lround (1.5e-3 * fs);
    const int guardAttack = (int) std::lround (0.5e-3 * fs);
    return 3 + (lookahead + maxi::kMidDetectorDelay) + (guardAttack + maxi::kTpDetectorDelay + 6);
}

struct ClassicLimiter::Impl
{
    ClassicLimiterMeters meters;
    StereoSampleFifo history;
    DelayLine delay[2];
    int latency = latencyFor (48000.0);
};

ClassicLimiter::ClassicLimiter() : impl (std::make_unique<Impl>()) {}
ClassicLimiter::~ClassicLimiter() = default;

void ClassicLimiter::prepare (double sampleRate, int)
{
    impl->latency = latencyFor (sampleRate);
    for (auto& d : impl->delay)
    {
        d.prepare (impl->latency);
        d.setDelay (impl->latency);
    }
    impl->history.prepare (1 << 14);
}

void ClassicLimiter::reset() noexcept
{
    for (auto& d : impl->delay)
        d.reset();
}

void ClassicLimiter::setParams (ParamValues) noexcept {}

void ClassicLimiter::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        impl->delay[c].process (channels[c], numSamples);
        if (ctx.deltaOut != nullptr)
            std::memset (ctx.deltaOut[c], 0, sizeof (float) * (size_t) numSamples);
    }
}

int ClassicLimiter::getLatencySamples() const noexcept { return impl->latency; }
const ClassicLimiterMeters& ClassicLimiter::getMeters() const noexcept { return impl->meters; }
ClassicLimiterMeters& ClassicLimiter::getMeters() noexcept { return impl->meters; }
StereoSampleFifo& ClassicLimiter::getHistoryFifo() noexcept { return impl->history; }
} // namespace ps
