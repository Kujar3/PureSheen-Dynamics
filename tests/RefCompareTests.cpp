#include "TestFramework.h"
#include "TestSignals.h"

#include "core/RefCompare.h"

#include <algorithm>
#include <string>

// Tests for RefCompare.h (Track Referencing, ported from RefMatch): matching gain, band / curve comparison and the
// plain-language hints.

namespace
{
using ps::Band;
using ps::Severity;
using ps::Topic;

// A plausible, valid profile of a finished master (synthetic, so each test can change exactly one thing).
ps::TrackProfile makeProfile()
{
    ps::TrackProfile p;
    p.valid = true;
    p.analysedSeconds = 30.0;
    p.integratedLufs = -9.0f;
    p.shortTermLufs = -8.5f;
    p.momentaryLufs = -8.0f;
    p.loudnessRangeLu = 5.0f;
    p.truePeakDb = -1.2f;
    p.samplePeakDb = -1.3f;
    p.rmsDb = -11.0f;
    p.crestDb = 9.7f;
    p.plrDb = p.samplePeakDb - p.integratedLufs;   // 7.7, as TrackAnalyser computes it
    p.spectralLoudnessDb = -9.2f;

    const float levels[] = { -18.0f, -14.0f, -19.0f, -20.0f, -24.0f, -29.0f };
    const float widths[] = { 2.0f, 8.0f, 30.0f, 40.0f, 55.0f, 70.0f };

    for (size_t b = 0; b < (size_t) ps::kNumBands; ++b)
    {
        p.bands[b].levelDb = levels[b];
        p.bands[b].widthPct = widths[b];
        p.bands[b].correlation = 1.0f - widths[b] / 150.0f;
    }

    for (int k = 0; k < ps::kCurvePoints; ++k)
        p.curveDb[(size_t) k] = (float) (-60.0 - 4.5 * std::log2 (ps::curveFrequency (k) / 1000.0));

    p.correlation = 0.6f;
    p.widthPct = 40.0f;
    p.lowEndCorrelation = 0.97f;
    p.isMono = false;
    return p;
}

const ps::Hint* findHint (const ps::Comparison& c, Topic topic, int band = -2)
{
    for (const auto& h : c.hints)
        if (h.topic == topic && (band == -2 || h.band == band))
            return &h;

    return nullptr;
}

bool contains (const std::string& text, const std::string& part) { return text.find (part) != std::string::npos; }

bool isPrintableAscii (const std::string& s)
{
    return std::all_of (s.begin(), s.end(), [] (char ch) { return ch >= 32 && ch < 127; });
}

// The invariants every comparison must satisfy.
void checkHintList (const ps::Comparison& c)
{
    REQUIRE (! c.hints.empty());
    CHECK ((int) c.hints.size() <= ps::kMaxHints);

    bool pastSetup = false;
    for (size_t i = 0; i < c.hints.size(); ++i)
    {
        const auto& h = c.hints[i];
        CHECK (! h.title.empty());
        CHECK (! h.detail.empty());
        CHECK (isPrintableAscii (h.title));
        CHECK (isPrintableAscii (h.detail));
        CHECK (h.title.size() < 70);

        // Setup hints lead, then non-increasing severity.
        if (h.topic != Topic::Setup)
            pastSetup = true;
        else
            CHECK (! pastSetup);

        if (i > 0 && h.topic != Topic::Setup && c.hints[i - 1].topic != Topic::Setup)
            CHECK ((int) c.hints[i - 1].severity >= (int) h.severity);

        // Tone problems are always about one band; otherwise band is -1 or a valid index.
        if (h.topic == Topic::Tone && h.severity != Severity::Good)
            CHECK (h.band >= 0);
        CHECK (h.band >= -1 && h.band < ps::kNumBands);
    }
}
} // namespace

