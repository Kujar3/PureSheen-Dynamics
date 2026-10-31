#include "TestFramework.h"

#include "plugin/import/ImportEngine.h"

#include <cmath>
#include <cstdlib>
#include <cfenv>
#include <cstdint>
#include <set>
#include <string>

// Preset importer (engine + mapping tables, pure C++) on SYNTHETIC trees written by us. Recognition works by key, so
// the tests give our own generic names to the sections and controls through ImportOptions::aliases (the keys come
// from the importer's own tables, looked up by their our-words labels). The XML text path (juce::XmlDocument), the
// processor apply / undo step and a fuzz of the text parser run in `PureSheenDev importtest`.
using namespace ps::imp;

namespace
{
using Attrs = std::vector<std::pair<std::string, std::string>>;

XmlNode el (std::string tag, Attrs attrs = {}, std::vector<XmlNode> children = {})
{
    XmlNode n;
    n.tag = std::move (tag);
    n.attributes = std::move (attrs);
    n.children = std::move (children);
    return n;
}

XmlNode param (const std::string& element, const std::string& id, const std::string& value)
{
    return el ("Param", { { "ElementID", element }, { "ParamID", id }, { "Value", value } });
}

std::string base64 (const std::vector<unsigned char>& bytes)
{
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3)
    {
        const unsigned v = (unsigned) bytes[i] << 16 | (unsigned) bytes[i + 1] << 8 | bytes[i + 2];
        out += abc[(v >> 18) & 63]; out += abc[(v >> 12) & 63]; out += abc[(v >> 6) & 63]; out += abc[v & 63];
    }
    if (bytes.size() - i == 1)
    {
        const unsigned v = (unsigned) bytes[i] << 16;
        out += abc[(v >> 18) & 63]; out += abc[(v >> 12) & 63]; out += "==";
    }
    else if (bytes.size() - i == 2)
    {
        const unsigned v = (unsigned) bytes[i] << 16 | (unsigned) bytes[i + 1] << 8;
        out += abc[(v >> 18) & 63]; out += abc[(v >> 12) & 63]; out += abc[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::vector<unsigned char> chainBytes (const std::vector<std::pair<std::string, int>>& entries)
{
    std::vector<unsigned char> b;
    for (const auto& [name, flag] : entries)
    {
        b.push_back ((unsigned char) flag);
        const auto n = (std::uint32_t) name.size();
        for (int i = 0; i < 4; ++i)
            b.push_back ((unsigned char) ((n >> (8 * i)) & 0xffu));
        b.insert (b.end(), name.begin(), name.end());
    }
    return b;
}

const ModuleSpec& specOf (ps::ModuleType t)
{
    for (const auto* s : allModuleSpecs())
        if (s->type == t)
            return *s;
    throw pstest::RequireFailed {};
}

Key ruleKey (ps::ModuleType t, const char* what)
{
    for (const auto& r : *specOf (t).rules)
        if (std::string (r.what) == what)
            return r.key;
    pstest::reportFailure (__FILE__, __LINE__, std::string ("no rule labelled ") + what);
    throw pstest::RequireFailed {};
}

// Our generic names for the format's sections and the Imager's controls.
const Aliases& aliases()
{
    static const Aliases a = []
    {
        Aliases m;
        const auto img = ps::ModuleType::Imager;
        m["Spread"] = specOf (img).tagKey;
        m["SpreadChain"] = specOf (img).elementKey;
        m["Tone"] = specOf (ps::ModuleType::Equalizer1).tagKey;
        m["ToneChain"] = specOf (ps::ModuleType::Equalizer1).elementKey;
        m["Common"] = kGlobalSectionKey;
        m["Order"] = kChainBlobKey;
        m["Tape"] = skippedModuleKey ("tape emulation");
        m["Width #"] = ruleKey (img, "band width");
        m["Present #"] = ruleKey (img, "band present");
        m["Count"] = ruleKey (img, "band count (older files)");
        m["Edge #"] = ruleKey (img, "crossover frequency");
        m["Kind"] = ruleKey (img, "crossover kind");
        m["Off #"] = ruleKey (img, "band bypass");
        m["Solo #"] = ruleKey (img, "band solo");
        m["Wide On"] = ruleKey (img, "stereoize on");
        m["Wide Style"] = ruleKey (img, "stereoize mode");
        m["Wide Time"] = ruleKey (img, "stereoize amount");
        m["Split"] = ruleKey (img, "channel mode");
        m["Power Off"] = kSectionBypassKey;   // every module section's own power switch
        return m;
    }();
    return a;
}

ImportOptions opts()
{
    ImportOptions o;
    o.aliases = &aliases();
    return o;
}

XmlNode chainNode (const std::vector<std::pair<std::string, int>>& entries)
{
    return el ("ExtraBytes", { { "ElementID", "Order" }, { "Data", base64 (chainBytes (entries)) } });
}

// A single-module file: the module section enabled, the global section not.
XmlNode moduleFile (const std::string& version, std::vector<XmlNode> params)
{
    return el ("Root", { { "PresetVer", "1" }, { "PluginVer", version } },
               { el ("Common", { { "Enabled", "0" } }, { chainNode ({ { "SpreadChain", 0 } }) }),
                 el ("Spread", { { "Enabled", "1" } }, std::move (params)),
                 el ("Tone", { { "Enabled", "0" } }) });
}

float value (const ImportPlan& p, const char* id)
{
    const int i = ps::paramIndex (id);
    if (i < 0 || i >= (int) p.values.size())
    {
        pstest::reportFailure (__FILE__, __LINE__, std::string ("unknown id ") + id);
        return -1.0e9f;
    }
    return p.values[(size_t) i];
}

bool near (float a, double b) { return std::abs ((double) a - b) < 1.0e-3; }
} // namespace

//==============================================================================
TEST_CASE ("import. key is FNV-1a 64 of the exact bytes")
{
    static_assert (importKey ("") == 0xcbf29ce484222325ull, "offset basis");
    static_assert (importKey ("a") == 0xaf63dc4c8601ec8cull, "test vector");
    CHECK (importKey ("Width #") != importKey ("width #"));
}

TEST_CASE ("import. ParamID split: set B prefix, first number, template key")
{
    const auto p = splitParamName ("Aux: Band 12 Gain 3");
    CHECK_EQ (p.set, 1);
    CHECK_EQ (p.number, 12);
    CHECK_EQ (p.templ, std::string ("Band # Gain 3"));
    CHECK_EQ (p.key, importKey ("Band # Gain 3"));

    const auto q = splitParamName ("Gain");
    CHECK_EQ (q.set, 0);
    CHECK_EQ (q.number, -1);
    CHECK_EQ (q.key, importKey ("Gain"));

    CHECK_EQ (splitParamName ("Band 12345678 Q").number, -2);
    CHECK_EQ (splitParamName (std::string (300, 'x')).number, -2);
    CHECK_EQ (splitParamName ("Width 3", &aliases()).key, aliases().at ("Width #"));
}

TEST_CASE ("import. numbers: dot, decimal comma, integers; garbage rejected")
{
    CHECK (near ((float) parseNumber ("5499.99951172").value_or (0), 5499.99951172));
    CHECK (near ((float) parseNumber ("1,79999995").value_or (0), 1.79999995));
    CHECK (near ((float) parseNumber ("13439,62500000").value_or (0), 13439.625));
    CHECK (near ((float) parseNumber ("-12").value_or (0), -12.0));
    CHECK (near ((float) parseNumber ("+3").value_or (0), 3.0));
    CHECK (near ((float) parseNumber ("  7 ").value_or (0), 7.0));
    for (const char* bad : { "", " ", "nan", "NaN", "inf", "-inf", "1.2.3", "1,2.3", "0x10", "1e999", "12a", "--1",
                             "1 000", "\xff" })
        CHECK (! parseNumber (bad).has_value());
}

TEST_CASE ("import. base64 and chain blob decoding")
{
    const auto bytes = chainBytes ({ { "Alpha", 0 }, { "Beta Two", 1 } });
    const auto decoded = decodeBase64 (base64 (bytes));
    REQUIRE (decoded.has_value());
    CHECK (*decoded == bytes);

    const auto chain = decodeChain (*decoded);
    CHECK (chain.complete);
    REQUIRE (chain.entries.size() == 2);
    CHECK_EQ (chain.entries[0].name, std::string ("Alpha"));
    CHECK_EQ (chain.entries[1].name, std::string ("Beta Two"));
    CHECK_EQ (chain.entries[1].flag, 1);

    auto truncated = bytes;
    truncated.resize (bytes.size() - 2);
    const auto t = decodeChain (truncated);
    CHECK (! t.complete);
    CHECK_EQ (t.entries.size(), (size_t) 1);

    std::vector<unsigned char> huge { 0, 0xff, 0xff, 0xff, 0x7f, 'a' };
    CHECK (! decodeChain (huge).complete);
    CHECK (decodeChain (huge).entries.empty());

    CHECK (! decodeBase64 ("AAo*AAB").has_value());
    CHECK (! decodeBase64 ("AA==AA").has_value());
    CHECK (decodeBase64 ("").has_value());
}

TEST_CASE ("import. prescan rejects hostile structure, accepts a preset")
{
    CHECK (prescanXmlText ("<?xml version=\"1.0\"?>\n<!-- c > -->\n<R a=\"x>y\"><S><P b='1'/></S></R>").empty());

    std::string deep;
    for (int i = 0; i < 100; ++i)
        deep += "<a>";
    for (int i = 0; i < 100; ++i)
        deep += "</a>";
    CHECK (! prescanXmlText (deep).empty());

    CHECK (! prescanXmlText ("<!DOCTYPE r [<!ENTITY a \"aaaa\">]><r>&a;</r>").empty());
    CHECK (! prescanXmlText ("<r><!ENTITY x \"y\"></r>").empty());
    CHECK (! prescanXmlText (std::string ("<r>\0</r>", 8)).empty());
    CHECK (! prescanXmlText ("<r><!-- open").empty());
    CHECK (! prescanXmlText ("<r a=\"open").empty());

    std::string many = "<r>";
    for (int i = 0; i <= kMaxElements; ++i)
        many += "<a/>";
    many += "</r>";
    CHECK (! prescanXmlText (many).empty());
}

TEST_CASE ("import. single-module file: absent values take the foreign defaults")
{
    // Only band 2's width is stored (sparse). Four bands present.
    const auto plan = buildImportPlan (moduleFile ("110000", { param ("SpreadChain", "Present 1", "1"),
                                                               param ("SpreadChain", "Present 2", "1"),
                                                               param ("SpreadChain", "Present 3", "1"),
                                                               param ("SpreadChain", "Width 2", "30.00000000") }),
                                       opts());
    REQUIRE (plan.ok);
    CHECK_EQ (plan.scope, (int) ps::ModuleType::Imager);
    CHECK (! plan.report.globalFile);
    CHECK (near (value (plan, "img.bands"), 4.0));
    CHECK (near (value (plan, "img.b2.width"), 30.0));
    CHECK (near (value (plan, "img.b1.width"), 0.0));
    CHECK (near (value (plan, "img.xoverType"), (double) ps::CrossoverType::Digital));  // foreign default kind
    CHECK (near (value (plan, "img.stz.mode"), 0.0));                                   // 110000: mode I
    CHECK_EQ (plan.report.mapped, 4);
    CHECK_EQ (plan.report.unmapped, 0);
    CHECK (plan.report.defaulted > 0);

    // The same file from a 100000 writer: the stereoize mode default differs (only the non-default is ever stored).
    const auto older = buildImportPlan (moduleFile ("100000", {}), opts());
    REQUIRE (older.ok);
    CHECK (near (value (older, "img.stz.mode"), 1.0));
    CHECK (near (value (older, "img.bands"), 1.0));   // nothing stored: one band
}

TEST_CASE ("import. decimal comma values import like dots")
{
    const auto plan = buildImportPlan (moduleFile ("8000", { param ("SpreadChain", "Width 1", "-12,50000000"),
                                                             param ("SpreadChain", "Count", "2") }),
                                       opts());
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "img.b1.width"), -12.5));
    CHECK (near (value (plan, "img.bands"), 2.0));   // older writer: the band count control
}

