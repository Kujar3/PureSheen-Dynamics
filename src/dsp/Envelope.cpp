#include "dsp/Envelope.h"

#include <algorithm>
#include <cmath>

// Realtime building blocks for the dynamics-type modules (dsp-common). See Envelope.h for the definitions.
// Choices: Rms reads true RMS (a sine reads 3 dB below its peak); Envelope is a peak envelope (0.1 ms attack, 10 ms
// release) that reads a sine at (nearly) its peak.
namespace ps
{
float onePoleCoeff (double timeMs, double sampleRate) noexcept
{
    const double samples = timeMs * 0.001 * sampleRate;
    return samples > 0.0 ? (float) std::exp (-1.0 / samples) : 0.0f;
}

//==============================================================================
void LevelDetector::prepare (double sampleRate) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    envAtt = onePoleCoeff (0.1, sr);
    envRel = onePoleCoeff (10.0, sr);
    rmsCoeff = onePoleCoeff (10.0, sr);
    int size = 1;
    while (size < (int) std::ceil (kMaxPeakHoldMs * 0.001 * sr) + 2)
        size <<= 1;
    holdVal.assign ((size_t) size, 0.0f);
    holdAt.assign ((size_t) size, 0);
    holdMask = size - 1;
    setPeakHold (holdMs);
    reset();
}

void LevelDetector::setMode (DetectionMode m) noexcept
{
    if (m != mode)
    {
        mode = m;
        env = 0.0f;
    }
}

void LevelDetector::reset() noexcept
{
    env = 0.0f;
    holdHead = holdTail = 0;
    holdCount = 0;
}

void LevelDetector::setPeakHold (double timeMs) noexcept
{
    holdMs = timeMs;
    const int maxLen = holdMask > 0 ? holdMask - 1 : 0;
    holdLen = std::clamp ((int) std::lround (std::clamp (timeMs, 0.0, kMaxPeakHoldMs) * 0.001 * sr), 0, maxLen);
}

float LevelDetector::processDb (float x) noexcept
{
    const float a = std::abs (x);

    switch (mode)
    {
        case DetectionMode::Peak:
        {
            if (holdLen <= 0)
                return gainToDbF (a);
            // sliding maximum over the last holdLen + 1 samples
            while (holdTail != holdHead && holdVal[(size_t) ((holdTail - 1) & holdMask)] <= a)
                --holdTail;
            holdVal[(size_t) (holdTail & holdMask)] = a;
            holdAt[(size_t) (holdTail & holdMask)] = holdCount;
            ++holdTail;
            while (holdAt[(size_t) (holdHead & holdMask)] < holdCount - holdLen)
                ++holdHead;
            ++holdCount;
            return gainToDbF (holdVal[(size_t) (holdHead & holdMask)]);
        }

        case DetectionMode::Envelope:
            env = a > env ? a + envAtt * (env - a) : a + envRel * (env - a);
            if (env < 1.0e-15f)
                env = 0.0f;
            return gainToDbF (env);

        case DetectionMode::Rms:
        default:
        {
            const float sq = a * a;
            env = sq + rmsCoeff * (env - sq);
            if (env < 1.0e-30f)
                env = 0.0f;
            return env > 1.0e-20f ? 10.0f * std::log10 (env) : -200.0f;
        }
    }
}

