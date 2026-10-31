#include "TestFramework.h"

#include "plugin/import/ImportEngine.h"
#include "dsp/Dynamics.h"

#include <array>
#include <cfenv>
#include <cmath>
#include <string>
#include <vector>

// Preset importer, core dynamics family tables (Dynamics, Exciter, Maximizer) on SYNTHETIC trees written by us.
// Recognition works by key, so the sections and controls get our own generic names through ImportOptions::aliases
// (the keys come from the importer's tables, looked up by their our-words labels). Corpus checks run locally through
// `PureSheenDev import`, never here.
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

XmlNode param (const std::string& id, const std::string& value)
{
    return el ("Param", { { "ElementID", "Unit" }, { "ParamID", id }, { "Value", value } });
}

const ModuleSpec& specOf (ModuleType t)
{
    for (const auto* s : allModuleSpecs())
        if (s->type == t)
            return *s;
    throw pstest::RequireFailed {};
}

Key ruleKey (ModuleType t, const char* what)
{
    for (const auto& r : *specOf (t).rules)
        if (std::string (r.what) == what)
            return r.key;
    pstest::reportFailure (__FILE__, __LINE__, std::string ("no rule labelled ") + what);
    throw pstest::RequireFailed {};
}

// Our generic names: one section name per module ("Squash" = Dynamics, "Glow" = Exciter, "Wall" = Maximizer), the
// controls named after what they do. A single-module file's ElementIDs are all "Unit" (section found by its tag).
const Aliases& aliases()
{
    static const Aliases a = []
    {
        Aliases m;
        const auto dyn = ModuleType::Dynamics, exc = ModuleType::Exciter, max = ModuleType::Maximizer;
        m["Squash"] = specOf (dyn).tagKey;
        m["Glow"] = specOf (exc).tagKey;
        m["Wall"] = specOf (max).tagKey;
        m["Spread"] = specOf (ModuleType::Imager).tagKey;
        m["Unit"] = 0x1234567890abcdefull;   // an ElementID no table knows
        m["Common"] = kGlobalSectionKey;

        // Dynamics
        m["Present #"] = ruleKey (dyn, "band present");
        m["Count"] = ruleKey (dyn, "band count (older files)");
        m["Edge #"] = ruleKey (dyn, "crossover frequency");
        m["Off #"] = ruleKey (dyn, "band bypass");
        m["Kind"] = ruleKey (dyn, "crossover kind");
        m["Lane # Thr"] = ruleKey (dyn, "compressor threshold");
        m["Lane # Ratio"] = ruleKey (dyn, "compressor ratio");
        m["Lane # Att"] = ruleKey (dyn, "compressor attack");
        m["Lane # Rel"] = ruleKey (dyn, "compressor release");
        m["Lane # Knee"] = ruleKey (dyn, "compressor knee");
        m["Lane # Cap Thr"] = ruleKey (dyn, "limiter threshold");
        m["Lane # Cap Ratio"] = ruleKey (dyn, "limiter ratio");
        m["Lane # Cap Att"] = ruleKey (dyn, "limiter attack");
        m["Lane # Cap Rel"] = ruleKey (dyn, "limiter release");
        m["Lane # Cap Knee"] = ruleKey (dyn, "limiter knee");
        m["Lane # Blend"] = ruleKey (dyn, "band mix");
        m["Lane # Level"] = ruleKey (dyn, "band gain");
        m["Sense"] = ruleKey (dyn, "detection kind");
        m["Recovery"] = ruleKey (dyn, "release mode");
        m["Key Filter"] = ruleKey (dyn, "detection filter kind");
        m["Key Freq"] = ruleKey (dyn, "detection filter frequency");
        m["Key Slope"] = ruleKey (dyn, "detection filter slope");
        m["Key Lean"] = ruleKey (dyn, "detection tilt amount");
        m["Makeup"] = ruleKey (dyn, "auto gain");
        m["Out"] = ruleKey (dyn, "output gain");
        m["Blend All"] = ruleKey (dyn, "module mix");
        m["Look"] = ruleKey (dyn, "lookahead");
        m["First Off"] = ruleKey (dyn, "set A off");
        m["Second Off"] = ruleKey (dyn, "set B off");
        m["Tie"] = ruleKey (dyn, "set link");
        m["Split"] = ruleKey (dyn, "channel mode");     // the same key in the Exciter table
        m["Steep"] = ruleKey (dyn, "crossover steepness");
        m["Lane # Solo"] = ruleKey (dyn, "band solo");
        m["Lane # Spread"] = ruleKey (ModuleType::Imager, "band width");
        m["Wide On"] = ruleKey (ModuleType::Imager, "stereoize on");
        m["Rescue On"] = ruleKey (ModuleType::Imager, "recover sides on");

        // Exciter
        m["Lane # Model"] = ruleKey (exc, "band model");
        m["Model"] = ruleKey (exc, "model of every band");
        m["Lane # Drive"] = ruleKey (exc, "band drive");
        m["After Freq"] = ruleKey (exc, "post filter frequency");
        m["After Gain"] = ruleKey (exc, "post filter gain");
        m["After Width"] = ruleKey (exc, "post filter width");
        m["Fine"] = ruleKey (exc, "oversampling");

        // Maximizer
        m["Algo"] = ruleKey (max, "algorithm");
        m["Style A"] = ruleKey (max, "aggressive style");
        m["Style S"] = ruleKey (max, "spectral style");
        m["Push Old"] = ruleKey (max, "threshold (older files)");
        m["Push"] = ruleKey (max, "gain");
        m["Roof"] = ruleKey (max, "output ceiling");
        m["Feel"] = ruleKey (max, "character");
        m["Peaks"] = ruleKey (max, "true peak");
        m["Snap On"] = ruleKey (max, "transient emphasis on");
        m["Snap"] = ruleKey (max, "transient emphasis");
        m["Clip On"] = ruleKey (max, "soft clip on");
        m["Clip Kind"] = ruleKey (max, "soft clip kind");
        m["Clip Amount"] = ruleKey (max, "soft clip amount");
        m["Lift On"] = ruleKey (max, "upward compression on");
        m["Lift"] = ruleKey (max, "upward compression");
        m["Free Hit"] = ruleKey (max, "transient independence");
        m["Free Body"] = ruleKey (max, "sustain independence");
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

// A single-module file: that section enabled, the global section not (no chain needed for a single module).
ImportPlan importModule (const std::string& section, const std::string& version, std::vector<XmlNode> params)
{
    const auto root = el ("Root", { { "PresetVer", "1" }, { "PluginVer", version } },
                          { el ("Common", { { "Enabled", "0" } }),
                            el (section, { { "Enabled", "1" } }, std::move (params)) });
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

bool hasNote (const ImportPlan& p, const std::string& start)
{
    for (const auto& n : p.report.notes)
        if (n.rfind (start, 0) == 0)
            return true;
    return false;
}

int unmappedWithReason (const ImportPlan& p, const std::string& reason)
{
    int n = 0;
    for (const auto& u : p.report.unmappedList)
        n += u.reason == reason ? 1 : 0;
    return n;
}
} // namespace

//==============================================================================
// Dynamics
TEST_CASE ("import.core. dynamics: absent band values take the foreign defaults confirmed in the real product")
{
    // 110000: one band (slot 0 only), only its threshold stored.
    const auto plan = importModule ("Squash", "110000", { param ("Lane 1 Thr", "-20.5") });
    REQUIRE (plan.ok);
    CHECK_EQ (plan.scope, (int) ModuleType::Dynamics);
    CHECK (near (value (plan, "dyn.bands"), 1.0));
    CHECK (near (value (plan, "dyn.a.b1.comp.thr"), -20.5));
    CHECK (near (value (plan, "dyn.a.b1.comp.ratio"), 2.0));
    CHECK (near (value (plan, "dyn.a.b1.comp.att"), 20.0));
    CHECK (near (value (plan, "dyn.a.b1.comp.rel"), 60.0));
    CHECK (near (value (plan, "dyn.a.b1.comp.knee"), 10.0));       // knob maximum = the default (display units = dB)
    CHECK (near (value (plan, "dyn.a.b1.lim.thr"), 0.0));
    CHECK (near (value (plan, "dyn.a.b1.lim.on"), 0.0));          // at 0 dB the band limiter stays off
    CHECK (near (value (plan, "dyn.a.b1.lim.ratio"), 10.0));
    CHECK (near (value (plan, "dyn.a.b1.lim.att"), 20.0));
    CHECK (near (value (plan, "dyn.a.b1.lim.rel"), 100.0));
    CHECK (near (value (plan, "dyn.a.b1.lim.knee"), 10.0));
    CHECK (near (value (plan, "dyn.a.b1.mix"), 100.0));
    CHECK (near (value (plan, "dyn.b.b1.comp.thr"), -12.0));       // set B from the same defaults
    CHECK (near (value (plan, "dyn.b.b1.comp.ratio"), 2.0));
    CHECK (near (value (plan, "dyn.adaptRel"), 1.0));              // absent release mode = adaptive
    CHECK (near (value (plan, "dyn.detect"), (double) ps::DetectionMode::Peak));   // absent detection = peak
    CHECK (near (value (plan, "dyn.detSlope"), 1.0));
    CHECK (near (value (plan, "dyn.xoverType"), (double) ps::CrossoverType::Digital));   // absent kind = linear phase
    CHECK (near (value (plan, "dyn.a.on"), 1.0));
    CHECK (near (value (plan, "dyn.b.on"), 1.0));
    CHECK_EQ (plan.report.mapped, 1);
    CHECK_EQ (plan.report.unmapped, 0);

    // An older writer: four contiguous bands by default; the band defaults are the same (no writer stores them).
    const auto old = importModule ("Squash", "8000", { param ("Lane 2 Ratio", "3") });
    REQUIRE (old.ok);
    CHECK (near (value (old, "dyn.bands"), 4.0));
    CHECK (near (value (old, "dyn.a.b1.comp.thr"), -12.0));
    CHECK (near (value (old, "dyn.a.b3.lim.att"), 20.0));
    CHECK (near (value (old, "dyn.a.b2.comp.ratio"), 3.0));
    CHECK (near (value (old, "dyn.a.b4.comp.ratio"), 2.0));

    // A stored older band count wins below 9000.
    const auto two = importModule ("Squash", "8000", { param ("Count", "2") });
    CHECK (near (value (two, "dyn.bands"), 2.0));
}

TEST_CASE ("import.core. dynamics: detection kinds, release mode, detection filter (every index)")
{
    // Detection kinds as confirmed by one test save each: 0 peak (the default), 1 RMS, 2 envelope.
    const double expected[] { (double) ps::DetectionMode::Peak, (double) ps::DetectionMode::Rms,
                              (double) ps::DetectionMode::Envelope };
    for (int i = 0; i < 3; ++i)
    {
        const auto p = importModule ("Squash", "100000", { param ("Sense", std::to_string (i)) });
        CHECK (near (value (p, "dyn.detect"), expected[i]));
    }
    const auto bad = importModule ("Squash", "100000", { param ("Sense", "5") });
    CHECK (near (value (bad, "dyn.detect"), (double) ps::DynamicsParams().detection));   // ours kept
    CHECK_EQ (bad.report.unmapped, 1);

    CHECK (near (value (importModule ("Squash", "100000", { param ("Recovery", "1") }), "dyn.adaptRel"), 0.0));
    CHECK (near (value (importModule ("Squash", "100000", { param ("Recovery", "0") }), "dyn.adaptRel"), 1.0));

    for (int i = 0; i < 3; ++i)
        CHECK (near (value (importModule ("Squash", "100000", { param ("Key Filter", std::to_string (i)) }),
                            "dyn.detFilter"), i));
    for (int i = 0; i < 4; ++i)
        CHECK (near (value (importModule ("Squash", "100000", { param ("Key Slope", std::to_string (i)) }),
                            "dyn.detSlope"), i));

    const auto hp = importModule ("Squash", "100000", { param ("Key Filter", "1"), param ("Key Freq", "283.1") });
    CHECK (near (value (hp, "dyn.detFreq"), 283.1, 0.01));

    // Crossover kinds: 0 linear phase, 1 analog, absent = the third kind.
    CHECK (near (value (importModule ("Squash", "100000", { param ("Kind", "0") }), "dyn.xoverType"),
                 (double) ps::CrossoverType::Digital));
    CHECK (near (value (importModule ("Squash", "100000", { param ("Kind", "1") }), "dyn.xoverType"),
                 (double) ps::CrossoverType::Analog));
}

TEST_CASE ("import.core. dynamics: an absent third crossover takes 10 kHz, stored ones win")
{
    const auto plan = importModule ("Squash", "120000", { param ("Present 1", "1"), param ("Present 2", "1"),
                                                         param ("Present 3", "1"), param ("Edge 1", "132"),
                                                         param ("Edge 2", "520") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "dyn.bands"), 4.0));
    CHECK (near (value (plan, "dyn.xover1"), 132.0));
    CHECK (near (value (plan, "dyn.xover2"), 520.0));
    CHECK (near (value (plan, "dyn.xover3"), 10000.0));
}

TEST_CASE ("import.core. dynamics: gaps closed, band bypass in both sets, limiter switch, stale values ignored")
{
    // Slots 0, 2 present (slot 1 removed). Band numbers are slot + 1; bypass and present flags count from 0.
    const auto plan = importModule ("Squash", "100000", { param ("Present 2", "1"),
                                                         param ("Edge 1", "300"),
                                                         param ("Edge 2", "2500"),
                                                         param ("Lane 1 Thr", "-18"),
                                                         param ("Lane 2 Thr", "-30"),     // removed slot: stale
                                                         param ("Lane 3 Thr", "-24"),
                                                         param ("Lane 3 Cap Thr", "-3"),
                                                         param ("Lane 3 Cap Att", "0"),
                                                         param ("Aux: Lane 3 Cap Thr", "-6"),
                                                         param ("Off 2", "1"),
                                                         param ("Lane 1 Solo", "1") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "dyn.bands"), 2.0));
    CHECK (near (value (plan, "dyn.xover1"), 2500.0));
    CHECK (near (value (plan, "dyn.a.b1.comp.thr"), -18.0));
    CHECK (near (value (plan, "dyn.a.b2.comp.thr"), -24.0));
    CHECK (near (value (plan, "dyn.a.b2.lim.thr"), -3.0));
    CHECK (near (value (plan, "dyn.a.b2.lim.on"), 1.0));
    CHECK (near (value (plan, "dyn.a.b2.lim.att"), 0.1));          // 0 ms = our shortest, not a clamp
    CHECK (near (value (plan, "dyn.b.b2.lim.on"), 1.0));
    CHECK (near (value (plan, "dyn.a.b1.lim.on"), 0.0));
    CHECK (near (value (plan, "dyn.a.b2.on"), 0.0));               // slot 2 bypassed: both sets
    CHECK (near (value (plan, "dyn.b.b2.on"), 0.0));
    CHECK (near (value (plan, "dyn.a.b1.on"), 1.0));
    CHECK_EQ (plan.report.clamped, 0);
    CHECK_EQ (plan.report.unmapped, 0);
    CHECK_EQ (plan.report.ignored, 3);                              // stale threshold, stale edge 1, band solo
}

