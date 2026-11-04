#include "TestFramework.h"

#include "plugin/import/ImportEngine.h"

#include <cmath>
#include <cstdint>
#include <string>

// Preset importer, vintage-style family tables (Classic Compressor, Passive EQ, Classic Limiter, Tape) on SYNTHETIC
// trees written by us. Recognition works by key, so every section and control gets a generic name of our own through
// ImportOptions::aliases: "<our prefix> <our label>", with the key taken from the importer's own table row of that
// label. Example: "Aux: ccmp ratio" is the Classic Compressor ratio of set B.
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

const ModuleType kFamily[] { ModuleType::ClassicComp, ModuleType::PassiveEq, ModuleType::ClassicLimiter, ModuleType::Tape,
                             ModuleType::DeLimiter };

std::string prefixOf (ModuleType t) { return ps::moduleInfo (t).idPrefix; }

const Aliases& aliases()
{
    static const Aliases a = []
    {
        Aliases m;
        m["Common"] = kGlobalSectionKey;
        m["Order"] = kChainBlobKey;
        m["Power Off"] = kSectionBypassKey;
        for (const auto t : kFamily)
        {
            const auto& spec = specOf (t);
            m["Sec " + prefixOf (t)] = spec.tagKey;
            m["Chain " + prefixOf (t)] = spec.elementKey;
            for (const auto& r : *spec.rules)
                m[prefixOf (t) + " " + r.what] = r.key;
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

// One stored control: "<prefix> <label>", optionally set B. A label starting with '=' is stored as written.
struct P
{
    std::string label;
    std::string value;
    bool setB = false;
};

XmlNode section (ModuleType t, const std::vector<P>& params, bool enabled = true)
{
    std::vector<XmlNode> children;
    const std::string chainName = "Chain " + prefixOf (t);
    for (const auto& p : params)
    {
        const std::string name = p.label[0] == '=' ? p.label.substr (1) : prefixOf (t) + " " + p.label;
        const std::string id = (p.setB ? "Aux: " : "") + name;
        children.push_back (el ("Param", { { "ElementID", chainName }, { "ParamID", id }, { "Value", p.value } }));
    }
    return el ("Sec " + prefixOf (t), { { "Enabled", enabled ? "1" : "0" } }, std::move (children));
}

// A single-module file of module t from writer `version`.
ImportPlan moduleFile (ModuleType t, const std::vector<P>& params, const std::string& version = "120000")
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
} // namespace

//==============================================================================
TEST_CASE ("import.vint. a section storing every control leaves nothing unknown or unused")
{
    for (const auto t : kFamily)
    {
        std::vector<P> params;
        for (const auto& r : *specOf (t).rules)
        {
            params.push_back ({ r.what, "1" });   // 1 is in range (and a valid index) for every control
            if (r.idB != nullptr)
                params.push_back ({ r.what, "1", true });
        }
        const auto plan = moduleFile (t, params);
        REQUIRE (plan.ok);
        CHECK_EQ (plan.scope, (int) t);
        CHECK_EQ (plan.report.unmapped, 0);
        CHECK_EQ (countReason (plan, "unknown"), 0);
        CHECK (plan.report.mapped > 0);
        CHECK (skippedModuleDescription (specOf (t).tagKey) == nullptr);   // no longer a module we do not have
    }
}

TEST_CASE ("import.vint. every module imports the foreign defaults table from an empty section")
{
    const auto ccmp = moduleFile (ModuleType::ClassicComp, {});
    REQUIRE (ccmp.ok);
    for (const char* s : { "a", "b" })
    {
        const std::string p = std::string ("ccmp.") + s + ".";
        CHECK (near (value (ccmp, p + "thr"), -12.0));
        CHECK (near (value (ccmp, p + "ratio"), 1.5));
        CHECK (near (value (ccmp, p + "attack"), 20.0, 0.01));
        CHECK (near (value (ccmp, p + "release"), 50.0, 0.05));
        CHECK (near (value (ccmp, p + "mode"), 1.0));       // Balanced
        CHECK (near (value (ccmp, p + "makeup"), 0.0));
        CHECK (near (value (ccmp, p + "autoGain"), 1.0));
        CHECK (near (value (ccmp, p + "sc.hpf"), 40.0, 0.05));
        CHECK (near (value (ccmp, p + "sc.bellHz"), 2000.0, 1.0));
        CHECK (near (value (ccmp, p + "sc.bellDb"), 0.0));
        CHECK (near (value (ccmp, p + "sc.shelfHz"), 6000.0, 2.0));
        CHECK (near (value (ccmp, p + "sc.shelfDb"), 0.0));
    }
    CHECK (near (value (ccmp, "ccmp.chMode"), 0.0));
    CHECK_EQ (ccmp.report.mapped, 0);
    CHECK_EQ (ccmp.report.unmapped, 0);
    CHECK (ccmp.report.defaulted > 0);

    const auto peq = moduleFile (ModuleType::PassiveEq, {});
    REQUIRE (peq.ok);
    for (const char* s : { "a", "b" })
    {
        const std::string p = std::string ("peq.") + s + ".";
        CHECK (near (value (peq, p + "lowHz"), 2.0));       // 45 Hz
        CHECK (near (value (peq, p + "highHz"), 3.0));      // 8 kHz
        CHECK (near (value (peq, p + "highQ"), 0.0));       // broadest
        CHECK (near (value (peq, p + "highCutHz"), 1.0));   // 10 kHz
        CHECK (near (value (peq, p + "lmHz"), 2.0));        // 500 Hz
        CHECK (near (value (peq, p + "midHz"), 5.0));       // 1.5 kHz
        CHECK (near (value (peq, p + "hmHz"), 2.0));        // 3 kHz
        for (const char* amount : { "lowBoost", "lowCut", "highBoost", "highCut", "lmBoost", "midCut", "hmBoost" })
            CHECK (near (value (peq, p + amount), 0.0));
    }
    CHECK (near (value (peq, "peq.drive"), 0.0));            // no counterpart: ours
    CHECK_EQ (peq.report.unmapped, 0);

    const auto clim = moduleFile (ModuleType::ClassicLimiter, {});
    REQUIRE (clim.ok);
    CHECK (near (value (clim, "clim.thr"), -2.0));
    CHECK (near (value (clim, "clim.ceiling"), 0.0));
    CHECK (near (value (clim, "clim.character"), 2.0));
    CHECK (near (value (clim, "clim.mode"), 1.0));          // Tube
    CHECK (near (value (clim, "clim.truePeak"), 0.0));
    CHECK (near (value (clim, "clim.link"), 0.0));

    const auto tape = moduleFile (ModuleType::Tape, {});
    REQUIRE (tape.ok);
    CHECK (near (value (tape, "tape.speed"), 2.0));         // 30 ips
    CHECK (near (value (tape, "tape.drive"), 0.0));
    CHECK (near (value (tape, "tape.bias"), 0.0));
    CHECK (near (value (tape, "tape.harmonics"), 0.0));
    CHECK (near (value (tape, "tape.lowEmph"), 2.0));
    CHECK (near (value (tape, "tape.highEmph"), 4.0));
}

TEST_CASE ("import.vint. classic compressor: sets, mid/side, mode table, the same ranges, out-of-range values clamped")
{
    const auto plan = moduleFile (ModuleType::ClassicComp,
                                  { { "channel mode", "1" }, { "threshold", "-18.5" }, { "ratio", "2.4" },
                                    { "threshold", "-60", true }, { "ratio", "1.8", true }, { "mode", "2", true },
                                    { "mode", "0" }, { "attack", "0.1" }, { "release", "150" }, { "makeup", "1" },
                                    { "release", "400", true },
                                    { "makeup", "-30", true }, { "auto gain", "0" }, { "sidechain high-pass", "400" },
                                    { "sidechain bell frequency", "9000" }, { "sidechain bell gain", "-8" },
                                    { "sidechain shelf frequency", "4000" }, { "sidechain shelf gain", "6.5" },
                                    { "selected set", "1" } });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "ccmp.chMode"), 1.0));        // M/S
    CHECK (near (value (plan, "ccmp.a.thr"), -18.5));
    CHECK (near (value (plan, "ccmp.a.ratio"), 2.4));
    CHECK (near (value (plan, "ccmp.b.thr"), -60.0));       // the bottom of both ranges
    CHECK (near (value (plan, "ccmp.b.ratio"), 1.8));
    CHECK (near (value (plan, "ccmp.a.mode"), 0.0));        // Sharp
    CHECK (near (value (plan, "ccmp.b.mode"), 2.0));        // Smooth
    CHECK (near (value (plan, "ccmp.a.attack"), 0.1, 1.0e-4));
    CHECK (near (value (plan, "ccmp.a.release"), 150.0, 0.05));
    CHECK (near (value (plan, "ccmp.b.release"), 150.0, 0.05));   // clamped (outside both ranges)
    CHECK (near (value (plan, "ccmp.a.makeup"), 1.0));
    CHECK (near (value (plan, "ccmp.b.makeup"), -30.0));    // the bottom of both ranges
    CHECK (near (value (plan, "ccmp.a.autoGain"), 0.0));
    CHECK (near (value (plan, "ccmp.b.autoGain"), 1.0));    // set B keeps its default
    CHECK (near (value (plan, "ccmp.b.attack"), 20.0, 0.01));
    CHECK (near (value (plan, "ccmp.a.sc.hpf"), 400.0, 0.1));
    CHECK (near (value (plan, "ccmp.a.sc.bellHz"), 9000.0, 1.0));   // the top of both ranges
    CHECK (near (value (plan, "ccmp.a.sc.bellDb"), -8.0));
    CHECK (near (value (plan, "ccmp.a.sc.shelfHz"), 4000.0, 1.0));
    CHECK (near (value (plan, "ccmp.a.sc.shelfDb"), 6.5));
    CHECK_EQ (plan.report.clamped, 1);
    CHECK_EQ (plan.report.unmapped, 0);
    CHECK_EQ (plan.report.ignored, 1);                      // the view's selected set

    // a mode index outside the list cannot be represented: not imported, our default kept
    const auto bad = moduleFile (ModuleType::ClassicComp, { { "mode", "3" }, { "mode", "1.5", true } });
    REQUIRE (bad.ok);
    CHECK_EQ (bad.report.unmapped, 2);
    CHECK (near (value (bad, "ccmp.a.mode"), (double) ps::ClassicCompMode::Balanced));
}

