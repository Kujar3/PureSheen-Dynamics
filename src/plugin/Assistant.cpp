#include "plugin/Assistant.h"

#include "assist/OfflineChain.h"
#include "dsp/Params.h"
#include "plugin/PluginState.h"

#include <algorithm>
#include <cmath>

namespace psplug
{
namespace
{
constexpr float kSilentMeanSquare = 1.0e-7f;   // -70 dBFS
constexpr double kMaxReferenceSeconds = 600.0;

const juce::Identifier kTag ("Assistant"), kCard ("Card"), kP ("P"), kRef ("Reference");

using ps::assist::Macro;

bool macroOwns (Macro m, const std::string& id)
{
    auto starts = [&] (const char* p) { return id.rfind (p, 0) == 0; };
    switch (m)
    {
        case Macro::ToneMatch:    return id == "eq1.amount";
        case Macro::Tilt:         return starts ("eq1.a.b6.") || starts ("eq1.a.b7.");
        case Macro::Dynamics:     return id == "pnch.amount";
        case Macro::Width:        return id == "img.amount";
        case Macro::WidthLow:     return id == "img.b1.width";
        case Macro::WidthLowMid:  return id == "img.b2.width";
        case Macro::WidthHighMid: return id == "img.b3.width";
        case Macro::WidthHigh:    return id == "img.b4.width";
        case Macro::Compress:     return starts ("dyn.");
        case Macro::Definition:   return id == "defn.a.amount";
        case Macro::ToneBalance:  return id == "tbal.a.amount";
        case Macro::Loudness:     return id == "max.learnTarget" || id == "max.gain";
        case Macro::Count:
        default: break;
    }
    return false;
}

float valueOf (const std::vector<float>& v, const char* id)
{
    const int i = ps::paramIndex (id);
    return i >= 0 && i < (int) v.size() ? v[(size_t) i] : 0.0f;
}

juce::String orderToString (const std::vector<ps::ModuleType>& order)
{
    juce::StringArray s;
    for (auto t : order)
        s.add (ps::moduleInfo (t).idPrefix);
    return s.joinIntoString (",");
}

std::vector<ps::ModuleType> orderFromString (const juce::String& text)
{
    std::vector<ps::ModuleType> order;
    for (const auto& token : juce::StringArray::fromTokens (text, ",", {}))
        for (int t = 0; t < ps::kNumModuleTypes; ++t)
            if (token.trim() == ps::moduleInfo ((ps::ModuleType) t).idPrefix
                && std::find (order.begin(), order.end(), (ps::ModuleType) t) == order.end())
                order.push_back ((ps::ModuleType) t);
    return order;
}

template <typename Container>
juce::String floatsToString (const Container& c)
{
    juce::StringArray s;
    for (float v : c)
        s.add (juce::String (v, 4));
    return s.joinIntoString (" ");
}

template <size_t N>
void floatsFromString (const juce::String& text, std::array<float, N>& out, float fallback)
{
    const auto tokens = juce::StringArray::fromTokens (text, " ", {});
    for (size_t i = 0; i < N; ++i)
    {
        const float v = (int) i < tokens.size() ? tokens[(int) i].getFloatValue() : fallback;
        out[i] = std::isfinite (v) ? v : fallback;
    }
}

bool isMessageThread()
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    return mm != nullptr && mm->isThisTheMessageThread();
}

float finiteOr (float v, float fallback) noexcept { return std::isfinite (v) ? v : fallback; }

float prop (const juce::ValueTree& t, const juce::Identifier& name, float fallback)
{
    return finiteOr ((float) t.getProperty (name, fallback), fallback);
}

bool readFile (const juce::File& file, double maxSeconds, std::vector<float>& left, std::vector<float>& right,
               double& rate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0 || reader->lengthInSamples <= 0)
        return false;

    rate = reader->sampleRate;
    const auto length = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (maxSeconds * rate));
    juce::AudioBuffer<float> buffer (2, length);
    reader->read (&buffer, 0, length, 0, true, reader->numChannels > 1);
    if (reader->numChannels == 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, length);
    left.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);
    right.assign (buffer.getReadPointer (1), buffer.getReadPointer (1) + length);
    return true;
}
} // namespace

