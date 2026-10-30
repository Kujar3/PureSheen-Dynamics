#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>

#include "dsp/MatchAnalysis.h"

#include <map>
#include <memory>

namespace ps
{
class Chain;
class MatchEq;
}

// Match EQ parts of the processor (owner: dsp-meq / plugin-meq; internal helper of PluginProcessor.cpp, not a
// contract). Message thread unless noted.
//
//   - The "MatchEq" state child (PluginProcessor.h): version 1, one <Spectrum/> per stored (valid) spectrum:
//       <MatchEq version="1">
//         <Spectrum part="0" role="reference" source="file:Song" seconds="182.4" sampleRate="44100" valid="1"
//                   L="base64 float32 LE x 540" R=".." M=".." S=".."/>
//       </MatchEq>
//     The plug-in state and A-D snapshots carry both roles of both parts (part 1 = the T/S twin); global and Match EQ
//     module presets carry the Reference of both parts. A state without the child (0.1.x) clears every spectrum.
//     Loading a preset that carries the child replaces the Reference of both parts (a part without a stored
//     reference is cleared); a preset without the child (factory presets, presets saved before 0.2) leaves the
//     References alone, except the factory Match EQ presets named in factoryMatchReference (synthesised reference).
//   - Undo: every history entry is tagged with the spectra at that point (shared, immutable sets: memory is only
//     used when spectra change). A capture stop, Clear, a loaded file (or any other spectrum change made directly on
//     the module) becomes one history step at the next housekeeping tick ("Match EQ: captured the reference"); undo /
//     redo / jump restore the tagged spectra.
//   - The reference-file loader thread (WAV / AIFF / FLAC / MP3 / Ogg via juce::AudioFormatManager): decodes 64k
//     sample chunks into a ps::MatchAnalyser prepared at the file's rate (no resampling: the grid is in Hz), up to 20
//     minutes, mono = L = R, cancellable, progress 0..1, plain-text errors. The result is applied on the message
//     thread by poll() (source "file:<name>").
namespace psplug
{
struct MatchSpectraSet
{
    ps::MatchSpectrum spectra[2][2];   // [part][role]: part 0 = normal instance, 1 = T/S twin; role 0 = Reference
};
using MatchSpectraPtr = std::shared_ptr<const MatchSpectraSet>;

bool sameSpectrum (const ps::MatchSpectrum&, const ps::MatchSpectrum&) noexcept;
ps::MatchSpectrum emptySpectrum() noexcept;

// <MatchEq version="1"> with the valid spectra (includeCurrent false: references only).
juce::ValueTree matchSpectraToTree (const MatchSpectraSet&, bool includeCurrent);
// Reads a <MatchEq> tree. Every role it covers (both, or references only) that has no <Spectrum/> is cleared; an
// invalid tree clears them all. Returns false when the tree is valid but not a "MatchEq" version 1 child.
bool matchSpectraFromTree (const juce::ValueTree&, MatchSpectraSet&, bool includeCurrent);

// Synthesised references of the factory Match EQ presets (proposed names): "Pink Balance" (-3 dB/oct), "Warm Tilt"
// (-4.5 dB/oct), "Bright Tilt" (-2 dB/oct with a -2 dB shelf below 60 Hz). false for any other name.
bool factoryMatchReference (const juce::String& presetName, ps::MatchSpectrum& out);

class UndoHistory;

class MatchEqState
{
public:
    explicit MatchEqState (ps::Chain&);
    ~MatchEqState();

    // ---- plug-in state (host-thread reads are serialised with spectrum-set restores)
    juce::ValueTree createStateTree() const;
    void restoreStateTree (const juce::ValueTree& matchEqChildOrInvalid);

    // ---- A-D snapshots
    void storeSnapshot (int slot);
    void storeSnapshotFromHistory (int slot, const UndoHistory&, int historyIndex);
    bool recallSnapshot (int slot);                      // true when spectra changed
    void clearSnapshot (int slot);
    void clearAllSnapshots();
    juce::ValueTree snapshotTree (int slot) const;       // invalid when the slot has no Match EQ data
    void restoreSnapshotTree (int slot, const juce::ValueTree& matchEqChildOrInvalid);

    // ---- presets
    bool appendToPresetFile (const juce::File&, int scope) const;   // global / Match EQ scope only
    void beginPresetLoad (int scope, bool factory, const juce::String& name, const juce::File& file);
    bool applyPendingPreset();                           // true when spectra changed
    void setPresetBaseline();                           // reference spectra of the last global load / save
    bool isPresetModified() const;

    // ---- history
    void historyReset (const UndoHistory&);
    void historyRecorded (const UndoHistory&);           // tags the current entry with the accepted spectra
    bool applyHistory (const UndoHistory&);              // restores the current entry's spectra; true if changed
    // Housekeeping: applies a finished file load, then detects spectra that differ from the accepted ones. true =
    // the caller should commit pending gestures, call acceptChange() and record `description` as a history step.
    bool checkChange (juce::String& description);
    void acceptChange();

    // ---- reference file loader
    bool startFile (const juce::File&, int part, bool linkedParts = false);
    void cancelFile();
    float getFileProgress() const;                       // 0..1 while loading, -1 when idle (any thread)
    juce::String getFileError() const;

    MatchSpectraPtr readModules() const;                 // fresh copy of every module spectrum

private:
    ps::MatchEq& module (int part) const;
    bool applyToModules (const MatchSpectraSet&, bool stopCaptures = true); // returns true if any spectra changed
    void noteModuleGenerations();
    void prune (const UndoHistory&);

    ps::Chain& chain;
    mutable juce::CriticalSection lock;
    MatchSpectraPtr accepted, pendingChange, presetBaseline;
    unsigned seenGeneration[2] { 0, 0 };
    std::map<int, MatchSpectraPtr> tags;                 // history entry id -> spectra
    MatchSpectraPtr snapshots[4];

    bool presetPending = false;
    bool presetHasData = false;
    MatchSpectraSet presetSpectra;

    class Loader;
    std::unique_ptr<Loader> loader;
};

#if PURESHEEN_DEV_TOOL
// Plug-in level tests of the Match EQ state, undo, presets and file loader (PureSheenDev; MatchEqStateTests.cpp).
// Prints "  PASS / FAIL <name>" lines and returns the number of failed checks.
int runMatchEqStateTests();
#endif
} // namespace psplug
