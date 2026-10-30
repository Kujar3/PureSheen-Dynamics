#pragma once

#include "dsp/Module.h"

#include <complex>
#include <memory>

// Equalizer 1 / Equalizer 2 (reference-suite parity, 8 bands per channel set). Owner: dsp-eq. Implementation: Equalizer.cpp,
// EqDesign.cpp.
//
// Analog mode : minimum-phase cascade of matched biquads per band, 0 latency, coefficient changes smoothed.
// Digital mode: one linear-phase FIR per channel set built from the summed band response, run by ps::Convolver.
//               Per band "Phase" blends that band from linear (0 %) to its minimum-phase response (100 %):
//                 H(w) = prod_b |H_b(w)| * exp (j * p_b * arg H_b,min(w))   then  * exp (-j w D), IFFT, window.
//               The kernel is designed on the EQ's own worker thread (std::thread owned by the module, started in
//               prepare, polling an atomic "dirty" flag every 5 ms; never touched by the audio thread) and swapped in
//               click-free by the convolver. Latency = D + convolver partition, constant for a given FIR size and
//               sample rate, so moving nodes never changes the plug-in latency.
// Surgical: Digital only; in Analog mode it is rendered as Bell.
// Brickwall HP / LP: Analog = 8th-order elliptic-like steep IIR (cascade, ~ 96 dB/oct), Digital = FIR brickwall.
// Soft saturation: analog-style soft clip on the boosted part of the signal (applied after the EQ to the difference
//   y - x when it adds energy; never on cuts), level-transparent below -6 dBFS.
// Amount: scales every band's gain (0..200 %), scaled gain clamped to -30..+15 dB (reference-suite parity).
// Audition (ModuleMonitor::auditionOn): output = input band-passed at auditionHz / auditionQ (pre-EQ), on the chosen
//   channel set, used by the UI's Alt-drag.
namespace ps
{
// Frequency response of one band as configured (gain already scaled by amount). Used by the UI to draw the curve and
// by the Digital-mode designer. `minimumPhase` returns the band's minimum-phase (Analog) response.
std::complex<double> eqBandResponse (const EqBandParams&, EqFilterMode, double freqHz, double sampleRate,
                                     float amountPercent) noexcept;

// Default shape for a node created at freqHz (reference-suite parity): < 100 Hz Baxandall low shelf, 100 Hz..8 kHz
// Proportional Q, > 8 kHz Baxandall high shelf; falls back to ProportionalQ if that Baxandall type is already used by
// another enabled band of the same channel set.
EqShape defaultShapeForNewNode (const EqBandParams* bandsOfSet, double freqHz) noexcept;

class Equalizer final : public Module
{
public:
    explicit Equalizer (ModuleType eq1or2);
    ~Equalizer() override;

    ModuleType getType() const noexcept override;
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;

    // For the UI: true while Digital mode is still installing a new kernel (the curve shows the target already).
    bool isDesignPending() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
