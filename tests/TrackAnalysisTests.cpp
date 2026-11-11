#include "TestFramework.h"
#include "TestSignals.h"
#include "AllocationCounter.h"

#include "core/TrackAnalysis.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

// Tests for TrackAnalysis.h (ported with the analyser from RefMatch): TrackAnalyser (bands, stereo, averaging, curve)
// and the offline helpers.

//==============================================================================
namespace
{
using ps::Averaging;
using ps::Band;

ps::TrackProfile analyse (const std::vector<float>& left, const std::vector<float>* right, double sampleRate,
                          Averaging averaging = Averaging::SinceReset, int blockSize = 512)
{
    ps::TrackAnalyser a;
    a.prepare (sampleRate, averaging);

    for (size_t pos = 0; pos < left.size(); pos += (size_t) blockSize)
    {
        const int n = (int) std::min ((size_t) blockSize, left.size() - pos);
        a.push (left.data() + pos, right != nullptr ? right->data() + pos : nullptr, n);
    }

    return a.profile();
}

ps::TrackProfile analyseStereo (const std::vector<float>& left, const std::vector<float>& right, double sampleRate,
                                Averaging averaging = Averaging::SinceReset)
{
    return analyse (left, &right, sampleRate, averaging);
}

float bandLevel (const ps::TrackProfile& p, Band b) { return p.bands[(size_t) b].levelDb; }

// Sum of sines, one per band (40, 120, 350, 1000, 3500, 10000 Hz); the right channel is shifted by `phaseR`.
std::vector<float> multiSine (double seconds, double sampleRate, double phaseR = 0.0)
{
    const double freqs[] = { 40.0, 120.0, 350.0, 1000.0, 3500.0, 10000.0 };
    const double peaks[] = { -14.0, -14.0, -20.0, -20.0, -26.0, -30.0 };

    std::vector<float> out ((size_t) std::llround (seconds * sampleRate), 0.0f);
    for (int s = 0; s < 6; ++s)
        out = pstest::added (out, pstest::sine (freqs[s], peaks[s], seconds, sampleRate, phaseR));

    return out;
}

// Curve value nearest a frequency.
float curveAt (const ps::TrackProfile& p, double hz)
{
    int best = 0;
    for (int i = 1; i < ps::kCurvePoints; ++i)
        if (std::abs (std::log (ps::curveFrequency (i) / hz)) < std::abs (std::log (ps::curveFrequency (best) / hz)))
            best = i;

    return p.curveDb[(size_t) best];
}
} // namespace

//==============================================================================
TEST_CASE ("ta. Band edges are contiguous and the FFT order follows the sample rate")
{
    for (int b = 0; b + 1 < ps::kNumBands; ++b)
        CHECK_EQ (ps::bandInfo (b).highHz, ps::bandInfo (b + 1).lowHz);

    CHECK_EQ (ps::bandInfo (0).lowHz, 20.0f);
    CHECK_EQ (ps::bandInfo (ps::kNumBands - 1).highHz, 20000.0f);
    CHECK_EQ (std::string (ps::bandInfo (Band::LowMid).name), std::string ("Low mids"));

    // The stereo chart's short names use the same words as the tonal panel ("Bass", not "Low" next to "Low-mid").
    CHECK_EQ (std::string (ps::bandInfo (Band::Low).name), std::string ("Bass"));
    CHECK_EQ (std::string (ps::bandInfo (Band::Low).shortName), std::string ("Bass"));

    CHECK_EQ (ps::fftOrderForSampleRate (44100.0), 13);
    CHECK_EQ (ps::fftOrderForSampleRate (48000.0), 13);
    CHECK_EQ (ps::fftOrderForSampleRate (96000.0), 14);
    CHECK_EQ (ps::fftOrderForSampleRate (192000.0), 15);

    CHECK_NEAR (ps::curveFrequency (0), 20.0, 1.0e-3);
    CHECK_NEAR (ps::curveFrequency (ps::kCurvePoints - 1), 20000.0, 0.1);
}

TEST_CASE ("ta. Sines land in their band at 20 log10 (A) - 3.01 dB (Parseval scale)")
{
    struct Probe { double hz; Band band; };
    const double expected = 20.0 * std::log10 (0.5) - 3.0103;   // A = 0.5 in both channels

    for (auto mode : { Averaging::SinceReset, Averaging::Recent })
    {
        for (const Probe probe : { Probe { 40.0, Band::Sub }, Probe { 1000.0, Band::Mid }, Probe { 10000.0, Band::Air },
                                   Probe { 333.0, Band::LowMid }, Probe { 3001.0, Band::HighMid },
                                   Probe { 97.0, Band::Low } })
        {
            const auto x = pstest::sine (probe.hz, 20.0 * std::log10 (0.5), 5.0, 48000.0);
            const auto p = analyseStereo (x, x, 48000.0, mode);

            REQUIRE (p.valid);
            CHECK_NEAR (bandLevel (p, probe.band), expected, 0.5);

            for (int b = 0; b < ps::kNumBands; ++b)
                if (b != (int) probe.band)
                    CHECK (p.bands[(size_t) b].levelDb < bandLevel (p, probe.band) - 30.0f);

            if (mode == Averaging::SinceReset && probe.hz == 1000.0)
                pstest::note ("1 kHz A=0.5 Mid band dB (expect -9.03)", bandLevel (p, probe.band));
        }
    }
}