TEST_CASE ("import. set B prefix writes channel set B")
{
    const auto plan = buildImportPlan (moduleFile ("110000", { param ("SpreadChain", "Aux: Width 1", "-50.00000000"),
                                                               param ("SpreadChain", "Aux: Wide On", "1"),
                                                               param ("SpreadChain", "Split", "1") }),
                                       opts());
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "img.b.b1.width"), -50.0));
    CHECK (near (value (plan, "img.b1.width"), 0.0));
    CHECK (near (value (plan, "img.b.stz.on"), 1.0));
    CHECK (near (value (plan, "img.stz.on"), 0.0));
    CHECK (near (value (plan, "img.chMode"), 1.0));   // T/S is the second Imager mode
    CHECK_EQ (plan.report.mapped, 3);
}

TEST_CASE ("import. multiband layout closes gaps and maps the crossovers")
{
    // Slots 0, 2, 3 present; slot 1 removed (its width and the cutoff at its lower edge are stale).
    const auto plan = buildImportPlan (moduleFile ("100000", { param ("SpreadChain", "Present 2", "1"),
                                                               param ("SpreadChain", "Present 3", "1"),
                                                               param ("SpreadChain", "Edge 1", "200.00000000"),
                                                               param ("SpreadChain", "Edge 2", "900.00000000"),
                                                               param ("SpreadChain", "Edge 3", "5000.00000000"),
                                                               param ("SpreadChain", "Width 2", "77.00000000"),
                                                               param ("SpreadChain", "Width 3", "40.00000000"),
                                                               param ("SpreadChain", "Off 3", "1") }),
                                       opts());
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "img.bands"), 3.0));
    CHECK (near (value (plan, "img.xover1"), 900.0));
    CHECK (near (value (plan, "img.xover2"), 5000.0));
    CHECK (near (value (plan, "img.b2.width"), 40.0));   // foreign slot 2 -> our band 2
    CHECK (near (value (plan, "img.b3.width"), 0.0));    // slot 3 bypassed -> neutral
    CHECK_EQ (plan.report.ignored, 2);                    // stale width of slot 1, stale edge 1
    CHECK_EQ (plan.report.unmapped, 0);
}

