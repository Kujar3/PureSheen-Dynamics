#include "plugin/UndoHistory.h"

namespace psplug
{
void UndoHistory::reset (const FullState& state, const juce::String& description)
{
    entries.clear();
    Entry e;
    e.id = nextId++;
    e.description = description;
    e.state = state;
    entries.push_back (std::move (e));
    position = 0;
}

void UndoHistory::record (const juce::String& description, const FullState& state, int singleParam, double timeMs)
{
    if (entries.empty())
    {
        reset (state);
        return;
    }

    // Recording after an undo / jump drops the entries after the current one.
    entries.erase (entries.begin() + position + 1, entries.end());

    auto& top = entries.back();
    if (position > 0 && singleParam >= 0 && top.singleParam == singleParam && timeMs - top.timeMs < kCoalesceMs
        && timeMs >= top.timeMs)
    {
        top.description = description;
        top.state = state;
        top.timeMs = timeMs;
        return;
    }

    Entry e;
    e.id = nextId++;
    e.description = description;
    e.state = state;
    e.singleParam = singleParam;
    e.timeMs = timeMs;
    entries.push_back (std::move (e));
    position = (int) entries.size() - 1;
    trim();
}

int UndoHistory::setPosition (int index) noexcept
{
    if (! entries.empty())
        position = juce::jlimit (0, (int) entries.size() - 1, index);
    return position;
}

void UndoHistory::updateCurrentState (const FullState& state)
{
    if (! entries.empty())
        entries[(size_t) position].state = state;
}

void UndoHistory::setDepth (int maxEntries)
{
    depth = juce::jlimit (kMinDepth, kMaxDepth, maxEntries);
    trim();
}

void UndoHistory::trim()
{
    // depth = number of undo steps kept (entries = depth + 1, the oldest being the state they undo to).
    const int excess = (int) entries.size() - (depth + 1);
    if (excess <= 0)
        return;

    // Never drop the current entry: drop from the front, at most up to the position.
    const int drop = juce::jmin (excess, position);
    entries.erase (entries.begin(), entries.begin() + drop);
    position -= drop;
}
} // namespace psplug
