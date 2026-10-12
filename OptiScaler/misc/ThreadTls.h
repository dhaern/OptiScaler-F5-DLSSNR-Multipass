#pragma once

#include <intrin.h>

#include <atomic>

// Some processes run threads whose TEB has no static-TLS array (ThreadLocalStoragePointer == nullptr). World of
// Warcraft's Wow_loader.dll does, and calls FreeLibrary/LoadLibrary/GetProcAddress from them. MSVC dereferences that
// array for thread_local variables and for the epoch check of thread-safe function-local statics (State::Instance()
// and every `static` with a dynamic initializer), even once they are initialized, so code reached from such a thread
// faults at [nullptr + _tls_index * 8]. Hooks that can run on game/loader threads check this first and call the
// original function without touching any OptiScaler state.
// Reads the TEB directly: no statics, no CRT, no TLS of its own, safe on any thread.
inline bool ThreadHasNoTls()
{
#if defined(_M_X64) || defined(_M_ARM64EC)
    return __readgsqword(0x58) == 0;
#elif defined(_M_IX86)
    return __readfsdword(0x2C) == 0;
#else
    return false;
#endif
}

// A leaked singleton that can be reached from a thread without a static-TLS array once it exists. Get() reads a
// constant-initialized pointer, so only the first call goes through a thread-safe static. Existing() never does and
// returns nullptr until the first Get(). T makes LeakedInstance<T> a friend if its constructor is private.
template <typename T> class LeakedInstance
{
  public:
    static T& Get()
    {
        if (auto* instance = _instance.load(std::memory_order_acquire))
            return *instance;

        return Create();
    }

    static T* Existing() { return _instance.load(std::memory_order_acquire); }

  private:
    __declspec(noinline) static T& Create()
    {
        static auto* instance = new T;
        _instance.store(instance, std::memory_order_release);
        return *instance;
    }

    inline static constinit std::atomic<T*> _instance { nullptr };
};
