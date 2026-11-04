#pragma once

#include "dsp/Filters.h"
#include "dsp/Params.h"

// Passive EQ section designs, shared by the DSP (PassiveEq.cpp, where the coefficients and their constants live) and
// the panel (its per-section curves). Owner: mod-peq. Not a team contract.
namespace ps::peq
{
// The seven passive sections of one channel set, in processing order (PassiveEq.h, 1 - 7).
enum Section { kLowBoost, kLowCut, kHighBoost, kHighCut, kLowMid, kMidCut, kHighMid, kNumSections };

// The biquad PassiveEq runs for `section` of the set's values at `sampleRate`: exactly unity (numerator =
// denominator, the section's poles kept) at amount 0 (section 4: when the high cut and the high boost are both 0).
// Any thread; allocation-free.
BiquadCoeffs designSection (int section, const PassiveEqSetParams&, double sampleRate) noexcept;

// The section's own amount is above 0. Section 4 also carries the high boost's upper skirt, so it can shape the
// response while its own amount is 0: sum every section's design for the total curve.
bool isSectionActive (int section, const PassiveEqSetParams&) noexcept;

// Where the section acts, for the panel's markers: the shelves' half-gain centre (geometric mean of pole and zero),
// the peaks' centre. Low shelf centres use the audio's bilinear frequency map. Clamped to 0.49 x the sample rate.
double sectionHz (int section, const PassiveEqSetParams&, double sampleRate) noexcept;
} // namespace ps::peq
