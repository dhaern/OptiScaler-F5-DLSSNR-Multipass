"""Reproduce the pre-hook no-init discovery boundary with a real isolated DLL.

The exact production GetDllNameWModule/CheckModulesInMemory bodies are compiled.
Provider hook entries are counting stand-ins: this probe never detours or calls
code in an uninitialized third-party provider. Exit 0 confirms the blocker is
present, NOT that executable-load classification or Plan 2 passes.
Usage after vcvars64: python tests/loader_noinit_discovery_probe.py <scratch-dir>
"""
from pathlib import Path
import subprocess
import sys

from loader_wrapper_contract import ROOT, function_body

FIXTURE = r'''
#include <windows.h>
extern "C" { __declspec(dllexport) volatile LONG ProbeAttachCount = 0; }
BOOL WINAPI DllMain(HINSTANCE, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) {
        ProbeAttachCount = 1;
        auto callback = reinterpret_cast<void (WINAPI*)()>(GetProcAddress(GetModuleHandleW(nullptr), "Plan2LoaderCallout"));
        if (callback) callback();
    }
    return TRUE;
}
'''
PREAMBLE = r'''
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <cstdio>
#define LOG_DEBUG(...) ((void)0)
static int failures = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s error=%lu\n", __LINE__, #x, GetLastError()); ++failures; } } while (0)
struct KernelBaseProxy {
    static auto GetModuleHandleW_() { return &GetModuleHandleW; }
};
static std::vector<std::wstring> slInterposerNamesW {L"sl.interposer.dll"};
static std::vector<std::wstring> slDlssNamesW, slDlssgNamesW, slReflexNamesW, slPclNamesW, slCommonNamesW;
static HMODULE slInterposerModule = nullptr;
enum class FGOutput { NoFG, DLSSG };
struct State {
    HMODULE optiSlInterposer = nullptr, optiSlDLSSG = nullptr, optiSlReflex = nullptr;
    HMODULE optiSlPCL = nullptr, optiSlCommon = nullptr;
    bool isShuttingDown = false;
    static bool SkipDllChecks() { return false; }
    FGOutput activeFgOutput = FGOutput::NoFG;
    static State& Instance() { static State s; return s; }
};
struct Config {
    std::optional<std::filesystem::path> MainDllPath {L"C:/unrelated-opti-path"};
    static Config* Instance() { static Config c; return &c; }
};
struct Util {
    static bool IsSubpath(const std::filesystem::path&, const std::filesystem::path&) { return false; }
};
static int interposerHookEntries = 0;
static HMODULE observed = nullptr;
struct StreamlineHooks {
    static bool isInterposerHooked() { return false; }
    static bool isDlssHooked() { return true; }
    static bool isDlssgHooked() { return true; }
    static bool isLocalDlssgHooked() { return true; }
    static bool isReflexHooked() { return true; }
    static bool isPclHooked() { return true; }
    static bool isCommonHooked() { return true; }
    static void hookInterposer(HMODULE m) { ++interposerHookEntries; observed = m; }
    static void hookDlss(HMODULE) {}
    static void hookDlssg(HMODULE) {}
    static void hookLocalDlssg(HMODULE) {}
    static void hookReflex(HMODULE) {}
    static void hookPcl(HMODULE) {}
    static void hookCommon(HMODULE) {}
};
#include "ThreadTls.h"
#include "DeferredModuleScan.h"
struct DxgiProxy { static bool InsideFactoryCreation() { return false; } };
static int scansInsideDllMain = 0;
struct LibraryLoadHooks {
    inline static constinit DeferredModuleScan _deferredScan;
    static void CheckModulesInMemory();
    static void CatchUpDeferredLoads();
    static void ScanLoadedModules() { ++scansInsideDllMain; }
};
'''
CALLOUT = r'''
extern "C" __declspec(dllexport) void WINAPI Plan2LoaderCallout() {
    printf("real_dllmain_callback_has_tls=%d\n", !ThreadHasNoTls());
    LibraryLoadHooks::_deferredScan.Mark();
    LibraryLoadHooks::CatchUpDeferredLoads();
}
'''
MAIN = r'''
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HMODULE noinit = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(noinit != nullptr);
    if (!noinit) return 2;
    auto* attached = reinterpret_cast<volatile LONG*>(GetProcAddress(noinit, "ProbeAttachCount"));
    CHECK(attached && *attached == 0);
    CHECK((reinterpret_cast<ULONG_PTR>(noinit) & 3) == 0);
    CHECK(GetModuleHandleW(L"sl.interposer.dll") == noinit);
    wchar_t path[32768] {};
    CHECK(GetModuleFileNameW(noinit, path, 32768) != 0);
    HMODULE retained = nullptr;
    CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(noinit), &retained));
    CHECK(retained == noinit);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    CHECK(snapshot != INVALID_HANDLE_VALUE);
    bool enumerated = false;
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry {}; entry.dwSize = sizeof(entry);
        for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry))
            if (entry.hModule == noinit) enumerated = true;
        CloseHandle(snapshot);
    }
    CHECK(enumerated);
    // Exact source-backed startup discovery has no executable-initialization test.
    LibraryLoadHooks::CheckModulesInMemory();
    CHECK(interposerHookEntries == 1 && observed == noinit && slInterposerModule == noinit);
    CHECK(attached && *attached == 0);
    HMODULE ordinary = LoadLibraryW(argv[1]);
    CHECK(ordinary == noinit);
    CHECK(attached && *attached == 0);
    printf("noinit_untagged=%d snapshot_enumerated=%d startup_hook_entries=%d ordinary_same_image=%d attach_count=%ld\n",
           (reinterpret_cast<ULONG_PTR>(noinit) & 3) == 0, enumerated, interposerHookEntries,
           ordinary == noinit, attached ? *attached : -1);
    if (ordinary) CHECK(FreeLibrary(ordinary));
    if (retained) CHECK(FreeLibrary(retained));
    CHECK(FreeLibrary(noinit));
    HMODULE initialized = LoadLibraryW(argv[1]);
    CHECK(initialized != nullptr);
    if (initialized) {
        auto* count = reinterpret_cast<volatile LONG*>(GetProcAddress(initialized, "ProbeAttachCount"));
        CHECK(count && *count == 1);
        CHECK(FreeLibrary(initialized));
    }
    CHECK(scansInsideDllMain == 1);
    printf("source_backed_scan_entries_from_real_dllmain=%d\n", scansInsideDllMain);
    if (!failures) printf("BLOCKERS REPRODUCED: startup discovery offers an uninitialized image to a provider hook; TLS-capable DllMain reaches the scanner. All probe references released.\n");
    return failures ? 1 : 0;
}
'''