//==============================================================================
float GainCurve::gainDb (float x) const noexcept
{
    const float T = thresholdDb;
    const float W = std::max (0.0f, kneeDb);
    const float d = x - T;
    const bool inKnee = W > 0.0f && 2.0f * std::abs (d) <= W;

    if (ratio >= 1.0f)
    {
        const float c = 1.0f / ratio - 1.0f;   // <= 0
        if (inKnee)
            return c * (d + 0.5f * W) * (d + 0.5f * W) / (2.0f * W);
        return d > 0.0f ? c * d : 0.0f;
    }

    if (ratio > -1.0f)
        return 0.0f;

    const float r = -ratio;
    constexpr float kMaxUp = 24.0f;

    if (upwardExpands)
    {
        const float c = r - 1.0f;   // >= 0
        float g;
        if (inKnee)
            g = c * (d + 0.5f * W) * (d + 0.5f * W) / (2.0f * W);
        else
            g = d > 0.0f ? c * d : 0.0f;
        return std::min (g, kMaxUp);
    }

    const float c = 1.0f - 1.0f / r;   // >= 0
    float g;
    if (inKnee)
        g = c * (d - 0.5f * W) * (d - 0.5f * W) / (2.0f * W);
    else
        g = d < 0.0f ? -c * d : 0.0f;

    constexpr float kRange = 48.0f;
    if (d < -kRange)
        g = std::max (0.0f, kRange * c - (-kRange - d));

    return std::min (g, kMaxUp);
}

//==============================================================================
namespace
{
    // double-precision one-pole coefficient (onePoleCoeff's float result is too coarse near 1 for long times)
    inline double onePoleCoeffD (double timeMs, double sampleRate) noexcept
    {
        const double samples = timeMs * 0.001 * sampleRate;
        return samples > 0.0 ? std::exp (-1.0 / samples) : 0.0;
    }
}

void GainSmoother::prepare (double sampleRate) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    fastDecay = 300.0 / sr;
    slowDecay = 20.0 / sr;
    setTimes (10.0f, releaseMs, adaptive);
    reset (0.0f);
}

void GainSmoother::setTimes (float attackMs, float relMs, bool adaptiveRelease) noexcept
{
    attCoeff = onePoleCoeffD (std::max (0.0, (double) attackMs), sr);
    if (relMs != releaseMs || adaptiveRelease != adaptive || relCoeff == 0.0)
    {
        releaseMs = std::max (0.0f, relMs);
        adaptive = adaptiveRelease;
        relCoeff = onePoleCoeffD ((double) releaseMs * (adaptive ? releaseScale : 1.0), sr);
    }
}

void GainSmoother::reset (float valueDb) noexcept
{
    state = (double) valueDb;
    fastEnv = slowEnv = -200.0;
    releaseScale = 1.0;
    counter = 0;
    relCoeff = onePoleCoeffD ((double) releaseMs, sr);
}

float GainSmoother::process (float targetGainDb, float detectorDb) noexcept
{
    if (adaptive)
    {
        fastEnv = std::max ((double) detectorDb, fastEnv - fastDecay);
        slowEnv = std::max ((double) detectorDb, slowEnv - slowDecay);

        if (++counter >= 16)
        {
            counter = 0;
            const double crest = std::clamp (slowEnv - fastEnv, 3.0, 15.0);
            releaseScale = 2.0 * std::pow (10.0, -(crest - 3.0) / 12.0);   // 2 .. 0.2
            relCoeff = onePoleCoeffD ((double) releaseMs * releaseScale, sr);
        }
    }

    const double target = (double) targetGainDb;
    const double coeff = target < state ? attCoeff : relCoeff;
    state = target + coeff * (state - target);
    if (std::abs (state) < 1.0e-12)
        state = 0.0;
    return (float) state;
}

//==============================================================================
void DelayLine::prepare (int maxDelaySamples)
{
    int size = 1;
    while (size < maxDelaySamples + 1)
        size <<= 1;
    buffer.assign ((size_t) size, 0.0f);
    mask = size - 1;
    writePos = 0;
    delay = std::min (delay, mask);
}

void DelayLine::setDelay (int samples) noexcept
{
    delay = samples < 0 ? 0 : (samples > mask ? mask : samples);
}

void DelayLine::reset() noexcept
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    writePos = 0;
}

float DelayLine::process (float x) noexcept
{
    if (buffer.empty())
        return x;
    buffer[(size_t) writePos] = x;
    const float y = buffer[(size_t) ((writePos - delay) & mask)];
    writePos = (writePos + 1) & mask;
    return y;
}

