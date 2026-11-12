#include "TestFramework.h"

#include <cstring>

// Usage: CoreTests [--speed] [substring-filter]
//   --speed   count the speed checks (CHECK_SPEED) as failures too. Run it alone on an idle machine.
int main (int argc, char** argv)
{
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--speed") == 0)
            pstest::speedChecksRequested() = true;
        else if (filter == nullptr)
            filter = argv[i];
    }
    int run = 0, failedCases = 0;

    for (auto& t : pstest::registry())
    {
        if (filter != nullptr && std::strstr (t.name, filter) == nullptr)
            continue;

        ++run;
        const int before = pstest::failureCount();
        std::cout << "[ RUN  ] " << t.name << "\n";

        try
        {
            t.fn();
        }
        catch (const pstest::RequireFailed&)
        {
        }
        catch (const std::exception& e)
        {
            pstest::reportFailure (t.file, 0, std::string ("uncaught exception: ") + e.what());
        }

        if (pstest::failureCount() != before)
            ++failedCases;
    }

    std::cout << "\n" << run << " test cases, " << failedCases << " failed, " << pstest::failureCount() << " failed checks\n";
    if (pstest::speedMissCount() > 0)
        std::cout << pstest::speedMissCount() << " speed checks missed, not counted (CoreTests --speed counts them)\n";
    return failedCases == 0 ? 0 : 1;
}
