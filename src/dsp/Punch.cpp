// STUB (contracts): replaced by dsp-punch
#include "dsp/Punch.h"

#include <algorithm>
#include <cmath>

// Passthrough stub of the Punch contract (Punch.h): process() leaves the audio untouched, latency 0, meters at rest.
// The envelope-time helper is real.
namespace ps
{
double punchEnvelopeMs (const PunchParams& p, double bpm) noexcept
{
    double ms = (double) p.envelopeMs;
    if (p.sync)
    {
        const double tempo = std::isfinite (bpm) && bpm > 0.0 ? bpm : 120.0;
        const int note = std::clamp (p.syncNote, 0, kPunchSyncNotes - 1);
        ms = kPunchSyncFractions[(size_t) note] * 4.0 * 60000.0 / tempo;
    }
    return std::clamp (std::isfinite (ms) ? ms : 100.0, 10.0, 1000.0);
}

struct Punch::Impl
{
    PunchMeters meters;
};

Punch::Punch() : impl (std::make_unique<Impl>()) {}
Punch::~Punch() = default;

void Punch::prepare (double, int) {}
void Punch::reset() noexcept {}
void Punch::setParams (ParamValues) noexcept {}
void Punch::setMonitor (const ModuleMonitor&) noexcept {}
void Punch::process (float* const*, int, const ProcessContext&) noexcept {}
int Punch::getLatencySamples() const noexcept { return 0; }
const PunchMeters& Punch::getMeters() const noexcept { return impl->meters; }
} // namespace ps