TEST_CASE ("ta. Stereo: identical channels are mono, inverted channels correlate -1")
{
    const auto noise = pstest::whiteNoise (0.3, 5.0, 48000.0, 11);

    const auto same = analyseStereo (noise, noise, 48000.0);
    CHECK (same.isMono);
    CHECK_EQ (same.correlation, 1.0f);
    CHECK_EQ (same.widthPct, 0.0f);
    CHECK_EQ (same.lowEndCorrelation, 1.0f);
    for (const auto& b : same.bands)
    {
        CHECK_EQ (b.correlation, 1.0f);
        CHECK_EQ (b.widthPct, 0.0f);
    }

    const auto inverted = analyseStereo (noise, pstest::scaled (noise, -1.0f), 48000.0);
    CHECK (! inverted.isMono);
    CHECK_NEAR (inverted.correlation, -1.0, 1.0e-3);
    CHECK_NEAR (inverted.lowEndCorrelation, -1.0, 1.0e-3);
    CHECK_NEAR (inverted.widthPct, 300.0, 1.0e-3);   // clamped
    for (const auto& b : inverted.bands)
        CHECK_NEAR (b.correlation, -1.0, 1.0e-3);

    // Mono input (right == nullptr): same spectrum as L == R, loudness per BS.1770 mono (one channel).
    const auto mono = analyse (noise, nullptr, 48000.0);
    CHECK (mono.isMono);
    CHECK_NEAR (bandLevel (mono, Band::Mid), bandLevel (same, Band::Mid), 1.0e-3);
    CHECK_NEAR (same.integratedLufs - mono.integratedLufs, 3.0103, 0.01);
}

TEST_CASE ("ta. Stereo: independent noise has correlation ~0 and width ~100 %")
{
    const auto l = pstest::whiteNoise (0.3, 10.0, 48000.0, 21);
    const auto r = pstest::whiteNoise (0.3, 10.0, 48000.0, 22);
    const auto p = analyseStereo (l, r, 48000.0);

    CHECK (! p.isMono);
    CHECK_NEAR (p.correlation, 0.0, 0.05);
    CHECK_NEAR (p.widthPct, 100.0, 10.0);

    for (int b = 1; b < ps::kNumBands; ++b)
    {
        CHECK_NEAR (p.bands[(size_t) b].correlation, 0.0, 0.1);
        CHECK_NEAR (p.bands[(size_t) b].widthPct, 100.0, 20.0);
    }

    pstest::note ("independent noise correlation", p.correlation);
    pstest::note ("independent noise width %", p.widthPct);
}

TEST_CASE ("ta. Stereo: a phase offset gives correlation cos (phi) and width 100 (1 - cos) / (1 + cos)")
{
    const double phi = pstest::kPi / 3.0;   // 60 degrees: correlation 0.5, width 33.3 %
    const auto p = analyseStereo (multiSine (6.0, 48000.0), multiSine (6.0, 48000.0, phi), 48000.0);

    for (const auto& b : p.bands)
    {
        CHECK_NEAR (b.correlation, 0.5, 0.02);
        CHECK_NEAR (b.widthPct, 100.0 / 3.0, 2.0);
    }
}

TEST_CASE ("ta. Low-end correlation: wide bass vs mono bass")
{
    const double sr = 48000.0;
    const auto mids = pstest::sine (1000.0, -12.0, 6.0, sr);
    const auto midsR = pstest::sine (1000.0, -12.0, 6.0, sr, 0.5);   // keeps the mix itself stereo

    const auto bassL = pstest::sine (50.0, -10.0, 6.0, sr);
    const auto bassWideR = pstest::sine (50.0, -10.0, 6.0, sr, pstest::kPi / 2.0);   // 90 degrees apart

    const auto wide = analyseStereo (pstest::added (bassL, mids), pstest::added (bassWideR, midsR), sr);
    const auto mono = analyseStereo (pstest::added (bassL, mids), pstest::added (bassL, midsR), sr);

    CHECK (! wide.isMono);
    CHECK (! mono.isMono);
    CHECK_NEAR (wide.lowEndCorrelation, 0.0, 0.1);
    CHECK (mono.lowEndCorrelation > 0.99f);
    CHECK (wide.bands[(size_t) Band::Sub].widthPct > 80.0f);
    CHECK (mono.bands[(size_t) Band::Sub].widthPct < 1.0f);
}

