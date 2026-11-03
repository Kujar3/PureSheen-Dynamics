#include "TestFramework.h"

#include "dsp/ClassicLimiter.h"

#include <string>

// Classic Limiter tests (dsp/ClassicLimiter.h). Prefix "clim.". Owner: mod-clim.
namespace
{
using namespace ps;
} // namespace

TEST_CASE ("clim. module type and contract registration")
{
    ClassicLimiter m;
    CHECK (m.getType() == ModuleType::ClassicLimiter);
    CHECK_EQ (std::string (moduleInfo (ModuleType::ClassicLimiter).idPrefix), std::string ("clim"));
}
