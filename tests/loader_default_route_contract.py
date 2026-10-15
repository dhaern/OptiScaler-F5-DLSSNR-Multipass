"""Actual Windows Win32 -> native routing, production selection and real Detours.

Only unrelated query/unload hooks are omitted. The production load wrappers,
HookBase body, native load body and native selection are source-backed. The
speculative provider load is deliberately failed; the OS original loads a real
SDK-shaped isolated DLL. Does not load a game/provider or falsify loader/TLS state.
"""
from pathlib import Path
import subprocess
import sys
from loader_wrapper_contract import ROOT, function_body
from loader_runtime_contract import PRE, FIXTURE, REAL_MAIN


def main():
    out = Path(sys.argv[1]).resolve(); out.mkdir(parents=True, exist_ok=True)
    revision = sys.argv[2] if len(sys.argv) > 2 else None
    def source(path):
        if revision:
            return subprocess.check_output(['git','-C',str(ROOT),'show',f'{revision}:{path}'],text=True)
        return (ROOT/path).read_text(encoding='utf-8-sig')
    pre = PRE[:PRE.index('// Deterministic transaction stand-ins:')]
    pre = pre.replace('#include "LoaderBoundary.h"',source('OptiScaler/misc/LoaderBoundary.h').replace('#pragma once',''))
    pre = pre.replace('#include "D3d12Binding.h"',source('OptiScaler/misc/D3d12Binding.h').replace('#pragma once',''))
    # Replace only the fixture's platform declarations, not production bodies.
    pre = pre[:pre.index('struct KernelBaseProxy')] + pre[pre.index('static bool veto'):]
    pre = pre.replace('struct KernelHooks { inline static auto o_K32_LoadLibraryW = &LoadLibraryW; static HMODULE hk_K32_LoadLibraryW(LPCWSTR); };','')
    pre = pre.replace('static RecoveryResult HandleCallerModule(', 'static void RecordNonExecutable(HMODULE m, HMODULE previous) { if (m && m!=previous && !(reinterpret_cast<ULONG_PTR>(m)&3)) veto=true; }\n    static bool IsApiSetName(std::wstring_view n) { return n.starts_with(L"api-ms-"); }\n    static HMODULE LoadLibraryCheckW(std::wstring, LPCWSTR, bool* failed) { *failed=true; ++nativeChecks; return nullptr; }\n    static RecoveryResult HandleCallerModule(')
    pre = pre.replace('static bool veto = false;', 'static unsigned nativeChecks=0, nativeEntries=0;\nstatic bool veto = false;')
    pre = pre.replace('static bool SkipDllChecks() { return false; }', 'static bool SkipDllChecks() { return false; } static std::string SkipDllName() { return {}; }')
    pre = pre.replace('struct Config { struct Option { bool value_or_default() const { return false; } } UseNtdllHooks; static Config* Instance() { static Config c; return &c; } };', 'struct Config { struct Option { bool value=true; bool value_or_default() const { return value; } } UseNtdllHooks; static Config* Instance() { static Config c; return &c; } };')
    pre += r'''
#include <winternl.h>
#include <filesystem>
#include <detours/detours.h>
#define LOG_FUNC(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
#ifndef STATUS_DLL_NOT_FOUND
#define STATUS_DLL_NOT_FOUND ((NTSTATUS)0xc0000135L)
#endif
static std::wstring string_to_wstring(const std::string& s) { return {s.begin(),s.end()}; }
struct KernelBaseProxy {
    using PFN_LoadLibraryExW=decltype(&LoadLibraryExW);
    using PFN_GetProcAddress=decltype(&GetProcAddress);
    inline static PFN_LoadLibraryExW original=&LoadLibraryExW;
    static auto GetModuleHandleW_() { return &GetModuleHandleW; }
    static auto GetModuleHandleExW_() { return &GetModuleHandleExW; }
    static auto GetProcAddress_() { return &GetProcAddress; }
    static PFN_LoadLibraryExW LoadLibraryExW_Hooked() {
        return reinterpret_cast<PFN_LoadLibraryExW>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),"LoadLibraryExW"));
    }
    static PFN_GetProcAddress Hook_GetProcAddress(PVOID) { return &GetProcAddress; }
    static PFN_LoadLibraryExW Hook_LoadLibraryExW(PVOID hook) {
        original=LoadLibraryExW_Hooked();
        CHECK(DetourTransactionBegin()==0); CHECK(DetourUpdateThread(GetCurrentThread())==0);
        CHECK(DetourAttach(reinterpret_cast<PVOID*>(&original),hook)==0);
        CHECK(DetourTransactionCommit()==0); return original;
    }
};
'''
    nproxy = source('OptiScaler/proxies/Ntdll_Proxy.h')
    # Omit the unused hFile parameter name (ABI/body unchanged) in the inherited partial proxy.
    pre += nproxy[nproxy.index('class NtdllProxy'):].replace('HANDLE hFile, DWORD dwFlags', 'HANDLE, DWORD dwFlags')
    kproxy = source('OptiScaler/proxies/Kernel32_Proxy.h')
    pre += kproxy[kproxy.index('#define PROXY_FUNCTIONS'):]
    proxy = source('OptiScaler/proxies/D3D12_Proxy.h')
    pre += '\n' + proxy[proxy.index('class D3d12Proxy'):]
    decl = r'''
static D3d12Proxy::PFN_D3D12CreateDevice o_D3D12CreateDevice=nullptr;
static D3d12Proxy::PFN_D3D12SerializeRootSignature o_D3D12SerializeRootSignature=nullptr;
static D3d12Proxy::PFN_D3D12SerializeVersionedRootSignature o_D3D12SerializeVersionedRootSignature=nullptr;
static PVOID hkD3D12CreateDevice=nullptr, hkD3D12SerializeRootSignature=nullptr, hkD3D12SerializeVersionedRootSignature=nullptr;
struct D3D12Hooks { static bool Hook(); };
struct KernelHooks {
    inline static std::mutex hookMutexBase;
    inline static KernelBaseProxy::PFN_GetProcAddress o_KB_GetProcAddress=nullptr;
    inline static KernelBaseProxy::PFN_LoadLibraryExW o_KB_LoadLibraryExW=nullptr;
    static FARPROC WINAPI hk_KB_GetProcAddress(HMODULE m,LPCSTR n) { return GetProcAddress(m,n); }
    static HMODULE hk_KB_LoadLibraryExW(LPCWSTR,HANDLE,DWORD);
    static void HookBase();
    inline static Kernel32Proxy::PFN_LoadLibraryW o_K32_LoadLibraryW=nullptr;
    inline static Kernel32Proxy::PFN_LoadLibraryA o_K32_LoadLibraryA=nullptr;
    inline static Kernel32Proxy::PFN_LoadLibraryExW o_K32_LoadLibraryExW=nullptr;
    inline static Kernel32Proxy::PFN_LoadLibraryExA o_K32_LoadLibraryExA=nullptr;
    static void Hook();
    static HMODULE hk_K32_LoadLibraryW(LPCWSTR);
    static HMODULE hk_K32_LoadLibraryA(LPCSTR);
    static HMODULE hk_K32_LoadLibraryExW(LPCWSTR,HANDLE,DWORD);
    static HMODULE hk_K32_LoadLibraryExA(LPCSTR,HANDLE,DWORD);
};
struct NtdllHooks {
    inline static NtdllProxy::PFN_LdrLoadDll o_LdrLoadDll=nullptr;
    static NTSTATUS NTAPI hkLdrLoadDll(PWSTR,PULONG,PUNICODE_STRING,PHANDLE);
    static void Hook();
};
'''
    kh = source('OptiScaler/hooks/Kernel_Hooks.h')
    kb = function_body(kh,'static void HookBase()').replace('static void HookBase()', 'void KernelHooks::HookBase()')
    k32 = function_body(kh,'static void Hook()')
    # Exact production load-selection statements; unrelated query/free hooks omitted.
    if 'Forwarded exports share' in k32:
        k32 = 'void KernelHooks::Hook() {\n' + k32[k32.index('        if (o_K32_LoadLibraryA'):]
    else:
        k32 = 'void KernelHooks::Hook() {\nif (!Config::Instance()->UseNtdllHooks.value_or_default()) {\n' + k32[k32.index('            if (o_K32_LoadLibraryA'):]
    native_header = source('OptiScaler/hooks/Ntdll_Hooks.h')
    native = function_body(native_header,'static NTSTATUS NTAPI hkLdrLoadDll(').replace('static NTSTATUS NTAPI hkLdrLoadDll(', 'NTSTATUS NTAPI NtdllHooks::hkLdrLoadDll(')
    # Count entries in the source-backed native body to establish real OS nesting.
    native = native.replace('    {','    {\n        ++nativeEntries;',1)
    nhook = function_body(native_header,'static void Hook()')
    select = nhook[nhook.index('        if (!Config'):nhook.index('        if (o_RtlGetVersion')]
    install = nhook[nhook.index('        if (o_LdrLoadDll'):nhook.index('        if (o_LdrUnloadDll')]
    nhook = 'void NtdllHooks::Hook() {\n' + select + install + '}\n'
    library = source('OptiScaler/hooks/LibraryLoad_Hooks.cpp')
    bodies=[function_body(source('OptiScaler/hooks/D3D12_Hooks.cpp'),'bool D3D12Hooks::Hook()'),kb,k32,native,nhook]
    for sig in ['HMODULE RetainImage(', 'void ReleaseImage(', 'bool LibraryLoadHooks::EndsWithInsensitive(std::wstring_view', 'LibraryLoadHooks::RecoveryResult LibraryLoadHooks::ObserveOrdinaryModule(', 'LibraryLoadHooks::RecoveryResult LibraryLoadHooks::HandleCallerModule(']:
        bodies.append(function_body(library,sig))
    kernel = source('OptiScaler/hooks/Kernel_Hooks.cpp')
    if 'static bool NativeLoadOnly(' in kernel:
        bodies += [function_body(kernel,'template <class C> static bool NativeLoadOnly(')]
    bodies += [function_body(kernel,f'HMODULE KernelHooks::{name}(') for name in ('hk_KB_LoadLibraryExW','hk_K32_LoadLibraryW','hk_K32_LoadLibraryA','hk_K32_LoadLibraryExW','hk_K32_LoadLibraryExA')]
    # Reuse the real export/trampoline/reference assertions, but enter via the
    # OS API with the actual configured detours, never by calling its wrapper.
    caller = r'''
static unsigned api = 0;
static HMODULE CallerLoad(LPCWSTR path) {
    char narrow[32768]; CHECK(WideCharToMultiByte(CP_ACP,0,path,-1,narrow,sizeof(narrow),nullptr,nullptr)>0);
    switch (api) {
    case 0: return LoadLibraryW(path);
    case 1: return LoadLibraryA(narrow);
    case 2: return LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    case 3: return LoadLibraryExA(narrow,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    default: return reinterpret_cast<decltype(&LoadLibraryExW)>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),"LoadLibraryExW"))(path,nullptr,0);
    }
}
'''
    main_cpp=caller+REAL_MAIN.replace('if (argc != 2)', 'if (argc != 4)')
    setup=r'''
    Config::Instance()->UseNtdllHooks.value = wcscmp(argv[2],L"false")!=0;
    api = static_cast<unsigned>(_wtoi(argv[3]));
    NtdllProxy::Init(); Kernel32Proxy::Init(); NtdllHooks::Hook();
    KernelHooks::HookBase(); KernelHooks::Hook(); KernelHooks::HookBase(); KernelHooks::Hook();
    CHECK(KernelHooks::o_KB_LoadLibraryExW != nullptr || !Config::Instance()->UseNtdllHooks.value);
    if (Config::Instance()->UseNtdllHooks.value && !KernelHooks::o_KB_LoadLibraryExW) return 1;

    const unsigned beforeNative=nativeEntries;
    auto unrelated=LoadLibraryW(L"version.dll"); CHECK(unrelated); if (unrelated) FreeLibrary(unrelated);
    if (Config::Instance()->UseNtdllHooks.value) CHECK(nativeEntries>beforeNative && nativeChecks>0);
    // Classify the real Win32 no-execution modes before any native/speculative work.
    const std::wstring noinit=std::wstring(argv[1])+L".noinit.dll";
    CHECK(CopyFileW(argv[1],noinit.c_str(),FALSE));
    const auto beforeChecks=nativeChecks;
    for (DWORD flags : {DWORD(DONT_RESOLVE_DLL_REFERENCES),DWORD(LOAD_LIBRARY_AS_DATAFILE),
                        DWORD(LOAD_LIBRARY_AS_IMAGE_RESOURCE),DWORD(LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE)}) {
        const auto image=LoadLibraryExW(noinit.c_str(),nullptr,flags); CHECK(image);
        CHECK(nativeChecks==beforeChecks && retryMarks==0 && !D3d12Proxy::Module());
        if (image) CHECK(FreeLibrary(image));
    }
    // Fixture veto stand-in is scoped to those images; the committed boundary test
    // separately exercises the real retained negative-identity table.
    veto=false;
'''
    main_cpp=main_cpp.replace('    hkD3D12CreateDevice =',setup+'\n    hkD3D12CreateDevice =')
    main_cpp=main_cpp.replace('KernelHooks::hk_K32_LoadLibraryW(argv[1])','CallerLoad(argv[1])')
    main_cpp=main_cpp.replace('    CHECK(first &&', '    printf("route_first: module=%p bound=%p native_entries=%u native_checks=%u retries=%d\\n", first,D3d12Proxy::Module(),nativeEntries,nativeChecks,retryMarks);\n    CHECK(first &&')
    # Single native entry or nested Win32 entry must not duplicate the observer.
    main_cpp=main_cpp.replace('    if (failures) return 1;\n    const auto create', '    CHECK(retryMarks==1);\n    if (Config::Instance()->UseNtdllHooks.value) CHECK(nativeEntries>=3);\n    if (failures) return 1;\n    const auto create',1)
    main_cpp=main_cpp.replace('    return failures ? 1 : 0;','    printf("default_route: mode=%ls native_entries=%u native_checks=%u retry_marks=%d failures=%d\\n",argv[2],nativeEntries,nativeChecks,retryMarks,failures);\n    return failures ? 1 : 0;')
    cpp=out/'route.cpp'; cpp.write_text(pre+decl+'\n'.join(bodies)+main_cpp)
    fixture=out/'fixture.cpp'; fixture.write_text(FIXTURE)
    dll=out/'d3d12.dll'; exe=out/'route.exe'
    commands=[['cl','/nologo','/std:c++20','/EHsc','/W4','/WX','/O2','/LD',str(fixture),f'/Fo{out}/fixture.obj',f'/Fe{dll}','/link','/OPT:NOICF',f'/IMPLIB:{out}/fixture.lib'],['cl','/nologo','/std:c++20','/EHsc','/W4','/WX','/O2',f'/I{ROOT}/OptiScaler/include',str(cpp),f'/Fo{out}/route.obj',f'/Fe{exe}',str(ROOT/'OptiScaler/library/detours/detours.lib')]]
    commands += [[str(exe),str(dll),mode,str(api)] for mode in ('auto','true','false') for api in range(5)]
    for command in commands:
        p=subprocess.run(command,capture_output=True,text=True,timeout=90)
        text=subprocess.list2cmdline(command)+'\n'+p.stdout+p.stderr+f'\nprocess_exit={p.returncode}\n'
        with (out/'route.log').open('a') as log: log.write(text)
        print(text,flush=True)
        if p.returncode: return p.returncode
    return 0

if __name__=='__main__': raise SystemExit(main())
