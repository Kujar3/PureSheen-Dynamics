#include "plugin/PluginProcessor.h"

#include "dsp/Maximizer.h"
#include "plugin/FactoryPresets.h"
#include "plugin/MatchEqState.h"   // (phase 2) Match EQ state child, spectrum undo, reference-file loader
#include "plugin/PluginState.h"
#include "plugin/Presets.h"
#include "plugin/UndoHistory.h"
#include "ui/PluginEditor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <optional>

// PureSheen Dynamics processor (owner: plugin).
//
// Choices made where neither the contract nor the research decides:
// - Undo history: full-state snapshots (every parameter + chain order). Gestures (host or UI) are committed ~250 ms
//   after the last gesture ended, so a drag, a double-click reset or a mouse-wheel burst is one step. Host automation
//   (value changes without a gesture) is never recorded by itself. Bypass and Gain Match are listening aids: they are
//   not recorded, not restored by undo / snapshots and not stored in presets (the host project state keeps them).
// - addModule() brings a module in at its default settings (a fresh module, like the reference suite), as part of the "Added X"
//   step; setChainOrder() keeps the parameters as they are (drag reorder / re-add from undo).
// - resetModule() resets every parameter of the module except its power switch.
// - Presets: a global preset sets every parameter except Bypass / Gain Match plus the chain; a module preset sets that
//   module's parameters except its power switch. "Modified" = the current state differs from the state right after
//   the last preset load / save (or the restored project).
// - Learn crossovers: 1/3-octave smoothed, pink-compensated (+3 dB/oct) long-term spectrum; each crossover goes to the
//   most prominent local minimum in its region (1 crossover: 100 Hz-1 kHz; 2: 80-500 Hz, 1.5-8 kHz; 3: 60-300 Hz,
//   0.5-3.5 kHz, 4-12 kHz), at least 3/4 octave above the previous one. With a single band all three are learned.
namespace
{
const juce::Identifier kStateTag ("PureSheen"), kChainTag ("Chain"), kOptionsTag ("Options"), kUiLegacyTag ("UiState"),
    kSnapshotsTag ("Snapshots"), kSnapshotTag ("Snapshot"), kSlot ("slot"), kEditorW ("editorW"), kEditorH ("editorH"),
    kPresetName ("presetName"), kPresetModified ("presetModified"), kVersion ("version"), kHistoryDepth ("historyDepth"),
    kActiveSnapshot ("activeSnapshot");

constexpr int kStateVersion = 1;
constexpr double kGestureSettleMs = 250.0;
constexpr double kStuckGestureMs = 10000.0;   // a gesture the host never ended stops blocking the history after 10 s

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    const auto& specs = ps::paramSpecs();
    for (size_t index = 0; index < specs.size(); ++index)
    {
        const auto& s = specs[index];
        // version hint 1: the 0.1.1 table; 2: the phase 2 appendix (append-only, so the index decides)
        const juce::ParameterID pid { juce::String (s.id), index < (size_t) ps::kNumParams_0_1_1 ? 1 : 2 };
        const juce::String name (s.name);

        switch (s.kind)
        {
            case ps::ParamKind::Bool:
                layout.add (std::make_unique<juce::AudioParameterBool> (pid, name, s.defaultValue >= 0.5f));
                break;

            case ps::ParamKind::Choice:
            {
                juce::StringArray choices;
                for (const auto& c : s.choices)
                    choices.add (juce::String (c));
                layout.add (std::make_unique<juce::AudioParameterChoice> (pid, name, choices, (int) s.defaultValue));
                break;
            }

            case ps::ParamKind::Float:
            default:
            {
                juce::NormalisableRange<float> range (s.minValue, s.maxValue, s.interval);
                if (s.skewCentre > s.minValue && s.skewCentre < s.maxValue)
                    range.setSkewForCentre (s.skewCentre);
                // Host text: 2 decimals (1 from 1000 up), so text -> value -> text round-trips exactly (the default
                // 7-decimal text drifts in the last digit through the skewed range; CLAP hosts check this).
                layout.add (std::make_unique<juce::AudioParameterFloat> (
                    pid, name, range, s.defaultValue,
                    juce::AudioParameterFloatAttributes()
                        .withLabel (juce::String (s.unit))
                        .withStringFromValueFunction ([] (float v, int) { return juce::String (v, std::abs (v) >= 1000.0f ? 1 : 2); })
                        .withValueFromStringFunction ([] (const juce::String& t) { return t.getFloatValue(); })));
                break;
            }
        }
    }
    return layout;
}

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

bool validModule (int t) noexcept { return t >= 0 && t < ps::kNumModuleTypes; }

bool isMultiband (ps::ModuleType t) noexcept
{
    return t == ps::ModuleType::Dynamics || t == ps::ModuleType::Imager || t == ps::ModuleType::Exciter
           || t == ps::ModuleType::Punch;
}

juce::String slotName (int slot) { return juce::String::charToString ((juce::juce_wchar) ('A' + slot)); }

//==============================================================================
// Crossover learning (see the comment at the top).
struct SmoothedSpectrum
{
    static constexpr int kPointsPerOctave = 24;
    std::vector<double> hz, db;   // log grid 20 Hz .. top, pink-compensated dB

    bool build (const std::vector<float>& magnitudeDb, double binHz)
    {
        hz.clear();
        db.clear();
        if (magnitudeDb.size() < 16 || ! (binHz > 0.0))
            return false;

        const double top = std::min (20000.0, binHz * (double) (magnitudeDb.size() - 1));
        double maxDb = -1000.0;

        for (int k = 0;; ++k)
        {
            const double f = 20.0 * std::pow (2.0, (double) k / kPointsPerOctave);
            if (f > top)
                break;

            // 1/3-octave power average (at least the nearest bin).
            const double lo = f * std::pow (2.0, -1.0 / 6.0), hi = f * std::pow (2.0, 1.0 / 6.0);
            auto b0 = (size_t) std::max (1.0, std::ceil (lo / binHz));
            auto b1 = (size_t) std::min ((double) magnitudeDb.size() - 1.0, std::floor (hi / binHz));
            double power = 0.0;
            int count = 0;
            for (auto b = b0; b <= b1; ++b)
            {
                const float v = magnitudeDb[b];
                if (std::isfinite (v))
                {
                    power += std::pow (10.0, (double) v / 10.0);
                    ++count;
                }
            }
            if (count == 0)
            {
                const auto nearest = (size_t) juce::jlimit (1.0, (double) magnitudeDb.size() - 1.0, std::round (f / binHz));
                const float v = magnitudeDb[nearest];
                power = std::isfinite (v) ? std::pow (10.0, (double) v / 10.0) : 1.0e-30;
                count = 1;
            }

            const double level = 10.0 * std::log10 (std::max (1.0e-30, power / count));
            maxDb = std::max (maxDb, level);
            hz.push_back (f);
            db.push_back (level + 3.0 * std::log2 (f / 1000.0));
        }

        return hz.size() > (size_t) kPointsPerOctave && maxDb > -140.0;   // -140: no audio yet
    }

