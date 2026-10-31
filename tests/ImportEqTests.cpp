#include "TestFramework.h"

#include "plugin/import/ImportEngine.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <string>

// Preset importer, EQ-family tables (Equalizer 1 / 2, Dynamic EQ, Match EQ) on SYNTHETIC trees written by us. The
// sections and controls get our own generic names through ImportOptions::aliases; the keys come from the tables,
// looked up by their our-words labels. Corpus checks run only through `PureSheenDev import` on local folders.
using namespace ps::imp;

namespace
{
using Attrs = std::vector<std::pair<std::string, std::string>>;
using T = ps::ModuleType;

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

const ModuleSpec& specOf (T t)
{
    for (const auto* s : allModuleSpecs())
        if (s->type == t)
            return *s;
    throw pstest::RequireFailed {};
}

Key ruleKey (T t, const char* what)
{
    for (const auto& r : *specOf (t).rules)
        if (std::string (r.what) == what)
            return r.key;
    pstest::reportFailure (__FILE__, __LINE__, std::string ("no rule labelled ") + what);
    throw pstest::RequireFailed {};
}

const Aliases& aliases()
{
    static const Aliases a = []
    {
        Aliases m;
        m["Tone"] = specOf (T::Equalizer1).tagKey;
        m["ToneChain"] = specOf (T::Equalizer1).elementKey;
        m["Late"] = specOf (T::Equalizer2).tagKey;
        m["LateChain"] = specOf (T::Equalizer2).elementKey;
        m["Swell"] = specOf (T::DynamicEq).tagKey;
        m["SwellChain"] = specOf (T::DynamicEq).elementKey;
        m["Copy"] = specOf (T::MatchEq).tagKey;
        m["CopyChain"] = specOf (T::MatchEq).elementKey;
        m["Common"] = kGlobalSectionKey;
        m["Order"] = kChainBlobKey;

        const auto eq = T::Equalizer1;
        m["Node # On"] = ruleKey (eq, "band on");
        m["Node # Kind"] = ruleKey (eq, "band shape");
        m["Node # Hz"] = ruleKey (eq, "band frequency");
        m["Node # Lift"] = ruleKey (eq, "band gain");
        m["Node # Width"] = ruleKey (eq, "band Q");
        m["Node # Phase"] = ruleKey (eq, "band phase");
        m["Node # Shown"] = ruleKey (eq, "node drawn");
        m["Engine"] = ruleKey (eq, "filter mode");
        m["Split"] = ruleKey (eq, "channel mode");
        m["Look"] = ruleKey (eq, "analyser smoothing");
        m["Peek Hz"] = ruleKey (eq, "audition frequency");

        const auto deq = T::DynamicEq;
        m["Node # Trigger"] = ruleKey (deq, "band threshold");
        m["Node # Up"] = ruleKey (deq, "band direction");
        m["Node # Auto"] = ruleKey (deq, "band automatic timing");
        m["Node # Rise"] = ruleKey (deq, "band attack");
        m["Node # Fall"] = ruleKey (deq, "band release");
        m["Node # Rise Factor"] = ruleKey (deq, "band attack multiplier");
        m["Node # Fall Factor"] = ruleKey (deq, "band release multiplier");
        m["Node # Offset"] = ruleKey (deq, "band static offset");
        m["Dyn Engine"] = ruleKey (deq, "filter mode");

        const auto meq = T::MatchEq;
        m["Strength"] = ruleKey (meq, "match amount");
        m["Low Edge"] = ruleKey (meq, "region low edge");
        m["High Edge"] = ruleKey (meq, "region high edge");
        m["Slot # Used"] = ruleKey (meq, "reference spectrum slot");
        m["Picture"] = 0xce256a8918a7f18dull;   // element of the binary child holding the spectrum records
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

// A single-module file: that section enabled, the global section not.
ImportPlan moduleFile (const std::string& section, const std::string& version, std::vector<XmlNode> params)
{
    const auto chain = section + "Chain";
    for (auto& p : params)
        p.attributes[0].second = chain;
    const auto root = el ("Root", { { "PresetVer", "1" }, { "PluginVer", version } },
                          { el ("Common", { { "Enabled", "0" } }),
                            el (section, { { "Enabled", "1" } }, std::move (params)) });
    return buildImportPlan (root, opts());
}

XmlNode p (const std::string& id, const std::string& value) { return param ("", id, value); }

float value (const ImportPlan& plan, const std::string& id)
{
    const int i = ps::paramIndex (id);
    if (i < 0 || i >= (int) plan.values.size())
    {
        pstest::reportFailure (__FILE__, __LINE__, std::string ("unknown id ") + id);
        return -1.0e9f;
    }
    return plan.values[(size_t) i];
}

bool near (float a, double b, double tol = 1.0e-3) { return std::abs ((double) a - b) < tol; }

bool hasNote (const ImportPlan& plan, const std::string& start)
{
    return std::any_of (plan.report.notes.begin(), plan.report.notes.end(),
                        [&] (const std::string& n) { return n.rfind (start, 0) == 0; });
}

int unmappedWithReason (const ImportPlan& plan, const std::string& reason)
{
    return (int) std::count_if (plan.report.unmappedList.begin(), plan.report.unmappedList.end(),
                                [&] (const Report::Unmapped& u) { return u.reason == reason; });
}

std::string band (const char* prefix, int set, int n, const char* name)
{
    return std::string (prefix) + (set == 0 ? ".a.b" : ".b.b") + std::to_string (n) + "." + name;
}
} // namespace

//==============================================================================
// Equalizer
TEST_CASE ("import.eq. band on defaults: odd bands below 110000, none from 110000, both sets")
{
    const auto older = moduleFile ("Tone", "9030", { p ("Node 2 Lift", "1.5") });
    REQUIRE (older.ok);
    CHECK_EQ (older.scope, (int) T::Equalizer1);
    for (int set = 0; set < 2; ++set)
        for (int n = 1; n <= 8; ++n)
            CHECK (near (value (older, band ("eq1", set, n, "on")), n % 2 == 1 ? 1.0 : 0.0));
    CHECK (near (value (older, "eq1.a.b2.gain"), 1.5));   // a stored gain on a band that stays off

    const auto newer = moduleFile ("Tone", "110000", { p ("Node 3 On", "1"), p ("Aux: Node 5 On", "1") });
    REQUIRE (newer.ok);
    for (int n = 1; n <= 8; ++n)
    {
        CHECK (near (value (newer, band ("eq1", 0, n, "on")), n == 3 ? 1.0 : 0.0));
        CHECK (near (value (newer, band ("eq1", 1, n, "on")), n == 5 ? 1.0 : 0.0));
    }

    // No version at all counts as the newest writer.
    const auto unknown = moduleFile ("Tone", "", {});
    REQUIRE (unknown.ok);
    CHECK (near (value (unknown, "eq1.a.b1.on"), 0.0));

    // A stored "off" on an odd band of an older writer.
    const auto off = moduleFile ("Tone", "100000", { p ("Node 1 On", "0") });
    CHECK (near (value (off, "eq1.a.b1.on"), 0.0));
    CHECK (near (value (off, "eq1.a.b3.on"), 1.0));
}

TEST_CASE ("import.eq. extreme finite timing multipliers clamp in both sets without overflow")
{
    std::feclearexcept (FE_ALL_EXCEPT);
    const auto plan = moduleFile ("Swell", "120000", { p ("Node 1 Rise Factor", "1e308"),
                                                       p ("Node 1 Fall Factor", "1e308"),
                                                       p ("Aux: Node 2 Rise Factor", "-1e308"),
                                                       p ("Aux: Node 2 Fall Factor", "-1e308") });
    const int exceptions = std::fetestexcept (FE_INVALID | FE_OVERFLOW);
    REQUIRE (plan.ok);
    CHECK_EQ (exceptions, 0);
    for (const auto& id : { "deq.a.b1.att", "deq.a.b1.rel", "deq.b.b2.att", "deq.b.b2.rel" })
    {
        const auto& spec = ps::paramSpecs()[(size_t) ps::paramIndex (id)];
        CHECK (near (value (plan, id), id[4] == 'a' ? spec.maxValue : spec.minValue));
    }
    CHECK_EQ (plan.report.clamped, 4);
    CHECK_EQ (plan.report.unmapped, 0);
}

TEST_CASE ("import.eq. extreme finite phase amounts clamp without overflow")
{
    std::feclearexcept (FE_ALL_EXCEPT);
    const auto plan = moduleFile ("Tone", "120000", { p ("Node 1 Phase", "1e308"),
                                                      p ("Aux: Node 1 Phase", "-1e308") });
    const int exceptions = std::fetestexcept (FE_INVALID | FE_OVERFLOW);
    REQUIRE (plan.ok);
    CHECK_EQ (exceptions, 0);
    CHECK (near (value (plan, "eq1.a.b1.phase"), 100.0));
    CHECK (near (value (plan, "eq1.b.b1.phase"), 100.0));
}

TEST_CASE ("import.eq. every foreign band shape index maps to our shape")
{
    using S = ps::EqShape;
    const S expected[20] { S::ProportionalQ,     S::LowShelfBaxandall, S::HighShelfBaxandall, S::LowPassFlat,
                           S::HighPassFlat,      S::LowPassResonant,   S::HighPassResonant,   S::LowPassBrickwall,
                           S::HighPassBrickwall, S::LowShelfResonant,  S::HighShelfResonant,  S::LowShelfAnalog,
                           S::HighShelfAnalog,   S::LowShelfVintage,   S::HighShelfVintage,   S::Bell,
                           S::BandShelf,         S::Surgical,          S::LowShelfAnalog,     S::HighShelfAnalog };
    for (int i = 0; i < 20; ++i)
    {
        const auto plan = moduleFile ("Tone", "120000", { p ("Node 4 Kind", std::to_string (i)),
                                                          p ("Aux: Node 6 Kind", std::to_string (i)) });
        REQUIRE (plan.ok);
        CHECK (near (value (plan, "eq1.a.b4.shape"), (double) expected[i]));
        CHECK (near (value (plan, "eq1.b.b6.shape"), (double) expected[i]));
        CHECK_EQ (plan.report.unmapped, 0);
    }

    const auto surgical = moduleFile ("Tone", "9030", { p ("Node 1 Kind", "18") });
    CHECK (hasNote (surgical, "EQ: a surgical low shelf"));

    // An index we do not know: our bell stays, the value counts as not imported, with a note.
    const auto unknown = moduleFile ("Tone", "120000", { p ("Node 2 Kind", "23"), p ("Node 3 Kind", "-4") });
    REQUIRE (unknown.ok);
    CHECK (near (value (unknown, "eq1.a.b2.shape"), (double) S::Bell));
    CHECK (near (value (unknown, "eq1.a.b3.shape"), (double) S::Bell));
    CHECK_EQ (unknown.report.unmapped, 2);
    CHECK (hasNote (unknown, "EQ: a band shape we do not have"));
}

TEST_CASE ("import.eq. absent band shape takes the writer's default per band")
{
    using S = ps::EqShape;
    struct Row { const char* version; int bandNo; S shape; };
    const Row rows[] {
        { "6100", 1, S::LowShelfBaxandall },  { "6100", 3, S::Bell },           { "6100", 7, S::HighShelfBaxandall },
        { "8000", 3, S::ProportionalQ },      { "8000", 8, S::HighShelfBaxandall },
        { "9030", 1, S::LowShelfAnalog },     { "9030", 2, S::ProportionalQ },  { "9030", 6, S::ProportionalQ },
        { "9030", 7, S::HighShelfAnalog },    { "9030", 8, S::LowPassFlat },
        { "100000", 1, S::LowShelfBaxandall },{ "110000", 5, S::ProportionalQ },{ "120000", 7, S::HighShelfBaxandall },
        { "120000", 8, S::LowPassFlat },
    };
    for (const auto& r : rows)
    {
        const auto plan = moduleFile ("Tone", r.version, {});
        REQUIRE (plan.ok);
        CHECK (near (value (plan, band ("eq1", 0, r.bandNo, "shape")), (double) r.shape));
        CHECK (near (value (plan, band ("eq1", 1, r.bandNo, "shape")), (double) r.shape));   // set B: same default
    }
}

TEST_CASE ("import.eq. absent frequency / Q take the foreign layout, stored values win")
{
    const double hz[8] { 50.0, 100.0, 150.0, 500.0, 1000.0, 4000.0, 10000.0, 15000.0 };
    const auto plan = moduleFile ("Tone", "9030", { p ("Node 3 Hz", "212.5"), p ("Node 3 Width", "2.5"),
                                                    p ("Aux: Node 7 Hz", "8000") });
    REQUIRE (plan.ok);
    for (int n = 1; n <= 8; ++n)
    {
        if (n != 3)
            CHECK (near (value (plan, band ("eq1", 0, n, "freq")), hz[n - 1], 0.01));
        if (n != 3)
            CHECK (near (value (plan, band ("eq1", 0, n, "q")), 0.707));
    }
    CHECK (near (value (plan, "eq1.a.b3.freq"), 212.5, 0.01));
    CHECK (near (value (plan, "eq1.a.b3.q"), 2.5));
    CHECK (near (value (plan, "eq1.b.b7.freq"), 8000.0, 0.1));
    CHECK (near (value (plan, "eq1.a.b7.freq"), 10000.0, 0.1));
    CHECK (near (value (plan, "eq1.a.b1.gain"), 0.0));
    CHECK_EQ (plan.report.mapped, 3);

    // Newer writers: the outer shelf bands default to Q 1.
    const auto newer = moduleFile ("Tone", "110000", {});
    CHECK (near (value (newer, "eq1.a.b1.q"), 1.0));
    CHECK (near (value (newer, "eq1.a.b7.q"), 1.0));
    CHECK (near (value (newer, "eq1.a.b4.q"), 0.707));
}

TEST_CASE ("import.eq. band phase, filter mode, channel modes and clamps")
{
    const auto plan = moduleFile ("Tone", "9030", { p ("Engine", "1"), p ("Node 1 Phase", "-1"),
                                                    p ("Aux: Node 7 Phase", "-0.5481928"), p ("Split", "2"),
                                                    p ("Node 2 Lift", "20"), p ("Node 2 Hz", "5") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "eq1.mode"), (double) ps::EqFilterMode::Digital));
    CHECK (near (value (plan, "eq1.a.b1.phase"), 100.0));
    CHECK (near (value (plan, "eq1.b.b7.phase"), 54.81928, 0.01));
    CHECK (near (value (plan, "eq1.a.b3.phase"), 0.0));
    CHECK (near (value (plan, "eq1.chMode"), (double) ps::ChannelMode::LeftRight));
    CHECK (near (value (plan, "eq1.a.b2.gain"), 15.0));
    CHECK (near (value (plan, "eq1.a.b2.freq"), 20.0, 0.01));
    CHECK_EQ (plan.report.clamped, 2);

    // The foreign mode list is St, M/S, L/R, T/S, the same order as ours.
    for (int i = 0; i < 4; ++i)
    {
        const auto m = moduleFile ("Tone", "120000", { p ("Split", std::to_string (i)) });
        CHECK (near (value (m, "eq1.chMode"), (double) i));
    }
}

TEST_CASE ("import.eq. node-drawn flag: fallback without band on, ignored otherwise; view state ignored")
{
    const auto plan = moduleFile ("Tone", "120000", { p ("Node 2 Shown", "1"), p ("Node 4 On", "1"),
                                                      p ("Node 4 Shown", "1"), p ("Look", "0.5"),
                                                      p ("Peek Hz", "1200"), p ("Mystery", "3") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "eq1.a.b2.on"), 1.0));
    CHECK (near (value (plan, "eq1.a.b4.on"), 1.0));
    CHECK_EQ (plan.report.mapped, 2);    // band 4 on, band 2 drawn (used as on)
    CHECK_EQ (plan.report.ignored, 3);   // band 4 drawn, analyser smoothing, audition position
    CHECK_EQ (plan.report.unmapped, 1);
    CHECK_EQ (unmappedWithReason (plan, "unknown"), 1);
}