TEST_CASE ("import. channel mode out of range falls back to Stereo and counts a clamp")
{
    const auto plan = buildImportPlan (moduleFile ("110000", { param ("SpreadChain", "Split", "7") }), opts());
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "img.chMode"), 0.0));
    CHECK_EQ (plan.report.clamped, 1);
}

TEST_CASE ("import. values are clamped into our ranges and counted")
{
    const auto plan = buildImportPlan (moduleFile ("110000", { param ("SpreadChain", "Width 1", "1000000000000.0"),
                                                               param ("SpreadChain", "Edge 1", "-5"),
                                                               param ("SpreadChain", "Present 1", "1") }),
                                       opts());
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "img.b1.width"), 100.0));
    CHECK (near (value (plan, "img.xover1"), 20.0));
    CHECK_EQ (plan.report.clamped, 2);
}

TEST_CASE ("import. whole-chain file: chain order, skipped and unknown modules, stale sections ignored")
{
    const auto root = el ("Root", { { "PresetVer", "3" }, { "PluginVer", "100000" } },
                          { el ("Common", { { "Enabled", "1" } },
                                { chainNode ({ { "Tape", 0 }, { "SpreadChain", 0 }, { "Somebody Else", 0 },
                                               { "ToneChain", 0 }, { "SpreadChain", 0 } }),
                                  el ("ExtraBytes", { { "ElementID", "Common" }, { "Data", "" } }) }),
                            el ("Spread", { { "Enabled", "1" } }, { param ("SpreadChain", "Width 1", "25.00000000") }),
                            el ("Tone", { { "Enabled", "1" } }),
                            el ("Tape", { { "Enabled", "1" } }, { param ("Tape", "Drive", "3") }),
                            el ("Unrelated", { { "Enabled", "1" } }, { param ("Unrelated", "Width 1", "80") }) });
    const auto plan = buildImportPlan (root, opts());
    REQUIRE (plan.ok);
    CHECK_EQ (plan.scope, -1);
    CHECK (plan.report.globalFile);
    REQUIRE (plan.order.size() == 2);
    CHECK_EQ (plan.order[0], ps::ModuleType::Imager);
    CHECK_EQ (plan.order[1], ps::ModuleType::Equalizer1);
    REQUIRE (plan.report.skipped.size() == 2);
    CHECK_EQ (plan.report.skipped[0], std::string ("tape emulation"));
    CHECK_EQ (plan.report.skipped[1], std::string ("unknown module"));
    CHECK (near (value (plan, "img.b1.width"), 25.0));
    CHECK (near (value (plan, "img.on"), 1.0));
    CHECK (near (value (plan, "eq1.on"), 1.0));
    CHECK_EQ (plan.report.mapped, 1);    // the skipped and unrelated sections are not looked at
    CHECK_EQ (plan.report.unmapped, 0);

    const auto line = summariseReport (plan);
    CHECK_EQ (line, std::string ("Imported 2 modules, 1 setting. Skipped: tape emulation, unknown module."));
}

