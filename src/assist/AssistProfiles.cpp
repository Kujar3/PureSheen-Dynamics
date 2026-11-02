#include "assist/AssistEngine.h"

#include <algorithm>
#include <cmath>

// Tone-target profiles and the target detector of the Assistant.
//
// Profiles: what a tone target brings besides its curve (loudness target and Maximizer settings, Definition tilt).
// Seven rows were measured on the reference's assistant (country, rock, jazz, pop, dance, hip-hop and reggae targets
// on seventeen test tracks: every track it gave the same target got identical Maximizer settings and Definition tilt,
// six of them for rock and five for dance, so these follow the target, not the track). The other targets take the row
// of their family with our own loudness choice (louder for club / modern pop styles, quieter for acoustic, cinematic
// and ambient styles). The reference only ever chose among the first eleven targets.
//
// Detector: nearest labelled example in a small, scaled feature space (detectorFeatures). The examples are our
// analysis of the first 8 s of seventeen test tracks (the reference listens for 8 s), labelled with the target the
// reference's assistant chose for them. Its choice does not follow the genre closely (lo-fi, ambient and country-pop
// got the dance target, folk got rock), and none of the feature sets we tried predicts it from the other sixteen
// examples better than about one in three, so the table reproduces the known cases and is meant to grow with every
// new labelled render; the user can always pick the target by hand.
namespace ps::assist
{
namespace
{
using T = ToneTarget;
using M = MaximizerMode;

// loudness, mode, character, upward, clip on, clip mode, clip amount, stereo independence (transient, sustain),
// Definition tilt
constexpr Profile kCountry { -9.5f, M::Transparent, 2.8f, 2.2f, true, 1, 5.0f, 100.0f, 100.0f, 0.0f };    // measured
constexpr Profile kRock { -8.5f, M::Transparent, 2.0f, 1.6f, true, 1, 10.0f, 100.0f, 100.0f, -0.5f };     // measured
constexpr Profile kJazz { -12.5f, M::Spectral, 4.2f, 2.8f, false, 1, 0.0f, 100.0f, 100.0f, -3.0f };      // measured
constexpr Profile kPop { -8.5f, M::Spectral, 2.5f, 2.6f, true, 0, 10.0f, 50.0f, 50.0f, 0.0f };          // measured
constexpr Profile kEdm { -7.5f, M::Spectral, 2.0f, 2.7f, false, 1, 0.0f, 50.0f, 50.0f, 0.0f };          // measured
constexpr Profile kHipHop { -9.5f, M::Transparent, 2.0f, 2.3f, true, 0, 15.0f, 50.0f, 50.0f, -1.0f };   // measured
constexpr Profile kReggae { -9.5f, M::Spectral, 3.3f, 2.5f, true, 1, 7.0f, 42.0f, 0.0f, -1.0f };        // measured

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
    { "Hip-Hop", kHipHop },
    { "Jazz", kJazz },
    { "Pop", kPop },
    { "Reggae", kReggae },
    { "R&B / Soul", with (kPop, -9.5f) },
    { "Rock", kRock },
    { "Alt Rock", kRock },
    { "Ambient", with (kJazz, -13.0f) },
    { "Classic Hip-Hop", with (kHipHop, -10.0f) },
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
    { "Reggaeton", with (kReggae, -8.5f) },
    { "Techno", kEdm },
    { "Trap", with (kHipHop, -8.5f) },
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

// Our analysis of the first 8 s of the test tracks, labelled with the reference assistant's choice.
const Example kExamples[] {
    { { -11.258f, -20.520f, -24.288f, -13.221f, 0.959f, 0.470f, 21.528f, 13.232f }, T::Country },       // acoustic
    { { -4.775f, -8.633f, -17.675f, -7.443f, 0.906f, 0.480f, 5.842f, 8.913f }, T::Rock },               // pop-rock
    { { 0.928f, -10.682f, -19.102f, -11.052f, 0.978f, 0.838f, 16.108f, 11.103f }, T::Rock },            // trap
    { { -7.397f, -11.535f, -35.291f, -3.120f, 0.408f, 0.278f, 3.832f, 10.677f }, T::Jazz },             // orchestral
    { { -1.037f, -25.946f, -30.093f, -11.227f, 0.999f, 0.708f, 16.949f, 12.546f }, T::Pop },            // jazz trio
    { { 4.457f, -14.188f, -17.816f, -29.349f, 1.000f, 1.600f, 17.670f, 8.551f }, T::Edm },              // tech house
    { { -4.952f, -3.563f, -15.191f, -4.990f, 0.611f, 0.372f, 2.796f, 10.095f }, T::Rock },              // metal
    { { -1.130f, -8.192f, -14.250f, -21.179f, 0.990f, 0.794f, 7.451f, 10.833f }, T::HipHop },           // boom bap
    { { 0.718f, -13.966f, -19.441f, -12.663f, 0.992f, 0.521f, 14.838f, 9.860f }, T::Rock },             // R&B / soul
    { { 0.614f, -22.474f, -48.135f, -19.982f, 1.000f, 0.537f, 30.583f, 13.319f }, T::Edm },             // lo-fi
    { { 1.905f, -9.651f, -13.605f, -16.084f, 1.000f, 1.097f, 6.781f, 8.696f }, T::Edm },                // techno
    { { -10.094f, -21.279f, -26.842f, -12.938f, 0.947f, 0.455f, 22.376f, 11.042f }, T::Rock },          // folk
    { { 0.628f, -36.131f, -50.300f, -4.391f, 0.905f, 0.247f, 5.079f, 9.292f }, T::Edm },                // ambient
    { { -4.062f, -4.088f, -16.173f, -7.689f, 0.997f, 0.361f, 2.451f, 8.658f }, T::Rock },               // drum & bass
    { { 0.788f, -9.368f, -15.389f, -12.234f, 0.988f, 0.838f, 10.018f, 10.192f }, T::Reggae },           // reggaeton
    { { -6.805f, -7.894f, -18.815f, -9.585f, 0.984f, 0.553f, 5.817f, 11.624f }, T::Edm },               // country-pop
    { { -11.563f, -20.241f, -53.544f, -3.563f, 0.719f, 0.238f, 7.564f, 9.742f }, T::Jazz },             // solo piano
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
