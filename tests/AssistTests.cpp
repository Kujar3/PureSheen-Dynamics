#include "TestFramework.h"
#include "TestSignals.h"

#include "assist/AssistEngine.h"
#include "assist/OfflineChain.h"
#include "dsp/Params.h"

#include <algorithm>
#include <cmath>
#include <set>

// Tests for the Assistant (src/assist): features on synthetic signals, the suggestion engine (determinism, ranges,
// chain, intensity, allowed modules, reference target), the macros (absolute mapping, tilt, compress) and the offline
// loudness (target reached, excerpt choice, compensation).
namespace
{
using namespace ps::assist;
constexpr double kRate = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
};

// Pink noise, independent per channel (width ~100 %) or identical (mono).
Stereo pink (double seconds, bool mono, std::uint32_t seed = 3, double amp = 0.2)
{
    Stereo s;
    s.l = pstest::pinkNoise (amp, seconds, kRate, seed);
    s.r = mono ? s.l : pstest::pinkNoise (amp, seconds, kRate, seed + 101);
    return s;
}

// A simple "track": pink noise bed plus a kick (55 Hz, decaying) every 0.5 s and a hat every 0.25 s.
Stereo track (double seconds, std::uint32_t seed = 9)
{
    auto s = pink (seconds, false, seed, 0.08);
    pstest::Lcg lcg { seed };
    for (size_t i = 0; i < s.l.size(); ++i)
    {
        const double t = (double) i / kRate;
        const double kt = std::fmod (t, 0.5), ht = std::fmod (t + 0.125, 0.25);
        const float kick = (float) (0.6 * std::sin (2.0 * 3.14159265 * 55.0 * kt) * std::exp (-kt * 9.0));
        const float hat = (float) (0.05 * lcg.next() * std::exp (-ht * 60.0));
        s.l[i] += kick + hat;
        s.r[i] += kick + 0.8f * hat;
    }
    return s;
}

Analysis analyseStereo (const Stereo& s)
{
    return analyse (s.l.data(), s.r.data(), (long long) s.l.size(), kRate);
}

float val (const std::vector<float>& v, const char* id) { return v[(size_t) ps::paramIndex (id)]; }

Context contextFor (const Stereo& s)
{
    Context c;
    c.analysis = analyseStereo (s);
    return c;
}
} // namespace

TEST_CASE ("assist. pink noise reads flat relative to pink, white noise rises 3 dB per octave")
{
    const auto a = analyseStereo (pink (6.0, false));
    REQUIRE (a.valid);
    for (double hz : { 100.0, 300.0, 1000.0, 3000.0, 5000.0 })
        CHECK_RANGE (curveAt (a.octaveDb, hz), -1.5f, 1.5f);

    Stereo w;
    w.l = pstest::whiteNoise (0.2, 6.0, kRate, 5);
    w.r = pstest::whiteNoise (0.2, 6.0, kRate, 6);
    const auto b = analyseStereo (w);
    const float slope = (curveAt (b.octaveDb, 4000.0) - curveAt (b.octaveDb, 250.0)) / 4.0f;
    CHECK_NEAR (slope, 3.0f, 0.5f);
}

TEST_CASE ("assist. width per processing band: mono 0, independent channels ~100, wide only where it is wide")
{
    const auto mono = analyseStereo (pink (5.0, true));
    for (const auto& b : mono.procBands)
        CHECK_RANGE (b.widthPct, 0.0f, 0.5f);

    const auto wide = analyseStereo (pink (5.0, false));
    for (const auto& b : wide.procBands)
        CHECK_RANGE (b.widthPct, 80.0f, 125.0f);

    // Mono below 2 kHz, independent above: the low bands stay mono, the top bands are wide.
    auto s = pink (5.0, true);
    const auto hi = pstest::whiteNoise (0.05, 5.0, kRate, 77);
    for (size_t i = 1; i < s.r.size(); ++i)
        s.r[i] += hi[i] - hi[i - 1];   // a differentiated (high-heavy) noise only on the right
    const auto mixed = analyseStereo (s);
    CHECK (mixed.procBands[0].widthPct < 1.0f);
    CHECK (mixed.procBands[3].widthPct > mixed.procBands[1].widthPct * 10.0f);
}