//==============================================================================
struct Assistant::Results
{
    int generation = 0;             // the job (or reference load) it belongs to
    int stage = 0;                  // 1 = default ready (apply it), 2 = all gains, 3 = tables
    bool applyDefault = false;
    bool failed = false;
    juce::String message;
    std::vector<ps::assist::Suggestion> suggestions;
    ps::ToneTarget detected = ps::ToneTarget::Balanced;
    float inputLufs = ps::kNoValue, rmsDb = -18.0f;
    ps::assist::ReferenceTarget reference;
    ps::Curve trackCurve {};
    std::vector<float> keepLeft, keepRight;   // stage 1: the analysed audio (kept for rebuilds)
    double keepRate = 0.0;
};

Assistant::Assistant (AssistantHost& h) : host (h)
{
    cachedState = buildStateTree();   // a host may save before the first change
}

Assistant::~Assistant()
{
    cancelFlag.store (true);
    joinWorker();
    if (referenceWorker.joinable())
        referenceWorker.join();
    releaseCapture();
}

void Assistant::prepare (double rate)
{
    sampleRate.store (rate > 0.0 ? rate : 48000.0);
    if (getPhase() != Phase::Listening)
        return;
    if (isMessageThread())
        cancel();
    else
        stopRequested.store (true);   // the next tick stops the listen (the capture length was for the old rate)
}

void Assistant::pushAudio (const float* left, const float* right, int numSamples) noexcept
{
    if (! capArmed.load (std::memory_order_relaxed) || numSamples <= 0 || left == nullptr)
        return;

    // busy first, then armed and the pointers: see "Capture handover" in the header
    capBusy.store (true);
    float* l = capArmed.load() ? capL.load() : nullptr;
    float* r = l != nullptr ? capR.load() : nullptr;
    if (l != nullptr && r != nullptr)
    {
        if (right == nullptr)
            right = left;
        double sq = 0.0;
        for (int i = 0; i < numSamples; ++i)
            sq += 0.5 * ((double) left[i] * left[i] + (double) right[i] * right[i]);

        if (sq / numSamples >= kSilentMeanSquare)
        {
            const long long cap = capCapacity.load();
            const long long at = capWritten.load();
            const long long n = std::min<long long> (numSamples, cap - at);
            if (n > 0)
            {
                std::copy (left, left + n, l + at);
                std::copy (right, right + n, r + at);
                capWritten.store (at + n);
            }
        }
    }
    capBusy.store (false);
}

void Assistant::releaseCapture()
{
    capArmed.store (false);
    capL.store (nullptr);
    capR.store (nullptr);
    while (capBusy.load())
        std::this_thread::yield();
    capCapacity.store (0);
    capWritten.store (0);
    std::vector<float>().swap (captureL);
    std::vector<float>().swap (captureR);
}

void Assistant::joinWorker()
{
    if (worker.joinable())
        worker.join();
}

void Assistant::startWorker (std::function<void()> job)
{
    cancelFlag.store (true);
    joinWorker();
    cancelFlag.store (false);
    workerActive.store (true);
    worker = std::thread ([this, job = std::move (job)] {
        job();
        workerActive.store (false);
    });
}

// Drops results of earlier jobs: called before every new listen / analysis / rebuild / cancel / restore.
void Assistant::invalidateResults()
{
    generation.fetch_add (1);
    const std::lock_guard<std::mutex> lock (resultsMutex);
    pending.reset();
}

void Assistant::publish (std::unique_ptr<Results> r)
{
    const std::lock_guard<std::mutex> lock (resultsMutex);
    if (r->generation != generation.load())
        return;
    if (pending != nullptr)
    {
        if (pending->applyDefault)
            r->applyDefault = true;   // not taken yet: keep the request
        if (pending->keepRate > 0.0 && r->keepRate <= 0.0)
        {
            r->keepLeft = std::move (pending->keepLeft);   // ... and the audio to keep for rebuilds
            r->keepRight = std::move (pending->keepRight);
            r->keepRate = pending->keepRate;
        }
    }
    pending = std::move (r);
    pendingFlag.store (true);
}

//==============================================================================
Assistant::Settings Assistant::getSettings() const { return settings; }

void Assistant::setSettings (const Settings& s, bool rebuildNow)
{
    settings = s;
    settings.analysisSeconds = std::clamp (finiteOr (s.analysisSeconds, 8.0f), kMinSeconds, kMaxSeconds);
    settings.engine.loudnessLufs = std::clamp (finiteOr (s.engine.loudnessLufs, -10.0f), ps::assist::kMinTargetLufs,
                                               ps::assist::kMaxTargetLufs);
    settings.engine.ceilingDb = std::clamp (finiteOr (s.engine.ceilingDb, -0.1f), -3.0f, 0.0f);
    if (rebuildNow)
        rebuild();
    changed();
}

