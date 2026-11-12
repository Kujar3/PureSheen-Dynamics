#pragma once

// Minimal dependency-free test framework for the core tests.
//
//   TEST_CASE ("voicing is ascending") { CHECK (x < y); CHECK_EQ (a, b); REQUIRE (ok); }
//
// CHECK* failures are recorded and the test continues; REQUIRE* failures abort the current test case.

#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace pstest
{
struct TestCase
{
    const char* name;
    const char* file;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount()
{
    static int n = 0;
    return n;
}

struct RequireFailed {};

struct Registrar
{
    Registrar (const char* name, const char* file, std::function<void()> fn) { registry().push_back ({ name, file, std::move (fn) }); }
};

inline void reportFailure (const char* file, int line, const std::string& message)
{
    ++failureCount();
    std::cout << "    FAIL " << file << ":" << line << "  " << message << "\n";
}

#ifdef _MSC_VER
 #pragma warning (push)
 #pragma warning (disable : 4996)   // getenv: read once, before any thread starts
#endif
inline bool envSet (const char* name)
{
    const char* v = std::getenv (name);
    return v != nullptr && *v != '\0' && *v != '0';
}
#ifdef _MSC_VER
 #pragma warning (pop)
#endif

// Speed checks count only when asked for: `CoreTests --speed` or PURESHEEN_SPEED_CHECKS=1, on a machine that is doing
// nothing else. In a normal run a miss is printed and not counted, so "0 failed" does not depend on the machine's load.
// PURESHEEN_SLOW_MACHINE (the shared CI runners) keeps them uncounted even then.
inline bool& speedChecksRequested()
{
    static bool on = envSet ("PURESHEEN_SPEED_CHECKS");
    return on;
}

inline bool slowMachine()
{
    static const bool slow = envSet ("PURESHEEN_SLOW_MACHINE");
    return slow;
}

inline bool speedChecksCount() { return speedChecksRequested() && ! slowMachine(); }

inline int& speedMissCount()
{
    static int n = 0;
    return n;
}

template <typename T>
std::string show (const T& v)
{
    std::ostringstream os;
    if constexpr (std::is_enum_v<T>)
        os << (int) v;
    else
        os << v;
    return os.str();
}

template <typename T>
std::string show (const std::vector<T>& v)
{
    std::ostringstream os;
    os << "{";
    for (size_t i = 0; i < v.size(); ++i)
        os << (i ? ", " : "") << show (v[i]);
    os << "}";
    return os.str();
}
} // namespace pstest

#define CB_CONCAT2(a, b) a##b
#define CB_CONCAT(a, b) CB_CONCAT2 (a, b)

#define TEST_CASE(name)                                                                                   \
    static void CB_CONCAT (cbtest_fn_, __LINE__)();                                                       \
    static pstest::Registrar CB_CONCAT (cbtest_reg_, __LINE__) (name, __FILE__, &CB_CONCAT (cbtest_fn_, __LINE__)); \
    static void CB_CONCAT (cbtest_fn_, __LINE__)()

#define CHECK(cond)                                                                    \
    do { if (! (cond)) pstest::reportFailure (__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)

#define CHECK_EQ(a, b)                                                                                         \
    do {                                                                                                       \
        const auto& cb_a_ = (a);                                                                               \
        const auto& cb_b_ = (b);                                                                               \
        if (! (cb_a_ == cb_b_))                                                                                \
            pstest::reportFailure (__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")  got " + pstest::show (cb_a_) \
                                                           + " vs " + pstest::show (cb_b_));                   \
    } while (0)

// A speed (or thread-timing) check: counted only in a speed run (see speedChecksRequested). Otherwise a miss is printed
// with the numbers still in the log, and the summary line says how many there were.
#define CHECK_SPEED(cond)                                                                                  \
    do {                                                                                                   \
        if (! (cond))                                                                                      \
        {                                                                                                  \
            if (pstest::speedChecksCount())                                                                \
                pstest::reportFailure (__FILE__, __LINE__, "CHECK_SPEED(" #cond ")");                      \
            else                                                                                           \
            {                                                                                              \
                ++pstest::speedMissCount();                                                                \
                std::cout << "    speed, not counted: " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
            }                                                                                              \
        }                                                                                                  \
    } while (0)

#define REQUIRE(cond)                                                                    \
    do { if (! (cond)) { pstest::reportFailure (__FILE__, __LINE__, "REQUIRE(" #cond ")"); throw pstest::RequireFailed{}; } } while (0)
