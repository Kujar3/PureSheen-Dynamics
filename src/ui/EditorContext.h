#pragma once

#include "ui/Widgets.h"

// What the editor's own components (header, chain strip, I/O panel, dither panel, popups) can ask of the editor.
// Implemented by PureSheenEditor::Impl. Owner: ui-shell (internal, not a contract for module panels).
namespace ps::ui
{
// (phase 3) Selections besides the modules (0..) and the Dither panel (-1): the Assistant page, and "back to the chain"
// (select (kChainSelection) returns to the module that was selected before the Assistant opened).
inline constexpr int kAssistantSelection = -3;
inline constexpr int kChainSelection = -4;

struct EditorContext
{
    virtual ~EditorContext() = default;

    virtual PureSheenProcessor& getProcessor() = 0;
    virtual EditorServices& getServices() = 0;            // spectrum analysers, hints
    virtual const UiOptions& getUiOptions() const = 0;
    virtual void applyOptions (const UiOptions&) = 0;

    virtual void select (int moduleTypeOrMinus1) = 0;     // -1 = Dither panel, kAssistantSelection, kChainSelection
    virtual int getSelection() const = 0;

    virtual void toggleHelp() = 0;
    virtual bool isHelpOpen() const = 0;
    virtual void toggleHistory() = 0;
    virtual bool isHistoryOpen() const = 0;

    virtual void notify (const juce::String& text) = 0;  // short message at the bottom of the editor

    // L/R link of the input / output gain faders (UI state, not a parameter).
    virtual bool isGainLinked (bool output) const = 0;
    virtual void setGainLinked (bool output, bool linked) = 0;
};
} // namespace ps::ui