TEST_CASE ("ta. Silent frames are skipped: pausing does not change the picture")
{
    for (auto mode : { Averaging::SinceReset, Averaging::Recent })
    {
        const auto music = multiSine (4.0, 48000.0);
        const auto paused = pstest::concat (music, std::vector<float> ((size_t) (20 * 48000), 0.0f));

        const auto before = analyseStereo (music, music, 48000.0, mode);
        const auto after = analyseStereo (paused, paused, 48000.0, mode);

        for (int b = 0; b < ps::kNumBands; ++b)
            CHECK_NEAR (after.bands[(size_t) b].levelDb, before.bands[(size_t) b].levelDb, 0.3);

        CHECK_NEAR (after.analysedSeconds, before.analysedSeconds, 0.2);
        CHECK_NEAR (after.rmsDb, before.rmsDb, 0.1);
        CHECK_NEAR (after.spectralLoudnessDb, before.spectralLoudnessDb, 0.3);
        CHECK (after.valid);
    }

    // Only silence: nothing known, profile not valid.
    const std::vector<float> zeros ((size_t) (3 * 48000), 0.0f);
    const auto silent = analyseStereo (zeros, zeros, 48000.0);
    CHECK (! silent.valid);
    CHECK_EQ (silent.analysedSeconds, 0.0);
    CHECK_EQ (silent.rmsDb, ps::kNoValue);
    CHECK_EQ (silent.spectralLoudnessDb, ps::kNoValue);
    CHECK_EQ (silent.bands[0].levelDb, ps::kNoValue);
    CHECK_EQ (silent.curveDb[50], ps::kNoValue);
    CHECK_EQ (silent.integratedLufs, ps::kNoValue);
    CHECK_EQ (silent.truePeakDb, ps::kNoValue);
}

TEST_CASE ("ta. Recent averaging forgets old content, Since reset keeps it")
{
    // Recent's time constant is about one 16-bar loop (so a looped section reads steadily).
    static_assert (ps::kRecentSeconds >= 20.0 && ps::kRecentSeconds <= 40.0, "Recent should follow about one loop");

    const double sr = 48000.0;
    const double firstSeconds = 10.0, secondSeconds = 3.0 * ps::kRecentSeconds;
    const double total = firstSeconds + secondSeconds;
    const auto first = pstest::sine (1000.0, -6.0206, firstSeconds, sr);    // Mid band
    const auto second = pstest::sine (100.0, -6.0206, secondSeconds, sr);   // Low band
    const auto x = pstest::concat (first, second);

    const auto recent = analyseStereo (x, x, sr, Averaging::Recent);
    const auto all = analyseStereo (x, x, sr, Averaging::SinceReset);

    // Recent: the 1 kHz part is 3 time constants old -> below -15 dB; the 100 Hz part holds ~all the weight.
    CHECK (bandLevel (recent, Band::Mid) < bandLevel (recent, Band::Low) - 10.0f);
    CHECK_NEAR (bandLevel (recent, Band::Low), -9.03, 0.5);

    // Since reset: plain shares of the time (10 of 100 s -> -10 dB, 90 of 100 s -> -0.46 dB at kRecentSeconds 30).
    CHECK_NEAR (bandLevel (all, Band::Mid), -9.03 + 10.0 * std::log10 (firstSeconds / total), 0.5);
    CHECK_NEAR (bandLevel (all, Band::Low), -9.03 + 10.0 * std::log10 (secondSeconds / total), 0.5);

    // Loudness and peaks do not depend on the averaging mode.
    CHECK_EQ (recent.integratedLufs, all.integratedLufs);
    CHECK_EQ (recent.truePeakDb, all.truePeakDb);
    CHECK_NEAR (recent.analysedSeconds, all.analysedSeconds, 1.0e-9);

    pstest::note ("Recent: Mid - Low dB after 3 time constants",
                  bandLevel (recent, Band::Mid) - bandLevel (recent, Band::Low));
}

