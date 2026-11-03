#pragma once

#include "dsp/Envelope.h"
#include "dsp/Oversampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// Internal building blocks of the Maximizer (owner: dsp-maximizer). Not a public contract: only Maximizer*.cpp and
// tests/MaximizerTests.cpp include this header.
//
// Every limiter stage here works on "reductions" in natural-log units (D = ln (level / threshold) >= 0, so the gain is
// exp (-D)) in double precision, and shares one gain construction that makes the brickwall property provable:
//   1. a sliding maximum of D over a window that covers every sample a peak depends on (hold),
//   2. a release that may only make the reduction larger (G >= D),
//   3. a moving average (box) of exp (-G) whose length A is at most the lookahead,
// so every gain value that touches a peak is at most the gain that peak needs.
namespace ps::maxi
{
constexpr int kChunk = 64;                 // internal processing block (all stage buffers hold kChunk samples)
constexpr int kMidDetectorDelay = 4;       // LimiterCore: half-sample interpolator centre
constexpr int kTpTaps = 16;                // Guard: true-peak interpolator taps per phase
constexpr int kTpPhases = 7;               // 4 phases matching ps::TruePeakMeter + 3 in between (1/8-sample grid)
constexpr int kTpDetectorDelay = 8;        // Guard: interpolated points of interval m are known at m + 8

inline int nextPow2 (int v) noexcept
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

inline double msToSamples (double ms, double sr) noexcept { return ms * 0.001 * sr; }

// One-pole coefficients from a time constant in samples (guarded against 0).
inline double decayCoeff (double samples) noexcept { return samples > 1.0e-9 ? std::exp (-1.0 / samples) : 0.0; }
inline double chargeCoeff (double samples) noexcept { return 1.0 - decayCoeff (samples); }

// Bounded, allocation-free control ramp. Limiter reductions remain conservative because
// the smoothed independence only blends linked and independent gains, both already safe.
inline double rampControl (double& current, double target, double step) noexcept
{
    current += std::clamp (target - current, -step, step);
    return current;
}

//==============================================================================
// Sliding maximum over absolute sample indices (monotonic deque in a power-of-two ring).
class MaxDeque
{
public:
    void prepare (int capacity);
    void reset() noexcept { head = tail = 0; }

    void push (long long index, double v) noexcept
    {
        while (tail != head && vals[(tail - 1u) & mask] <= v)
            --tail;
        vals[tail & mask] = v;
        stamps[tail & mask] = index;
        ++tail;
        if (tail - head > (unsigned) mask + 1u)   // never happens when capacity >= window + 2
            ++head;
    }

    void dropOlderThan (long long minIndex) noexcept
    {
        while (head != tail && stamps[head & mask] < minIndex)
            ++head;
    }

    double max() const noexcept { return head != tail ? vals[head & mask] : 0.0; }

private:
    std::vector<double> vals;
    std::vector<long long> stamps;
    unsigned head = 0, tail = 0, mask = 0;
};

//==============================================================================
// Moving average of the last `len` values (len changes take effect at once and keep the sum exact: the ring keeps
// the older values). Values are gains in (0, 1]; the ring starts full of 1.0, so with no reduction the average is
// exactly 1.0. The running sum is recomputed exactly every `capacity` pushes (no drift).
class BoxAverage
{
public:
    void prepare (int maxLength);
    void reset() noexcept;
    void setLength (int newLength) noexcept;
    int getLength() const noexcept { return len; }

    double process (double v) noexcept
    {
        ++pos;
        const double leaving = ring[(size_t) ((pos - len) & mask)];
        ring[(size_t) (pos & mask)] = v;
        sum += v - leaving;
        if (++sinceResync > mask)
            resync();
        return sum * invLen;
    }