TEST_CASE ("import.core. dynamics: shared controls take set A; set B gain and module mixes fold into the bands")
{
    // Mid/side: set B's detection kind differs (shared here: set A wins, with a note); set B's output gain is 2 dB
    // above set A's; set A's module mix is 50 %, set B's 50 % over a band mix of 80 %.
    const auto plan = importModule ("Squash", "100000", { param ("Split", "1"),
                                                         param ("Sense", "0"),
                                                         param ("Aux: Sense", "1"),
                                                         param ("Out", "1"),
                                                         param ("Aux: Out", "3"),
                                                         param ("Lane 1 Level", "0.5"),
                                                         param ("Aux: Lane 1 Level", "-1"),
                                                         param ("Blend All", "50"),
                                                         param ("Aux: Blend All", "50"),
                                                         param ("Aux: Lane 1 Blend", "80"),
                                                         param ("Makeup", "1"),
                                                         param ("First Off", "1") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "dyn.chMode"), 1.0));
    CHECK (near (value (plan, "dyn.detect"), (double) ps::DetectionMode::Peak));
    CHECK (hasNote (plan, "Dynamics: the second set's detection kind"));
    CHECK (near (value (plan, "dyn.gain"), 1.0));
    CHECK (near (value (plan, "dyn.a.b1.gain"), 0.5));
    CHECK (near (value (plan, "dyn.b.b1.gain"), 1.0));             // -1 + (3 - 1)
    CHECK (hasNote (plan, "Dynamics: the second set's output gain"));
    CHECK (near (value (plan, "dyn.a.b1.mix"), 50.0));
    CHECK (near (value (plan, "dyn.b.b1.mix"), 40.0));
    CHECK (hasNote (plan, "Dynamics: the module mix"));
    CHECK (near (value (plan, "dyn.autoGain"), 1.0));
    CHECK (near (value (plan, "dyn.a.on"), 0.0));
    CHECK (near (value (plan, "dyn.b.on"), 1.0));
    CHECK_EQ (plan.report.unmapped, 0);

    // In Stereo, a stored set B leftover equal or not, no note.
    const auto st = importModule ("Squash", "100000", { param ("Aux: Sense", "1") });
    CHECK (near (value (st, "dyn.detect"), (double) ps::DetectionMode::Peak));
    CHECK (st.report.notes.empty());
    CHECK_EQ (st.report.ignored, 1);
}

