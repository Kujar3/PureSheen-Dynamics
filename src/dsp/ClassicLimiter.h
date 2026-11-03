#pragma once

#include "dsp/Module.h"

#include <memory>

// Classic Limiter: a variable-mu feedback tube limiter with three modes, followed by a brickwall stage (Stereo only).
// Owner: mod-clim. Implementation: ClassicLimiter.cpp. Tests: tests/ClassicLimiterTests.cpp, prefix "clim.". Every
// coefficient below named k... lives in ONE constant block at the top of ClassicLimiter.cpp (first-principles values
// until the reference renders are fitted).
//
//   x -> input gain G = ceiling - threshold (dB; lowering the threshold makes it louder, as a maximizer)
//     -> variable-mu stage (stereo-linked, feedback):
//          detector: the stage's own output of the previous sample, max (|L|, |R|) (Analog: through a 60 Hz
//            high-pass, kAnalogDetHpHz, "tighter lows"), smoothed by attack / release one-poles into env (linear)
//          gain element g = 1 / (1 + u^kMuExponent), u = max (0, env - K) / (C - K), C = the ceiling (linear) and
//            K = C x 10^(-kKneeDb / 20) the start of the very soft knee (kKneeDb 10): exactly 1 below the knee, the
//            ratio rising with level above it
//          times from character (0 = fast .. 10 = slow) per mode: kAttackMs[mode][0..10], kReleaseMs[mode][0..10]
//            (tables, interpolated linearly in character); program-dependent release as in the Classic Compressor
//            (ClassicComp.h: the held share h, kHoldTauMs, slow path kSlowReleaseMul x release)
//          mode colour: Analog = clean element (no saturation), the fastest attacks; Tube = tube saturation in the gain
//            element, its drive scaled by the current reduction (|20 log10 g| / kSatFullDb, clamped to 1: exactly
//            linear while nothing is reduced); Modern = kModernBlend x the Tube path + (1 - kModernBlend) x the same
//            gain without saturation (the brickwall below supplies the transparent limiting)
//          Tube and Modern run the saturator 2x oversampled (Oversampler, MinimumPhase, 3 samples); Analog uses an
//            exact 3-sample delay in its place, so every mode has the same latency; a mode change crossfades 20 ms
//     -> brickwall at the ceiling: maxi::LimiterCore (MaximizerStages.h) with lookahead LA = round (1.5 ms x fs),
//          then maxi::Guard with max attack GA = round (0.5 ms x fs): true-peak detection when truePeak is on, sample
//          peak when off (the Guard is always in the path, so the latency does not depend on truePeak). Output never
//          above the ceiling (sample peak, or true peak with truePeak on: the Maximizer's guarantee).
// Latency D = 3 + (LA + maxi::kMidDetectorDelay) + (GA + maxi::kTpDetectorDelay + 6) = latencyFor (sampleRate): 109
// samples at 44.1 kHz, 117 at 48 kHz; constant for every parameter value.
// Neutral: Analog, threshold 0, ceiling 0 and an input below the knee (peaks under -10 dBFS): output = input delayed
// by D (< -120 dB). There is no neutral setting for louder input (it limits by design).
// Delta (handlesDelta): the module writes what the limiting stages changed, output - G x input delayed by D, so the
// Threshold / Ceiling gain is not heard in Delta (as the Maximizer excludes Gain and Output Level).
// Parameter changes: G glides over 20 ms; character / mode change the times without resetting the envelopes.
// "clim.link" is UI only (Threshold and Ceiling move together; no DSP effect).
// T/S: not offered (no twin).
namespace ps
{
struct ClassicLimiterMeters
{
    AtomicFloat muReductionDb;                  // current variable-mu reduction, 20 log10 g (<= 0)
    AtomicFloat brickwallReductionDb;           // current brickwall reduction (<= 0)
    AtomicFloat peakReductionDb;                // deepest total reduction since the UI last exchanged it, stored as a
                                                //   positive amount (3.5 = -3.5 dB) so storeMax keeps the deepest
    AtomicFloat outputPeakDb { -200.0f };       // output sample peak since the UI last exchanged it (dBFS)
};

class ClassicLimiter final : public Module
{
public:
    ClassicLimiter();
    ~ClassicLimiter() override;

    ModuleType getType() const noexcept override { return ModuleType::ClassicLimiter; }
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void setParams (ParamValues) noexcept override;
    void process (float* const* channels, int numSamples, const ProcessContext&) noexcept override;
    int getLatencySamples() const noexcept override;
    bool handlesDelta() const noexcept override { return true; }

    // The latency D at a sample rate (see above); getLatencySamples() == latencyFor (prepared rate).
    static int latencyFor (double sampleRate) noexcept;

    const ClassicLimiterMeters& getMeters() const noexcept;
    ClassicLimiterMeters& getMeters() noexcept;   // for the UI's exchange() of the peak values

    // Gain-reduction history for the panel (single consumer): per 64 output samples one pair, L = the variable-mu
    // reduction, R = the total reduction (both dB, <= 0, minimum over the 64 samples).
    StereoSampleFifo& getHistoryFifo() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
