#pragma once

#include "ui/EditorContext.h"
#include "ui/RefComparison.h"

#include <memory>

// The Reference page (Track Referencing, phase 3; owner: referencing UI). Shown in the module-panel area when the
// header's view switch is on "Reference" (EditorContext selection kReferenceSelection); the chain strip and the I/O
// panel stay visible.
//
//   Left     the reference list (up to 10: number, name, length, loudness, loading / error / missing states; click =
//            compare with it, x = remove, a missing file = click to find it) with "+ Add" (several files at once)
//   Right    the waveform strip of the selected reference with the loops A-E (RefWaveform), the controls (Master |
//            Reference switch with the level match, Level match, Analysis window, Sync, Play when stopped, Reset) and
//            the comparison views (RefComparison)
//   Empty    one large drop target ("Drop reference tracks here ... or click to browse")
// Audio files dropped anywhere on the editor are added through addFiles (the editor implements the drop target).
namespace ps::ui
{
class ReferencePanel final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ReferencePanel (EditorContext&);
    ~ReferencePanel() override;

    void tick();                              // 30 Hz: list, playhead, analysis (repaints only what changed)
    void browse();                            // "+ Add": async file chooser, several files allowed

    // Adds audio files to Track Referencing's list (the first one added is selected) and says what happened in the
    // editor's notice pill (list full, unsupported files). Returns how many were added. Message thread.
    static int addFiles (EditorContext&, const juce::StringArray& paths);

    // Snapshot / test hooks
    void showComparisonView (RefComparison::View);
    void armLoop (int letter);                // -1 = none

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