    // Most prominent local minimum in [lo, hi]; the region minimum when there is no local minimum.
    double pickMinimum (double lo, double hi) const
    {
        constexpr int kLocal = 2, kWide = kPointsPerOctave / 2;   // +-1/12 octave, +-1/2 octave
        int best = -1, lowest = -1;
        double bestScore = -1.0e9;
        const int n = (int) hz.size();

        for (int i = 0; i < n; ++i)
        {
            if (hz[(size_t) i] < lo || hz[(size_t) i] > hi)
                continue;
            if (lowest < 0 || db[(size_t) i] < db[(size_t) lowest])
                lowest = i;

            bool isMin = true;
            for (int j = std::max (0, i - kLocal); j <= std::min (n - 1, i + kLocal) && isMin; ++j)
                isMin = j == i || db[(size_t) i] <= db[(size_t) j];
            if (! isMin || i < kLocal || i >= n - kLocal)
                continue;

            double sum = 0.0;
            int count = 0;
            for (int j = std::max (0, i - kWide); j <= std::min (n - 1, i + kWide); ++j)
            {
                sum += db[(size_t) j];
                ++count;
            }
            const double score = sum / count - db[(size_t) i];
            if (score > bestScore)
            {
                bestScore = score;
                best = i;
            }
        }

        if (best < 0)
            best = lowest;
        return best >= 0 ? hz[(size_t) best] : std::sqrt (lo * hi);
    }
};
} // namespace