void Assistant::rebuild()
{
    if (! hasAnalysis() || getPhase() == Phase::Listening)
        return;
    invalidateResults();
    phase.store ((int) Phase::Analysing);
    progress.store (0.0f);
    status = "Rebuilding the suggestions...";
    auto l = lastLeft, r = lastRight;
    const double rate = lastRate;
    const auto set = settings;
    const auto ref = reference;
    const int gen = generation.load();
    startWorker ([this, gen, l, r, rate, set, ref]() mutable {
        runAnalysis (gen, std::move (l), std::move (r), rate, set, ref, true);
    });
}

void Assistant::startListening()
{
    cancelFlag.store (true);
    joinWorker();
    releaseCapture();
    invalidateResults();
    stopRequested.store (false);

    const auto capacity = (long long) std::ceil (settings.analysisSeconds * sampleRate.load());
    captureL.assign ((size_t) capacity, 0.0f);
    captureR.assign ((size_t) capacity, 0.0f);
    capWritten.store (0);
    capCapacity.store (capacity);
    capL.store (captureL.data());
    capR.store (captureR.data());
    progress.store (0.0f);
    phase.store ((int) Phase::Listening);
    status = "Play the loudest part of your track.";
    capArmed.store (true);
    changed();
}

void Assistant::cancel()
{
    cancelFlag.store (true);
    joinWorker();
    releaseCapture();
    invalidateResults();
    stopRequested.store (false);
    phase.store ((int) (suggestions.empty() ? Phase::Idle : Phase::Ready));
    progress.store (0.0f);
    status = suggestions.empty() ? juce::String() : juce::String ("Stopped.");
    changed();
}

bool Assistant::analyseFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;
    cancelFlag.store (true);
    joinWorker();
    releaseCapture();
    invalidateResults();
    phase.store ((int) Phase::Analysing);
    progress.store (0.0f);
    status = "Reading " + file.getFileName() + "...";
    const auto set = settings;
    const auto ref = reference;
    const int gen = generation.load();
    startWorker ([this, gen, file, set, ref] {
        std::vector<float> l, r;
        double rate = 0.0;
        if (! readFile (file, kMaxReferenceSeconds, l, r, rate))
        {
            auto res = std::make_unique<Results>();
            res->generation = gen;
            res->failed = true;
            res->message = "That file could not be read.";
            publish (std::move (res));
            return;
        }
        auto ex = ps::assist::loudestExcerpt (l.data(), r.data(), (long long) l.size(), rate, set.analysisSeconds);
        runAnalysis (gen, std::move (ex.left), std::move (ex.right), rate, set, ref, true);
    });
    changed();
    return true;
}

Assistant::Phase Assistant::getPhase() const noexcept { return (Phase) phase.load(); }

float Assistant::getProgress() const noexcept
{
    if (getPhase() == Phase::Listening)
    {
        const auto cap = capCapacity.load();
        return cap > 0 ? juce::jlimit (0.0f, 1.0f, (float) capWritten.load() / (float) cap) : 0.0f;
    }
    return workerActive.load() ? progress.load() : 0.0f;
}

juce::String Assistant::getStatusText() const
{
    if (getPhase() == Phase::Listening)
    {
        const auto heard = (double) capWritten.load() / sampleRate.load();
        if (heard < 0.05)
            return "Play the loudest part of your track.";
        return "Listening... " + juce::String (heard, 0) + " of " + juce::String (settings.analysisSeconds, 0) + " s";
    }
    return status;
}

bool Assistant::hasAnalysis() const { return ! lastLeft.empty() && lastRate > 0.0; }

