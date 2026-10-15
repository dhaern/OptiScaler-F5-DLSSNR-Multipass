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
static bool noTls = false;
static bool ThreadHasNoTls() { return noTls; }
static HMODULE originalModule = reinterpret_cast<HMODULE>(0x20000);
static int failures = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static std::wstring string_to_wstring(const std::string& s) { return {s.begin(), s.end()}; }
struct State {
    bool isShuttingDown = false;
    static State& Instance() { static State s; return s; }
    static bool SkipDllChecks() { return false; }
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
struct LibraryLoadHooks {
    static void CatchUpDeferredLoads() { SetLastError(0xaaaa); }
    static void RecordDeferredLoad(HMODULE m) { if (m) ++marks; }
    static bool IsApiSetName(std::wstring_view) { return false; }
    static bool EndsWithInsensitive(std::wstring_view, std::wstring_view) { return false; }
    static HMODULE LoadLibraryCheckW(std::wstring name, LPCWSTR, bool* failed) {
        ++checks; checkedName = std::move(name); *failed = failOwn;
        SetLastError(0xbbbb); return speculative;
    }
    static void HandleCallerModule(std::wstring, LPCWSTR, HMODULE m) {
        CHECK(m == actual); ++observations; SetLastError(0xcccc);
    }
};
static HMODULE OriginalW(LPCWSTR) {
    ++originalCalls; originalIncoming = GetLastError(); SetLastError(expectedOriginalError); return actual;
}
static HMODULE OriginalA(LPCSTR) { return OriginalW(nullptr); }
static HMODULE OriginalExW(LPCWSTR, HANDLE, DWORD) { return OriginalW(nullptr); }
static HMODULE OriginalExA(LPCSTR, HANDLE, DWORD) { return OriginalW(nullptr); }
static NTSTATUS NTAPI OriginalLdr(PWSTR, PULONG, PUNICODE_STRING, PHANDLE out) {
    OriginalW(nullptr); if (out) *out = actual; return originalStatus;
}
static NTSTATUS NTAPI OriginalNt(PUNICODE_STRING, PULONG, PUNICODE_STRING, PHANDLE out) {
    return OriginalLdr(nullptr, nullptr, nullptr, out);
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
    checks = observations = marks = originalCalls = 0;
    checkedName.clear(); speculative = nullptr; actual = reinterpret_cast<HMODULE>(0x10000);
    noTls = false; failOwn = true; originalStatus = 0x123; expectedOriginalError = 0x5678;
    SetLastError(0x1234);
}
static void CheckOriginalOutcome() {
    CHECK(originalCalls == 1); CHECK(observations == 1);
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
    CHECK(out == actual); CheckOriginalOutcome();
    Reset(); CHECK(NtdllHooks::hkNtLoadDll(nullptr, nullptr, &u, &out) == originalStatus);
    CHECK(out == actual); CheckOriginalOutcome();
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
               f"/Fe:{exe}", f"/Fo:{out / 'loader_wrapper_contract.obj'}"]
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
