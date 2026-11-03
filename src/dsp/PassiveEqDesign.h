#pragma once

#include "dsp/Filters.h"
#include "dsp/Params.h"

// Passive EQ section designs, shared by the DSP (PassiveEq.cpp, where the coefficients and their constants live) and
// the panel (its per-section curves). Owner: mod-peq. Not a team contract.
namespace ps::peq
{
// The seven passive sections of one channel set, in processing order (PassiveEq.h, 1 - 7).
enum Section { kLowBoost, kLowCut, kHighBoost, kHighCut, kLowMid, kMidCut, kHighMid, kNumSections };

// The matched biquad PassiveEq runs for `section` of the set's values at `sampleRate`: exactly unity (numerator =
// denominator, the section's poles kept) at amount 0. Any thread; allocation-free.
BiquadCoeffs designSection (int section, const PassiveEqSetParams&, double sampleRate) noexcept;

// The section's amount is above 0 (it shapes the response).
bool isSectionActive (int section, const PassiveEqSetParams&) noexcept;

// The frequency the section is designed at (after the clamp below Nyquist; the low cut's corner sits above the low
// boost's).
double sectionHz (int section, const PassiveEqSetParams&, double sampleRate) noexcept;
} // namespace ps::peq