TEST_CASE ("import.eq. the second equalizer uses the same table")
{
    const auto plan = moduleFile ("Late", "9030", { p ("Node 3 Lift", "-2"), p ("Node 3 Kind", "15"),
                                                    p ("Aux: Node 1 Hz", "40") });
    REQUIRE (plan.ok);
    CHECK_EQ (plan.scope, (int) T::Equalizer2);
    CHECK (near (value (plan, "eq2.a.b3.gain"), -2.0));
    CHECK (near (value (plan, "eq2.a.b3.shape"), (double) ps::EqShape::Bell));
    CHECK (near (value (plan, "eq2.a.b3.on"), 1.0));
    CHECK (near (value (plan, "eq2.b.b1.freq"), 40.0, 0.01));
    CHECK (near (value (plan, "eq1.a.b3.gain"), 0.0));   // the first equalizer is untouched
}

//==============================================================================
// Dynamic EQ
TEST_CASE ("import.eq. dynamic EQ band on and shape defaults per version")
{
    using D = ps::DynEqShape;
    const auto older = moduleFile ("Swell", "9030", {});
    REQUIRE (older.ok);
    CHECK_EQ (older.scope, (int) T::DynamicEq);
    const bool onOld[6] { true, false, true, true, false, true };
    const D shapes[6] { D::LowShelfBaxandall, D::ProportionalQ, D::ProportionalQ, D::ProportionalQ, D::ProportionalQ,
                        D::HighShelfBaxandall };
    for (int set = 0; set < 2; ++set)
        for (int n = 1; n <= 6; ++n)
        {
            CHECK (near (value (older, band ("deq", set, n, "on")), onOld[n - 1] ? 1.0 : 0.0));
            CHECK (near (value (older, band ("deq", set, n, "shape")), (double) shapes[n - 1]));
            CHECK (near (value (older, band ("deq", set, n, "auto")), 1.0));
            CHECK (near (value (older, band ("deq", set, n, "dir")), (double) ps::DynEqDirection::Down));
        }

    const auto newer = moduleFile ("Swell", "110000", { p ("Node 6 On", "1") });
    for (int n = 1; n <= 6; ++n)
        CHECK (near (value (newer, band ("deq", 0, n, "on")), n == 6 ? 1.0 : 0.0));
}

