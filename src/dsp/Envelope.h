#pragma once

#include "dsp/Types.h"

#include <cmath>
#include <vector>

// Small realtime building blocks shared by the dynamics-type modules. Owner: dsp-common (header-mostly; Envelope.cpp).
namespace ps
{
inline float dbToGainF (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDbF (float g) noexcept { return g > 1.0e-10f ? 20.0f * std::log10 (g) : -200.0f; }

// One-pole coefficient for a time constant (time to reach 1 - 1/e).
float onePoleCoeff (double timeMs, double sampleRate) noexcept;

//==============================================================================
// Level detector producing a dB level per sample.
//   Peak     : instantaneous |x| with attack/release ballistics on the gain computer side (detector = |x|)
//   Envelope : |x| through a peak envelope with fixed 0.1 ms attack / 10 ms release (smooth, still fast)
//   Rms      : mean square over ~10 ms (one-pole), sqrt
class LevelDetector
{
public:
    void prepare (double sampleRate) noexcept;
    void setMode (DetectionMode) noexcept;
    void reset() noexcept;
    float processDb (float x) noexcept;   // returns level in dB (>= -200)

private:
    DetectionMode mode = DetectionMode::Envelope;
    double sr = 48000.0;
    float env = 0.0f, envAtt = 0.0f, envRel = 0.0f, rmsCoeff = 0.0f;
};

//==============================================================================
// Static gain curve: compression / limiting above the threshold, upward compression / expansion for negative ratios.
// Returns the gain change in dB for an input level in dB (<= 0 for downward, >= 0 for upward; upward gain is
// capped at +24 dB and only applies within 48 dB below the threshold).
//   ratio >= 1      : downward, above the threshold, (x - T) (1/ratio - 1); quadratic soft knee of kneeDb centred on T
//   |ratio| < 1     : 1:1 (0 dB)
//   ratio <= -1     : upwardExpands == false (Compressor): upward compression below the threshold, the level is
//                     raised toward T by (T - x) (1 - 1/|ratio|); more than 48 dB below T the lift falls back by 1 dB
//                     per dB (continuous curve, quiet noise is not raised); capped at +24 dB
//                     upwardExpands == true (Limiter): upward expansion above the threshold, (x - T) (|ratio| - 1),
//                     capped at +24 dB
struct GainCurve
{
    float thresholdDb = 0.0f;
    float ratio = 1.0f;     // see CompressorParams::ratio
    float kneeDb = 0.0f;
    bool upwardExpands = false;   // negative ratios: false = upward compression (below T), true = upward expansion

    float gainDb (float inputDb) const noexcept;
};

//==============================================================================
// Attack / release smoothing of a gain-reduction signal in dB (attack = moving toward more reduction).
// Adaptive release (Dynamics "Adaptive Release"): release scales from 0.2x (transient material, fast GR changes) to
// 2x (sustained material) of the set value, using the crest of the detector signal over ~50 ms.
//   crest = slow peak envelope (dB, releases 20 dB/s) - fast peak envelope (dB, releases 300 dB/s): how far the level
//   has fallen below its recent peak. crest <= 3 dB -> 2x, >= 15 dB -> 0.2x, exponential in between.
class GainSmoother
{
public:
    void prepare (double sampleRate) noexcept;
    void setTimes (float attackMs, float releaseMs, bool adaptiveRelease) noexcept;
    void reset (float valueDb = 0.0f) noexcept;
    float process (float targetGainDb, float detectorDb) noexcept;

private:
    // double state and coefficients: at long releases / high rates the per-sample step of a float one-pole falls
    // below the float resolution of the state and the gain stalls short of its target
    double sr = 48000.0;
    double attCoeff = 0.0, relCoeff = 0.0, state = 0.0;
    float releaseMs = 100.0f;
    bool adaptive = false;
    double fastEnv = -200.0, slowEnv = -200.0;
    double fastDecay = 0.0025, slowDecay = 0.0004;   // dB per sample
    double releaseScale = 1.0;
    int counter = 0;
};

//==============================================================================
// Fixed-capacity delay line (stereo or mono use: one instance per channel). prepare allocates.
class DelayLine
{
public:
    void prepare (int maxDelaySamples);
    void setDelay (int samples) noexcept;     // 0 .. maxDelaySamples
    int getDelay() const noexcept { return delay; }
    void reset() noexcept;
    float process (float x) noexcept;
    void process (float* data, int numSamples) noexcept;

private:
    std::vector<float> buffer;
    int writePos = 0, delay = 0, mask = 0;
};

//==============================================================================
// Running maximum over the last `window` samples (monotonic deque in a ring; O(1) amortised). For lookahead limiters.
// The last maxWindow inputs are kept, so a window that grows covers the older samples at once (setWindow rebuilds the
// deque from that history, O(new window) once per growth); a window that shrinks just drops the expired entries.
class SlidingMax
{
public:
    void prepare (int maxWindow);
    void setWindow (int samples) noexcept;
    void reset() noexcept;
    float process (float x) noexcept;

private:
    void push (float x, int stamp) noexcept;

    std::vector<float> values;
    std::vector<int> stamps;
    std::vector<float> history;     // the last capacity - 1 inputs (ring)
    int head = 0, tail = 0, capacity = 0, window = 1, time = 0;
    int histPos = 0, histFill = 0;
};

//==============================================================================
// Linear parameter smoother (per-block targets, per-sample values).
class LinearSmoother
{
public:
    void prepare (double sampleRate, double rampMs) noexcept;
    void setTarget (float v) noexcept;
    void snap (float v) noexcept;
    float next() noexcept;
    float getCurrent() const noexcept { return current; }
    bool isSmoothing() const noexcept { return remaining > 0; }

private:
    float current = 0.0f, target = 0.0f, step = 0.0f;
    int remaining = 0, rampSamples = 1;
};

} // namespace ps