    // Adjacent window lengths are both valid lookahead envelopes. Interpolating
    // them prevents a control change from abruptly exposing older unity gains.
    double process (double v, double fractionalLength) noexcept
    {
        const double length = std::clamp (fractionalLength, 1.0, (double) maxLen);
        const int shorter = (int) length;
        setLength (shorter);
        const double a = process (v);
        const double fraction = length - shorter;
        if (fraction == 0.0 || shorter == maxLen)
            return a;
        const double b = (sum + ring[(size_t) ((pos - len) & mask)]) / (len + 1);
        return a + fraction * (b - a);
    }

private:
    void resync() noexcept;

    std::vector<double> ring;
    long long pos = 0;
    long long mask = 0;
    int len = 1, maxLen = 1;
    long long sinceResync = 0;
    double sum = 1.0, invLen = 1.0;
};

//==============================================================================
// The lookahead limiter core shared by Smooth, Transparent, Aggressive and as the post stage of Spectral / Multiband.
// Peak detection (optionally with a half-sample interpolator for intersample peaks), transient preservation, two-stage
// program-dependent release (fast stage for transients, slow stage that charges on sustained limiting), extra hold and
// slower fast-release on low-frequency material, stereo independence separately for the fast (transient) and slow
// (sustain) stages. Gain applied to the signal delayed by the latency.
struct CoreSettings
{
    double attackMs = 1.0;            // box length (clamped to the lookahead)
    double holdMs = 0.0;
    double fastReleaseMs = 40.0;
    double slowReleaseMs = 600.0;
    double slowChargeMs = 250.0;     // <= 0: no slow stage
    double lfAdapt = 0.0;             // 0..1: on bass-heavy material the fast release slows (x4) and the hold grows
    double transientPreserve = 0.0;   // 0..1: part of a transient's reduction left to the (fast) guard after the core
    double siTrans = 0.0, siSus = 0.0;   // 0..1 stereo independence of the fast / slow stage
    bool midDetect = false;           // add half-sample interpolated peaks to the detector (True Peak on)
    double denseReleaseMs = 0.0;      // the fast release grows by this x u^densePower (0: off), u = (density - denseKnee) /
    double denseKnee = 0.6;           //   (1 - denseKnee) clamped to 0..1, density = the share of recent time spent
    double denseChargeMs = 50.0;      //   limiting (one-pole, charge / release times)
    double denseReleaseEnvMs = 100.0;
    double densePower = 3.0;
};

class LimiterCore
{
public:
    void prepare (double sampleRate, double maxLookaheadMs);
    void reset() noexcept;
    void setLookahead (int samples) noexcept;            // <= prepared maximum; call reset() after it
    void setSettings (const CoreSettings&) noexcept;     // cheap; recomputes coefficients only when changed
    int getLatency() const noexcept { return lookahead + kMidDetectorDelay; }

    // in / out may alias. thr[i] = linear threshold of the sample with index (input index - kMidDetectorDelay).
    void process (const float* const* in, float* const* out, const double* thr, int n) noexcept;

    double minGain[2] { 1.0, 1.0 };   // since the caller last reset it

private:
    void updateCoefficients() noexcept;

    double sr = 48000.0;
    int maxLookahead = 1, lookahead = 1, sigMask = 0, redMask = 0;
    CoreSettings settings;
    bool settingsValid = false;
    double controlStep = 1.0, siTransNow = 0.0, siSusNow = 0.0, preserveNow = 0.0;

    std::array<std::vector<float>, 2> sig;     // input ring (index = absolute sample index)
    std::array<std::vector<double>, 2> red;    // reduction ring (at detector index)
    std::array<MaxDeque, 2> windowMax;
    std::array<BoxAverage, 2> box;
    long long t = 0, k = 0;                    // newest input index, newest index pushed into the window
    int attack = 1, attackTarget = 1, hold = 0, holdTarget = 0, maxHold = 0;

    // release state: [0], [1] per channel, [2] linked
    std::array<double, 3> slow {}, fast {};
    double kSlowCharge = 0.0, kSlowRelease = 0.0, kFast = 0.0, kFastBase = 0.0;
    // dense limiting (linked): share of recent samples with a reduction, and what it adds to the fast release
    double density = 0.0, kDenseCharge = 0.0, kDenseRelease = 0.0, denseSamples = 0.0;