TEST_CASE ("ta. Switching the averaging mode shows the other average at once, exactly")
{
    const double sr = 48000.0;
    const double loudSeconds = 120.0, quietSeconds = ps::kRecentSeconds;
    const auto loudPart = pstest::sine (1000.0, -6.0206, loudSeconds, sr);       // A = 0.5: Mid band -9.03 dB
    const auto quietPart = pstest::sine (1000.0, -26.0206, quietSeconds, sr);    // 20 dB lower
    const auto noiseL = pstest::whiteNoise (0.2, 20.0, sr, 61);
    const auto noiseR = pstest::added (pstest::scaled (noiseL, 0.6f), pstest::whiteNoise (0.15, 20.0, sr, 62));

    // Always-Recent / always-Since-reset analysers as the references.
    ps::TrackAnalyser recentAll, sinceAll, switched;
    recentAll.prepare (sr, Averaging::Recent);
    sinceAll.prepare (sr, Averaging::SinceReset);
    switched.prepare (sr, Averaging::SinceReset);

    const auto feed = [&] (const std::vector<float>& l, const std::vector<float>& r)
    {
        for (auto* a : { &recentAll, &sinceAll, &switched })
            a->push (l.data(), r.data(), (int) l.size());
    };

    // A long Since-reset run, then switch to Recent: the profile is the true recent average at once (the long
    // history does not drag it), and new audio fades in with the kRecentSeconds time constant.
    feed (loudPart, loudPart);
    switched.setAveraging (Averaging::Recent);
    CHECK (switched.getAveraging() == Averaging::Recent);
    CHECK_NEAR (bandLevel (switched.profile(), Band::Mid), bandLevel (recentAll.profile(), Band::Mid), 1.0e-4);

    feed (quietPart, quietPart);
    const auto afterDrop = switched.profile();
    for (int b = 0; b < ps::kNumBands; ++b)
        CHECK_NEAR (afterDrop.bands[(size_t) b].levelDb, recentAll.profile().bands[(size_t) b].levelDb, 1.0e-4);

    // One time constant after a 20 dB drop about e^-1 of the weight is still the old level (-4.3 dB); the old
    // behaviour (history weight carried over from Since reset) read about -1.5 dB here.
    const double tau = ps::kRecentSeconds;
    const double oldShare = std::exp (-quietSeconds / tau) * (1.0 - std::exp (-loudSeconds / tau))
                          / (1.0 - std::exp (-(loudSeconds + quietSeconds) / tau));
    const double loudLevel = 20.0 * std::log10 (0.5) - 3.0103;
    CHECK_NEAR (bandLevel (afterDrop, Band::Mid), loudLevel + 10.0 * std::log10 (oldShare + (1.0 - oldShare) * 0.01),
                0.15);
    pstest::note ("Recent Mid change one time constant after a 20 dB drop, dB", bandLevel (afterDrop, Band::Mid) - loudLevel);

    // And back: Since reset is the plain average of everything, again exactly.
    feed (noiseL, noiseR);
    switched.setAveraging (Averaging::SinceReset);
    const auto backSince = switched.profile();
    for (int b = 0; b < ps::kNumBands; ++b)
    {
        CHECK_NEAR (backSince.bands[(size_t) b].levelDb, sinceAll.profile().bands[(size_t) b].levelDb, 1.0e-4);
        CHECK_NEAR (backSince.bands[(size_t) b].widthPct, sinceAll.profile().bands[(size_t) b].widthPct, 1.0e-3);
    }
    for (int k = 0; k < ps::kCurvePoints; ++k)
        CHECK_NEAR (backSince.curveDb[(size_t) k], sinceAll.profile().curveDb[(size_t) k], 1.0e-4);

    CHECK_NEAR (backSince.spectralLoudnessDb, sinceAll.profile().spectralLoudnessDb, 1.0e-4);
    CHECK_EQ (backSince.analysedSeconds, sinceAll.profile().analysedSeconds);
    CHECK_EQ (backSince.integratedLufs, sinceAll.profile().integratedLufs);
}

TEST_CASE ("ta. Profile becomes valid after kMinProfileSeconds of non-silent audio; reset clears it")
{
    const auto x = multiSine (2.0, 48000.0);
    const std::vector<float> half (x.begin(), x.begin() + 24000);

    ps::TrackAnalyser a;
    a.prepare (48000.0, Averaging::Recent);
    a.push (half.data(), half.data(), (int) half.size());
    CHECK (! a.profile().valid);
    CHECK (a.profile().analysedSeconds < ps::kMinProfileSeconds);

    a.push (x.data(), x.data(), (int) x.size());
    const auto p = a.profile();
    CHECK (p.valid);
    CHECK (p.analysedSeconds >= ps::kMinProfileSeconds);
    CHECK (p.analysedSeconds <= 2.5);

    a.reset();
    const auto cleared = a.profile();
    CHECK (! cleared.valid);
    CHECK_EQ (cleared.analysedSeconds, 0.0);
    CHECK_EQ (cleared.integratedLufs, ps::kNoValue);
    CHECK_EQ (cleared.truePeakDb, ps::kNoValue);
    CHECK_EQ (cleared.bands[3].levelDb, ps::kNoValue);
    CHECK (! cleared.isMono);
}

TEST_CASE ("ta. Peaks, RMS, crest and PLR")
{
    const auto x = pstest::sine (1000.0, -6.0206, 5.0, 48000.0);   // A = 0.5
    const auto p = analyseStereo (x, x, 48000.0);

    CHECK_NEAR (p.samplePeakDb, -6.02, 0.01);
    CHECK_NEAR (p.truePeakDb, -6.02, 0.1);
    CHECK_NEAR (p.rmsDb, -9.03, 0.02);
    CHECK_NEAR (p.crestDb, 3.01, 0.03);
    CHECK_NEAR (p.integratedLufs, -6.02, 0.1);
    CHECK_NEAR (p.momentaryLufs, -6.02, 0.1);
    CHECK_NEAR (p.shortTermLufs, -6.02, 0.1);
    CHECK_NEAR (p.plrDb, p.samplePeakDb - p.integratedLufs, 1.0e-4);
    CHECK_NEAR (p.loudnessRangeLu, 0.0, 0.1);

    // The spectral loudness of a steady sine equals its integrated loudness.
    CHECK_NEAR (p.spectralLoudnessDb, p.integratedLufs, 0.1);
}

