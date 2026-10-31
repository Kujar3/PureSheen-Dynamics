#include "plugin/import/ImportEngine.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

// Preset importer engine. Design and conventions: out/importer/spec.md (private notes) and the header comments.
//
// Structure of an importable file (recognised structurally, never by the root tag):
//   - the root carries PresetVer and PluginVer attributes;
//   - each child is a module section with an Enabled attribute, holding Param (ElementID, ParamID, Value) and
//     ExtraBytes (ElementID, Data) children;
//   - one "global" section holds the chain blob (an ExtraBytes child) and a few global switches.
// Enabled is a load mask, not module power. A file with exactly one enabled module section is a single-module preset
// (its chain blob may be stale and is ignored); otherwise it is a whole-chain preset whose modules are exactly the
// chain entries, in that order (sections outside the chain hold stale settings and are ignored). Module power is the
// section's own power switch (kSectionBypassKey, confirmed with a test save); the flag byte of the chain entries is
// parsed but is not power (it stays 0 for a switched-off module).
namespace ps::imp
{
namespace
{
    constexpr Key kGlobalSection = kGlobalSectionKey;
    constexpr Key kMetersSection = kMetersSectionKey;
    constexpr Key kPluginSection = kPluginSectionKey;
    constexpr Key kChainBlob     = kChainBlobKey;

    // Modules we do not have, by section tag and by chain name, in our own words.
    struct Skipped
    {
        Key tag, element;
        const char* words;
    };
    constexpr Skipped kSkipped[] = {
        { 0x26c11f0faab3834cull, 0x72e0609f4e29b1baull, "analog-style compressor" },
        { 0x14ed3e9e040de2d3ull, 0x06506d82d1caaaddull, "analog-style EQ" },
        { 0x22259cc996cef7b3ull, 0x09d5140dccdc1c0dull, "analog-style limiter" },
        { 0x6a5cc98556008c35ull, 0xf4e4871d0736260full, "tape emulation" },
        { 0xe9ef2e15463fa562ull, 0xe9ef2e15463fa562ull, "de-limiter" },
        { 0xdb145da424cb2642ull, 0xa3c20be8958df8f2ull, "stem-separation EQ" },
        { 0x846e6a75db401626ull, 0x91d819139693b5f0ull, "mix rebalancing" },
    };

    bool startsWith (std::string_view s, size_t at, std::string_view prefix) noexcept
    {
        return s.size() >= at + prefix.size() && s.compare (at, prefix.size(), prefix) == 0;
    }

    bool startsWithNoCase (std::string_view s, size_t at, std::string_view prefix) noexcept
    {
        if (s.size() < at + prefix.size())
            return false;
        for (size_t i = 0; i < prefix.size(); ++i)
        {
            char a = s[at + i], b = prefix[i];
            if (a >= 'a' && a <= 'z') a = (char) (a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char) (b - 'a' + 'A');
            if (a != b)
                return false;
        }
        return true;
    }

    struct Section
    {
        const XmlNode* node = nullptr;
        Key tagKey = 0;
        bool enabled = false;
        std::vector<Key> elementKeys;   // ElementIDs of its children
    };

    bool isEnabled (const XmlNode& n)
    {
        if (const auto* e = n.attribute ("Enabled"))
        {
            const auto v = parseNumber (*e);
            return v.has_value() && *v >= 0.5;
        }
        for (const auto& c : n.children)   // tolerant: no attribute, but values -> load it
            if (c.tag == "Param")
                return true;
        return false;
    }

    std::vector<float> ourDefaults()
    {
        const auto& specs = paramSpecs();
        std::vector<float> v (specs.size());
        for (size_t i = 0; i < specs.size(); ++i)
            v[i] = specs[i].defaultValue;
        return v;
    }