//==============================================================================
void Assistant::runAnalysis (int gen, std::vector<float> left, std::vector<float> right, double rate, Settings set,
                             ps::assist::ReferenceTarget ref, bool applyDefault)
{
    ps::assist::Context ctx;
    ctx.settings = set.engine;
    ctx.reference = ref;
    ctx.analysis = ps::assist::analyse (left.data(), right.data(), (long long) left.size(), rate);
    progress.store (0.15f);

    if (! ctx.analysis.valid)
    {
        auto r = std::make_unique<Results>();
        r->generation = gen;
        r->failed = true;
        r->message = "Not enough audio was heard. Play the track and try again.";
        publish (std::move (r));
        return;
    }

    auto cards = ps::assist::buildSuggestions (ctx);
    ps::assist::Excerpt ex;
    ex.left = std::move (left);
    ex.right = std::move (right);
    ex.sampleRate = rate;

    auto results = [&] (int stage) {
        auto r = std::make_unique<Results>();
        r->generation = gen;
        r->stage = stage;
        r->applyDefault = applyDefault && stage == 1;
        r->suggestions = cards;
        r->detected = ps::assist::detectTarget (ctx.analysis);
        r->inputLufs = ctx.analysis.profile.integratedLufs;
        r->rmsDb = ctx.analysis.profile.rmsDb;
        r->trackCurve = ctx.analysis.octaveDb;
        r->message = juce::String (cards.front().name) + ": " + ps::assist::targetName (cards.front().target)
                     + " target, " + juce::String (cards.front().loudnessLufs, 1) + " LUFS";
        if (stage == 1)
        {
            r->keepLeft = ex.left;
            r->keepRight = ex.right;
            r->keepRate = ex.sampleRate;
        }
        return r;
    };

    // 1. the default card's gain, then apply it
    if (! ps::assist::refineLoudness (cards.front(), ex, &cancelFlag))
        return;
    progress.store (0.35f);
    publish (results (1));

    // 2. the other cards' gains
    for (size_t k = 1; k < cards.size(); ++k)
    {
        if (! ps::assist::refineLoudness (cards[k], ex, &cancelFlag))
            return;
        progress.store (0.35f + 0.25f * (float) k / (float) cards.size());
    }
    publish (results (2));

    // 3. loudness-match tables, the default first
    for (size_t k = 0; k < cards.size(); ++k)
    {
        if (! ps::assist::buildLoudnessTables (cards[k], ex, ctx.analysis, &cancelFlag))
            return;
        progress.store (0.6f + 0.4f * (float) (k + 1) / (float) cards.size());
        if (k == 0 || k + 1 == cards.size())
            publish (results (k + 1 == cards.size() ? 3 : 2));
    }
}

void Assistant::tick()
{
    // A restore from another thread, a prepare() from another thread during a listen
    if (restorePending.load())
    {
        juce::ValueTree t;
        {
            const std::lock_guard<std::mutex> lock (restoreMutex);
            t = pendingRestore;
            pendingRestore = {};
            restorePending.store (false);
        }
        restoreNow (t);
        changed();
    }
    if (stopRequested.exchange (false) && getPhase() == Phase::Listening)
        cancel();

    // Capture full -> analyse
    if (getPhase() == Phase::Listening && capCapacity.load() > 0 && capWritten.load() >= capCapacity.load())
    {
        capArmed.store (false);
        while (capBusy.load())
            std::this_thread::yield();
        lastLeft = captureL;
        lastRight = captureR;
        lastRate = sampleRate.load();
        releaseCapture();
        invalidateResults();
        phase.store ((int) Phase::Analysing);
        progress.store (0.0f);
        status = "Analysing...";
        auto l = lastLeft, r = lastRight;
        const double rate = lastRate;
        const auto set = settings;
        const auto ref = reference;
        const int gen = generation.load();
        startWorker ([this, gen, l, r, rate, set, ref]() mutable {
            runAnalysis (gen, std::move (l), std::move (r), rate, set, ref, true);
        });
        changed();
    }

    // A finished reference load (dropped when superseded by a clear / another reference / a restore)
    if (referencePendingFlag.exchange (false))
    {
        std::unique_ptr<Results> ref;
        {
            const std::lock_guard<std::mutex> lock (resultsMutex);
            ref = std::move (referenceResult);
        }
        if (referenceWorker.joinable())
            referenceWorker.join();   // it published as its last step
        if (ref != nullptr && ref->generation == referenceGeneration.load())
        {
            loadingReference.store (false);
            if (ref->failed)
                status = ref->message;
            else
            {
                reference = ref->reference;
                settings.engine.useReference = true;
                rebuild();   // with the reference (when the audio of a listen is still here)
            }
            changed();
        }
    }

    if (! pendingFlag.exchange (false))
        return;

    std::unique_ptr<Results> r;
    {
        const std::lock_guard<std::mutex> lock (resultsMutex);
        r = std::move (pending);
    }
    if (r == nullptr || r->generation != generation.load())
        return;

    if (r->failed)
    {
        phase.store ((int) (suggestions.empty() ? Phase::Failed : Phase::Ready));
        status = r->message;
        changed();
        return;
    }

    // A file analysis keeps its excerpt for rebuilds as well.
    suggestions = std::move (r->suggestions);
    detected = r->detected;
    inputLufs = r->inputLufs;
    rmsDb = r->rmsDb;
    trackCurve = r->trackCurve;
    status = r->message;
    if (r->keepRate > 0.0)
    {
        lastLeft = std::move (r->keepLeft);
        lastRight = std::move (r->keepRight);
        lastRate = r->keepRate;
    }
    phase.store ((int) Phase::Ready);
    if (r->applyDefault && ! suggestions.empty())
        apply (0);
    if (applied >= (int) suggestions.size())
        applied = -1;
    changed();
}

