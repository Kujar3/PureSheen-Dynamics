#pragma once

#include "plugin/import/ImportEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

class PureSheenProcessor;

// Preset importer, JUCE side (owner: importer framework): reads a foreign .xml preset file, checks and parses it with
// juce::XmlDocument into the engine's neutral tree, builds the plan (ImportEngine.h) and applies it to the processor
// as ONE undo step (PureSheenProcessor::applyImportedPreset). UI label everywhere: "Import preset (.xml)...".
namespace psplug
{
// Text -> tree. Rejects oversized / deeply nested files, DOCTYPE / ENTITY declarations and invalid XML (plain-word
// error). Never throws.
bool parseImportXml (const juce::String& text, ps::imp::XmlNode& root, juce::String& error);

ps::imp::ImportPlan planXmlPresetText (const juce::String& text, const ps::imp::ImportOptions& = {});
ps::imp::ImportPlan planXmlPresetFile (const juce::File&, const ps::imp::ImportOptions& = {});

struct XmlImportResult
{
    bool ok = false;
    juce::String name;        // the preset name (file name without extension)
    juce::String message;     // one line for the UI: "Imported 9 modules, 214 settings. Skipped: tape emulation."
    ps::imp::ImportPlan plan;
};

// Imports a file into the processor. scope -1 (header preset menu): a whole-chain file replaces the chain, a
// single-module file goes to its module (inserted into the chain when missing). scope = a module (that module's
// preset menu): only a single-module file for that module is accepted (Equalizer 1 / 2 interchangeable).
XmlImportResult importXmlPreset (PureSheenProcessor&, const juce::File&, int scope = -1,
                                 const ps::imp::ImportOptions& = {});
XmlImportResult applyImportPlan (PureSheenProcessor&, ps::imp::ImportPlan, const juce::String& name, int scope = -1);

// "Import preset (.xml)..." -> async file chooser -> import -> done (message thread). Owned by the component that
// shows the menu; destroying it cancels a pending callback. Message-thread only. A second launch while a dialog is
// open is ignored. Folder memory is shared by the live importers and released when the last importer is destroyed.
class XmlImportChooser
{
public:
    XmlImportChooser();
    ~XmlImportChooser();
    XmlImportChooser (const XmlImportChooser&) = delete;
    XmlImportChooser& operator= (const XmlImportChooser&) = delete;

    void launch (PureSheenProcessor&, int scope, std::function<void (const XmlImportResult&)> done);

private:
    struct State;
    std::shared_ptr<State> state;

    // Reserve one dialog and make its one-shot completion independently of the platform dialog implementation.
    std::function<void (const juce::File&)> begin (PureSheenProcessor&, int,
                                                std::function<void (const XmlImportResult&)>,
                                                const ps::imp::ImportOptions& = {});
    juce::File startingFolder() const;
    void cancelPending();
    friend struct XmlImportChooserTestAccess;
};
} // namespace psplug