TEST_CASE ("ta. Band dynamics: a steady band reads ~0 dB, a band switching between two levels reads their spacing")
{
    const double sr = 48000.0, seconds = 12.0;
    // Bass: a steady 120 Hz sine. Mids: a 1 kHz sine 1 s at -10 dB, then 2 s at -22 dB, and so on (12 dB apart; a
    // third of the time loud, so the median is the quiet level).
    auto x = pstest::sine (120.0, -12.0, seconds, sr);
    const auto mid = pstest::sine (1000.0, -10.0, seconds, sr);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += ((i / (size_t) sr) % 3 == 0 ? 1.0f : 0.25119f) * mid[i];

    for (auto mode : { Averaging::SinceReset, Averaging::Recent })
    {
        const auto p = analyseStereo (x, x, sr, mode);
        const auto& bass = p.bands[(size_t) Band::Low];
        const auto& mids = p.bands[(size_t) Band::Mid];
        REQUIRE (ps::hasValue (bass.dynamicRangeDb));
        REQUIRE (ps::hasValue (mids.dynamicRangeDb));
        CHECK (bass.dynamicRangeDb < 0.5f);
        CHECK (bass.peakOverMedianDb < 0.5f);
        // Frames straddling a switch read in between, so the spread is close to 12 dB, not exact.
        CHECK_NEAR (mids.dynamicRangeDb, 12.0, 1.0);
        CHECK_NEAR (mids.peakOverMedianDb, 12.0, 1.0);
        if (mode == Averaging::SinceReset)
        {
            pstest::note ("two-level mids: dynamic range dB (expect ~12)", mids.dynamicRangeDb);
            pstest::note ("two-level mids: peak over median dB", mids.peakOverMedianDb);
        }
    }

    // A band with no content has no reading, and too short a stretch has none either.
    const auto steady = analyseStereo (pstest::sine (1000.0, -10.0, seconds, sr), pstest::sine (1000.0, -10.0, seconds, sr), sr);
    CHECK (! ps::hasValue (steady.bands[(size_t) Band::Sub].dynamicRangeDb)
           || steady.bands[(size_t) Band::Sub].levelDb < -90.0f);
    const auto shortRun = pstest::sine (1000.0, -10.0, 1.2, sr);
    CHECK (! ps::hasValue (analyseStereo (shortRun, shortRun, sr).bands[(size_t) Band::Mid].dynamicRangeDb));
}

TEST_CASE ("ta. PLR ignores the overshoot between samples (clipped masters must not look more dynamic)")
{
    // A sine at a quarter of the sample rate, 45 degrees off the sample grid: every sample is at +-0.707 of the true
    // peak, so the true peak is 3 dB above the sample peak - like the overshoot of a hard-clipped master.
    const double sr = 48000.0;
    const auto x = pstest::sine (sr / 4.0, -1.0, 5.0, sr, pstest::kPi / 4.0);
    const auto p = analyseStereo (x, x, sr);

    CHECK_NEAR (p.samplePeakDb, -1.0 - 3.01, 0.02);
    CHECK_NEAR (p.truePeakDb - p.samplePeakDb, 3.01, 0.2);
    CHECK_NEAR (p.plrDb, p.samplePeakDb - p.integratedLufs, 1.0e-4);
    pstest::note ("fs/4 sine: true peak - sample peak dB", p.truePeakDb - p.samplePeakDb);
}

TEST_CASE ("ta. Same profile at 44.1, 48 and 96 kHz")
{
    std::vector<ps::TrackProfile> sines, noises;

    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const auto l = multiSine (8.0, sr);
        const auto r = multiSine (8.0, sr, 0.7);
        sines.push_back (analyseStereo (l, r, sr));

        // White noise scaled so that its PSD (power per Hz) is the same at every rate. 48 s keeps the estimator
        // variance of the narrow low-frequency 1/3-octave windows well below the tolerance (12 s: up to ~1 dB).
        const auto n = pstest::whiteNoise (0.25 * std::sqrt (sr / 48000.0), 48.0, sr, 5);
        noises.push_back (analyseStereo (n, n, sr));
    }

    for (size_t i = 1; i < sines.size(); ++i)
    {
        for (int b = 0; b < ps::kNumBands; ++b)
        {
            CHECK_NEAR (sines[i].bands[(size_t) b].levelDb, sines[0].bands[(size_t) b].levelDb, 0.3);
            CHECK_NEAR (sines[i].bands[(size_t) b].correlation, sines[0].bands[(size_t) b].correlation, 0.02);
            CHECK_NEAR (sines[i].bands[(size_t) b].widthPct, sines[0].bands[(size_t) b].widthPct, 2.0);
        }

        CHECK_NEAR (sines[i].integratedLufs, sines[0].integratedLufs, 0.1);
        CHECK_NEAR (sines[i].spectralLoudnessDb, sines[0].spectralLoudnessDb, 0.2);
        CHECK_NEAR (sines[i].rmsDb, sines[0].rmsDb, 0.05);
        CHECK_NEAR (sines[i].correlation, sines[0].correlation, 0.02);
        CHECK_NEAR (sines[i].lowEndCorrelation, sines[0].lowEndCorrelation, 0.02);

        float worst = 0.0f;
        for (int k = 0; k < ps::kCurvePoints; ++k)
        {
            if (ps::curveFrequency (k) < 30.0f)
                continue;

            REQUIRE (ps::hasValue (noises[i].curveDb[(size_t) k]));
            worst = std::max (worst, std::abs (noises[i].curveDb[(size_t) k] - noises[0].curveDb[(size_t) k]));
        }

        CHECK (worst < 1.0f);
        pstest::note ("worst curve difference vs 44.1 kHz, dB", worst);
    }
}

