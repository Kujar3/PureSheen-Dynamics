// STUB (contracts): replaced by dsp-tone
#include "dsp/ToneBalance.h"

#include <cmath>

// Passthrough stub of the Tone Balance contract (ToneBalance.h): process() leaves the audio untouched, latency 0,
// meters at rest; every target is flat (0 dB, tolerance 2 dB) until ToneTargets.cpp exists.
namespace ps
{
double toneBalanceDisplayHz (int index) noexcept
{
    return 20.0 * std::pow (1000.0, (double) index / (double) (kToneDisplayPoints - 1));
}

double toneTargetDb (ToneTarget, double, double* tolDb) noexcept
{
    if (tolDb != nullptr)
        *tolDb = 2.0;
    return 0.0;
}

struct ToneBalance::Impl
{
    ToneBalanceMeters meters;
};

ToneBalance::ToneBalance() : impl (std::make_unique<Impl>()) {}
ToneBalance::~ToneBalance() = default;

void ToneBalance::prepare (double, int) {}
void ToneBalance::reset() noexcept {}
void ToneBalance::setParams (ParamValues) noexcept {}
void ToneBalance::process (float* const*, int, const ProcessContext&) noexcept {}
int ToneBalance::getLatencySamples() const noexcept { return 0; }
const ToneBalanceMeters& ToneBalance::getMeters() const noexcept { return impl->meters; }
} // namespace ps
