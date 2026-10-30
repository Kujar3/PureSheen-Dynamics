// STUB (contracts): replaced by dsp-defn
#include "dsp/Definition.h"

#include <cmath>

// Passthrough stub of the Definition contract (Definition.h): process() leaves the audio untouched, latency 0,
// meters at rest.
namespace ps
{
double definitionDisplayHz (int index) noexcept
{
    return 250.0 * std::pow (80.0, (double) index / (double) (kDefinitionDisplayPoints - 1));
}

struct Definition::Impl
{
    DefinitionMeters meters;
};

Definition::Definition() : impl (std::make_unique<Impl>()) {}
Definition::~Definition() = default;

void Definition::prepare (double, int) {}
void Definition::reset() noexcept {}
void Definition::setParams (ParamValues) noexcept {}
void Definition::setMonitor (const ModuleMonitor&) noexcept {}
void Definition::process (float* const*, int, const ProcessContext&) noexcept {}
int Definition::getLatencySamples() const noexcept { return 0; }
const DefinitionMeters& Definition::getMeters() const noexcept { return impl->meters; }
} // namespace ps