TEST_CASE ("ta. Pink noise: curve falls ~3 dB per octave; spectral loudness ~ integrated loudness")
{
    const double sr = 48000.0;
    const auto l = pstest::pinkNoise (1.0, 20.0, sr, 31);
    const auto r = pstest::pinkNoise (1.0, 20.0, sr, 32);
    const auto p = analyseStereo (l, r, sr);

    const float slope = (curveAt (p, 10000.0) - curveAt (p, 100.0)) / (float) std::log2 (100.0);
    CHECK_NEAR (slope, -3.01, 0.3);
    pstest::note ("pink slope dB/oct 100 Hz..10 kHz", slope);

    for (double f = 125.0; f < 8000.0; f *= 2.0)
        CHECK_NEAR (curveAt (p, 2.0 * f) - curveAt (p, f), -3.01, 0.75);

    // Each curve point is a PSD (dB/Hz): integrating 10^(dB/10) over 20 Hz-20 kHz gives back the total power.
    double power = 0.0;
    for (int k = 0; k + 1 < ps::kCurvePoints; ++k)
    {
        const double f0 = ps::curveFrequency (k), f1 = ps::curveFrequency (k + 1);
        power += 0.5 * (std::pow (10.0, p.curveDb[(size_t) k] / 10.0) + std::pow (10.0, p.curveDb[(size_t) k + 1] / 10.0))
               * (f1 - f0);
    }

    double broadband = 0.0;
    for (const auto& b : p.bands)
        broadband += std::pow (10.0, b.levelDb / 10.0);

    CHECK_NEAR (10.0 * std::log10 (power), 10.0 * std::log10 (broadband), 0.3);

    CHECK_NEAR (p.spectralLoudnessDb, p.integratedLufs, 0.3);
    pstest::note ("pink spectral - integrated LU", p.spectralLoudnessDb - p.integratedLufs);
}

TEST_CASE ("ta. Parseval holds up to the Nyquist bin: (-1)^n at 32 kHz reads 0 dBFS in Air")
{
    std::vector<float> x ((size_t) (3 * 32000));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (i % 2) == 0 ? 1.0f : -1.0f;

    const auto p = analyseStereo (x, x, 32000.0);
    CHECK_NEAR (bandLevel (p, Band::Air), 0.0, 0.05);
    pstest::note ("Nyquist tone Air level dBFS", bandLevel (p, Band::Air));
}

TEST_CASE ("ta. Curve: a 1 kHz sine peaks at 1 kHz; points above Nyquist are unknown")
{
    const auto x = pstest::sine (1000.0, -6.0, 3.0, 32000.0);
    const auto p = analyseStereo (x, x, 32000.0);

    int peak = 0;
    for (int k = 1; k < ps::kCurvePoints; ++k)
        if (p.curveDb[(size_t) k] > p.curveDb[(size_t) peak])
            peak = k;

    // Hann smoothing of a pure tone: a bump centred on the tone, so the peak is the curve point nearest 1 kHz.
    CHECK_NEAR (std::log2 (ps::curveFrequency (peak) / 1000.0), 0.0, 1.0 / 30.0);

    // Its height: the kernel k(u) = 0.5 + 0.5 cos (pi u / B) at u = log2 (1000 / fc), normalised by its mass B over
    // log2 f, i.e. a PSD of power * k(u) / (B * ln 2 * 1000 Hz). One side of the bump shows the kernel's shape.
    const double power = 0.5 * std::pow (10.0, -6.0 / 10.0);
    const double b = ps::kCurveSmoothingOctaves;
    const auto expectedDb = [&] (int k)
    {
        const double u = std::log2 (1000.0 / ps::curveFrequency (k));
        const double kernel = std::abs (u) < b ? 0.5 + 0.5 * std::cos (pstest::kPi * u / b) : 0.0;
        return 10.0 * std::log10 (power * kernel / (b * std::log (2.0) * 1000.0));
    };

    CHECK_NEAR (p.curveDb[(size_t) peak], expectedDb (peak), 0.1);
    for (int k = peak - 4; k <= peak + 4; ++k)
        CHECK_NEAR (p.curveDb[(size_t) k], expectedDb (k), 0.25);

    pstest::note ("1 kHz tone curve peak - expected dB", p.curveDb[(size_t) peak] - expectedDb (peak));

    for (int k = 0; k < ps::kCurvePoints; ++k)
        if (ps::curveFrequency (k) >= 16000.0f)
            CHECK_EQ (p.curveDb[(size_t) k], ps::kNoValue);
}

