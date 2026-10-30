#pragma once

// Allocation counting for CoreTests (added by dsp-common; implementation in AllocationCounter.cpp, which replaces the
// global operator new / delete of the CoreTests binary). Only allocations made on the calling thread while a
// pstest::AllocationScope is alive are counted, so the replacement is harmless for every other test.
//
//   { pstest::AllocationScope scope; module.process (...); CHECK_EQ (scope.count(), 0); }
//
// Counts operator new (all forms). Direct malloc calls are not counted (pure C++ DSP code does not use them).
namespace pstest
{
class AllocationScope
{
public:
    AllocationScope() noexcept;
    ~AllocationScope();
    long long count() const noexcept;   // allocations on this thread since the scope started

    AllocationScope (const AllocationScope&) = delete;
    AllocationScope& operator= (const AllocationScope&) = delete;

private:
    long long start = 0;
    bool wasTracking = false;
};
} // namespace pstest
