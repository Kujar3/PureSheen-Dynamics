#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>

#include "assist/AssistEngine.h"
#include "core/RefCompare.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

// Track Referencing (phase 3, owner: referencing). The processor owns one (PureSheenProcessor::getTrackReferencing()).
// Ported from RefMatch (the family's reference-track matcher): its playback engine, threads and analysis, extended to a
// list of up to kMaxReferences references with five loops (A-E) each.
//
//   List: up to 10 references. Each is decoded and analysed ONCE on the loader thread (ps::assist::analyse: the
//   TrackProfile the comparison uses and the ReferenceTarget the Assistant uses). Decoded audio stays in memory for
//   the selected reference and, least recently selected first out, the others within getMemoryBudget(); an evicted
//   reference keeps its analysis, overview and loops, and is decoded again when selected (the master plays meanwhile).
//   Loops: five per reference (A-E), each set or empty; at most one is active (-1 = the whole track). The active
//   region is what plays (looped) and what the comparison analyses.
//   Playback (audio thread, process(), after the chain): the selected reference at the host rate, synced to the host
//   position while the host plays (Sync on), free running while it is stopped (Play when stopped on); level matched to
//   the master; equal-power A/B crossfade (30 ms); jumps crossfade (8 ms) through a pool of 8 read heads. The master is
//   never changed in level; with Listen settled on the master the buffer is left untouched (bit-exact). Mono and Swap
//   of the I/O panel apply to the reference too, so the comparison stays fair. Offline renders always pass the master.
//   Analysis thread: a TrackAnalyser on the master (the output after every module), the Comparison against the active
//   region, and the level-match target. The master is fed only while at least one reference is in the list, or while
//   setMasterAnalysisWanted is on (1.3: the connector).
//
// Threading: the list, settings and state are message thread unless noted. process() is the audio thread: no
// allocation, no locks (one try-locked slot, as in RefMatch). Getters marked "any thread" are atomics or copies under
// short locks. createStateTree / restoreStateTree may run on a host thread (cached tree / queued restore, applied at
// the next tick()).
namespace psplug
{
// An immutable, fully decoded reference at the host sample rate (always 2 channels: mono files are duplicated).
struct RefTrack
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 0.0;

    long long numSamples() const noexcept { return audio.getNumSamples(); }
    double lengthSeconds() const noexcept { return sampleRate > 0.0 ? (double) numSamples() / sampleRate : 0.0; }
};

// What the UI draws for a reference; survives eviction of the audio.
struct RefDrawData
{
    ps::WaveformOverview overview;        // kOverviewColumns min/max columns of the mono sum
    std::vector<float> shortTermCurve;    // short-term LUFS every 100 ms (ps::shortTermLoudnessCurve)
    double lengthSeconds = 0.0;

    static constexpr int kOverviewColumns = 2048;
};

class TrackReferencing
{
public:
    static constexpr int kMaxReferences = 10;
    static constexpr int kNumLoops = 5;                       // A-E
    static constexpr double kMinLoopSeconds = 0.5;
    static constexpr double kMaxReferenceSeconds = 20.0 * 60.0;
    static constexpr double kListenFadeMs = 30.0, kJumpFadeMs = 8.0, kMonoFadeMs = 20.0, kGainSmoothMs = 250.0;
    static constexpr long long kDefaultMemoryBudget = 768LL * 1024 * 1024;

    enum class State
    {
        Loading,    // decoding / analysing (first load, relink or a re-decode after eviction / a rate change)
        Ready,      // analysed; audio resident or not (Entry::resident)
        Error,      // could not be loaded (Entry::error says why); relink to fix
        Missing     // the file was not found (a project opened on another PC); relink to fix
    };

    struct Loop
    {
        bool set = false;
        double start = 0.0, end = 0.0;   // seconds

        bool operator== (const Loop& o) const noexcept
        {
            return set == o.set && (! set || (start == o.start && end == o.end));
        }
        bool operator!= (const Loop& o) const noexcept { return ! (*this == o); }
    };

    struct Settings
    {
        ps::LevelMatch levelMatch = ps::LevelMatch::Integrated;
        bool sync = true;                 // follow the host position while it plays (off: run on its own)
        bool playWhenStopped = true;      // keep playing while the host is stopped
        ps::Averaging averaging = ps::Averaging::Recent;
    };

    // A snapshot of one reference for the UI.
    struct Entry
    {
        int id = 0;                       // stable while the reference is in the list (not saved)
        juce::File file;
        juce::String name;                // file name without extension
        State state = State::Loading;
        juce::String error;               // plain words (Error / Missing)
        float progress = 0.0f;            // 0..1 while Loading
        bool resident = false;            // decoded audio is in memory (playable now)
        bool analysing = false;           // the active region's profile is being measured
        double lengthSeconds = 0.0;
        double fileSampleRate = 0.0;
        int fileChannels = 0;
        float integratedLufs = ps::kNoValue;   // whole file
        float truePeakDb = ps::kNoValue;       // whole file
        std::array<Loop, kNumLoops> loops {};
        int activeLoop = -1;              // -1 = whole track
    };

