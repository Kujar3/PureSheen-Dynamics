// STUB (contracts): replaced by dsp-bass
#include "dsp/BassShaper.h"

#include <cmath>

// Passthrough stub of the Bass Shaper contract (BassShaper.h): process() leaves the audio untouched, latency 0,
// meters at rest, the scope FIFOs stay empty.
namespace ps
{
double bassShaperBalanceTargetDb (double cutoffHz) noexcept
{
    return -4.0 + 3.0 * std::log2 ((cutoffHz > 1.0 ? cutoffHz : 120.0) / 120.0);
}

struct BassShaper::Impl
{
    BassShaperMeters meters;
    StereoSampleFifo scope, mask;
};

BassShaper::BassShaper() : impl (std::make_unique<Impl>()) {}
BassShaper::~BassShaper() = default;

void BassShaper::prepare (double, int)
{
    impl->scope.prepare (1 << 14);
    impl->mask.prepare (1 << 14);
}

void BassShaper::reset() noexcept {}
void BassShaper::setParams (ParamValues) noexcept {}
void BassShaper::process (float* const*, int, const ProcessContext&) noexcept {}
int BassShaper::getLatencySamples() const noexcept { return 0; }
const BassShaperMeters& BassShaper::getMeters() const noexcept { return impl->meters; }
StereoSampleFifo& BassShaper::getScopeFifo() noexcept { return impl->scope; }
StereoSampleFifo& BassShaper::getMaskFifo() noexcept { return impl->mask; }
} // namespace ps
