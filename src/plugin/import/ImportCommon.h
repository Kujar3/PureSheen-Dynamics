#pragma once

#include "dsp/Params.h"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Preset importer: shared types of the mapping tables (pure C++17, no JUCE; compiled into the plug-in, the dev tool
// and CoreTests). Owner: importer framework. The mapping tables live in ImportMapEq.cpp, ImportMapCore.cpp and
// ImportMapPhase2.cpp; the engine (structure, chain, plan) in ImportEngine.h/.cpp; the JUCE side (file reading,
// juce::XmlDocument, applying to the processor, file chooser) in XmlPresetImport.h/.cpp.
//
// The imported format (a foreign mastering suite's .xml presets): a root element with PresetVer / PluginVer
// attributes, one child element per module section (attribute Enabled), and inside each section
//   <Param ElementID ParamID Value/>  and  <ExtraBytes ElementID Data/> (base64).
// Only values that differ from the WRITER's own default are stored (sparse files), so every parameter a table knows
// takes the foreign default of that writer version (PluginVer) when it is absent. Our defaults are never used for a
// parameter a table maps; parameters no table maps keep our default.
//
// Naming rule: foreign section tags, ElementIDs and ParamIDs are NEVER written in this repository. They are matched by
// a 64-bit FNV-1a key of the exact string (importKey below, used at dev time: `PureSheenDev importkey "<ParamID>"`),
// and the source holds the numeric constant with a comment in our own words.
//
// ParamID keys are TEMPLATE keys: an optional leading "Aux: " (= channel set B) is removed and the first run of decimal
// digits is replaced by '#'; the digits are kept as the "number" of the stored value. So the stored names for band 1..8
// of one control all share one key, and the set A / set B variants share it too.
namespace ps::imp
{
using Key = std::uint64_t;

// FNV-1a, 64 bit (offset basis 0xcbf29ce484222325, prime 0x100000001b3) over the bytes of the exact string.
constexpr Key importKey (std::string_view s) noexcept
{
    Key h = 0xcbf29ce484222325ull;
    for (const char c : s)
    {
        h ^= (Key) (unsigned char) c;
        h *= 0x100000001b3ull;
    }
    return h;
}

inline constexpr double kNoValue = std::numeric_limits<double>::quiet_NaN();

// A stored ParamID split into its template key, number and channel set.
struct ParamName
{
    Key key = 0;
    int number = -1;          // the first decimal number in the name (-1 = none, -2 = unreadable / too large)
    int set = 0;              // 0 = set A, 1 = set B ("Aux: " prefix)
    std::string templ;        // the template string the key was computed from ("Band # Width" style)
};

// Aliases: template string -> key, consulted before hashing. Lets tests (and future formats) use our own generic names
// for sections and ParamIDs (see tests/XmlPresetImportTests.cpp). Empty in the product.
using Aliases = std::map<std::string, Key, std::less<>>;

ParamName splitParamName (std::string_view paramId, const Aliases* aliases = nullptr);
Key sectionKey (std::string_view tagOrElementId, const Aliases* aliases = nullptr);

//==============================================================================
// Mapping tables
//==============================================================================

// How sure we are of a foreign default. Known: proven from the files (sparse storage, never stored), documented, or
// confirmed in the real product (Reset screenshots and one-change test saves of writer 120000);
// Likely: consistent evidence of one kind; Guess: no evidence, needs a Reset check in the foreign app
// (out/importer/defaults-to-confirm.md lists every Likely / Guess default; `PureSheenDev import --defaults` prints them).
enum class Conf : std::uint8_t { Known, Likely, Guess };

class ModuleImport;

// The foreign default of a value. fn (when set) computes it from the writer version / number / set (e.g. EQ bands on
// by default only for odd bands below version 110000). value NaN and no fn = no default known: the parameter keeps
// OUR default when the file does not store it.
using DefaultFn = double (*) (const ModuleImport&, int number, int set);
struct Def
{
    double value = kNoValue;
    DefaultFn fn = nullptr;
    Conf conf = Conf::Known;
};
constexpr Def known (double v) noexcept { return { v, nullptr, Conf::Known }; }
constexpr Def likely (double v) noexcept { return { v, nullptr, Conf::Likely }; }
constexpr Def guess (double v) noexcept { return { v, nullptr, Conf::Guess }; }
constexpr Def byVersion (DefaultFn fn, Conf c) noexcept { return { kNoValue, fn, c }; }
constexpr Def noDefault() noexcept { return { kNoValue, nullptr, Conf::Guess }; }

// Foreign value -> our plain value (choice index, bool 0/1, Hz, dB ...). NaN = cannot be represented: nothing is
// written and the stored value counts as "not imported". nullptr in a rule = 1:1.
using Transform = double (*) (double foreignValue, ModuleImport&, int number, int set);

enum class Act : std::uint8_t
{
    Map,           // write idA / idB through the transform (absent: the default)
    ChannelMode,   // the module's channel-mode index (the foreign list = our supportedChannelModes order); written
                   //   through ps::channelModeParamValues (handles the Exciter's T/S switch)
    Hook,          // known control handled by the module's prepare / finish hook (it reads it with ModuleImport::get)
    Ignore,        // known view / monitoring state: dropped silently (counted as "ignored")
    Unsupported    // known control we do not have: counted as "not imported"
};

enum class Band : std::uint8_t
{
    Same,          // our index = the stored number - first + 1
    Layout         // multiband slot: our band = ModuleImport::layout().bandOfSlot[number - first] (0 = dropped)
};

// One row of a module table. Numbering: rules without a number in their template use count = 0; numbered ones accept
// the stored numbers first .. first + count - 1 (first = 0 or 1, the foreign numbering of that control).
// Target ids: "~" = the module's id prefix ("eq1"), "#" = our 1-based band number; idB nullptr = we have no set B
// counterpart (a stored set B value then counts as "not imported"; no default is written for set B).
struct Rule
{
    Key key;                    // template key of the ParamID
    Act act = Act::Map;
    int first = 0, count = 0;
    Band band = Band::Same;
    const char* idA = nullptr;
    const char* idB = nullptr;
    Transform xf = nullptr;
    Def def {};
    const char* what = "";      // our words, for reports ("imager band width")
};

using Hook = void (*) (ModuleImport&);

// The module power switch every foreign module section may store (one key for all modules, no number, set A only):
// stored >= 0.5 = the module is switched off. Handled by ModuleImport::run for every module (see setWritesModulePower).
inline constexpr Key kSectionBypassKey = 0xe40567630146944dull;

// One foreign module that maps to one of ours.
struct ModuleSpec
{
    ModuleType type = ModuleType::Count;   // Count = the global section (I/O, chain)
    Key tagKey = 0;                        // the section element's tag
    Key elementKey = 0;                    // its ElementID = the name used in the chain blob
    const std::vector<Rule>* rules = nullptr;
    Hook prepare = nullptr;                // before the table (e.g. the multiband layout)
    Hook finish = nullptr;                 // after the table (cross-parameter rules)
};

// The three family files (one function each, called by the engine; never at static-init time).
const std::vector<ModuleSpec>& eqFamilySpecs();        // ImportMapEq.cpp:     EQ 1, EQ 2, Dynamic EQ, Match EQ
const std::vector<ModuleSpec>& coreFamilySpecs();      // ImportMapCore.cpp:   Dynamics, Imager, Exciter, Maximizer
const ModuleSpec& globalSpec();                        // ImportMapCore.cpp:   the global section (type Count)
const std::vector<ModuleSpec>& phase2FamilySpecs();    // ImportMapPhase2.cpp: Tone Balance .. Bass Shaper

//==============================================================================
// Result bookkeeping
//==============================================================================
struct Report
{
    bool globalFile = false;
    int pluginVersion = 0;
    std::vector<ModuleType> imported;          // our modules, in order
    std::vector<std::string> skipped;          // our words ("tape emulation"), one entry per skipped module
    int mapped = 0;                            // stored foreign values that were imported
    int defaulted = 0;                         // our parameters set from a foreign default (absent in the file)
    int unmapped = 0;                          // stored foreign values not imported (unknown / unsupported / bad)
    int ignored = 0;                           // stored view / monitoring state dropped on purpose
    int clamped = 0;                           // imported values that were outside our range
    std::vector<std::string> notes;            // approximations, in our words (deduplicated)