TEST_CASE ("import.core. dynamics: older files with linked sets copy set A into the set B values they left out")
{
    const auto plan = importModule ("Squash", "8000", { param ("Tie", "1"),
                                                       param ("Split", "1"),
                                                       param ("Lane 1 Thr", "-20"),
                                                       param ("Lane 2 Thr", "-25"),
                                                       param ("Lane 2 Cap Thr", "-4"),
                                                       param ("Aux: Lane 1 Thr", "-15") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "dyn.link"), 1.0));
    CHECK (near (value (plan, "dyn.b.b1.comp.thr"), -15.0));       // stored
    CHECK (near (value (plan, "dyn.b.b2.comp.thr"), -25.0));       // copied from set A
    CHECK (near (value (plan, "dyn.b.b2.lim.thr"), -4.0));
    CHECK (near (value (plan, "dyn.b.b2.lim.on"), 1.0));
    CHECK (near (value (plan, "dyn.b.b3.comp.thr"), -12.0));       // the foreign default

    // Without the link switch the set B values stay at the defaults; newer files keep our link switch.
    const auto unlinked = importModule ("Squash", "8000", { param ("Lane 2 Thr", "-25") });
    CHECK (near (value (unlinked, "dyn.b.b2.comp.thr"), -12.0));
    CHECK (near (value (unlinked, "dyn.link"), 0.0));
    const auto newer = importModule ("Squash", "100000", {});
    CHECK (near (value (newer, "dyn.link"), 1.0));
}

