#pragma once

#include "plugin/PluginState.h"

#include <juce_core/juce_core.h>

#include <vector>

// Preset files (owner: plugin). XML, extension .pspreset:
//
//   <PureSheenPreset format="1" scope="global" name="Warm Glue" category="Gentle">   (scope = "global" or a module
//     <Chain><Module type="eq1"/>...</Chain>                                           idPrefix, e.g. "dyn")
//     <Params><P id="eq1.on" v="1"/>...</Params>
//   </PureSheenPreset>
//
// A global preset stores the chain and every parameter except the monitoring ones (Bypass, Gain Match); a module
// preset stores that module's parameters except its power switch. Loading is tolerant: unknown ids are ignored,
// missing ones get their defaults, values are clamped / snapped into range.
//
// Folders: <root>/Global/[<category>/]<name>.pspreset and <root>/<module idPrefix>/[<category>/]<name>.pspreset, root
// = Documents/PureSheen Dynamics/Presets (the processor can point it elsewhere, e.g. for tests).
namespace psplug
{
inline constexpr const char* kPresetExtension = ".pspreset";
inline constexpr const char* kUserCategory = "User";   // presets directly in the scope folder

struct PresetData
{
    int scope = -1;                      // -1 = global, else (int) ModuleType
    juce::String name, category;
    bool hasChain = false;               // global presets
    std::vector<ps::ModuleType> order;
    std::vector<float> values;           // full paramSpecs() size; only the scope's parameters are meaningful
};

juce::File defaultPresetRoot();
juce::File presetFolder (const juce::File& root, int scope);

bool writePresetFile (const juce::File&, const PresetData&);
bool readPresetFile (const juce::File&, PresetData&);

struct UserPresetFile
{
    juce::String name, category;
    juce::File file;
};
// Every .pspreset below the scope folder, sorted by category then name. Category = the subfolder path relative to the
// scope folder ("User" for files directly in it).
std::vector<UserPresetFile> scanUserPresets (const juce::File& root, int scope);
} // namespace psplug
