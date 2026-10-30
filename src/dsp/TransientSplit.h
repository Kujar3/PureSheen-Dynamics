#pragma once

#include <vector>

// Transient / sustain splitter of the chain's Transient/Sustain channel mode (pure C++17). Owner: chain.
// Implementation: TransientSplit.cpp.
//
// Splits a stereo signal into a transient part T and a sustain part S with T + S == input delayed by
// getLatencySamples() (S is computed as x_L - T, so the sum is exact up to one float rounding: < -135 dBFS for a
// full-scale input, bit-exact wherever the mask is 0).
//
//   x --> detection bank (complementary 4-band split: low1 = LP 120 Hz (x), r1 = x - low1, low2 = LP 800 Hz (r1), ...,
//         2nd-order Butterworth TPT state-variable low-passes, double; sum of the bands == x exactly, no all-pass phase)
//         per band: linked power p = (l^2 + r^2) / 2, fast envelope F and slow envelope W (power one-poles)
//         every 8 samples: d = 10 log10 (F / W) - baseline (a slow tracker that removes the steady F/W offset of
//         noise-like material), silence-gated; mask target = smoothstep ((d - t0) / (t1 - t0))
//         per sample: target interpolated, then a one-pole (rise 0.3 ms, fall 8 ms x band scale), snapped to 0 / 1
//   x --> delay L --> audio bank (the same split) --> T = sum_b g_b * band_b (same g on L and R: the image is kept)
//                                                     S = x_L - T
// Band scales s = { 2.0, 1.25, 0.75, 0.5 } (low .. high); fast attack { 1.0, 0.5, 0.25, 0.15 } ms; slow attack = 3 x
// length x s (tuned so a hit stays in T for about the length); release (both envelopes) = 2.5 x length x s.
// Sensitivity s01 = sens / 100: t0 = 9 - 7.5 s01 dB,
// t1 = t0 + 9 - 6 s01 dB (default 50 %: 5.25 / 11.25 dB). Sensitivity / length changes glide over ~50 ms (no reset).
// Latency L = 64 samples x round (fs / 48 kHz) (minimum 64): 64 at 44.1 / 48 kHz, 128 at 88.2 / 96, 256 at 176.4 / 192;
// constant per sample rate (the mask's 0.3 ms rise completes inside L, so an onset lands in T from its first sample).
// Realtime: prepare() allocates; reset / setParams / process do not. Deterministic, double-precision states, no
// denormals (states are flushed).
namespace ps
{
class TransientSplitter
{
public:
    // lookahead == false: L = 0 (test variant: the first ~0.5 ms of an attack stays in S).
    explicit TransientSplitter (bool lookahead = true) noexcept;

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    // sensitivityPercent 0..100, lengthMs 5..100. Glides; the first call after prepare / reset applies at once.
    void setParams (float sensitivityPercent, float lengthMs) noexcept;

    // in: 2 channels. outT / outS: 2 channels each, distinct from each other and from in.
    // outT + outS == in delayed by getLatencySamples().
    void process (const float* const* in, float* const* outT, float* const* outS, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return latency; }
    static int latencyFor (double sampleRate) noexcept;

    // Meters (audio thread values of the last process call; the chain publishes them).
    float getMaskActivity() const noexcept { return activity; }        // max over bands and samples of g (0..1)
    double getLastTransientEnergy() const noexcept { return energyT; } // sum of T^2 over both channels
    double getLastInputEnergy() const noexcept { return energyX; }     // sum of x_L^2 over both channels

    static constexpr int kNumBands = 4;
    float getBandMask (int band) const noexcept;                       // tests: current g of a band

private:
    struct Svf
    {
        double a1 = 0.0, a2 = 0.0, a3 = 0.0;
        double ic1[2] {}, ic2[2] {};
        void design (double hz, double sampleRate) noexcept;
        double lowPass (int ch, double x) noexcept;
        void clear() noexcept;
        void flush() noexcept;
    };

    struct Band
    {
        double fast = 0.0, slow = 0.0, baseline = 0.0;
        double gain = 0.0, target = 0.0, targetStep = 0.0, interp = 0.0;
        double fastAtt = 0.0, slowAtt = 0.0, release = 0.0, fall = 0.0;
    };

    void updateCoefficients (bool force) noexcept;
    void controlTick() noexcept;

    bool useLookahead = true;
    double sr = 48000.0;
    int latency = 64;
    bool prepared = false, fresh = true;

    Svf detect[3], audio[3];
    Band bands[kNumBands];
    double rise = 0.0, baselineUp = 0.0, baselineDown = 0.0, paramGlide = 0.0;
    double sensTarget = 50.0, lenTarget = 20.0, sens = 50.0, len = 20.0, lenUsed = -1.0, t0 = 5.25, t1 = 11.25;
    int tickPos = 0;

    std::vector<float> ring[2];
    int ringMask = 0, ringPos = 0;

    float activity = 0.0f;
    double energyT = 0.0, energyX = 0.0;
};
} // namespace ps
