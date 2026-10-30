#include "TestFramework.h"

#include "dsp/Params.h"

#include <cstdint>
#include <cstdio>
#include <string>

// Compatibility of the released parameter table (prefix "params."). The table is append-only: every entry released
// in 0.1.1 keeps its index, id, kind, range, default, skew, step, choices, unit and module, so host automation, saved
// projects and presets of 0.1.x keep their meaning. The fingerprint below was taken from the 0.1.1 table.
namespace
{
using namespace ps;

constexpr int kReleasedCount_0_1_1 = 479;             // number of entries in the 0.1.1 table
constexpr std::uint64_t kReleasedHash_0_1_1 = 12387188370879499014ull;    // FNV-1a over the fields of those entries

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

std::uint64_t fingerprint (int count)
{
    Fnv f;
    const auto& specs = paramSpecs();
    for (int i = 0; i < count && i < (int) specs.size(); ++i)
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
