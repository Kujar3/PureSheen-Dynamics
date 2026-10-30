#pragma once

#include "dsp/MatchAnalysis.h"
#include "dsp/Module.h"

#include <memory>

// Match EQ: a linear- or minimum-phase FIR that moves the music's long-term spectrum toward a reference spectrum
// (channel modes Stereo, M/S, L/R, T/S). Owner: dsp-meq. Implementation: MatchEq.cpp (+ MatchAnalysis.cpp, and e.g.
// CurveFir.h/.cpp for the curve -> kernel designer). Tests: tests/MatchEqTests.cpp, prefix "meq.".
//
// Two spectra per instance (MatchAnalysis.h): the Reference (what the music should sound like) and the Current (the
// music itself). The matched curve per channel set = computeMatchCurve (reference, current, matchChannelFor (mode,
// set), set params). Kernel per channel set, designed on the module's own worker thread (std::thread started in
// prepare, joined in the destructor, polling every 5 ms, never blocking the audio thread):
//   |G (f)| = 10^(curve / 20) sampled on a 2N grid (log-f interpolation of the grid points)
//   Linear : H = |G| e^{-j w D}, inverse FFT, Kaiser (beta 9) window centred at D = N / 2, residual r = h - delta (D)
//   Minimum: real cepstrum of ln |G| on the 2N grid, folded, exp, inverse FFT, right half-Kaiser over N taps,
//            residual r = h - delta (0)
//   flat curve -> r = 0 exactly
// run by ps::Convolver (partition P = N / 16, click-free kernel swap), direct path = exact delay line, so neutral is
// bit-exact (delayed). Stereo / T/S: set A kernel on both channels; M/S, L/R: set A on ch0, set B on ch1.
// N = firSize (4096 .. 32768 at 44.1 / 48 kHz, x2 at 88.2 / 96, x4 above). Latency: Linear D + P (16384 at 48 kHz:
// 9216 samples), Minimum P; it never depends on whether a curve exists (capturing / clearing never moves the host's
// delay compensation). Phase / FIR size switches fade out 4 ms, reconfigure, fade in. Parameter and spectrum changes
// only swap kernels (no fade).
// Neutral: no spectra, either spectrum invalid, amount 0, or reference == current -> output == input delayed.
//
// Capture (live): startCapture (role) makes the audio thread push the module INPUT (before processing, so the
// existing curve never colours a capture; in M/S the pushed signal is decoded back to L/R) into a module-owned
// StereoSampleFifo (capacity 2^17) while capturing. The worker drains it into a MatchAnalyser; dropped samples count
// as gaps. The running spectrum is published every 250 ms; stopCapture() publishes the final one as the role's
// spectrum ("capture until Stop", a plain average). In T/S each instance captures its own part.
// Spectra from elsewhere (state, presets, an audio file analysed by the processor's loader thread, phase 3 Track
// Referencing) come in through setSpectrum. All spectrum calls are message thread; isCapturing / isDesignPending /
// getCaptureSeconds / getCaptureGaps are atomics (any thread).
// Monitors: audition (auditionOn / auditionHz / auditionQ / auditionChannelSet) = band-pass of the input as in the
// Equalizer; no band solo. Delta: the slot's.
// T/S: the twin (getParamSet() == 1) reads set B for both channels and keeps its own spectra (Module.h).
namespace ps
{
struct MatchEqMeters
{
    AtomicFloat curveMaxDb[kNumChannelSets];   // largest boost / cut of the curve that is installed (for the UI)
    AtomicFloat curveMinDb[kNumChannelSets];
};

class MatchEq final : public Module
{
public:
    MatchEq();
    ~MatchEq() override;

    ModuleType getType() const noexcept override { return ModuleType::MatchEq; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void setMonitor (const ModuleMonitor&) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;
    int getLatencySamplesForParams (ParamValues) const noexcept override;

    const MatchEqMeters& getMeters() const noexcept;
    bool isDesignPending() const noexcept;

    //==============================================================================
    // Spectra (message thread)
    void startCapture (MatchRole);                      // restarts the capture of that role (one capture at a time)
    void stopCapture();                                 // the captured average becomes the role's spectrum
    bool isCapturing (MatchRole* roleOut = nullptr) const noexcept;
    double getCaptureSeconds() const noexcept;
    int getCaptureGaps() const noexcept;
    bool getRunningSpectrum (MatchSpectrum&) const;     // during a capture: the average so far (false otherwise)

    MatchSpectrum getSpectrum (MatchRole) const;
    void setSpectrum (MatchRole, const MatchSpectrum&);
    void clearSpectrum (MatchRole);
    // Increments whenever a spectrum changes (capture stop, set, clear), so the UI / processor can react (undo, repaint).
    unsigned getSpectrumGeneration() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