//==============================================================================
TEST_CASE ("ref. referenceMatchGainDb: mix minus reference, clamped, with fallbacks")
{
    CHECK_EQ (ps::referenceMatchGainDb (-14.0f, -8.0f), -6.0f);
    CHECK_EQ (ps::referenceMatchGainDb (-8.0f, -14.0f), 6.0f);
    CHECK_EQ (ps::referenceMatchGainDb (-70.0f, -8.0f), ps::kMinMatchGainDb);
    CHECK_EQ (ps::referenceMatchGainDb (-8.0f, -40.0f), ps::kMaxMatchGainDb);
    CHECK_EQ (ps::referenceMatchGainDb (-14.0f, ps::kNoValue), 0.0f);
    CHECK_EQ (ps::referenceMatchGainDb (ps::kNoValue, ps::kNoValue), 0.0f);
    CHECK_EQ (ps::referenceMatchGainDb (ps::kNoValue, -8.0f), ps::kFallbackMixLufs - -8.0f);
    CHECK_EQ (ps::referenceMatchGainDb (ps::kNoValue, -20.0f), 6.0f);
}

TEST_CASE ("ref. bandSeverity thresholds")
{
    CHECK (ps::bandSeverity (0.0f) == Severity::Good);
    CHECK (ps::bandSeverity (1.4f) == Severity::Good);
    CHECK (ps::bandSeverity (-1.4f) == Severity::Good);
    CHECK (ps::bandSeverity (1.5f) == Severity::Minor);
    CHECK (ps::bandSeverity (-2.9f) == Severity::Minor);
    CHECK (ps::bandSeverity (3.0f) == Severity::Major);
    CHECK (ps::bandSeverity (-5.0f) == Severity::Major);
}

TEST_CASE ("ref. bandSeverity with hold: steps only past the threshold +- kBandHysteresisDb")
{
    using ps::bandSeverity;
    constexpr float h = ps::kBandHysteresisDb;
    static_assert (h > 0.2f && h < 0.6f, "hysteresis margin out of the tested range");

    // From Good: up to Minor only at 1.5 + h, straight to Major only at 3 + h.
    CHECK (bandSeverity (1.6f, Severity::Good) == Severity::Good);
    CHECK (bandSeverity (-(1.5f + h) + 0.01f, Severity::Good) == Severity::Good);
    CHECK (bandSeverity (1.5f + h, Severity::Good) == Severity::Minor);
    CHECK (bandSeverity (3.2f, Severity::Good) == Severity::Minor);
    CHECK (bandSeverity (-(3.0f + h), Severity::Good) == Severity::Major);

    // Minor holds between 1.5 - h and 3 + h.
    CHECK (bandSeverity (1.5f - h + 0.01f, Severity::Minor) == Severity::Minor);
    CHECK (bandSeverity (1.5f - h - 0.01f, Severity::Minor) == Severity::Good);
    CHECK (bandSeverity (-3.2f, Severity::Minor) == Severity::Minor);
    CHECK (bandSeverity (3.0f + h, Severity::Minor) == Severity::Major);

    // Major holds down to 3 - h, then drops one or two levels.
    CHECK (bandSeverity (-2.8f, Severity::Major) == Severity::Major);
    CHECK (bandSeverity (3.0f - h - 0.01f, Severity::Major) == Severity::Minor);
    CHECK (bandSeverity (0.5f, Severity::Major) == Severity::Good);

    // Info counts as Good; NaN is Good.
    CHECK (bandSeverity (1.7f, Severity::Info) == Severity::Good);
    CHECK (bandSeverity (std::nanf (""), Severity::Major) == Severity::Good);
}

