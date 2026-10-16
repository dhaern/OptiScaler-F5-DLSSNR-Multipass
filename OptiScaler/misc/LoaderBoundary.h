#pragma once

#include <string>
#include <string_view>
#include <new>
#include <stdexcept>
#include <atomic>
#include <windows.h>

namespace loader_boundary
{
struct ThreadScope
{
    std::atomic<DWORD> thread { 0 };
    unsigned depth = 0; // Accessed only by the owning thread.
};
inline constinit ThreadScope scopes[128];
inline constinit std::atomic_uint overflowScopes { 0 };

inline bool OriginalOnly() noexcept
{
    if (overflowScopes.load(std::memory_order_acquire) != 0)
        return true;
    const auto thread = GetCurrentThreadId();
    for (auto& scope : scopes)
        if (scope.thread.load(std::memory_order_acquire) == thread)
            return true;
    return false;
}

// Bounded nesting, including Win32 -> native calls, without static TLS or waiting.
class OriginalOnlyScope
{
    ThreadScope* slot = nullptr;

  public:
    OriginalOnlyScope() noexcept
    {
        const auto thread = GetCurrentThreadId();
        for (auto& scope : scopes)
        {
            if (scope.thread.load(std::memory_order_acquire) == thread)
            {
                slot = &scope;
                ++slot->depth;
                return;
            }
        }
        for (auto& scope : scopes)
        {
            DWORD empty = 0;
            if (scope.thread.compare_exchange_strong(empty, thread, std::memory_order_acq_rel))
            {
                slot = &scope;
                slot->depth = 1;
                return;
            }
        }
        // Capacity loss only narrows interception; never let an original-only nested call escape.
        overflowScopes.fetch_add(1, std::memory_order_acq_rel);
    }
    ~OriginalOnlyScope()
    {
        if (slot)
        {
            if (--slot->depth == 0)
                slot->thread.store(0, std::memory_order_release);
        }
        else
            overflowScopes.fetch_sub(1, std::memory_order_acq_rel);
    }
    OriginalOnlyScope(const OriginalOnlyScope&) = delete;
    OriginalOnlyScope& operator=(const OriginalOnlyScope&) = delete;
};

inline bool NonExecutable(DWORD flags) noexcept
{
    return (flags & (DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                     LOAD_LIBRARY_AS_IMAGE_RESOURCE)) != 0;
}

// Retained veto identities cannot be confused with a subsequently reused HMODULE.
// A preexisting image remains legacy policy, not proof that its initialization ran.
class KnownNoInitImages
{
    std::atomic<HMODULE> images[64] {};
    std::atomic_bool unavailable { false };

  public:
    bool Contains(HMODULE module) const noexcept
    {
        if (unavailable.load(std::memory_order_acquire))
            return true;
        for (const auto& image : images)
            if (image.load(std::memory_order_acquire) == module && module != nullptr)
                return true;
        return false;
    }
    template <class Retain, class Release>
    void Record(HMODULE module, HMODULE previous, Retain retain, Release release) noexcept
    {
        if (module == nullptr || (reinterpret_cast<ULONG_PTR>(module) & 3) != 0 || module == previous ||
            Contains(module))
            return;
        const auto retained = retain(module);
        if (retained == nullptr)
        {
            unavailable.store(true, std::memory_order_release);
            return;
        }
        for (auto& image : images)
        {
            HMODULE empty = nullptr;
            if (image.compare_exchange_strong(empty, retained, std::memory_order_acq_rel))
                return;
            if (empty == retained)
            {
                release(retained);
                return;
            }
        }
        unavailable.store(true, std::memory_order_release);
        release(retained);
    }
};

// Only name acquisition is recoverable here. Never catch an installer after partial publication.
inline bool CopyName(std::wstring_view view, std::wstring& name) noexcept
{
    try
    {
        name.assign(view);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    catch (const std::length_error&)
    {
        return false;
    }
}
} // namespace loader_boundary
