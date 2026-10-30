// STUB (skeleton): replaced by dsp-maximizer
#include "dsp/Maximizer.h"

namespace ps
{
struct Maximizer::Impl
{
    MaximizerMeters meters;
};

Maximizer::Maximizer() : impl (std::make_unique<Impl>()) {}
Maximizer::~Maximizer() = default;

void Maximizer::prepare (double, int) {}
void Maximizer::reset() noexcept {}
void Maximizer::setParams (ParamValues) noexcept {}
void Maximizer::process (float* const*, int, const ProcessContext&) noexcept {}
int Maximizer::getLatencySamples() const noexcept { return 0; }
const MaximizerMeters& Maximizer::getMeters() const noexcept { return impl->meters; }

void Maximizer::startLearn() noexcept {}
void Maximizer::cancelLearn() noexcept {}
bool Maximizer::isLearning() const noexcept { return false; }
float Maximizer::getLearnProgress() const noexcept { return 0.0f; }
bool Maximizer::takeLearnedGain (float&) noexcept { return false; }
} // namespace ps