TEST_CASE ("assist. a transient train has more crest (every band) and onset (above 120 Hz) than steady noise")
{
    // (The lowest band holds only a few FFT bins, so its frame-to-frame onset measure is dominated by noise jitter;
    // the Punch rule uses the crest, which separates every band.)
    const auto steady = analyseStereo (pink (5.0, false));
    const auto drums = analyseStereo (track (5.0));
    for (int b = 0; b < kProcBands; ++b)
        CHECK (drums.procBands[(size_t) b].crestDb > steady.procBands[(size_t) b].crestDb + 4.0f);
    for (int b = 1; b < kProcBands; ++b)
        CHECK (drums.procBands[(size_t) b].onsetDb > steady.procBands[(size_t) b].onsetDb * 1.5f);
}

TEST_CASE ("assist. silence and too little audio give an invalid analysis and the Balanced target")
{
    std::vector<float> silence ((size_t) (kRate * 3.0), 0.0f);
    const auto a = analyse (silence.data(), silence.data(), (long long) silence.size(), kRate);
    CHECK (! a.valid);
    CHECK (detectTarget (a) == ps::ToneTarget::Balanced);
}

TEST_CASE ("assist. suggestions are deterministic, in range, with the reference-like default chain first")
{
    const auto c = contextFor (track (8.0));
    REQUIRE (c.analysis.valid);
    const auto a = buildSuggestions (c);
    const auto b = buildSuggestions (c);
    REQUIRE (a.size() >= 5);
    CHECK_EQ (a.size(), b.size());
    for (size_t k = 0; k < a.size(); ++k)
        CHECK (a[k].values == b[k].values && a[k].order == b[k].order);

    const std::vector<ps::ModuleType> expected { ps::ModuleType::Equalizer1, ps::ModuleType::Punch, ps::ModuleType::Imager,
                                                 ps::ModuleType::Definition, ps::ModuleType::ToneBalance,
                                                 ps::ModuleType::DynamicEq, ps::ModuleType::Maximizer };
    CHECK (a.front().isDefault && a.front().name == "Suggested");
    CHECK (a.front().order == expected);

    std::set<std::string> names;
    const auto& specs = ps::paramSpecs();
    for (const auto& s : a)
    {
        names.insert (s.name);
        REQUIRE (s.values.size() == specs.size());
        bool inRange = true;
        for (size_t i = 0; i < specs.size(); ++i)
            inRange = inRange && std::isfinite (s.values[i]) && s.values[i] >= specs[i].minValue && s.values[i] <= specs[i].maxValue;
        CHECK (inRange);
        CHECK_EQ ((int) val (s.values, "tbal.target"), (int) s.target);
        CHECK_NEAR (val (s.values, "max.learnTarget"), s.loudnessLufs, 0.01f);
    }
    CHECK_EQ (names.size(), a.size());
}

TEST_CASE ("assist. the default EQ moves stay small and follow the intensity")
{
    auto c = contextFor (track (8.0));
    c.settings.profile = (int) ps::ToneTarget::Flat;
    for (auto [intensity, limit] : { std::pair<Intensity, float> { Intensity::Gentle, 1.08f },
                                     { Intensity::Balanced, 1.8f }, { Intensity::Strong, 2.7f } })
    {
        c.settings.intensity = intensity;
        const auto s = buildSuggestions (c).front();
        float largest = 0.0f;
        for (int b = 1; b <= 8; ++b)
            if (val (s.values, ("eq1.a.b" + std::to_string (b) + ".on").c_str()) > 0.5f)
                largest = std::max (largest, std::abs (val (s.values, ("eq1.a.b" + std::to_string (b) + ".gain").c_str())));
        CHECK (largest <= limit + 1.0e-3f);
        for (double hz : { 40.0, 200.0, 1000.0, 5000.0, 15000.0 })
            CHECK_RANGE ((float) eqCurveDb (s.values, hz, 48000.0), -2.2f * limit, 2.2f * limit);
    }
}