TEST_CASE ("import.eq. dynamic EQ shape index, direction, threshold, offset, filter mode")
{
    for (int i = 0; i <= 4; ++i)
    {
        const auto plan = moduleFile ("Swell", "9030", { p ("Node 2 Kind", std::to_string (i)) });
        CHECK (near (value (plan, "deq.a.b2.shape"), (double) i));
    }
    const auto unknown = moduleFile ("Swell", "9030", { p ("Node 2 Kind", "7") });
    CHECK (near (value (unknown, "deq.a.b2.shape"), (double) ps::DynEqShape::ProportionalQ));
    CHECK_EQ (unknown.report.unmapped, 1);
    CHECK (hasNote (unknown, "Dynamic EQ: a band shape we do not have"));

    const auto plan = moduleFile ("Swell", "100000", { p ("Node 4 Trigger", "-130"), p ("Node 4 Lift", "-4"),
                                                       p ("Node 4 Hz", "6500"), p ("Aux: Node 2 Up", "1"),
                                                       p ("Node 4 Offset", "-0.5"), p ("Dyn Engine", "1"),
                                                       p ("Split", "1") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "deq.a.b4.thr"), -130.0));
    CHECK (near (value (plan, "deq.a.b4.gain"), -4.0));
    CHECK (near (value (plan, "deq.a.b4.freq"), 6500.0, 0.1));
    CHECK (near (value (plan, "deq.b.b2.dir"), (double) ps::DynEqDirection::Up));
    CHECK (near (value (plan, "deq.a.b2.dir"), (double) ps::DynEqDirection::Down));
    CHECK (near (value (plan, "deq.a.b4.offset"), -0.5));
    CHECK (near (value (plan, "deq.mode"), (double) ps::EqFilterMode::Digital));
    CHECK (near (value (plan, "deq.chMode"), (double) ps::ChannelMode::MidSide));
    CHECK (near (value (plan, "deq.a.b1.thr"), -30.0));      // absent: the default threshold
    CHECK (near (value (plan, "deq.a.b1.freq"), 60.0, 0.01)); // absent: the default layout
    CHECK_EQ (plan.report.unmapped, 0);
}