    // transient detector (linked)
    double envFast = 0.0, envSlow = 0.0, kEnvFastRel = 0.0, kEnvSlowAtt = 0.0, kEnvSlowRel = 0.0;
    // low-frequency content estimate
    double lfState = 0.0, lfPower = 0.0, allPower = 0.0, kLf = 0.0, kPower = 0.0, lfRatio = 0.0;
    std::array<double, 8> midTaps {};
};

//==============================================================================
// The final brickwall stage of every mode: true-peak (or sample-peak) detection on a 1/8-sample grid that contains the
// exact evaluation points of ps::TruePeakMeter, a window that covers all 16 samples each interpolated point depends
// on, single-stage release, linked / unlinked by `independence`, box attack of length <= maxAttack.
// Latency maxAttack + 14 samples (8 detector + 6 window centring), whatever the attack, release or detection mode.
class Guard
{
public:
    Guard();
    void prepare (double sampleRate, int maxAttackSamples);
    void reset() noexcept;
    void setMaxAttack (int samples) noexcept;        // <= prepared maximum; call reset() after it
    // A separate sustain independence enables the Low Latency two-part control.
    void setTimes (int attackSamples, double releaseMs, double independence,
                   double sustainIndependence = -1.0) noexcept;
    int getLatency() const noexcept { return maxAttack + kTpDetectorDelay + 6; }

    // thr[i] / truePeak[i]: threshold and detection of the sample with index (input index - 8).
    void process (const float* const* in, float* const* out, const double* thr, const std::uint8_t* truePeak,
                  int n) noexcept;

    double minGain[2] { 1.0, 1.0 };

private:
    double sr = 48000.0;
    int prepMaxAttack = 1, maxAttack = 1, attack = 1, sigMask = 0;
    double kRelease = 0.0, independence = 0.0, independenceNow = 0.0, controlStep = 1.0;
    double sustainIndependence = -1.0, sustainIndependenceNow = 0.0, kSustainCharge = 0.0;
    double attackLogNow = 0.0, attackLogTarget = 0.0;
    std::array<std::vector<float>, 2> sig;
    std::array<std::array<double, 2 * kTpTaps>, 2> hist {};
    int histPos = 0;
    std::array<MaxDeque, 2> windowMax;
    std::array<BoxAverage, 2> box;
    std::array<double, 3> release {};
    std::array<double, 3> sustained {};
    long long t = 0;
    std::array<std::array<double, kTpTaps>, kTpPhases> phases {};
};

//==============================================================================
// Soft clip on 4x oversampled audio. Output = input delayed by the oversampler latency + the downsampled clipping
// residue (curve (x) - x) * amount, so at amount 0 it is an exact delay. The curve is relative to the output level: a
// drive (Light +1.5 dB, Moderate +3.2 dB, Heavy +5.9 dB), linear up to a knee (0.9 / 0.75 / 0.6 x the output level),
// then a tanh knee that approaches the output level. So quiet passages get (1 - amount) + amount x drive of gain
// (measured: the reference's soft clip at 15 % Light lifts everything below the limiter by +0.23 dB, Heavy +1.2 dB).
class SoftClipStage
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void setMinimumPhase (bool minimumPhase) noexcept;   // call reset() after it (mode switch)
    int getLatency() const noexcept;
    void setParams (bool on, int mode, double amount01) noexcept;
    // ceilingLin[i] carries input sample i's smoothed ceiling; the control is
    // interpolated at the oversampled rate without depending on chunk boundaries.
    void process (const float* const* in, float* const* out, const double* ceilingLin, int n) noexcept;

private:
    Oversampler linear, minimum;
    bool useMinimum = false, stale = true;
    std::array<DelayLine, 2> dry;
    double onset = 0.708, onsetNow = 0.708, amountTarget = 0.0, amountNow = 0.0, amountStep = 0.0;
    double drive = 1.0, driveNow = 1.0;
    double previousCeiling = 1.0;
    bool ceilingValid = false;
    std::array<std::vector<float>, 2> residue;
};

} // namespace ps::maxi
