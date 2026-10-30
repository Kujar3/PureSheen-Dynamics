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

void LevelDetector::reset() noexcept { env = 0.0f; }

float LevelDetector::processDb (float x) noexcept
{
    const float a = std::abs (x);

    switch (mode)
    {
        case DetectionMode::Peak:
            return gainToDbF (a);

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
void GainSmoother::prepare (double sampleRate) noexcept
{
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    fastDecay = (float) (300.0 / sr);
    slowDecay = (float) (20.0 / sr);
    setTimes (10.0f, releaseMs, adaptive);
    reset (0.0f);
}

void GainSmoother::setTimes (float attackMs, float relMs, bool adaptiveRelease) noexcept
{
    attCoeff = onePoleCoeff (std::max (0.0f, attackMs), sr);
    if (relMs != releaseMs || adaptiveRelease != adaptive || relCoeff == 0.0f)
    {
        releaseMs = std::max (0.0f, relMs);
        adaptive = adaptiveRelease;
        relCoeff = onePoleCoeff (releaseMs * (adaptive ? releaseScale : 1.0f), sr);
    }
}

void GainSmoother::reset (float valueDb) noexcept
{
    state = valueDb;
    fastEnv = slowEnv = -200.0f;
    releaseScale = 1.0f;
    counter = 0;
    relCoeff = onePoleCoeff (releaseMs, sr);
}

float GainSmoother::process (float targetGainDb, float detectorDb) noexcept
{
    if (adaptive)
    {
        fastEnv = std::max (detectorDb, fastEnv - fastDecay);
        slowEnv = std::max (detectorDb, slowEnv - slowDecay);

        if (++counter >= 16)
        {
            counter = 0;
            const float crest = std::clamp (slowEnv - fastEnv, 3.0f, 15.0f);
            releaseScale = 2.0f * std::pow (10.0f, -(crest - 3.0f) / 12.0f);   // 2 .. 0.2
            relCoeff = onePoleCoeff (releaseMs * releaseScale, sr);
        }
    }

    const float coeff = targetGainDb < state ? attCoeff : relCoeff;
    state = targetGainDb + coeff * (state - targetGainDb);
    if (std::abs (state) < 1.0e-12f)
        state = 0.0f;
    return state;
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
// decreasing value order; stamps use wrap-safe unsigned differences.
void SlidingMax::prepare (int maxWindow)
{
    capacity = std::max (1, maxWindow) + 1;
    values.assign ((size_t) capacity, 0.0f);
    stamps.assign ((size_t) capacity, 0);
    window = std::clamp (window, 1, capacity - 1);
    reset();
}

void SlidingMax::setWindow (int samples) noexcept
{
    window = std::clamp (samples, 1, std::max (1, capacity - 1));
}

void SlidingMax::reset() noexcept
{
    head = tail = time = 0;
}

float SlidingMax::process (float x) noexcept
{
    if (capacity < 2)
        return x;

    time = (int) ((unsigned) time + 1u);

    while (head != tail)
    {
        const int back = tail == 0 ? capacity - 1 : tail - 1;
        if (values[(size_t) back] <= x)
            tail = back;
        else
            break;
    }

    values[(size_t) tail] = x;
    stamps[(size_t) tail] = time;
    tail = tail + 1 == capacity ? 0 : tail + 1;

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
