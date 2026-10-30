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
    runs one 30 Hz timer that ticks the visible panel, the strip and the I/O panel, tells the processor which module
    to tap (setTapModule = selected module), and feeds processor.feedLearnSpectrum for the selected multiband module.
    Nothing takes keyboard focus away from the host (FL keeps its shortcuts); right-clicks never act as left clicks.
*/
class PureSheenEditor final : public juce::AudioProcessorEditor,
                              public ps::ui::EditorServices,
                              private juce::ChangeListener,
                              private juce::Timer
{
public:
    explicit PureSheenEditor (PureSheenProcessor&);
    ~PureSheenEditor() override;

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

    void setOptions (const ps::ui::UiOptions&);         // Options menu; saved in the processor UI state
    void setHelpVisible (bool);
    void setHistoryVisible (bool);                       // (added) the History popup under the header's button

    // Snapshot / test hooks
    ps::ui::ModulePanel* getCurrentPanel() noexcept;
    void updateNow();                                    // one synchronous tick (dev tool: timers do not run)

    static constexpr int kDefaultWidth = 1180, kDefaultHeight = 720;
    static constexpr int kMinWidth = 980, kMinHeight = 600;
    static constexpr int kMaxWidth = 1800, kMaxHeight = 1100;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PureSheenEditor)
};
