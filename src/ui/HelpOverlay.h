#pragma once

#include "ui/EditorContext.h"

// Help overlay (a short plain-language guide over the whole editor) and the History popup. Owner: ui-shell.
namespace ps::ui
{
class HelpOverlay final : public juce::Component
{
public:
    explicit HelpOverlay (PureSheenProcessor&);
    ~HelpOverlay() override;

    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// The undo history as a list under the header's History button: every change, the current state highlighted, steps
// that can be redone dimmed. Click an entry to jump there. Sits on a transparent full-editor layer that closes it on
// a click outside.
class HistoryPopup final : public juce::Component
{
public:
    explicit HistoryPopup (PureSheenProcessor&);
    ~HistoryPopup() override;

    void open (juce::Rectangle<int> anchorInParent);   // shows it under `anchor` (parent coordinates)
    void refresh();
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