//==============================================================================
struct PureSheenProcessor::Impl final : private juce::Timer,
                                        private juce::AudioProcessorParameter::Listener
{
    explicit Impl (PureSheenProcessor& p)
        : owner (p), apvts (p, nullptr, "PARAMS", createLayout())
    {
        const auto& specs = ps::paramSpecs();
        params.reserve (specs.size());
        raw.reserve (specs.size());
        for (const auto& s : specs)
        {
            const juce::String id (s.id);
            params.push_back (apvts.getParameter (id));
            raw.push_back (apvts.getRawParameterValue (id));
            jassert (params.back() != nullptr && raw.back() != nullptr);
        }
        values = psplug::defaultValues();

        for (auto* prm : params)
            prm->addListener (this);

        channelSolo.fill (-1);
        bypassIndex = ps::paramIndex ("io.bypass");

        history.reset (capture());
        presetBaseline = capture();

        // ---- Match EQ (MatchEqState.cpp)
        matchEq = std::make_unique<psplug::MatchEqState> (chain);
        matchEq->historyReset (history);
        // ----

        startTimerHz (30);
    }

    ~Impl() override
    {
        stopTimer();
        for (auto* prm : params)
            prm->removeListener (this);
    }

    PureSheenProcessor& owner;
    juce::AudioProcessorValueTreeState apvts;
    ps::Chain chain;
    std::vector<juce::RangedAudioParameter*> params;   // paramSpecs() order
    std::vector<std::atomic<float>*> raw;
    std::vector<float> values;                         // audio thread: plain values handed to the chain
    int bypassIndex = -1;

    // Audio setup
    bool prepared = false;
    int maxBlock = 0;

    // Monitoring
    std::array<int, ps::kNumModuleTypes> channelSolo {};
    std::array<ps::ModuleMonitor, ps::kNumModuleTypes> monitors {};

    // Saved extras (getStateInformation may run on another thread)
    juce::CriticalSection extrasLock;
    juce::ValueTree uiState { kOptionsTag };
    int editorW = 0, editorH = 0;
    std::array<std::optional<psplug::FullState>, kNumSnapshots> snapshots;
    int activeSnapshot = -1;
    juce::String presetName { "Default" };

    // Presets
    juce::File presetRoot { psplug::defaultPresetRoot() };
    psplug::FullState presetBaseline;
    bool presetForcedModified = false;

    // History. editLock serialises the timer's housekeeping with a state load from a host thread (both are normally
    // on the message thread, so it is uncontended).
    juce::CriticalSection editLock;
    psplug::UndoHistory history;
    std::atomic<bool> applying { false };      // our own edits: gesture callbacks ignored
    std::atomic<int> activeGestures { 0 };
    std::atomic<bool> gesturePending { false };
    std::atomic<int> lastGestureParam { -1 };
    std::atomic<double> lastGestureEventMs { 0.0 };

    // Learn
    struct LearnSpectrum
    {
        std::vector<float> db;
        double binHz = 0.0;
    };
    std::array<LearnSpectrum, ps::kNumModuleTypes> learnSpectra;
    bool maxLearnActive = false;

    // ---- Match EQ (MatchEqState.cpp): spectra in the state / snapshots / presets, spectrum undo, file loader.
    // Declared after `chain`, so it (and its loader thread) is destroyed first.
    std::unique_ptr<psplug::MatchEqState> matchEq;

    // A spectrum change is an undo step even when no parameter changed.
    void recordMatchEq (const juce::String& description)
    {
        history.record (description, capture(), -1, nowMs());
        matchEq->historyRecorded (history);
        {
            const juce::ScopedLock sl (extrasLock);
            activeSnapshot = -1;
        }
        owner.sendChangeMessage();
    }

    // Records a pending spectrum change (capture stop, Clear, loaded file) as its own step.
    void matchEqHousekeeping()
    {
        juce::String description;
        if (matchEq != nullptr && matchEq->checkChange (description))
        {
            commitGesture (true);
            matchEq->acceptChange();
            recordMatchEq (description);
        }
    }
    // ----

    ps::Maximizer& maximizer() noexcept
    {
        return static_cast<ps::Maximizer&> (chain.getModule (ps::ModuleType::Maximizer));
    }

    //==============================================================================
    psplug::FullState capture() const
    {
        psplug::FullState s;
        s.values.resize (raw.size());
        for (size_t i = 0; i < raw.size(); ++i)
            s.values[i] = raw[i]->load (std::memory_order_relaxed);
        s.order = chain.getOrder();
        return s;
    }

    // Sets one parameter like a host edit (gesture + set unless withGesture is false). Callers mute the gesture
    // listener (applying) around it.
    void setParam (int index, float plain, bool withGesture = true)
    {
        auto* p = params[(size_t) index];
        const float target = psplug::sanitiseValue (index, plain);
        if (raw[(size_t) index]->load() == target)
            return;
        if (withGesture)
            p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (target));
        if (withGesture)
            p->endChangeGesture();
    }

    // Applies values (and optionally the chain order). mask(i) chooses the parameters.
    template <typename Mask>
    void applyValues (const std::vector<float>& target, Mask&& mask)
    {
        const bool was = applying.exchange (true);
        for (size_t i = 0; i < target.size() && i < params.size(); ++i)
            if (mask ((int) i) && ! psplug::sameValue ((int) i, raw[i]->load(), target[i]))
                setParam ((int) i, target[i]);
        applying.store (was);
    }

    void applyState (const psplug::FullState& s, bool includeChain, int scope = -1)
    {
        applyValues (s.values, [scope] (int i) { return psplug::isInPresetScope (i, scope); });
        if (includeChain && s.order != chain.getOrder())
            setOrder (s.order);
    }

    void setOrder (const std::vector<ps::ModuleType>& order)
    {
        chain.setOrder (order);
        if (chain.getSolo() >= 0 && ! chain.isInChain ((ps::ModuleType) chain.getSolo()))
            chain.setSolo (-1);
        updateLatency();
        owner.sendChangeMessage();
    }

    void updateLatency()
    {
        if (! prepared)
            return;
        const int l = chain.getLatencySamples();
        if (l != owner.getLatencySamples())
            owner.setLatencySamples (l);
    }

    //==============================================================================
    // History
    bool record (const juce::String& description, int singleParam = -1)
    {
        auto s = capture();
        if (psplug::sameState (s, history.current().state))
            return false;
        history.record (description, s, singleParam, nowMs());
        matchEq->historyRecorded (history);   // Match EQ: tag the step with the spectra
        {
            const juce::ScopedLock sl (extrasLock);
            activeSnapshot = -1;
        }
        owner.sendChangeMessage();
        return true;
    }

    void commitGesture (bool force)
    {
        if (! gesturePending.load())
            return;

        const double t = nowMs();
        if (activeGestures.load() > 0)
        {
            if (! force && t - lastGestureEventMs.load() < kStuckGestureMs)
                return;
            activeGestures.store (0);
        }
        if (! force && t - lastGestureEventMs.load() < kGestureSettleMs)
            return;

        gesturePending.store (false);

        const auto s = capture();
        const auto changed = psplug::changedParams (history.current().state, s);
        if (changed.empty())
            return;

        juce::String description;
        const int last = lastGestureParam.load();
        if (changed.size() == 1)
            description = psplug::describeParam (changed[0], s.values[(size_t) changed[0]]);
        else if (std::find (changed.begin(), changed.end(), last) != changed.end())
            description = psplug::describeParam (last, s.values[(size_t) last]) + " (+"
                          + juce::String ((int) changed.size() - 1) + " more)";
        else
            description = "Changed " + juce::String ((int) changed.size()) + " parameters";

        record (description, changed.size() == 1 ? changed[0] : -1);
    }

    void goTo (int index)
    {
        const juce::ScopedLock editSl (editLock);   // Match EQ: restore parameters and spectra as one host-save transaction
        const int selectedId = index >= 0 && index < history.size() ? history[index].id : -1;
        commitGesture (true);
        matchEqHousekeeping();   // Match EQ: a pending spectrum change becomes its own step first
        index = -1;
        for (int i = 0; i < history.size(); ++i)
            if (history[i].id == selectedId)
                index = i;
        if (index < 0 || index == history.getPosition())
            return;
        history.setPosition (index);
        applyState (history.current().state, true);
        matchEq->applyHistory (history);   // Match EQ: the step's spectra
        history.updateCurrentState (capture());
        {
            const juce::ScopedLock sl (extrasLock);
            activeSnapshot = -1;
        }
        owner.sendChangeMessage();
    }

    //==============================================================================
    // Presets
    bool applyPreset (const psplug::PresetData& d)
    {
        const juce::ScopedLock editSl (editLock);   // Match EQ: keep preset parameters paired with their reference spectra
        matchEqHousekeeping();   // Match EQ: preserve pending spectrum edits before replacing the preset
        commitGesture (true);
        const int scope = d.scope;
        applyValues (d.values, [scope] (int i) { return psplug::isInPresetScope (i, scope); });
        if (scope < 0)
            setOrder (d.order);

        const auto description = scope < 0 ? "Loaded preset " + d.name
                                            : "Loaded " + psplug::moduleName ((ps::ModuleType) scope) + " preset " + d.name;
        const bool spectraChanged = matchEq->applyPendingPreset();   // Match EQ: the preset's reference
        if (! record (description) && spectraChanged)
            recordMatchEq (description);

        if (scope < 0)
        {
            const juce::ScopedLock sl (extrasLock);
            presetName = d.name;
            presetBaseline = capture();
            presetForcedModified = false;
            matchEq->setPresetBaseline();   // Match EQ: global presets also baseline their reference spectra
        }
        owner.sendChangeMessage();
        return true;
    }

    //==============================================================================
    void housekeeping (bool force)
    {
        const juce::ScopedLock sl (editLock);
        matchEqHousekeeping();   // Match EQ: finished file loads, capture stops, clears
        commitGesture (force);
        updateLatency();

        if (maxLearnActive)
        {
            float gainDb = 0.0f;
            if (maximizer().takeLearnedGain (gainDb))
            {
                maxLearnActive = false;
                const int i = ps::paramIndex ("max.gain");
                commitGesture (true);
                const bool was = applying.exchange (true);
                setParam (i, gainDb);
                applying.store (was);
                record ("Learned Maximizer gain: " + psplug::formatParamValue (i, raw[(size_t) i]->load()));
                owner.sendChangeMessage();
            }
            else if (! maximizer().isLearning())
            {
                maxLearnActive = false;
                owner.sendChangeMessage();
            }
        }
    }

    void timerCallback() override { housekeeping (false); }

    void parameterValueChanged (int, float) override {}

    void parameterGestureChanged (int index, bool starting) override
    {
        if (applying.load())
            return;
        lastGestureEventMs.store (nowMs());
        if (starting)
        {
            activeGestures.fetch_add (1);
        }
        else
        {
            int g = activeGestures.load();
            while (g > 0 && ! activeGestures.compare_exchange_weak (g, g - 1)) {}
            lastGestureParam.store (index);
            gesturePending.store (true);
        }
    }

    //==============================================================================
    std::vector<float> learnedCrossovers (ps::ModuleType t) const
    {
        const auto prefix = psplug::moduleIdPrefix (t).toStdString();
        const int numBands = (int) std::lround (raw[(size_t) ps::paramIndex (prefix + ".bands")]->load());
        const int k = numBands <= 1 ? ps::kMaxCrossovers : std::min (numBands - 1, ps::kMaxCrossovers);

        std::vector<float> result;
        SmoothedSpectrum spectrum;
        const auto& ls = learnSpectra[(size_t) t];
        if (! spectrum.build (ls.db, ls.binHz))
        {
            for (int i = 0; i < k; ++i)
                result.push_back (ps::paramSpecs()[(size_t) ps::paramIndex (prefix + ".xover" + std::to_string (i + 1))]
                                      .defaultValue);
            return result;
        }

        static const double regions[3][3][2] {
            { { 100.0, 1000.0 }, { 0, 0 }, { 0, 0 } },
            { { 80.0, 500.0 }, { 1500.0, 8000.0 }, { 0, 0 } },
            { { 60.0, 300.0 }, { 500.0, 3500.0 }, { 4000.0, 12000.0 } },
        };

        double previous = 0.0;
        for (int i = 0; i < k; ++i)
        {
            double lo = regions[k - 1][i][0], hi = regions[k - 1][i][1];
            lo = std::max (lo, previous * std::pow (2.0, 0.75));
            hi = std::max (hi, lo * 1.5);
            const double f = juce::jlimit (20.0, 20000.0, spectrum.pickMinimum (lo, hi));
            result.push_back ((float) f);
            previous = f;
        }
        return result;
    }
};

