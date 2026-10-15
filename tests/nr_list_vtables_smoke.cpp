#include "pch.h"

// vcvars64, then cl /std:c++20 /EHsc /W4 /O2 /I OptiScaler (project include paths)
// tests\nr_list_vtables_smoke.cpp /Fe:nr_list_vtables_smoke.exe
namespace
{
bool g_refuseProtect = false;
BOOL WINAPI ProtectEntry(LPVOID address, SIZE_T bytes, DWORD protection, PDWORD previous)
{
    return !g_refuseProtect && VirtualProtect(address, bytes, protection, previous);
}
} // namespace
#define VirtualProtect ProtectEntry
#include "../OptiScaler/hooks/ListVtables.h"
#undef VirtualProtect

#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace
{
struct DepthTag;
struct SpatialTag;
constinit native::ListVtables<DepthTag> g_depth;
constinit native::ListVtables<SpatialTag> g_spatial;
using Function = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using Table = std::array<void*, 81>;
struct List
{
    void** table;
    int calls = 0, depth = 0, spatial = 0;
    ID3D12GraphicsCommandList* Cmd() { return reinterpret_cast<ID3D12GraphicsCommandList*>(this); }
    void Call(int slot = 12) { reinterpret_cast<Function>(table[slot])(Cmd()); }
};
int g_failures = 0;
void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += !ok;
}
void STDMETHODCALLTYPE Standard(ID3D12GraphicsCommandList* list) { ++reinterpret_cast<List*>(list)->calls; }
void STDMETHODCALLTYPE Private(ID3D12GraphicsCommandList* list) { reinterpret_cast<List*>(list)->calls += 2; }
void STDMETHODCALLTYPE DepthThunk(ID3D12GraphicsCommandList* list)
{
    ++reinterpret_cast<List*>(list)->depth;
    g_depth.Original(list, 12, Standard)(list);
}
void STDMETHODCALLTYPE SpatialThunk(ID3D12GraphicsCommandList* list)
{
    ++reinterpret_cast<List*>(list)->spatial;
    g_spatial.Original(list, 12, Standard)(list);
}
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
ULONG STDMETHODCALLTYPE StandardRelease(IUnknown* list)
{
    ++reinterpret_cast<List*>(list)->calls;
    return 1;
}
ULONG STDMETHODCALLTYPE PrivateRelease(IUnknown* list)
{
    reinterpret_cast<List*>(list)->calls += 2;
    return 2;
}
ReleaseFn g_release = StandardRelease;
ULONG STDMETHODCALLTYPE ReleaseBody(ReleaseFn original, IUnknown* list) { return original(list); }
using ReleaseHook = native::ListVtableHook<g_depth, 2, &g_release, ReleaseBody>;
Function g_trampoline = Standard;
void STDMETHODCALLTYPE ForwardBody(Function original, ID3D12GraphicsCommandList* list) { original(list); }
using ForwardHook = native::ListVtableHook<g_depth, 13, &g_trampoline, ForwardBody>;
void STDMETHODCALLTYPE ForwardToDetour(ID3D12GraphicsCommandList* list) { ForwardHook::Detour(list); }
} // namespace