TEST_CASE ("import.vint. passive EQ: frequency tables, High Q 1:1, set B, every channel mode")
{
    const auto plan = moduleFile (ModuleType::PassiveEq,
                                  { { "low frequency", "4" }, { "low boost", "5" }, { "low cut", "3" },
                                    { "high boost frequency", "6" }, { "high boost", "4" }, { "high boost Q", "4" },
                                    { "high cut frequency", "2" }, { "high cut", "2.5" }, { "low-mid frequency", "0" },
                                    { "low-mid boost", "4.5" }, { "mid frequency", "10" }, { "mid cut", "5.5" },
                                    { "high-mid frequency", "4" }, { "high-mid boost", "6" },
                                    { "low frequency", "0", true }, { "high boost", "2.5", true },
                                    { "mid frequency", "8", true } });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "peq.a.lowHz"), 4.0));
    CHECK (near (value (plan, "peq.a.lowBoost"), 5.0));
    CHECK (near (value (plan, "peq.a.lowCut"), 3.0));
    CHECK (near (value (plan, "peq.a.highHz"), 6.0));
    CHECK (near (value (plan, "peq.a.highBoost"), 4.0));
    CHECK (near (value (plan, "peq.a.highQ"), 4.0));        // the same control, the same direction
    CHECK (near (value (plan, "peq.a.highCutHz"), 2.0));
    CHECK (near (value (plan, "peq.a.highCut"), 2.5));
    CHECK (near (value (plan, "peq.a.lmHz"), 0.0));
    CHECK (near (value (plan, "peq.a.lmBoost"), 4.5));
    CHECK (near (value (plan, "peq.a.midHz"), 10.0));
    CHECK (near (value (plan, "peq.a.midCut"), 5.5));
    CHECK (near (value (plan, "peq.a.hmHz"), 4.0));
    CHECK (near (value (plan, "peq.a.hmBoost"), 6.0));
    CHECK (near (value (plan, "peq.b.lowHz"), 0.0));
    CHECK (near (value (plan, "peq.b.highBoost"), 2.5));
    CHECK (near (value (plan, "peq.b.midHz"), 8.0));
    CHECK (near (value (plan, "peq.b.lowBoost"), 0.0));     // set B keeps the foreign defaults
    CHECK (near (value (plan, "peq.b.highHz"), 3.0));
    CHECK_EQ (plan.report.unmapped, 0);

    // an index past the end of a list is not imported
    const auto bad = moduleFile (ModuleType::PassiveEq, { { "low frequency", "5" }, { "mid frequency", "11" } });
    REQUIRE (bad.ok);
    CHECK_EQ (bad.report.unmapped, 2);

    // channel modes in their order: Stereo, Mid/Side, Left/Right, Transient/Sustain
    const ps::ChannelMode modes[] { ps::ChannelMode::Stereo, ps::ChannelMode::MidSide, ps::ChannelMode::LeftRight,
                                    ps::ChannelMode::TransientSustain };
    for (int i = 0; i < 4; ++i)
    {
        const auto m = moduleFile (ModuleType::PassiveEq, { { "channel mode", std::to_string (i) } });
        REQUIRE (m.ok);
        ps::SlotParams slot;
        ps::readSlotParams (m.values.data(), ModuleType::PassiveEq, slot);
        CHECK (slot.channelMode == modes[i]);
    }
}