TEST_CASE ("ref. compareProfiles with the previous comparison holds band severities and tone hints")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();

    // Bass at -1.7 dB: Minor from scratch.
    mix.bands[(size_t) Band::Low].levelDb -= 1.7f;
    const auto first = ps::compareProfiles (mix, ref, nullptr);
    CHECK (first.bands[(size_t) Band::Low].severity == Severity::Minor);
    REQUIRE (findHint (first, Topic::Tone, (int) Band::Low) != nullptr);

    // The same as the two-argument version when there is nothing to hold.
    const auto plain = ps::compareProfiles (mix, ref);
    CHECK (plain.bands[(size_t) Band::Low].severity == first.bands[(size_t) Band::Low].severity);
    CHECK_EQ (plain.hints.size(), first.hints.size());

    // It drifts to -1.3 dB: without hold it would flip to Good (and the hint would vanish); with hold it stays Minor.
    mix.bands[(size_t) Band::Low].levelDb = ref.bands[(size_t) Band::Low].levelDb - 1.3f;
    CHECK (ps::compareProfiles (mix, ref).bands[(size_t) Band::Low].severity == Severity::Good);
    auto held = ps::compareProfiles (mix, ref, &first);
    checkHintList (held);
    CHECK (held.bands[(size_t) Band::Low].severity == Severity::Minor);
    const auto* h = findHint (held, Topic::Tone, (int) Band::Low);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Minor);
    CHECK (contains (h->title, "-1.3 dB"));   // the title always shows the current number

    // Further down to -1.0 dB (below 1.5 - 0.4): Good, no tone hint, the "close to your reference" hint returns.
    mix.bands[(size_t) Band::Low].levelDb = ref.bands[(size_t) Band::Low].levelDb - 1.0f;
    held = ps::compareProfiles (mix, ref, &held);   // previous may be the assigned-to object
    CHECK (held.bands[(size_t) Band::Low].severity == Severity::Good);
    CHECK (findHint (held, Topic::Tone, (int) Band::Low) == nullptr);
    CHECK (held.hints.back().severity == Severity::Good);

    // Back up to -1.7: stays Good until -1.9.
    mix.bands[(size_t) Band::Low].levelDb = ref.bands[(size_t) Band::Low].levelDb - 1.7f;
    held = ps::compareProfiles (mix, ref, &held);
    CHECK (held.bands[(size_t) Band::Low].severity == Severity::Good);
    CHECK (findHint (held, Topic::Tone, (int) Band::Low) == nullptr);

    // A previous comparison that was not valid (no reference yet) holds nothing.
    const auto noRef = ps::compareProfiles (mix, ps::TrackProfile {});
    CHECK (ps::compareProfiles (mix, ref, &noRef).bands[(size_t) Band::Low].severity == Severity::Minor);
}

TEST_CASE ("ref. formatSigned / formatValue")
{
    CHECK_EQ (ps::formatSigned (3.14f), std::string ("+3.1"));
    CHECK_EQ (ps::formatSigned (-0.44f), std::string ("-0.4"));
    CHECK_EQ (ps::formatSigned (0.0f), std::string ("0.0"));
    CHECK_EQ (ps::formatSigned (-0.0f), std::string ("0.0"));
    CHECK_EQ (ps::formatSigned (-0.04f), std::string ("0.0"));
    CHECK_EQ (ps::formatSigned (0.04f), std::string ("0.0"));
    CHECK_EQ (ps::formatSigned (2.0f, 0), std::string ("+2"));
    CHECK_EQ (ps::formatSigned (-0.4f, 0), std::string ("0"));
    CHECK_EQ (ps::formatSigned (-1.234f, 2), std::string ("-1.23"));
    CHECK_EQ (ps::formatSigned (0.5f, 9), std::string ("+0.500"));
    CHECK_EQ (ps::formatSigned (ps::kNoValue), std::string ("--"));

    CHECK_EQ (ps::formatValue (-14.24f, 1, "LUFS"), std::string ("-14.2 LUFS"));
    CHECK_EQ (ps::formatValue (-0.01f, 1, "dB"), std::string ("0.0 dB"));
    CHECK_EQ (ps::formatValue (0.3f, 2, nullptr), std::string ("0.30"));
    CHECK_EQ (ps::formatValue (7.0f, 0, ""), std::string ("7"));
    CHECK_EQ (ps::formatValue (ps::kNoValue, 1, "LUFS"), std::string ("--"));
}

//==============================================================================
TEST_CASE ("ref. Equal profiles: every band Good, one Good hint")
{
    const auto p = makeProfile();
    const auto c = ps::compareProfiles (p, p);

    CHECK (c.valid());
    CHECK_EQ (c.loudnessDiffLu, 0.0f);
    CHECK_EQ (c.truePeakDiffDb, 0.0f);
    CHECK_EQ (c.plrDiffDb, 0.0f);
    CHECK_EQ (c.lraDiffLu, 0.0f);
    CHECK_EQ (c.normalisationOffsetDb, 0.0f);

    for (const auto& b : c.bands)
    {
        CHECK (b.valid);
        CHECK (b.severity == Severity::Good);
        CHECK_EQ (b.levelDiffDb, 0.0f);
        CHECK_EQ (b.widthDiffPct, 0.0f);
    }

    for (float d : c.curveDiffDb)
        CHECK_EQ (d, 0.0f);

    checkHintList (c);
    REQUIRE (c.hints.size() == 1);
    CHECK (c.hints[0].severity == Severity::Good);
    pstest::note ("good hint: " + c.hints[0].title, 0);
}

