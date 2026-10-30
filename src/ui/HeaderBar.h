#pragma once

#include "ui/EditorContext.h"

// Top bar (46 px): logo + "PureSheen", global preset bar (< name* >, Save, menu), Undo / Redo / History, snapshots
// A B C D, Options, Help. Owner: ui-shell.
namespace ps::ui
{
class HeaderBar final : public juce::Component
{
public:
    explicit HeaderBar (EditorContext&);
    ~HeaderBar() override;

    void update();                                  // preset name, undo state, snapshots (cheap; 30 Hz is fine)
    juce::Rectangle<int> getHistoryButtonBounds() const;   // in this component

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