TEST_CASE ("import.eq. dynamic EQ timing: multipliers with automatic timing, ms without")
{
    const auto plan = moduleFile ("Swell", "9030", {
        p ("Node 3 Rise Factor", "2"), p ("Node 3 Fall Factor", "0.5"), p ("Node 3 Rise", "40"),   // auto on: ms inactive
        p ("Node 4 Auto", "0"), p ("Node 4 Rise", "20"), p ("Node 4 Fall", "100"), p ("Node 4 Rise Factor", "3"),
        p ("Aux: Node 6 Auto", "0"), p ("Aux: Node 6 Fall", "200") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "deq.a.b3.auto"), 1.0));
    CHECK (near (value (plan, "deq.a.b3.att"), 10.0, 0.01));
    CHECK (near (value (plan, "deq.a.b3.rel"), 40.0, 0.01));
    CHECK (near (value (plan, "deq.a.b4.auto"), 0.0));
    CHECK (near (value (plan, "deq.a.b4.att"), 20.0, 0.01));
    CHECK (near (value (plan, "deq.a.b4.rel"), 100.0, 0.01));
    CHECK (near (value (plan, "deq.b.b6.auto"), 0.0));
    CHECK (near (value (plan, "deq.b.b6.att"), 5.0, 0.01));    // absent attack: the default
    CHECK (near (value (plan, "deq.b.b6.rel"), 200.0, 0.1));
    CHECK (near (value (plan, "deq.a.b1.att"), 5.0, 0.01));    // untouched bands: our base times
    CHECK (near (value (plan, "deq.a.b1.rel"), 80.0, 0.01));
    CHECK_EQ (plan.report.mapped, 7);
    CHECK_EQ (plan.report.ignored, 2);    // the inactive ms value and the inactive multiplier
    CHECK_EQ (plan.report.unmapped, 0);
    CHECK (hasNote (plan, "Dynamic EQ: automatic attack / release multipliers"));
}

