#pragma once

#include "ui/Widgets.h"

#include <vector>

// Dynamic EQ panel helpers (owner: ui-deq): a cached view of the Dynamic EQ's parameters for the UI, shape traits,
// short names, the shape / sidechain menus and the default shape of a new node. Internal to DynamicEqPanel /
// DynamicEqDisplay (not a team contract). Band colours are the Equalizer's (eqBandColour, EqModel.h).
namespace ps::ui
{
struct DynEqShapeTraits
{
    bool q = true;            // has a width (Baxandall shelves do not)
    bool lowShelf = false;
    bool highShelf = false;
};
DynEqShapeTraits dynEqShapeTraits (ps::DynEqShape) noexcept;

juce::String dynEqShapeShortName (ps::DynEqShape);     // "Bell", "Prop Q", "Band Shelf", "Baxandall Low", ...
juce::String dynEqShapeFullName (ps::DynEqShape);      // the parameter's choice label
juce::String dynEqSidechainName (ps::DynEqSidechain);  // "Own band", "Band 3", "Other channel"

// Grouped shape menu (item id = 1 + shape index), current shape ticked.
juce::PopupMenu dynEqShapeMenu (ps::DynEqShape current);

// Shape of a node created at hz: < 100 Hz Baxandall low shelf, 100 Hz .. 8 kHz Proportional Q, > 8 kHz Baxandall high
// shelf; Proportional Q when that Baxandall shelf is already used by another enabled band of the set.
ps::DynEqShape dynEqDefaultShapeForNewNode (const ps::DynamicEqBandParams* bandsOfSet, double hz) noexcept;

//==============================================================================
// Reads every Dynamic EQ parameter through cached raw parameter pointers (cheap enough for 30 Hz) into a
// ps::DynamicEqParams. Message thread only.
class DynamicEqModel
{
public:
    explicit DynamicEqModel (PureSheenProcessor&);

    // Re-reads the module's parameters; true when anything changed since the last call.
    bool update();

    const ps::DynamicEqParams& get() const noexcept { return params; }
    const ps::DynamicEqBandParams& band (int set, int b) const noexcept
    {
        return params.bands[set & 1][juce::jlimit (0, ps::kDynEqBands - 1, b)];
    }
    bool isDigital() const noexcept { return params.filterMode == ps::EqFilterMode::Digital; }

    ps::ChannelMode channelMode() const noexcept { return mode; }
    bool isSplit() const noexcept { return isDualChannelMode (mode); }
    bool isLinked() const noexcept { return isSplit() && linked; }   // edits go to both channel sets
    bool hasOtherChannel() const noexcept                              // "Other channel" sidechain means something
    {
        return mode == ps::ChannelMode::MidSide || mode == ps::ChannelMode::LeftRight;
    }
    juce::String setName (int set) const { return channelSetName (mode, set); }

    juce::String bandId (int set, int b, const char* name) const;     // "deq.a.b3.freq"
    juce::String moduleId (const char* name) const;                   // "deq.mode"
    juce::RangedAudioParameter* bandParam (int set, int b, const char* name) const;

    // The channel sets an edit made on `set` applies to (one, or both when linked).
    std::vector<int> editSets (int set) const;

    // "Dyn EQ Band 3" / "Dyn EQ Band 3 (Side)" for undo descriptions.
    juce::String bandLabel (int set, int b) const;

    PureSheenProcessor& processor;

private:
    std::vector<int> indices;
    std::vector<std::atomic<float>*> raw;
    std::vector<float> values;
    std::atomic<float>* chModeRaw = nullptr;
    std::atomic<float>* linkRaw = nullptr;
    ps::DynamicEqParams params;
    ps::ChannelMode mode = ps::ChannelMode::Stereo;
    bool linked = true;
    bool first = true;
};

// Plain default value of a parameter id (0 when unknown).
float dynEqDefaultOf (const juce::String& paramId);
} // namespace ps::ui
