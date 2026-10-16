#pragma once
#include <atomic>

namespace d3d12_binding
{
// Shared by proxy binding and every export-hook entry, including direct startup callers.
inline constinit std::atomic_flag owner = ATOMIC_FLAG_INIT;
class Claim
{
    bool acquired = !owner.test_and_set(std::memory_order_acquire);

  public:
    explicit operator bool() const noexcept { return acquired; }
    ~Claim()
    {
        if (acquired)
            owner.clear(std::memory_order_release);
    }
    Claim() = default;
    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;
};
} // namespace d3d12_binding
