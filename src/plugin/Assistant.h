#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>

#include "assist/AssistEngine.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// The Assistant on the processor side (owner: assistant). The processor owns one (PureSheenProcessor::getAssistant()).
//
//   Listen: the processor feeds every input block (pushAudio, audio thread, before the chain: what the Assistant hears is
//   the unprocessed mix). While listening, non-silent blocks (> -70 dBFS) are copied into a buffer the message thread
//   allocated for Settings::analysisSeconds; silence does not count, so the user can start playback after pressing
//   Listen. A file can be analysed instead (analyseFile: loader thread, the first analysisSeconds of its loudest part).
//   Analyse (worker thread): ps::assist::analyse, buildSuggestions, then the Maximizer gain of the default suggestion
//   (refineLoudness on the captured audio), which is applied right away (one undo step, "Assistant: Suggested (Rock)"),
//   then the other cards' gains and the default's loudness-match tables. A card applied later gets its tables then.
//   Macros: positions are read from the current parameter values (Tone Match = EQ 1 Amount, Width Match = Imager Amount,
//   Loudness = the Maximizer's target ...), so undo, snapshots and hand edits keep them consistent; a move writes only
//   that macro's parameters (+ the Maximizer gain when loudness matching is on, or for the Loudness macro). A drag is
//   one undo step (setMacro with gestureEnd on mouse-up).
//   Reference: a ReferenceTarget from a file (loadReferenceFile, worker thread) or from a caller (setReference; Track
//   Referencing will use it). Changing targets or the reference rebuilds the cards from the kept analysis.
//
// State: the "Assistant" child of the plug-in state (version 1): settings, loudness matching, the reference target, the
// cards (sparse values: only those that differ from our defaults), the applied card, and the bits of the analysis the
// macros need. The captured audio is not saved: after reopening, new targets need a new listen.
//
// Threading: everything is message thread except pushAudio (audio thread), the workers, prepare (any thread: a listen
// in progress is stopped at the next tick) and restoreStateTree / createStateTree (any thread: a restore from another
// thread is queued for the next tick, the saved tree is a cache the message thread rebuilds at every change). Results
// reach the message thread through tick() (the processor's 30 Hz housekeeping), which also sends the change message;
// every job and reference load carries a generation, and results of a superseded one are dropped.
// Capture handover (audio thread vs message thread): the audio thread raises `busy`, then checks `armed` and loads the
// pointers; the message thread lowers `armed`, clears the pointers, then waits for `busy` to drop before freeing. With
// sequentially consistent atomics an audio callback either sees the cleared state or holds the buffer until the message
// thread has waited for it.
namespace psplug
{
// What the Assistant needs from the processor (implemented by PureSheenProcessor::Impl).
struct AssistantHost
{
    virtual ~AssistantHost() = default;
    virtual std::vector<float> currentValues() const = 0;
    virtual std::vector<ps::ModuleType> currentOrder() const = 0;
    // A whole chain (every preset-scope value + order) as one undo step.
    virtual bool applyAssistantChain (const std::vector<float>& values, const std::vector<ps::ModuleType>& order,
                                      const juce::String& description) = 0;
    // Some values (+ order). recordDescription empty: applied without an undo step (a drag in progress); else the
    // current state is recorded as one step.
    virtual void applyAssistantEdit (const std::vector<std::pair<int, float>>& values,
                                     const std::vector<ps::ModuleType>& order, const juce::String& recordDescription) = 0;
    virtual void assistantChanged() = 0;
};

class Assistant
{
public:
    enum class Phase { Idle, Listening, Analysing, Ready, Failed };

    static constexpr float kMinSeconds = 5.0f, kMaxSeconds = 30.0f;

    struct Settings
    {
        ps::assist::Settings engine;
        float analysisSeconds = 8.0f;     // 5..30
    };

    explicit Assistant (AssistantHost&);
    ~Assistant();

    // Message thread (prepareToPlay): the capture rate. Stops a listen in progress.
    void prepare (double sampleRate);
    // Audio thread. No allocation, no locks.
    void pushAudio (const float* left, const float* right, int numSamples) noexcept;
    // Message thread, ~30 Hz (processor housekeeping): starts the analysis when the capture is full, collects results.
    void tick();