TEST_CASE ("import.core. dynamics: the tilt amount acts in the tilt filter kind only; unsupported controls count")
{
    const auto tilt = importModule ("Squash", "100000", { param ("Key Filter", "2"), param ("Key Lean", "40") });
    CHECK (near (value (tilt, "dyn.detFilter"), 2.0));
    CHECK (near (value (tilt, "dyn.detTilt"), 1.2));
    CHECK_EQ (tilt.report.unmapped, 0);

    const auto tiltDefault = importModule ("Squash", "100000", { param ("Key Filter", "2") });
    CHECK (near (value (tiltDefault, "dyn.detTilt"), 1.5));

    const auto hp = importModule ("Squash", "100000", { param ("Key Filter", "1"), param ("Key Lean", "100"),
                                                       param ("Steep", "27") });
    CHECK (near (value (hp, "dyn.detTilt"), 0.0));
    CHECK_EQ (hp.report.unmapped, 1);                         // the crossover steepness
    CHECK_EQ (unmappedWithReason (hp, "unsupported"), 1);
    CHECK_EQ (hp.report.ignored, 1);                          // the inert tilt amount (no effect outside the tilt kind)

    const auto look = importModule ("Squash", "100000", { param ("Look", "0") });
    CHECK (near (value (look, "dyn.lookahead"), 0.0));
    CHECK_EQ (look.report.mapped, 1);
}