    // Binary data a hook hands on to the JUCE side, which decodes it (a stored Match EQ reference spectrum, see
    // ModuleImport::attach). `bytes` = the base64-decoded ExtraBytes, `slot` = the foreign record number (1-based).
    struct Attachment
    {
        ModuleType type = ModuleType::Count;
        int slot = 0;
        std::vector<unsigned char> bytes;
        int mappedControls = 1;  // selected flags represented by these shared bytes (for decode-failure accounting)
    };
    std::vector<Attachment> attachments;

    struct Unmapped
    {
        std::string section, param, value;     // raw strings from the file (dev tool output only, never the UI)
        std::string reason;                    // "unknown", "unsupported", "set B", "number", "value"
    };
    std::vector<Unmapped> unmappedList;

    void note (const std::string& text);       // adds once
};

// One stored value of a section.
struct StoredParam
{
    ParamName name;
    std::string rawName, rawValue;
    double value = kNoValue;                   // parsed (NaN = unreadable)
    enum class Use : std::uint8_t { None, Mapped, Ignored, Rejected } use = Use::None;
};

// Bands of a multiband section (see ModuleImport::readLayout).
struct BandLayout
{
    int numBands = 1;
    int slotOfBand[kMaxBands] { 0, 1, 2, 3 };     // our band i (0-based) -> foreign slot (0-based)
    int bandOfSlot[kMaxBands] { 1, 2, 3, 4 };     // foreign slot -> our band (1-based), 0 = not used
    int cutoffOfXover[kMaxCrossovers] { 1, 2, 3 };// our crossover i (0-based) -> foreign cutoff number (1-based)
};

//==============================================================================
// What a table row, transform or hook works with: one section of one file, mapped into one module.
class ModuleImport
{
public:
    ModuleImport (const ModuleSpec&, std::vector<StoredParam>& stored, int pluginVersion, std::vector<float>& values,
                  Report&, std::string sectionName);

