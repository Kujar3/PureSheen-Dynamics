#pragma once

#include "ui/EditorContext.h"

// Right column (176 px): input / output meters (type and scale from UiOptions, peak hold, clip lights), readouts (peak,
// RMS, momentary / short-term / integrated LUFS, true peak, LRA, with a reset), input and output gain (L / R with
// link), Gain Match, Bypass, Mono, Swap, and the Dither button that opens the Dither panel. Owner: ui-shell.
//
// The editor pulls the chain's input / output FIFOs once per tick and hands the audio to processInput / processOutput
// (peak, RMS, true peak and M/S are measured here, in the UI); loudness, LRA, clip and gain-match values come from
// ps::IoMeters.
namespace ps::ui
{
class IoPanel final : public juce::Component
{
public:
    explicit IoPanel (EditorContext&);
    ~IoPanel() override;

    void setSampleRate (double);
    void processInput (const float* left, const float* right, int numSamples);
    void processOutput (const float* left, const float* right, int numSamples);
    void tick();                         // 30 Hz: ballistics, peak hold, readouts
    void resetReadouts();                // clip lights, true peak / peak maxima, integrated loudness and LRA
    void optionsChanged();
    void setDitherSelected (bool);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
