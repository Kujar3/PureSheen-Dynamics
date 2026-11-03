#include "TestFramework.h"

#include "dsp/Params.h"

#include <cstdint>
#include <cstdio>
#include <string>

// Compatibility of the released parameter table (prefix "params."). The table is append-only: every entry released
// in 0.1.1 keeps its index, id, kind, range, default, skew, step, choices, unit and module, so host automation, saved
// projects and presets of 0.1.x keep their meaning. The fingerprints below were taken from the 0.1.1 table, the 0.2 ..
// 0.5 table (0.1.1 + the phase 2 appendix) and the vintage appendix (phase 3, not released yet: when a (TBC) range or
// default of Params.h is confirmed before the release, update kVintageHash with the value the test prints).
namespace
{
using namespace ps;

constexpr int kReleasedCount_0_1_1 = 479;             // number of entries in the 0.1.1 table
constexpr std::uint64_t kReleasedHash_0_1_1 = 12387188370879499014ull;    // FNV-1a over the fields of those entries
constexpr int kReleasedCount_0_5_0 = 841;             // entries of the 0.2 .. 0.5 table (phase 2 appendix included)
constexpr std::uint64_t kReleasedHash_0_5_0 = 5899188831216511340ull;
constexpr int kVintageCount = 79;                     // the vintage appendix: ccmp, peq, clim, tape
constexpr std::uint64_t kVintageHash = 194287539300555884ull;   // FNV-1a over the fields of the appendix entries only

struct Fnv
{
    std::uint64_t h = 1469598103934665603ull;

    void bytes (const void* data, size_t n)
    {
        const auto* p = static_cast<const unsigned char*> (data);
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }

    void text (const std::string& s)
    {
        bytes (s.data(), s.size());
        const char sep = '\x1f';
        bytes (&sep, 1);
    }

    void number (float v)
    {
        char buf[64];
        std::snprintf (buf, sizeof (buf), "%.9g", (double) v);
        text (buf);
    }
};

// FNV-1a over the fields of entries [first, first + count).
std::uint64_t fingerprint (int first, int count)
{
    Fnv f;
    const auto& specs = paramSpecs();
    for (int i = first; i < first + count && i < (int) specs.size(); ++i)
    {
        const auto& s = specs[(size_t) i];
        f.text (s.id);
        f.text (s.name);
        f.number ((float) (int) s.kind);
        f.number (s.minValue);
        f.number (s.maxValue);
        f.number (s.defaultValue);
        f.number (s.skewCentre);
        f.number (s.interval);
        f.number ((float) s.choices.size());
        for (const auto& c : s.choices)
            f.text (c);
        f.text (s.unit);
        f.number ((float) s.module);
    }
    return f.h;
}

std::uint64_t fingerprint (int count) { return fingerprint (0, count); }
} // namespace

TEST_CASE ("params. 0.1.1 table unchanged (append-only)")
{
    const int count = (int) paramSpecs().size();
    std::printf ("    0.1.1 fingerprint: current table %d entries, first %d hash %llu, full hash %llu\n", count,
                 kReleasedCount_0_1_1, (unsigned long long) fingerprint (kReleasedCount_0_1_1),
                 (unsigned long long) fingerprint (count));
    REQUIRE (count >= kReleasedCount_0_1_1);
    CHECK_EQ (fingerprint (kReleasedCount_0_1_1), kReleasedHash_0_1_1);
    CHECK_EQ (paramSpecs()[(size_t) kReleasedCount_0_1_1 - 1].id, std::string ("dith.dc"));
}

TEST_CASE ("params. 0.5.0 table unchanged (append-only)")
{
    const int count = (int) paramSpecs().size();
    std::printf ("    0.5.0 fingerprint: first %d hash %llu\n", kReleasedCount_0_5_0,
                 (unsigned long long) fingerprint (kReleasedCount_0_5_0));
    REQUIRE (count >= kReleasedCount_0_5_0);
    CHECK_EQ (kReleasedCount_0_5_0, kNumParams_0_5_0);
    CHECK_EQ (fingerprint (kReleasedCount_0_5_0), kReleasedHash_0_5_0);
    CHECK_EQ (paramSpecs()[(size_t) kReleasedCount_0_5_0 - 1].id, std::string ("bass.autoGain"));
}

TEST_CASE ("params. vintage appendix: count and fingerprint")
{
    const auto& specs = paramSpecs();
    const int count = (int) specs.size();
    const int appendix = count - kReleasedCount_0_5_0;
    std::printf ("    vintage appendix: %d entries (%s .. %s), hash %llu\n", appendix,
                 specs[(size_t) kReleasedCount_0_5_0].id.c_str(), specs.back().id.c_str(),
                 (unsigned long long) fingerprint (kReleasedCount_0_5_0, appendix));
    CHECK_EQ (appendix, kVintageCount);
    CHECK_EQ (fingerprint (kReleasedCount_0_5_0, kVintageCount), kVintageHash);
    CHECK_EQ (specs[(size_t) kReleasedCount_0_5_0].id, std::string ("ccmp.on"));
    CHECK_EQ (specs.back().id, std::string ("tape.highEmph"));

    // every appendix entry belongs to one of the four modules, in ModuleType order
    int last = -1;
    for (int i = kReleasedCount_0_5_0; i < count; ++i)
    {
        const int m = specs[(size_t) i].module;
        CHECK (m >= kFirstVintageModuleType && m < kNumModuleTypes);
        CHECK (m >= last);
        last = m;
    }
}