    //==============================================================================
    Settings getSettings() const;
    // rebuildNow: rebuild the cards from the kept audio (when there is one) and apply the new default card (one undo step).
    void setSettings (const Settings&, bool rebuildNow = false);
    void rebuild();                       // the same without changing the settings (no-op without kept audio)

    void startListening();
    void cancel();                        // stops listening / analysing; keeps the cards
    bool analyseFile (const juce::File&); // false: unreadable
    Phase getPhase() const noexcept;
    float getProgress() const noexcept;   // 0..1 while listening / analysing
    juce::String getStatusText() const;   // plain words for the UI
    bool hasAnalysis() const;             // the audio of the last listen is still in memory (new targets work)

    std::vector<ps::assist::Suggestion> getSuggestions() const;
    int getAppliedIndex() const;          // -1 = none
    bool apply (int index);               // one undo step
    ps::ToneTarget getDetectedTarget() const;
    ps::Curve getTrackCurve() const;      // the analysed track relative to pink (TrackAnalysis curve points)
    float getInputLufs() const;

    ps::assist::MacroValues getMacros() const;
    void setMacro (ps::assist::Macro, float value, bool gestureEnd);
    bool isLoudnessMatched() const noexcept { return loudnessMatched; }
    void setLoudnessMatched (bool on);

    bool loadReferenceFile (const juce::File&);   // worker thread; false: unreadable
    void setReference (const ps::assist::ReferenceTarget&);
    void clearReference();
    ps::assist::ReferenceTarget getReference() const;
    bool isLoadingReference() const noexcept { return loadingReference.load(); }
    bool isWorking() const noexcept { return workerActive.load(); }   // cards shown, gains / tables still measured

    // Any thread (a host may save from its own thread): a copy of the tree the message thread built at the last change.
    juce::ValueTree createStateTree() const;
    void restoreStateTree (const juce::ValueTree&);   // invalid tree = reset

private:
    struct Results;
    void startWorker (std::function<void()>);
    void joinWorker();
    void runAnalysis (int generation, std::vector<float> left, std::vector<float> right, double rate,
                      Settings settings, ps::assist::ReferenceTarget reference, bool applyDefault);
    void publish (std::unique_ptr<Results>);
    void invalidateResults();
    void releaseCapture();
    void restoreNow (const juce::ValueTree&);
    bool referenceActive() const;   // the reference is loaded and selected as the target
    void changed();                      // rebuilds the cached state tree, notifies the host
    juce::ValueTree buildStateTree() const;

    AssistantHost& host;

    // capture (pointer + capacity published to the audio thread)
    std::vector<float> captureL, captureR;
    std::atomic<float*> capL { nullptr }, capR { nullptr };
    std::atomic<long long> capCapacity { 0 }, capWritten { 0 };
    std::atomic<bool> capArmed { false }, capBusy { false };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<bool> stopRequested { false };   // prepare() from another thread while listening

    // worker
    std::thread worker, referenceWorker;
    std::atomic<bool> cancelFlag { false }, loadingReference { false }, workerActive { false };
    std::atomic<int> phase { (int) Phase::Idle };
    std::atomic<int> generation { 0 }, referenceGeneration { 0 };
    std::atomic<float> progress { 0.0f };

    // results (worker -> message thread)
    mutable std::mutex resultsMutex;
    std::unique_ptr<Results> pending;     // set by the worker, taken by tick()
    std::unique_ptr<Results> referenceResult;   // set by the reference loader, taken by tick()
    std::atomic<bool> pendingFlag { false }, referencePendingFlag { false };

    std::mutex restoreMutex;
    juce::ValueTree pendingRestore;              // a restore from another thread, applied at the next tick
    std::atomic<bool> restorePending { false };

    mutable std::mutex stateMutex;
    juce::ValueTree cachedState;

    // message-thread state
    Settings settings;
    ps::assist::ReferenceTarget reference;
    std::vector<ps::assist::Suggestion> suggestions;
    int applied = -1;
    bool loudnessMatched = true;
    ps::ToneTarget detected = ps::ToneTarget::Balanced;
    float inputLufs = ps::kNoValue;
    float rmsDb = -18.0f;
    ps::Curve trackCurve {};
    juce::String status;
    std::vector<float> lastLeft, lastRight;   // the last captured audio (for rebuilds)
    double lastRate = 0.0;
};
} // namespace psplug