TEST_CASE ("import.vint. classic limiter: threshold, ceiling, character, mode table, true peak, link")
{
    const auto plan = moduleFile (ModuleType::ClassicLimiter,
                                  { { "threshold", "-5" }, { "ceiling", "-0.4" }, { "character", "8.2" },
                                    { "mode", "2" }, { "true peak", "1" }, { "link threshold and ceiling", "1" } });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "clim.thr"), -5.0));
    CHECK (near (value (plan, "clim.ceiling"), -0.4));
    CHECK (near (value (plan, "clim.character"), 8.2));
    CHECK (near (value (plan, "clim.mode"), 2.0));          // Modern
    CHECK (near (value (plan, "clim.truePeak"), 1.0));
    CHECK (near (value (plan, "clim.link"), 1.0));
    CHECK_EQ (plan.report.mapped, 6);
    CHECK_EQ (plan.report.unmapped, 0);

    // the limiter has no second set: a stored set B value is not imported
    const auto setB = moduleFile (ModuleType::ClassicLimiter, { { "threshold", "-3", true } });
    REQUIRE (setB.ok);
    CHECK_EQ (setB.report.unmapped, 1);
    CHECK (near (value (setB, "clim.thr"), -2.0));
}

TEST_CASE ("import.vint. tape: the speed index runs the other way, values 1:1, drive clamped outside both ranges")
{
    const double ours[] { 2.0, 1.0, 0.0 };   // their 30 / 15 / 7.5 ips
    for (int i = 0; i < 3; ++i)
    {
        const auto p = moduleFile (ModuleType::Tape, { { "speed", std::to_string (i) } });
        REQUIRE (p.ok);
        CHECK (near (value (p, "tape.speed"), ours[i]));
        CHECK_EQ (p.report.unmapped, 0);
    }
    const auto bad = moduleFile (ModuleType::Tape, { { "speed", "3" } });
    REQUIRE (bad.ok);
    CHECK_EQ (bad.report.unmapped, 1);

    const auto plan = moduleFile (ModuleType::Tape, { { "drive", "30" }, { "bias", "-6" }, { "harmonics", "3.2" },
                                                      { "low emphasis", "4.1" }, { "high emphasis", "5.6" } });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "tape.drive"), 30.0));        // the top of both ranges
    CHECK (near (value (plan, "tape.bias"), -6.0));
    CHECK (near (value (plan, "tape.harmonics"), 3.2));
    CHECK (near (value (plan, "tape.lowEmph"), 4.1));
    CHECK (near (value (plan, "tape.highEmph"), 5.6));
    CHECK_EQ (plan.report.clamped, 0);
    const auto hot = moduleFile (ModuleType::Tape, { { "drive", "36" } });
    REQUIRE (hot.ok);
    CHECK (near (value (hot, "tape.drive"), 30.0));         // clamped
    CHECK_EQ (hot.report.clamped, 1);
}

