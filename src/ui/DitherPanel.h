#pragma once

#include "ui/EditorContext.h"

// The Dither panel (shown in the module area when the I/O panel's Dither button is clicked): every "dith." parameter,
// the bit meter (which of the 24 bits are in use, per channel, with peak hold) and the DC offset meter.
// Owner: ui-shell.
namespace ps::ui
{
class DitherPanel final : public juce::Component
{
public:
    explicit DitherPanel (EditorContext&);
    ~DitherPanel() override;

    void tick();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
