#include "TestFramework.h"

#include <cstring>

// Usage: CoreTests [substring-filter]
int main (int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
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
    return failedCases == 0 ? 0 : 1;
}
