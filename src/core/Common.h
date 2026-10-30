#pragma once

#include <array>
#include <cmath>

// Shared constants for the PureSheen core (pure C++17, no JUCE).
namespace ps
{
// Sentinel for "no measurement yet" (silence, not enough audio). Every dB / LUFS getter returns this instead of
// -infinity so arithmetic never produces NaN. Test with hasValue().
inline constexpr float kNoValue = -200.0f;

inline bool hasValue (float dbOrLufs) noexcept { return dbOrLufs > -150.0f; }

inline float powerToDb (double power) noexcept
{
    return power > 1.0e-20 ? (float) (10.0 * std::log10 (power)) : kNoValue;
}

inline float gainToDb (double gain) noexcept
{
    return gain > 1.0e-10 ? (float) (20.0 * std::log10 (gain)) : kNoValue;
}

inline double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }

} // namespace ps
