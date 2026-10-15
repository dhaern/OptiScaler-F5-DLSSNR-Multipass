"""Build/run the actual seven loader-wrapper bodies with safe API stand-ins.

No Windows loader call is made with an artificially cleared TLS pointer. Source
extraction keeps the tested ABI bodies identical to production without linking
all graphics providers into this host test. Run with a vcvars64 environment:
  python tests/loader_wrapper_contract.py <scratch-output-directory>
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def function_body(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


PREAMBLE = r'''
#include <windows.h>
#include <winternl.h>
#include <intrin.h>
#include <string>
#include <string_view>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <new>
#include "LoaderBoundary.h"
static bool failAllocation = false;
void* operator new(size_t n) { if (failAllocation) throw std::bad_alloc(); if (auto p = malloc(n)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
#ifndef NT_SUCCESS
#define NT_SUCCESS(x) ((x) >= 0)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000dL)
#endif
#ifndef STATUS_DLL_NOT_FOUND
#define STATUS_DLL_NOT_FOUND ((NTSTATUS)0xc0000135L)
#endif
#define LOG_TRACE(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
static bool noTls = false, skipChecks = false, allocateAfterOriginal = false;
struct Config { struct Option { bool value_or_default() const { return false; } } UseNtdllHooks; static Config* Instance() { static Config c; return &c; } };
static bool ThreadHasNoTls() { return noTls; }
static HMODULE originalModule = reinterpret_cast<HMODULE>(0x20000);
static int failures = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static std::wstring string_to_wstring(const std::string& s) { return {s.begin(), s.end()}; }
struct State {
    bool isShuttingDown = false;
    static State& Instance() { static State s; return s; }
    static bool SkipDllChecks() { return skipChecks; }
    static std::string SkipDllName() { return {}; }
};
struct Util { static HMODULE GetCallerModule(void*) { return nullptr; } };
static HMODULE speculative = nullptr;
static HMODULE actual = reinterpret_cast<HMODULE>(0x10000);
static bool failOwn = true;
static int originalCalls = 0, checks = 0, observations = 0, marks = 0;
static DWORD originalIncoming = 0, expectedOriginalError = 0x5678;
static NTSTATUS originalStatus = 0x123;
static std::wstring checkedName;
static LPCWSTR seenW=nullptr; static LPCSTR seenA=nullptr;
static HANDLE seenFile=nullptr; static DWORD seenFlags=0;
static void* seenSearch=nullptr; static PULONG seenNativeFlags=nullptr;
static PUNICODE_STRING seenUnicode=nullptr; static PHANDLE seenOutput=nullptr;
static bool writeOutput=true;
struct LibraryLoadHooks {
    static void CatchUpDeferredLoads() { SetLastError(0xaaaa); }
    static void RecordNonExecutable(HMODULE, HMODULE) {}
    static void RecordDeferredLoad(HMODULE m, bool = true) { if (m) ++marks; }
    static bool IsApiSetName(std::wstring_view n) { return n.starts_with(L"api-ms-"); }
    static bool EndsWithInsensitive(std::wstring_view, std::wstring_view) { return false; }
    static HMODULE LoadLibraryCheckW(std::wstring name, LPCWSTR, bool* failed) {
        ++checks; checkedName = std::move(name); *failed = failOwn;
        SetLastError(0xbbbb); return speculative;
    }
    static void HandleCallerModule(const std::wstring&, LPCWSTR, HMODULE m) {
        CHECK(m == actual); ++observations; SetLastError(0xcccc);
    }
};
static HMODULE OriginalW(LPCWSTR n) {
    seenW=n;
    ++originalCalls; originalIncoming = GetLastError(); SetLastError(expectedOriginalError); failAllocation = allocateAfterOriginal; return actual;
}
static HMODULE OriginalA(LPCSTR n) { seenA=n; return OriginalW(nullptr); }
static HMODULE OriginalExW(LPCWSTR n, HANDLE f, DWORD flags) { seenFile=f; seenFlags=flags; return OriginalW(n); }
static HMODULE OriginalExA(LPCSTR n, HANDLE f, DWORD flags) { seenFile=f; seenFlags=flags; return OriginalA(n); }
static NTSTATUS NTAPI OriginalLdr(PWSTR path, PULONG flags, PUNICODE_STRING n, PHANDLE out) {
    seenSearch=path; seenNativeFlags=flags; seenUnicode=n; seenOutput=out;
    OriginalW(nullptr); if (out && writeOutput) *out = actual; return originalStatus;
}
static NTSTATUS NTAPI OriginalNt(PUNICODE_STRING path, PULONG flags, PUNICODE_STRING n, PHANDLE out) {
    const auto status=OriginalLdr(nullptr,flags,n,out); seenSearch=path; return status;
}
struct KernelHooks {
    inline static auto o_K32_LoadLibraryW = OriginalW;
    inline static auto o_K32_LoadLibraryA = OriginalA;
    inline static auto o_K32_LoadLibraryExW = OriginalExW;
    inline static auto o_K32_LoadLibraryExA = OriginalExA;
    inline static auto o_KB_LoadLibraryExW = OriginalExW;
    static HMODULE hk_K32_LoadLibraryW(LPCWSTR);
    static HMODULE hk_K32_LoadLibraryA(LPCSTR);
    static HMODULE hk_K32_LoadLibraryExW(LPCWSTR, HANDLE, DWORD);
    static HMODULE hk_K32_LoadLibraryExA(LPCSTR, HANDLE, DWORD);
    static HMODULE hk_KB_LoadLibraryExW(LPCWSTR, HANDLE, DWORD);
};
'''

MAIN = r'''
static void Reset() {
    checks = observations = marks = originalCalls = 0; writeOutput=true;
    checkedName.clear(); speculative = nullptr; actual = reinterpret_cast<HMODULE>(0x10000);
    noTls = skipChecks = allocateAfterOriginal = failAllocation = false; failOwn = true; originalStatus = 0x123; expectedOriginalError = 0x5678;
    SetLastError(0x1234);
}
static void CheckOriginalOutcome(int expectedObservations = 1) {
    CHECK(originalCalls == 1); CHECK(observations == expectedObservations);
    CHECK(originalIncoming == 0x1234); CHECK(GetLastError() == expectedOriginalError);
}
int main() {
    wchar_t name[] = L"d3d12.dll";
    UNICODE_STRING u {18, sizeof(name), name};
    HANDLE out = nullptr;
    Reset(); CHECK(KernelHooks::hk_K32_LoadLibraryW(name) == actual); CheckOriginalOutcome();
    Reset(); CHECK(KernelHooks::hk_K32_LoadLibraryA("d3d12.dll") == actual); CheckOriginalOutcome();
    Reset(); CHECK(KernelHooks::hk_K32_LoadLibraryExW(name, nullptr, 0) == actual); CheckOriginalOutcome();
    Reset(); CHECK(KernelHooks::hk_K32_LoadLibraryExA("d3d12.dll", nullptr, 0) == actual); CheckOriginalOutcome();
    Reset(); CHECK(KernelHooks::hk_KB_LoadLibraryExW(name, nullptr, 0) == actual); CheckOriginalOutcome();
    Reset(); CHECK(NtdllHooks::hkLdrLoadDll(nullptr, nullptr, &u, &out) == originalStatus);
    CHECK(out == actual); CheckOriginalOutcome(0);
    Reset(); CHECK(NtdllHooks::hkNtLoadDll(nullptr, nullptr, &u, &out) == originalStatus);
    CHECK(out == actual); CheckOriginalOutcome(0);
    ULONG nativeFlags=0x1000; wchar_t search[]=L"C:/original-search";
    Reset(); NtdllHooks::hkLdrLoadDll(search,&nativeFlags,&u,&out);
    CHECK(seenSearch==search && seenNativeFlags==&nativeFlags && seenUnicode==&u && seenOutput==&out);
    CHECK(checks==0); CHECK(originalIncoming==0x1234);
    Reset(); NtdllHooks::hkNtLoadDll(&u,&nativeFlags,&u,&out);
    CHECK(seenSearch==&u && seenNativeFlags==&nativeFlags && seenUnicode==&u && seenOutput==&out);
    Reset(); originalStatus=STATUS_DLL_NOT_FOUND; writeOutput=false;out=reinterpret_cast<HANDLE>(0x7777);
    NtdllHooks::hkLdrLoadDll(search,nullptr,&u,&out);
    CHECK(out==reinterpret_cast<HANDLE>(0x7777)); CHECK(observations==0); CHECK(GetLastError()==expectedOriginalError);
    Reset(); auto file=reinterpret_cast<HANDLE>(0x9999);
    KernelHooks::hk_K32_LoadLibraryExW(name,file,0x1000);CHECK(seenW==name && seenFile==file && seenFlags==0x1000);
    Reset(); const char* ansi="d3d12.dll";
    KernelHooks::hk_K32_LoadLibraryExA(ansi,file,0x1000);CHECK(seenA==ansi && seenFile==file && seenFlags==0x1000);
    Reset(); KernelHooks::hk_KB_LoadLibraryExW(name,file,0x1000);CHECK(seenW==name && seenFile==file && seenFlags==0x1000);
    // The historical sentinel behavior is deliberately different at these ABI boundaries.
    Reset(); speculative = reinterpret_cast<HMODULE>(1337); failOwn = false;
    CHECK(KernelHooks::hk_K32_LoadLibraryW(name) == actual); CHECK(originalCalls == 1);
    Reset(); speculative = reinterpret_cast<HMODULE>(1337); failOwn = false;
    out = reinterpret_cast<HANDLE>(0x7777);
    CHECK(NtdllHooks::hkLdrLoadDll(nullptr, nullptr, &u, &out) == STATUS_DLL_NOT_FOUND);
    CHECK(originalCalls == 0); CHECK(out == reinterpret_cast<HANDLE>(0x7777));
    Reset(); actual = nullptr; originalStatus = STATUS_DLL_NOT_FOUND;
    CHECK(NtdllHooks::hkLdrLoadDll(nullptr, nullptr, &u, &out) == originalStatus);
    CHECK(out == nullptr); CHECK(observations == 0); CHECK(GetLastError() == expectedOriginalError);
    // Counted native names need not have a terminator at Length.
    wchar_t bounded[] = L"d3d12.dll.trailing";
    UNICODE_STRING counted {18, sizeof(bounded), bounded};
    Reset(); NtdllHooks::hkLdrLoadDll(nullptr, nullptr, &counted, &out);
    CHECK(checkedName == L"d3d12.dll");
    Reset(); NtdllHooks::hkNtLoadDll(nullptr, nullptr, &counted, &out);
    CHECK(checkedName == L"d3d12.dll");
    for (DWORD flags : {DONT_RESOLVE_DLL_REFERENCES, LOAD_LIBRARY_AS_DATAFILE,
                        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE, LOAD_LIBRARY_AS_IMAGE_RESOURCE}) {
        Reset(); CHECK(KernelHooks::hk_K32_LoadLibraryExW(name, reinterpret_cast<HANDLE>(0x9999), flags) == actual);
        CHECK(checks == 0); CHECK(observations == 0); CHECK(originalCalls == 1);
        CHECK(originalIncoming == 0x1234); CHECK(GetLastError() == expectedOriginalError);
        Reset(); CHECK(KernelHooks::hk_K32_LoadLibraryExA("d3d12.dll", nullptr, flags) == actual);
        CHECK(checks == 0); CHECK(observations == 0);
        Reset(); CHECK(KernelHooks::hk_KB_LoadLibraryExW(name, nullptr, flags) == actual);
        CHECK(checks == 0); CHECK(observations == 0);
    }
    // The ABI originals remain safe stand-ins; exercise nesting without invoking a real loader with cleared TLS.
    {
        loader_boundary::OriginalOnlyScope outer;
        Reset(); NtdllHooks::hkLdrLoadDll(nullptr,nullptr,&u,&out); CHECK(checks==0); CHECK(observations==0);
        NtdllHooks::hkNtLoadDll(nullptr,nullptr,&u,&out); CHECK(checks==0);
    }
    for (int which=0; which!=2; ++which) {
        for (UNICODE_STRING malformed : {UNICODE_STRING{1,sizeof(name),name},
             UNICODE_STRING{20,18,name}, UNICODE_STRING{18,18,nullptr}, UNICODE_STRING{0,0,nullptr}}) {
            Reset();
            if (!which) NtdllHooks::hkLdrLoadDll(nullptr,nullptr,&malformed,&out);
            else NtdllHooks::hkNtLoadDll(nullptr,nullptr,&malformed,&out);
            CHECK(originalCalls==1); CHECK(checks==0); CHECK(originalIncoming==0x1234);
        }
        wchar_t embedded[] = L"d3d12.dll\0ignored";
        UNICODE_STRING embeddedName{static_cast<USHORT>(sizeof(embedded)-2),sizeof(embedded),embedded};
        Reset();
        if (!which) NtdllHooks::hkLdrLoadDll(nullptr,nullptr,&embeddedName,&out);
        else NtdllHooks::hkNtLoadDll(nullptr,nullptr,&embeddedName,&out);
        CHECK(checks==0); CHECK(originalCalls==1);
        Reset(); skipChecks=true;
        if (!which) NtdllHooks::hkLdrLoadDll(nullptr,nullptr,&u,&out);
        else NtdllHooks::hkNtLoadDll(nullptr,nullptr,&u,&out);
        CHECK(checks==0); CHECK(originalIncoming==0x1234);
    }
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    auto memory=static_cast<wchar_t*>(VirtualAlloc(nullptr,static_cast<size_t>(info.dwPageSize)*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    CHECK(memory!=nullptr);
    if(memory) {
        auto guard=reinterpret_cast<BYTE*>(memory)+info.dwPageSize; DWORD old=0;
        CHECK(VirtualProtect(guard,info.dwPageSize,PAGE_NOACCESS,&old));
        auto end=reinterpret_cast<wchar_t*>(guard)-9; memcpy(end,L"d3d12.dll",18);
        UNICODE_STRING pageName{18,18,end};
        Reset();NtdllHooks::hkLdrLoadDll(nullptr,nullptr,&pageName,&out);CHECK(checkedName==L"d3d12.dll");
        Reset();NtdllHooks::hkNtLoadDll(nullptr,nullptr,&pageName,&out);CHECK(checkedName==L"d3d12.dll");
        CHECK(VirtualFree(memory,0,MEM_RELEASE));
    }
    Reset(); allocateAfterOriginal=true;
    bool postThrew=false;
    try { KernelHooks::hk_K32_LoadLibraryW(name); } catch(const std::bad_alloc&) { postThrew=true; }
    failAllocation=false; CHECK(!postThrew); CHECK(observations==1); CHECK(GetLastError()==expectedOriginalError);
    // Allocation failure must never escape an original-only native bypass.
    wchar_t api[] = L"api-ms-win-core-libraryloader-test-long-name.dll";
    UNICODE_STRING apiName {static_cast<USHORT>(sizeof(api) - sizeof(wchar_t)), sizeof(api), api};
    for (int which = 0; which != 2; ++which) {
        Reset(); State::Instance().isShuttingDown = true;
        bool threw = false; failAllocation = true;
        try {
            if (which == 0) NtdllHooks::hkLdrLoadDll(nullptr, nullptr, &apiName, &out);
            else NtdllHooks::hkNtLoadDll(nullptr, nullptr, &apiName, &out);
        } catch (const std::bad_alloc&) { threw = true; }
        failAllocation = false; State::Instance().isShuttingDown = false;
        CHECK(!threw); CHECK(originalCalls == 1); CHECK(checks == 0);
        CHECK(originalIncoming == 0x1234); CHECK(GetLastError() == expectedOriginalError);
    }
    if (!failures) printf("loader_wrapper_contract: original outcomes, counted names and sentinel characterization passed\n");
    return failures ? 1 : 0;
}
'''


def main():
    out = Path(sys.argv[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    revision = sys.argv[2] if len(sys.argv) > 2 else None
    def source_at(path):
        if revision:
            return subprocess.run(["git", "-C", str(ROOT), "show", f"{revision}:{path}"],
                                  capture_output=True, text=True, check=True).stdout
        return (ROOT / path).read_text(encoding="utf-8-sig")
    print(f"source_revision={revision or 'working-tree'}", flush=True)
    kernel = source_at("OptiScaler/hooks/Kernel_Hooks.cpp")
    native = source_at("OptiScaler/hooks/Ntdll_Hooks.h")
    parts = [PREAMBLE]
    if 'static bool NativeLoadOnly(' in kernel:
        parts += [function_body(kernel, 'template <class C> static bool NativeLoadOnly(')]
    parts += [function_body(kernel, "static inline HMODULE CheckLoad(")]
    parts += [function_body(kernel, f"HMODULE KernelHooks::{name}(") for name in (
        "hk_K32_LoadLibraryW", "hk_K32_LoadLibraryA", "hk_K32_LoadLibraryExW",
        "hk_K32_LoadLibraryExA", "hk_KB_LoadLibraryExW")]
    parts += ["struct NtdllHooks { inline static auto o_LdrLoadDll = OriginalLdr; inline static auto o_NtLoadDll = OriginalNt;"]
    parts += [function_body(native, f"static NTSTATUS NTAPI {name}(") for name in ("hkLdrLoadDll", "hkNtLoadDll")]
    parts += ["};", MAIN]
    source = out / "loader_wrapper_contract.generated.cpp"
    source.write_text("\n\n".join(parts), encoding="utf-8")
    exe = out / "loader_wrapper_contract.exe"
    command = ["cl", "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/O2", str(source),
               f"/Fe:{exe}", f"/Fo:{out / 'loader_wrapper_contract.obj'}", f"/I{ROOT / 'OptiScaler/misc'}"]
    print(subprocess.list2cmdline(command), flush=True)
    result = subprocess.run(command, capture_output=True, text=True)
    (out / "compile.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    print(result.stdout, result.stderr, f"compile_exit={result.returncode}", flush=True)
    if result.returncode:
        return result.returncode
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    (out / "run.log").write_text(result.stdout + result.stderr + f"\nprocess_exit={result.returncode}\n", encoding="utf-8")
    print(result.stdout, result.stderr, f"process_exit={result.returncode}", flush=True)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
