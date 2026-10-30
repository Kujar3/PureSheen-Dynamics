// STUB (contracts): replaced by dsp-meq
#include "dsp/MatchAnalysis.h"

#include <algorithm>
#include <cmath>

// Stub of the Match EQ analysis contract (MatchAnalysis.h): the grid helpers are real, the analyser collects nothing
// (its spectrum stays invalid) and the matched curve is flat (0 dB everywhere).
namespace ps
{
double matchGridHz (int index) noexcept
{
    return 10.0 * std::pow (2.0, (double) index / 48.0);
}

MatchChannel matchChannelFor (ChannelMode mode, int channelSet) noexcept
{
    switch (mode)
    {
        case ChannelMode::MidSide:   return channelSet == 1 ? MatchChannel::Side : MatchChannel::Mid;
        case ChannelMode::LeftRight: return channelSet == 1 ? MatchChannel::Right : MatchChannel::Left;
        case ChannelMode::Stereo:
        case ChannelMode::TransientSustain:
        default: break;
    }
    return MatchChannel::StereoMean;
}

float MatchSpectrum::valueDb (MatchChannel ch, int index) const noexcept
{
    if (index < 0 || index >= kMatchPoints)
        return kMatchNoValue;
    if (ch != MatchChannel::StereoMean)
        return psdDb[(int) ch][(size_t) index];

    const float l = psdDb[0][(size_t) index], r = psdDb[1][(size_t) index];
    if (l <= kMatchNoValue || r <= kMatchNoValue)
        return kMatchNoValue;
    const double p = 0.5 * (std::pow (10.0, l / 10.0) + std::pow (10.0, r / 10.0));
    return (float) (10.0 * std::log10 (std::max (p, 1.0e-30)));
}

double matchSmoothingOctaves (float smoothPercent) noexcept
{
    const double s = std::clamp ((double) smoothPercent, 0.0, 100.0) / 100.0;
    return (1.0 / 48.0) * std::pow (96.0, s);
}

struct MatchAnalyser::Impl
{
};

MatchAnalyser::MatchAnalyser() : impl (std::make_unique<Impl>()) {}
MatchAnalyser::~MatchAnalyser() = default;
void MatchAnalyser::prepare (double) {}
void MatchAnalyser::reset() noexcept {}
void MatchAnalyser::push (const float*, const float*, int) noexcept {}
double MatchAnalyser::getSeconds() const noexcept { return 0.0; }

void MatchAnalyser::spectrum (MatchSpectrum& out) const noexcept
{
    out.valid = false;
    out.seconds = 0.0;
    for (auto& ch : out.psdDb)
        ch.fill (kMatchNoValue);
}

void computeMatchCurve (const MatchSpectrum&, const MatchSpectrum&, MatchChannel, const MatchEqSetParams&,
                        float* outDb) noexcept
{
    std::fill (outDb, outDb + kMatchPoints, 0.0f);
}
} // namespace ps