TEST_CASE ("import. the section power switch turns a chain module off; the chain flag byte is not power")
{
    // The flag byte of the chain entries is parsed but stays 0 for a switched-off module in the real product, so a
    // non-zero flag no longer switches anything; the section's own power switch does.
    for (const char* off : { "1", "0.5", "0", "0.25" })
    {
        const auto root = el ("Root", { { "PresetVer", "3" }, { "PluginVer", "120000" } },
                              { el ("Common", { { "Enabled", "1" } },
                                    { chainNode ({ { "SpreadChain", 0 }, { "ToneChain", 1 } }) }),
                                el ("Spread", { { "Enabled", "1" } }, { param ("SpreadChain", "Power Off", off) }),
                                el ("Tone", { { "Enabled", "1" } }) });
        const auto plan = buildImportPlan (root, opts());
        REQUIRE (plan.ok);
        const bool isOff = std::atof (off) >= 0.5;
        CHECK (near (value (plan, "img.on"), isOff ? 0.0 : 1.0));
        CHECK (near (value (plan, "eq1.on"), 1.0));           // flag 1, no power switch stored: on
        CHECK_EQ (plan.report.mapped, 1);                     // the power switch counts as imported
        CHECK_EQ (plan.report.unmapped, 0);
    }

    // An unreadable switch is not imported and leaves the module on.
    const auto bad = buildImportPlan (
        el ("Root", { { "PresetVer", "3" }, { "PluginVer", "120000" } },
              { el ("Common", { { "Enabled", "1" } }, { chainNode ({ { "SpreadChain", 0 } }) }),
                el ("Spread", { { "Enabled", "1" } }, { param ("SpreadChain", "Power Off", "x") }),
                el ("Tone", { { "Enabled", "1" } }) }), opts());
    REQUIRE (bad.ok);
    CHECK (near (value (bad, "img.on"), 1.0));
    CHECK_EQ (bad.report.unmapped, 1);
}