//==============================================================================
std::vector<ps::assist::Suggestion> Assistant::getSuggestions() const { return suggestions; }
int Assistant::getAppliedIndex() const { return applied; }
ps::ToneTarget Assistant::getDetectedTarget() const { return detected; }
ps::Curve Assistant::getTrackCurve() const { return trackCurve; }
float Assistant::getInputLufs() const { return inputLufs; }

bool Assistant::apply (int index)
{
    if (index < 0 || index >= (int) suggestions.size())
        return false;
    const auto& s = suggestions[(size_t) index];
    const juce::String description = "Assistant: " + juce::String (s.name) + " (" + ps::assist::targetName (s.target) + ")";
    if (! host.applyAssistantChain (s.values, s.order, description))
        return false;
    applied = index;
    changed();
    return true;
}

ps::assist::MacroValues Assistant::getMacros() const
{
    const auto v = host.currentValues();
    const auto order = host.currentOrder();
    ps::assist::MacroValues m {};
    m[(int) Macro::ToneMatch] = valueOf (v, "eq1.amount");

    const bool tiltBand = valueOf (v, "eq1.a.b7.on") > 0.5f
                          && (int) valueOf (v, "eq1.a.b7.shape") == (int) ps::EqShape::HighShelfBaxandall
                          && std::abs (valueOf (v, "eq1.a.b7.freq") - 4000.0f) < 1.0f;
    m[(int) Macro::Tilt] = tiltBand ? 2.0f * valueOf (v, "eq1.a.b7.gain") : 0.0f;
    m[(int) Macro::Dynamics] = valueOf (v, "pnch.amount");
    m[(int) Macro::Width] = valueOf (v, "img.amount");

    const ps::assist::Suggestion* base = applied >= 0 && applied < (int) suggestions.size()
                                             ? &suggestions[(size_t) applied] : nullptr;
    for (int b = 0; b < ps::assist::kProcBands; ++b)
    {
        const auto id = "img.b" + std::to_string (b + 1) + ".width";
        const float baseWidth = base != nullptr ? valueOf (base->values, id.c_str()) : 0.0f;
        m[(size_t) ((int) Macro::WidthLow + b)] = valueOf (v, id.c_str()) - baseWidth;
    }

    const bool dyn = std::find (order.begin(), order.end(), ps::ModuleType::Dynamics) != order.end();
    const float ratio = valueOf (v, "dyn.a.b1.comp.ratio");
    m[(int) Macro::Compress] = dyn && valueOf (v, "dyn.a.b1.comp.on") > 0.5f && ratio > 1.0f
                                   ? std::clamp ((ratio - 1.2f) / 2.3f * 100.0f, 0.0f, 100.0f) : 0.0f;
    m[(int) Macro::Definition] = valueOf (v, "defn.a.amount");
    m[(int) Macro::ToneBalance] = valueOf (v, "tbal.a.amount");
    m[(int) Macro::Loudness] = valueOf (v, "max.learnTarget");
    return m;
}

