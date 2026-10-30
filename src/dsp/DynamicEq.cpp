// STUB (contracts): replaced by dsp-deq
#include "dsp/DynamicEq.h"

#include "dsp/Equalizer.h"

#include <algorithm>

// Passthrough stub of the Dynamic EQ contract (DynamicEq.h): process() leaves the audio untouched, latency 0, meters
// at rest. The pure helpers (band response, rest / triggered gain) are already real so the UI can be built on them.
namespace ps
{
std::complex<double> dynEqBandResponse (const DynamicEqBandParams& b, double bandGainDb, double freqHz,
                                        double sampleRate) noexcept
{
    EqBandParams e;
    e.on = true;
    e.shape = dynEqToEqShape (b.shape);
    e.freqHz = b.freqHz;
    e.gainDb = (float) bandGainDb;
    e.q = b.q;
    return eqBandResponse (e, EqFilterMode::Analog, freqHz, sampleRate, 100.0f);
}

namespace
{
double clampGain (double db) noexcept { return std::clamp (db, -30.0, 15.0); }

// true: the band rests at 0 dB dynamic gain and moves to G when triggered
bool movesTowardGain (const DynamicEqBandParams& b) noexcept
{
    const bool cut = b.gainDb < 0.0f;
    return b.direction == DynEqDirection::Down ? cut : ! cut;
}
} // namespace

double dynEqRestGainDb (const DynamicEqBandParams& b) noexcept
{
    if (! b.dynamic)
        return clampGain ((double) b.offsetDb + (double) b.gainDb);
    return clampGain ((double) b.offsetDb + (movesTowardGain (b) ? 0.0 : (double) b.gainDb));
}

double dynEqTriggeredGainDb (const DynamicEqBandParams& b) noexcept
{
    if (! b.dynamic)
        return clampGain ((double) b.offsetDb + (double) b.gainDb);
    return clampGain ((double) b.offsetDb + (movesTowardGain (b) ? (double) b.gainDb : 0.0));
}

struct DynamicEq::Impl
{
    DynamicEqMeters meters;
};

DynamicEq::DynamicEq() : impl (std::make_unique<Impl>()) {}
DynamicEq::~DynamicEq() = default;

void DynamicEq::prepare (double, int) {}
void DynamicEq::reset() noexcept {}
void DynamicEq::setParams (ParamValues) noexcept {}
void DynamicEq::setMonitor (const ModuleMonitor&) noexcept {}
void DynamicEq::process (float* const*, int, const ProcessContext&) noexcept {}
int DynamicEq::getLatencySamples() const noexcept { return 0; }
const DynamicEqMeters& DynamicEq::getMeters() const noexcept { return impl->meters; }
bool DynamicEq::isDesignPending() const noexcept { return false; }
} // namespace ps
