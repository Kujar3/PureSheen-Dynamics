#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "plugin/PluginProcessor.h"
#include "ui/Widgets.h"

#include <memory>

/**
    PureSheen Dynamics editor. Owner: ui-shell. Implementation: PluginEditor.cpp, HeaderBar.*, ChainStrip.*,
    IoPanel.*, DitherPanel.*, HelpOverlay.*.

    Layout at the default 1180 x 720 (resizable 980 x 600 .. 1800 x 1100, size persisted in the UI state):

      +----------------------------------------------------------------------------------------------+
      | HeaderBar (46): logo + "PureSheen", preset bar (< name* >, Save, menu), Undo / Redo / History,  |
      |                 snapshots A B C D, Options, Help                                              |
      +----------------------------------------------------------------------------------------------+
      | ChainStrip (70): module tiles in processing order (drag to reorder; click = select; per tile:   |
      |   name, power LED, S(olo), delta, difference meter, x on hover) + "+" tile (add menu)           |
      +------------------------------------------------------------------------+---------------------+
      | Selected module's ModulePanel (or the DitherPanel)                    | IoPanel (176):       |
      |                                                                        |  input / output      |
      |                                                                        |  meters + readouts,  |
      |                                                                        |  gains, Gain Match,  |
      |                                                                        |  Bypass, Mono, Swap, |
      |                                                                        |  Dither, LUFS / TP   |
      +------------------------------------------------------------------------+---------------------+

    The editor implements EditorServices (options, the three SpectrumAnalysers fed from the chain's FIFOs, hints),
    pulls the FIFOs and repaints the spectrum views on every display refresh (VBlankAttachment), and runs one 30 Hz
    timer that ticks the visible panel, the strip and the I/O panel, tells the processor which module
    to tap (setTapModule = selected module), and feeds processor.feedLearnSpectrum for the selected multiband module.
    Nothing takes keyboard focus away from the host (FL keeps its shortcuts); right-clicks never act as left clicks.
*/
class PureSheenEditor final : public juce::AudioProcessorEditor,
                              public ps::ui::EditorServices,
                              public juce::FileDragAndDropTarget,
                              private juce::ChangeListener,
                              private juce::Timer
{
public:
    explicit PureSheenEditor (PureSheenProcessor&);
    ~PureSheenEditor() override;

    // A single-module plugin has no chain strip, so its window is that much lower.
    static int defaultHeightFor (const psplug::Product&) noexcept;
    static int minHeightFor (const psplug::Product&) noexcept;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Selection (the tile highlighted in the strip). -1 = the Dither panel.
    void selectModule (int moduleTypeOrMinus1);
    int getSelectedModule() const noexcept;

    // EditorServices
    const ps::ui::UiOptions& getOptions() const override;
    ps::ui::SpectrumAnalyser& getModulePreSpectrum() override;
    ps::ui::SpectrumAnalyser& getModulePostSpectrum() override;
    ps::ui::SpectrumAnalyser& getOutputSpectrum() override;
    void showHint (const juce::String&) override;
    // (phase 3) the selected reference's curve on a spectrum display (Track Referencing; see EditorServices)
    void drawReferenceSpectrum (juce::Graphics&, const ps::ui::FreqAxis&, float minDb, float maxDb) override;

    // (phase 3) Audio files dropped anywhere on the editor go to Track Referencing's list and open the Reference page
    // (an overlay shows while dragging, coral for files that are not audio).
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    void setOptions (const ps::ui::UiOptions&);         // Options menu; saved in the processor UI state
    void setHelpVisible (bool);
    void setHistoryVisible (bool);                       // (added) the History popup under the header's button

    // Snapshot / test hooks
    ps::ui::ModulePanel* getCurrentPanel() noexcept;
    void updateNow();                                    // one synchronous tick (dev tool: timers do not run)
    void showNotice (const juce::String&);               // (added) the notice pill (header results, e.g. an import)

    static constexpr int kDefaultWidth = 1180, kDefaultHeight = 720;
    static constexpr int kMinWidth = 980, kMinHeight = 600;
    static constexpr int kMaxWidth = 1800, kMaxHeight = 1100;
    static constexpr int kScreenMarginW = 40, kScreenMarginH = 90;   // a first window keeps this much of the screen free

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PureSheenEditor)
};