//==============================================================================
// Exciter
TEST_CASE ("import.core. exciter: every-band model, per-band models, absent = the default model")
{
    for (int i = 0; i < (int) ps::ExciterMode::Count; ++i)
    {
        const auto p = importModule ("Glow", "100000", { param ("Present 1", "1"), param ("Model", std::to_string (i)),
                                                        param ("Lane 2 Model", "0") });   // overridden
        REQUIRE (p.ok);
        CHECK (near (value (p, "exc.a.b1.mode"), i));
        CHECK (near (value (p, "exc.a.b2.mode"), i));
        CHECK (near (value (p, "exc.b.b1.mode"), (double) ps::ExciterMode::Triode));
        CHECK_EQ (p.report.unmapped, 0);
    }

    // "Per band" (7): each band's own model; absent ones take the default (triode). Set B has its own.
    const auto per = importModule ("Glow", "100000", { param ("Present 1", "1"), param ("Present 2", "1"),
                                                      param ("Model", "7"), param ("Lane 1 Model", "2"),
                                                      param ("Lane 3 Model", "6"), param ("Aux: Model", "3") });
    CHECK (near (value (per, "exc.bands"), 3.0));
    CHECK (near (value (per, "exc.a.b1.mode"), 2.0));
    CHECK (near (value (per, "exc.a.b2.mode"), (double) ps::ExciterMode::Triode));
    CHECK (near (value (per, "exc.a.b3.mode"), 6.0));
    CHECK (near (value (per, "exc.b.b2.mode"), 3.0));
    CHECK_EQ (per.report.unmapped, 0);

    // Oldest writers: only the every-band model, four bands.
    const auto old = importModule ("Glow", "6100", { param ("Model", "4"), param ("Lane 3 Drive", "2.5") });
    CHECK (near (value (old, "exc.bands"), 4.0));
    for (int b = 1; b <= 4; ++b)
        CHECK (near (value (old, "exc.a.b" + std::to_string (b) + ".mode"), 4.0));
    CHECK (near (value (old, "exc.a.b3.drive"), 2.5));

    // A band model outside the list: not imported, the default model instead.
    const auto bad = importModule ("Glow", "100000", { param ("Model", "7"), param ("Lane 1 Model", "9") });
    CHECK (near (value (bad, "exc.a.b1.mode"), (double) ps::ExciterMode::Triode));
    CHECK_EQ (bad.report.unmapped, 1);
}

TEST_CASE ("import.core. exciter: drive and mix, post filter, channel modes, bypass, oversampling ignored")
{
    const auto plan = importModule ("Glow", "110000", { param ("Present 1", "1"),
                                                       param ("Split", "2"),
                                                       param ("Lane 1 Drive", "3.5"),
                                                       param ("Lane 2 Blend", "45"),
                                                       param ("Aux: Lane 2 Drive", "7"),
                                                       param ("After Gain", "-4"),
                                                       param ("After Freq", "5000"),
                                                       param ("Aux: After Freq", "20000"),
                                                       param ("Off 0", "1"),
                                                       param ("Fine", "0"),
                                                       param ("After Width", "2") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "exc.ts.on"), 1.0));                 // the third mode is T/S
    CHECK (near (value (plan, "exc.a.b1.drive"), 3.5));
    CHECK (near (value (plan, "exc.a.b2.mix"), 45.0));
    CHECK (near (value (plan, "exc.b.b2.drive"), 7.0));
    CHECK (near (value (plan, "exc.a.b2.drive"), 0.0));
    CHECK (near (value (plan, "exc.post.on"), 1.0));
    CHECK (near (value (plan, "exc.post.gain"), -4.0));
    CHECK (near (value (plan, "exc.post.freq"), 5000.0, 0.5));
    CHECK (hasNote (plan, "Exciter: the second set's post filter"));
    CHECK (near (value (plan, "exc.a.b1.on"), 0.0));
    CHECK (near (value (plan, "exc.b.b1.on"), 0.0));
    CHECK (near (value (plan, "exc.os"), 1.0));                    // ours stays on
    CHECK_EQ (unmappedWithReason (plan, "unsupported"), 1);        // post filter width
    CHECK_EQ (plan.report.unmapped, 1);

    const auto ms = importModule ("Glow", "110000", { param ("Split", "1") });
    CHECK (near (value (ms, "exc.chMode"), 1.0));
    CHECK (near (value (ms, "exc.ts.on"), 0.0));
    CHECK (near (value (ms, "exc.post.on"), 0.0));                  // 0 dB post gain = no filter
}

//==============================================================================
// Maximizer
TEST_CASE ("import.core. maximizer: algorithm table by writer version, both style tables")
{
    using M = ps::MaximizerMode;
    struct Case { const char* version; int index; double ours; };
    const Case cases[] {
        { "100000", 0, (double) M::Smooth },     { "100000", 1, (double) M::Transparent },
        { "100000", 2, (double) M::Aggressive }, { "100000", 3, (double) M::Spectral },
        { "100000", 4, (double) M::LowLatency }, { "120000", 4, (double) M::Multiband },
        { "120000", 5, (double) M::LowLatency }, { "9030", 4, (double) M::LowLatency },
    };
    for (const auto& c : cases)
    {
        const auto p = importModule ("Wall", c.version, { param ("Algo", std::to_string (c.index)) });
        REQUIRE (p.ok);
        CHECK (near (value (p, "max.mode"), c.ours));
    }
    const auto unknown = importModule ("Wall", "100000", { param ("Algo", "5") });   // no index 5 before 120000
    CHECK (near (value (unknown, "max.mode"), (double) ps::MaximizerParams().mode));
    CHECK_EQ (unknown.report.unmapped, 1);

    // Aggressive styles are stored in the opposite order to ours; absent = balanced.
    using A = ps::AggressiveStyle;
    const double style3[] { (double) A::Pumping, (double) A::Balanced, (double) A::Crisp, (double) A::Clipping };
    for (int i = 0; i < 4; ++i)
        CHECK (near (value (importModule ("Wall", "100000", { param ("Style A", std::to_string (i)) }), "max.style3"),
                     style3[i]));
    CHECK (near (value (importModule ("Wall", "100000", {}), "max.style3"), (double) A::Balanced));

    for (int i = 0; i < 3; ++i)
        CHECK (near (value (importModule ("Wall", "100000", { param ("Style S", std::to_string (i)) }), "max.style4"),
                     i));
}

