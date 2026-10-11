#pragma once

#include <intrin.h>

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
