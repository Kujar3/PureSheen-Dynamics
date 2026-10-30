#include "plugin/Presets.h"

#include <algorithm>

namespace psplug
{
namespace
{
const juce::Identifier kPresetTag ("PureSheenPreset"), kFormat ("format"), kScope ("scope"), kName ("name"),
    kCategory ("category"), kChain ("Chain"), kParams ("Params");

juce::String scopeName (int scope)
{
    return scope < 0 ? juce::String ("global") : moduleIdPrefix ((ps::ModuleType) scope);
}
} // namespace

juce::File defaultPresetRoot()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("PureSheen Dynamics")
        .getChildFile ("Presets");
}

juce::File presetFolder (const juce::File& root, int scope)
{
    return root.getChildFile (scope < 0 ? juce::String ("Global") : moduleIdPrefix ((ps::ModuleType) scope));
}

bool writePresetFile (const juce::File& file, const PresetData& data)
{
    juce::ValueTree tree (kPresetTag);
    tree.setProperty (kFormat, 1, nullptr);
    tree.setProperty (kScope, scopeName (data.scope), nullptr);
    tree.setProperty (kName, data.name, nullptr);
    if (data.category.isNotEmpty())
        tree.setProperty (kCategory, data.category, nullptr);

    if (data.scope < 0)
        tree.appendChild (chainToTree (data.order), nullptr);
    tree.appendChild (valuesToTree (kParams, data.values, data.scope), nullptr);

    const auto xml = tree.createXml();
    if (xml == nullptr)
        return false;

    if (! file.getParentDirectory().createDirectory())
        return false;

    // Write to a temporary sibling first so a failed write never leaves a truncated preset behind.
    const juce::TemporaryFile temp (file);
    if (! xml->writeTo (temp.getFile()))
        return false;
    return temp.overwriteTargetFileWithTemporary();
}

bool readPresetFile (const juce::File& file, PresetData& data)
{
    if (! file.existsAsFile() || file.getSize() > 16 * 1024 * 1024)
        return false;

    const auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr)
        return false;

    const auto tree = juce::ValueTree::fromXml (*xml);
    if (! tree.hasType (kPresetTag))
        return false;

    const auto scopeText = tree.getProperty (kScope, "global").toString();
    int scope = -1;
    if (scopeText != "global")
    {
        scope = moduleFromIdPrefix (scopeText);
        if (scope < 0)
            return false;
    }

    const auto params = tree.getChildWithName (kParams);
    if (! params.isValid())
        return false;

    PresetData d;
    d.scope = scope;
    d.name = tree.getProperty (kName, file.getFileNameWithoutExtension()).toString();
    if (d.name.trim().isEmpty())
        d.name = file.getFileNameWithoutExtension();
    d.category = tree.getProperty (kCategory).toString();
    d.values = defaultValues();
    valuesFromTree (params, d.values, scope, true);

    if (scope < 0)
    {
        const auto chain = tree.getChildWithName (kChain);
        d.hasChain = chain.isValid();
        d.order = d.hasChain ? chainFromTree (chain) : defaultChain();
    }

    data = std::move (d);
    return true;
}

std::vector<UserPresetFile> scanUserPresets (const juce::File& root, int scope)
{
    std::vector<UserPresetFile> result;
    const auto folder = presetFolder (root, scope);
    if (! folder.isDirectory())
        return result;

    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, juce::String ("*") + kPresetExtension,
                                                            juce::File::findFiles))
    {
        const auto f = entry.getFile();
        UserPresetFile u;
        u.file = f;
        u.name = f.getFileNameWithoutExtension();
        const auto parent = f.getParentDirectory();
        u.category = parent == folder ? juce::String (kUserCategory)
                                      : parent.getRelativePathFrom (folder).replaceCharacter ('\\', '/');
        result.push_back (u);
    }

    std::sort (result.begin(), result.end(), [] (const UserPresetFile& a, const UserPresetFile& b)
    {
        const int c = a.category.compareNatural (b.category);
        return c != 0 ? c < 0 : a.name.compareNatural (b.name) < 0;
    });
    return result;
}
} // namespace psplug
