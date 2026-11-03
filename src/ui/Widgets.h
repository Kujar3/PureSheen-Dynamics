#pragma once

#include "ui/Theme.h"
#include "plugin/PluginProcessor.h"

#include <functional>
#include <memory>

// Shared UI building blocks. Owner: ui-shell. Implementation: Widgets.cpp, Spectrum.cpp, Options.cpp, ModulePanel.cpp.
// Module panels (owned by the ui-* module agents) build only on these and on PureSheenProcessor.
namespace ps::ui
{
//==============================================================================
// Number formatting (ASCII only).
namespace fmt
{
    juce::String db (float v, int decimals = 1, bool sign = true);   // "+2.5 dB", "-inf" below -150
    juce::String hz (float v);                                        // "62 Hz", "1.25 kHz", "12.0 kHz"
    juce::String ms (float v);                                        // "0.30 ms", "12 ms", "1.20 s"
    juce::String percent (float v);                                   // "35 %"
    juce::String ratio (float r);                                     // "2.0:1", "-1.5:1" (upward), "1:1"
    juce::String lufs (float v);                                      // "-9.8 LUFS", "--"
    juce::String paramValue (const PureSheenProcessor&, const juce::String& paramId);   // spec unit aware
}

//==============================================================================
// Rotary knob bound to one parameter: caption above, value text below (editable on double-click is NOT used:
// double-click resets to default, the reference suite does "Alt/Option-click resets"; we do: double-click = reset,
// Ctrl-drag = fine, right-click = menu "Reset to default / Enter value... / (MIDI learn reserved)").
// One undo step per drag (begin/end gesture through juce::SliderParameterAttachment).
class ParamKnob : public juce::Component, public juce::SettableTooltipClient
{
public:
    ParamKnob (PureSheenProcessor&, const juce::String& paramId, const juce::String& caption = {});
    ~ParamKnob() override;

    void setParamId (const juce::String& paramId);   // rebinding (A/B channel switch, band switch)
    const juce::String& getParamId() const noexcept;
    void setColour (juce::Colour arc);              // arc / value colour (default accent)
    void setCompact (bool);                          // smaller variant for band strips
    // (added by ui-shell) A second parameter that follows every user edit of this one (A/B Link, L/R gain link):
    // it is set to the same plain value in the same gesture. Empty = none.
    void setLinkedParamId (const juce::String& paramId);
    void setCaption (const juce::String&);           // (added)
    // (added) Shows scale x value + offset (value text, reset item, "Enter value..." in the same scale); the
    // parameter keeps its own value. 1, 0 = as stored.
    void setDisplayScale (float scale, float offset);

    void resized() override;
    void paint (juce::Graphics&) override;
    // (added) mouse handling: drag / Ctrl-drag fine / wheel / double-click reset / right-click menu
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Horizontal / vertical slider variant (gain faders in the I/O panel, band gain strips).
class ParamFader : public juce::Component, public juce::SettableTooltipClient
{
public:
    ParamFader (PureSheenProcessor&, const juce::String& paramId, bool vertical, const juce::String& caption = {});
    ~ParamFader() override;
    void setParamId (const juce::String& paramId);
    void setLinkedParamId (const juce::String& paramId);   // (added) see ParamKnob
    void setColour (juce::Colour);                          // (added) fill colour (default accent)
    void resized() override;
    void paint (juce::Graphics&) override;
    // (added) mouse handling: drag / Ctrl-drag fine / wheel / double-click reset / right-click menu
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Pill toggle bound to a Bool parameter (left clicks only).
class ParamToggle : public juce::Component, public juce::SettableTooltipClient
{
public:
    ParamToggle (PureSheenProcessor&, const juce::String& paramId, const juce::String& label);
    ~ParamToggle() override;
    void setParamId (const juce::String& paramId);
    void setLinkedParamId (const juce::String& paramId);   // (added) see ParamKnob
    void setLabel (const juce::String&);                    // (added)
    void setOnColour (juce::Colour);
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Compact drop-down bound to a Choice parameter (label left, value + chevron right; popup menu on click).
class ParamChoice : public juce::Component, public juce::SettableTooltipClient
{
public:
    ParamChoice (PureSheenProcessor&, const juce::String& paramId, const juce::String& label = {});
    ~ParamChoice() override;
    void setParamId (const juce::String& paramId);
    void setLinkedParamId (const juce::String& paramId);   // (added) see ParamKnob
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Segmented control bound to a Choice parameter ("Stereo | M/S | L/R", "I | II").
class ParamSegments : public juce::Component, public juce::SettableTooltipClient
{
public:
    // labelsOverride: one label per choice; when it has fewer entries than the parameter has choices, only that many
    // (leading) choices are offered (e.g. "Stereo | M/S | L/R" for a channel mode whose 4th choice is reserved).
    ParamSegments (PureSheenProcessor&, const juce::String& paramId, juce::StringArray labelsOverride = {});
    ~ParamSegments() override;
    void setParamId (const juce::String& paramId);
    void setLinkedParamId (const juce::String& paramId);   // (added) see ParamKnob
    void setOnColour (juce::Colour);                        // (added) selected segment tint (default accent)
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

//==============================================================================
// Frequency / dB axes for every spectral display (log frequency 20 Hz .. 20 kHz by default).
struct FreqAxis
{
    float minHz = 20.0f, maxHz = 20000.0f;
    juce::Rectangle<float> area;
    bool mel = false;          // (added) Mel frequency scale instead of log (Options > Spectrum > Frequency scale)
    float hzToX (float hz) const noexcept;
    float xToHz (float x) const noexcept;
};
struct DbAxis
{
    float minDb = -30.0f, maxDb = 15.0f;
    juce::Rectangle<float> area;
    float dbToY (float db) const noexcept;
    float yToDb (float y) const noexcept;
};
void drawFrequencyGrid (juce::Graphics&, const FreqAxis&, const DbAxis&, bool labels = true);

//==============================================================================
// User options (Options menu; stored in the processor's UI state).
struct UiOptions
{
    enum class MeterType { Peak, Rms, PeakRms, KSystem, Momentary, ShortTerm, Integrated };
    enum class MeterScale { DbLinear, DbNonLinear, Bs1771, Ebu9, Ebu18 };
    enum class SpectrumType { Linear, ThirdOctave, Critical, Octave };
    enum class FreqScale { Log, Mel, ExtendedLog };

