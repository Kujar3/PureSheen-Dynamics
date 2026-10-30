#include "AllocationCounter.h"

#include <cstdlib>
#include <new>

// Global operator new / delete replacement for CoreTests (see AllocationCounter.h).
namespace
{
thread_local bool tlTracking = false;
thread_local long long tlCount = 0;

void* allocate (std::size_t size)
{
    if (tlTracking)
        ++tlCount;
    if (size == 0)
        size = 1;
    if (void* p = std::malloc (size))
        return p;
    throw std::bad_alloc();
}

void* allocateAligned (std::size_t size, std::size_t alignment)
{
    if (tlTracking)
        ++tlCount;
    if (size == 0)
        size = 1;
#ifdef _MSC_VER
    if (void* p = _aligned_malloc (size, alignment))
        return p;
#else
    if (void* p = std::aligned_alloc (alignment, (size + alignment - 1) / alignment * alignment))
        return p;
#endif
    throw std::bad_alloc();
}

void freeAligned (void* p) noexcept
{
#ifdef _MSC_VER
    _aligned_free (p);
#else
    std::free (p);
#endif
}
} // namespace

void* operator new (std::size_t size) { return allocate (size); }
void* operator new[] (std::size_t size) { return allocate (size); }
void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocate (size); } catch (...) { return nullptr; }
}
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocate (size); } catch (...) { return nullptr; }
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { std::free (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { std::free (p); }

void* operator new (std::size_t size, std::align_val_t a) { return allocateAligned (size, (std::size_t) a); }
void* operator new[] (std::size_t size, std::align_val_t a) { return allocateAligned (size, (std::size_t) a); }
void* operator new (std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept
{
    try { return allocateAligned (size, (std::size_t) a); } catch (...) { return nullptr; }
}
void* operator new[] (std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept
{
    try { return allocateAligned (size, (std::size_t) a); } catch (...) { return nullptr; }
}
void operator delete (void* p, std::align_val_t) noexcept { freeAligned (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { freeAligned (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { freeAligned (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { freeAligned (p); }
void operator delete (void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned (p); }
void operator delete[] (void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned (p); }

namespace pstest
{
AllocationScope::AllocationScope() noexcept
    : start (tlCount), wasTracking (tlTracking)
{
    tlTracking = true;
}

AllocationScope::~AllocationScope()
{
    tlTracking = wasTracking;
}

long long AllocationScope::count() const noexcept { return tlCount - start; }
} // namespace pstest