//==============================================================================
// Match EQ
TEST_CASE ("import.eq. match EQ settings into both sets; a flagged spectrum without its records is reported")
{
    const auto plan = moduleFile ("Copy", "9030", { p ("Strength", "28.5714"), p ("Look", "63.4921"),
                                                    p ("Low Edge", "250"), p ("Aux: High Edge", "9980.71"),
                                                    p ("Slot 1 Used", "1") });
    REQUIRE (plan.ok);
    CHECK_EQ (plan.scope, (int) T::MatchEq);
    CHECK (near (value (plan, "meq.a.amount"), 28.5714, 0.01));
    CHECK (near (value (plan, "meq.b.amount"), 50.0));   // set B absent: the default
    CHECK (near (value (plan, "meq.a.smooth"), 63.4921, 0.01));
    CHECK (near (value (plan, "meq.a.lo"), 250.0, 0.01));
    CHECK (near (value (plan, "meq.b.lo"), 20.0, 0.01));
    CHECK (near (value (plan, "meq.a.hi"), 20000.0, 1.0));
    CHECK (near (value (plan, "meq.b.hi"), 9980.71, 0.1));
    CHECK_EQ (plan.report.mapped, 4);
    CHECK_EQ (plan.report.unmapped, 1);
    CHECK_EQ (unmappedWithReason (plan, "value"), 1);
    CHECK (plan.report.attachments.empty());
    CHECK (hasNote (plan, "Match EQ: the stored reference spectrum could not be read"));

    const auto empty = moduleFile ("Copy", "120000", {});
    REQUIRE (empty.ok);
    CHECK (near (value (empty, "meq.a.amount"), 50.0));
    CHECK (empty.report.notes.empty());
}

