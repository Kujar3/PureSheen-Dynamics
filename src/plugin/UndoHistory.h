#pragma once

#include "plugin/PluginState.h"

#include <juce_core/juce_core.h>

#include <vector>

// Undo history as a list of full-state snapshots (owner: plugin). Pure bookkeeping, message thread only: the
// processor decides when to record (gesture ends, programmatic edits, chain edits, preset / snapshot loads) and
// applies the states it hands back.
//
// entries[0] is the oldest state ("Initial state" until the depth limit drops it), entries[position] is the current
// one. Recording after an undo drops the redo entries; jumping keeps them (click an entry, then click a later one).
namespace psplug
{
class UndoHistory
{
public:
    struct Entry
    {
        int id = 0;
        juce::String description;
        FullState state;
        int singleParam = -1;     // the one parameter this step changed (-1 = several / chain); for coalescing
        double timeMs = 0.0;
        int request = 0;          // the connector request behind this step (0 = none: the user, the host)
    };

    static constexpr int kDefaultDepth = 200, kMinDepth = 10, kMaxDepth = 1000;
    static constexpr double kCoalesceMs = 250.0;

    // Drops everything; the given state becomes entry 0.
    void reset (const FullState&, const juce::String& description = "Initial state");

    // Records a new current state. A step that changes the same single parameter as the current (newest) step within
    // kCoalesceMs replaces it instead (mouse-wheel / repeated nudges = one step). Trims to the depth.
    void record (const juce::String& description, const FullState&, int singleParam, double timeMs, int request = 0);

    int size() const noexcept { return (int) entries.size(); }
    int getPosition() const noexcept { return position; }
    const Entry& operator[] (int index) const noexcept { return entries[(size_t) index]; }
    const Entry& current() const noexcept { return entries[(size_t) position]; }

    bool canUndo() const noexcept { return position > 0; }
    bool canRedo() const noexcept { return position + 1 < (int) entries.size(); }
    // Moves the position (clamped); returns the new position.
    int setPosition (int index) noexcept;
    // Replaces the stored state of the current entry (after applying it, so float round trips do not look like edits).
    void updateCurrentState (const FullState&);

    void setDepth (int maxEntries);
    int getDepth() const noexcept { return depth; }

private:
    std::vector<Entry> entries;
    int position = 0;
    int depth = kDefaultDepth;
    int nextId = 0;

    void trim();
};
} // namespace psplug