TEST_CASE ("import.core. maximizer: gain from the older threshold or the newer gain; absent values are foreign")
{
    const auto old = importModule ("Wall", "100000", { param ("Push Old", "-4.5"), param ("Roof", "-0.3") });
    REQUIRE (old.ok);
    CHECK (near (value (old, "max.gain"), 4.5));
    CHECK (near (value (old, "max.ceiling"), -0.3));
    CHECK (near (value (old, "max.mode"), (double) ps::MaximizerMode::Smooth));   // absent = the first algorithm
    CHECK (near (value (old, "max.truePeak"), 0.0));                              // absent = off (ours is on)
    CHECK (near (value (old, "max.clip.mode"), (double) ps::SoftClipMode::Moderate));
    CHECK (near (value (old, "max.character"), 2.0));
    CHECK_EQ (old.report.unmapped, 0);

    const auto absent = importModule ("Wall", "120000", { param ("Feel", "4") });
    CHECK (near (value (absent, "max.ceiling"), 0.0));
    CHECK (near (value (absent, "max.gain"), 0.0));
    CHECK (near (value (absent, "max.character"), 4.0));

    const auto newer = importModule ("Wall", "120000", { param ("Push", "3.2"), param ("Peaks", "1"),
                                                         param ("Snap On", "1"), param ("Snap", "120"),
                                                         param ("Clip On", "1"), param ("Clip Kind", "2"),
                                                         param ("Free Hit", "70"), param ("Free Body", "20") });
    CHECK (near (value (newer, "max.gain"), 3.2));
    CHECK (near (value (newer, "max.truePeak"), 1.0));
    CHECK (near (value (newer, "max.trans.on"), 1.0));
    CHECK (near (value (newer, "max.trans.amount"), 60.0));        // 0..200 -> 0..100 %
    CHECK (near (value (newer, "max.clip.on"), 1.0));
    CHECK (near (value (newer, "max.clip.mode"), (double) ps::SoftClipMode::Heavy));
    CHECK (near (value (newer, "max.clip.amount"), 0.0));          // the foreign default amount (0 %)
    CHECK (near (value (newer, "max.si.trans"), 70.0));
    CHECK (near (value (newer, "max.si.sus"), 20.0));
    CHECK_EQ (newer.report.unmapped, 0);

    // Upward compression: its amount only while it is on; on without an amount = the default amount (0 dB).
    const auto liftOff = importModule ("Wall", "120000", { param ("Lift", "2.5") });
    CHECK (near (value (liftOff, "max.upward"), 0.0));
    CHECK_EQ (liftOff.report.ignored, 1);
    const auto liftOn = importModule ("Wall", "120000", { param ("Lift On", "1"), param ("Lift", "2.5") });
    CHECK (near (value (liftOn, "max.upward"), 2.5));
    const auto liftDefault = importModule ("Wall", "120000", { param ("Lift On", "1") });
    CHECK (near (value (liftDefault, "max.upward"), 0.0));

    // A positive older threshold would be a negative gain: clamped to 0 and counted.
    const auto positive = importModule ("Wall", "100000", { param ("Push Old", "2") });
    CHECK (near (value (positive, "max.gain"), 0.0));
    CHECK_EQ (positive.report.clamped, 1);
}

TEST_CASE ("import.core. hostile enum exponents never reach an integer conversion")
{
    struct Case { const char* section; const char* control; };
    const Case cases[] {
        { "Squash", "Kind" }, { "Squash", "Sense" }, { "Squash", "Key Filter" },
        { "Glow", "Model" }, { "Glow", "Aux: Model" },
        { "Glow", "Lane 1 Model" }, { "Glow", "Aux: Lane 1 Model" },
        { "Wall", "Algo" }, { "Wall", "Style A" }
    };
    for (const auto& c : cases)
        for (const char* v : { "1e308", "-1e308" })
        {
            std::feclearexcept (FE_ALL_EXCEPT);
            const auto plan = importModule (c.section, "120000", { param (c.control, v) });
            const int exceptions = std::fetestexcept (FE_INVALID | FE_OVERFLOW);
            REQUIRE (plan.ok);
            CHECK_EQ (exceptions, 0);
            for (const float plain : plan.values)
                CHECK (std::isfinite (plain));
        }

    const auto badKind = importModule ("Squash", "120000", { param ("Kind", "3") });
    CHECK_EQ (badKind.report.unmapped, 1);
}

TEST_CASE ("import.core. compound controls clamp extreme finite inputs without overflow")
{
    for (const char* v : { "1e308", "-1e308" })
    {
        std::feclearexcept (FE_ALL_EXCEPT);
        const auto plan = importModule ("Squash", "120000", { param ("Split", "1"), param ("Key Filter", "2"),
                                                              param ("Key Lean", v), param ("Key Freq", "1e308"),
                                                              param ("Aux: Key Freq", "-1e308"),
                                                              param ("Lane 1 Blend", v), param ("Blend All", "50"),
                                                              param ("Out", "-1e308"), param ("Aux: Out", "1e308"),
                                                              param ("Aux: Lane 1 Level", "1e308") });
        const int exceptions = std::fetestexcept (FE_INVALID | FE_OVERFLOW);
        REQUIRE (plan.ok);
        CHECK_EQ (exceptions, 0);
        CHECK (near (value (plan, "dyn.detTilt"), v[0] == '-' ? -6.0 : 6.0));
        for (const float plain : plan.values)
            CHECK (std::isfinite (plain));
    }

    std::feclearexcept (FE_ALL_EXCEPT);
    const auto post = importModule ("Glow", "120000", { param ("Split", "1"), param ("After Gain", "1e308"),
                                                        param ("Aux: After Gain", "-1e308"),
                                                        param ("After Freq", "1e308"),
                                                        param ("Aux: After Freq", "-1e308") });
    const int exceptions = std::fetestexcept (FE_INVALID | FE_OVERFLOW);
    REQUIRE (post.ok);
    CHECK_EQ (exceptions, 0);
}

