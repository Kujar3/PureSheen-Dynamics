#include "assist/AssistEngine.h"

#include <algorithm>
#include <cmath>

// Tone-target profiles and the target detector of the Assistant.
//
// Profiles: what a tone target brings besides its curve (loudness target and Maximizer settings, Definition tilt).
// Five rows were measured on the reference's assistant (country, rock, jazz, pop and dance targets on seven test
// tracks: the three tracks it gave the rock target got identical Maximizer settings, so these follow the target, not
// the track). The other targets take the row of their family with our own loudness choice (louder for club / modern
// pop styles, quieter for acoustic, cinematic and ambient styles).
//
// Detector: nearest labelled example in a small, scaled feature space (detectorFeatures). The examples are our
// analysis of seven test tracks, labelled with the target the reference's assistant chose for them. Seven examples
// cannot generalise far (leave-one-out: 2 of 7), so the table is meant to grow with every new labelled render; the
// user can always pick the target by hand.
namespace ps::assist
{
namespace
{
using T = ToneTarget;
using M = MaximizerMode;

// loudness, mode, character, upward, clip on, clip mode, clip amount, stereo independence, Definition tilt
constexpr Profile kCountry { -9.5f, M::Transparent, 2.8f, 2.2f, true, 1, 5.0f, 100.0f, 0.0f };     // measured
constexpr Profile kRock { -8.5f, M::Transparent, 2.0f, 1.6f, true, 1, 10.0f, 100.0f, -0.5f };      // measured
constexpr Profile kJazz { -12.5f, M::Spectral, 4.2f, 2.8f, false, 1, 0.0f, 100.0f, -3.0f };       // measured
constexpr Profile kPop { -8.5f, M::Spectral, 2.5f, 2.6f, true, 0, 10.0f, 50.0f, 0.0f };          // measured
constexpr Profile kEdm { -7.5f, M::Spectral, 2.0f, 2.7f, false, 1, 0.0f, 50.0f, 0.0f };          // measured

constexpr Profile with (Profile p, float lufs) noexcept
{
    p.loudnessLufs = lufs;
    return p;
}

struct Row
{
    const char* name;
    Profile profile;
};

// Index = (int) ToneTarget.
const Row kRows[(int) T::Count] {
    { "Balanced", with (kPop, -10.0f) },
    { "Cinematic", with (kJazz, -13.0f) },
    { "Country", kCountry },
    { "EDM", kEdm },
    { "Folk", with (kCountry, -10.5f) },
    { "Hip-Hop", with (kEdm, -8.5f) },
    { "Jazz", kJazz },
    { "Pop", kPop },
    { "Reggae", with (kPop, -9.5f) },
    { "R&B / Soul", with (kPop, -9.5f) },
    { "Rock", kRock },
    { "Alt Rock", kRock },
    { "Ambient", with (kJazz, -13.0f) },
    { "Classic Hip-Hop", with (kEdm, -9.5f) },
    { "Classic Rock", with (kRock, -9.5f) },
    { "Dance Pop", with (kPop, -8.0f) },
    { "Drum & Bass", kEdm },
    { "Dubstep", kEdm },
    { "Electropop", with (kPop, -8.0f) },
    { "Future Bass", kEdm },
    { "House", kEdm },
    { "Hyperpop", with (kEdm, -7.0f) },
    { "Indie Pop", with (kPop, -9.0f) },
    { "Indie Rock", with (kRock, -9.0f) },
    { "K-Pop / J-Pop", with (kPop, -8.0f) },
    { "Latin Pop", with (kPop, -8.0f) },
    { "Lo-Fi", with (kCountry, -11.0f) },
    { "Metal", with (kRock, -8.0f) },
    { "Pop Country", with (kCountry, -9.0f) },
    { "Post-Rock", with (kRock, -10.0f) },
    { "Punk Rock", with (kRock, -8.0f) },
    { "Reggaeton", with (kEdm, -8.0f) },
    { "Techno", kEdm },
    { "Trap", with (kEdm, -8.0f) },
    { "Vocal Jazz", with (kJazz, -12.0f) },
    { "Flat", with (kCountry, -11.0f) },
};

// detectorFeatures scales: dB features / 6, correlation and onsets / 0.3, PLR / 3.
constexpr std::array<float, 8> kScale { 6.0f, 6.0f, 6.0f, 6.0f, 0.3f, 0.3f, 6.0f, 3.0f };

struct Example
{
    std::array<float, 8> features;
    ToneTarget target;
};

// Our analysis of the test tracks (full 50 s slices), labelled with the reference assistant's choice.
const Example kExamples[] {
    { { -8.614f, -21.853f, -28.069f, -12.073f, 0.959f, 0.426f, 23.176f, 13.477f }, T::Country },   // acoustic
    { { -3.544f, -7.255f, -16.660f, -9.391f, 0.956f, 0.405f, 4.395f, 10.093f }, T::Rock },         // pop-rock
    { { 1.290f, -10.218f, -17.568f, -11.605f, 0.982f, 0.965f, 16.386f, 11.805f }, T::Rock },       // trap
    { { -3.968f, -9.156f, -25.510f, -2.444f, 0.332f, 0.280f, 5.319f, 10.931f }, T::Jazz },         // orchestral
    { { -2.511f, -26.013f, -28.621f, -12.345f, 0.999f, 0.677f, 17.683f, 12.010f }, T::Pop },       // jazz trio
    { { 1.052f, -15.801f, -19.535f, -20.662f, 0.998f, 1.137f, 14.168f, 9.936f }, T::Edm },         // tech house
    { { -4.009f, -4.062f, -15.516f, -5.084f, 0.701f, 0.377f, 3.480f, 10.278f }, T::Rock },         // metal
};
} // namespace

const Profile& profileFor (ToneTarget t) noexcept
{
    const int i = (int) t;
    return kRows[i >= 0 && i < (int) T::Count ? i : 0].profile;
}

const char* targetName (ToneTarget t) noexcept
{
    const int i = (int) t;
    return kRows[i >= 0 && i < (int) T::Count ? i : 0].name;
}

std::array<float, 8> detectorFeatures (const Analysis& a)
{
    const auto& b = a.procBands;
    const auto& p = a.profile;
    return { b[0].levelDb - b[1].levelDb,
             b[2].levelDb - b[1].levelDb,
             b[3].levelDb - b[1].levelDb,
             (float) (10.0 * std::log10 (std::max (p.widthPct, 0.05f) / 100.0)),
             p.lowEndCorrelation,
             (b[1].onsetDb + b[2].onsetDb + b[3].onsetDb) / 3.0f,
             b[2].crestDb,
             p.plrDb };
}

ToneTarget detectTarget (const Analysis& a, float* distance)
{
    const auto f = detectorFeatures (a);
    float best = 1.0e30f;
    ToneTarget target = T::Balanced;

    for (const auto& e : kExamples)
    {
        float d = 0.0f;
        for (size_t i = 0; i < f.size(); ++i)
        {
            const float x = std::isfinite (f[i]) ? (f[i] - e.features[i]) / kScale[i] : 0.0f;
            d += x * x;
        }
        if (d < best)
        {
            best = d;
            target = e.target;
        }
    }

    if (distance != nullptr)
        *distance = std::sqrt (best);
    return a.valid ? target : T::Balanced;
}
} // namespace ps::assist