TEST_CASE ("import.eq. match EQ: the first slot flagged in use hands its records on; damaged records are reported")
{
    auto file = [] (std::vector<XmlNode> children)
    {
        return buildImportPlan (el ("Root", { { "PresetVer", "1" }, { "PluginVer", "9030" } },
                                    { el ("Common", { { "Enabled", "0" } }),
                                      el ("Copy", { { "Enabled", "1" } }, std::move (children)) }),
                                opts());
    };
    auto flag = [] (int slot, const char* v) { return param ("CopyChain", "Slot " + std::to_string (slot) + " Used", v); };
    auto blob = [] (const char* data) { return el ("ExtraBytes", { { "ElementID", "Picture" }, { "Data", data } }); };

    // slot 1 flagged off, slot 2 in use, slot 3 also flagged (an extra display curve): slot 2 is attached
    const auto plan = file ({ flag (1, "0"), flag (2, "1"), flag (3, "1"), blob ("AQIDBA==") });
    REQUIRE (plan.ok);
    REQUIRE ((int) plan.report.attachments.size() == 1);
    const auto& a = plan.report.attachments[0];
    CHECK (a.type == T::MatchEq);
    CHECK_EQ (a.slot, 2);
    CHECK (a.bytes == std::vector<unsigned char> ({ 1, 2, 3, 4 }));
    CHECK_EQ (plan.report.mapped, 1);
    CHECK_EQ (plan.report.ignored, 2);
    CHECK_EQ (plan.report.unmapped, 0);
    CHECK (plan.report.notes.empty());

    // no flag in use: nothing attached even with records present
    const auto off = file ({ flag (1, "0"), blob ("AQIDBA==") });
    REQUIRE (off.ok);
    CHECK (off.report.attachments.empty());
    CHECK_EQ (off.report.unmapped, 0);

    // records that are not base64 / empty: the flag is not imported, with a note
    for (const char* bad : { "%%%", "" })
    {
        const auto damaged = file ({ flag (1, "1"), blob (bad) });
        REQUIRE (damaged.ok);
        CHECK (damaged.report.attachments.empty());
        CHECK_EQ (damaged.report.unmapped, 1);
        CHECK (hasNote (damaged, "Match EQ: the stored reference spectrum could not be read"));
    }

    // a whole chain carries the attachment of its Match EQ too
    std::vector<unsigned char> bytes { 0, 9, 0, 0, 0 };
    for (const char c : std::string ("CopyChain"))
        bytes.push_back ((unsigned char) c);
    static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        const unsigned v = (unsigned) bytes[i] << 16 | (i + 1 < bytes.size() ? (unsigned) bytes[i + 1] << 8 : 0u)
                           | (i + 2 < bytes.size() ? (unsigned) bytes[i + 2] : 0u);
        b64 += digits[(v >> 18) & 63];
        b64 += digits[(v >> 12) & 63];
        b64 += i + 1 < bytes.size() ? digits[(v >> 6) & 63] : '=';
        b64 += i + 2 < bytes.size() ? digits[v & 63] : '=';
    }
    const auto chain = buildImportPlan (
        el ("Root", { { "PresetVer", "1" }, { "PluginVer", "9030" } },
            { el ("Common", { { "Enabled", "1" } }, { el ("ExtraBytes", { { "ElementID", "Order" }, { "Data", b64 } }) }),
              el ("Copy", { { "Enabled", "1" } }, { flag (1, "1"), blob ("AQIDBA==") }),
              el ("Tone", { { "Enabled", "1" } }) }),
        opts());
    REQUIRE (chain.ok);
    CHECK_EQ (chain.scope, -1);
    REQUIRE ((int) chain.report.attachments.size() == 1);
    CHECK_EQ (chain.report.attachments[0].slot, 1);
}