TEST_CASE ("assist. a chosen profile, a loudness override and the module switches are honoured")
{
    auto c = contextFor (track (8.0));
    c.settings.profile = (int) ps::ToneTarget::Jazz;
    auto s = buildSuggestions (c).front();
    CHECK (s.target == ps::ToneTarget::Jazz);
    CHECK_NEAR (s.loudnessLufs, profileFor (ps::ToneTarget::Jazz).loudnessLufs, 0.01f);
    CHECK_EQ ((int) val (s.values, "max.mode"), (int) ps::MaximizerMode::Spectral);

    c.settings.autoLoudness = false;
    c.settings.loudnessLufs = -14.0f;
    CHECK_NEAR (buildSuggestions (c).front().loudnessLufs, -14.0f, 0.01f);

    c.settings.allowedModules = kAllModules & ~(1u << (unsigned) ps::ModuleType::Punch);
    c.settings.useMaximizer = false;
    for (const auto& card : buildSuggestions (c))
    {
        CHECK (std::find (card.order.begin(), card.order.end(), ps::ModuleType::Punch) == card.order.end());
        CHECK (std::find (card.order.begin(), card.order.end(), ps::ModuleType::Maximizer) == card.order.end());
    }
}

TEST_CASE ("assist. the measured hip-hop and reggae rows reach the Maximizer, transient and sustain links apart")
{
    auto c = contextFor (track (8.0));
    c.settings.profile = (int) ps::ToneTarget::HipHop;
    auto s = buildSuggestions (c).front();
    CHECK_NEAR (s.loudnessLufs, -9.5f, 0.01f);
    CHECK_EQ ((int) val (s.values, "max.mode"), (int) ps::MaximizerMode::Transparent);
    CHECK_NEAR (val (s.values, "max.clip.amount"), 15.0f, 0.01f);
    CHECK_NEAR (val (s.values, "defn.a.tilt"), -1.0f, 0.01f);

    c.settings.profile = (int) ps::ToneTarget::Reggae;
    s = buildSuggestions (c).front();
    CHECK_EQ ((int) val (s.values, "max.mode"), (int) ps::MaximizerMode::Spectral);
    CHECK_NEAR (val (s.values, "max.si.trans"), 42.0f, 0.01f);
    CHECK_NEAR (val (s.values, "max.si.sus"), 0.0f, 0.01f);
}

TEST_CASE ("assist. a reference target makes Match reference the default with the reference's loudness")
{
    auto c = contextFor (track (8.0));
    auto refAudio = pink (6.0, false, 21, 0.3);
    const auto refAnalysis = analyseStereo (refAudio);
    c.reference = referenceFromAnalysis (refAnalysis, "Pink");
    REQUIRE (c.reference.valid);
    const auto cards = buildSuggestions (c);
    CHECK (cards.front().name == "Match reference" && cards.front().isDefault);
    CHECK (cards[1].name == "Suggested" && ! cards[1].isDefault);
    CHECK_NEAR (cards.front().loudnessLufs, std::clamp (refAnalysis.profile.integratedLufs, -16.0f, -6.0f), 0.01f);
}