TEST_CASE ("import.core. bypassed set B bands retain their separate module output gain")
{
    const auto plan = importModule ("Squash", "120000", { param ("Split", "1"), param ("Off 0", "1"),
                                                          param ("Out", "3"), param ("Aux: Out", "7"),
                                                          param ("Lane 1 Level", "6"),
                                                          param ("Aux: Lane 1 Level", "9"),
                                                          param ("Aux: Lane 1 Thr", "-60"),
                                                          param ("Aux: Lane 1 Cap Thr", "-40"),
                                                          param ("Look", "0") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "dyn.a.b1.on"), 0.0));
    CHECK (near (value (plan, "dyn.b.b1.comp.on"), 0.0));
    CHECK (near (value (plan, "dyn.b.b1.lim.on"), 0.0));
    CHECK (near (value (plan, "dyn.b.b1.gain"), 4.0));

    ps::Dynamics dynamics;
    dynamics.prepare (48000.0, 256);
    dynamics.setParams (plan.values.data());
    ps::ProcessContext context;
    context.channelMode = ps::ChannelMode::MidSide;
    std::array<float, 256> first {}, second {};
    float* channels[] { first.data(), second.data() };
    for (int block = 0; block < 16; ++block)
    {
        first.fill (0.1f);
        second.fill (0.1f);
        dynamics.process (channels, 256, context);
    }
    CHECK (near (first.back(), 0.1 * std::pow (10.0, 3.0 / 20.0), 1.0e-5));
    CHECK (near (second.back(), 0.1 * std::pow (10.0, 7.0 / 20.0), 1.0e-5));
}

TEST_CASE ("import.core. exciter absent every-band selection uses its foreign default in each set")
{
    for (const char* version : { "6100", "9030", "120000", "0" })
    {
        const auto plan = importModule ("Glow", version, { param ("Lane 1 Model", "2"),
                                                          param ("Aux: Lane 1 Model", "6") });
        REQUIRE (plan.ok);
        CHECK (near (value (plan, "exc.a.b1.mode"), (double) ps::ExciterMode::Triode));
        CHECK (near (value (plan, "exc.b.b1.mode"), (double) ps::ExciterMode::Triode));
        CHECK_EQ (plan.report.ignored, 2);
        CHECK_EQ (plan.report.unmapped, 0);
    }

    const auto mixed = importModule ("Glow", "120000", { param ("Model", "7"), param ("Lane 1 Model", "2"),
                                                        param ("Aux: Lane 1 Model", "6") });
    CHECK (near (value (mixed, "exc.a.b1.mode"), 2.0));
    CHECK (near (value (mixed, "exc.b.b1.mode"), (double) ps::ExciterMode::Triode));
    CHECK_EQ (mixed.report.ignored, 1);
}

TEST_CASE ("import.core. invalid exciter every-band selection falls back to its foreign default")
{
    const auto plan = importModule ("Glow", "120000", { param ("Model", "99"), param ("Lane 1 Model", "2"),
                                                       param ("Aux: Model", "-4"),
                                                       param ("Aux: Lane 1 Model", "6") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "exc.a.b1.mode"), (double) ps::ExciterMode::Triode));
    CHECK (near (value (plan, "exc.b.b1.mode"), (double) ps::ExciterMode::Triode));
    CHECK_EQ (plan.report.unmapped, 2);
    CHECK_EQ (plan.report.ignored, 2);
}

TEST_CASE ("import.core. dynamics enables every negative limiter threshold in both sets")
{
    const auto plan = importModule ("Squash", "120000", { param ("Present 1", "1"),
                                                          param ("Lane 1 Cap Thr", "-0.005"),
                                                          param ("Aux: Lane 1 Cap Thr", "-0.0001"),
                                                          param ("Lane 2 Cap Thr", "0") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "dyn.a.b1.lim.on"), 1.0));
    CHECK (near (value (plan, "dyn.b.b1.lim.on"), 1.0));
    CHECK (near (value (plan, "dyn.a.b1.lim.thr"), -0.005, 1.0e-6));
    CHECK (near (value (plan, "dyn.b.b1.lim.thr"), -0.0001, 1.0e-6));
    CHECK (near (value (plan, "dyn.a.b2.lim.on"), 0.0));
    CHECK (near (value (plan, "dyn.b.b2.lim.on"), 0.0));
}

