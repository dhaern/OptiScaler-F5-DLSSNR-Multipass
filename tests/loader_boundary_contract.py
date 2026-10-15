"""Actual startup discovery, KernelBase query and worker bodies with a real DllMain fixture.

Provider hooks and worker per-image processing are counting boundaries, not real
provider initialization. Tests distinguish legacy unknown-preload behavior from
known-no-init vetoes and never claim a counter proves an actual deadlock.
Usage after vcvars64: python tests/loader_boundary_contract.py <scratch> [ref]
"""
from pathlib import Path
import subprocess, sys
from loader_wrapper_contract import ROOT, function_body
from loader_noinit_discovery_probe import FIXTURE, PREAMBLE

EXTRA = r'''
#include <atomic>
#include <intrin.h>
#include "LoaderBoundary.h"
#define LOG_TRACE(...) ((void)0)
static HMODULE dllModule = nullptr, amdxc64Mark = reinterpret_cast<HMODULE>(0x1337);
struct Amdxc64Hooks { static void hkAmdExtD3DCreateInterface() {} };
struct Kernel32Proxy { using PFN_GetProcAddress = decltype(&GetProcAddress); };
struct NtdllProxy { static void FreeLibrary_Ldr(HMODULE m) { FreeLibrary(m); } };
static int workerImages = 0;
struct KernelHooks {
    inline static auto o_KB_GetProcAddress = &GetProcAddress;
    static FARPROC WINAPI hk_KB_GetProcAddress(HMODULE, LPCSTR);
};
'''
CALL = r'''
extern "C" __declspec(dllexport) void WINAPI Plan2LoaderCallout() {
    printf("real_dllmain_callback_has_tls=%d\n", !ThreadHasNoTls());
    KernelHooks::hk_KB_GetProcAddress(GetModuleHandleW(nullptr), "Plan2LoaderCallout");
}
'''
MAIN = r'''
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const bool initializedMode = wcscmp(argv[2], L"initialized") == 0;
    LibraryLoadHooks::_deferredScan.Mark();
    auto module = LoadLibraryExW(argv[1], nullptr, initializedMode ? 0 : DONT_RESOLVE_DLL_REFERENCES);
    CHECK(module != nullptr); if (!module) return 2;
    auto count = reinterpret_cast<volatile LONG*>(GetProcAddress(module,"ProbeAttachCount"));
    CHECK(count && *count == (initializedMode ? 1 : 0));
    LibraryLoadHooks::CheckModulesInMemory();
    CHECK(interposerHookEntries == 1 && observed == module);
    if (!initializedMode) {
        HMODULE retained=nullptr;
        CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(module),&retained));
        CHECK(retained==module); CHECK(FreeLibrary(retained));
        auto ordinary=LoadLibraryW(argv[1]); CHECK(ordinary==module); CHECK(count && *count==0);
        CHECK(FreeLibrary(ordinary));
#ifdef CURRENT_BOUNDARY
        LibraryLoadHooks::RecordNonExecutable(module,nullptr);
        CHECK(LibraryLoadHooks::KnownNonExecutable(module));
        LibraryLoadHooks::CheckModulesInMemory(); CHECK(interposerHookEntries==1);
        // The quarantine reference deliberately remains process-lifetime to prohibit address reuse.
        CHECK(FreeLibrary(module));
#else
        CHECK(FreeLibrary(module));
#endif
    } else {
#ifdef CURRENT_BOUNDARY
        CHECK(scansInsideDllMain==0);
        // A data/no-init request for an already present ordinary image must not poison legacy startup.
        LibraryLoadHooks::RecordNonExecutable(module,module);
        CHECK(!LibraryLoadHooks::KnownNonExecutable(module));
        LibraryLoadHooks::RecordDeferredLoad(module);
        LibraryLoadHooks::CatchUpDeferredLoads();
        CHECK(workerImages==1); CHECK(scansInsideDllMain==0);
        LibraryLoadHooks::_deferredScan.Mark();
        LibraryLoadHooks::CatchUpDeferredLoads(); CHECK(workerImages==1);
        int pending=0; LibraryLoadHooks::_deferredScan.Run([&] { ++pending; }); CHECK(pending==1);
#else
        CHECK(scansInsideDllMain==0); // Fails on the frozen unsafe real-query-wrapper boundary.
#endif
        CHECK(FreeLibrary(module));
    }
    printf("initialized=%d startup_entries=%d dllmain_scan_entries=%d worker_images=%d\n",
           initializedMode, interposerHookEntries, scansInsideDllMain, workerImages);
    return failures ? 1 : 0;
}
'''