TEST_CASE ("import. the section power switch works for every module type")
{
    for (const auto* spec : allModuleSpecs())
    {
        Aliases a;
        a["Unit"] = spec->tagKey;
        a["UnitChain"] = spec->elementKey;
        a["Other"] = specOf (spec->type == ps::ModuleType::Imager ? ps::ModuleType::Equalizer1
                                                                  : ps::ModuleType::Imager).tagKey;
        a["OtherChain"] = specOf (spec->type == ps::ModuleType::Imager ? ps::ModuleType::Equalizer1
                                                                       : ps::ModuleType::Imager).elementKey;
        a["Common"] = kGlobalSectionKey;
        a["Order"] = kChainBlobKey;
        a["Power Off"] = kSectionBypassKey;
        ImportOptions o;
        o.aliases = &a;

        const auto root = el ("Root", { { "PresetVer", "3" }, { "PluginVer", "120000" } },
                              { el ("Common", { { "Enabled", "1" } },
                                    { chainNode ({ { "UnitChain", 0 }, { "OtherChain", 0 } }) }),
                                el ("Unit", { { "Enabled", "1" } }, { param ("UnitChain", "Power Off", "1") }),
                                el ("Other", { { "Enabled", "1" } }) });
        const auto plan = buildImportPlan (root, o);
        REQUIRE (plan.ok);
        const std::string prefix = ps::moduleInfo (spec->type).idPrefix;
        const std::string other = ps::moduleInfo (plan.order.size() > 1 ? plan.order[1] : spec->type).idPrefix;
        CHECK_EQ (plan.order.size(), (size_t) 2);
        CHECK (near (value (plan, (prefix + ".on").c_str()), 0.0));
        CHECK (near (value (plan, (other + ".on").c_str()), 1.0));
        CHECK_EQ (plan.report.mapped, 1);
        CHECK_EQ (plan.report.unmapped, 0);
    }
}

TEST_CASE ("import. a single-module file ignores the power switch (module presets never carry power)")
{
    const auto plan = buildImportPlan (moduleFile ("120000", { param ("SpreadChain", "Power Off", "1"),
                                                               param ("SpreadChain", "Width 1", "30") }), opts());
    REQUIRE (plan.ok);
    CHECK_EQ (plan.scope, (int) ps::ModuleType::Imager);
    CHECK (near (value (plan, "img.b1.width"), 30.0));
    CHECK_EQ (plan.report.mapped, 1);
    CHECK_EQ (plan.report.ignored, 1);
    CHECK_EQ (plan.report.unmapped, 0);
}

TEST_CASE ("import. a well-formed empty chain is refused as holding no modules")
{
    const auto root = el ("Root", { { "PresetVer", "1" }, { "PluginVer", "120000" } },
                          { el ("Common", { { "Enabled", "1" } },
                                { el ("ExtraBytes", { { "ElementID", "Order" }, { "Data", "" } }) }),
                            el ("Spread", { { "Enabled", "1" } }), el ("Tone", { { "Enabled", "1" } }) });
    const auto plan = buildImportPlan (root, opts());
    CHECK (! plan.ok);
    CHECK (plan.holdsNoModules);
    CHECK (plan.order.empty());
    CHECK_EQ (plan.error, std::string ("This preset holds no modules."));

    // A damaged chain is not "empty".
    const auto damaged = buildImportPlan (
        el ("Root", { { "PresetVer", "1" }, { "PluginVer", "120000" } },
              { el ("Common", { { "Enabled", "1" } }, { el ("ExtraBytes", { { "ElementID", "Order" }, { "Data", "A" } }) }),
                el ("Spread", { { "Enabled", "1" } }), el ("Tone", { { "Enabled", "1" } }) }), opts());
    CHECK (! damaged.ok);
    CHECK (! damaged.holdsNoModules);
}

