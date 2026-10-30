#pragma once

#include "ui/Widgets.h"

#include "dsp/Envelope.h"

#include <functional>

// Views of the Dynamics panel (owner: ui-dynamics; internal to DynamicsPanel, not a contract):
//   - DynamicsBandState: the static gain computer of one band (compressor -> limiter, knee, upward ratios, parallel
//     mix, auto-gain makeup), evaluated with the DSP's own ps::GainCurve so the drawing matches what is heard
//   - ParamGesture: a drag on one parameter (+ an optional linked one) as one host gesture / one undo step
//   - DynamicsTransferGraph: the selected band's transfer curve (input dB -> output dB) with draggable C / L
//     threshold nodes and a dot that follows the live detector level
//   - DynamicsGrMeters: one gain-change meter per band (downward amber, upward mint), click selects the band
namespace ps::ui
{
struct DynamicsBandState
{
    bool bandOn = true, compOn = true, limOn = false;
    ps::GainCurve comp, lim;          // lim.upwardExpands = true (negative limiter ratio = upward expansion)
    float mix = 1.0f;                 // 0..1 (wet)
    float makeupDb = 0.0f;            // auto gain (0 when off)

    // Static gain change (dB) of the band for a steady detector level, without the band gain.
    float gainDb (float inputDb) const noexcept;
    float outputDb (float inputDb) const noexcept { return inputDb + gainDb (inputDb); }

    static DynamicsBandState read (const PureSheenProcessor&, int channelSet, int band);
};

// "dyn.<a|b>.b<n>.<name>"
juce::String dynamicsBandParamId (int channelSet, int band, const juce::String& name);

//==============================================================================
class ParamGesture
{
public:
    explicit ParamGesture (PureSheenProcessor& p) : processor (p) {}
    ~ParamGesture() { end(); }

    void start (const juce::String& paramId, const juce::String& linkedParamId);
    void set (float plainValue);          // opens the gesture on the first change
    void end();
    bool isActive() const noexcept { return param != nullptr; }
    float get() const;                    // current plain value of the main parameter

    // One complete edit (one undo step) of a parameter and its linked twin.
    static void setComplete (PureSheenProcessor&, const juce::String& paramId, const juce::String& linkedParamId,
                             float plainValue);
    static float defaultOf (const juce::String& paramId);

private:
    PureSheenProcessor& processor;
    juce::RangedAudioParameter* param = nullptr;
    juce::RangedAudioParameter* linked = nullptr;
    bool open = false;
};

//==============================================================================
class DynamicsTransferGraph final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit DynamicsTransferGraph (PureSheenProcessor&);
    ~DynamicsTransferGraph() override;

    // Which band / channel set is shown; linkedSet >= 0: edits also go to that set (Link).
    void setTarget (int channelSet, int band, int linkedSet, juce::Colour colour, const juce::String& setName);
    void setLive (float detectorDb, float gainDb, bool hasSignal);   // smoothed live values (30 Hz)

    static constexpr float kMinDb = -60.0f, kMaxDb = 6.0f;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> plotArea() const;
    float xOf (float db) const;
    float yOf (float db) const;
    float dbOfX (float x) const;
    int nodeAt (juce::Point<float>) const;            // 0 = compressor, 1 = limiter, -1 = none
    juce::Point<float> nodePosition (int node, const DynamicsBandState&) const;
    juce::String idFor (int node, int set) const;

    PureSheenProcessor& processor;
    ParamGesture gesture;
    int set = 0, band = 0, linkedSet = -1;
    juce::Colour colour;
    juce::String setLabel;
    float liveDb = -200.0f, liveGainDb = 0.0f;
    bool live = false;
    int hoverNode = -1, dragNode = -1;
    float dragDb = 0.0f, lastX = 0.0f;
};

//==============================================================================
class DynamicsGrMeters final : public juce::Component, public juce::SettableTooltipClient
{
public:
    DynamicsGrMeters();

    // gainDb[b]: current total gain change of band b (<= 0 downward, > 0 upward).
    void setValues (int numBands, const float* gainDb, int selectedBand, int soloBand, juce::Colour colour);

    void resetHolds();                    // channel set switched: forget the other set's peaks

    std::function<void (int band)> onSelect;

    static constexpr float kMinDb = -24.0f, kMaxDb = 6.0f;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> meterArea (int band) const;

    int bands = 1, selected = 0, solo = -1;
    float values[ps::kMaxBands] {};
    float holds[ps::kMaxBands] {};
    int holdTicks[ps::kMaxBands] {};
    juce::Colour colour;
};
} // namespace ps::ui