int main()
{
    auto standard = std::make_unique<Table>();
    standard->fill(reinterpret_cast<void*>(Standard));
    (*standard)[2] = reinterpret_cast<void*>(StandardRelease);
    g_depth.Install(standard->data());
    g_depth.Hook(2, reinterpret_cast<void*>(ReleaseHook::Table));
    g_depth.Hook(12, reinterpret_cast<void*>(DepthThunk));
    g_depth.Hook(13, reinterpret_cast<void*>(ForwardHook::Table));
    for (int slot : { 68, 69, 80 })
        g_depth.Hook(slot, reinterpret_cast<void*>(DepthThunk));
    g_spatial.Install(standard->data());
    g_spatial.Hook(12, reinterpret_cast<void*>(SpatialThunk));

    List runtime { standard->data() };
    Check(g_depth.Examine(runtime.Cmd()), "the installation table takes the fast path");
    auto table = std::make_unique<Table>(*standard);
    List list { table->data() };
    Check(g_depth.Examine(list.Cmd()) && *table == *standard && g_depth.Original(list.Cmd(), 12, Standard) == Standard,
          "a different table with standard entries stays untouched and uses the trampoline");
    (*table)[12] = reinterpret_cast<void*>(Private);
    bool first = false;
    Check(g_depth.Examine(list.Cmd(), &first) && first && (*table)[12] == reinterpret_cast<void*>(DepthThunk),
          "a private entry is patched and announced once");
    list.Call();
    Check(list.calls == 2 && list.depth == 1, "the thunk forwards to the recorded entry");
    (*table)[12] = reinterpret_cast<void*>(Private);
    Check(g_depth.Examine(list.Cmd(), &first) && !first && (*table)[12] == reinterpret_cast<void*>(DepthThunk),
          "a table restored at the same address is patched again without another announcement");
    auto counts = g_depth.TakeCounts();
    Check(counts.tables == 1 && counts.patched == 1 && counts.repatched == 1 && counts.failed == 0,
          "tables and patches are cumulative, re-patches are counted for the interval");
    Check(g_depth.TakeCounts().repatched == 0, "taking interval counters clears them");
    (*table)[12] = reinterpret_cast<void*>(Standard);
    Check(g_depth.Examine(list.Cmd()) && (*table)[12] == reinterpret_cast<void*>(Standard),
          "restoring the runtime entry leaves the code detour in charge");
    (*table)[12] = nullptr;
    Check(!g_depth.Examine(list.Cmd()) && g_depth.TakeCounts().failed == 1,
          "a missing entry is refused and counted, not mistaken for an outer hook");
    (*table)[12] = reinterpret_cast<void*>(Private);
    Check(g_depth.Examine(list.Cmd()), "a refused entry can recover");

    for (int slot : { 13, 68, 69, 80 })
        (*table)[slot] = reinterpret_cast<void*>(slot == 13 ? ForwardToDetour : Private);
    Check(g_depth.Examine(list.Cmd()), "List4 render-pass and List7 Barrier slots can be hooked");
    for (int slot : { 68, 69, 80 })
        Check((*table)[slot] == reinterpret_cast<void*>(DepthThunk) &&
                  g_depth.Original(list.Cmd(), slot, Standard) == Private,
              "the high slot keeps its own original");
    list.calls = 0;
    list.Call(13);
    Check(list.calls == 1, "a private entry forwarding into a code detour uses the trampoline without looping");

    (*table)[2] = reinterpret_cast<void*>(PrivateRelease);
    Check(g_depth.Examine(list.Cmd()) &&
              reinterpret_cast<ReleaseFn>((*table)[2])(reinterpret_cast<IUnknown*>(list.Cmd())) == 2,
          "IUnknown-shaped Release thunks preserve the list's table original");

    std::vector<Table> chainTables(2, *standard);
    for (int order = 0; order != 2; ++order)
    {
        auto& t = chainTables[order];
        t[12] = reinterpret_cast<void*>(Private);
        List chained { t.data() };
        bool complete = order == 0 ? g_depth.Examine(chained.Cmd()) && g_spatial.Examine(chained.Cmd())
                                   : g_spatial.Examine(chained.Cmd()) && g_depth.Examine(chained.Cmd());
        void* outer = t[12];
        for (int sighting = 0; sighting != 4; ++sighting)
            complete = g_depth.Examine(chained.Cmd()) && g_spatial.Examine(chained.Cmd()) && complete;
        chained.Call();
        Check(complete && t[12] == outer && chained.calls == 2 && chained.depth == 1 && chained.spatial == 1,
              order == 0 ? "depth then spatial: the outer wrapper stays intact, each client runs once"
                         : "spatial then depth: the outer wrapper stays intact, each client runs once");
    }
    Check(g_depth.TakeCounts().foreign == 1 && g_spatial.TakeCounts().foreign == 1,
          "foreign wrapping is counted once per table, not on every call");

    std::vector<Table> many(3000, *standard);
    const auto before = g_depth.TakeCounts();
    bool all = true;
    for (auto& t : many)
    {
        t[12] = reinterpret_cast<void*>(Private);
        List item { t.data() };
        all = g_depth.Examine(item.Cmd()) && all;
        item.Call();
        all = item.calls == 2 && item.depth == 1 && all;
    }
    counts = g_depth.TakeCounts();
    Check(all && counts.tables - before.tables == many.size() && counts.patched - before.patched == many.size() &&
              counts.failed == 0,
          "3000 live heap tables are all hooked, no small fixed capacity");

    Table* memory =
        static_cast<Table*>(VirtualAlloc(nullptr, 32 * sizeof(Table), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (memory == nullptr)
        return 1;
    std::span<Table> shared(memory, 32);
    for (auto& t : shared)
        new (&t) Table(*standard);
    for (auto& t : shared)
        t[12] = reinterpret_cast<void*>(Private);
    DWORD protection = 0;
    Check(VirtualProtect(memory, shared.size_bytes(), PAGE_READONLY, &protection) != FALSE,
          "private tables can share read-only pages");
    std::atomic<bool> start { false };
    std::array<std::thread, 4> threads;
    for (auto& thread : threads)
        thread = std::thread(
            [&]
            {
                while (!start.load(std::memory_order_acquire))
                {
                }
                for (auto& t : shared)
                {
                    List item { t.data() };
                    g_depth.Examine(item.Cmd());
                }
            });
    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
        thread.join();
    all = true;
    for (auto& t : shared)
    {
        List item { t.data() };
        all = g_depth.Examine(item.Cmd()) && all;
        item.Call();
        all = item.calls == 2 && item.depth == 1 && all;
    }
    Check(all, "concurrent sightings on shared read-only pages retry without waiting and keep their originals");
    MEMORY_BASIC_INFORMATION info {};
    Check(VirtualQuery(memory, &info, sizeof(info)) == sizeof(info) && info.Protect == PAGE_READONLY &&
              info.RegionSize >= shared.size_bytes(),
          "all table pages have their read-only protection back");
    g_depth.TakeCounts();
    VirtualFree(memory, 0, MEM_RELEASE);

    auto refused = std::make_unique<Table>(*standard);
    (*refused)[12] = reinterpret_cast<void*>(Private);
    List retry { refused->data() };
    g_refuseProtect = true;
    Check(!g_depth.Examine(retry.Cmd()) && (*refused)[12] == reinterpret_cast<void*>(Private) &&
              g_depth.TakeCounts().failed == 1,
          "VirtualProtect refusal preserves the entry and is counted");
    g_refuseProtect = false;
    Check(g_depth.Examine(retry.Cmd()) && (*refused)[12] == reinterpret_cast<void*>(DepthThunk),
          "a writable-again table is retried rather than silently abandoned");
    std::printf("%s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