TEST_CASE ("import. whole-chain order follows the first decoded entry regardless of section load masks")
{
    for (const bool enabled : { false, true })
    {
        const auto root = el ("Root", { { "PresetVer", "3" }, { "PluginVer", "120000" } },
                              { el ("Common", { { "Enabled", "1" } },
                                    { chainNode ({ { "ToneChain", 1 }, { "SpreadChain", 0 },
                                                   { "ToneChain", 0 }, { "SpreadChain", 1 } }) }),
                                el ("Spread", { { "Enabled", enabled ? "1" : "0" } },
                                      { param ("SpreadChain", "Width 1", "27") }),
                                el ("Tone", { { "Enabled", enabled ? "1" : "0" } }) });
        const auto plan = buildImportPlan (root, opts());
        REQUIRE (plan.ok);
        CHECK_EQ (plan.scope, -1);
        CHECK (plan.order == (std::vector<ps::ModuleType> { ps::ModuleType::Equalizer1, ps::ModuleType::Imager }));
        CHECK (near (value (plan, "eq1.on"), 1.0));           // the entry flags are not power
        CHECK (near (value (plan, "img.on"), 1.0));
        CHECK (near (value (plan, "img.b1.width"), 27.0));
        CHECK_EQ (plan.report.mapped, 1);
        CHECK_EQ (plan.report.unmapped, 0);
    }

    // A chain entry with no section is still active and takes that writer's foreign module defaults.
    const auto missing = buildImportPlan (
        el ("Root", { { "PresetVer", "3" }, { "PluginVer", "100000" } },
              { el ("Common", { { "Enabled", "1" } }, { chainNode ({ { "SpreadChain", 0 } }) }) }), opts());
    REQUIRE (missing.ok);
    CHECK_EQ (missing.scope, -1);
    CHECK (near (value (missing, "img.on"), 1.0));
    CHECK (near (value (missing, "img.stz.mode"), 1.0));
}

TEST_CASE ("import. single-module load masks ignore stale chain entries and disabled sections")
{
    const auto root = el ("Root", { { "PresetVer", "3" }, { "PluginVer", "120000" } },
                          { el ("Common", { { "Enabled", "1" } },
                                { chainNode ({ { "ToneChain", 0 }, { "SpreadChain", 1 } }) }),
                            el ("Tone", { { "Enabled", "0" } }, { param ("ToneChain", "Unknown", "1") }),
                            el ("Spread", { { "Enabled", "1" } }, { param ("SpreadChain", "Width 1", "35") }) });
    const auto plan = buildImportPlan (root, opts());
    REQUIRE (plan.ok);
    CHECK_EQ (plan.scope, (int) ps::ModuleType::Imager);
    CHECK (! plan.report.globalFile);
    CHECK (plan.order == (std::vector<ps::ModuleType> { ps::ModuleType::Imager }));
    CHECK (near (value (plan, "img.on"), 1.0));
    CHECK (near (value (plan, "img.b1.width"), 35.0));
    CHECK_EQ (plan.report.mapped, 1);
    CHECK_EQ (plan.report.unmapped, 0);
}

TEST_CASE ("import. missing chain blob refuses stale enabled sections")
{
    const auto root = el ("Root", { { "PresetVer", "3" }, { "PluginVer", "120000" } },
                          { el ("Tone", { { "Enabled", "1" } }), el ("Spread", { { "Enabled", "1" } }) });
    const auto plan = buildImportPlan (root, opts());
    CHECK (! plan.ok);
    CHECK (plan.order.empty());
    CHECK (! plan.error.empty());
}

TEST_CASE ("import. damaged and empty chains never apply a prefix or stale sections")
{
    auto partial = chainBytes ({ { "SpreadChain", 0 }, { "ToneChain", 0 } });
    partial.pop_back();
    for (const auto& data : { std::string(), std::string ("!"), std::string ("A"), base64 (partial) })
    {
        const auto root = el ("Root", { { "PresetVer", "1" }, { "PluginVer", "120000" } },
                             { el ("Common", { { "Enabled", "1" } },
                                   { el ("ExtraBytes", { { "ElementID", "Order" }, { "Data", data } }) }),
                               el ("Spread", { { "Enabled", "1" } }), el ("Tone", { { "Enabled", "1" } }) });
        const auto plan = buildImportPlan (root, opts());
        CHECK (! plan.ok);
        CHECK (plan.order.empty());
    }
}

TEST_CASE ("import. base64 rejects incomplete groups and corrupt padding")
{
    for (const auto* bad : { "A", "AA", "AAA", "=", "====", "A===", "AAAA=", "AB==", "AAB=" })
        CHECK (! decodeBase64 (bad));
    CHECK (decodeBase64 (" AA==\r\n").value() == std::vector<unsigned char> { 0 });
    CHECK (decodeBase64 ("AAA=").value() == std::vector<unsigned char> ({ 0, 0 }));
}