    // The stored values of a section (Param children only).
    std::vector<StoredParam> collect (const XmlNode& section, const Aliases* aliases, Report& report,
                                      const std::string& sectionName)
    {
        constexpr size_t kMaxParams = 4096;
        std::vector<StoredParam> out;
        for (const auto& c : section.children)
        {
            if (c.tag != "Param")
                continue;

            const auto* id = c.attribute ("ParamID");
            const auto* value = c.attribute ("Value");
            if (id == nullptr || id->empty() || id->size() > kMaxNameLength || out.size() >= kMaxParams)
            {
                ++report.unmapped;
                report.unmappedList.push_back ({ sectionName, id != nullptr ? id->substr (0, 64) : std::string(),
                                                 value != nullptr ? value->substr (0, 32) : std::string(), "malformed" });
                continue;
            }

            StoredParam p;
            p.name = splitParamName (*id, aliases);
            p.rawName = *id;
            p.rawValue = value != nullptr ? value->substr (0, 64) : std::string();
            if (value != nullptr)
                if (const auto v = parseNumber (*value))
                    p.value = *v;

            const bool duplicate = std::any_of (out.begin(), out.end(), [&p] (const StoredParam& q)
            {
                return q.name.key == p.name.key && q.name.number == p.name.number && q.name.set == p.name.set;
            });
            if (duplicate || p.name.number == -2)
            {
                ++report.unmapped;
                report.unmappedList.push_back ({ sectionName, p.rawName, p.rawValue, duplicate ? "duplicate" : "number" });
                continue;
            }
            out.push_back (std::move (p));
        }
        return out;
    }

    // The binary children of a section: (ElementID key, base64 Data), pointing into the tree.
    std::vector<std::pair<Key, const std::string*>> collectExtraBytes (const XmlNode& section, const Aliases* aliases)
    {
        std::vector<std::pair<Key, const std::string*>> out;
        for (const auto& c : section.children)
        {
            if (c.tag != "ExtraBytes" || out.size() >= 64)
                continue;
            const auto* e = c.attribute ("ElementID");
            const auto* data = c.attribute ("Data");
            if (e == nullptr || data == nullptr || e->size() > kMaxNameLength)
                continue;
            const Key k = sectionKey (*e, aliases);
            if (std::none_of (out.begin(), out.end(), [k] (const auto& p) { return p.first == k; }))
                out.emplace_back (k, data);   // the first one wins, as for Params
        }
        return out;
    }

    // "a" / "an" before our words for a module ("an analog-style limiter").
    std::string withArticle (const std::string& words)
    {
        const char c = words.empty() ? 'x' : words.front();
        const bool vowel = c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
        return (vowel ? "an " : "a ") + words;
    }

