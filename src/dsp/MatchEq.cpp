// STUB (contracts): replaced by dsp-meq
#include "dsp/MatchEq.h"

#include <atomic>

// Passthrough stub of the Match EQ contract (MatchEq.h): process() leaves the audio untouched, latency 0. Spectra are
// stored and returned (setSpectrum / getSpectrum / clearSpectrum work, so the plugin and UI layers can be built on
// them); a capture records nothing.
namespace ps
{
struct MatchEq::Impl
{
    MatchEqMeters meters;
    MatchSpectrum spectra[2];
    std::atomic<bool> capturing { false };
    std::atomic<int> captureRole { 0 };
    std::atomic<unsigned> generation { 0 };
};

MatchEq::MatchEq() : impl (std::make_unique<Impl>()) {}
MatchEq::~MatchEq() = default;

void MatchEq::prepare (double, int) {}
void MatchEq::reset() noexcept {}
void MatchEq::setParams (ParamValues) noexcept {}
void MatchEq::setMonitor (const ModuleMonitor&) noexcept {}
void MatchEq::process (float* const*, int, const ProcessContext&) noexcept {}
int MatchEq::getLatencySamples() const noexcept { return 0; }
const MatchEqMeters& MatchEq::getMeters() const noexcept { return impl->meters; }
bool MatchEq::isDesignPending() const noexcept { return false; }

void MatchEq::startCapture (MatchRole role)
{
    impl->captureRole.store ((int) role);
    impl->capturing.store (true);
}

void MatchEq::stopCapture()
{
    if (impl->capturing.exchange (false))
        ++impl->generation;
}

bool MatchEq::isCapturing (MatchRole* roleOut) const noexcept
{
    if (roleOut != nullptr)
        *roleOut = (MatchRole) impl->captureRole.load();
    return impl->capturing.load();
}

double MatchEq::getCaptureSeconds() const noexcept { return 0.0; }
int MatchEq::getCaptureGaps() const noexcept { return 0; }
bool MatchEq::getRunningSpectrum (MatchSpectrum&) const { return false; }

MatchSpectrum MatchEq::getSpectrum (MatchRole role) const { return impl->spectra[role == MatchRole::Current ? 1 : 0]; }

void MatchEq::setSpectrum (MatchRole role, const MatchSpectrum& s)
{
    impl->spectra[role == MatchRole::Current ? 1 : 0] = s;
    ++impl->generation;
}

void MatchEq::clearSpectrum (MatchRole role)
{
    impl->spectra[role == MatchRole::Current ? 1 : 0] = MatchSpectrum {};
    ++impl->generation;
}

unsigned MatchEq::getSpectrumGeneration() const noexcept { return impl->generation.load(); }
} // namespace ps