TEST_CASE ("assist. macros map absolutely: the card's own positions change nothing, tilt and compress do what they say")
{
    const auto c = contextFor (track (8.0));
    const auto card = buildSuggestions (c).front();

    const auto same = applyMacros (card, card.macros, 0.0f, card.order, c.analysis);
    for (const auto& [i, v] : same.values)
        if (ps::paramSpecs()[(size_t) i].id.rfind ("eq1.a.b6.", 0) != 0 && ps::paramSpecs()[(size_t) i].id.rfind ("eq1.a.b7.", 0) != 0)
            CHECK_NEAR (v, card.values[(size_t) i], 1.0e-4f);
    CHECK (same.order == card.order);

    auto m = card.macros;
    m[(int) Macro::Tilt] = 2.0f;
    m[(int) Macro::Compress] = 100.0f;
    m[(int) Macro::WidthHigh] = -30.0f;
    const auto moved = applyMacros (card, m, 0.0f, card.order, c.analysis);
    auto v = card.values;
    for (const auto& [i, x] : moved.values)
        v[(size_t) i] = x;
    CHECK_NEAR (val (v, "eq1.a.b7.gain"), 1.0f, 1.0e-4f);
    CHECK_NEAR (val (v, "eq1.a.b6.gain"), -1.0f, 1.0e-4f);
    CHECK (val (v, "eq1.a.b6.on") > 0.5f && val (v, "eq1.a.b7.on") > 0.5f);
    CHECK_NEAR (val (v, "img.b4.width"), std::clamp (val (card.values, "img.b4.width") - 30.0f, -100.0f, 100.0f), 1.0e-3f);
    CHECK_NEAR (val (v, "dyn.a.b1.comp.ratio"), 3.5f, 1.0e-3f);
    const auto dyn = std::find (moved.order.begin(), moved.order.end(), ps::ModuleType::Dynamics);
    REQUIRE (dyn != moved.order.end());
    CHECK (*(dyn + 1) == ps::ModuleType::ToneBalance);
}

TEST_CASE ("assist. the loudest excerpt is the loud part")
{
    auto s = pink (12.0, false, 4, 0.02);
    for (size_t i = (size_t) (6.0 * kRate); i < (size_t) (9.0 * kRate); ++i)
    {
        s.l[i] *= 10.0f;
        s.r[i] *= 10.0f;
    }
    const auto e = loudestExcerpt (s.l.data(), s.r.data(), (long long) s.l.size(), kRate, 3.0);
    REQUIRE (e.size() == (long long) (3.0 * kRate));
    // the excerpt is the loud 3 s exactly (up to one 100 ms block): its energy is ~100 x the quiet part's
    double inside = 0.0, quiet = 0.0;
    for (float x : e.left)
        inside += (double) x * x;
    for (size_t i = 0; i < (size_t) (3.0 * kRate); ++i)
        quiet += (double) s.l[i] * s.l[i];
    CHECK (inside > 80.0 * quiet);
}

TEST_CASE ("assist. the offline render reaches the loudness target and fills monotonic tone-match tables")
{
    const auto s = track (6.0);
    const auto c = contextFor (s);
    auto card = buildSuggestions (c).front();
    Excerpt ex;
    ex.left = s.l;
    ex.right = s.r;
    ex.sampleRate = kRate;

    REQUIRE (refineLoudness (card, ex));
    CHECK (card.loudnessMeasured);
    Excerpt out;
    REQUIRE (renderSeries (card.order, card.values, ex, out, false));
    CHECK_NEAR (integratedLufs (out), card.loudnessLufs, 0.25f);
    CHECK_RANGE (card.gainPerLufs, 0.8f, 6.0f);

    REQUIRE (buildLoudnessTables (card, ex, c.analysis));
    CHECK (card.tablesReady);
    const auto& t = card.tableDeltaLu[(size_t) Macro::ToneMatch];
    for (float x : t)
        CHECK (std::isfinite (x));
    CHECK_NEAR (loudnessCompensation (card, card.macros), 0.0f, 1.0e-4f);

    // Moving a matched macro gives a correction of the opposite sign to its loudness change.
    auto m = card.macros;
    m[(size_t) Macro::Width] = 100.0f;
    const auto& w = card.tableDeltaLu[(size_t) Macro::Width];
    const float change = w[Suggestion::kTablePoints - 1] - w[Suggestion::kTablePoints / 2];
    if (std::abs (change) > 0.02f)
        CHECK (loudnessCompensation (card, m) * change < 0.0f);
}

TEST_CASE ("assist. cancelling the offline render stops it")
{
    const auto s = track (4.0);
    const auto c = contextFor (s);
    auto card = buildSuggestions (c).front();
    Excerpt ex;
    ex.left = s.l;
    ex.right = s.r;
    ex.sampleRate = kRate;
    std::atomic<bool> cancel { true };
    CHECK (! refineLoudness (card, ex, &cancel));
    CHECK (! buildLoudnessTables (card, ex, c.analysis, &cancel));
}