TEST_CASE ("ref. Band differences are loudness-normalised")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();

    // The same mix 6 dB quieter everywhere: no tonal difference at all.
    for (auto& b : mix.bands)
        b.levelDb -= 6.0f;
    for (auto& v : mix.curveDb)
        v -= 6.0f;
    mix.spectralLoudnessDb -= 6.0f;

    const auto c = ps::compareProfiles (mix, ref);
    CHECK_NEAR (c.normalisationOffsetDb, 6.0, 1.0e-5);
    for (const auto& b : c.bands)
    {
        CHECK_NEAR (b.levelDiffDb, 0.0, 1.0e-5);
        CHECK (b.severity == Severity::Good);
    }
    for (float d : c.curveDiffDb)
        CHECK_NEAR (d, 0.0, 1.0e-4);

    // Unknown curve points stay unknown.
    mix.curveDb[10] = ps::kNoValue;
    CHECK_EQ (ps::compareProfiles (mix, ref).curveDiffDb[10], ps::kNoValue);
}

TEST_CASE ("ref. Low mids +3.5 dB -> Major tone hint for LowMid")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();
    mix.bands[(size_t) Band::LowMid].levelDb += 3.5f;

    const auto c = ps::compareProfiles (mix, ref);
    checkHintList (c);

    CHECK_NEAR (c.bands[(size_t) Band::LowMid].levelDiffDb, 3.5, 1.0e-5);
    CHECK (c.bands[(size_t) Band::LowMid].severity == Severity::Major);

    const auto* h = findHint (c, Topic::Tone, (int) Band::LowMid);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Major);
    CHECK_NEAR (h->amount, 3.5, 1.0e-5);
    CHECK (contains (h->title, "Low mids"));
    CHECK (contains (h->title, "+3.5 dB"));
    CHECK (contains (h->detail, "250-400 Hz"));
    CHECK (contains (h->detail, "compared with your reference") || contains (h->detail, "Compared with your reference"));
    CHECK (c.hints[0].topic == Topic::Tone);
    CHECK (findHint (c, Topic::Tone, (int) Band::Air) == nullptr);
    pstest::note ("tone hint: " + h->title, h->amount);
}

TEST_CASE ("ref. Air -2 dB -> Minor tone hint")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();
    mix.bands[(size_t) Band::Air].levelDb -= 2.0f;

    const auto c = ps::compareProfiles (mix, ref);
    checkHintList (c);

    const auto* h = findHint (c, Topic::Tone, (int) Band::Air);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Minor);
    CHECK_EQ (h->title, std::string ("Air -2.0 dB vs reference"));
    CHECK (contains (h->detail, "less air"));
    CHECK (findHint (c, Topic::Tone, (int) Band::Mid) == nullptr);

    // Sub bass advice names the range, the module and a size.
    for (float d : { -2.4f, 2.4f })
    {
        auto subMix = makeProfile();
        subMix.bands[(size_t) Band::Sub].levelDb += d;
        const auto sc = ps::compareProfiles (subMix, ref);
        const auto* sub = findHint (sc, Topic::Tone, (int) Band::Sub);
        REQUIRE (sub != nullptr);
        CHECK (contains (sub->detail, "40-60 Hz"));
        CHECK (contains (sub->detail, "EQ 1"));
        CHECK (contains (sub->detail, "1-2 dB"));
    }
    CHECK (std::none_of (c.hints.begin(), c.hints.end(), [] (const ps::Hint& x) { return x.severity == Severity::Good; }));
}