TEST_CASE ("import.vint. de-limiter: the values read 1:1, the defaults from an empty section, out of range clamped")
{
    const auto empty = moduleFile (ModuleType::DeLimiter, {});
    REQUIRE (empty.ok);
    REQUIRE (empty.order.size() == 1);
    CHECK_EQ (empty.order[0], ModuleType::DeLimiter);
    for (const char* id : { "dlim.amount", "dlim.input", "dlim.output", "dlim.thr" })
        CHECK (near (value (empty, id), 0.0));

    const auto plan = moduleFile (ModuleType::DeLimiter, { { "amount", "150" }, { "threshold", "-6.08164692" },
                                                           { "input gain", "6" }, { "output gain", "-6" } });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "dlim.amount"), 150.0));
    CHECK (near (value (plan, "dlim.thr"), -6.08164692));
    CHECK (near (value (plan, "dlim.input"), 6.0));
    CHECK (near (value (plan, "dlim.output"), -6.0));
    CHECK_EQ (plan.report.unmapped, 0);
    CHECK_EQ (plan.report.clamped, 0);

    const auto hot = moduleFile (ModuleType::DeLimiter, { { "threshold", "-20" } });
    REQUIRE (hot.ok);
    CHECK (near (value (hot, "dlim.thr"), -12.0));
    CHECK_EQ (hot.report.clamped, 1);
}