//==============================================================================
PureSheenProcessor::PureSheenProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    impl = std::make_unique<Impl> (*this);
}

PureSheenProcessor::~PureSheenProcessor() = default;

void PureSheenProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    auto& d = *impl;
    d.maxBlock = juce::jmax (1, samplesPerBlock);
    d.chain.prepare (sampleRate, d.maxBlock);

    // One silent block applies the current parameters (modes decide the latency), then the chain starts clean, so the
    // host gets the right latency now (some hosts only read it around prepareToPlay).
    for (size_t i = 0; i < d.raw.size(); ++i)
        d.values[i] = d.raw[i]->load();
    juce::AudioBuffer<float> silence (2, d.maxBlock);
    silence.clear();
    d.chain.process (silence.getArrayOfWritePointers(), d.maxBlock, d.values.data(), {});
    d.chain.reset();

    d.prepared = true;
    setLatencySamples (d.chain.getLatencySamples());
}

void PureSheenProcessor::releaseResources() {}

bool PureSheenProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.inputBuses.size() == 1 && layouts.outputBuses.size() == 1
        && layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void PureSheenProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto& d = *impl;

    for (int ch = getTotalNumInputChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (! d.prepared || buffer.getNumChannels() < 2)
        return;

    const auto n = d.raw.size();
    for (size_t i = 0; i < n; ++i)
        d.values[i] = d.raw[i]->load (std::memory_order_relaxed);

    ps::HostInfo host;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())
                if (*bpm > 0.0)
                    host.bpm = *bpm;
            host.isPlaying = pos->getIsPlaying();
        }

    // Hosts may exceed the announced block size: process in chunks the chain was prepared for.
    float* const* channels = buffer.getArrayOfWritePointers();
    const int total = buffer.getNumSamples();
    for (int start = 0; start < total; start += d.maxBlock)
    {
        float* chunk[2] { channels[0] + start, channels[1] + start };
        d.chain.process (chunk, juce::jmin (d.maxBlock, total - start), d.values.data(), host);
    }
}

juce::AudioProcessorEditor* PureSheenProcessor::createEditor()
{
    return new PureSheenEditor (*this);
}

const juce::String PureSheenProcessor::getName() const
{
    return "PureSheen Dynamics";
}

juce::AudioProcessorParameter* PureSheenProcessor::getBypassParameter() const
{
    return impl->bypassIndex >= 0 ? impl->params[(size_t) impl->bypassIndex] : nullptr;
}