void Assistant::setMacro (Macro k, float value, bool gestureEnd)
{
    if (applied < 0 || applied >= (int) suggestions.size() || (int) k < 0 || (int) k >= ps::assist::kNumMacros)
        return;

    const auto base = suggestions[(size_t) applied];
    auto m = getMacros();
    const auto& info = ps::assist::macroInfo (k);
    m[(size_t) k] = std::clamp (value, info.minValue, info.maxValue);

    ps::assist::Analysis lite;
    lite.profile.rmsDb = rmsDb;
    const float comp = loudnessMatched ? ps::assist::loudnessCompensation (base, m) : 0.0f;
    const auto app = ps::assist::applyMacros (base, m, comp, host.currentOrder(), lite);

    const bool gainToo = k == Macro::Loudness || (loudnessMatched && info.loudnessMatched);
    std::vector<std::pair<int, float>> values;
    const auto& specs = ps::paramSpecs();
    for (const auto& [i, v] : app.values)
    {
        const auto& id = specs[(size_t) i].id;
        if (macroOwns (k, id) || (gainToo && id == "max.gain"))
            values.emplace_back (i, v);
    }

    juce::String description;
    if (gestureEnd)
        description = juce::String ("Assistant: ") + info.name + " " + juce::String (m[(size_t) k], 1)
                      + (juce::String (info.unit).isNotEmpty() ? " " + juce::String (info.unit) : juce::String());
    host.applyAssistantEdit (values, k == Macro::Compress ? app.order : host.currentOrder(), description);
}

//==============================================================================
bool Assistant::loadReferenceFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;
    if (loadingReference.exchange (true))
        return false;
    status = "Analysing the reference " + file.getFileName() + "...";
    // Its own thread: a running analysis goes on; the result rebuilds the cards when it arrives. (An older load that a
    // clear / restore superseded is still finishing in rare cases: wait for it.)
    if (referenceWorker.joinable())
        referenceWorker.join();
    const int gen = referenceGeneration.fetch_add (1) + 1;
    referenceWorker = std::thread ([this, gen, file] {
        auto r = std::make_unique<Results>();
        r->generation = gen;
        std::vector<float> l, rr;
        double rate = 0.0;
        if (! readFile (file, kMaxReferenceSeconds, l, rr, rate))
        {
            r->failed = true;
            r->message = "That reference file could not be read.";
        }
        else
        {
            const auto a = ps::assist::analyse (l.data(), rr.data(), (long long) l.size(), rate);
            r->reference = ps::assist::referenceFromAnalysis (a, file.getFileNameWithoutExtension().toStdString());
            if (! r->reference.valid)
            {
                r->failed = true;
                r->message = "The reference file is too short or silent.";
            }
        }
        const std::lock_guard<std::mutex> lock (resultsMutex);
        referenceResult = std::move (r);
        referencePendingFlag.store (true);
    });
    changed();
    return true;
}

void Assistant::setReference (const ps::assist::ReferenceTarget& r)
{
    referenceGeneration.fetch_add (1);   // a load still running is superseded
    loadingReference.store (false);
    reference = r;
    settings.engine.useReference = r.valid;
    rebuild();
    changed();
}

void Assistant::clearReference()
{
    setReference ({});
}

ps::assist::ReferenceTarget Assistant::getReference() const { return reference; }

bool Assistant::referenceActive() const { return reference.valid && settings.engine.useReference; }

//==============================================================================
juce::ValueTree Assistant::createStateTree() const
{
    const std::lock_guard<std::mutex> lock (stateMutex);
    return cachedState.createCopy();
}

void Assistant::changed()
{
    auto t = buildStateTree();
    {
        const std::lock_guard<std::mutex> lock (stateMutex);
        cachedState = t;
    }
    host.assistantChanged();
}

void Assistant::setLoudnessMatched (bool on)
{
    loudnessMatched = on;
    changed();
}

