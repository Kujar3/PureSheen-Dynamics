#pragma once

#include <vector>

// Transient / sustain splitter of the chain's Transient/Sustain channel mode (pure C++17). Owner: chain.
// Implementation: TransientSplit.cpp.
//
// Splits a stereo signal into a transient part T and a sustain part S with T + S == input delayed by
// getLatencySamples() (S is computed as x_L - T, so the sum is exact up to one float rounding: < -135 dBFS for a
// full-scale input, bit-exact wherever the mask is 0).
//
// One broadband gain mask per channel (the channels are independent): T = m (t) x_L, S = (1 - m (t)) x_L.
//   x --> detection bank (complementary 4-band split at 120 / 800 / 5000 Hz: low1 = LP (x), r1 = x - low1,
//         low2 = LP (r1), ...; 2nd-order Butterworth TPT state-variable low-passes, double) --> per band b:
//         p = y_b^2, fast envelope F_b and slow envelope W_b (power one-poles, attack / release),
//         r_b = 10 log10 ((F_b + e) / (W_b + e)) / span, h_b = max (r_b, h_b x release) (an onset sets the band's
//         transient state, which then decays exponentially), c_b = clamp (h_b + f min (r_b, 0), 0, 1) (a falling
//         level pulls it down sooner)
//   mask m = (sum_b w_b c_b^k / sum_b w_b)^(1 / k), w_b = (F_b + e)^a   (a power mean weighted by the bands' current
//         power: a hit in a loud band moves the whole signal, a hit in a quiet band only part of it)
//   x --> delay L --> T = m x_L, S = x_L - T  (the mask reads L samples ahead)
// The constants (.cpp) are fitted to the reference's measured masks (tone bursts, level steps, noise, drums, vocal,
// guitar and four songs): a steady tone gets 0 (a low one a little: the detector ripples on bass), steady noise
// ~0.2, an onset ~1 for ~50 ms, back to the steady value ~250 ms later; music ~0.3..0.5 on average.
// Sensitivity (default 50 %) scales the dB span of the mapping (span x 2^((50 - sens) / 50): 100 % halves it);
// length (default 20 ms) scales the release of the transient state (x length / 20). Both glide over ~50 ms.
// Latency L = 0.206 ms rounded (9 / 10 samples at 44.1 / 48 kHz, 18 / 20 at 88.2 / 96, 36 / 40 at 176.4 / 192): constant per
// sample rate. The band states and the mask run once per 8 samples (x rate family) and the mask is interpolated.
// Realtime: prepare() allocates; reset / setParams / process do not. Deterministic, double-precision states, no
// denormals (states are flushed).
namespace ps
{
class TransientSplitter
{
public:
    // lookahead == false: L = 0 (test variant: the mask reacts L samples later).
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
    float getMaskActivity() const noexcept { return activity; }        // max over channels and samples of m (0..1)
    double getLastTransientEnergy() const noexcept { return energyT; } // sum of T^2 over both channels
    double getLastInputEnergy() const noexcept { return energyX; }     // sum of x_L^2 over both channels

    static constexpr int kNumBands = 4;
    float getMask (int channel) const noexcept;                        // tests: current m of a channel

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
        double fast[2] {}, slow[2] {}, held[2] {};
    };

    void updateCoefficients (bool force) noexcept;
    void controlTick() noexcept;

    bool useLookahead = true;
    double sr = 48000.0;
    int latency = 20, tick = 8, tickPos = 0;
    bool prepared = false, fresh = true;

    Svf detect[kNumBands - 1];
    Band bands[kNumBands];
    double fastAtt = 0.0, fastRel = 0.0, slowAtt = 0.0, slowRel = 0.0, paramGlide = 0.0;
    double sensTarget = 50.0, lenTarget = 20.0, sens = 50.0, len = 20.0, sensUsed = -1.0, lenUsed = -1.0;
    double invSpan = 0.0, holdDecay = 0.0;
    double mask[2] {}, target[2] {}, maskStep[2] {};

    std::vector<float> ring[2];
    int ringMask = 0, ringPos = 0;

    float activity = 0.0f;
    double energyT = 0.0, energyX = 0.0;
};
} // namespace ps
