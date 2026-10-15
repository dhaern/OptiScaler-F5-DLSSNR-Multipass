// x64 MSVC: cl /std:c++20 /EHsc /W4 /WX /O2 tests/deferred_module_scan_smoke.cpp
// /DNO_DEFERRED_MODULE_SCAN models the old TLS-less bypass and must fail (RED).
#include "../OptiScaler/misc/ThreadTls.h"
#ifndef NO_DEFERRED_MODULE_SCAN
#include "../OptiScaler/misc/DeferredModuleScan.h"
#endif

#include <windows.h>
#include <cstdio>
#include <thread>
#include <vector>

#ifdef NO_DEFERRED_MODULE_SCAN
struct DeferredModuleScan
{
    void Mark() {}
    template <class F> void Run(F scan) { (void) scan; }
};
#endif

static constinit DeferredModuleScan deferred;
static int scans = 0;
static int failures = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

// The original call is a stand-in: the Windows loader itself is not safe with an artificially cleared TLS array.
__declspec(noinline) static HMODULE Original(HMODULE module) { return module; }
static HMODULE (*volatile original)(HMODULE) = Original;

__declspec(noinline) static HMODULE LoadHook(HMODULE module)
{
    if (ThreadHasNoTls())
    {
        auto result = original(module);
        if (result != nullptr)
            deferred.Mark();
        return result;
    }

    deferred.Run([] { ++scans; });
    return original(module);
}

static void TlslessLoad(HMODULE module)
{
    const auto saved = __readgsqword(0x58);
    __try
    {
        __writegsqword(0x58, 0);
        LoadHook(module);
    }
    __finally
    {
        __writegsqword(0x58, saved);
    }
}

int main()
{
    if (ThreadHasNoTls())
        return 77;

    int token = 0;
    auto module = reinterpret_cast<HMODULE>(&token);
    LoadHook(module);
    CHECK(scans == 0);
    TlslessLoad(nullptr);
    LoadHook(module);
    CHECK(scans == 0);
    TlslessLoad(module);
    TlslessLoad(module);
    CHECK(scans == 0);
    LoadHook(module);
    CHECK(scans == 1);
    LoadHook(module);
    CHECK(scans == 1);

    // Reentry must not recurse, and work published during a scan must survive for the next hook call.
    deferred.Mark();
    deferred.Run(
        []
        {
            ++scans;
            deferred.Mark();
            deferred.Run([] { ++scans; });
        });
    CHECK(scans == 2);
    deferred.Run([] { ++scans; });
    CHECK(scans == 3);
    deferred.Run([] { ++scans; });
    CHECK(scans == 3);

    std::atomic_int concurrentScans { 0 };
    std::atomic_int active { 0 };
    std::atomic_int overlaps { 0 };
    deferred.Mark();
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i)
        threads.emplace_back(
            [&]
            {
                deferred.Run(
                    [&]
                    {
                        if (active.fetch_add(1) != 0)
                            ++overlaps;
                        ++concurrentScans;
                        std::this_thread::yield();
                        --active;
                    });
            });
    for (auto& thread : threads)
        thread.join();
    CHECK(concurrentScans == 1);
    CHECK(overlaps == 0);

    if (failures == 0)
        printf("deferred_module_scan_smoke: all checks passed (TLS-less loads, coalescing, reentry, concurrent "
               "consumers)\n");
    return failures == 0 ? 0 : 1;
}