TEST_CASE ("ta. Curve: harmonic-rich music gives a smooth curve, not a zig-zag of single partials")
{
    // An A minor triad of band-limited sawtooths (110, 130.8, 164.8 Hz). With the old hard-edged 1/3-octave box the
    // RMS second difference of the curve (100 Hz - 10 kHz) was ~1.9 dB: partials jumped in and out at the box edges.
    const double sr = 48000.0, seconds = 4.0;
    const auto n = (size_t) (seconds * sr);
    std::vector<float> x (n, 0.0f);

    for (double f0 : { 110.0, 130.81, 164.81 })
    {
        for (int h = 1; h * f0 < 13000.0; ++h)
        {
            // Phasor recursion instead of std::sin per sample (exact enough over 4 s in double).
            const double w = 2.0 * pstest::kPi * h * f0 / sr;
            const double cw = std::cos (w), sw = std::sin (w);
            double re = std::cos (0.3 * h * f0 / 110.0), im = std::sin (0.3 * h * f0 / 110.0);
            const double a = 0.2 * 2.0 / pstest::kPi / h;

            for (size_t i = 0; i < n; ++i)
            {
                x[i] += (float) (a * im);
                const double nextRe = re * cw - im * sw;
                im = re * sw + im * cw;
                re = nextRe;
            }
        }
    }

    const auto p = analyseStereo (x, x, sr);

    std::vector<float> values;
    for (int k = 0; k < ps::kCurvePoints; ++k)
        if (ps::curveFrequency (k) >= 100.0f && ps::curveFrequency (k) <= 10000.0f)
            values.push_back (p.curveDb[(size_t) k]);

    REQUIRE (values.size() > 100);
    double sumSquares = 0.0;
    for (size_t i = 2; i < values.size(); ++i)
    {
        const double d2 = (double) values[i] - 2.0 * values[i - 1] + values[i - 2];
        sumSquares += d2 * d2;
    }

    const double rough = std::sqrt (sumSquares / (double) (values.size() - 2));
    CHECK (rough < 0.6);
    pstest::note ("saw triad: RMS second difference of the curve, dB", rough);
}

TEST_CASE ("ta. push(), profile(), process() and the getters do not allocate")
{
    const auto l = pstest::whiteNoise (0.3, 4.0, 48000.0, 41);
    const auto r = pstest::whiteNoise (0.3, 4.0, 48000.0, 42);

    ps::TrackAnalyser a;
    a.prepare (48000.0, Averaging::Recent);
    ps::LoudnessMeter m;
    m.prepare (48000.0);
    ps::TruePeakMeter tp;

    pstest::AllocationScope scope;
    float sink = 0.0f;

    for (size_t pos = 0; pos + 256 <= l.size(); pos += 256)
    {
        a.push (l.data() + pos, r.data() + pos, 256);
        a.push (l.data() + pos, nullptr, 0);
        m.process (l.data() + pos, r.data() + pos, 256);
        tp.process (l.data() + pos, r.data() + pos, 256);
        sink += m.momentaryLufs() + m.shortTermLufs() + m.integratedLufs() + m.loudnessRange() + tp.truePeakDb();
    }

    // profile() (bands, Hann-smoothed curve, spectral loudness) in both averaging modes, on populated averages.
    for (auto mode : { Averaging::Recent, Averaging::SinceReset })
    {
        a.setAveraging (mode);
        const auto p = a.profile();
        CHECK (p.valid);
        sink += p.curveDb[80] + p.bands[3].levelDb + p.spectralLoudnessDb;
    }

    a.reset();
    m.reset();
    tp.reset();

    CHECK_EQ (scope.count(), 0LL);
    CHECK (std::isfinite (sink));
}

//==============================================================================
TEST_CASE ("ta. analyseRegion analyses exactly the region")
{
    const double sr = 48000.0;
    const auto l = pstest::concat (pstest::sine (100.0, -20.0, 2.0, sr), pstest::sine (3000.0, -10.0, 3.0, sr));
    const auto r = pstest::scaled (l, 0.5f);

    const auto region = ps::analyseRegion (l.data(), r.data(), (long long) (2 * sr), (long long) l.size(), sr);
    const std::vector<float> l2 (l.begin() + (long) (2 * sr), l.end()), r2 (r.begin() + (long) (2 * sr), r.end());
    const auto direct = analyseStereo (l2, r2, sr);

    CHECK (region.valid);
    for (int b = 0; b < ps::kNumBands; ++b)
        CHECK_EQ (region.bands[(size_t) b].levelDb, direct.bands[(size_t) b].levelDb);
    CHECK_EQ (region.integratedLufs, direct.integratedLufs);
    CHECK (bandLevel (region, Band::Low) < bandLevel (region, Band::HighMid) - 30.0f);

    // Whole buffer, mono.
    const auto whole = ps::analyseRegion (l.data(), nullptr, 0, (long long) l.size(), sr);
    CHECK (whole.isMono);
    CHECK (whole.valid);

    // Empty / inverted regions.
    CHECK (! ps::analyseRegion (l.data(), r.data(), 1000, 1000, sr).valid);
    CHECK (! ps::analyseRegion (l.data(), r.data(), 5000, 1000, sr).valid);
    CHECK (! ps::analyseRegion (nullptr, nullptr, 0, 1000, sr).valid);
}