def main():
    out = Path(sys.argv[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    revision = sys.argv[2] if len(sys.argv) > 2 else None
    def source_at(path):
        if revision:
            return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{revision}:{path}"], text=True)
        return (ROOT / path).read_text(encoding="utf-8-sig")
    if not revision or "ObserveOrdinaryModule" in source_at("OptiScaler/hooks/LibraryLoad_Hooks.cpp"):
        from loader_boundary_contract import main as boundary_main
        return boundary_main()
    for header in ("ThreadTls.h", "DeferredModuleScan.h"):
        (out / header).write_text(source_at("OptiScaler/misc/" + header), encoding="utf-8")
    fixture = out / "noinit_fixture.cpp"
    fixture.write_text(FIXTURE, encoding="utf-8")
    dll = out / "sl.interposer.dll"
    sources = source_at("OptiScaler/hooks/LibraryLoad_Hooks.cpp")
    names = source_at("OptiScaler/DllNames.h")
    code = "\n\n".join([PREAMBLE,
        function_body(names, "inline static HMODULE GetDllNameWModule("),
        function_body(sources, "void LibraryLoadHooks::CheckModulesInMemory()"),
        function_body(sources, "void LibraryLoadHooks::CatchUpDeferredLoads()"), CALLOUT, MAIN])
    source = out / "noinit_discovery.generated.cpp"
    source.write_text(code, encoding="utf-8")
    exe = out / "noinit_discovery.exe"
    commands = [
        ["cl", "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/O2", "/LD", str(fixture),
         f"/Fe:{dll}", f"/Fo:{out / 'noinit_fixture.obj'}", "/link", f"/IMPLIB:{out / 'noinit_fixture.lib'}"],
        ["cl", "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/O2",
         f"/I{out}", str(source),
         f"/Fe:{exe}", f"/Fo:{out / 'noinit_discovery.obj'}"],
        [str(exe), str(dll)],
    ]
    transcript = []
    for command in commands:
        print(subprocess.list2cmdline(command), flush=True)
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        text = result.stdout + result.stderr + f"\nprocess_exit={result.returncode}\n"
        print(text, flush=True)
        transcript.append(subprocess.list2cmdline(command) + "\n" + text)
        (out / "probe.log").write_text("\n".join(transcript), encoding="utf-8")
        if result.returncode:
            return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