TEST_CASE ("ref. True peak +0.5 dBTP -> Major TruePeak hint first")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();
    mix.truePeakDb = 0.5f;
    mix.bands[(size_t) Band::LowMid].levelDb += 3.5f;   // another Major problem; the true peak still leads

    const auto c = ps::compareProfiles (mix, ref);
    checkHintList (c);

    REQUIRE (c.hints.size() >= 2);
    CHECK (c.hints[0].topic == Topic::TruePeak);
    CHECK (c.hints[0].severity == Severity::Major);
    CHECK_EQ (c.hints[0].title, std::string ("True peak +0.5 dBTP - may distort when streamed"));
    CHECK_NEAR (c.hints[0].amount, 1.5, 1.0e-5);
    CHECK (contains (c.hints[0].detail, "-1"));

    // The fix must work: a plain ceiling of -1.0 does not give -1 dBTP, so the advice is the True Peak mode.
    CHECK (contains (c.hints[0].detail, "True Peak in the Maximizer"));
    CHECK (contains (c.hints[0].detail, "Reset analysis"));   // the true peak is a maximum since reset
    CHECK (! contains (c.hints[0].detail, "turn the master down"));
    CHECK (! contains (c.hints[0].detail, "Your reference peaks"));   // the reference is at -1.2 dBTP

    // -1.2 dBTP is fine.
    CHECK (findHint (ps::compareProfiles (ref, ref), Topic::TruePeak) == nullptr);
}

TEST_CASE ("ref. True peak: Minor when the reference peaks at least as high, Major when the mix peaks higher")
{
    // The user's songs: mix +0.7 dBTP, reference +2.4 dBTP (a loud club master).
    auto ref = makeProfile();
    ref.truePeakDb = 2.4f;
    auto mix = makeProfile();
    mix.truePeakDb = 0.7f;
    mix.bands[(size_t) Band::Sub].levelDb -= 2.4f;

    auto c = ps::compareProfiles (mix, ref);
    checkHintList (c);
    const auto* h = findHint (c, Topic::TruePeak);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Minor);
    CHECK_EQ (h->title, std::string ("True peak +0.7 dBTP - may distort when streamed"));
    CHECK (contains (h->detail, "Your reference peaks at +2.4 dBTP too"));
    CHECK (! contains (h->detail, "turn the master down"));
    CHECK (h->detail.size() < 420);   // fits a hint card at the default size
    pstest::note ("true-peak detail length", (double) h->detail.size());

    // Good = no true-peak hint at all.
    const auto severityOf = [] (const ps::Comparison& cmp)
    {
        const auto* tp = findHint (cmp, Topic::TruePeak);
        return tp != nullptr ? tp->severity : Severity::Good;
    };

    // Equal peaks: still Minor. Mix above the reference: Major.
    mix.truePeakDb = 2.4f;
    CHECK (severityOf (ps::compareProfiles (mix, ref)) == Severity::Minor);
    mix.truePeakDb = 2.9f;
    h = findHint (c = ps::compareProfiles (mix, ref), Topic::TruePeak);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Major);
    CHECK (contains (h->detail, "Your reference peaks at +2.4 dBTP too"));

    // Reference below -1 dBTP, or no reference: Major.
    mix.truePeakDb = 0.7f;
    ref.truePeakDb = -1.2f;
    CHECK (severityOf (ps::compareProfiles (mix, ref)) == Severity::Major);
    CHECK (severityOf (ps::compareProfiles (mix, ps::TrackProfile {})) == Severity::Major);
}

TEST_CASE ("ref. Low-end correlation 0.3 -> LowEnd Major; 0.7 -> Minor")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();
    mix.lowEndCorrelation = 0.3f;

    auto c = ps::compareProfiles (mix, ref);
    checkHintList (c);
    const auto* h = findHint (c, Topic::LowEnd);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Major);
    CHECK (contains (h->title, "Bass is not mono-compatible"));
    CHECK (contains (h->title, "0.30"));
    CHECK (contains (h->detail, "0.97"));   // the reference's value
    CHECK (contains (h->detail, "Imager's lowest band"));
    CHECK (contains (h->detail, "Mono in the Monitor"));
    CHECK (! contains (h->detail, "Stereo Shaper"));
    CHECK (c.hints[0].topic == Topic::LowEnd);

    mix.lowEndCorrelation = 0.7f;
    c = ps::compareProfiles (mix, ref);
    h = findHint (c, Topic::LowEnd);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Minor);

    // Severity comes from the mix alone; a wide reference is only mentioned as context.
    auto wideRef = makeProfile();
    wideRef.lowEndCorrelation = 0.35f;
    mix.lowEndCorrelation = 0.3f;
    const auto bothWide = ps::compareProfiles (mix, wideRef);
    h = findHint (bothWide, Topic::LowEnd);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Major);
    CHECK (contains (h->detail, "style choice"));

    // Inverted bass cancels in mono, whatever the reference does.
    mix.lowEndCorrelation = -1.0f;
    const auto inverted = ps::compareProfiles (mix, wideRef);
    h = findHint (inverted, Topic::LowEnd);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Major);
    CHECK (contains (h->detail, "cancels"));

    // A mix with no low end at all: the correlation means nothing, no hint.
    for (auto b : { Band::Sub, Band::Low })
        mix.bands[(size_t) b].levelDb = -80.0f;
    CHECK (findHint (ps::compareProfiles (mix, ref), Topic::LowEnd) == nullptr);
}