    ModuleType type() const noexcept { return spec.type; }
    const std::string& prefix() const noexcept { return idPrefix; }   // "img" (empty for the global section)
    int pluginVersion() const noexcept { return version; }            // PluginVer of the file (0 when absent)

    // ---- reading foreign values
    // true when the file stores (key, number, set).
    bool has (Key, int number = -1, int set = 0) const;
    // The stored value (marked as imported), else the rule's foreign default for (number, set), else NaN.
    double get (Key, int number = -1, int set = 0);
    // Like get, but `fallback` when neither a stored value nor a default exists.
    double getOr (Key, int number, int set, double fallback);
    // The foreign default of a rule (NaN when none).
    double defaultOf (Key, int number = -1, int set = 0) const;
    // Like get, but without marking the stored value as imported (e.g. to compare set B with set A first).
    double peek (Key, int number = -1, int set = 0) const;
    // Marks a stored value as not imported (a hook decided it cannot be used) / as ignored view state.
    void reject (Key, int number = -1, int set = 0, const char* reason = "unsupported");
    void ignore (Key, int number = -1, int set = 0);

    // ---- writing our values (plain values; clamped / snapped into range, clamps counted; NaN ignored)
    // id: a full id or a pattern with "~" (prefix) and "#" (band, `band`).
    bool set (std::string_view id, double plainValue, int band = 0);
    // set() for a value a hook computed from a foreign DEFAULT (the file did not store it): also counted in
    // Report::defaulted, as the table rows do.
    bool setFromDefault (std::string_view id, double plainValue, int band = 0);
    // The module's channel mode from a foreign index into its mode list (= supportedChannelModes order).
    void setChannelMode (int foreignIndex);
    // Adds an approximation note (our words) to the report.
    void note (const std::string& text);

