#pragma once

#include <atomic>

// Publishing a missed load must not need a TLS array, a lock, or dynamic initialization.
class DeferredModuleScan
{
  public:
    void Mark() { _pending.store(true, std::memory_order_relaxed); }

    template <class F> void Run(F scan)
    {
        if (!_pending.load(std::memory_order_relaxed))
            return;

        // Keep both recursive hook calls and concurrent consumers out of the scan. A new load during the scan
        // stays pending rather than starting another scan inside an init/hook function.
        if (_running.test_and_set(std::memory_order_acquire))
            return;

        struct Reset
        {
            std::atomic_flag& running;
            ~Reset() { running.clear(std::memory_order_release); }
        } reset { _running };

        if (_pending.exchange(false, std::memory_order_relaxed))
            scan();
    }

  private:
    static_assert(std::atomic_bool::is_always_lock_free);
    std::atomic_bool _pending { false };
    std::atomic_flag _running {};
};
