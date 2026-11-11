#include "TestFramework.h"

#include "core/NumberText.h"

#include <cstring>
#include <string>

// core/NumberText.h: locale-free double <-> text. Windows uses std::from_chars / to_chars, Apple a classic-locale stream;
// both must accept and reject the same text (the macOS build runs these too).
namespace
{
bool parse (const char* s, double& v) { return ps::parseDoubleText (s, s + std::strlen (s), v); }
} // namespace

TEST_CASE ("numtext. parses plain decimals and rejects everything else")
{
    double v = 0.0;
    CHECK (parse ("0.5", v) && v == 0.5);
    CHECK (parse ("-12.25", v) && v == -12.25);
    CHECK (parse (".5", v) && v == 0.5);
    CHECK (parse ("1e3", v) && v == 1000.0);
    CHECK (parse ("2.5E-2", v) && v == 0.025);
    CHECK (parse ("48000", v) && v == 48000.0);

    for (const char* bad : { "", "+1", " 1", "1 ", "1,5", "nan", "inf", "-inf", "0x10", "1e", "abc", "1.2.3", "--1", "1e999" })
        CHECK (! parse (bad, v));
}

TEST_CASE ("numtext. formatting round-trips exactly and stays short")
{
    for (double x : { 0.0, 0.1, -0.5, 1.0 / 3.0, 48000.0, 44100.0 / 7.0, 1.0e-12, 123456789.125, 2.5e20 })
    {
        const auto s = ps::formatDoubleText (x);
        double back = 1.0;
        CHECK (ps::parseDoubleText (s.data(), s.data() + s.size(), back));
        CHECK (back == x);
        CHECK (s.find (',') == std::string::npos);
    }
    CHECK (ps::formatDoubleText (0.1) == "0.1");
    CHECK (ps::formatDoubleText (48000.0) == "48000");
}