def main():
    out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
    revision=sys.argv[2] if len(sys.argv)>2 else None
    def source(path):
        return subprocess.check_output(['git','-C',str(ROOT),'show',f'{revision}:{path}'],text=True) if revision else (ROOT/path).read_text(encoding='utf-8-sig')
    library=source('OptiScaler/hooks/LibraryLoad_Hooks.cpp');names=source('OptiScaler/DllNames.h');kernel=source('OptiScaler/hooks/Kernel_Hooks.cpp')
    current='ObserveOrdinaryModule' in library
    # Keep the standalone fixture's State/Config/provider stand-ins explicit.
    pre=PREAMBLE.replace('struct LibraryLoadHooks {','struct LibraryLoadHooks {\n    static bool KnownNonExecutable(HMODULE);')
    pre=pre.replace('static auto GetModuleHandleW_() { return &GetModuleHandleW; }','static auto GetModuleHandleW_() { return &GetModuleHandleW; }\n    static auto GetModuleHandleExW_() { return &GetModuleHandleExW; }')
    if current:
        pre=pre.replace('struct LibraryLoadHooks {', '''struct LibraryLoadHooks {
    inline static constinit loader_boundary::KnownNoInitImages _noInit;
    inline static constinit std::atomic<HMODULE> _deferredImages[32] {};
    inline static constinit std::atomic_bool _workerClosed { false };
    static void RecordNonExecutable(HMODULE,HMODULE);
    static void RecordDeferredLoad(HMODULE,bool ordinaryWin32=true);
    static void ObserveOrdinaryModule(HMODULE) { ++workerImages; _deferredScan.Mark(); }
''')
        pre=pre.replace('static void ScanLoadedModules() { ++scansInsideDllMain; }','static void ScanLoadedModules();')
    else:
        pre=pre.replace('static bool KnownNonExecutable(HMODULE);','static bool KnownNonExecutable(HMODULE) { return false; }')
    # EXTRA must precede the LibraryLoadHooks class, but follow basic fixture types/includes.
    split=pre.index('struct LibraryLoadHooks {');pre=pre[:split]+EXTRA+pre[split:]
    parts=[pre]
    if current:
        parts.append('#define CURRENT_BOUNDARY 1')
        for sig in ['HMODULE RetainImage(', 'void ReleaseImage(', 'bool LibraryLoadHooks::KnownNonExecutable(',
                    'void LibraryLoadHooks::RecordNonExecutable(', 'void LibraryLoadHooks::RecordDeferredLoad(',
                    'void LibraryLoadHooks::ScanLoadedModules(']: parts.append(function_body(library,sig))
    if 'static FARPROC ResolveAmdMarker(' in kernel: parts.append(function_body(kernel,'static FARPROC ResolveAmdMarker('))
    parts += [function_body(names,'inline static HMODULE GetDllNameWModule('),
              function_body(library,'void LibraryLoadHooks::CheckModulesInMemory()'),
              function_body(library,'void LibraryLoadHooks::CatchUpDeferredLoads()'),
              function_body(kernel,'FARPROC WINAPI KernelHooks::hk_KB_GetProcAddress('),CALL,MAIN]
    for header in ['ThreadTls.h','DeferredModuleScan.h']:(out/header).write_text(source('OptiScaler/misc/'+header),encoding='utf-8')
    cpp=out/'boundary.generated.cpp';cpp.write_text('\n'.join(parts),encoding='utf-8')
    fixture=out/'fixture.cpp';fixture.write_text(FIXTURE,encoding='utf-8');dll=out/'sl.interposer.dll';exe=out/'boundary.exe'
    commands=[['cl','/nologo','/std:c++20','/EHsc','/W4','/WX','/O2','/LD',str(fixture),f'/Fe:{dll}',f'/Fo:{out / "fixture.obj"}','/link',f'/IMPLIB:{out / "fixture.lib"}'],
              ['cl','/nologo','/std:c++20','/EHsc','/W4','/WX','/O2',f'/I{out}',f'/I{ROOT / "OptiScaler/misc"}',str(cpp),f'/Fe:{exe}',f'/Fo:{out / "boundary.obj"}'],
              [str(exe),str(dll),'unknown'],[str(exe),str(dll),'initialized']]
    logs=[];failed=0
    for command in commands:
        p=subprocess.run(command,capture_output=True,text=True,timeout=90);text=subprocess.list2cmdline(command)+'\n'+p.stdout+p.stderr+f'\nprocess_exit={p.returncode}\n';print(text,flush=True);logs.append(text);(out/'boundary.log').write_text('\n'.join(logs),encoding='utf-8')
        if p.returncode:
            failed=p.returncode
            if command[0]=='cl':return failed
    return failed
if __name__=='__main__':raise SystemExit(main())
