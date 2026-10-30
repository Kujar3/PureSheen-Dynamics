#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include "dsp/Params.h"

#include <vector>

// Internal helpers of the processor (owner: plugin): the full plug-in state used by the undo history, the A-D
// snapshots and the presets, value formatting for history descriptions, and (de)serialisation of parameter values.
// Not a contract: only src/plugin and tools/dev use it.
namespace psplug
{
//==============================================================================
// Everything an undo step / snapshot / global preset restores: every parameter (plain values, paramSpecs() order) and
// the chain order.
struct FullState
{
    std::vector<float> values;
    std::vector<ps::ModuleType> order;
};

// Monitoring parameters (global Bypass, Gain Match on / modern): never recorded in the history, never restored by
// undo / snapshots / presets (they are listening aids, reference-suite parity).
bool isMonitoringParam (int index) noexcept;

// Parameters a preset of this scope stores (-1 = global: every parameter except the monitoring ones; module: that
// module's parameters except its power switch "<prefix>.on").
bool isInPresetScope (int index, int moduleOrMinus1) noexcept;

// a == b for the purpose of history / modified flags: parameters within a tiny tolerance of their range (plain values
// go through the host's 0..1 mapping and back when applied), monitoring parameters ignored; order compared exactly
// when compareOrder.
bool sameValue (int index, float a, float b) noexcept;
bool sameState (const FullState& a, const FullState& b, bool compareOrder = true) noexcept;
std::vector<int> changedParams (const FullState& from, const FullState& to);   // indices, monitoring ones excluded

//==============================================================================
std::vector<float> defaultValues();
std::vector<ps::ModuleType> defaultChain();
// Clamps into range, snaps to the interval / choice / bool grid; non-finite -> default.
float sanitiseValue (int index, float plainValue) noexcept;

// "+2.5 dB", "1.25 kHz", "Proportional Q", "On"
juce::String formatParamValue (int index, float plainValue);
// "EQ 1 Band 3 Gain (A): +2.5 dB"
juce::String describeParam (int index, float plainValue);

//==============================================================================
juce::String moduleIdPrefix (ps::ModuleType);
int moduleFromIdPrefix (const juce::String&) noexcept;    // -1 when unknown
juce::String moduleName (ps::ModuleType);                 // "Equalizer 1"

// <Chain><Module type="eq1"/>...</Chain>
juce::ValueTree chainToTree (const std::vector<ps::ModuleType>&);
// Unknown types and duplicates dropped.
std::vector<ps::ModuleType> chainFromTree (const juce::ValueTree&);

// <type><P id="eq1.a.b3.gain" v="2.5"/>...</type> for the parameters of the scope (see isInPresetScope; pass
// includeMonitoring to store every parameter).
juce::ValueTree valuesToTree (const juce::Identifier& type, const std::vector<float>& values, int moduleOrMinus1,
                              bool includeMonitoring = false);
// Reads <P id v/> children (also accepts JUCE's APVTS <PARAM id value/>) into values for the parameters of the scope:
// known ids are sanitised, missing ones (older state, foreign preset) get their default when missingToDefault, unknown
// ids are ignored. Returns the number of values read.
int valuesFromTree (const juce::ValueTree&, std::vector<float>& values, int moduleOrMinus1, bool missingToDefault,
                    bool includeMonitoring = false);

// Snapshot / full state <-> tree: <Snapshot><Chain/><Params/></Snapshot>
juce::ValueTree fullStateToTree (const juce::Identifier& type, const FullState&);
bool fullStateFromTree (const juce::ValueTree&, FullState&);
} // namespace psplug
