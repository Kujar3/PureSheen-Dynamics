#pragma once

#include <cmath>
#include <string>

#if defined(__APPLE__)
 #include <iomanip>
 #include <locale>
 #include <sstream>
#else
 #include <charconv>
 #include <system_error>
#endif

// Locale-independent conversion between double and text, for saved state and imported files (a host may set a locale
// that writes "0,5"). Elsewhere this is std::to_chars / std::from_chars. Apple's C++ library only has their floating-point
// versions from macOS 13.3 (to_chars) and 26 (from_chars), so Apple builds use a stream in the classic locale instead,
// which keeps older macOS versions supported. Pure C++17.
namespace ps
{
// Parses all of [first, last) as a finite decimal number (no leading '+' or space, no "inf" / "nan", no hex).
inline bool parseDoubleText (const char* first, const char* last, double& out) noexcept
{
    if (first == nullptr || first >= last || *first == '+' || *first == ' ')
        return false;

   #if defined(__APPLE__)
    try
    {
        for (const char* c = first; c < last; ++c)
            if (! ((*c >= '0' && *c <= '9') || *c == '-' || *c == '+' || *c == '.' || *c == 'e' || *c == 'E'))
                return false;

        std::istringstream in (std::string (first, last));
        in.imbue (std::locale::classic());
        double v = 0.0;
        in >> v;
        if (in.fail() || in.peek() != std::char_traits<char>::eof() || ! std::isfinite (v))
            return false;
        out = v;
        return true;
    }
    catch (...)
    {
        return false;
    }
   #else
    double v = 0.0;
    const auto r = std::from_chars (first, last, v, std::chars_format::general);
    if (r.ec != std::errc() || r.ptr != last || ! std::isfinite (v))
        return false;
    out = v;
    return true;
   #endif
}

// The shortest decimal text that reads back as exactly `v`.
inline std::string formatDoubleText (double v)
{
   #if defined(__APPLE__)
    for (int precision = 15; precision <= 17; ++precision)
    {
        std::ostringstream text;
        text.imbue (std::locale::classic());
        text << std::setprecision (precision) << v;
        const auto s = text.str();
        double back = 0.0;
        if (precision == 17 || (parseDoubleText (s.data(), s.data() + s.size(), back) && back == v))
            return s;
    }
    return {};
   #else
    char buffer[64] {};
    const auto r = std::to_chars (buffer, buffer + sizeof (buffer) - 1, v);
    return std::string (buffer, r.ptr);
   #endif
}
} // namespace ps