    // ---- binary children of the section (<ExtraBytes ElementID Data/>)
    // The base64 text of the child whose ElementID has this key, or nullptr.
    const std::string* extraBytes (Key elementKey) const noexcept;
    // Hands decoded bytes on to the JUCE side (Report::attachments, for this module).
    void attach (int slot, std::vector<unsigned char> bytes, int mappedControls = 1);

    // ---- multiband modules
    // Reads the band layout: from version `activeFlagsFrom` on, slot k exists iff the per-slot "active" control
    // (activeKey, numbered from 0) is non-zero (absent: its rule default); below it, `numBandsKey` (default from its
    // rule) contiguous slots. Our bands = the active slots in order (gaps closed); our crossover i = the foreign cutoff
    // at the lower edge of our band i + 1. Writes "~.bands" and returns the layout (also kept for Band::Layout rules).
    const BandLayout& readLayout (Key activeKey, Key numBandsKey, int activeFlagsFrom = 9000);
    // For a module without a band-count control: the per-slot active flags in every version.
    const BandLayout& readLayout (Key activeKey) { return readLayout (activeKey, 0, 0); }
    // Writes "~.xover<i>" for the crossovers the layout uses from the foreign cutoffs (cutoffKey, numbered from 1;
    // absent: the rule default, else ours); stored cutoffs of removed bands are dropped as stale.
    void writeCrossovers (Key cutoffKey);
    const BandLayout& layout() const noexcept { return bands; }
    void setLayout (const BandLayout& l) noexcept { bands = l; }

    // ---- engine side
    void run();          // prepare hook, table, finish hook, module power, leftover accounting
    // Module power from the section's own power switch (kSectionBypassKey). true (whole-chain files): run() writes
    // "~.on" (absent = on) and counts a stored switch as imported. false (the default; single-module files): module
    // presets never carry power here, so a stored switch is counted as ignored.
    void setWritesModulePower (bool writes) noexcept { writesPower = writes; }
    const Rule* findRule (Key) const noexcept;
    // The section's <ExtraBytes> children as (ElementID key, base64 Data); the strings must outlive run().
    void setExtraBytes (std::vector<std::pair<Key, const std::string*>> blobs) { extra = std::move (blobs); }

private:
    const ModuleSpec& spec;
    std::vector<StoredParam>& stored;
    int version;
    std::vector<float>& values;
    Report& report;
    std::string section, idPrefix;
    BandLayout bands;
    std::vector<std::pair<Key, const std::string*>> extra;
    bool writesPower = false;

    void applyModulePower();
    StoredParam* find (Key, int number, int set);
    const StoredParam* find (Key, int number, int set) const;
    std::string resolve (std::string_view pattern, int band) const;
    void applyRule (const Rule&);
};

//==============================================================================
// Shared transforms (for Rule::xf)
namespace xf
{
double boolean (double v, ModuleImport&, int, int);         // v >= 0.5 -> 1, else 0
double invertBool (double v, ModuleImport&, int, int);      // "bypass" -> "on"
double roundIndex (double v, ModuleImport&, int, int);      // nearest integer (choice index 1:1)
}

// Our own words for modules we do not have, keyed by the foreign section tag / chain name (ImportEngine.cpp).
const char* skippedModuleDescription (Key tagOrElementKey) noexcept;   // nullptr = not a known module
} // namespace ps::imp