juce::ValueTree Assistant::buildStateTree() const
{
    juce::ValueTree t (kTag);
    t.setProperty ("version", 1, nullptr);
    const auto& e = settings.engine;
    t.setProperty ("profile", e.profile, nullptr);
    t.setProperty ("autoLoudness", e.autoLoudness, nullptr);
    t.setProperty ("loudness", e.loudnessLufs, nullptr);
    t.setProperty ("intensity", (int) e.intensity, nullptr);
    t.setProperty ("useMaximizer", e.useMaximizer, nullptr);
    t.setProperty ("ceiling", e.ceilingDb, nullptr);
    t.setProperty ("truePeak", e.truePeak, nullptr);
    t.setProperty ("useReference", e.useReference, nullptr);
    t.setProperty ("allowed", juce::String::toHexString ((juce::int64) e.allowedModules), nullptr);
    t.setProperty ("seconds", settings.analysisSeconds, nullptr);
    t.setProperty ("loudnessMatched", loudnessMatched, nullptr);
    t.setProperty ("applied", applied, nullptr);
    t.setProperty ("detected", (int) detected, nullptr);
    t.setProperty ("inputLufs", inputLufs, nullptr);
    t.setProperty ("rmsDb", rmsDb, nullptr);
    if (trackCurve[0] != 0.0f || trackCurve[80] != 0.0f)
        t.setProperty ("trackCurve", floatsToString (trackCurve), nullptr);

    if (reference.valid)
    {
        juce::ValueTree r (kRef);
        r.setProperty ("name", juce::String (reference.name), nullptr);
        r.setProperty ("lufs", reference.integratedLufs, nullptr);
        r.setProperty ("plr", reference.plrDb, nullptr);
        r.setProperty ("widths", floatsToString (reference.widthPct), nullptr);
        r.setProperty ("curve", floatsToString (reference.octaveDb), nullptr);
        t.appendChild (r, nullptr);
    }

    const auto& specs = ps::paramSpecs();
    for (const auto& s : suggestions)
    {
        juce::ValueTree c (kCard);
        c.setProperty ("name", juce::String (s.name), nullptr);
        c.setProperty ("description", juce::String (s.description), nullptr);
        c.setProperty ("target", (int) s.target, nullptr);
        c.setProperty ("loudness", s.loudnessLufs, nullptr);
        c.setProperty ("order", orderToString (s.order), nullptr);
        c.setProperty ("isDefault", s.isDefault, nullptr);
        c.setProperty ("gainPerLufs", s.gainPerLufs, nullptr);
        c.setProperty ("measured", s.loudnessMeasured, nullptr);
        c.setProperty ("macros", floatsToString (s.macros), nullptr);
        if (s.tablesReady)
        {
            juce::StringArray pos, delta;
            for (int k = 0; k < ps::assist::kNumMacros; ++k)
            {
                pos.add (floatsToString (s.tablePositions[(size_t) k]));
                delta.add (floatsToString (s.tableDeltaLu[(size_t) k]));
            }
            c.setProperty ("tablePos", pos.joinIntoString (" "), nullptr);
            c.setProperty ("tableDelta", delta.joinIntoString (" "), nullptr);
        }
        for (size_t i = 0; i < specs.size() && i < s.values.size(); ++i)
            if (s.values[i] != specs[i].defaultValue)
            {
                juce::ValueTree p (kP);
                p.setProperty ("id", juce::String (specs[i].id), nullptr);
                p.setProperty ("v", s.values[i], nullptr);
                c.appendChild (p, nullptr);
            }
        t.appendChild (c, nullptr);
    }
    return t;
}

void Assistant::restoreStateTree (const juce::ValueTree& t)
{
    if (isMessageThread())
    {
        restoreNow (t);
        changed();
        return;
    }

    // Another thread (a host loading a project off the message thread): queue it for the next tick; saves meanwhile
    // return the restored tree.
    const auto copy = t.isValid() && t.hasType (kTag) ? t.createCopy() : juce::ValueTree (kTag);
    {
        const std::lock_guard<std::mutex> lock (restoreMutex);
        pendingRestore = copy;
        restorePending.store (true);
    }
    const std::lock_guard<std::mutex> lock (stateMutex);
    cachedState = copy.createCopy();
}

