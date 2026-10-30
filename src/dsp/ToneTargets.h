#pragma once

#include "dsp/Types.h"

// Internal header of the Tone Balance target curves (owner: dsp-tone; not a contract). The public entry point is
// ps::toneTargetDb in ToneBalance.h; this exposes the 7 numbers behind each curve for the tests and for the module's
// precomputed tables. Implementation: ToneTargets.cpp.
namespace ps
{
// One target curve, relative to pink noise (octave density, 1 kHz = 0 dB):
//   sub     dB at 30 Hz            bass   dB at 80 Hz          lowMid  dB at 300 Hz
//   pres    extra dB at 3 kHz      slope  dB / octave above 1 kHz (negative = darker than pink)
//   air     extra dB at 14 kHz     tol    tolerance half-width in dB (250 Hz .. 4 kHz; 1.5 x below 100 Hz and above
//                                         8 kHz, blended linearly in log f in between)
// Anchors (monotone cubic, PCHIP, in log2 f; constant outside 20 Hz .. 20 kHz):
//   20 Hz: sub - 6, 30: sub, 80: bass, 300: lowMid, 1 k: 0, 3 k: pres + slope log2 3, 8 k: 3 slope,
//   14 k: slope log2 14 + air, 20 k: (14 k value) - 6.
// Flat (pink) has all six shape numbers at 0 and is exactly 0 dB everywhere (no roll-off anchors at the extremes).
struct ToneTargetShape
{
    float sub, bass, lowMid, pres, slope, air, tol;
};

const ToneTargetShape& toneTargetShape (ToneTarget) noexcept;
} // namespace ps