//==============================================================================
void PureSheenProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto& d = *impl;
    const juce::ScopedLock editSl (d.editLock);   // Match EQ: copy parameters and live spectra from the same transaction
    juce::ValueTree state (kStateTag);
    state.setProperty (kVersion, kStateVersion, nullptr);
    state.appendChild (d.apvts.copyState(), nullptr);
    state.appendChild (psplug::chainToTree (d.chain.getOrder()), nullptr);

    {
        const juce::ScopedLock sl (d.extrasLock);
        juce::ValueTree options (kOptionsTag);
        options.copyPropertiesAndChildrenFrom (d.uiState, nullptr);
        state.appendChild (options, nullptr);

        juce::ValueTree snaps (kSnapshotsTag);
        for (int i = 0; i < kNumSnapshots; ++i)
            if (d.snapshots[(size_t) i].has_value())
            {
                auto s = psplug::fullStateToTree (kSnapshotTag, *d.snapshots[(size_t) i]);
                s.setProperty (kSlot, i, nullptr);
                if (auto meq = d.matchEq->snapshotTree (i); meq.isValid())   // Match EQ spectra of the snapshot
                    s.appendChild (meq, nullptr);
                snaps.appendChild (s, nullptr);
            }
        snaps.setProperty (kActiveSnapshot, d.activeSnapshot, nullptr);
        state.appendChild (snaps, nullptr);

        if (d.editorW > 0 && d.editorH > 0)
        {
            state.setProperty (kEditorW, d.editorW, nullptr);
            state.setProperty (kEditorH, d.editorH, nullptr);
        }
        state.setProperty (kPresetName, d.presetName, nullptr);
        state.setProperty (kPresetModified, d.presetForcedModified
                                                || ! psplug::sameState (d.capture(), d.presetBaseline), nullptr);
        if (d.matchEq->isPresetModified())   // Match EQ: spectrum-only edits also modify the preset
            state.setProperty (kPresetModified, true, nullptr);
    }

    state.appendChild (d.matchEq->createStateTree(), nullptr);   // Match EQ: "MatchEq" child

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void PureSheenProcessor::setStateInformation (const void* data, int size)
{
    auto& d = *impl;
    const juce::ScopedLock editSl (d.editLock);
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, size));
    if (xml == nullptr && data != nullptr && size > 0)   // plain XML text (hand-edited / foreign wrappers)
        xml = juce::parseXML (juce::String::fromUTF8 (static_cast<const char*> (data), size));
    if (xml == nullptr)
        return;

    const auto state = juce::ValueTree::fromXml (*xml);
    const auto apvtsType = d.apvts.state.getType();

    juce::ValueTree paramsTree, chainTree, optionsTree, snapsTree;
    if (state.hasType (kStateTag))
    {
        paramsTree = state.getChildWithName (apvtsType);
        chainTree = state.getChildWithName (kChainTag);
        optionsTree = state.getChildWithName (kOptionsTag);
        if (! optionsTree.isValid())   // skeleton builds wrapped the UI tree in "UiState"
        {
            const auto legacy = state.getChildWithName (kUiLegacyTag);
            if (legacy.isValid() && legacy.getNumChildren() > 0)
                optionsTree = legacy.getChild (0);
        }
        snapsTree = state.getChildWithName (kSnapshotsTag);
    }
    else if (state.hasType (apvtsType))   // a bare APVTS state
    {
        paramsTree = state;
    }
    else
    {
        return;
    }

    d.commitGesture (true);

    std::vector<float> values = psplug::defaultValues();
    if (paramsTree.isValid())
        psplug::valuesFromTree (paramsTree, values, -1, true, true);

    {
        const bool was = d.applying.exchange (true);
        for (size_t i = 0; i < values.size(); ++i)
            if (! psplug::sameValue ((int) i, d.raw[i]->load(), values[i]))
                d.setParam ((int) i, values[i], false);   // a state load is not a user gesture
        d.applying.store (was);
    }
    d.setOrder (chainTree.isValid() ? psplug::chainFromTree (chainTree) : psplug::defaultChain());
    // Match EQ: the spectra (a state without the child, e.g. 0.1.x, clears them)
    d.matchEq->restoreStateTree (state.hasType (kStateTag) ? state.getChildWithName ("MatchEq") : juce::ValueTree());

    {
        const juce::ScopedLock sl (d.extrasLock);
        d.matchEq->clearAllSnapshots();   // Match EQ: publish snapshot parameters and spectra under the same lock
        d.uiState = juce::ValueTree (kOptionsTag);
        if (optionsTree.isValid())
            d.uiState.copyPropertiesAndChildrenFrom (optionsTree, nullptr);

        for (auto& s : d.snapshots)
            s.reset();
        d.activeSnapshot = -1;
        if (snapsTree.isValid())
        {
            for (const auto& s : snapsTree)
            {
                const int slot = (int) s.getProperty (kSlot, -1);
                psplug::FullState fs;
                if (s.hasType (kSnapshotTag) && slot >= 0 && slot < kNumSnapshots && psplug::fullStateFromTree (s, fs))
                {
                    d.snapshots[(size_t) slot] = std::move (fs);
                    d.matchEq->restoreSnapshotTree (slot, s.getChildWithName ("MatchEq"));   // Match EQ
                }
            }
            const int active = (int) snapsTree.getProperty (kActiveSnapshot, -1);
            if (active >= 0 && active < kNumSnapshots && d.snapshots[(size_t) active].has_value())
                d.activeSnapshot = active;
        }

        d.editorW = juce::jmax (0, (int) state.getProperty (kEditorW, 0));
        d.editorH = juce::jmax (0, (int) state.getProperty (kEditorH, 0));
        d.presetName = state.getProperty (kPresetName, "Default").toString();
        if (d.presetName.trim().isEmpty())
            d.presetName = "Default";
        d.presetForcedModified = (bool) state.getProperty (kPresetModified, false);
    }

    if (d.uiState.hasProperty (kHistoryDepth))
        d.history.setDepth ((int) d.uiState.getProperty (kHistoryDepth));

    d.presetBaseline = d.capture();
    d.history.reset (d.capture());
    d.matchEq->historyReset (d.history);   // Match EQ
    sendChangeMessage();
}

//==============================================================================
juce::AudioProcessorValueTreeState& PureSheenProcessor::getValueTreeState() noexcept { return impl->apvts; }

juce::RangedAudioParameter* PureSheenProcessor::getParameter (const juce::String& id) const
{
    return impl->apvts.getParameter (id);
}

float PureSheenProcessor::getParameterValue (const juce::String& id) const
{
    if (auto* v = impl->apvts.getRawParameterValue (id))
        return v->load();
    return 0.0f;
}

void PureSheenProcessor::setParameterValue (const juce::String& id, float plainValue)
{
    auto& d = *impl;
    const int i = ps::paramIndex (id.toStdString());
    if (i < 0)
        return;

    const juce::ScopedLock editSl (d.editLock);
    d.matchEqHousekeeping();   // keep an earlier spectrum edit before this parameter step
    d.commitGesture (true);
    const bool was = d.applying.exchange (true);
    d.setParam (i, plainValue);
    d.applying.store (was);
    d.record (psplug::describeParam (i, d.raw[(size_t) i]->load()), i);
}

void PureSheenProcessor::setParameterValues (const std::vector<std::pair<juce::String, float>>& idsAndPlainValues,
                                             const juce::String& undoDescription)
{
    auto& d = *impl;
    const juce::ScopedLock editSl (d.editLock);
    d.matchEqHousekeeping();
    d.commitGesture (true);

    int first = -1;
    const bool was = d.applying.exchange (true);
    for (const auto& [id, value] : idsAndPlainValues)
    {
        const int i = ps::paramIndex (id.toStdString());
        if (i < 0)
            continue;
        if (first < 0)
            first = i;
        d.setParam (i, value);
    }
    d.applying.store (was);

    if (first < 0)
        return;

    auto description = undoDescription;
    if (description.isEmpty())
        description = idsAndPlainValues.size() == 1 ? psplug::describeParam (first, d.raw[(size_t) first]->load())
                                                    : juce::String ("Changed ") + juce::String ((int) idsAndPlainValues.size())
                                                          + " parameters";
    d.record (description);
}

//==============================================================================
std::vector<ps::ModuleType> PureSheenProcessor::getChainOrder() const { return impl->chain.getOrder(); }

