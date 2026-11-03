// STUB (contracts): replaced by mod-ccmp
#include "dsp/ClassicComp.h"

// Passthrough stub of the Classic Compressor contract (ClassicComp.h): process() leaves the audio untouched, latency 0,
// meters at rest; the panel helpers return a flat detector and a 1:1 curve.
namespace ps
{
struct ClassicComp::Impl
{
    ClassicCompMeters meters;
};

ClassicComp::ClassicComp() : impl (std::make_unique<Impl>()) {}
ClassicComp::~ClassicComp() = default;

void ClassicComp::prepare (double, int) {}
void ClassicComp::reset() noexcept {}
void ClassicComp::setParams (ParamValues) noexcept {}
void ClassicComp::setMonitor (const ModuleMonitor&) noexcept {}
void ClassicComp::process (float* const*, int, const ProcessContext&) noexcept {}
int ClassicComp::getLatencySamples() const noexcept { return 0; }
const ClassicCompMeters& ClassicComp::getMeters() const noexcept { return impl->meters; }
double ClassicComp::detectorResponseDb (const ClassicCompSetParams&, double) noexcept { return 0.0; }
double ClassicComp::staticCurveDb (const ClassicCompSetParams&, double inputDb) noexcept { return inputDb; }
} // namespace ps
