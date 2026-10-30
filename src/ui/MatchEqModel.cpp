#include "ui/MatchEqModel.h"

#include <cmath>

// Match EQ panel model (owner: ui-meq). See MatchEqModel.h.
namespace ps::ui
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr float kMinLineOctaves = 1.0f / 24.0f;   // display lines are never smoothed less than this

bool hasValue (float v) noexcept { return std::isfinite (v) && v > ps::kMatchNoValue + 1.0f; }

// Smoothed PSD (dB) of one channel of a spectrum, like computeMatchCurve step 2 (holes stay holes here: the display
// breaks the line there instead of holding it flat). `has` receives which points have a value.
void smoothLine (const ps::MatchSpectrum& s, ps::MatchChannel channel, double halfPoints,
                 std::array<float, ps::kMatchPoints>& outDb, std::array<bool, ps::kMatchPoints>& has)
{
    std::array<double, ps::kMatchPoints> power {};

    for (int i = 0; i < ps::kMatchPoints; ++i)
    {
        const float v = s.valueDb (channel, i);
        has[(size_t) i] = hasValue (v);
        power[(size_t) i] = has[(size_t) i] ? std::pow (10.0, (double) v / 10.0) : 0.0;
    }

    const int reach = juce::jmin (ps::kMatchPoints - 1, (int) std::ceil (halfPoints) - 1);
    std::array<double, ps::kMatchPoints> kernel {};

    for (int j = 0; j <= reach; ++j)
        kernel[(size_t) j] = 0.5 + 0.5 * std::cos (kPi * (double) j / halfPoints);

    for (int i = 0; i < ps::kMatchPoints; ++i)
    {
        if (! has[(size_t) i])
        {
            outDb[(size_t) i] = ps::kMatchNoValue;
            continue;
        }

        double sum = 0.0, weight = 0.0;
        const int j0 = juce::jmax (0, i - reach), j1 = juce::jmin (ps::kMatchPoints - 1, i + reach);

        for (int j = j0; j <= j1; ++j)
        {
            if (! has[(size_t) j])
                continue;

            const double w = kernel[(size_t) std::abs (j - i)];
            sum += w * power[(size_t) j];
            weight += w;
        }

        const double p = weight > 0.0 ? sum / weight : power[(size_t) i];
        outDb[(size_t) i] = p > 1.0e-30 ? (float) (10.0 * std::log10 (p)) : ps::kMatchNoValue;

        if (outDb[(size_t) i] <= ps::kMatchNoValue)
            has[(size_t) i] = false;
    }
}
} // namespace

//==============================================================================
MatchEqModel::MatchEqModel (PureSheenProcessor& p) : processor (p)
{
    const auto& specs = ps::paramSpecs();
    values.assign (specs.size(), 0.0f);
    auto& state = processor.getValueTreeState();

    for (size_t i = 0; i < specs.size(); ++i)
    {
        if (specs[i].module != (int) ps::ModuleType::MatchEq)
            continue;

        indices.push_back ((int) i);
        raw.push_back (state.getRawParameterValue (specs[i].id));
        values[i] = specs[i].defaultValue;
    }

    for (auto& s : spectra)
        for (auto& ch : s.psdDb)
            ch.fill (ps::kMatchNoValue);

    readParams();
}

void MatchEqModel::readParams()
{
    ps::readMatchEqParams (values.data(), params);
}

ps::MatchEq& MatchEqModel::module (int partIndex) const
{
    return static_cast<ps::MatchEq&> (processor.getChain().getModule (ps::ModuleType::MatchEq, partIndex == 1 ? 1 : 0));
}

juce::String MatchEqModel::setName (int s) const { return channelSetName (mode, s); }

juce::Colour MatchEqModel::setColour() const
{
    return dual && set == 1 ? Colours::sideChannel : moduleColour (ps::ModuleType::MatchEq);
}

std::vector<int> MatchEqModel::editSets() const
{
    if (isLinked())
        return { set, 1 - set };

    return { set };
}

std::vector<int> MatchEqModel::partsForSpectrumActions() const
{
    if (mode == ps::ChannelMode::TransientSustain && isLinked())
        return { part, 1 - part };

    return { part };
}

juce::String MatchEqModel::setId (int s, const char* name) const
{
    return juce::String ("meq.") + (s == 1 ? "b." : "a.") + name;
}

juce::String MatchEqModel::moduleId (const char* name) { return juce::String ("meq.") + name; }

juce::RangedAudioParameter* MatchEqModel::setParam (int s, const char* name) const
{
    return processor.getParameter (setId (s, name));
}

bool MatchEqModel::isDesignPending() const { return shownModule().isDesignPending(); }

int MatchEqModel::getLatencySamples() const { return module (0).getLatencySamples(); }

double MatchEqModel::getSampleRate() const
{
    const double sr = processor.getChain().getSampleRate();
    return sr > 1000.0 ? sr : 48000.0;
}

juce::String MatchEqModel::sourceName (const ps::MatchSpectrum& s)
{
    const juce::String tag (s.source.data());

    if (tag.isEmpty() || tag == "capture")
        return "Captured";

    if (tag.startsWith ("file:"))
        return tag.fromFirstOccurrenceOf ("file:", false, false);

    if (tag.startsWith ("import:"))
        return tag.fromFirstOccurrenceOf ("import:", false, false);

    if (tag == "synth")
        return "Synthesised curve";

    return tag;
}