    MeterType meterType = MeterType::PeakRms;
    MeterScale meterScale = MeterScale::DbNonLinear;
    bool meterMidSide = false;
    float peakHoldMs = 1000.0f;            // 5 .. 1e9 (infinite)
    float rmsWindowMs = 300.0f;            // 10, 50, 300, 1475, 2650, 3825, 5000
    bool detectTruePeaks = true;
    int kSystemType = 14;                  // K-12 / K-14 / K-20

    SpectrumType spectrumType = SpectrumType::Linear;
    FreqScale freqScale = FreqScale::Log;
    float spectrumAveragingS = 0.0f;       // 0 = real time; 1, 3, 5, 10; -1 = infinite
    bool spectrumPeakHold = false;
    bool spectrumFill = true;
    float spectrumTiltDbPerOct = 3.0f;     // around 1 kHz
    int fftSize = 4096;                    // 2048 / 4096 / 8192 / 16384

    bool showTooltips = true;
    bool dimBypassed = true;
    int historyDepth = 200;

    bool showReferenceSpectrum = true;     // (phase 3) the selected reference's curve on the module analysers

    juce::ValueTree toValueTree() const;
    static UiOptions fromValueTree (const juce::ValueTree&);
};

// (added by ui-shell) A frequency axis over `area` that follows the user's frequency-scale option: Log 20 Hz..20 kHz,
// Mel 20 Hz..20 kHz, Extended Log 10 Hz..30 kHz (capped at Nyquist when sampleRate > 0).
FreqAxis makeFreqAxis (const UiOptions&, juce::Rectangle<float> area, double sampleRate = 0.0);

//==============================================================================
// UI-thread spectrum analyser: pulls from a StereoSampleFifo, Hann-windowed FFT (ps::Fft, one frame per pull from the
// newest samples), averaging / peak hold / tilt from UiOptions, output as dB at the display points of a FreqAxis.
class SpectrumAnalyser
{
public:
    SpectrumAnalyser();
    ~SpectrumAnalyser();

    void setOptions (const UiOptions&);
    void setSampleRate (double);
    // Pulls new audio (call once per display frame). Returns true if the shown spectrum changed (a new frame, or the
    // display glide still moving). channelMix: 0 = (L+R)/2,
    // 1 = L, 2 = R, 3 = Mid, 4 = Side.
    bool update (StereoSampleFifo&, int channelMix = 0);
    // (added) Same, for audio the caller already pulled (the editor pulls each FIFO once and shares the block between
    // the meters and the analyser). `right` may be nullptr.
    bool process (const float* left, const float* right, int numSamples, int channelMix = 0);
    bool hasData() const noexcept;                   // (added) at least one FFT frame since clear()
    // dB (tilt applied) at frequency hz, interpolated; -200 before any data.
    float levelDbAt (float hz) const noexcept;
    float peakDbAt (float hz) const noexcept;
    const std::vector<float>& getBinMagnitudesDb() const noexcept;   // long-term average, for Learn
    double getBinHz() const noexcept;
    void clear();

