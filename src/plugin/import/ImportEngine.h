#pragma once

#include "plugin/import/ImportCommon.h"
#include "dsp/MatchAnalysis.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Preset importer engine (pure C++17, no JUCE): recognises the file structurally, decodes the chain, runs the module
// tables and produces an ImportPlan (our full parameter vector + chain + report). The JUCE side (XmlPresetImport.h)
// turns the file into an XmlNode tree with juce::XmlDocument and applies the plan to the processor.
namespace ps::imp
{
// Keys of the format's structural sections (FNV-1a of the foreign strings; see ImportCommon.h).
inline constexpr Key kGlobalSectionKey = 0x681c086b8f7afcaeull;   // the global section (tag and ElementID)
inline constexpr Key kMetersSectionKey = 0xbfad8af521e0fb63ull;   // the meter settings section (never holds values)
inline constexpr Key kPluginSectionKey = 0x3409ff086bb765daull;   // host-wrapper section of older files (empty)
inline constexpr Key kChainBlobKey     = 0xbe619146aa3cbf6cull;   // ElementID of the chain blob in the global section

// The section key of a module we do not have, from our own words ("tape emulation"); 0 if none (tests).
Key skippedModuleKey (std::string_view words) noexcept;

// A neutral element tree (what juce::XmlDocument produced, or what a test built by hand).
struct XmlNode
{
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlNode> children;

    const std::string* attribute (std::string_view name) const noexcept;
};

// Limits for untrusted input.
inline constexpr size_t kMaxFileBytes = 16u * 1024u * 1024u;   // the largest foreign preset is ~0.4 MB
inline constexpr int kMaxDepth = 16;                           // the format nests 3 deep
inline constexpr int kMaxElements = 200000;
inline constexpr size_t kMaxNameLength = 256;

// Cheap structural check of the raw text before any XML parser sees it: size, nesting depth, element count, no
// DOCTYPE / ENTITY declarations (no entity expansion), no NUL bytes. Returns an error in plain words, or empty.
std::string prescanXmlText (std::string_view text);

// Bounds recursive JSON parsing and rejects numeric tokens that would overflow the parser's integer accumulator.
bool prescanJsonText (std::string_view text);

// Numbers as the format writes them: "5499.99951172", "-12", "1,79999995" (decimal comma from the writer's locale).
// Locale independent; NaN / inf / garbage -> nullopt.
std::optional<double> parseNumber (std::string_view);

// Base64 (standard alphabet, '=' padding, whitespace ignored). nullopt on a bad character.
std::optional<std::vector<unsigned char>> decodeBase64 (std::string_view);

// The chain blob: repeated [u8 flag][u32 little-endian length][length bytes of name].
struct ChainEntry
{
    std::string name;
    int flag = 0;            // meaning unknown (0 in every file seen); NOT module power: it stays 0 when the module is
                             // switched off, which the section's own power switch stores (kSectionBypassKey)
};
struct ChainDecode
{
    std::vector<ChainEntry> entries;
    bool complete = true;    // false: the blob ended inside a record or held an implausible length
};
ChainDecode decodeChain (const std::vector<unsigned char>& bytes);

//==============================================================================
struct ImportOptions
{
    const Aliases* aliases = nullptr;     // tests: our own names for sections / ParamIDs
};

struct ImportPlan
{
    bool ok = false;
    std::string error;                    // plain words, no foreign names ("This file is not a preset ...")
    bool holdsNoModules = false;          // not ok because the file is well formed but its chain holds no module
    int scope = -1;                       // -1 = a whole chain; else the (int) ModuleType of a single-module file
    std::vector<ModuleType> order;        // global files: our chain in the foreign order
    std::vector<float> values;            // paramSpecs() order: our defaults + every imported / foreign-default value
    Report report;

    // The Match EQ reference spectrum stored in the file (both parts get it), or null. The engine only attaches the
    // raw record (report.attachments); the JUCE side decodes it (XmlPresetImport.cpp: zlib + JSON) and fills this.
    std::shared_ptr<const MatchSpectrum> matchReference;
};

ImportPlan buildImportPlan (const XmlNode& root, const ImportOptions& = {});

// Where a single-module import inserts its module when the chain does not hold it yet: before the first module that
// comes later in our usual mastering order (EQ, Dynamic EQ, Match EQ, Tone Balance, Definition, Smoother, Low Contrast,
// Bass Shaper, Punch, Dynamics, Classic Compressor, Passive EQ, Imager, Exciter, Tape, EQ 2, Classic Limiter,
// Maximizer). Returns the new order (unchanged if present).
std::vector<ModuleType> insertIntoChain (const std::vector<ModuleType>& order, ModuleType);

// Every module spec the engine knows (the four families), and the spec for a key (tag or ElementID), or nullptr.
std::vector<const ModuleSpec*> allModuleSpecs();
const ModuleSpec* findModuleSpec (Key tagOrElementKey);

// A short line for the UI: "Imported 9 modules, 214 settings. Skipped: de-limiter."
std::string summariseReport (const ImportPlan&);
} // namespace ps::imp
