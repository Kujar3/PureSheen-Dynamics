#include "TestFramework.h"

#include "dsp/PassiveEq.h"

#include <string>

// Passive EQ tests (dsp/PassiveEq.h). Prefix "peq.". Owner: mod-peq.
namespace
{
using namespace ps;
} // namespace

TEST_CASE ("peq. module type and contract registration")
{
    PassiveEq m;
    CHECK (m.getType() == ModuleType::PassiveEq);
    CHECK_EQ (std::string (moduleInfo (ModuleType::PassiveEq).idPrefix), std::string ("peq"));
}