TEST_CASE ("ta. shortTermLoudnessCurve: 100 ms steps, first 29 unknown, then the meter's short-term value")
{
    const double sr = 44100.0;
    const auto x = pstest::sineSequence (1000.0, { { -20.0, 5.0 }, { -30.0, 5.05 } }, sr);
    const auto curve = ps::shortTermLoudnessCurve (x.data(), x.data(), (long long) x.size(), sr);

    REQUIRE (curve.size() == (size_t) (x.size() / 4410));
    CHECK_EQ (curve.size(), (size_t) 100);

    for (size_t i = 0; i < 29; ++i)
        CHECK_EQ (curve[i], ps::kNoValue);

    ps::LoudnessMeter m;
    m.prepare (sr);
    for (size_t i = 0; i < curve.size(); ++i)
    {
        m.process (x.data() + i * 4410, x.data() + i * 4410, 4410);
        CHECK_EQ (curve[i], m.shortTermLufs());
    }

    CHECK_NEAR (curve[29], -20.0, 0.1);   // window 0..3 s
    CHECK_NEAR (curve[49], -20.0, 0.1);   // window 2..5 s
    CHECK_NEAR (curve[99], -30.0, 0.1);   // window 7..10 s

    CHECK (ps::shortTermLoudnessCurve (nullptr, nullptr, 100000, sr).empty());

    // 11025 Hz: steps alternate 1102 / 1103 samples, so element i still ends at (i + 1) * 100 ms.
    const auto slow = pstest::sine (1000.0, -20.0, 120.0, 11025.0);
    const auto slowCurve = ps::shortTermLoudnessCurve (slow.data(), slow.data(), (long long) slow.size(), 11025.0);
    CHECK_EQ (slowCurve.size(), (size_t) 1200);
    CHECK_NEAR (slowCurve.back(), -20.0, 0.1);
    CHECK (ps::shortTermLoudnessCurve (x.data(), x.data(), 100, sr).empty());
}

TEST_CASE ("ta. shortTermAt picks the element ending at or before the time")
{
    // Distinct values everywhere so that an off-by-one cannot pass.
    std::vector<float> curve (40);
    for (size_t i = 0; i < curve.size(); ++i)
        curve[i] = -10.0f - (float) i;

    CHECK_EQ (ps::shortTermAt (curve, 3.0), -39.0f);    // element 29 ends at 3.0 s
    CHECK_EQ (ps::shortTermAt (curve, 3.05), -39.0f);
    CHECK_EQ (ps::shortTermAt (curve, 3.1), -40.0f);
    CHECK_EQ (ps::shortTermAt (curve, 0.3), -12.0f);    // exact multiples of 0.1 s are not rounded down
    CHECK_EQ (ps::shortTermAt (curve, 0.7), -16.0f);
    CHECK_EQ (ps::shortTermAt (curve, 0.1), -10.0f);
    CHECK_EQ (ps::shortTermAt (curve, 0.19), -10.0f);
    CHECK_EQ (ps::shortTermAt (curve, 2.95), -38.0f);
    CHECK_EQ (ps::shortTermAt (curve, 4.0), -49.0f);
    CHECK_EQ (ps::shortTermAt (curve, 4.09), -49.0f);
    CHECK_EQ (ps::shortTermAt (curve, 4.15), ps::kNoValue);
    CHECK_EQ (ps::shortTermAt (curve, 0.05), ps::kNoValue);
    CHECK_EQ (ps::shortTermAt (curve, -1.0), ps::kNoValue);
    CHECK_EQ (ps::shortTermAt (curve, std::numeric_limits<double>::quiet_NaN()), ps::kNoValue);
    CHECK_EQ (ps::shortTermAt ({}, 5.0), ps::kNoValue);
}

TEST_CASE ("ta. makeWaveformOverview: min / max of (L + R) / 2 per column, clamped")
{
    std::vector<float> ramp (1000);
    for (size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = -1.5f + 3.0f * (float) i / 999.0f;

    const auto o = ps::makeWaveformOverview (ramp.data(), nullptr, (long long) ramp.size(), 10);
    REQUIRE (o.minimum.size() == 10);
    REQUIRE (o.maximum.size() == 10);
    CHECK_EQ (o.minimum[0], -1.0f);
    CHECK_EQ (o.maximum[9], 1.0f);
    CHECK_NEAR (o.minimum[5], ramp[500], 1.0e-6);
    CHECK_NEAR (o.maximum[5], ramp[599], 1.0e-6);
    for (size_t c = 0; c < 10; ++c)
        CHECK (o.minimum[c] <= o.maximum[c]);

    // L = -R sums to silence.
    const auto cancel = ps::makeWaveformOverview (ramp.data(), pstest::scaled (ramp, -1.0f).data(), 1000, 4);
    for (size_t c = 0; c < 4; ++c)
    {
        CHECK_EQ (cancel.minimum[c], 0.0f);
        CHECK_EQ (cancel.maximum[c], 0.0f);
    }

    // More columns than samples: every column still shows a sample.
    const float three[] = { 0.1f, -0.2f, 0.3f };
    const auto wide = ps::makeWaveformOverview (three, nullptr, 3, 8);
    REQUIRE (wide.minimum.size() == 8);
    CHECK_EQ (wide.minimum[0], 0.1f);
    CHECK_EQ (wide.maximum[7], 0.3f);

    CHECK (ps::makeWaveformOverview (ramp.data(), nullptr, 1000, 0).minimum.empty());
    const auto none = ps::makeWaveformOverview (nullptr, nullptr, 0, 5);
    CHECK_EQ (none.minimum.size(), (size_t) 5);
    CHECK_EQ (none.maximum[4], 0.0f);
}