void Assistant::restoreNow (const juce::ValueTree& t)
{
    cancelFlag.store (true);
    joinWorker();
    releaseCapture();
    invalidateResults();
    referenceGeneration.fetch_add (1);   // a reference load still running belongs to the old project
    loadingReference.store (false);
    stopRequested.store (false);

    settings = {};
    reference = {};
    suggestions.clear();
    applied = -1;
    loudnessMatched = true;
    detected = ps::ToneTarget::Balanced;
    inputLufs = ps::kNoValue;
    rmsDb = -18.0f;
    trackCurve = {};
    lastLeft.clear();
    lastRight.clear();
    lastRate = 0.0;
    status = {};
    phase.store ((int) Phase::Idle);

    if (! t.isValid() || ! t.hasType (kTag) || (int) t.getProperty ("version", 0) != 1)
        return;

    auto& e = settings.engine;
    e.profile = std::clamp ((int) t.getProperty ("profile", -1), -1, (int) ps::ToneTarget::Count - 1);
    e.autoLoudness = (bool) t.getProperty ("autoLoudness", true);
    e.loudnessLufs = std::clamp (prop (t, "loudness", -10.0f), ps::assist::kMinTargetLufs, ps::assist::kMaxTargetLufs);
    e.intensity = (ps::assist::Intensity) std::clamp ((int) t.getProperty ("intensity", 1), 0, 2);
    e.useMaximizer = (bool) t.getProperty ("useMaximizer", true);
    e.ceilingDb = std::clamp (prop (t, "ceiling", -0.1f), -3.0f, 0.0f);
    e.truePeak = (bool) t.getProperty ("truePeak", false);
    e.useReference = (bool) t.getProperty ("useReference", true);
    e.allowedModules = (unsigned) t.getProperty ("allowed", "ffffffff").toString().getHexValue64();
    settings.analysisSeconds = std::clamp (prop (t, "seconds", 8.0f), kMinSeconds, kMaxSeconds);
    loudnessMatched = (bool) t.getProperty ("loudnessMatched", true);
    detected = (ps::ToneTarget) std::clamp ((int) t.getProperty ("detected", 0), 0, (int) ps::ToneTarget::Count - 1);
    inputLufs = prop (t, "inputLufs", ps::kNoValue);
    rmsDb = std::clamp (prop (t, "rmsDb", -18.0f), -100.0f, 0.0f);
    if (t.hasProperty ("trackCurve"))
        floatsFromString (t.getProperty ("trackCurve").toString(), trackCurve, ps::kNoValue);

    const auto r = t.getChildWithName (kRef);
    if (r.isValid())
    {
        reference.valid = true;
        reference.name = r.getProperty ("name").toString().toStdString();
        reference.integratedLufs = prop (r, "lufs", ps::kNoValue);
        reference.plrDb = prop (r, "plr", ps::kNoValue);
        floatsFromString (r.getProperty ("widths").toString(), reference.widthPct, 0.0f);
        floatsFromString (r.getProperty ("curve").toString(), reference.octaveDb, ps::kNoValue);
    }

    const auto& specs = ps::paramSpecs();
    for (const auto& c : t)
    {
        if (! c.hasType (kCard))
            continue;
        ps::assist::Suggestion s;
        s.name = c.getProperty ("name").toString().toStdString();
        s.description = c.getProperty ("description").toString().toStdString();
        s.target = (ps::ToneTarget) std::clamp ((int) c.getProperty ("target", 0), 0, (int) ps::ToneTarget::Count - 1);
        s.loudnessLufs = std::clamp (prop (c, "loudness", -10.0f), ps::assist::kMinTargetLufs, ps::assist::kMaxTargetLufs);
        s.order = orderFromString (c.getProperty ("order").toString());
        s.isDefault = (bool) c.getProperty ("isDefault", false);
        s.gainPerLufs = std::clamp (prop (c, "gainPerLufs", 1.3f), 0.5f, 6.0f);
        s.loudnessMeasured = (bool) c.getProperty ("measured", false);
        floatsFromString (c.getProperty ("macros").toString(), s.macros, 0.0f);

        const auto pos = juce::StringArray::fromTokens (c.getProperty ("tablePos").toString(), " ", {});
        const auto delta = juce::StringArray::fromTokens (c.getProperty ("tableDelta").toString(), " ", {});
        constexpr int cells = ps::assist::kNumMacros * ps::assist::Suggestion::kTablePoints;
        if (pos.size() == cells && delta.size() == cells)
        {
            bool ok = true;
            for (int i = 0; i < cells; ++i)
            {
                const auto k = (size_t) (i / ps::assist::Suggestion::kTablePoints);
                const auto p = (size_t) (i % ps::assist::Suggestion::kTablePoints);
                const float x = pos[i].getFloatValue(), d = delta[i].getFloatValue();
                ok = ok && std::isfinite (x) && std::isfinite (d) && (p == 0 || x >= s.tablePositions[k][p - 1]);
                s.tablePositions[k][p] = x;
                s.tableDeltaLu[k][p] = std::clamp (finiteOr (d, 0.0f), -24.0f, 24.0f);
            }
            s.tablesReady = ok;
        }

        s.values.resize (specs.size());
        for (size_t i = 0; i < specs.size(); ++i)
            s.values[i] = specs[i].defaultValue;
        for (const auto& p : c)
        {
            const int i = ps::paramIndex (p.getProperty ("id").toString().toStdString());
            if (i >= 0)
                s.values[(size_t) i] = psplug::sanitiseValue (i, prop (p, "v", specs[(size_t) i].defaultValue));
        }
        suggestions.push_back (std::move (s));
    }

    applied = std::clamp ((int) t.getProperty ("applied", -1), -1, (int) suggestions.size() - 1);
    if (! suggestions.empty())
        phase.store ((int) Phase::Ready);
}
} // namespace psplug