    // Draws the spectrum as a filled path in [minDb, maxDb] (its own dB scale, typically -90 .. 0).
    void draw (juce::Graphics&, const FreqAxis&, float minDb, float maxDb, juce::Colour, bool fill) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

//==============================================================================
// What every module panel can ask of the editor.
struct EditorServices
{
    virtual ~EditorServices() = default;
    virtual const UiOptions& getOptions() const = 0;
    virtual SpectrumAnalyser& getModulePreSpectrum() = 0;    // module tap of the selected module (pre)
    virtual SpectrumAnalyser& getModulePostSpectrum() = 0;   // (post)
    virtual SpectrumAnalyser& getOutputSpectrum() = 0;
    virtual void showHint (const juce::String& text) = 0;    // transient hint in the panel's header

    // (phase 3, Track Referencing) Draws the selected reference's spectrum (its active region) as a thin lavender line
    // on a spectrum display, in that display's own dB scale [minDb, maxDb] (as SpectrumAnalyser::draw), with the
    // analyser's tilt and aligned in level to the master. Draws nothing when UiOptions::showReferenceSpectrum is off
    // or no reference is analysed. Call it after the spectra, before the curves.
    virtual void drawReferenceSpectrum (juce::Graphics&, const FreqAxis&, float minDb, float maxDb)
    {
        juce::ignoreUnused (minDb, maxDb);
    }
};

//==============================================================================
// Base class of the module panels (one per ModuleType, created by createModulePanel).
// Header row (drawn and managed by the base, 34 px): module name, preset chip (module presets), channel-mode
// segments (supportedChannelModes), A/B channel tabs ("Mid | Side" / "Left | Right") with channel power, solo and
// Link when not Stereo, Learn (multiband modules, if hasLearn()), Delta, Reset. Derived panels lay out in
// getContentArea() and read the selected channel set with getChannelSet().
class ModulePanel : public juce::Component
{
public:
    ModulePanel (PureSheenProcessor&, EditorServices&, ps::ModuleType);
    ~ModulePanel() override;

    ps::ModuleType getModuleType() const noexcept;
    int getChannelSet() const noexcept;                 // 0 = A, 1 = B
    void setChannelSet (int);
    juce::String paramId (const juce::String& name, bool perChannelSet) const;   // "eq1.a.b3.freq" builder:
                                                        // paramId ("b3.freq", true) -> "<prefix>.<a|b>.b3.freq"

    // Derived panels that override tick / refresh / paint / resized call the base version (it runs the header).
    virtual void tick();                                // 30 Hz from the editor (meters, spectra, animations)
    virtual void frameTick() {}                         // every display refresh with new spectrum data: repaint the
                                                        // spectrum views only (they move at the screen's rate)
    virtual void refresh();                             // parameters / channel set / chain changed
    virtual bool hasLearn() const { return false; }
    virtual void channelSetChanged() {}

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;   // (added) "bypassed" veil (UiOptions::dimBypassed)
    void resized() override;

    // (added) Transient hint in the header (EditorServices::showHint lands here); fades after ~3 s.
    void showHeaderHint (const juce::String&);
    // (added, importer) The result of "Import preset (.xml)..." from this panel's preset menu: on success the preset
    // chip shows the imported name; the message goes to the header hint.
    void showImportResult (bool ok, const juce::String& presetName, const juce::String& message);

protected:
    PureSheenProcessor& processor;
    EditorServices& services;
    juce::Rectangle<int> getContentArea() const noexcept;
    virtual void layoutContent (juce::Rectangle<int> contentArea) = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Factory (ModulePanels.cpp, owner ui-shell): returns the panel class of each module (EqPanel, DynamicsPanel, ...).
std::unique_ptr<ModulePanel> createModulePanel (PureSheenProcessor&, EditorServices&, ps::ModuleType);

// Module accent colours (chain tiles, panel titles, curves).
juce::Colour moduleColour (ps::ModuleType);

// (phase 2) Channel modes in the UI. currentChannelMode reads "<prefix>.chMode" (and the Exciter's "exc.ts.on");
// isDualChannelMode: the mode has two channel sets (M/S, L/R, T/S); channelSetName: "Mid" / "Side", "Left" / "Right",
// "Transient" / "Sustain" ("" in Stereo); channelModeShortLabel: "Stereo", "M/S", "L/R", "T/S".
ps::ChannelMode currentChannelMode (const PureSheenProcessor&, ps::ModuleType);
bool isDualChannelMode (ps::ChannelMode) noexcept;
juce::String channelSetName (ps::ChannelMode, int set);
juce::String channelModeShortLabel (ps::ChannelMode);
// The module instance whose meters belong to channel set `set` (the T/S twin for set 1 in Transient/Sustain).
ps::Module& moduleForChannelSet (PureSheenProcessor&, ps::ModuleType, int set);

} // namespace ps::ui