TEST_CASE ("import.eq. match EQ accepts set B reference flags and shares matching selections")
{
    auto file = [] (std::vector<XmlNode> children)
    {
        return buildImportPlan (el ("Root", { { "PresetVer", "1" }, { "PluginVer", "120000" } },
                                    { el ("Common", { { "Enabled", "0" } }),
                                      el ("Copy", { { "Enabled", "1" } }, std::move (children)) }), opts());
    };
    auto blob = [] (const char* data) { return el ("ExtraBytes", { { "ElementID", "Picture" }, { "Data", data } }); };

    const auto aux = file ({ param ("CopyChain", "Aux: Slot 2 Used", "1"),
                            param ("CopyChain", "Aux: Slot 3 Used", "1"), blob ("AQIDBA==") });
    REQUIRE (aux.ok);
    REQUIRE (aux.report.attachments.size() == 1);
    CHECK_EQ (aux.report.attachments[0].slot, 2);
    CHECK_EQ (aux.report.attachments[0].mappedControls, 1);
    CHECK_EQ (aux.report.mapped, 1);
    CHECK_EQ (aux.report.ignored, 1);
    CHECK_EQ (aux.report.unmapped, 0);

    const auto same = file ({ param ("CopyChain", "Slot 2 Used", "1"),
                             param ("CopyChain", "Aux: Slot 2 Used", "1"), blob ("AQIDBA==") });
    REQUIRE (same.ok);
    REQUIRE (same.report.attachments.size() == 1);
    CHECK_EQ (same.report.attachments[0].slot, 2);
    CHECK_EQ (same.report.attachments[0].mappedControls, 2);
    CHECK_EQ (same.report.mapped, 2);
    CHECK_EQ (same.report.unmapped, 0);
    CHECK (same.report.notes.empty());

    const auto different = file ({ param ("CopyChain", "Slot 2 Used", "1"),
                                  param ("CopyChain", "Aux: Slot 1 Used", "1"), blob ("AQIDBA==") });
    REQUIRE (different.ok);
    REQUIRE (different.report.attachments.size() == 1);
    CHECK_EQ (different.report.attachments[0].slot, 2);
    CHECK_EQ (different.report.attachments[0].mappedControls, 1);
    CHECK_EQ (different.report.mapped, 1);
    CHECK_EQ (different.report.unmapped, 1);
    CHECK_EQ (unmappedWithReason (different, "unsupported"), 1);
    CHECK (hasNote (different, "Match EQ: the second set selected a different reference"));

    const auto damaged = file ({ param ("CopyChain", "Slot 2 Used", "1"),
                                param ("CopyChain", "Aux: Slot 2 Used", "1"), blob ("%%") });
    REQUIRE (damaged.ok);
    CHECK (damaged.report.attachments.empty());
    CHECK_EQ (damaged.report.unmapped, 2);
    CHECK_EQ (unmappedWithReason (damaged, "value"), 2);
}