TEST_CASE ("import.core. shared dynamics controls report omitted set B defaults unless older sets are linked")
{
    for (const char* version : { "8000", "120000" })
    {
        const auto plan = importModule ("Squash", version, { param ("Split", "1"), param ("Sense", "2"),
                                                            param ("Makeup", "1") });
        REQUIRE (plan.ok);
        CHECK (hasNote (plan, "Dynamics: the second set's detection kind"));
        CHECK (hasNote (plan, "Dynamics: the second set's auto gain"));
        CHECK (near (value (plan, "dyn.detect"), (double) ps::DetectionMode::Envelope));
        CHECK (near (value (plan, "dyn.autoGain"), 1.0));
    }
    const auto linked = importModule ("Squash", "8000", { param ("Split", "1"), param ("Tie", "1"),
                                                        param ("Sense", "2"), param ("Makeup", "1") });
    REQUIRE (linked.ok);
    CHECK (! hasNote (linked, "Dynamics: the second set's detection kind"));
    CHECK (! hasNote (linked, "Dynamics: the second set's auto gain"));
    const auto explicitB = importModule ("Squash", "8000", { param ("Split", "1"), param ("Tie", "1"),
                                                           param ("Sense", "2"), param ("Aux: Sense", "1") });
    CHECK (hasNote (explicitB, "Dynamics: the second set's detection kind"));
}

TEST_CASE ("import.core. imager removed bands ignore bypass and crossover leftovers")
{
    const auto plan = importModule ("Spread", "120000", { param ("Present 2", "1"), param ("Off 1", "1"),
                                                         param ("Off 2", "1"), param ("Edge 1", "70"),
                                                         param ("Edge 2", "900"), param ("Edge 3", "8000"),
                                                         param ("Lane 2 Spread", "45"),
                                                         param ("Lane 3 Spread", "60"),
                                                         param ("Aux: Lane 3 Spread", "-20") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "img.bands"), 2.0));
    CHECK (near (value (plan, "img.xover1"), 900.0));
    CHECK (near (value (plan, "img.b2.width"), 0.0));
    CHECK (near (value (plan, "img.b.b2.width"), 0.0));
    CHECK_EQ (plan.report.ignored, 4);  // removed slot's bypass / width and the two unused crossover edges
    CHECK_EQ (plan.report.unmapped, 0);
}

TEST_CASE ("import.core. dynamics knee: stored in display units 0..10, read as dB; both knees default to 10")
{
    const auto plan = importModule ("Squash", "120000", { param ("Lane 1 Knee", "4.99411774"),
                                                         param ("Lane 1 Cap Knee", "0"),
                                                         param ("Aux: Lane 1 Knee", "0") });
    REQUIRE (plan.ok);
    CHECK (near (value (plan, "dyn.a.b1.comp.knee"), 4.994, 0.01));
    CHECK (near (value (plan, "dyn.a.b1.lim.knee"), 0.0));
    CHECK (near (value (plan, "dyn.b.b1.comp.knee"), 0.0));
    CHECK (near (value (plan, "dyn.b.b1.lim.knee"), 10.0));         // absent: the knob maximum
    CHECK_EQ (plan.report.mapped, 3);
    CHECK_EQ (plan.report.clamped, 0);
}

TEST_CASE ("import.core. maximizer, exciter and imager defaults confirmed in the real product")
{
    const auto max = importModule ("Wall", "120000", {});
    REQUIRE (max.ok);
    CHECK (near (value (max, "max.mode"), (double) ps::MaximizerMode::Smooth));
    CHECK (near (value (max, "max.style3"), (double) ps::AggressiveStyle::Balanced));
    CHECK (near (value (max, "max.style4"), (double) ps::SpectralStyle::Modern));
    CHECK (near (value (max, "max.gain"), 0.0));
    CHECK (near (value (max, "max.ceiling"), 0.0));
    CHECK (near (value (max, "max.character"), 2.0));
    CHECK (near (value (max, "max.truePeak"), 0.0));
    CHECK (near (value (max, "max.clip.on"), 0.0));
    CHECK (near (value (max, "max.clip.amount"), 0.0));
    CHECK (near (value (max, "max.upward"), 0.0));
    CHECK (near (value (max, "max.trans.on"), 0.0));
    CHECK (near (value (max, "max.trans.amount"), 0.0));
    CHECK (near (value (max, "max.si.trans"), 0.0));
    CHECK (near (value (max, "max.si.sus"), 0.0));
    CHECK (near (value (max, "max.learnTarget"), -11.0));
    CHECK_EQ (max.report.mapped, 0);

    const auto exc = importModule ("Glow", "120000", {});
    REQUIRE (exc.ok);
    CHECK (near (value (exc, "exc.bands"), 1.0));
    CHECK (near (value (exc, "exc.a.b1.mode"), (double) ps::ExciterMode::Triode));
    CHECK (near (value (exc, "exc.a.b1.drive"), 0.0));
    CHECK (near (value (exc, "exc.a.b1.mix"), 100.0));

    // Imager: one band at width 0, amount 100 %, stereoize mode I with a 6 ms delay (= 30 % of our amount), recover
    // sides at 0 dB.
    const auto img = importModule ("Spread", "120000", { param ("Wide On", "1"), param ("Rescue On", "1") });
    REQUIRE (img.ok);
    CHECK (near (value (img, "img.bands"), 1.0));
    CHECK (near (value (img, "img.b1.width"), 0.0));
    CHECK (near (value (img, "img.amount"), 100.0));
    CHECK (near (value (img, "img.stz.on"), 1.0));
    CHECK (near (value (img, "img.stz.mode"), (double) ps::StereoizeMode::ModeI));
    CHECK (near (value (img, "img.stz.amount"), 30.0));
    CHECK (near (value (img, "img.rec.on"), 1.0));
    CHECK (near (value (img, "img.rec.gain"), 0.0));
    CHECK_EQ (img.report.mapped, 2);
    CHECK_EQ (img.report.unmapped, 0);
}
