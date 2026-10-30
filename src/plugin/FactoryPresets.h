#pragma once

#include "plugin/Presets.h"

#include <string>
#include <utility>
#include <vector>

// Built-in (factory) presets (owner: plugin). Each is a list of parameter overrides on top of the defaults of its
// scope; global presets also carry the chain order. Equalizer presets are shared by Equalizer 1 and 2 (written with
// the placeholder prefix "eq." which toPresetData replaces by "eq1." / "eq2.").
namespace psplug
{
struct FactoryPreset
{
    juce::String name, category;
    int scope = -1;                                    // -1 = global; Equalizer1 stands for "either Equalizer"
    std::vector<ps::ModuleType> order;                 // global presets
    std::vector<std::pair<std::string, float>> values; // id -> plain value
};

const std::vector<FactoryPreset>& factoryPresets();

// The factory presets offered for a scope (-1 = global, or a ModuleType), in display order.
std::vector<const FactoryPreset*> factoryPresetsFor (int scope);

// Defaults of the scope + the preset's overrides (EQ placeholder prefix resolved for `scope`).
PresetData toPresetData (const FactoryPreset&, int scope);
} // namespace psplug
