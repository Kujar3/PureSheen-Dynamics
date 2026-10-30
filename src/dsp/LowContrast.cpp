// STUB (contracts): replaced by dsp-lowc
#include "dsp/LowContrast.h"

#include <cmath>

// Passthrough stub of the Low Contrast contract (LowContrast.h): process() leaves the audio untouched, latency 0,
// meters at rest.
namespace ps
{
double lowContrastDisplayHz (int index) noexcept
{
    return 16.0 * std::pow (25.0, (double) index / (double) (LowContrastMeters::kPoints - 1));
}

struct LowContrast::Impl
{
    LowContrastMeters meters;
};

LowContrast::LowContrast() : impl (std::make_unique<Impl>()) {}
LowContrast::~LowContrast() = default;

void LowContrast::prepare (double, int) {}
void LowContrast::reset() noexcept {}
void LowContrast::setParams (ParamValues) noexcept {}
void LowContrast::setMonitor (const ModuleMonitor&) noexcept {}
void LowContrast::process (float* const*, int, const ProcessContext&) noexcept {}
int LowContrast::getLatencySamples() const noexcept { return 0; }
const LowContrastMeters& LowContrast::getMeters() const noexcept { return impl->meters; }
} // namespace ps