TEST_CASE ("ref. Loudness hints: quieter is Info (normal before mastering), louder is Minor / Major")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();
    mix.integratedLufs = ref.integratedLufs - 4.2f;

    auto c = ps::compareProfiles (mix, ref);
    checkHintList (c);
    CHECK_NEAR (c.loudnessDiffLu, -4.2, 1.0e-5);
    const auto* h = findHint (c, Topic::Loudness);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Info);
    CHECK_EQ (h->title, std::string ("Master is 4.2 LU quieter"));
    CHECK (contains (h->detail, "Level match plays both"));
    CHECK (c.hints.back().severity == Severity::Good);   // nothing else to do

    mix.integratedLufs = ref.integratedLufs + 2.0f;
    h = findHint (c = ps::compareProfiles (mix, ref), Topic::Loudness);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Minor);
    CHECK_EQ (h->title, std::string ("Master is 2.0 LU louder"));

    mix.integratedLufs = ref.integratedLufs + 4.0f;
    h = findHint (c = ps::compareProfiles (mix, ref), Topic::Loudness);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Major);

    mix.integratedLufs = ref.integratedLufs + 0.9f;
    CHECK (findHint (ps::compareProfiles (mix, ref), Topic::Loudness) == nullptr);
}

TEST_CASE ("ref. Dynamics (PLR) hints")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();

    mix.plrDb = ref.plrDb + 3.0f;
    const auto peaky = ps::compareProfiles (mix, ref);
    const auto* h = findHint (peaky, Topic::Dynamics);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Minor);
    CHECK (contains (h->title, "PLR +3.0 dB"));

    // Much quieter and more dynamic: an unmastered mix, which is normal.
    mix.integratedLufs = ref.integratedLufs - 6.0f;
    const auto unmastered = ps::compareProfiles (mix, ref);
    h = findHint (unmastered, Topic::Dynamics);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Info);
    CHECK (contains (h->detail, "not been limited as hard"));

    mix = makeProfile();
    mix.plrDb = ref.plrDb - 5.0f;
    const auto squashed = ps::compareProfiles (mix, ref);
    h = findHint (squashed, Topic::Dynamics);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Major);
    CHECK (contains (h->title, "squashed"));
}

TEST_CASE ("ref. Dynamics: a clipped reference's overshoot between samples does not make the mix look squashed")
{
    // TrackProfile::plrDb is sample peak - integrated (as TrackAnalyser computes it). The user's case after following
    // the true-peak hint: mix limited to -1 dBTP (sample peak -1.1), reference hard clipped (sample peak -0.3,
    // true peak +2.4), both about -8 LUFS. A true-peak PLR would read 7.0 vs 10.4 (-3.4 dB, "more squashed").
    const auto withPeaks = [] (float samplePeak, float truePeak, float integrated)
    {
        auto p = makeProfile();
        p.samplePeakDb = samplePeak;
        p.truePeakDb = truePeak;
        p.integratedLufs = integrated;
        p.plrDb = samplePeak - integrated;
        return p;
    };

    const auto clippedRef = withPeaks (-0.3f, 2.4f, -8.0f);
    const auto limitedMix = withPeaks (-1.1f, -1.0f, -8.0f);

    auto c = ps::compareProfiles (limitedMix, clippedRef);
    checkHintList (c);
    CHECK_NEAR (c.plrDiffDb, -0.8, 1.0e-4);
    CHECK (findHint (c, Topic::Dynamics) == nullptr);

    // Mirror: a clipped mix against a clean reference is not "more dynamic, compress more".
    c = ps::compareProfiles (withPeaks (-0.3f, 2.4f, -8.0f), withPeaks (-1.1f, -1.0f, -8.0f));
    CHECK_NEAR (c.plrDiffDb, 0.8, 1.0e-4);
    CHECK (findHint (c, Topic::Dynamics) == nullptr);
}

