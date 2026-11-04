#pragma once

#include "dsp/Module.h"

#include <memory>

// Classic Limiter: a tube-style limiter with three modes (a slow, program-driven stage and a fast peak stage per
// channel), Stereo only. Owner: mod-clim. Implementation: ClassicLimiter.cpp. Tests: tests/ClassicLimiterTests.cpp,
// prefix "clim.". The model and every coefficient (one constant block at the top of ClassicLimiter.cpp) are fitted to
// the reference's renders (black-box measurement only).
//
//   x -> input gain G = ceiling - threshold + the mode's make-up (+0.35 dB Analog / Tube, +0.125 dB Modern, measured;
//        full from 2 dB of ceiling - threshold, scaled to zero at 0 dB). Lowering the threshold makes it louder,
//        as a maximizer. Below the ceiling that gain is all the module does.
//     -> per channel (the channels are limited independently, as the reference) and per mode (all three run; a mode
//        change crossfades their gains over 20 ms), on the overshoot O (dB) of G x over Ct = ceiling - 0.01 dB:
//          slow stage S: a fast one-pole attack (about 1 ms) toward a depth law of O (Analog / Tube: O - 0.345 O^0.961,
//            Modern: 0.21 O), released in dB with the measured time constant per mode and Character (Analog
//            0.39 + 0.163 C s, Tube 0.30 + 0.33 C + 0.008 C^2 s, Modern 0.061 + 0.027 C + 0.0054 C^2 s), slower
//            for deep reductions
//          fast stage: the rest of the overshoot held over a short window W (mode and Character), with a medium
//            part (charge / release, a share of the held value) and a fast release
//          gain = the box average over W of 10^(-(slow + fast) / 20); at fixed controls the sliding maximum covers
//            the box. During window automation, interpolate adjacent averages starting at the same output sample.
//     -> maxi::Guard (MaximizerStages.h) with max attack GA = round (0.5 ms x fs): true-peak detection at the ceiling
//          minus 0.02 dB when truePeak is on, sample peak at the ceiling when off (always in the path, so the latency
//          does not depend on truePeak), then a sample clamp at the ceiling.
// Latency D = (Wmax - 1) + (GA + maxi::kTpDetectorDelay + 6) = latencyFor (sampleRate), Wmax = the longest window of
// any mode and Character (104 / 112 / 210 samples at 44.1 / 48 / 96 kHz); constant for every parameter value.
// Under the ceiling: output = G x delayed by D exactly (< -120 dB), in every mode. Neutral: threshold = ceiling (no
// gain, no make-up), input under the ceiling: output = input delayed by D, bit-exact. There is no neutral setting for
// louder input (it limits by design).
// Delta (handlesDelta): the module writes what the limiting stages changed, output - G x input delayed by D, so the
// Threshold / Ceiling gain and the make-up are not heard in Delta (as the Maximizer excludes Gain and Output Level).
// Parameter changes: G and the ceiling glide over 20 ms; Character changes the times and slews the fractional window
// by one sample per 0.25 ms, with at most two detector steps per output sample; the mode crossfades over 20 ms.
// "clim.link" is UI only (Threshold and Ceiling move together; no DSP effect).
// T/S: not offered (no twin).
namespace ps
{
struct ClassicLimiterMeters
{
    AtomicFloat muReductionDb;                  // current slow-stage reduction (dB, <= 0; the deeper channel)
    AtomicFloat brickwallReductionDb;           // current reduction of the rest: fast stage, guard, clamp (<= 0)
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

    // Gain-reduction history for the panel (single consumer): per 64 output samples one pair, L = the slow-stage
    // reduction, R = the total reduction (both dB, <= 0, minimum over the 64 samples and the two channels).
    StereoSampleFifo& getHistoryFifo() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ps
