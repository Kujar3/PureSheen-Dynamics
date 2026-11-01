#include "TestFramework.h"

#include "plugin/import/ImportEngine.h"

#include <cmath>
#include <cstdint>
#include <set>
#include <string>

// Preset importer, phase 2 family tables (Tone Balance, Definition, Smoother, Punch, Low Contrast, Bass Shaper) on
// SYNTHETIC trees written by us. Recognition works by key, so every section and control gets a generic name of our
// own through ImportOptions::aliases: "<our prefix> <our label>" (plus " #" for numbered controls), with the key taken
// from the importer's own table row of that label. Example: "pnch band amount 2" is the Punch band amount of band 2,
// "Aux: tbal amount" the Tone Balance amount of set B.
using namespace ps::imp;

namespace
{
using Attrs = std::vector<std::pair<std::string, std::string>>;
using ps::ModuleType;

XmlNode el (std::string tag, Attrs attrs = {}, std::vector<XmlNode> children = {})
{
    XmlNode n;
    n.tag = std::move (tag);
    n.attributes = std::move (attrs);
    n.children = std::move (children);
    return n;
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

XmlNode chainNode (const std::vector<std::string>& names)
{
    std::vector<unsigned char> b;
    for (const auto& name : names)
    {
        b.push_back (0);
        const auto n = (std::uint32_t) name.size();
        for (int i = 0; i < 4; ++i)
            b.push_back ((unsigned char) ((n >> (8 * i)) & 0xffu));
        b.insert (b.end(), name.begin(), name.end());
    }
    return el ("ExtraBytes", { { "ElementID", "Order" }, { "Data", base64 (b) } });
}

const ModuleSpec& specOf (ModuleType t)
{
    for (const auto* s : allModuleSpecs())
        if (s->type == t)
            return *s;
    throw pstest::RequireFailed {};
}

const ModuleType kFamily[] { ModuleType::ToneBalance, ModuleType::Definition, ModuleType::Smoother,
                             ModuleType::Punch,       ModuleType::LowContrast, ModuleType::BassShaper };

std::string prefixOf (ModuleType t) { return ps::moduleInfo (t).idPrefix; }

// Our generic name of a control: "<prefix> <label>" (+ " #" when numbered).
std::string templateName (ModuleType t, const Rule& r)
{
    return prefixOf (t) + " " + r.what + (r.count > 0 ? " #" : "");
}

const Aliases& aliases()
{
    static const Aliases a = []
    {
        Aliases m;
        m["Common"] = kGlobalSectionKey;
        m["Order"] = kChainBlobKey;
        for (const auto t : kFamily)
        {
            const auto& spec = specOf (t);
            m["Sec " + prefixOf (t)] = spec.tagKey;
            m["Chain " + prefixOf (t)] = spec.elementKey;
            for (const auto& r : *spec.rules)
                m[templateName (t, r)] = r.key;
        }
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

// One stored control: "<prefix> <label>" with an optional number and set B.
struct P
{
    std::string label;
    std::string value;
    int number = -1;
    bool setB = false;
};

XmlNode section (ModuleType t, const std::vector<P>& params, bool enabled = true)
{
    std::vector<XmlNode> children;
    const std::string chainName = "Chain " + prefixOf (t);
    for (const auto& p : params)
    {
        std::string id = (p.setB ? "Aux: " : "") + prefixOf (t) + " " + p.label;
        if (p.number >= 0)
            id += " " + std::to_string (p.number);
        children.push_back (el ("Param", { { "ElementID", chainName }, { "ParamID", id }, { "Value", p.value } }));
    }
    return el ("Sec " + prefixOf (t), { { "Enabled", enabled ? "1" : "0" } }, std::move (children));
}

// A single-module file of module t from writer `version`.
ImportPlan moduleFile (ModuleType t, const std::string& version, const std::vector<P>& params)
{
    const auto root = el ("Root", { { "PresetVer", "1" }, { "PluginVer", version } },
                          { el ("Common", { { "Enabled", "0" } }, { chainNode ({ "Chain " + prefixOf (t) }) }),
                            section (t, params) });
    return buildImportPlan (root, opts());
}

float value (const ImportPlan& p, const std::string& id)
{
    const int i = ps::paramIndex (id);
    if (i < 0 || i >= (int) p.values.size())
    {
        pstest::reportFailure (__FILE__, __LINE__, "unknown id " + id);
        return -1.0e9f;
    }
    return p.values[(size_t) i];
}

bool near (float a, double b, double tol = 1.0e-3) { return std::abs ((double) a - b) < tol; }

int countReason (const ImportPlan& p, const std::string& reason)
{
    int n = 0;
    for (const auto& u : p.report.unmappedList)
        n += u.reason == reason ? 1 : 0;
    return n;
}

bool hasNoteContaining (const ImportPlan& p, const std::string& text)
{
    for (const auto& n : p.report.notes)
        if (n.find (text) != std::string::npos)
            return true;
    return false;
}
} // namespace

//==============================================================================
TEST_CASE ("import.p2. a section storing every control leaves nothing unknown or unused")
{
    for (const auto t : kFamily)
    {
        std::vector<P> params;
        for (const auto& r : *specOf (t).rules)
        {
            // Value 1 is in range for every control (and a bypass of 1 / 0 alike); numbered controls store each number.
            const int numbers = r.count > 0 ? r.count : 1;
            for (int i = 0; i < numbers; ++i)
                params.push_back ({ r.what, "1", r.count > 0 ? r.first + i : -1, false });
        }
        const auto plan = moduleFile (t, "120000", params);
        REQUIRE (plan.ok);
        CHECK_EQ (plan.scope, (int) t);
        CHECK_EQ (countReason (plan, "unknown"), 0);
        CHECK_EQ (countReason (plan, "unused"), 0);
        CHECK_EQ (countReason (plan, "number"), 0);
        CHECK_EQ (countReason (plan, "target id"), 0);
        for (const auto& u : plan.report.unmappedList)
            CHECK (u.reason == "unsupported" || u.reason == "set B" || u.reason == "value");
    }
}

TEST_CASE ("import.p2. every module imports its foreign defaults from an empty section")
{
    const auto tbal = moduleFile (ModuleType::ToneBalance, "120000", {});
    REQUIRE (tbal.ok);
    CHECK (near (value (tbal, "tbal.target"), 0.0));
    CHECK (near (value (tbal, "tbal.mode"), 0.0));
    for (const char* s : { "a", "b" })
    {
        const std::string p = std::string ("tbal.") + s + ".";
        CHECK (near (value (tbal, p + "amount"), 0.0));
        CHECK (near (value (tbal, p + "speed"), 50.0));
        CHECK (near (value (tbal, p + "smooth"), 50.0));
        CHECK (near (value (tbal, p + "cutSpeed"), 100.0));
        CHECK (near (value (tbal, p + "sens"), 30.0));
        CHECK (near (value (tbal, p + "low"), 100.0));
        CHECK (near (value (tbal, p + "mid"), 100.0));
        CHECK (near (value (tbal, p + "high"), 100.0));
        CHECK (near (value (tbal, p + "tame"), 0.0));
        CHECK (near (value (tbal, std::string ("tbal.") + s + ".on"), 1.0));
    }
    CHECK_EQ (tbal.report.mapped, 0);
    CHECK_EQ (tbal.report.unmapped, 0);
    CHECK (tbal.report.defaulted > 0);

    const auto defn = moduleFile (ModuleType::Definition, "120000", {});
    REQUIRE (defn.ok);
    CHECK (near (value (defn, "defn.a.amount"), 0.0));
    CHECK (near (value (defn, "defn.a.tilt"), 0.0));
    CHECK (near (value (defn, "defn.a.low"), 300.0));
    CHECK (near (value (defn, "defn.b.high"), 20000.0));
    CHECK (near (value (defn, "defn.a.att"), 100.0));   // confirmed foreign defaults
    CHECK (near (value (defn, "defn.b.rel"), 100.0));

    const auto smth = moduleFile (ModuleType::Smoother, "120000", {});
    REQUIRE (smth.ok);
    CHECK (near (value (smth, "smth.a.amount"), 0.0));
    CHECK (near (value (smth, "smth.a.mode"), 1.0));    // medium
    CHECK (near (value (smth, "smth.b.mode"), 1.0));
    CHECK (near (value (smth, "smth.a.tone"), 0.0));
    CHECK (near (value (smth, "smth.a.att"), 1.0));
    CHECK (near (value (smth, "smth.b.rel"), 30.0));
    CHECK (near (value (smth, "smth.a.low"), 2000.0, 0.5));
    CHECK (near (value (smth, "smth.b.high"), 8000.0, 1.0));

    const auto pnch = moduleFile (ModuleType::Punch, "120000", {});
    REQUIRE (pnch.ok);
    CHECK (near (value (pnch, "pnch.bands"), 4.0));
    CHECK (near (value (pnch, "pnch.xover1"), 160.0, 0.1));
    CHECK (near (value (pnch, "pnch.xover2"), 1000.0, 0.5));
    CHECK (near (value (pnch, "pnch.xover3"), 6000.0, 2.0));
    CHECK (near (value (pnch, "pnch.stLink"), 0.0));     // stereo link / band link off (ours, the same as theirs)
    CHECK (near (value (pnch, "pnch.bandLink"), 0.0));
    CHECK (near (value (pnch, "pnch.amount"), 100.0));
    CHECK (near (value (pnch, "pnch.env"), 100.0));
    CHECK (near (value (pnch, "pnch.sync"), 1.0));
    CHECK (near (value (pnch, "pnch.syncNote"), 7.0));   // 1/16
    CHECK (near (value (pnch, "pnch.autoGain"), 0.0));
    for (int b = 1; b <= 4; ++b)
    {
        CHECK (near (value (pnch, "pnch.a.b" + std::to_string (b) + ".amount"), 0.0));
        CHECK (near (value (pnch, "pnch.a.b" + std::to_string (b) + ".on"), 1.0));
        CHECK (near (value (pnch, "pnch.b.b" + std::to_string (b) + ".on"), 1.0));
    }

    const auto lowc = moduleFile (ModuleType::LowContrast, "120000", {});
    REQUIRE (lowc.ok);
    CHECK (near (value (lowc, "lowc.low"), 20.0));
    CHECK (near (value (lowc, "lowc.high"), 250.0));
    CHECK (near (value (lowc, "lowc.a.mode"), 0.0));
    CHECK (near (value (lowc, "lowc.a.contrast"), 0.0));
    CHECK (near (value (lowc, "lowc.b.gain"), 0.0));

    const auto bass = moduleFile (ModuleType::BassShaper, "120000", {});
    REQUIRE (bass.ok);
    CHECK (near (value (bass, "bass.cutoff"), 120.0));
    CHECK (near (value (bass, "bass.balance"), 0.0));
    CHECK (near (value (bass, "bass.punch"), 0.0));
    CHECK (near (value (bass, "bass.sus.on"), 1.0));      // every section on by default, at inert settings
    CHECK (near (value (bass, "bass.sus.thr"), 0.0));
    CHECK (near (value (bass, "bass.sus.amount"), 0.0));
    CHECK (near (value (bass, "bass.sus.char"), 50.0));
    CHECK (near (value (bass, "bass.peak.on"), 1.0));
    CHECK (near (value (bass, "bass.peak.thr"), 0.0));
    CHECK (near (value (bass, "bass.peak.char"), 50.0));
    CHECK_EQ (bass.report.unmapped, 0);
}

TEST_CASE ("import.p2. tone balance: target and mode tables, set B, power, channel mode")
{
    // Every target index 0..34 maps to ours; 35 and fractions cannot be represented (our default kept, reported).
    for (int i = 0; i <= 34; ++i)
    {
        const auto p = moduleFile (ModuleType::ToneBalance, "120000", { { "target curve", std::to_string (i) } });
        REQUIRE (p.ok);
        CHECK (near (value (p, "tbal.target"), (double) i));
        CHECK_EQ (p.report.unmapped, 0);
    }
    for (const char* bad : { "35", "2.5", "-1" })
    {
        const auto p = moduleFile (ModuleType::ToneBalance, "120000", { { "target curve", bad } });
        REQUIRE (p.ok);
        CHECK (near (value (p, "tbal.target"), 0.0));
        CHECK_EQ (p.report.unmapped, 1);
    }

    for (int i = 0; i <= 2; ++i)
    {
        const auto p = moduleFile (ModuleType::ToneBalance, "120000", { { "correction mode", std::to_string (i) } });
        CHECK (near (value (p, "tbal.mode"), (double) i));   // shape, cut, boost
    }
    CHECK_EQ (moduleFile (ModuleType::ToneBalance, "120000", { { "correction mode", "3" } }).report.unmapped, 1);

    const auto p = moduleFile (ModuleType::ToneBalance, "120000",
                               { { "amount", "40" },
                                 { "amount", "65.5", -1, true },
                                 { "low amount", "0", -1, true },
                                 { "tame transients", "1" },
                                 { "set B bypass", "1" },
                                 { "channel mode", "2" },
                                 { "module auto gain", "1" },
                                 { "selected set", "1" } });
    REQUIRE (p.ok);
    CHECK (near (value (p, "tbal.a.amount"), 40.0));
    CHECK (near (value (p, "tbal.b.amount"), 65.5));
    CHECK (near (value (p, "tbal.b.low"), 0.0));
    CHECK (near (value (p, "tbal.a.low"), 100.0));
    CHECK (near (value (p, "tbal.a.tame"), 1.0));
    CHECK (near (value (p, "tbal.b.tame"), 0.0));
    CHECK (near (value (p, "tbal.a.on"), 1.0));
    CHECK (near (value (p, "tbal.b.on"), 0.0));
    CHECK (near (value (p, "tbal.chMode"), 2.0));        // Transient/Sustain
    CHECK_EQ (p.report.mapped, 6);
    CHECK_EQ (countReason (p, "unsupported"), 1);         // the per-module auto gain
    CHECK_EQ (p.report.ignored, 1);                       // view state
}

TEST_CASE ("import.p2. tone balance: older writers drive the cut speed with the one speed control")
{
    const std::vector<P> stored { { "speed", "70" }, { "speed", "20", -1, true } };

    const auto older = moduleFile (ModuleType::ToneBalance, "100000", stored);
    REQUIRE (older.ok);
    CHECK (near (value (older, "tbal.a.speed"), 70.0));
    CHECK (near (value (older, "tbal.a.cutSpeed"), 70.0));
    CHECK (near (value (older, "tbal.b.cutSpeed"), 20.0));

    const auto newer = moduleFile (ModuleType::ToneBalance, "110000", stored);
    REQUIRE (newer.ok);
    CHECK (near (value (newer, "tbal.a.speed"), 70.0));
    CHECK (near (value (newer, "tbal.a.cutSpeed"), 100.0));   // its own control, at the default

    const auto own = moduleFile (ModuleType::ToneBalance, "110000", { { "cut speed", "88.8" } });
    CHECK (near (value (own, "tbal.a.cutSpeed"), 88.8));
    CHECK (near (value (own, "tbal.b.cutSpeed"), 100.0));
    CHECK_EQ (own.report.mapped, 1);
    CHECK_EQ (own.report.unmapped, 0);
}

TEST_CASE ("import.p2. tone balance writer era determines which cut speed controls are active")
{
    const std::vector<P> stored { { "speed", "72" }, { "cut speed", "19" },
                                  { "cut speed", "31", -1, true } };
    const auto older = moduleFile (ModuleType::ToneBalance, "109999", stored);
    REQUIRE (older.ok);
    CHECK (near (value (older, "tbal.a.cutSpeed"), 72.0));
    CHECK (near (value (older, "tbal.b.cutSpeed"), 50.0));   // the older shared-speed foreign default
    CHECK_EQ (older.report.mapped, 1);
    CHECK_EQ (older.report.ignored, 2);
    CHECK_EQ (older.report.unmapped, 0);

    for (const char* version : { "110000", "120000", "0" })
    {
        const auto newer = moduleFile (ModuleType::ToneBalance, version, stored);
        REQUIRE (newer.ok);
        CHECK (near (value (newer, "tbal.a.cutSpeed"), 19.0));
        CHECK (near (value (newer, "tbal.b.cutSpeed"), 31.0));
        CHECK_EQ (newer.report.mapped, 3);
        CHECK_EQ (newer.report.ignored, 0);
        CHECK_EQ (newer.report.unmapped, 0);
    }
}

TEST_CASE ("import.p2. definition: values per set, region, attack and release, channel modes")
{
    const auto p = moduleFile (ModuleType::Definition, "110000",
                               { { "amount", "62.4" },
                                 { "tilt", "2.0168" },
                                 { "attack", "80" },
                                 { "release", "76" },
                                 { "region start", "1010.25" },
                                 { "region end", "7014.57" },
                                 { "tilt", "-6", -1, true },
                                 { "attack", "2000", -1, true },   // above our range: clamped, counted
                                 { "channel mode", "1" } });
    REQUIRE (p.ok);
    CHECK (near (value (p, "defn.a.amount"), 62.4));
    CHECK (near (value (p, "defn.a.tilt"), 2.0168));
    CHECK (near (value (p, "defn.a.att"), 80.0));
    CHECK (near (value (p, "defn.a.rel"), 76.0));
    CHECK (near (value (p, "defn.a.low"), 1010.25, 0.01));
    CHECK (near (value (p, "defn.a.high"), 7014.57, 0.05));
    CHECK (near (value (p, "defn.b.tilt"), -6.0));
    CHECK (near (value (p, "defn.b.att"), 1000.0));
    CHECK (near (value (p, "defn.b.low"), 300.0));
    CHECK (near (value (p, "defn.chMode"), 1.0));
    CHECK_EQ (p.report.clamped, 1);
    CHECK_EQ (p.report.mapped, 9);

    // Channel-mode indices of the three spectral modules and Low Contrast: 0 Stereo, 1 Mid/Side, 2 Transient/Sustain.
    for (const auto t : { ModuleType::ToneBalance, ModuleType::Definition, ModuleType::Smoother,
                          ModuleType::LowContrast })
        for (int i = 0; i <= 2; ++i)
        {
            const auto q = moduleFile (t, "120000", { { "channel mode", std::to_string (i) } });
            const auto modes = ps::supportedChannelModes (t);
            REQUIRE (modes.size() == 3);
            CHECK (near (value (q, prefixOf (t) + ".chMode"), (double) i));
            CHECK (modes[(size_t) i] == (i == 0 ? ps::ChannelMode::Stereo
                                                : i == 1 ? ps::ChannelMode::MidSide : ps::ChannelMode::TransientSustain));
        }
}

TEST_CASE ("import.p2. smoother: amount, the older threshold heuristic and the style table")
{
    // Older writer: a threshold instead of an amount (converted, noted); absent set B threshold = never acts.
    const auto older = moduleFile (ModuleType::Smoother, "9030", { { "threshold (older files)", "-20" },
                                                                    { "region start", "2500" },
                                                                    { "region end", "20000" } });
    REQUIRE (older.ok);
    CHECK (near (value (older, "smth.a.amount"), 55.5));   // T = -64.5 + (100 - amount)
    CHECK (near (value (older, "smth.b.amount"), 0.0));
    CHECK (near (value (older, "smth.a.low"), 2500.0));
    CHECK (near (value (older, "smth.a.high"), 20000.0));
    CHECK (hasNoteContaining (older, "threshold converted"));
    CHECK_EQ (older.report.unmapped, 0);

    const double cases[][2] { { -3.0, 38.5 }, { -45.0, 80.5 }, { -80.0, 100.0 }, { -0.5, 0.0 }, { 6.0, 0.0 } };
    for (const auto& c : cases)
    {
        const auto q = moduleFile (ModuleType::Smoother, "100000",
                                   { { "threshold (older files)", std::to_string (c[0]), -1, true } });
        CHECK (near (value (q, "smth.b.amount"), c[1]));
        CHECK (near (value (q, "smth.a.amount"), 0.0));
    }

    // Newer writer: the amount; a stored threshold next to it is stale (ignored, not reported).
    const auto newer = moduleFile (ModuleType::Smoother, "110000", { { "amount", "44.4" },
                                                                      { "threshold (older files)", "-30" } });
    REQUIRE (newer.ok);
    CHECK (near (value (newer, "smth.a.amount"), 44.4));
    CHECK_EQ (newer.report.ignored, 1);
    CHECK_EQ (newer.report.unmapped, 0);
    CHECK (! hasNoteContaining (newer, "threshold"));

    // A newer sparse amount is still the writer's amount default; a leftover threshold cannot activate it.
    const auto sparseNewer = moduleFile (ModuleType::Smoother, "110000",
                                         { { "threshold (older files)", "-30" },
                                           { "threshold (older files)", "-20", -1, true } });
    REQUIRE (sparseNewer.ok);
    CHECK (near (value (sparseNewer, "smth.a.amount"), 0.0));
    CHECK (near (value (sparseNewer, "smth.b.amount"), 0.0));
    CHECK_EQ (sparseNewer.report.ignored, 2);
    CHECK_EQ (sparseNewer.report.mapped, 0);
    CHECK_EQ (sparseNewer.report.unmapped, 0);
    CHECK (! hasNoteContaining (sparseNewer, "threshold"));

    // Without writer metadata, the stored control remains the only available era evidence.
    const auto unknownWriter = moduleFile (ModuleType::Smoother, "0",
                                           { { "threshold (older files)", "-20", -1, true } });
    CHECK (near (value (unknownWriter, "smth.b.amount"), 55.5));

    // Reduction style: light / medium / heavy -> Gentle / Medium / Strong; 3 is not imported.
    for (int i = 0; i <= 2; ++i)
    {
        const auto q = moduleFile (ModuleType::Smoother, "110000", { { "reduction style", std::to_string (i), -1, true } });
        CHECK (near (value (q, "smth.b.mode"), (double) i));
        CHECK (near (value (q, "smth.a.mode"), 1.0));
    }
    const auto bad = moduleFile (ModuleType::Smoother, "110000", { { "reduction style", "3" } });
    CHECK (near (value (bad, "smth.a.mode"), 1.0));
    CHECK_EQ (bad.report.unmapped, 1);
}

TEST_CASE ("import.p2. punch: band layout with a gap, crossovers, band bypass in both sets")
{
    // Slots 0, 2, 3 present (slot 1 removed): our bands 1..3; the cutoff at slot 1's lower edge is stale.
    const auto p = moduleFile (ModuleType::Punch, "110000",
                               { { "band present", "0", 1 },
                                 { "crossover frequency", "150", 1 },
                                 { "crossover frequency", "900", 2 },
                                 { "crossover frequency", "5000", 3 },
                                 { "band amount", "25", 1 },
                                 { "band amount", "-30", 2 },    // removed slot: stale
                                 { "band amount", "40", 3 },
                                 { "band amount", "-12.5", 4, true },
                                 { "band bypass", "1", 3 },
                                 { "band bypass", "1", 1 } });   // removed slot: stale
    REQUIRE (p.ok);
    CHECK (near (value (p, "pnch.bands"), 3.0));
    CHECK (near (value (p, "pnch.xover1"), 900.0));
    CHECK (near (value (p, "pnch.xover2"), 5000.0));
    CHECK (near (value (p, "pnch.a.b1.amount"), 25.0));
    CHECK (near (value (p, "pnch.a.b2.amount"), 40.0));
    CHECK (near (value (p, "pnch.b.b3.amount"), -12.5));
    CHECK (near (value (p, "pnch.a.b3.amount"), 0.0));
    CHECK (near (value (p, "pnch.a.b3.on"), 0.0));
    CHECK (near (value (p, "pnch.b.b3.on"), 0.0));
    CHECK (near (value (p, "pnch.a.b2.on"), 1.0));
    CHECK_EQ (p.report.ignored, 3);   // stale cutoff 1, band amount 2, band bypass 1
    CHECK_EQ (p.report.unmapped, 0);

    // Two bands: only crossover 1 is used; stored edges above are parked values.
    const auto two = moduleFile (ModuleType::Punch, "100000", { { "band present", "0", 2 },
                                                                 { "band present", "0", 3 },
                                                                 { "crossover frequency", "127", 1 },
                                                                 { "crossover frequency", "6000", 2 } });
    CHECK (near (value (two, "pnch.bands"), 2.0));
    CHECK (near (value (two, "pnch.xover1"), 127.0));
    CHECK_EQ (two.report.ignored, 1);

    // A sparse four-band file relies on the foreign cutoff defaults for the edges it does not store.
    const auto sparse = moduleFile (ModuleType::Punch, "100000", { { "crossover frequency", "230", 1 } });
    CHECK (near (value (sparse, "pnch.xover1"), 230.0));
    CHECK (near (value (sparse, "pnch.xover2"), 1000.0, 0.5));
    CHECK (near (value (sparse, "pnch.xover3"), 6000.0, 2.0));
}

TEST_CASE ("import.p2. punch: sync note table, shared per-set controls, auto gain switches")
{
    for (int i = 0; i <= 14; ++i)
    {
        const auto p = moduleFile (ModuleType::Punch, "120000", { { "sync note", std::to_string (i) } });
        CHECK (near (value (p, "pnch.syncNote"), (double) (i + 3)));
        CHECK (! hasNoteContaining (p, "longer than"));
    }
    for (int i = 15; i <= 17; ++i)
    {
        const auto p = moduleFile (ModuleType::Punch, "120000", { { "sync note", std::to_string (i) } });
        CHECK (near (value (p, "pnch.syncNote"), 17.0));   // 1/2 dotted, noted
        CHECK (hasNoteContaining (p, "sync note"));
    }
    const auto bad = moduleFile (ModuleType::Punch, "120000", { { "sync note", "18" } });
    CHECK (near (value (bad, "pnch.syncNote"), 7.0));       // not representable: the foreign default
    CHECK_EQ (countReason (bad, "value"), 1);

    // Envelope / sync / amount are shared here: set A wins, an equal set B imports, a different one is reported.
    const auto p = moduleFile (ModuleType::Punch, "120000", { { "envelope", "80" },
                                                               { "envelope", "80", -1, true },
                                                               { "envelope sync", "0" },
                                                               { "module amount", "60" },
                                                               { "module amount", "30", -1, true } });
    REQUIRE (p.ok);
    CHECK (near (value (p, "pnch.env"), 80.0));
    CHECK (near (value (p, "pnch.sync"), 0.0));
    CHECK (near (value (p, "pnch.amount"), 60.0));
    CHECK_EQ (p.report.mapped, 4);
    CHECK_EQ (countReason (p, "set B"), 1);
    CHECK (hasNoteContaining (p, "set B amount"));

    // Either stored make-up switch turns our auto gain on.
    CHECK (near (value (moduleFile (ModuleType::Punch, "100000", { { "auto gain", "1" } }), "pnch.autoGain"), 1.0));
    CHECK (near (value (moduleFile (ModuleType::Punch, "110000", { { "auto gain (module switch)", "1" } }),
                        "pnch.autoGain"), 1.0));

    // Mid/Side is the second Punch mode; a third index is out of range (Stereo, counted).
    CHECK (near (value (moduleFile (ModuleType::Punch, "110000", { { "channel mode", "1" } }), "pnch.chMode"), 1.0));
    const auto out = moduleFile (ModuleType::Punch, "110000", { { "channel mode", "2" } });
    CHECK (near (value (out, "pnch.chMode"), 0.0));
    CHECK_EQ (out.report.clamped, 1);
}

TEST_CASE ("import.p2. low contrast: character table, shared region, mix")
{
    const auto p = moduleFile (ModuleType::LowContrast, "9030", { { "contrast", "-70.19" },
                                                                   { "region gain", "0.81" },
                                                                   { "character", "1" },
                                                                   { "character", "1", -1, true },
                                                                   { "region start", "59.5" },
                                                                   { "region end", "218.4" },
                                                                   { "contrast", "41.1", -1, true } });
    REQUIRE (p.ok);
    CHECK (near (value (p, "lowc.a.contrast"), 0.4 * -70.19));   // negative strengths: 0.4 x
    CHECK (near (value (p, "lowc.a.gain"), 0.81));
    CHECK (near (value (p, "lowc.a.mode"), 1.0));
    CHECK (near (value (p, "lowc.b.mode"), 1.0));
    CHECK (near (value (p, "lowc.b.contrast"), 41.1));
    CHECK (near (value (p, "lowc.low"), 59.5));
    CHECK (near (value (p, "lowc.high"), 218.4));
    CHECK_EQ (p.report.mapped, 7);

    const auto bad = moduleFile (ModuleType::LowContrast, "110000", { { "character", "2" } });
    CHECK (near (value (bad, "lowc.a.mode"), 0.0));
    CHECK_EQ (bad.report.unmapped, 1);

    // The region is shared here: a different set B region is reported.
    const auto region = moduleFile (ModuleType::LowContrast, "110000", { { "region end", "120" },
                                                                          { "region end", "180", -1, true } });
    CHECK (near (value (region, "lowc.high"), 120.0));
    CHECK_EQ (countReason (region, "set B"), 1);

    // No wet / dry here: a full mix is fine, anything else is reported with a note.
    const auto full = moduleFile (ModuleType::LowContrast, "110000", { { "mix", "100" } });
    CHECK_EQ (full.report.unmapped, 0);
    CHECK_EQ (full.report.mapped, 1);
    const auto half = moduleFile (ModuleType::LowContrast, "110000", { { "mix", "50" } });
    CHECK_EQ (countReason (half, "unsupported"), 1);
    CHECK (hasNoteContaining (half, "mix"));

    const auto gain = moduleFile (ModuleType::LowContrast, "110000", { { "auto gain", "1" } });
    CHECK (near (value (gain, "lowc.autoGain"), 1.0));
}

TEST_CASE ("import.p2. shared controls compare the effective second set including sparse defaults")
{
    const auto envelope = moduleFile (ModuleType::Punch, "120000",
                                      { { "channel mode", "1" }, { "envelope", "80" } });
    REQUIRE (envelope.ok);
    CHECK (near (value (envelope, "pnch.env"), 80.0));
    CHECK (hasNoteContaining (envelope, "set B envelope"));
    CHECK_EQ (envelope.report.unmapped, 0);   // an absent value is not a stored setting

    const auto region = moduleFile (ModuleType::LowContrast, "120000",
                                    { { "channel mode", "1" }, { "region end", "120" } });
    REQUIRE (region.ok);
    CHECK (near (value (region, "lowc.high"), 120.0));
    CHECK (hasNoteContaining (region, "set B region"));
    CHECK_EQ (region.report.unmapped, 0);

    // In Stereo the foreign set B is inaudible: its sparse default differing from set A is no loss, no note.
    const auto stereo = moduleFile (ModuleType::LowContrast, "120000", { { "region end", "120" } });
    REQUIRE (stereo.ok);
    CHECK (near (value (stereo, "lowc.high"), 120.0));
    CHECK (! hasNoteContaining (stereo, "set B region"));
    const auto stereoPunch = moduleFile (ModuleType::Punch, "120000", { { "envelope", "80" } });
    REQUIRE (stereoPunch.ok);
    CHECK (! hasNoteContaining (stereoPunch, "set B envelope"));

    // Both booleans resolve to on; distinct raw values still represent the same control setting.
    const auto equal = moduleFile (ModuleType::Punch, "120000",
                                   { { "envelope sync", "1" }, { "envelope sync", "0.75", -1, true } });
    REQUIRE (equal.ok);
    CHECK_EQ (equal.report.mapped, 2);
    CHECK_EQ (equal.report.unmapped, 0);
    CHECK (! hasNoteContaining (equal, "set B sync"));

    // An invalid shared enum must not be counted as imported merely because both raw values agree.
    const auto invalid = moduleFile (ModuleType::Punch, "120000",
                                     { { "sync note", "18" }, { "sync note", "18", -1, true } });
    REQUIRE (invalid.ok);
    CHECK (near (value (invalid, "pnch.syncNote"), 7.0));
    CHECK_EQ (invalid.report.mapped, 0);
    CHECK_EQ (countReason (invalid, "value"), 2);
}

TEST_CASE ("import.p2. punch layout always uses presence flags without a band count control")
{
    for (const char* version : { "0", "1", "8999", "9000", "120000" })
    {
        const auto p = moduleFile (ModuleType::Punch, version,
                                   { { "band present", "0", 1 }, { "band amount", "42", 3, true } });
        REQUIRE (p.ok);
        CHECK (near (value (p, "pnch.bands"), 3.0));
        CHECK (near (value (p, "pnch.b.b2.amount"), 42.0));
        CHECK (near (value (p, "pnch.xover1"), 1000.0, 0.5));
        CHECK_EQ (p.report.unmapped, 0);
    }
}

TEST_CASE ("import.p2. bass shaper: sections follow their power (on by default); character scale")
{
    const auto p = moduleFile (ModuleType::BassShaper, "120000", { { "cutoff", "140.4" },
                                                                    { "balance", "0.96" },
                                                                    { "punch", "2.016" },
                                                                    { "sustain threshold", "-9.09" },
                                                                    { "sustain amount", "34.8" },
                                                                    { "sustain character", "48.1" },
                                                                    { "peak threshold", "-3.72" } });
    REQUIRE (p.ok);
    CHECK (near (value (p, "bass.cutoff"), 140.4, 0.01));
    CHECK (near (value (p, "bass.balance"), 0.96));
    CHECK (near (value (p, "bass.punch"), 2.016));
    CHECK (near (value (p, "bass.sus.on"), 1.0));
    CHECK (near (value (p, "bass.sus.thr"), -9.09));
    CHECK (near (value (p, "bass.sus.amount"), 34.8));
    CHECK (near (value (p, "bass.sus.char"), 48.1));
    CHECK (near (value (p, "bass.peak.on"), 1.0));
    CHECK (near (value (p, "bass.peak.thr"), -3.72));
    CHECK (near (value (p, "bass.peak.char"), 50.0));   // foreign default 3 of 0..6
    CHECK_EQ (p.report.mapped, 7);
    CHECK_EQ (p.report.unmapped, 0);

    const double chars[][2] { { 0.0, 0.0 }, { 6.0, 100.0 }, { 2.4507, 40.845 }, { 9.0, 100.0 } };
    for (const auto& c : chars)
    {
        const auto q = moduleFile (ModuleType::BassShaper, "120000", { { "peak character", std::to_string (c[0]) } });
        CHECK (near (value (q, "bass.peak.char"), c[1], 0.01));
        CHECK (near (value (q, "bass.peak.on"), 1.0));
        CHECK (near (value (q, "bass.sus.on"), 1.0));
    }

    // A section powered off: levels read 0 dB (the stored value is stale), lift / limiter off.
    const auto off = moduleFile (ModuleType::BassShaper, "120000", { { "balance", "3" },
                                                                      { "balance power", "0" },
                                                                      { "sustain amount", "50" },
                                                                      { "sustain power", "0" },
                                                                      { "peak threshold", "-2" },
                                                                      { "peak power", "0" } });
    REQUIRE (off.ok);
    CHECK (near (value (off, "bass.balance"), 0.0));
    CHECK (near (value (off, "bass.sus.on"), 0.0));
    CHECK (near (value (off, "bass.peak.on"), 0.0));
    CHECK_EQ (off.report.unmapped, 0);
    CHECK_EQ (off.report.ignored, 1);
}

TEST_CASE ("import.p2. bass shaper explicit power switches sections with sparse controls")
{
    for (const char* power : { "0", "1" })
    {
        const auto p = moduleFile (ModuleType::BassShaper, "120000",
                                   { { "sustain power", power }, { "peak power", power } });
        REQUIRE (p.ok);
        const double expected = power[0] == '1' ? 1.0 : 0.0;
        CHECK (near (value (p, "bass.sus.on"), expected));
        CHECK (near (value (p, "bass.peak.on"), expected));
        CHECK_EQ (p.report.mapped, 2);
        CHECK_EQ (p.report.unmapped, 0);
    }
}

TEST_CASE ("import.p2. whole chain: family modules in the stored order, stale sections ignored")
{
    const auto root = el ("Root", { { "PresetVer", "4" }, { "PluginVer", "110000" } },
                          { el ("Common", { { "Enabled", "1" } }, { chainNode ({ "Chain pnch", "Chain tbal", "Chain lowc" }) }),
                            section (ModuleType::ToneBalance, { { "amount", "35" } }),
                            section (ModuleType::Punch, { { "band amount", "20", 1 } }),
                            section (ModuleType::LowContrast, { { "contrast", "50" } }, false),
                            section (ModuleType::Smoother, { { "amount", "70" } }) });   // not in the chain: stale
    const auto plan = buildImportPlan (root, opts());
    REQUIRE (plan.ok);
    CHECK (plan.report.globalFile);
    REQUIRE (plan.order.size() == 3);
    CHECK (plan.order[0] == ModuleType::Punch);
    CHECK (plan.order[1] == ModuleType::ToneBalance);
    CHECK (plan.order[2] == ModuleType::LowContrast);
    CHECK (near (value (plan, "tbal.a.amount"), 35.0));
    CHECK (near (value (plan, "pnch.a.b1.amount"), 20.0));
    CHECK (near (value (plan, "pnch.bands"), 4.0));
    CHECK (near (value (plan, "lowc.a.contrast"), 50.0));
    CHECK (near (value (plan, "lowc.on"), 1.0));          // chain flag is on; section Enabled is only a load mask
    CHECK (near (value (plan, "smth.a.amount"), 0.0));    // never read
    CHECK_EQ (plan.report.mapped, 3);
}