TEST_CASE ("import. structural prescans reject parser depth and integer overflow bypasses")
{
    for (const auto* bad : { "</a><a/>", "<a></b>", "<a/><b/>", "<a/>&bad;", "<a>" })
        CHECK (! prescanXmlText (bad).empty());
    const auto hiddenChildren = std::string ("<a <!-- ' --> >") + std::string (100, '<') + "<!-- ' --></a>";
    CHECK (! prescanXmlText (hiddenChildren).empty());
    CHECK (! prescanXmlText ("<a <?quote ' ?> ><b/><b/><?quote ' ?></a>").empty());
    CHECK (prescanJsonText ("{\"a\":[1e+2,-2.5,{\"b\":true}],\"c\":\"[{\\\"\"}"));
    for (const auto* bad : { "{'a':']]]]]]]]','b':[[[[[[[[[]]]]]]]]]}",
                             "{\"a\":9223372036854775808}", "{\"a\":99999999999999999999.0}",
                             "{\"a\":1}{}", "{\"a\":1}]", "{\"a\":[[[[[[[[[]]]]]]]]]}" })
        CHECK (! prescanJsonText (bad));
}

TEST_CASE ("import. extreme older band counts clamp before integer conversion")
{
    for (const auto* number : { "1e308", "-1e308" })
    {
        std::feclearexcept (FE_ALL_EXCEPT);
        const auto plan = buildImportPlan (moduleFile ("8000", { param ("SpreadChain", "Count", number) }), opts());
        REQUIRE (plan.ok);
        CHECK (std::fetestexcept (FE_INVALID) == 0);
        CHECK (near (value (plan, "img.bands"), number[0] == '-' ? 1.0 : 4.0));
    }
}

TEST_CASE ("import. attribute and section identifier counts bound parser work")
{
    std::string text = "<Root";
    std::vector<XmlNode> params;
    for (int i = 0; i < 65; ++i)
    {
        text += " a" + std::to_string (i) + "='x'";
        params.push_back (param ("Element" + std::to_string (i), "Width 1", "1"));
    }
    text += "/>";
    CHECK (! prescanXmlText (text).empty());
    CHECK (! buildImportPlan (moduleFile ("120000", std::move (params)), opts()).ok);
}

TEST_CASE ("import. unknown elements, params and bad values are counted, never fatal")
{
    const auto plan = buildImportPlan (moduleFile ("110000", { param ("SpreadChain", "Present 1", "1"),
                                                               param ("SpreadChain", "Mystery 1", "1"),
                                                               param ("SpreadChain", "Width 1", "abc"),
                                                               param ("SpreadChain", "Width 9", "10"),
                                                               param ("SpreadChain", "Width 2", "10"),
                                                               param ("SpreadChain", "Width 2", "20"),
                                                               el ("Param", { { "Value", "1" } }),
                                                               el ("Strange", { { "x", "y" } }, { el ("Deeper") }),
                                                               param ("SpreadChain", "Solo 0", "1") }),
                                       opts());
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "img.b2.width"), 10.0));   // the first of a duplicate wins
    CHECK (near (value (plan, "img.b1.width"), 0.0));    // unreadable -> the default
    CHECK_EQ (plan.report.mapped, 2);                     // band 2 present + its width
    CHECK_EQ (plan.report.ignored, 1);                    // the solo flag
    CHECK_EQ (plan.report.unmapped, 5);                   // mystery, bad value, number 9, duplicate, no ParamID
    CHECK_EQ (plan.report.unmappedList.size(), (size_t) 5);
}

TEST_CASE ("import. files that are not importable say so in plain words")
{
    CHECK (! buildImportPlan (el ("Root", { { "PresetVer", "1" } })).ok);
    CHECK (! buildImportPlan (el ("Root")).ok);

    const auto empty = buildImportPlan (el ("Root", { { "PresetVer", "1" }, { "PluginVer", "1" } }), opts());
    CHECK (! empty.ok);
    CHECK (! empty.error.empty());

    // A single-module file for a module we do not have.
    const auto tape = buildImportPlan (el ("Root", { { "PresetVer", "1" }, { "PluginVer", "110000" } },
                                           { el ("Tape", { { "Enabled", "1" } }, { param ("Tape", "Drive", "3") }),
                                             el ("Common", { { "Enabled", "0" } }) }),
                                       opts());
    CHECK (! tape.ok);
    CHECK (tape.error.find ("tape emulation") != std::string::npos);
}

