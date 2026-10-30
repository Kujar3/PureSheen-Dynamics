#pragma once

// Deterministic test signals and tolerance checks for the core tests (no rand, no time).

#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace pstest
{
constexpr double kPi = 3.14159265358979323846;

// Numerical Recipes LCG. next() is uniform in [-1, 1).
struct Lcg
{
    explicit Lcg (std::uint32_t seed = 12345u) : state (seed) {}

    float next() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return (float) (state >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }

    std::uint32_t state;
};

inline double dbfsToAmplitude (double dbfs) { return std::pow (10.0, dbfs / 20.0); }

// Sine with peak level `peakDbfs`.
inline std::vector<float> sine (double frequency, double peakDbfs, double seconds, double sampleRate,
                                double phaseRadians = 0.0)
{
    const auto n = (size_t) std::llround (seconds * sampleRate);
    const double a = dbfsToAmplitude (peakDbfs);
    std::vector<float> out (n);

    for (size_t i = 0; i < n; ++i)
        out[i] = (float) (a * std::sin (2.0 * kPi * frequency * (double) i / sampleRate + phaseRadians));

    return out;
}

struct Segment
{
    double peakDbfs;
    double seconds;
};

// One continuous-phase sine whose level changes from segment to segment (EBU Tech 3341 / 3342 style).
inline std::vector<float> sineSequence (double frequency, std::initializer_list<Segment> segments, double sampleRate)
{
    std::vector<float> out;
    size_t i = 0;

    for (const auto& s : segments)
    {
        const auto n = (size_t) std::llround (s.seconds * sampleRate);
        const double a = dbfsToAmplitude (s.peakDbfs);

        for (size_t k = 0; k < n; ++k, ++i)
            out.push_back ((float) (a * std::sin (2.0 * kPi * frequency * (double) i / sampleRate)));
    }

    return out;
}

inline std::vector<float> whiteNoise (double amplitude, double seconds, double sampleRate, std::uint32_t seed)
{
    Lcg lcg (seed);
    std::vector<float> out ((size_t) std::llround (seconds * sampleRate));

    for (auto& x : out)
        x = (float) (amplitude * lcg.next());

    return out;
}

// Pink noise (-3 dB / octave) from Paul Kellet's refined filter on LCG white noise.
inline std::vector<float> pinkNoise (double amplitude, double seconds, double sampleRate, std::uint32_t seed)
{
    Lcg lcg (seed);
    std::vector<float> out ((size_t) std::llround (seconds * sampleRate));
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;

    for (auto& x : out)
    {
        const double white = lcg.next();
        b0 = 0.99886 * b0 + white * 0.0555179;
        b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520;
        b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522;
        b5 = -0.7616 * b5 - white * 0.0168980;
        const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
        b6 = white * 0.115926;
        x = (float) (amplitude * 0.11 * pink);
    }

    return out;
}

inline std::vector<float> concat (std::vector<float> a, const std::vector<float>& b)
{
    a.insert (a.end(), b.begin(), b.end());
    return a;
}

inline std::vector<float> scaled (std::vector<float> a, float gain)
{
    for (auto& x : a)
        x *= gain;

    return a;
}

inline std::vector<float> added (std::vector<float> a, const std::vector<float>& b)
{
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        a[i] += b[i];

    return a;
}

// Prints a measured value under the current test (keeps the key numbers visible in the test log).
inline void note (const std::string& what, double value)
{
    std::cout << "    " << what << " = " << value << "\n";
}
} // namespace pstest

#define CHECK_NEAR(a, b, tol)                                                                                       \
    do {                                                                                                           \
        const double rm_a_ = (double) (a), rm_b_ = (double) (b), rm_t_ = (double) (tol);                           \
        if (! (std::abs (rm_a_ - rm_b_) <= rm_t_))                                                                 \
            pstest::reportFailure (__FILE__, __LINE__, "CHECK_NEAR(" #a ", " #b ", " #tol ")  got "                \
                                                           + pstest::show (rm_a_) + " vs " + pstest::show (rm_b_)); \
    } while (0)

#define CHECK_RANGE(v, lo, hi)                                                                                      \
    do {                                                                                                           \
        const double rm_v_ = (double) (v);                                                                         \
        if (! (rm_v_ >= (double) (lo) && rm_v_ <= (double) (hi)))                                                  \
            pstest::reportFailure (__FILE__, __LINE__, "CHECK_RANGE(" #v ", " #lo ", " #hi ")  got "               \
                                                           + pstest::show (rm_v_));                                 \
    } while (0)