//==============================================================================
int MatchEqModel::update (int headerSet)
{
    int changes = 0;

    // Parameters
    bool paramsMoved = first;

    for (size_t k = 0; k < indices.size(); ++k)
    {
        if (raw[k] == nullptr)
            continue;

        const float v = raw[k]->load (std::memory_order_relaxed);
        float& slot = values[(size_t) indices[k]];

        if (v != slot)
        {
            slot = v;
            paramsMoved = true;
        }
    }

    if (paramsMoved)
    {
        readParams();
        changes |= paramsChanged;
    }

    // Channel sets
    const auto newMode = currentChannelMode (processor, ps::ModuleType::MatchEq);
    const bool newDual = isDualChannelMode (newMode);
    const bool newLinked = processor.getParameterValue ("meq.link") >= 0.5f;
    const int newSet = newDual && headerSet == 1 ? 1 : 0;
    const int newPart = newMode == ps::ChannelMode::TransientSustain ? newSet : 0;

    if (first || newMode != mode || newDual != dual || newLinked != linked || newSet != set || newPart != part)
    {
        mode = newMode;
        dual = newDual;
        linked = newLinked;
        set = newSet;
        part = newPart;
        changes |= modeChanged;
    }

    // Stored spectra of the shown part
    auto& m = shownModule();
    const unsigned generation = m.getSpectrumGeneration();

    if (first || part != seenPart || generation != seenGeneration)
    {
        seenPart = part;
        seenGeneration = generation;
        spectra[0] = m.getSpectrum (ps::MatchRole::Reference);
        spectra[1] = m.getSpectrum (ps::MatchRole::Current);
        changes |= spectraChanged;
    }

    // Capture
    ps::MatchRole role = ps::MatchRole::Reference;
    const bool nowCapturing = m.isCapturing (&role);
    const double seconds = m.getCaptureSeconds();
    const int gaps = m.getCaptureGaps();

    if (nowCapturing != capturing || role != captureRole || std::abs (seconds - captureSeconds) > 0.05 || gaps != captureGaps)
    {
        capturing = nowCapturing;
        captureRole = role;
        captureSeconds = seconds;
        captureGaps = gaps;
        changes |= captureChanged;
    }

    if (capturing)
    {
        if (m.getRunningSpectrum (runningSpectrum))
        {
            if (! runningValid || runningSpectrum.seconds != runningSeconds)
            {
                runningValid = true;
                runningSeconds = runningSpectrum.seconds;
                changes |= runningChanged;
            }
        }
        else if (runningValid)
        {
            runningValid = false;
            changes |= runningChanged;
        }
    }
    else if (runningValid)
    {
        runningValid = false;
        runningSeconds = -1.0;
        changes |= runningChanged;
    }

    // File loader
    const float progress = processor.getMatchReferenceFileProgress();

    if (std::abs (progress - loadProgress) > 0.001f)
    {
        loadProgress = progress;
        changes |= loaderChanged;
    }

    if ((changes & (paramsChanged | spectraChanged | runningChanged | modeChanged)) != 0)
        recomputeCurves();

    first = false;
    return changes;
}

//==============================================================================
void MatchEqModel::recomputeCurves()
{
    const auto channel = ps::matchChannelFor (mode, set);
    const auto& sp = params.sets[set];

    // The installed curve
    ps::computeMatchCurve (spectra[0], spectra[1], channel, sp, curve.data());
    curveActive = spectra[0].valid && spectra[1].valid;
    curveMax = 0.0f;
    curveMin = 0.0f;

    for (int i = 0; i < ps::kMatchPoints; ++i)
    {
        if (ps::matchGridHz (i) < 20.0 || ps::matchGridHz (i) > 20000.0)
            continue;

        curveMax = juce::jmax (curveMax, curve[(size_t) i]);
        curveMin = juce::jmin (curveMin, curve[(size_t) i]);
    }

    // The display lines
    const double halfPoints = juce::jmax (1.0, (double) juce::jmax ((float) ps::matchSmoothingOctaves (sp.smoothPercent), kMinLineOctaves) * 48.0);
    std::array<bool, ps::kMatchPoints> has[3] {};
    const ps::MatchSpectrum* sources[3] { &spectra[0], &spectra[1], runningValid ? &runningSpectrum : nullptr };

    for (int k = 0; k < 3; ++k)
    {
        auto& line = lines[k];
        line.valid = sources[k] != nullptr && (sources[k]->valid || k == 2);

        if (line.valid)
            smoothLine (*sources[k], channel, halfPoints, line.db, has[k]);
        else
            has[k].fill (false);

        if (line.valid)
        {
            bool any = false;

            for (bool h : has[k])
                any = any || h;

            line.valid = any;
        }
    }

    // Level normalisation: each line over the points it shares with the other role's line (when that one exists).
    for (int k = 0; k < 3; ++k)
    {
        auto& line = lines[k];

        if (! line.valid)
            continue;

        const int otherRole = k == 2 ? (captureRole == ps::MatchRole::Current ? 0 : 1) : 1 - k;
        std::array<bool, ps::kMatchPoints> mask {};

        for (int i = 0; i < ps::kMatchPoints; ++i)
            mask[(size_t) i] = has[k][(size_t) i] && (! lines[otherRole].valid || has[otherRole][(size_t) i]);

        double level = ps::matchWeightedLevelDb (line.db.data(), mask.data());

        if (level <= (double) ps::kMatchNoValue)
            level = ps::matchWeightedLevelDb (line.db.data(), has[k].data());

        if (level <= (double) ps::kMatchNoValue)
        {
            line.valid = false;
            continue;
        }

        for (int i = 0; i < ps::kMatchPoints; ++i)
            line.db[(size_t) i] = has[k][(size_t) i] ? (float) ((double) line.db[(size_t) i] - level) : ps::kMatchNoValue;
    }
}
} // namespace ps::ui
