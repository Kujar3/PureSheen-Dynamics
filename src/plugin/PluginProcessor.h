#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/Chain.h"

#include <functional>
#include <memory>
#include <vector>

// PureSheen Dynamics audio processor (public API = contract; private Impl is free). Owner: plugin.
// Implementation: PluginProcessor.cpp, UndoHistory.cpp, Presets.cpp, FactoryPresets.cpp, PluginState.cpp (all in
// src/plugin; the other headers there are internal helpers, not contracts).
//
// Parameters: one APVTS parameter per ps::paramSpecs() entry (same order, same ids; version hint 1 for the 479
// entries of 0.1.1, 2 for the phase 2 appendix). processBlock
// copies the raw parameter values (getRawParameterValue atomics, plain values) into a preallocated float array and
// calls ps::Chain::process. Stereo in / stereo out only (mono buses rejected in isBusesLayoutSupported).
//
// State (getStateInformation): one ValueTree "PureSheen" with
//   - the APVTS parameter state
//   - "Chain"   : ordered children <Module type="eq1"/> (ids from ps::moduleInfo().idPrefix)
//   - "Options" : ps::ui options (meter, spectrum, history depth ...) as properties (opaque to the processor)
//   - "Snapshots": A..D (each a full copy of params + chain), "editorW" / "editorH"
//   - "presetName", "presetModified"
//   - (phase 2) "MatchEq": the Match EQ spectra of both instances (normal + T/S twin), see the Match EQ section below
// Undo history is NOT saved (reference-suite parity). States and presets store parameters by id and the chain by
// module id prefix, so a 0.1.x state (six module types, 479 parameters) loads unchanged: every phase 2 parameter
// takes its (neutral) default.
//
// Threading: everything here is message-thread only except processBlock and the getters marked "any thread".
class PureSheenProcessor final : public juce::AudioProcessor,
                                 public juce::ChangeBroadcaster   // chain / history / preset / snapshot changes
{
public:
    PureSheenProcessor();
    ~PureSheenProcessor() override;

    //==============================================================================
    // juce::AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override;
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Parameters
    juce::AudioProcessorValueTreeState& getValueTreeState() noexcept;
    juce::RangedAudioParameter* getParameter (const juce::String& id) const;   // nullptr if unknown
    float getParameterValue (const juce::String& id) const;                     // plain value (any thread)
    // One complete host gesture (begin, set, end) -> one undo step. For clicks / menu items / programmatic edits.
    void setParameterValue (const juce::String& id, float plainValue);
    // Several parameters as ONE undo step (e.g. "Reset module", "Learn crossovers", linked A/B edits).
    void setParameterValues (const std::vector<std::pair<juce::String, float>>& idsAndPlainValues,
                             const juce::String& undoDescription);

    //==============================================================================
    // Chain (undoable)
    std::vector<ps::ModuleType> getChainOrder() const;
    void setChainOrder (const std::vector<ps::ModuleType>&);          // reorder / add / remove in one step
    void addModule (ps::ModuleType, int insertIndex = -1);            // -1 = end (before the Maximizer if present)
    void removeModule (ps::ModuleType);
    void resetModule (ps::ModuleType);                                // every parameter of the module to default
    bool isModuleInChain (ps::ModuleType) const;

    // Monitoring (not undoable, not saved): forwarded to ps::Chain
    void setSoloModule (int moduleTypeOrMinus1);
    int getSoloModule() const;
    void setDelta (ps::ModuleType, bool);
    bool getDelta (ps::ModuleType) const;
    void setChannelSolo (ps::ModuleType, int channelSetOrMinus1);
    int getChannelSolo (ps::ModuleType) const;
    void setMonitor (ps::ModuleType, const ps::ModuleMonitor&);
    ps::ModuleMonitor getMonitor (ps::ModuleType) const;
    void setTapModule (int moduleTypeOrMinus1);

    ps::Chain& getChain() noexcept;                                   // meters, FIFOs, modules (UI reads)
    int getCurrentLatencySamples() const noexcept;                    // any thread

    //==============================================================================
    // Learn actions
    // Multiband modules: moves the crossovers to minima of the module's input spectrum (long-term average from the
    // module pre-tap, collected while the UI is open; falls back to defaults when there is no audio yet). One undo step.
    void learnCrossovers (ps::ModuleType);
    void startMaximizerLearn();                                       // result written to "max.gain" (one undo step)
    void cancelMaximizerLearn();                                      // (added) stop listening, keep the gain
    bool isMaximizerLearning() const;
    float getMaximizerLearnProgress() const;
    // Long-term average spectrum (dB per FFT bin, 4096-point, Hann) of the module pre-tap; empty if not enough data.
    // The editor feeds it via feedLearnSpectrum from its spectrum analyser (the processor does no FFT work itself).
    void feedLearnSpectrum (ps::ModuleType, const std::vector<float>& magnitudeDb, double binHz);

    //==============================================================================
    // Undo history (reference-suite parity: list of changes, click to jump, A-D snapshots)
    struct HistoryEntry
    {
        int id = 0;
        juce::String description;    // "EQ 1 Band 3 Gain (A): +2.5 dB", "Added Exciter", "Loaded preset Warm Glue"
    };
    std::vector<HistoryEntry> getHistory() const;                     // oldest first; entry 0 = "Initial state"
    int getHistoryPosition() const;                                   // index of the current state in getHistory()
    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
    void jumpToHistory (int index);                                   // like clicking an entry in the history list
    void clearHistory();                                              // keeps the current state as "Initial state"
    void setHistoryDepth (int maxEntries);                            // 10..1000, default 200

    static constexpr int kNumSnapshots = 4;                           // A, B, C, D
    void storeSnapshot (int slot);
    void recallSnapshot (int slot);                                   // one undo step
    bool hasSnapshot (int slot) const;
    void clearSnapshot (int slot);
    int getActiveSnapshot() const;                                    // last recalled / stored slot, -1 when edited since

    //==============================================================================
    // Presets. Files: Documents/PureSheen Dynamics/Presets/Global/<name>.pspreset and
    // Documents/PureSheen Dynamics/Presets/<module idPrefix>/<name>.pspreset (XML). Factory presets are built in.
    struct PresetInfo
    {
        juce::String name;
        juce::String category;      // folder ("Mastering", "Loud", "Gentle", ... / user subfolder)
        bool factory = true;
        juce::File file;            // user presets only
    };
    // moduleOrMinus1: -1 = global presets (whole chain + every parameter), else that module's presets.
    std::vector<PresetInfo> getPresets (int moduleOrMinus1) const;
    bool loadPreset (int moduleOrMinus1, const PresetInfo&);          // one undo step
    bool savePreset (int moduleOrMinus1, const juce::String& name, const juce::String& category = {});
    bool deletePreset (const PresetInfo&);                            // user presets only
    juce::String getCurrentPresetName() const;                        // "Default" for a fresh instance
    bool isPresetModified() const;                                    // asterisk in the header
    void loadNextPreset (int direction);                              // header arrows, global presets

    //==============================================================================
    // Opaque UI state stored with the plug-in (options, last selected module, editor size).
    juce::ValueTree getUiState() const;
    void setUiState (const juce::ValueTree&);

    //==============================================================================
    // Additions (plugin owner, after the skeleton). All message thread.
    //
    // The housekeeping the internal 30 Hz timer does: commits a finished host / UI gesture to the undo history once
    // no gesture has ended for ~250 ms (force = now), reports a changed chain latency to the host (setLatencySamples)
    // and collects a finished Maximizer learn. undo / redo / presets / snapshots flush first, so pending gestures
    // are never lost. Call it directly where timers do not run (dev tool).
    void flushPendingChanges (bool force = false);

    // A-D snapshot from an entry of the history list (reference-suite parity: snapshots are taken from history entries).
    void storeSnapshotFromHistory (int slot, int historyIndex);

    // Preset folder root (default Documents/PureSheen Dynamics/Presets); the self-test points it at a temp folder.
    void setPresetRoot (const juce::File&);
    juce::File getPresetRoot() const;

    // Editor size saved with the state as "editorW" / "editorH" ({0, 0} = never set).
    void setEditorSize (int width, int height);
    juce::Point<int> getEditorSize() const;

    // "io.bypass" doubles as the host's bypass switch (latency-aligned bypass inside the chain).
    juce::AudioProcessorParameter* getBypassParameter() const override;

    //==============================================================================
    // (phase 2) Match EQ. The spectra live in the module (ps::MatchEq, getChain().getModule (MatchEq, part), part 1 =
    // the T/S twin). The processor adds:
    //   - persistence: the plug-in state, the A-D snapshots and global / Match EQ module presets carry the Reference
    //     spectrum of both parts; the state and snapshots also carry the Current spectrum (presets do not). Child
    //     "MatchEq" version 1: <Spectrum part="0" role="reference" source="..." seconds="..." sampleRate="..."
    //     L="base64 float32 x 540" R=".." M=".." S=".."/> per stored spectrum.
    //   - undo: a capture stop, Clear or a loaded file is one history step ("Match EQ: captured the reference").
    //   - a reference spectrum from an audio file (WAV / AIFF / FLAC / MP3), analysed with ps::MatchAnalyser on the
    //     processor's loader thread (never the audio or message thread; cancellable; up to 20 minutes of audio; mono
    //     files count as L = R). When done, the result goes to setSpectrum (Reference) of that part on the message
    //     thread with source "file:<name>" and a change message is sent.
    bool startMatchReferenceFile (const juce::File&, int part = 0);   // false: unreadable / unsupported file
    // Linked T/S references are applied together by the processor, even after the panel has been closed.
    bool startMatchReferenceFile (const juce::File&, int part, bool linkedParts);
    void cancelMatchReferenceFile();
    float getMatchReferenceFileProgress() const;                     // 0..1 while loading, -1 when idle
    juce::String getMatchReferenceFileError() const;                 // last failure (plain text), empty if none

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PureSheenProcessor)
};