TEST_CASE ("ref. Stereo width hints: one per direction, other bands named in the text")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();
    mix.bands[(size_t) Band::Air].widthPct = 20.0f;       // 70 -> 20: -50 points
    mix.bands[(size_t) Band::HighMid].widthPct = 30.0f;   // 55 -> 30: -25 points

    const auto c = ps::compareProfiles (mix, ref);
    checkHintList (c);
    CHECK_NEAR (c.bands[(size_t) Band::Air].widthDiffPct, -50.0, 1.0e-4);

    int stereoHints = 0;
    for (const auto& h : c.hints)
        stereoHints += h.topic == Topic::Stereo ? 1 : 0;
    CHECK_EQ (stereoHints, 1);

    const auto* h = findHint (c, Topic::Stereo);
    REQUIRE (h != nullptr);
    CHECK_EQ (h->band, (int) Band::Air);
    CHECK (h->severity == Severity::Major);
    CHECK (contains (h->title, "narrower"));
    CHECK (contains (h->title, "20%"));
    CHECK (contains (h->title, "70%"));
    CHECK (contains (h->detail, "high mids"));

    // Wider in the mids, narrower in the air: two hints.
    mix.bands[(size_t) Band::HighMid].widthPct = ref.bands[(size_t) Band::HighMid].widthPct;
    mix.bands[(size_t) Band::Mid].widthPct = 65.0f;   // 40 -> 65
    const auto both = ps::compareProfiles (mix, ref);
    const auto* wider = findHint (both, Topic::Stereo, (int) Band::Mid);
    REQUIRE (wider != nullptr);
    CHECK (wider->severity == Severity::Minor);
    CHECK (contains (wider->title, "wider"));
    REQUIRE (findHint (both, Topic::Stereo, (int) Band::Air) != nullptr);
}

TEST_CASE ("ref. Mono mix vs stereo reference, and broadband phase cancellation")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();
    mix.isMono = true;
    for (auto& b : mix.bands)
    {
        b.widthPct = 0.0f;
        b.correlation = 1.0f;
    }
    mix.widthPct = 0.0f;
    mix.correlation = 1.0f;

    auto c = ps::compareProfiles (mix, ref);
    checkHintList (c);
    const auto* h = findHint (c, Topic::Stereo);
    REQUIRE (h != nullptr);
    CHECK_EQ (h->title, std::string ("Master is mono, reference is stereo"));
    CHECK (h->severity == Severity::Major);

    mix = makeProfile();
    mix.correlation = -0.2f;
    c = ps::compareProfiles (mix, ref);
    h = findHint (c, Topic::Stereo);
    REQUIRE (h != nullptr);
    CHECK (h->severity == Severity::Major);
    CHECK (contains (h->title, "-0.20"));
}

TEST_CASE ("ref. Setup hints when the reference or the mix is missing")
{
    const auto p = makeProfile();
    const ps::TrackProfile none;

    auto c = ps::compareProfiles (p, none);
    checkHintList (c);
    CHECK (c.mixReady);
    CHECK (! c.referenceReady);
    CHECK (! c.valid());
    CHECK_EQ (c.loudnessDiffLu, ps::kNoValue);
    CHECK_EQ (c.curveDiffDb[0], ps::kNoValue);
    CHECK (! c.bands[0].valid);
    REQUIRE (c.hints.size() == 1);
    CHECK (c.hints[0].topic == Topic::Setup);
    CHECK_EQ (c.hints[0].title, std::string ("Add a reference track"));
    CHECK (contains (c.hints[0].detail, "WAV, MP3, FLAC, AIFF or OGG"));

    c = ps::compareProfiles (none, p);
    checkHintList (c);
    REQUIRE (c.hints.size() == 1);
    CHECK (c.hints[0].topic == Topic::Setup);
    CHECK_EQ (c.hints[0].title, std::string ("Play your track for a few seconds"));
    CHECK (contains (c.hints[0].detail, "same section as the reference"));
    // What is compared: the output, after every module.
    CHECK (contains (c.hints[0].detail, "after every module"));

    c = ps::compareProfiles (none, none);
    checkHintList (c);
    REQUIRE (c.hints.size() == 2);
    CHECK_EQ (c.hints[0].title, std::string ("Add a reference track"));
    CHECK_EQ (c.hints[1].title, std::string ("Play your track for a few seconds"));

    // Reference region analysed but too short / silent.
    ps::TrackProfile shortRef;
    shortRef.analysedSeconds = 0.4;
    c = ps::compareProfiles (p, shortRef);
    CHECK_EQ (c.hints[0].title, std::string ("Reference region is too short"));

    // Mix-only problems still show without a reference, after the setup hint.
    auto clipping = makeProfile();
    clipping.truePeakDb = 0.2f;
    c = ps::compareProfiles (clipping, none);
    checkHintList (c);
    REQUIRE (c.hints.size() == 2);
    CHECK (c.hints[0].topic == Topic::Setup);
    CHECK (c.hints[1].topic == Topic::TruePeak);
}