void PureSheenProcessor::setChainOrder (const std::vector<ps::ModuleType>& requested)
{
    auto& d = *impl;
    const juce::ScopedLock editSl (d.editLock);
    d.matchEqHousekeeping();
    d.commitGesture (true);

    const auto before = d.chain.getOrder();
    d.setOrder (requested);
    const auto after = d.chain.getOrder();
    if (after == before)
        return;

    auto contains = [] (const std::vector<ps::ModuleType>& v, ps::ModuleType t)
    { return std::find (v.begin(), v.end(), t) != v.end(); };

    std::vector<ps::ModuleType> added, removed;
    for (auto t : after)
        if (! contains (before, t))
            added.push_back (t);
    for (auto t : before)
        if (! contains (after, t))
            removed.push_back (t);

    juce::String description;
    if (added.size() == 1 && removed.empty())
        description = "Added " + psplug::moduleName (added[0]);
    else if (removed.size() == 1 && added.empty())
        description = "Removed " + psplug::moduleName (removed[0]);
    else if (added.empty() && removed.empty())
        description = "Changed module order";
    else
        description = "Changed modules";
    d.record (description);
}

void PureSheenProcessor::addModule (ps::ModuleType type, int insertIndex)
{
    auto& d = *impl;
    if (! validModule ((int) type) || isModuleInChain (type))
        return;

    const juce::ScopedLock editSl (d.editLock);
    d.matchEqHousekeeping();
    d.commitGesture (true);
    auto order = getChainOrder();
    if (insertIndex < 0 || insertIndex > (int) order.size())
    {
        const auto maxIt = std::find (order.begin(), order.end(), ps::ModuleType::Maximizer);
        insertIndex = (int) std::distance (order.begin(), maxIt);
    }
    order.insert (order.begin() + insertIndex, type);

    // A fresh module: every parameter of it (power included) at its default.
    const auto defaults = psplug::defaultValues();
    d.applyValues (defaults, [type] (int i) { return ps::paramSpecs()[(size_t) i].module == (int) type; });
    d.setOrder (order);
    d.record ("Added " + psplug::moduleName (type));
}

void PureSheenProcessor::removeModule (ps::ModuleType type)
{
    if (! isModuleInChain (type))
        return;
    auto order = getChainOrder();
    order.erase (std::remove (order.begin(), order.end(), type), order.end());
    setChainOrder (order);
}

void PureSheenProcessor::resetModule (ps::ModuleType type)
{
    auto& d = *impl;
    if (! validModule ((int) type))
        return;
    const juce::ScopedLock editSl (d.editLock);
    d.matchEqHousekeeping();
    d.commitGesture (true);
    const auto defaults = psplug::defaultValues();
    const int scope = (int) type;
    d.applyValues (defaults, [scope] (int i) { return psplug::isInPresetScope (i, scope); });
    d.record ("Reset " + psplug::moduleName (type));
}

bool PureSheenProcessor::isModuleInChain (ps::ModuleType type) const { return impl->chain.isInChain (type); }

void PureSheenProcessor::setSoloModule (int m)
{
    impl->chain.setSolo (validModule (m) ? m : -1);
    sendChangeMessage();
}

int PureSheenProcessor::getSoloModule() const { return impl->chain.getSolo(); }

void PureSheenProcessor::setDelta (ps::ModuleType t, bool on)
{
    impl->chain.setDelta (t, on);
    sendChangeMessage();
}

bool PureSheenProcessor::getDelta (ps::ModuleType t) const { return impl->chain.getDelta (t); }

void PureSheenProcessor::setChannelSolo (ps::ModuleType t, int set)
{
    if (! validModule ((int) t))
        return;
    set = set >= 0 && set < ps::kNumChannelSets ? set : -1;
    impl->channelSolo[(size_t) t] = set;
    impl->chain.setChannelSolo (t, set);
    sendChangeMessage();
}

int PureSheenProcessor::getChannelSolo (ps::ModuleType t) const
{
    return validModule ((int) t) ? impl->channelSolo[(size_t) t] : -1;
}

void PureSheenProcessor::setMonitor (ps::ModuleType t, const ps::ModuleMonitor& m)
{
    if (! validModule ((int) t))
        return;
    impl->monitors[(size_t) t] = m;
    impl->chain.setMonitor (t, m);
}

ps::ModuleMonitor PureSheenProcessor::getMonitor (ps::ModuleType t) const
{
    return validModule ((int) t) ? impl->monitors[(size_t) t] : ps::ModuleMonitor {};
}

void PureSheenProcessor::setTapModule (int m) { impl->chain.setTapModule (validModule (m) ? m : -1); }

ps::Chain& PureSheenProcessor::getChain() noexcept { return impl->chain; }
int PureSheenProcessor::getCurrentLatencySamples() const noexcept { return impl->chain.getLatencySamples(); }

//==============================================================================
void PureSheenProcessor::learnCrossovers (ps::ModuleType t)
{
    if (! isMultiband (t))
        return;

    const auto xovers = impl->learnedCrossovers (t);
    std::vector<std::pair<juce::String, float>> changes;
    const auto prefix = psplug::moduleIdPrefix (t);
    for (size_t i = 0; i < xovers.size(); ++i)
        changes.emplace_back (prefix + ".xover" + juce::String ((int) i + 1), xovers[i]);
    setParameterValues (changes, "Learned crossovers (" + psplug::moduleName (t) + ")");
}

void PureSheenProcessor::startMaximizerLearn()
{
    impl->maximizer().startLearn();
    impl->maxLearnActive = true;
    sendChangeMessage();
}

void PureSheenProcessor::cancelMaximizerLearn()
{
    impl->maximizer().cancelLearn();
    impl->maxLearnActive = false;
    sendChangeMessage();
}

bool PureSheenProcessor::isMaximizerLearning() const
{
    return impl->maxLearnActive && impl->maximizer().isLearning();
}

float PureSheenProcessor::getMaximizerLearnProgress() const
{
    return impl->maximizer().getLearnProgress();
}

void PureSheenProcessor::feedLearnSpectrum (ps::ModuleType t, const std::vector<float>& magnitudeDb, double binHz)
{
    if (! validModule ((int) t))
        return;
    auto& ls = impl->learnSpectra[(size_t) t];
    ls.db = magnitudeDb;
    ls.binHz = binHz;
}

//==============================================================================
std::vector<PureSheenProcessor::HistoryEntry> PureSheenProcessor::getHistory() const
{
    std::vector<HistoryEntry> list;
    const auto& h = impl->history;
    list.reserve ((size_t) h.size());
    for (int i = 0; i < h.size(); ++i)
        list.push_back ({ h[i].id, h[i].description });
    return list;
}

int PureSheenProcessor::getHistoryPosition() const { return impl->history.getPosition(); }

bool PureSheenProcessor::canUndo() const
{
    return impl->history.canUndo() || impl->gesturePending.load();
}