    struct Info
    {
        std::vector<Entry> entries;       // list order
        int selected = -1;                // index into entries, -1 = none
        int version = 0;                  // bumps at every change
    };

    explicit TrackReferencing (juce::AudioProcessor& owner);
    ~TrackReferencing();

    //==============================================================================
    // Host lifecycle (message thread, audio stopped).
    void prepare (double sampleRate, int maximumBlockSize);
    void release();

    // Audio thread, after the chain, on the final output (the master). `mono` / `swap`: the I/O panel's monitor
    // switches (applied to the reference). No allocation, no locks.
    void process (float* left, float* right, int numSamples, bool mono, bool swap) noexcept;

    // Message thread, ~30 Hz (processor housekeeping): finished loads, queued restores, the Assistant link, the cached
    // state. Returns true when something the UI shows changed.
    bool tick();

    //==============================================================================
    // List (message thread)
    static bool isSupportedFile (const juce::File&);
    static juce::String getSupportedWildcard();          // "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"

    // Adds a file (selects it when `select`). Returns the new id, or 0 when the list is full or the file type is not
    // supported. Adding a file that is already in the list selects that entry and returns its id.
    int addReference (const juce::File&, bool select = true);
    void removeReference (int id);
    void relink (int id, const juce::File&);             // Missing / Error: load another file, keeping the loops
    void select (int id);                                // 0 = none
    int getSelectedId() const;
    Info getInfo() const;                                // any thread
    int getInfoVersion() const noexcept;                 // any thread
    std::shared_ptr<const RefDrawData> getDrawData (int id) const;   // nullptr until loaded; any thread

    // Loops (message thread). setLoop clamps to the file (>= kMinLoopSeconds) and makes that letter active.
    void setLoop (int id, int letter, double startSeconds, double endSeconds);
    void clearLoop (int id, int letter);                 // clearing the active loop plays the whole track
    void setActiveLoop (int id, int letter);             // -1 = whole track; an empty letter is ignored

    //==============================================================================
    // Listening (message thread unless noted)
    void setListening (bool reference);                  // A/B: true = reference
    bool isListening() const noexcept;                   // any thread
    Settings getSettings() const;
    void setSettings (const Settings&);
    void seek (double seconds);                          // in the selected reference (clamped into the active region)

    double getPlayheadSeconds() const noexcept;          // any thread
    float getMatchGainDb() const noexcept;               // the reference's level-match gain now (any thread)
    float getListenPosition() const noexcept;            // 0 = master .. 1 = reference (the A/B fade), any thread
    bool isHostPlaying() const noexcept;
    double getHostSeconds() const noexcept;      // the host's playhead at the last audio block; < 0 when it has none
    int getMixAnalysisRun() const noexcept;      // changes whenever the master's analysis starts over
    bool isReferenceSounding() const noexcept;           // the reference is playing and audible
    float getReferenceShortTermLufs() const noexcept;    // the reference as heard (unity gain), kNoValue until 3 s

    //==============================================================================
    // Analysis (any thread)
    ps::TrackProfile getMixProfile() const;              // the master
    ps::TrackProfile getReferenceProfile() const;        // the selected reference's active region
    ps::Comparison getComparison() const;
    int getAnalysisVersion() const noexcept;
    void resetMixAnalysis();
    // (1.3) Analyse the master with no reference in the list too (the connector's get_meters reads the master's bands
    // from getMixProfile()). Turning it on with an empty list starts the analysis over: what it held is old.
    void setMasterAnalysisWanted (bool);
    bool isMasterAnalysisWanted() const noexcept;

    // The Assistant's view of a reference (valid once it is analysed); `path` names the file.
    ps::assist::ReferenceTarget getAssistantTarget (int id) const;
    int findByPath (const juce::String& path) const;     // 0 = not in the list

    // Called on the message thread (from tick) when a reference finished its analysis (ok) or failed (ok == false).
    std::function<void (int id, bool ok)> onReferenceAnalysed;
    std::function<void (int id, const juce::String& path)> onReferenceRemoved;   // also for entries a restore drops
    std::function<void (int id, const juce::String& oldPath, const juce::String& newPath)> onReferenceRelinked;

    //==============================================================================
    // State ("TrackReferencing" child, version 1). Listen always restores to the master.
    juce::ValueTree createStateTree() const;             // any thread (cached copy)
    void restoreStateTree (const juce::ValueTree&);      // any thread (queued off the message thread); invalid = clear

    //==============================================================================
    // Tests / dev tool
    void runAnalysisNow();                               // one analysis-thread iteration, synchronously
    bool waitUntilIdle (int timeoutMs) const;            // the loader has nothing pending (does not pump tick())
    int getDropoutCount() const noexcept;                // failed slot try-locks on the audio thread (expected 0)
    void setMemoryBudget (long long bytes);
    long long getMemoryBudget() const noexcept;
    long long getResidentBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace psplug