TEST_CASE ("import.eq. a whole chain with all four modules keeps their order and values")
{
    // chain blob: [u8 flag][u32 LE length][name] per entry
    std::vector<unsigned char> bytes;
    for (const std::string name : { "CopyChain", "ToneChain", "SwellChain", "LateChain" })
    {
        bytes.push_back (0);
        const auto n = (std::uint32_t) name.size();
        for (int i = 0; i < 4; ++i)
            bytes.push_back ((unsigned char) ((n >> (8 * i)) & 0xffu));
        bytes.insert (bytes.end(), name.begin(), name.end());
    }
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3)
    {
        const unsigned v = (unsigned) bytes[i] << 16 | (unsigned) bytes[i + 1] << 8 | bytes[i + 2];
        b64 += abc[(v >> 18) & 63]; b64 += abc[(v >> 12) & 63]; b64 += abc[(v >> 6) & 63]; b64 += abc[v & 63];
    }
    if (bytes.size() - i == 1)
    {
        const unsigned v = (unsigned) bytes[i] << 16;
        b64 += abc[(v >> 18) & 63]; b64 += abc[(v >> 12) & 63]; b64 += "==";
    }
    else if (bytes.size() - i == 2)
    {
        const unsigned v = (unsigned) bytes[i] << 16 | (unsigned) bytes[i + 1] << 8;
        b64 += abc[(v >> 18) & 63]; b64 += abc[(v >> 12) & 63]; b64 += abc[(v >> 6) & 63]; b64 += '=';
    }

    const auto root = el ("Root", { { "PresetVer", "3" }, { "PluginVer", "100000" } },
                          { el ("Common", { { "Enabled", "1" } }, { el ("ExtraBytes", { { "ElementID", "Order" }, { "Data", b64 } }) }),
                            el ("Tone", { { "Enabled", "1" } }, { param ("ToneChain", "Node 7 Lift", "1.5") }),
                            el ("Late", { { "Enabled", "1" } }, { param ("LateChain", "Node 1 Kind", "8") }),
                            el ("Swell", { { "Enabled", "1" } }, { param ("SwellChain", "Node 6 Lift", "-3") }),
                            el ("Copy", { { "Enabled", "1" } }, { param ("CopyChain", "Strength", "40") }) });
    const auto plan = buildImportPlan (root, opts());
    REQUIRE (plan.ok);
    REQUIRE (plan.order.size() == 4);
    CHECK_EQ (plan.order[0], T::MatchEq);
    CHECK_EQ (plan.order[1], T::Equalizer1);
    CHECK_EQ (plan.order[2], T::DynamicEq);
    CHECK_EQ (plan.order[3], T::Equalizer2);
    CHECK (near (value (plan, "eq1.a.b7.gain"), 1.5));
    CHECK (near (value (plan, "eq1.a.b7.shape"), (double) ps::EqShape::HighShelfBaxandall));
    CHECK (near (value (plan, "eq2.a.b1.shape"), (double) ps::EqShape::HighPassBrickwall));
    CHECK (near (value (plan, "deq.a.b6.gain"), -3.0));
    CHECK (near (value (plan, "meq.a.amount"), 40.0));
    CHECK_EQ (plan.report.mapped, 4);
    CHECK_EQ (plan.report.unmapped, 0);
}