bool PureSheenProcessor::canRedo() const
{
    return impl->history.canRedo() && ! impl->gesturePending.load();
}

void PureSheenProcessor::undo()
{
    const juce::ScopedLock editSl (impl->editLock);   // Match EQ: housekeeping and the restored spectrum set stay together
    impl->matchEqHousekeeping();   // Match EQ: record pending spectra before calculating the relative target
    impl->commitGesture (true);
    impl->goTo (impl->history.getPosition() - 1);
}

void PureSheenProcessor::redo()
{
    const juce::ScopedLock editSl (impl->editLock);   // Match EQ: housekeeping and the restored spectrum set stay together
    impl->matchEqHousekeeping();   // Match EQ: a pending spectrum edit discards the old redo branch
    impl->commitGesture (true);
    impl->goTo (impl->history.getPosition() + 1);
}

void PureSheenProcessor::jumpToHistory (int index) { impl->goTo (index); }

void PureSheenProcessor::clearHistory()
{
    impl->commitGesture (true);
    impl->history.reset (impl->capture());
    impl->matchEq->historyReset (impl->history);   // Match EQ
    sendChangeMessage();
}

void PureSheenProcessor::setHistoryDepth (int maxEntries)
{
    impl->history.setDepth (maxEntries);
    sendChangeMessage();
}

void PureSheenProcessor::storeSnapshot (int slot)
{
    const juce::ScopedLock editSl (impl->editLock);   // Match EQ: a host state restore cannot split the stored parameter/spectrum pair
    if (slot < 0 || slot >= kNumSnapshots)
        return;
    impl->matchEqHousekeeping();
    impl->commitGesture (true);
    {
        const juce::ScopedLock sl (impl->extrasLock);
        impl->snapshots[(size_t) slot] = impl->capture();
        impl->activeSnapshot = slot;
        impl->matchEq->storeSnapshot (slot);   // Match EQ: keep spectra atomic with the snapshot parameters
    }
    sendChangeMessage();
}

void PureSheenProcessor::storeSnapshotFromHistory (int slot, int historyIndex)
{
    const juce::ScopedLock editSl (impl->editLock);   // Match EQ: hold the history entry and its spectra stable while storing
    if (slot < 0 || slot >= kNumSnapshots || historyIndex < 0 || historyIndex >= impl->history.size())
        return;
    // Pending edits may append and trim the history. Preserve the entry the user selected before its index moves.
    const int selectedId = impl->history[historyIndex].id;
    {
        const juce::ScopedLock sl (impl->extrasLock);
        impl->snapshots[(size_t) slot] = impl->history[historyIndex].state;
        impl->matchEq->storeSnapshotFromHistory (slot, impl->history, historyIndex);   // Match EQ
    }
    impl->matchEqHousekeeping();
    impl->commitGesture (true);
    {
        const juce::ScopedLock sl (impl->extrasLock);
        impl->activeSnapshot = selectedId == impl->history.current().id ? slot : -1;
    }
    sendChangeMessage();
}

void PureSheenProcessor::recallSnapshot (int slot)
{
    const juce::ScopedLock editSl (impl->editLock);   // Match EQ: host saves see a complete snapshot recall
    if (! hasSnapshot (slot))
        return;
    auto& d = *impl;
    d.matchEqHousekeeping();   // Match EQ: retain the spectra being replaced so recall can be undone
    d.commitGesture (true);
    psplug::FullState s;
    {
        const juce::ScopedLock sl (d.extrasLock);
        s = *d.snapshots[(size_t) slot];
    }
    d.applyState (s, true);
    const bool spectraChanged = d.matchEq->recallSnapshot (slot);   // Match EQ
    if (! d.record ("Recalled snapshot " + slotName (slot)) && spectraChanged)
        d.recordMatchEq ("Recalled snapshot " + slotName (slot));
    {
        const juce::ScopedLock sl (d.extrasLock);
        d.activeSnapshot = slot;
    }
    sendChangeMessage();
}

bool PureSheenProcessor::hasSnapshot (int slot) const
{
    if (slot < 0 || slot >= kNumSnapshots)
        return false;
    const juce::ScopedLock sl (impl->extrasLock);
    return impl->snapshots[(size_t) slot].has_value();
}

void PureSheenProcessor::clearSnapshot (int slot)
{
    const juce::ScopedLock editSl (impl->editLock);   // Match EQ: serialize snapshot removal with compound recalls/restores
    if (slot < 0 || slot >= kNumSnapshots)
        return;
    {
        const juce::ScopedLock sl (impl->extrasLock);
        impl->snapshots[(size_t) slot].reset();
        if (impl->activeSnapshot == slot)
            impl->activeSnapshot = -1;
        impl->matchEq->clearSnapshot (slot);   // Match EQ: clear parameters and spectra together
    }
    sendChangeMessage();
}

int PureSheenProcessor::getActiveSnapshot() const
{
    if (impl->gesturePending.load())
        return -1;
    const juce::ScopedLock sl (impl->extrasLock);
    return impl->activeSnapshot;
}

//==============================================================================
std::vector<PureSheenProcessor::PresetInfo> PureSheenProcessor::getPresets (int scope) const
{
    std::vector<PresetInfo> list;
    if (scope >= ps::kNumModuleTypes)
        return list;
    scope = scope < 0 ? -1 : scope;

    for (const auto* f : psplug::factoryPresetsFor (scope))
        list.push_back ({ f->name, f->category, true, {} });

    for (const auto& u : psplug::scanUserPresets (impl->presetRoot, scope))
        list.push_back ({ u.name, u.category, false, u.file });

    return list;
}

