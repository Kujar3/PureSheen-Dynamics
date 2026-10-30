#pragma once

#include "ui/Widgets.h"

// What the editor's own components (header, chain strip, I/O panel, dither panel, popups) can ask of the editor.
// Implemented by PureSheenEditor::Impl. Owner: ui-shell (internal, not a contract for module panels).
namespace ps::ui
{
struct EditorContext
{
    virtual ~EditorContext() = default;

    virtual PureSheenProcessor& getProcessor() = 0;
    virtual EditorServices& getServices() = 0;            // spectrum analysers, hints
    virtual const UiOptions& getUiOptions() const = 0;
    virtual void applyOptions (const UiOptions&) = 0;

    virtual void select (int moduleTypeOrMinus1) = 0;     // -1 = Dither panel
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