void DelayLine::process (float* data, int numSamples) noexcept
{
    if (buffer.empty() || delay == 0)
    {
        for (int i = 0; i < numSamples && ! buffer.empty(); ++i)
        {
            buffer[(size_t) writePos] = data[i];
            writePos = (writePos + 1) & mask;
        }
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        buffer[(size_t) writePos] = data[i];
        data[i] = buffer[(size_t) ((writePos - delay) & mask)];
        writePos = (writePos + 1) & mask;
    }
}

//==============================================================================
// Ring of capacity = maxWindow + 1 entries (never full): front = head, back = tail - 1. Entries are kept in
// decreasing value order; stamps use wrap-safe unsigned differences. `history` keeps the last maxWindow inputs
// (histFill of them valid since reset) so a growing window can be rebuilt exactly.
void SlidingMax::prepare (int maxWindow)
{
    capacity = std::max (1, maxWindow) + 1;
    values.assign ((size_t) capacity, 0.0f);
    stamps.assign ((size_t) capacity, 0);
    history.assign ((size_t) (capacity - 1), 0.0f);
    window = std::clamp (window, 1, capacity - 1);
    reset();
}

void SlidingMax::setWindow (int samples) noexcept
{
    const int w = std::clamp (samples, 1, std::max (1, capacity - 1));
    const bool grows = w > window;
    window = w;
    if (! grows || capacity < 2)
        return;   // shrinking: process() drops the entries that left the window

    // Rebuild the deque from the last min (window, histFill) inputs, oldest first. history[histPos - 1] is the
    // newest input (stamp `time`).
    head = tail = 0;
    const int n = std::min (window, histFill);
    const int histSize = capacity - 1;
    for (int age = n - 1; age >= 0; --age)
    {
        int idx = histPos - 1 - age;
        while (idx < 0)
            idx += histSize;
        push (history[(size_t) idx], (int) ((unsigned) time - (unsigned) age));
    }
}

void SlidingMax::reset() noexcept
{
    head = tail = time = 0;
    histPos = histFill = 0;
}

void SlidingMax::push (float x, int stamp) noexcept
{
    while (head != tail)
    {
        const int back = tail == 0 ? capacity - 1 : tail - 1;
        if (values[(size_t) back] <= x)
            tail = back;
        else
            break;
    }

    values[(size_t) tail] = x;
    stamps[(size_t) tail] = stamp;
    tail = tail + 1 == capacity ? 0 : tail + 1;
}

float SlidingMax::process (float x) noexcept
{
    if (capacity < 2)
        return x;

    time = (int) ((unsigned) time + 1u);

    history[(size_t) histPos] = x;
    histPos = histPos + 1 == capacity - 1 ? 0 : histPos + 1;
    histFill = std::min (histFill + 1, capacity - 1);

    push (x, time);

    while ((unsigned) time - (unsigned) stamps[(size_t) head] >= (unsigned) window)
        head = head + 1 == capacity ? 0 : head + 1;

    return values[(size_t) head];
}

//==============================================================================
void LinearSmoother::prepare (double sampleRate, double rampMs) noexcept
{
    rampSamples = (int) std::lround (sampleRate * rampMs * 0.001);
    if (rampSamples < 1)
        rampSamples = 1;
}

void LinearSmoother::setTarget (float v) noexcept
{
    if (v == target)
        return;
    target = v;
    remaining = rampSamples;
    step = (target - current) / (float) rampSamples;
}

void LinearSmoother::snap (float v) noexcept
{
    current = target = v;
    step = 0.0f;
    remaining = 0;
}

float LinearSmoother::next() noexcept
{
    if (remaining > 0)
    {
        if (--remaining == 0)
            current = target;
        else
            current += step;
    }
    return current;
}
} // namespace ps