bool PureSheenProcessor::loadPreset (int scope, const PresetInfo& info)
{
    if (scope >= ps::kNumModuleTypes)
        return false;
    scope = scope < 0 ? -1 : scope;

    psplug::PresetData data;
    if (info.factory)
    {
        const psplug::FactoryPreset* found = nullptr;
        for (const auto* f : psplug::factoryPresetsFor (scope))
            if (f->name == info.name && (info.category.isEmpty() || f->category == info.category))
            {
                found = f;
                break;
            }
        if (found == nullptr)
            return false;
        data = psplug::toPresetData (*found, scope);
    }
    else
    {
        if (! psplug::readPresetFile (info.file, data) || data.scope != scope)
        {
            // Module presets are interchangeable between EQ 1 and EQ 2 (same parameter layout).
            const bool eqSwap = (data.scope == (int) ps::ModuleType::Equalizer1 && scope == (int) ps::ModuleType::Equalizer2)
                                || (data.scope == (int) ps::ModuleType::Equalizer2 && scope == (int) ps::ModuleType::Equalizer1);
            if (! eqSwap || ! info.file.existsAsFile())
                return false;

            const auto from = psplug::moduleIdPrefix ((ps::ModuleType) data.scope).toStdString();
            const auto to = psplug::moduleIdPrefix ((ps::ModuleType) scope).toStdString();
            std::vector<float> moved = psplug::defaultValues();
            const auto& specs = ps::paramSpecs();
            for (size_t i = 0; i < specs.size(); ++i)
                if (specs[i].module == data.scope)
                {
                    const int j = ps::paramIndex (to + specs[i].id.substr (from.size()));
                    if (j >= 0)
                        moved[(size_t) j] = data.values[i];
                }
            data.values = std::move (moved);
            data.scope = scope;
        }
        data.name = info.name.isNotEmpty() ? info.name : data.name;
    }

    impl->matchEq->beginPresetLoad (scope, info.factory, info.name, info.file);   // Match EQ: its reference spectra
    return impl->applyPreset (data);
}

bool PureSheenProcessor::savePreset (int scope, const juce::String& name, const juce::String& category)
{
    const juce::ScopedLock editSl (impl->editLock);   // Match EQ: save matching parameters and references during host state restores
    if (scope >= ps::kNumModuleTypes)
        return false;
    scope = scope < 0 ? -1 : scope;

    const auto cleanName = juce::File::createLegalFileName (name.trim()).trim();
    if (cleanName.isEmpty())
        return false;

    auto& d = *impl;
    d.commitGesture (true);

    auto folder = psplug::presetFolder (d.presetRoot, scope);
    const auto cleanCategory = juce::File::createLegalFileName (category.trim()).trim();
    if (cleanCategory.isNotEmpty() && cleanCategory != psplug::kUserCategory)
        folder = folder.getChildFile (cleanCategory);

    const auto current = d.capture();
    psplug::PresetData data;
    data.scope = scope;
    data.name = cleanName;
    data.category = cleanCategory;
    data.hasChain = scope < 0;
    data.order = current.order;
    data.values = current.values;

    const auto presetFile = folder.getChildFile (cleanName + psplug::kPresetExtension);
    if (! psplug::writePresetFile (presetFile, data))
        return false;
    if (! d.matchEq->appendToPresetFile (presetFile, scope))   // Match EQ: report failure to save the references
        return false;

    if (scope < 0)
    {
        const juce::ScopedLock sl (d.extrasLock);
        d.presetName = cleanName;
        d.presetBaseline = current;
        d.presetForcedModified = false;
        d.matchEq->setPresetBaseline();   // Match EQ: baseline the references saved in this global preset
    }
    sendChangeMessage();
    return true;
}

bool PureSheenProcessor::deletePreset (const PresetInfo& info)
{
    if (info.factory || ! info.file.existsAsFile() || ! info.file.hasFileExtension (psplug::kPresetExtension))
        return false;
    if (! info.file.isAChildOf (impl->presetRoot))
        return false;
    const bool ok = info.file.deleteFile();
    sendChangeMessage();
    return ok;
}

juce::String PureSheenProcessor::getCurrentPresetName() const
{
    const juce::ScopedLock sl (impl->extrasLock);
    return impl->presetName;
}

bool PureSheenProcessor::isPresetModified() const
{
    const juce::ScopedLock sl (impl->extrasLock);
    if (impl->matchEq->isPresetModified())   // Match EQ: references are part of global presets
        return true;
    return impl->presetForcedModified || ! psplug::sameState (impl->capture(), impl->presetBaseline);
}

void PureSheenProcessor::loadNextPreset (int direction)
{
    const auto list = getPresets (-1);
    if (list.empty() || direction == 0)
        return;

    const auto current = getCurrentPresetName();
    int index = -1;
    for (int i = 0; i < (int) list.size(); ++i)
        if (list[(size_t) i].name == current)
        {
            index = i;
            break;
        }

    const int n = (int) list.size();
    const int next = index < 0 ? (direction > 0 ? 0 : n - 1) : ((index + (direction > 0 ? 1 : -1)) % n + n) % n;
    loadPreset (-1, list[(size_t) next]);
}

void PureSheenProcessor::setPresetRoot (const juce::File& root)
{
    impl->presetRoot = root == juce::File() ? psplug::defaultPresetRoot() : root;
    sendChangeMessage();
}

juce::File PureSheenProcessor::getPresetRoot() const { return impl->presetRoot; }

//==============================================================================
juce::ValueTree PureSheenProcessor::getUiState() const
{
    const juce::ScopedLock sl (impl->extrasLock);
    return impl->uiState;
}

void PureSheenProcessor::setUiState (const juce::ValueTree& tree)
{
    {
        const juce::ScopedLock sl (impl->extrasLock);
        impl->uiState = juce::ValueTree (kOptionsTag);
        if (tree.isValid())
            impl->uiState.copyPropertiesAndChildrenFrom (tree, nullptr);
    }
    if (tree.hasProperty (kHistoryDepth))
        impl->history.setDepth ((int) tree.getProperty (kHistoryDepth));
}

void PureSheenProcessor::setEditorSize (int width, int height)
{
    const juce::ScopedLock sl (impl->extrasLock);
    impl->editorW = juce::jmax (0, width);
    impl->editorH = juce::jmax (0, height);
}

juce::Point<int> PureSheenProcessor::getEditorSize() const
{
    const juce::ScopedLock sl (impl->extrasLock);
    return { impl->editorW, impl->editorH };
}

void PureSheenProcessor::flushPendingChanges (bool force) { impl->housekeeping (force); }

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PureSheenProcessor();
}

//==============================================================================
// (phase 2) Match EQ reference from a file: the loader thread lives in MatchEqState.cpp; housekeeping (30 Hz timer /
// flushPendingChanges) applies the result and records the undo step.
bool PureSheenProcessor::startMatchReferenceFile (const juce::File& file, int part)
{
    return startMatchReferenceFile (file, part, false);
}
bool PureSheenProcessor::startMatchReferenceFile (const juce::File& file, int part, bool linkedParts)
{
    return impl->matchEq->startFile (file, part, linkedParts);
}
void PureSheenProcessor::cancelMatchReferenceFile() { impl->matchEq->cancelFile(); }
float PureSheenProcessor::getMatchReferenceFileProgress() const { return impl->matchEq->getFileProgress(); }
juce::String PureSheenProcessor::getMatchReferenceFileError() const { return impl->matchEq->getFileError(); }
