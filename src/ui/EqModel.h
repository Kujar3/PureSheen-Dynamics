#pragma once

#include "ui/Widgets.h"

#include <array>
#include <vector>

// Equalizer panel helpers (owner: ui-eq): a cached view of one Equalizer's parameters for the UI, shape traits,
// band colours and short shape names. Internal to EqPanel / EqDisplay (not a team contract).
namespace ps::ui
{
// What a shape uses (decides what a drag, the wheel and the band strip edit).
struct EqShapeTraits
{
    bool gain = true;     // has a gain (bells, shelves)
    bool q = true;        // has a Q (bells, analog / vintage / resonant shelves, resonant pass)
    bool slope = false;   // HP / LP Flat and Resonant
    bool pass = false;    // HP / LP (any)
};
EqShapeTraits eqShapeTraits (ps::EqShape) noexcept;

// Q as the band strip and the display show it. The analog shelves keep the reference's stored Q (what presets import
// and the DSP is fitted on), but the reference's own UI shows those on another scale (measured, round 17: stored =
// 0.513 x shown - 0.313, UI 1 .. 24 = stored 0.2 .. 12), so we show shown = 1.949 x stored + 0.610 there.
struct EqQDisplay
{
    float scale = 1.0f, offset = 0.0f;
};
EqQDisplay eqQDisplay (ps::EqShape) noexcept;

juce::Colour eqBandColour (int band);             // 0..7
juce::String eqShapeShortName (ps::EqShape);      // "Bell", "Prop Q", "Low Shelf", "Baxandall Low", ...
juce::String eqShapeFullName (ps::EqShape);       // the parameter's choice label

// Builds the grouped shape menu (item id = 1 + shape index), current shape ticked.
juce::PopupMenu eqShapeMenu (ps::EqShape current, bool digital);

//==============================================================================
// Reads the parameters of EQ 1 or EQ 2 through cached raw parameter pointers (cheap enough for 30 Hz) and keeps them
// as a ps::EqParams. Message thread only.
class EqModel
{
public:
    EqModel (PureSheenProcessor&, ps::ModuleType eq1or2);

    // Re-reads every parameter of the module. Returns true when anything changed since the last call.
    bool update();

    const ps::EqParams& get() const noexcept { return params; }
    const ps::EqBandParams& band (int set, int b) const noexcept { return params.bands[set & 1][juce::jlimit (0, ps::kEqBands - 1, b)]; }
    bool isDigital() const noexcept { return params.filterMode == ps::EqFilterMode::Digital; }

    ps::ChannelMode channelMode() const noexcept { return mode; }
    bool isSplit() const noexcept
    {
        return mode == ps::ChannelMode::MidSide || mode == ps::ChannelMode::LeftRight
               || mode == ps::ChannelMode::TransientSustain;
    }
    bool isLinked() const noexcept { return isSplit() && linked; }   // edits go to both channel sets
    juce::String setName (int set) const;                            // "Mid" / "Side" / "Left" / "Right" /
                                                                     // "Transient" / "Sustain" / ""

    juce::String bandId (int set, int b, const char* name) const;    // "eq1.a.b3.freq"
    juce::String moduleId (const char* name) const;                  // "eq1.mode"
    juce::RangedAudioParameter* bandParam (int set, int b, const char* name) const;

    // The channel sets an edit made on `set` applies to (one, or both when linked).
    std::vector<int> editSets (int set) const;

    // Display gain of a band (gain scaled by Amount and clamped like the DSP does).
    float scaledGain (const ps::EqBandParams&) const noexcept;
    float amountScale() const noexcept { return juce::jmax (0.0f, params.amountPercent) / 100.0f; }

    PureSheenProcessor& processor;
    const ps::ModuleType type;

private:
    juce::String prefix;
    std::vector<int> indices;                         // paramSpecs() indices of this module
    std::vector<std::atomic<float>*> raw;             // their raw values
    std::vector<float> values;                        // full-size ParamValues array (only this module's entries used)
    std::atomic<float>* chModeRaw = nullptr;
    std::atomic<float>* linkRaw = nullptr;
    ps::EqParams params;
    ps::ChannelMode mode = ps::ChannelMode::Stereo;
    bool linked = true;
    bool first = true;
};
} // namespace ps::ui