TEST_CASE ("import. hostile trees never crash and every value stays in range")
{
    std::uint32_t seed = 12345u;
    auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    const char* tags[] = { "Spread", "Tone", "Common", "Tape", "Param", "ExtraBytes", "X", "" };
    const char* attrs[] = { "Enabled", "ElementID", "ParamID", "Value", "Data", "PresetVer", "PluginVer" };
    const char* texts[] = { "Width 1", "Aux: Width 4", "Present 2", "Edge 3", "Kind", "Split", "Count", "Off 0",
                            "Wide Style", "Wide Time", "1", "0", "-1", "1e308", "-1e308", "nan", "", "9999999999",
                            "SpreadChain", "Order", "AAoAAAB", "1,5", "Band 99999999 Gain", "Aux: ", "Aux: 7" };
    const auto& specs = ps::paramSpecs();

    for (int round = 0; round < 3000; ++round)
    {
        std::function<XmlNode (int)> make = [&] (int depth)
        {
            XmlNode n;
            n.tag = tags[next() % std::size (tags)];
            const int na = (int) (next() % 5);
            for (int i = 0; i < na; ++i)
                n.attributes.emplace_back (attrs[next() % std::size (attrs)], texts[next() % std::size (texts)]);
            if (depth < 4)
            {
                const int nc = (int) (next() % 6);
                for (int i = 0; i < nc; ++i)
                    n.children.push_back (make (depth + 1));
            }
            return n;
        };

        auto root = make (0);
        if (next() % 2 == 0)
            root.attributes = { { "PresetVer", "1" }, { "PluginVer", texts[next() % std::size (texts)] } };
        if (next() % 3 == 0)   // a readable chain now and then
            root.children.push_back (el ("Common", { { "Enabled", "1" } },
                                         { chainNode ({ { "SpreadChain", (int) (next() % 2) }, { "ToneChain", 0 } }) }));

        const auto plan = buildImportPlan (root, opts());
        if (! plan.ok)
            continue;

        REQUIRE (plan.values.size() == specs.size());
        for (size_t i = 0; i < specs.size(); ++i)
        {
            const float v = plan.values[i];
            const bool inRange = std::isfinite (v) && v >= specs[i].minValue - 1.0e-3f && v <= specs[i].maxValue + 1.0e-3f;
            if (! inRange)
            {
                pstest::reportFailure (__FILE__, __LINE__, "out of range: " + specs[i].id + " = " + std::to_string (v));
                return;
            }
        }
        (void) summariseReport (plan);
    }
}

TEST_CASE ("import. a single module is inserted in our usual mastering order")
{
    using T = ps::ModuleType;
    CHECK (insertIntoChain ({ T::Equalizer1, T::Dynamics, T::Maximizer }, T::Imager)
           == (std::vector<T> { T::Equalizer1, T::Dynamics, T::Imager, T::Maximizer }));
    CHECK (insertIntoChain ({ T::Maximizer }, T::ToneBalance) == (std::vector<T> { T::ToneBalance, T::Maximizer }));
    CHECK (insertIntoChain ({ T::Maximizer, T::Equalizer1 }, T::Equalizer2)
           == (std::vector<T> { T::Equalizer2, T::Maximizer, T::Equalizer1 }));
    CHECK (insertIntoChain ({ T::Imager, T::Dynamics }, T::Dynamics) == (std::vector<T> { T::Imager, T::Dynamics }));
    CHECK (insertIntoChain ({}, T::Punch) == (std::vector<T> { T::Punch }));
}

TEST_CASE ("import. every table row targets real ids; keys unique per table and across sections")
{
    std::set<Key> sectionKeys;
    std::set<ps::ModuleType> types;
    for (const auto* spec : allModuleSpecs())
    {
        CHECK (types.insert (spec->type).second);
        CHECK (sectionKeys.insert (spec->tagKey).second);
        if (spec->elementKey != spec->tagKey)
            CHECK (sectionKeys.insert (spec->elementKey).second);
        CHECK (skippedModuleDescription (spec->tagKey) == nullptr);
        CHECK (spec->tagKey != kGlobalSectionKey && spec->tagKey != kChainBlobKey);
    }
    CHECK_EQ (types.size(), (size_t) ps::kNumModuleTypes);   // every module type has a spec

    std::vector<const ModuleSpec*> all = allModuleSpecs();
    all.push_back (&globalSpec());
    for (const auto* spec : all)
    {
        REQUIRE (spec->rules != nullptr);
        const std::string prefix = (int) spec->type < ps::kNumModuleTypes ? ps::moduleInfo (spec->type).idPrefix : "";
        std::set<Key> keys;
        for (const auto& r : *spec->rules)
        {
            if (! keys.insert (r.key).second)
                pstest::reportFailure (__FILE__, __LINE__, prefix + ": duplicate key in its table (" + r.what + ")");
            CHECK (std::string (r.what).size() > 0);
            CHECK (r.count >= 0 && r.count <= 64 && (r.first == 0 || r.first == 1));
            if (r.act != Act::Map)
                continue;

            for (const char* pattern : { r.idA, r.idB })
            {
                if (pattern == nullptr)
                    continue;
                for (int band = 1; band <= std::max (1, r.count); ++band)
                {
                    std::string id;
                    for (const char* c = pattern; *c != 0; ++c)
                        id += *c == '~' ? prefix : (*c == '#' ? std::to_string (band) : std::string (1, *c));
                    if (ps::paramIndex (id) < 0)
                        pstest::reportFailure (__FILE__, __LINE__, "table target does not exist: " + id);
                }
            }
        }
    }
}
