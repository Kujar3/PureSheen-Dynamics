#pragma once

#include "ui/Widgets.h"

#include "dsp/MatchAnalysis.h"
#include "dsp/MatchEq.h"

#include <array>
#include <vector>

// Match EQ panel helpers (owner: ui-meq): a cached view of the Match EQ's parameters, spectra, capture and file-load
// state, and the curves the display draws. Internal to MatchEqPanel / MatchDisplay (not a team contract).
// Message thread only. update() is called from the panel's 30 Hz tick.
//
// Which instance: the spectra and the capture of channel set `set` live in moduleForChannelSet (part 1 = the T/S twin
// in Transient/Sustain, part 0 otherwise; in M/S and L/R both sets share part 0's spectra and differ only by the
// channel they match, ps::matchChannelFor).
//
// Curves (all on the 540-point ps::matchGridHz grid):
//   - curveDb: ps::computeMatchCurve (reference, current, matchChannelFor (mode, set), the set's parameters as
//     ps::readMatchEqParams reads them), i.e. exactly the curve the module installs for that set
//   - reference / current / running lines: the chosen channel's PSD smoothed like computeMatchCurve step 2 (power
//     domain raised cosine over log2 f, half-width matchSmoothingOctaves, at least 1/24 octave for readability) and
//     level-normalised like step 3 (ps::matchWeightedLevelDb over the points both lines have), so both lines sit at
//     the same loudness. No display tilt (the display adds it).
namespace ps::ui
{
class MatchEqModel
{
public:
    explicit MatchEqModel (PureSheenProcessor&);

    enum Change : int
    {
        paramsChanged  = 1,
        spectraChanged = 2,    // the stored reference / current of the shown part
        captureChanged = 4,    // capture state, seconds, gaps
        runningChanged = 8,    // a new running spectrum (during a capture)
        modeChanged    = 16,   // channel mode, Link, shown set / part
        loaderChanged  = 32    // file-load progress / error
    };

    // Re-reads everything; headerSet = ModulePanel::getChannelSet(). Returns a mask of Change flags.
    int update (int headerSet);

    PureSheenProcessor& processor;

    //==============================================================================
    // Channel sets
    ps::ChannelMode getMode() const noexcept { return mode; }
    bool isDual() const noexcept { return dual; }
    bool isLinked() const noexcept { return dual && linked; }
    int getSet() const noexcept { return set; }               // 0 / 1 (0 in Stereo)
    int getPart() const noexcept { return part; }             // module instance of the shown set
    juce::String setName (int s) const;                       // "Mid" / "Side" / ... ("" in Stereo)
    juce::Colour setColour() const;                           // module colour (set A), Colours::sideChannel (set B)
    std::vector<int> editSets() const;                        // the shown set (+ the other one when linked)
    std::vector<int> partsForSpectrumActions() const;         // the shown part (+ the twin in T/S when linked)

    juce::String setId (int s, const char* name) const;       // "meq.a.amount"
    static juce::String moduleId (const char* name);          // "meq.phase"
    juce::RangedAudioParameter* setParam (int s, const char* name) const;

    const ps::MatchEqParams& getParams() const noexcept { return params; }
    const ps::MatchEqSetParams& getSetParams() const noexcept { return params.sets[set]; }

    ps::MatchEq& module (int partIndex) const;
    ps::MatchEq& shownModule() const { return module (part); }

    //==============================================================================
    // Spectra of the shown part
    const ps::MatchSpectrum& spectrum (ps::MatchRole r) const noexcept { return spectra[r == ps::MatchRole::Current ? 1 : 0]; }
    bool has (ps::MatchRole r) const noexcept { return spectrum (r).valid; }

    // Capture of the shown part
    bool isCapturing() const noexcept { return capturing; }
    ps::MatchRole getCaptureRole() const noexcept { return captureRole; }
    double getCaptureSeconds() const noexcept { return captureSeconds; }
    int getCaptureGaps() const noexcept { return captureGaps; }
    bool hasRunning() const noexcept { return runningValid; }
    const ps::MatchSpectrum& running() const noexcept { return runningSpectrum; }

    bool isDesignPending() const;
    int getLatencySamples() const;
    double getSampleRate() const;

    // Reference file loader (processor)
    bool isLoading() const noexcept { return loadProgress >= 0.0f; }
    float getLoadProgress() const noexcept { return loadProgress; }
    juce::String getLoadError() const { return processor.getMatchReferenceFileError(); }

    //==============================================================================
    // Curves (see the header comment)
    struct Line
    {
        bool valid = false;
        std::array<float, ps::kMatchPoints> db {};
    };
    const std::array<float, ps::kMatchPoints>& getCurve() const noexcept { return curve; }
    bool isCurveActive() const noexcept { return curveActive; }   // both spectra valid (else the curve is flat)
    float getCurveMax() const noexcept { return curveMax; }
    float getCurveMin() const noexcept { return curveMin; }
    const Line& referenceLine() const noexcept { return lines[0]; }
    const Line& currentLine() const noexcept { return lines[1]; }
    const Line& runningLine() const noexcept { return lines[2]; }

    // Readable name of a spectrum's source tag ("capture" -> "Captured", "file:Song" -> "Song", "synth" -> ...).
    static juce::String sourceName (const ps::MatchSpectrum&);

private:
    void readParams();
    void recomputeCurves();

    std::vector<int> indices;
    std::vector<std::atomic<float>*> raw;
    std::vector<float> values;
    ps::MatchEqParams params;
    bool first = true;

    ps::ChannelMode mode = ps::ChannelMode::Stereo;
    bool dual = false, linked = true;
    int set = 0, part = 0;

    ps::MatchSpectrum spectra[2];
    unsigned seenGeneration = 0;
    int seenPart = -1;

    bool capturing = false, runningValid = false;
    ps::MatchRole captureRole = ps::MatchRole::Reference;
    double captureSeconds = 0.0, runningSeconds = -1.0;
    int captureGaps = 0;
    ps::MatchSpectrum runningSpectrum;

    float loadProgress = -1.0f;

    std::array<float, ps::kMatchPoints> curve {};
    bool curveActive = false;
    float curveMax = 0.0f, curveMin = 0.0f;
    Line lines[3];
};
} // namespace ps::ui
