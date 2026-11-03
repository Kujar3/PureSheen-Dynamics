#include "TestFramework.h"

#include "dsp/Tape.h"

#include <string>

// Tape tests (dsp/Tape.h). Prefix "tape.". Owner: mod-tape.
namespace
{
using namespace ps;
} // namespace

TEST_CASE ("tape. module type and contract registration")
{
    Tape m;
    CHECK (m.getType() == ModuleType::Tape);
    CHECK_EQ (std::string (moduleInfo (ModuleType::Tape).idPrefix), std::string ("tape"));
}
