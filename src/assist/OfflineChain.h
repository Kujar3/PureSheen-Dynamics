#pragma once

#include "assist/AssistEngine.h"

#include <atomic>
#include <vector>

// Offline rendering for the Assistant (pure C++17; worker thread, allocates). Builds only the modules a suggestion
// uses (ps::makeModule), runs them in series in Stereo with parameter set A, and measures integrated loudness. No
// chain host, no T/S twins, no meters: a light copy for numbers, not for listening.
namespace ps::assist
{
struct Excerpt
{
    std::vector<float> left, right;
    double sampleRate = 48000.0;
    long long size() const noexcept { return (long long) left.size(); }
};

// The loudest `seconds` of the audio (by 3 s short-term loudness), or all of it when shorter.
Excerpt loudestExcerpt (const float* left, const float* right, long long numSamples, double sampleRate, double seconds);

// Renders `order` with `values` over the excerpt into `out` (resized). stopAtMaximizer: the Maximizer and everything
// after it are skipped. `cancel` (optional) stops early; returns false then.
bool renderSeries (const std::vector<ModuleType>& order, const std::vector<float>& values, const Excerpt& in,
                   Excerpt& out, bool stopAtMaximizer, const std::atomic<bool>* cancel = nullptr);

float integratedLufs (const Excerpt&);

// Sets the Maximizer gain so the excerpt reaches the suggestion's loudness target (secant iterations on the
// Maximizer alone after one pre-Maximizer render; within 0.1 LU or 6 runs) and measures gainPerLufs. No-op without a
// Maximizer in the order. Returns false when cancelled.
bool refineLoudness (Suggestion&, const Excerpt&, const std::atomic<bool>* cancel = nullptr);

// Fills the loudness-match tables (pre-Maximizer loudness at kTablePoints positions of each loudness-matched macro).
bool buildLoudnessTables (Suggestion&, const Excerpt&, const Analysis&, const std::atomic<bool>* cancel = nullptr);
} // namespace ps::assist