TEST_CASE ("import.vint. whole chain: the four modules in the stored order, section power, stale sections ignored")
{
    const auto root = el ("Root", { { "PresetVer", "1" }, { "PluginVer", "120000" } },
                          { el ("Common", { { "Enabled", "1" } },
                                { chainNode ({ "Chain peq", "Chain ccmp", "Chain tape", "Chain clim" }) }),
                            section (ModuleType::ClassicComp, { { "ratio", "4" } }),
                            section (ModuleType::PassiveEq, { { "low boost", "2" }, { "=Power Off", "1" } }),
                            section (ModuleType::ClassicLimiter, { { "threshold", "-6" } }),
                            section (ModuleType::Tape, { { "drive", "6" } }) });
    const auto plan = buildImportPlan (root, opts());
    REQUIRE (plan.ok);
    CHECK (plan.report.globalFile);
    REQUIRE (plan.order.size() == 4);
    CHECK (plan.order[0] == ModuleType::PassiveEq);
    CHECK (plan.order[1] == ModuleType::ClassicComp);
    CHECK (plan.order[2] == ModuleType::Tape);
    CHECK (plan.order[3] == ModuleType::ClassicLimiter);
    CHECK (near (value (plan, "ccmp.a.ratio"), 4.0));
    CHECK (near (value (plan, "peq.a.lowBoost"), 2.0));
    CHECK (near (value (plan, "peq.on"), 0.0));             // the section's own power switch
    CHECK (near (value (plan, "ccmp.on"), 1.0));
    CHECK (near (value (plan, "clim.thr"), -6.0));
    CHECK (near (value (plan, "tape.drive"), 6.0));
    CHECK (plan.report.skipped.empty());

    // a section that is not in the chain holds stale settings and is never read
    const auto stale = buildImportPlan (
        el ("Root", { { "PresetVer", "1" }, { "PluginVer", "120000" } },
            { el ("Common", { { "Enabled", "1" } }, { chainNode ({ "Chain clim" }) }),
              section (ModuleType::ClassicLimiter, { { "threshold", "-4" } }),
              section (ModuleType::Tape, { { "drive", "12" } }) }),
        opts());
    REQUIRE (stale.ok);
    REQUIRE (stale.order.size() == 1);
    CHECK (near (value (stale, "tape.drive"), 0.0));        // ours
    CHECK_EQ (stale.report.mapped, 1);
}

TEST_CASE ("import.vint. a single-module import inserts the module at its usual place")
{
    using T = ModuleType;
    const std::vector<T> core { T::Equalizer1, T::Dynamics, T::Imager, T::Exciter, T::Maximizer };
    CHECK (insertIntoChain (core, T::ClassicComp) == (std::vector<T> { T::Equalizer1, T::Dynamics, T::ClassicComp,
                                                                        T::Imager, T::Exciter, T::Maximizer }));
    CHECK (insertIntoChain (core, T::PassiveEq) == (std::vector<T> { T::Equalizer1, T::Dynamics, T::PassiveEq,
                                                                      T::Imager, T::Exciter, T::Maximizer }));
    CHECK (insertIntoChain (core, T::Tape) == (std::vector<T> { T::Equalizer1, T::Dynamics, T::Imager, T::Exciter,
                                                                 T::Tape, T::Maximizer }));
    CHECK (insertIntoChain (core, T::ClassicLimiter) == (std::vector<T> { T::Equalizer1, T::Dynamics, T::Imager,
                                                                           T::Exciter, T::ClassicLimiter, T::Maximizer }));
}