TEST_CASE ("ref. Many problems: capped at kMaxHints, sorted, ASCII")
{
    const auto ref = makeProfile();
    auto mix = makeProfile();
    const float diffs[] = { 4.0f, -3.5f, 2.0f, -1.8f, 5.0f, -2.5f };
    for (size_t b = 0; b < (size_t) ps::kNumBands; ++b)
    {
        mix.bands[b].levelDb += diffs[b];
        mix.bands[b].widthPct += 45.0f;
    }
    mix.truePeakDb = 0.3f;
    mix.lowEndCorrelation = 0.6f;
    mix.integratedLufs += 2.5f;
    mix.plrDb -= 3.0f;
    mix.samplePeakDb = mix.plrDb + mix.integratedLufs;   // keep plrDb = samplePeakDb - integratedLufs consistent

    const auto c = ps::compareProfiles (mix, ref);
    checkHintList (c);
    CHECK_EQ ((int) c.hints.size(), ps::kMaxHints);
    CHECK (c.hints[0].topic == Topic::TruePeak);

    // Among the Tone hints of equal severity the bigger difference comes first.
    const ps::Hint* high = findHint (c, Topic::Tone, (int) Band::HighMid);   // +5.0
    const ps::Hint* sub = findHint (c, Topic::Tone, (int) Band::Sub);        // +4.0
    REQUIRE (high != nullptr);
    REQUIRE (sub != nullptr);
    CHECK (high < sub);

    for (const auto& h : c.hints)
        pstest::note ("  [" + std::to_string ((int) h.severity) + "] " + h.title, h.amount);
}

//==============================================================================
TEST_CASE ("ref. End to end: analysed audio with extra low mids gets a low-mid hint")
{
    const double sr = 48000.0;
    // Mostly correlated channels (correlation ~0.9), like real music; independent channels would rightly trigger
    // the low-end mono hint.
    const auto refL = pstest::pinkNoise (0.4, 12.0, sr, 51);
    const auto refR = pstest::added (pstest::scaled (refL, 0.9f), pstest::pinkNoise (0.4 * 0.436, 12.0, sr, 52));
    const auto bump = pstest::sine (330.0, -28.0, 12.0, sr);

    const auto ref = ps::analyseRegion (refL.data(), refR.data(), 0, (long long) refL.size(), sr);
    const auto same = ps::analyseRegion (refL.data(), refR.data(), 0, (long long) refL.size(), sr);
    const auto muddy = ps::analyseRegion (pstest::added (refL, bump).data(), pstest::added (refR, bump).data(), 0,
                                          (long long) refL.size(), sr);

    const auto good = ps::compareProfiles (same, ref);
    checkHintList (good);
    REQUIRE (good.hints.size() == 1);
    CHECK (good.hints[0].severity == Severity::Good);

    const auto c = ps::compareProfiles (muddy, ref);
    checkHintList (c);
    pstest::note ("low-mid diff dB", c.bands[(size_t) Band::LowMid].levelDiffDb);
    CHECK (c.bands[(size_t) Band::LowMid].levelDiffDb > 1.5f);
    CHECK (findHint (c, Topic::Tone, (int) Band::LowMid) != nullptr);
    CHECK (std::abs (c.bands[(size_t) Band::Air].levelDiffDb) < c.bands[(size_t) Band::LowMid].levelDiffDb);
}
