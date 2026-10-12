// Host check of ThreadHasNoTls (OptiScaler/misc/ThreadTls.h) and of why the loader hooks need it.
// Some processes call FreeLibrary/LoadLibrary/GetProcAddress from threads whose TEB has no static-TLS array (World of
// Warcraft's Wow_loader.dll does). A hook that reaches a thread-safe function-local static there (State::Instance())
// faults: MSVC reads the init epoch through that array, cold or already initialised. This test runs the hook pattern
// with the TLS pointer of its own thread cleared for the duration of one call, against a stand-in original.
// x64 MSVC, keep thread-safe statics on (the default), no game needed:
// cl /std:c++20 /EHsc /W4 /O2 tests/no_tls_guard_smoke.cpp
// Adding /DNO_TLS_GUARD removes the guard from the stand-in hook: the test must then fail (RED).
// Also checks LeakedInstance (State::Instance()), which static destructors reach on such a thread during ExitProcess.
#include "../OptiScaler/misc/ThreadTls.h"

#include <windows.h>

#include <cstdio>

#if !defined(_M_X64)
#error This test reads and writes the x64 TEB directly
#endif

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

static volatile int seed = 7;
static volatile long initialisations = 0;
static volatile long originalCalls = 0;
static HMODULE originalArg = nullptr;

// Same shape as State::Instance(): a function-local static with a dynamic initializer
__declspec(noinline) static int Initialise()
{
    ++initialisations;
    return seed;
}

__declspec(noinline) static int TouchState()
{
    static int value = Initialise();
    return value;
}

__declspec(noinline) static BOOL WINAPI Original(HMODULE module)
{
    ++originalCalls;
    originalArg = module;
    return 0x1357;
}

static BOOL(WINAPI* volatile o_FreeLibrary)(HMODULE) = Original;

// Same shape as KernelHooks::hk_K32_FreeLibrary / NtdllHooks::hkLdrUnloadDll
__declspec(noinline) static BOOL WINAPI Hook(HMODULE module)
{
#ifndef NO_TLS_GUARD
    if (ThreadHasNoTls())
        return o_FreeLibrary(module);
#endif

    if (TouchState() != seed)
        return FALSE;

    return o_FreeLibrary(module);
}

struct Result
{
    DWORD exception = 0;
    BOOL value = FALSE;
    bool sawNoTls = false;
};

// Clears this thread's TLS pointer around one hook call and always puts it back
__declspec(noinline) static void CallWithTls(bool clearTls, HMODULE module, Result& r)
{
    const unsigned __int64 saved = __readgsqword(0x58);
    __try
    {
        if (clearTls)
            __writegsqword(0x58, 0);

        r.sawNoTls = ThreadHasNoTls();
        r.value = Hook(module);
    }
    __finally
    {
        __writegsqword(0x58, saved);
    }
}

static volatile long constructions = 0;

struct Leaked
{
    int value;
    Leaked() : value(seed) { ++constructions; }
};

__declspec(noinline) static int ExistingValue()
{
    auto* instance = LeakedInstance<Leaked>::Existing();
    return instance != nullptr ? instance->value : -1;
}

__declspec(noinline) static int GetValue() { return LeakedInstance<Leaked>::Get().value; }

struct Probe
{
    DWORD exception = 0;
    int value = 0;
};

__declspec(noinline) static void ProbeWithTls(bool clearTls, int (*fn)(), Probe& p)
{
    const unsigned __int64 saved = __readgsqword(0x58);
    __try
    {
        if (clearTls)
            __writegsqword(0x58, 0);

        p.value = fn();
    }
    __finally
    {
        __writegsqword(0x58, saved);
    }
}

static Probe RunProbe(bool clearTls, int (*fn)())
{
    Probe p;
    __try
    {
        ProbeWithTls(clearTls, fn, p);
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        p.exception = EXCEPTION_ACCESS_VIOLATION;
    }
    return p;
}

static Result Call(bool clearTls, HMODULE module)
{
    Result r;
    __try
    {
        CallWithTls(clearTls, module, r);
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        r.exception = EXCEPTION_ACCESS_VIOLATION;
    }
    return r;
}

int main()
{
    const unsigned __int64 tls = __readgsqword(0x58);
    if (tls == 0)
    {
        printf("SKIP: this thread has no TLS array to begin with\n");
        return 77;
    }

    CHECK(!ThreadHasNoTls());

    int token = 0;
    const auto module = reinterpret_cast<HMODULE>(&token);

    // TLS-less thread before the static was ever touched: straight to the original, no OptiScaler state
    auto r = Call(true, module);
    CHECK(r.sawNoTls);
    CHECK(r.exception == 0);
    CHECK(r.value == 0x1357);
    CHECK(originalCalls == 1 && originalArg == module);
    CHECK(initialisations == 0);
    CHECK(__readgsqword(0x58) == tls);

    // Normal thread: the hook's own path still runs
    r = Call(false, module);
    CHECK(!r.sawNoTls);
    CHECK(r.exception == 0);
    CHECK(r.value == 0x1357);
    CHECK(originalCalls == 2);
    CHECK(initialisations == 1);

    // TLS-less thread after the static is initialised: the epoch check would still read TLS
    r = Call(true, module);
    CHECK(r.sawNoTls);
    CHECK(r.exception == 0);
    CHECK(r.value == 0x1357);
    CHECK(originalCalls == 3);
    CHECK(initialisations == 1);
    CHECK(__readgsqword(0x58) == tls);

    // The thread-safe static itself still faults there, which is why State::Instance() no longer is one
    auto p = RunProbe(true, TouchState);
    CHECK(p.exception == EXCEPTION_ACCESS_VIOLATION);
    CHECK(__readgsqword(0x58) == tls);

    // LeakedInstance before the first Get(): Existing() answers nullptr without TLS and creates nothing
    p = RunProbe(true, ExistingValue);
    CHECK(p.exception == 0 && p.value == -1);
    CHECK(constructions == 0);

    // Created on a normal thread, then reachable without TLS through both entry points
    p = RunProbe(false, GetValue);
    CHECK(p.exception == 0 && p.value == seed);
    CHECK(constructions == 1);

    p = RunProbe(true, GetValue);
    CHECK(p.exception == 0 && p.value == seed);
    p = RunProbe(true, ExistingValue);
    CHECK(p.exception == 0 && p.value == seed);
    CHECK(constructions == 1);
    CHECK(__readgsqword(0x58) == tls);

    if (fails == 0)
        printf("no_tls_guard_smoke: all checks passed\n");

    return fails == 0 ? 0 : 1;
}
