#include "TestFramework.h"

#include "dsp/ClassicComp.h"

#include <string>

// Classic Compressor tests (dsp/ClassicComp.h). Prefix "ccmp.". Owner: mod-ccmp.
namespace
{
using namespace ps;
} // namespace

TEST_CASE ("ccmp. module type and contract registration")
{
    ClassicComp m;
    CHECK (m.getType() == ModuleType::ClassicComp);
    CHECK_EQ (std::string (moduleInfo (ModuleType::ClassicComp).idPrefix), std::string ("ccmp"));
}