    int rankInOurOrder (ModuleType t) noexcept
    {
        constexpr ModuleType order[] = {
            ModuleType::Equalizer1, ModuleType::DynamicEq, ModuleType::MatchEq, ModuleType::ToneBalance,
            ModuleType::Definition, ModuleType::Smoother, ModuleType::LowContrast, ModuleType::BassShaper,
            ModuleType::Punch, ModuleType::Dynamics, ModuleType::Imager, ModuleType::Exciter, ModuleType::Equalizer2,
            ModuleType::Maximizer
        };
        for (int i = 0; i < (int) std::size (order); ++i)
            if (order[i] == t)
                return i;
        return (int) std::size (order) - 1;   // future types: just before the end
    }
} // namespace

//==============================================================================
const std::string* XmlNode::attribute (std::string_view name) const noexcept
{
    for (const auto& [k, v] : attributes)
        if (k == name)
            return &v;
    return nullptr;
}

const char* skippedModuleDescription (Key key) noexcept
{
    for (const auto& s : kSkipped)
        if (s.tag == key || s.element == key)
            return s.words;
    return nullptr;
}

Key skippedModuleKey (std::string_view words) noexcept
{
    for (const auto& s : kSkipped)
        if (words == s.words)
            return s.tag;
    return 0;
}

//==============================================================================
std::string prescanXmlText (std::string_view text)
{
    if (text.size() > kMaxFileBytes)
        return "The file is too large to be a preset.";
    if (text.find ('\0') != std::string_view::npos)
        return "The file is not a text file.";

    int depth = 0, elements = 0;
    std::vector<std::string_view> openTags;
    bool sawRoot = false;
    size_t i = 0;
    while (i < text.size())
    {
        if (text[i] != '<')
        {
            if (depth == 0 && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
            {
                if (i == 0 && startsWith (text, i, "\xef\xbb\xbf"))
                {
                    i += 3;
                    continue;
                }
                return "The file is not valid XML.";
            }
            ++i;
            continue;
        }

        if (startsWith (text, i, "<!--"))
        {
            const auto end = text.find ("-->", i + 4);
            if (end == std::string_view::npos)
                return "The file is not valid XML.";
            i = end + 3;
        }
        else if (startsWith (text, i, "<![CDATA["))
        {
            if (depth == 0)
                return "The file is not valid XML.";
            const auto end = text.find ("]]>", i + 9);
            if (end == std::string_view::npos)
                return "The file is not valid XML.";
            i = end + 3;
        }
        else if (startsWith (text, i, "<?"))
        {
            const auto end = text.find ("?>", i + 2);
            if (end == std::string_view::npos)
                return "The file is not valid XML.";
            i = end + 2;
        }
        else if (startsWith (text, i, "<!"))
        {
            if (startsWithNoCase (text, i, "<!DOCTYPE") || startsWithNoCase (text, i, "<!ENTITY"))
                return "The file uses XML features that presets never use.";
            return "The file is not valid XML.";
        }
        else if (startsWith (text, i, "</"))
        {
            const auto end = text.find ('>', i + 2);
            if (end == std::string_view::npos)
                return "The file is not valid XML.";
            auto name = text.substr (i + 2, end - i - 2);
            while (! name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r' || name.back() == '\n'))
                name.remove_suffix (1);
            if (openTags.empty() || name != openTags.back())
                return "The file is not valid XML.";
            openTags.pop_back();
            --depth;
            i = end + 1;
        }
        else
        {
            // An element start tag: find its end, skipping quoted attribute values.
            size_t j = i + 1;
            char quote = 0;
            int attributes = 0;
            for (; j < text.size(); ++j)
            {
                const char c = text[j];
                // Nested markup is invalid inside a start tag, even in an attribute. Some permissive parsers
                // accept comments here; their quotes must never hide child elements from this depth scan.
                if (c == '<')
                    return "The file is not valid XML.";
                if (quote != 0)
                {
                    if (c == quote)
                        quote = 0;
                }
                else if (c == '"' || c == '\'')
                {
                    quote = c;
                }
                else if (c == '=' && ++attributes > 64)
                {
                    return "The file holds far more attributes than a preset.";
                }
                else if (c == '>')
                {
                    break;
                }
            }
            if (j >= text.size())
                return "The file is not valid XML.";

            if (++elements > kMaxElements)
                return "The file holds far more elements than a preset.";
            if (depth == 0 && sawRoot)
                return "The file is not valid XML.";
            sawRoot = true;
            if (depth + 1 > kMaxDepth)
                return "The file is nested far deeper than a preset.";
            if (text[j - 1] != '/')
            {
                const auto endName = text.find_first_of (" \t\r\n/>", i + 1);
                if (endName == i + 1 || endName == std::string_view::npos)
                    return "The file is not valid XML.";
                openTags.push_back (text.substr (i + 1, endName - i - 1));
                ++depth;
            }
            i = j + 1;
        }
    }
    return sawRoot && depth == 0 ? std::string() : "The file is not valid XML.";
}

bool prescanJsonText (std::string_view text)
{
    // The downstream parser accepts single quotes and accumulates integers in signed 64 bits. Restrict it to
    // double-quoted JSON and bounded integer prefixes before it sees any input, including unknown members.
    char stack[8] {};
    int depth = 0;
    bool root = false;
    for (size_t i = 0; i < text.size();)
    {
        const char c = text[i++];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (depth == 0 && (root || (c != '{' && c != '[')))
            return false;
        if (c == '{' || c == '[')
        {
            if (depth == 8)
                return false;
            stack[depth++] = c;
            root = true;
        }
        else if (c == '}' || c == ']')
        {
            if (depth == 0 || stack[--depth] != (c == '}' ? '{' : '['))
                return false;
        }
        else if (c == '"')
        {
            bool closed = false;
            while (i < text.size())
            {
                const unsigned char ch = (unsigned char) text[i++];
                if (ch < 32)
                    return false;
                if (ch == '"')
                {
                    closed = true;
                    break;
                }
                if (ch == '\\')
                {
                    if (i == text.size())
                        return false;
                    const char escape = text[i++];
                    if (escape == 'u')
                    {
                        for (int n = 0; n < 4; ++n)
                        {
                            if (i == text.size())
                                return false;
                            const char digit = text[i++];
                            if (! ((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')
                                   || (digit >= 'A' && digit <= 'F')))
                                return false;
                        }
                    }
                    else if (std::string_view ("\"\\/bfnrt").find (escape) == std::string_view::npos)
                        return false;
                }
            }
            if (! closed)
                return false;
        }
        else if (c == '-' || (c >= '0' && c <= '9'))
        {
            const size_t start = i - 1;
            size_t digits = 0;
            for (size_t j = start + (c == '-' ? 1u : 0u); j < text.size() && text[j] >= '0' && text[j] <= '9'; ++j)
                if (++digits > 18)
                    return false;
            while (i < text.size() && std::string_view ("0123456789.eE+-").find (text[i]) != std::string_view::npos)
                ++i;
            if (! parseNumber (text.substr (start, i - start)))
                return false;
        }
        else if (c == 't' || c == 'f' || c == 'n')
        {
            const std::string_view rest = c == 't' ? "rue" : c == 'f' ? "alse" : "ull";
            if (! startsWith (text, i, rest))
                return false;
            i += rest.size();
        }
        else if (c != ',' && c != ':')
            return false;
    }
    return root && depth == 0;
}

std::optional<double> parseNumber (std::string_view s)
{
    while (! s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix (1);
    while (! s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix (1);
    if (! s.empty() && s.front() == '+')
        s.remove_prefix (1);
    if (s.empty() || s.size() > 64)
        return std::nullopt;

    char buffer[65];
    int commas = 0, dots = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (c == ',')
        {
            ++commas;
            c = '.';
        }
        else if (c == '.')
        {
            ++dots;
        }
        else if (! ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == 'e' || c == 'E'))
        {
            return std::nullopt;   // no "nan", "inf", hex, thousands separators ...
        }
        buffer[i] = c;
    }
    if (commas + dots > 1)
        return std::nullopt;

    double v = 0.0;
    const auto r = std::from_chars (buffer, buffer + s.size(), v, std::chars_format::general);
    if (r.ec != std::errc() || r.ptr != buffer + s.size() || ! std::isfinite (v))
        return std::nullopt;
    return v;
}

std::optional<std::vector<unsigned char>> decodeBase64 (std::string_view s)
{
    if (s.size() > kMaxFileBytes)
        return std::nullopt;
    std::vector<unsigned char> out;
    out.reserve (s.size() * 3 / 4 + 3);
    unsigned int acc = 0;
    int bits = 0;
    int padding = 0;
    size_t digits = 0;

    for (const char c : s)
    {
        int v;
        if (c >= 'A' && c <= 'Z')      v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+')             v = 62;
        else if (c == '/')             v = 63;
        else if (c == '=')             { if (++padding > 2) return std::nullopt; continue; }
        else if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        else                           return std::nullopt;

        if (padding)
            return std::nullopt;   // data after '='
        ++digits;
        acc = (acc << 6) | (unsigned int) v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back ((unsigned char) ((acc >> bits) & 0xffu));
        }
    }
    if ((digits + (size_t) padding) % 4 != 0 || digits % 4 == 1
        || (padding == 1 && digits % 4 != 3) || (padding == 2 && digits % 4 != 2)
        || (bits != 0 && (acc & ((1u << bits) - 1u)) != 0))
        return std::nullopt;
    return out;
}

ChainDecode decodeChain (const std::vector<unsigned char>& b)
{
    ChainDecode d;
    size_t pos = 0;
    while (pos < b.size())
    {
        if (b.size() - pos < 5 || d.entries.size() >= 64)
        {
            d.complete = false;
            break;
        }
        const int flag = b[pos];
        const size_t length = (size_t) b[pos + 1] | ((size_t) b[pos + 2] << 8) | ((size_t) b[pos + 3] << 16)
                              | ((size_t) b[pos + 4] << 24);
        pos += 5;
        if (length == 0 || length > kMaxNameLength || length > b.size() - pos)
        {
            d.complete = false;
            break;
        }
        ChainEntry e;
        e.flag = flag;
        e.name.assign (reinterpret_cast<const char*> (b.data() + pos), length);
        pos += length;
        d.entries.push_back (std::move (e));
    }
    return d;
}

//==============================================================================
std::vector<const ModuleSpec*> allModuleSpecs()
{
    std::vector<const ModuleSpec*> all;
    for (const auto* family : { &eqFamilySpecs(), &coreFamilySpecs(), &phase2FamilySpecs() })
        for (const auto& s : *family)
            all.push_back (&s);
    return all;
}

const ModuleSpec* findModuleSpec (Key key)
{
    for (const auto* s : allModuleSpecs())
        if (s->tagKey == key || s->elementKey == key)
            return s;
    return nullptr;
}

std::vector<ModuleType> insertIntoChain (const std::vector<ModuleType>& order, ModuleType type)
{
    if (std::find (order.begin(), order.end(), type) != order.end())
        return order;

    auto result = order;
    const int rank = rankInOurOrder (type);
    auto at = std::find_if (result.begin(), result.end(), [rank] (ModuleType t) { return rankInOurOrder (t) > rank; });
    result.insert (at, type);
    return result;
}

//==============================================================================
ImportPlan buildImportPlan (const XmlNode& root, const ImportOptions& options)
{
    ImportPlan plan;
    const auto* aliases = options.aliases;

    const auto* presetVer = root.attribute ("PresetVer");
    const auto* pluginVer = root.attribute ("PluginVer");
    if (presetVer == nullptr || pluginVer == nullptr)
    {
        plan.error = "This file is not a preset of a supported format.";
        return plan;
    }

    int version = 0;
    if (const auto v = parseNumber (*pluginVer); v.has_value() && *v >= 0.0 && *v < 1.0e9)
        version = (int) *v;
    plan.report.pluginVersion = version;

    // ---- sections
    std::vector<Section> sections;
    const Section* global = nullptr;
    int enabledModules = 0;
    const Section* enabledModule = nullptr;

    sections.reserve (root.children.size());
    for (const auto& c : root.children)
    {
        if (c.tag.empty() || c.tag.size() > kMaxNameLength)
            continue;
        Section s;
        s.node = &c;
        s.tagKey = sectionKey (c.tag, aliases);
        s.enabled = isEnabled (c);
        for (const auto& p : c.children)
            if (const auto* e = p.attribute ("ElementID"); e != nullptr && e->size() <= kMaxNameLength)
            {
                const Key k = sectionKey (*e, aliases);
                if (std::find (s.elementKeys.begin(), s.elementKeys.end(), k) == s.elementKeys.end())
                {
                    if (s.elementKeys.size() == 64)
                    {
                        plan.error = "The section holds far more element identifiers than a preset.";
                        return plan;
                    }
                    s.elementKeys.push_back (k);
                }
            }
        sections.push_back (std::move (s));
    }

    for (const auto& s : sections)
    {
        const bool holdsChain = std::find (s.elementKeys.begin(), s.elementKeys.end(), kChainBlob) != s.elementKeys.end();
        if (s.tagKey == kGlobalSection || holdsChain)
        {
            if (global == nullptr || holdsChain)
                global = &s;
            continue;
        }
        if (s.tagKey == kMetersSection || s.tagKey == kPluginSection)
            continue;
        if (s.enabled)
        {
            ++enabledModules;
            enabledModule = &s;
        }
    }

    auto specOf = [] (const Section& s) -> const ModuleSpec*
    {
        if (const auto* spec = findModuleSpec (s.tagKey))
            return spec;
        for (const Key k : s.elementKeys)
            if (const auto* spec = findModuleSpec (k))
                return spec;
        return nullptr;
    };

    auto describeUnknown = [] (const Section& s) -> std::string
    {
        if (const char* w = skippedModuleDescription (s.tagKey))
            return w;
        for (const Key k : s.elementKeys)
            if (const char* w = skippedModuleDescription (k))
                return w;
        return {};
    };

    plan.values = ourDefaults();

    // ---- a single-module file
    if (enabledModules == 1)
    {
        const auto& s = *enabledModule;
        const auto* spec = specOf (s);
        if (spec == nullptr)
        {
            const auto words = describeUnknown (s);
            plan.error = words.empty() ? "This preset is for a module PureSheen does not have."
                                       : "This preset is for " + withArticle (words)
                                             + " module, which PureSheen does not have.";
            if (! words.empty())
                plan.report.skipped.push_back (words);
            return plan;
        }

        auto stored = collect (*s.node, aliases, plan.report, s.node->tag);
        ModuleImport m (*spec, stored, version, plan.values, plan.report, s.node->tag);
        m.setExtraBytes (collectExtraBytes (*s.node, aliases));
        m.run();
        plan.scope = (int) spec->type;
        plan.order = { spec->type };
        plan.report.imported = { spec->type };
        plan.ok = true;
        return plan;
    }

    // ---- a whole-chain file
    plan.report.globalFile = true;
    std::vector<std::pair<std::string, int>> chain;   // names and flags
    bool chainComplete = false;

    if (global != nullptr)
    {
        for (const auto& c : global->node->children)
        {
            if (c.tag != "ExtraBytes")
                continue;
            const auto* e = c.attribute ("ElementID");
            const auto* data = c.attribute ("Data");
            if (e == nullptr || data == nullptr || sectionKey (*e, aliases) != kChainBlob)
                continue;
            if (const auto bytes = decodeBase64 (*data))
            {
                const auto decoded = decodeChain (*bytes);
                chainComplete = decoded.complete;
                if (chainComplete)
                    for (const auto& entry : decoded.entries)
                        chain.emplace_back (entry.name, entry.flag);
            }
            break;
        }
    }

    if (chainComplete && chain.empty())
    {
        plan.error = "This preset holds no modules.";   // a well-formed empty chain: nothing to import
        plan.holdsNoModules = true;
        return plan;
    }
    if (! chainComplete)
    {
        plan.error = "The module order is missing, empty or damaged.";
        return plan;
    }

    std::vector<std::pair<const Section*, int>> modules;   // section (nullptr = at defaults), flag
    std::vector<const ModuleSpec*> specs;

    if (! chain.empty())
    {
        for (const auto& [name, flag] : chain)
        {
            const Key k = sectionKey (name, aliases);
            const auto* spec = findModuleSpec (k);
            if (spec == nullptr)
            {
                const char* w = skippedModuleDescription (k);
                plan.report.skipped.push_back (w != nullptr ? w : "unknown module");
                continue;
            }
            if (std::find (specs.begin(), specs.end(), spec) != specs.end())
                continue;   // listed twice: the first one wins

            const Section* found = nullptr;
            for (const auto& s : sections)
                if (s.tagKey == spec->tagKey)
                    found = &s;
            if (found == nullptr)
                for (const auto& s : sections)
                    if (std::find (s.elementKeys.begin(), s.elementKeys.end(), spec->elementKey) != s.elementKeys.end())
                        found = &s;

            specs.push_back (spec);
            modules.emplace_back (found, flag);
        }
    }

    for (size_t i = 0; i < specs.size(); ++i)
    {
        const auto& spec = *specs[i];
        const auto* section = modules[i].first;
        const std::string name = section != nullptr ? section->node->tag : std::string ("(module at defaults)");
        std::vector<StoredParam> stored;
        if (section != nullptr)
            stored = collect (*section->node, aliases, plan.report, name);

        // Enabled is only the section load mask used to identify single-module files. In a whole chain, module power
        // comes from the section's own power switch (absent = on), written by run(); the entry's flag is not power.
        ModuleImport m (spec, stored, version, plan.values, plan.report, name);
        if (section != nullptr)
            m.setExtraBytes (collectExtraBytes (*section->node, aliases));
        m.setWritesModulePower (true);
        m.run();

        plan.order.push_back (spec.type);
        plan.report.imported.push_back (spec.type);
    }

    if (global != nullptr)
    {
        auto stored = collect (*global->node, aliases, plan.report, global->node->tag);
        ModuleImport g (globalSpec(), stored, version, plan.values, plan.report, global->node->tag);
        g.run();
    }

    if (plan.order.empty())
    {
        plan.error = plan.report.skipped.empty() ? "This preset holds no modules."
                                                 : "None of the modules in this preset is available in PureSheen.";
        return plan;
    }

    plan.scope = -1;
    plan.ok = true;
    return plan;
}

//==============================================================================
std::string summariseReport (const ImportPlan& plan)
{
    if (! plan.ok)
        return plan.error;

    const auto& r = plan.report;
    auto plural = [] (int n, const char* one, const char* many)
    {
        return std::to_string (n) + " " + (n == 1 ? one : many);
    };

    std::string text;
    if (r.globalFile)
        text = "Imported " + plural ((int) r.imported.size(), "module", "modules") + ", "
               + plural (r.mapped, "setting", "settings");
    else
        text = std::string ("Imported ") + moduleInfo ((ModuleType) plan.scope).name + ", "
               + plural (r.mapped, "setting", "settings");

    if (r.unmapped > 0)
        text += " (" + std::to_string (r.unmapped) + " not imported)";
    text += ".";

    if (! r.skipped.empty())
    {
        std::vector<std::string> unique;
        for (const auto& s : r.skipped)
            if (std::find (unique.begin(), unique.end(), s) == unique.end())
                unique.push_back (s);
        text += " Skipped: ";
        for (size_t i = 0; i < unique.size(); ++i)
            text += (i > 0 ? ", " : "") + unique[i];
        text += ".";
    }
    return text;
}
} // namespace ps::imp
