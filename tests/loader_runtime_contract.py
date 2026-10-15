"""Real DLL lifetime + actual D3D12 proxy/installer bodies, strict MSVC host contract.

The fixture exports SDK-shaped functions, not a graphics provider. Real Windows
reference accounting is exercised in both executable phases. Deterministic
transaction stand-ins test stage failures; a separate phase links real Detours,
patches fixture exports and calls their actual trampolines. No game is touched.
"""
from pathlib import Path
import subprocess
import sys
from loader_wrapper_contract import ROOT, function_body

FIXTURE = r'''
#include <windows.h>
#include <d3d12.h>
#pragma comment(linker, "/export:D3D12CreateDevice=FixtureD3D12CreateDevice")
extern "C" __declspec(dllexport) HRESULT WINAPI FixtureD3D12CreateDevice(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**) { return E_NOTIMPL; }
#pragma comment(linker, "/export:D3D12SerializeRootSignature=FixtureD3D12SerializeRootSignature")
extern "C" __declspec(dllexport) HRESULT WINAPI FixtureD3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**) { return E_NOTIMPL; }
#pragma comment(linker, "/export:D3D12SerializeVersionedRootSignature=FixtureD3D12SerializeVersionedRootSignature")
extern "C" __declspec(dllexport) HRESULT WINAPI FixtureD3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**) { return E_NOTIMPL; }
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
'''
PRE = r'''
#include <windows.h>
#include <d3d12.h>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <string_view>
#include <mutex>
#include "LoaderBoundary.h"
#include "D3d12Binding.h"
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
static int failures = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)
struct KernelBaseProxy {
    static auto GetModuleHandleW_() { return &GetModuleHandleW; }
    static auto GetModuleHandleExW_() { return &GetModuleHandleExW; }
    static auto GetProcAddress_() { return &GetProcAddress; }
};
struct NtdllProxy {
    static HMODULE LoadLibraryExW_Ldr(LPCWSTR p, HANDLE h, DWORD f) { return LoadLibraryExW(p,h,f); }
    static bool FreeLibrary_Ldr(HMODULE m) { return FreeLibrary(m) != 0; }
};
static bool veto = false;
enum class WorkingMode { Other, D3d12 };
struct State { WorkingMode workingMode = WorkingMode::Other; bool isShuttingDown=false; static bool SkipDllChecks() { return false; } static State& Instance() { static State s; return s; } };
struct DxgiProxy { static bool InsideFactoryCreation() { return false; } };
static int retryMarks=0;
struct Config { struct Option { bool value_or_default() const { return false; } } UseNtdllHooks; static Config* Instance() { static Config c; return &c; } };
struct KernelHooks { inline static auto o_K32_LoadLibraryW = &LoadLibraryW; static HMODULE hk_K32_LoadLibraryW(LPCWSTR); };
inline bool ThreadHasNoTls() { return false; }
inline HMODULE CheckLoad(const std::wstring&, bool* failed) { *failed=true; return nullptr; }
static HMODULE dllModule = nullptr;
struct LibraryLoadHooks {
    enum class RecoveryResult { Ready, Retry, Unsupported, NotApplicable };
    static bool KnownNonExecutable(HMODULE) { return veto; }
    static void RecordDeferredLoad(HMODULE,bool=true) { ++retryMarks; }
    static bool EndsWithInsensitive(std::wstring_view,std::wstring_view);
    static RecoveryResult ObserveOrdinaryModule(HMODULE);
    static RecoveryResult HandleCallerModule(const std::wstring&,LPCWSTR,HMODULE);
};
// Deterministic transaction stand-ins: actual proxy/installer logic is compiled;
// no fixture bytecode is patched. Inject a failed commit, then succeed.
static int commits = 0, failCommit = 0;
inline LONG DetourTransactionBegin() { return 0; }
inline LONG DetourTransactionAbort() { return 0; }
inline LONG DetourUpdateThread(HANDLE) { return 0; }
inline void (*slotProbe)(PVOID*) = nullptr;
inline LONG DetourAttach(PVOID* slot, PVOID) { if (slotProbe) slotProbe(slot); return 0; }
inline LONG DetourDetach(PVOID*, PVOID) { return 0; }
inline LONG DetourTransactionCommit() { ++commits; if (failCommit) { --failCommit; return ERROR_INVALID_OPERATION; } return 0; }
'''
MAIN = r'''
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    auto caller = LoadLibraryW(argv[1]); CHECK(caller != nullptr);
    veto = true; D3d12Proxy::Init(caller); CHECK(D3d12Proxy::Module() == nullptr);
    veto = false;
    std::vector<std::thread> threads;
    for (int i=0; i!=16; ++i) threads.emplace_back([&] { D3d12Proxy::Init(caller); });
    for (auto& t : threads) t.join();
    CHECK(D3d12Proxy::Module() == caller);
    CHECK(D3d12Proxy::D3D12CreateDevice_() != nullptr);
    CHECK(D3d12Proxy::D3D12SerializeRootSignature_() != nullptr);
    for (int i=0; i!=100; ++i) D3d12Proxy::Init(caller);
#ifdef CURRENT_OBSERVATION
    slotProbe = [](PVOID* slot) {
        CHECK(slot == reinterpret_cast<PVOID*>(&o_D3D12CreateDevice) ||
              slot == reinterpret_cast<PVOID*>(&o_D3D12SerializeRootSignature) ||
              slot == reinterpret_cast<PVOID*>(&o_D3D12SerializeVersionedRootSignature));
    };
    failCommit = 1;
    CHECK(LibraryLoadHooks::ObserveOrdinaryModule(caller)==LibraryLoadHooks::RecoveryResult::Retry);
    CHECK(retryMarks==1);
    const int partialCommits = commits;
    auto ordinary=KernelHooks::hk_K32_LoadLibraryW(argv[1]);
    CHECK(ordinary==caller); CHECK(o_D3D12CreateDevice!=nullptr);
    CHECK(commits == partialCommits + 1); // Before returning the real original load to its executable caller.
    CHECK(LibraryLoadHooks::HandleCallerModule(L"wrong-request.dll",L"C:/wrong/path",caller)==LibraryLoadHooks::RecoveryResult::Ready);
    CHECK(FreeLibrary(ordinary));
    CHECK(LibraryLoadHooks::ObserveOrdinaryModule(GetModuleHandleW(L"kernel32.dll"))==LibraryLoadHooks::RecoveryResult::Unsupported);
    CHECK(LibraryLoadHooks::ObserveOrdinaryModule(reinterpret_cast<HMODULE>(0x1337))==LibraryLoadHooks::RecoveryResult::NotApplicable);
    const int readyCommits = commits;
    CHECK(D3D12Hooks::Hook()); CHECK(commits == readyCommits);
    {
        d3d12_binding::Claim owner;
        CHECK(static_cast<bool>(owner));
        std::thread busy([&] { CHECK(!D3D12Hooks::Hook()); CHECK(!D3d12Proxy::Init(caller)); });
        busy.join();
    }
#endif
    CHECK(FreeLibrary(caller));
    HMODULE found = nullptr;
    CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(caller), &found));
    CHECK(found == caller);
    if (!found) return 1; // Do not use an unloaded fixture's pointers on the negative control.
    // Release precisely the proxy's one reference; duplicate observations must not add references.
    CHECK(FreeLibrary(found));
    found = nullptr;
    CHECK(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCWSTR>(caller), &found));
    if (!failures) printf("runtime_contract: known-no-init veto, concurrent complete binding, one retained reference, duplicate accounting passed\n");
    return failures ? 1 : 0;
}
'''

REAL_MAIN = r'''
static unsigned realCreate = 0, realSerialize = 0, realVersioned = 0;
static HRESULT WINAPI RealCreate(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** out)
{
    ++realCreate;
    return o_D3D12CreateDevice(adapter, level, iid, out);
}
static HRESULT WINAPI RealSerialize(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION version,
                                   ID3DBlob** blob, ID3DBlob** error)
{
    ++realSerialize;
    return o_D3D12SerializeRootSignature(desc, version, blob, error);
}
static HRESULT WINAPI RealVersioned(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                   ID3DBlob** blob, ID3DBlob** error)
{
    ++realVersioned;
    return o_D3D12SerializeVersionedRootSignature(desc, blob, error);
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    hkD3D12CreateDevice = reinterpret_cast<PVOID>(RealCreate);
    hkD3D12SerializeRootSignature = reinterpret_cast<PVOID>(RealSerialize);
    hkD3D12SerializeVersionedRootSignature = reinterpret_cast<PVOID>(RealVersioned);
    // An already-open real transaction makes the installer's Begin fail without
    // replacing any Detours API. Original caller ownership and retry must survive.
    CHECK(DetourTransactionBegin() == NO_ERROR);
    const auto first = KernelHooks::hk_K32_LoadLibraryW(argv[1]);
    CHECK(first && D3d12Proxy::Module() == first);
    CHECK(o_D3D12CreateDevice == nullptr && o_D3D12SerializeRootSignature == nullptr &&
          o_D3D12SerializeVersionedRootSignature == nullptr && retryMarks == 1);
    CHECK(DetourTransactionAbort() == NO_ERROR);
    const auto second = KernelHooks::hk_K32_LoadLibraryW(argv[1]);
    CHECK(second == first && o_D3D12CreateDevice && o_D3D12SerializeRootSignature &&
          o_D3D12SerializeVersionedRootSignature);
    if (failures) return 1;
    const auto create = reinterpret_cast<D3d12Proxy::PFN_D3D12CreateDevice>(GetProcAddress(first, "D3D12CreateDevice"));
    const auto serialize = reinterpret_cast<D3d12Proxy::PFN_D3D12SerializeRootSignature>(GetProcAddress(first, "D3D12SerializeRootSignature"));
    const auto versioned = reinterpret_cast<D3d12Proxy::PFN_D3D12SerializeVersionedRootSignature>(GetProcAddress(first, "D3D12SerializeVersionedRootSignature"));
    CHECK(create && serialize && versioned);
    if (failures) return 1;
    // Call immediately after the ordinary loader wrapper returns: all three real
    // export detours and their original trampoline slots must already work.
    CHECK(create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(IUnknown), nullptr) == E_NOTIMPL);
    CHECK(serialize(nullptr, D3D_ROOT_SIGNATURE_VERSION_1_0, nullptr, nullptr) == E_NOTIMPL);
    CHECK(versioned(nullptr, nullptr, nullptr) == E_NOTIMPL);
    CHECK(realCreate == 1 && realSerialize == 1 && realVersioned == 1);
    for (unsigned i = 0; i != 100; ++i)
        CHECK(LibraryLoadHooks::ObserveOrdinaryModule(first) == LibraryLoadHooks::RecoveryResult::Ready);
    CHECK(FreeLibrary(first) && FreeLibrary(second));
    HMODULE retained = nullptr;
    CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(first), &retained) && retained == first);
    CHECK(create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(IUnknown), nullptr) == E_NOTIMPL && realCreate == 2);
    // Fixture teardown only: remove every real patch before releasing precisely
    // the proxy's one hidden reference. No proxy call is made after this release.
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourUpdateThread(GetCurrentThread()) == NO_ERROR);
    CHECK(DetourDetach(reinterpret_cast<PVOID*>(&o_D3D12CreateDevice), hkD3D12CreateDevice) == NO_ERROR);
    CHECK(DetourDetach(reinterpret_cast<PVOID*>(&o_D3D12SerializeRootSignature), hkD3D12SerializeRootSignature) == NO_ERROR);
    CHECK(DetourDetach(reinterpret_cast<PVOID*>(&o_D3D12SerializeVersionedRootSignature), hkD3D12SerializeVersionedRootSignature) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    if (failures) return 1;
    CHECK(FreeLibrary(retained));
    retained = nullptr;
    CHECK(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCWSTR>(first), &retained));
    if (!failures) printf("runtime_real_detours: begin failure/retry, before-return exports, real trampolines, caller release, one retained reference and detach/unload passed\n");
    return failures ? 1 : 0;
}
'''

def main():
    out = Path(sys.argv[1]).resolve(); out.mkdir(parents=True, exist_ok=True)
    revision = sys.argv[2] if len(sys.argv) > 2 else None
    def source(path):
        if revision:
            return subprocess.check_output(['git','-C',str(ROOT),'show',f'{revision}:{path}'], text=True)
        return (ROOT / path).read_text(encoding='utf-8-sig')
    proxy = source('OptiScaler/proxies/D3D12_Proxy.h')
    # Includes are replaced by explicit API stand-ins above; the complete class body is unchanged.
    proxy = proxy[proxy.index('class D3d12Proxy'):]
    hooks = source('OptiScaler/hooks/D3D12_Hooks.cpp')
    hook = function_body(hooks, 'bool D3D12Hooks::Hook()') if 'bool D3D12Hooks::Hook()' in hooks else function_body(hooks, 'void D3D12Hooks::Hook()')
    declarations = r'''
static D3d12Proxy::PFN_D3D12CreateDevice o_D3D12CreateDevice = nullptr;
static D3d12Proxy::PFN_D3D12SerializeRootSignature o_D3D12SerializeRootSignature = nullptr;
static D3d12Proxy::PFN_D3D12SerializeVersionedRootSignature o_D3D12SerializeVersionedRootSignature = nullptr;
static PVOID hkD3D12CreateDevice = nullptr, hkD3D12SerializeRootSignature = nullptr, hkD3D12SerializeVersionedRootSignature = nullptr;
struct D3D12Hooks { inline static std::mutex hookMutex; static bool Hook(); };
'''
    if 'void D3D12Hooks::Hook()' in hook:
        # The frozen void installer has no completion contract: adapter reports false after the unchanged body.
        hook = hook.replace('void D3D12Hooks::Hook()', 'bool D3D12Hooks::Hook()')
        hook = hook[:-1] + 'return false; }'
    library=source('OptiScaler/hooks/LibraryLoad_Hooks.cpp')
    observation=[]
    if 'ObserveOrdinaryModule' in library:
        observation.append('#define CURRENT_OBSERVATION 1')
        for sig in ['HMODULE RetainImage(', 'void ReleaseImage(', 'bool LibraryLoadHooks::EndsWithInsensitive(std::wstring_view',
                    'LibraryLoadHooks::RecoveryResult LibraryLoadHooks::ObserveOrdinaryModule(',
                    'LibraryLoadHooks::RecoveryResult LibraryLoadHooks::HandleCallerModule(']:
            observation.append(function_body(library,sig))
        kernel=source('OptiScaler/hooks/Kernel_Hooks.cpp')
        if 'static bool NativeLoadOnly(' in kernel:
            observation.append(function_body(kernel,'template <class C> static bool NativeLoadOnly('))
        observation.append(function_body(kernel,'HMODULE KernelHooks::hk_K32_LoadLibraryW('))
    cpp = out/'runtime.generated.cpp'; cpp.write_text(PRE+'\n'+proxy+'\n'+declarations+'\n'+hook+'\n'+'\n'.join(observation)+'\n'+MAIN, encoding='utf-8')
    fixture=out/'runtime_fixture.cpp'; fixture.write_text(FIXTURE,encoding='utf-8')
    dll=out/'d3d12.dll'; exe=out/'runtime.exe'
    real_cpp = out/'runtime.real.generated.cpp'; real_exe = out/'runtime-real.exe'
    real_pre = PRE[:PRE.index('// Deterministic transaction stand-ins:')] + '#include <detours/detours.h>\n'
    real_cpp.write_text(real_pre+'\n'+proxy+'\n'+declarations+'\n'+hook+'\n'+'\n'.join(observation)+'\n'+REAL_MAIN, encoding='utf-8')
    commands=[['cl','/nologo','/std:c++20','/EHsc','/W4','/WX','/O2','/LD',str(fixture),f'/Fe:{dll}',f'/Fo:{out / "fixture.obj"}','/link','/OPT:NOICF',f'/IMPLIB:{out / "fixture.lib"}'],
              ['cl','/nologo','/std:c++20','/EHsc','/W4','/WX','/O2',f'/I{ROOT / "OptiScaler/misc"}',str(cpp),f'/Fe:{exe}',f'/Fo:{out / "runtime.obj"}'],[str(exe),str(dll)],
              ['cl','/nologo','/std:c++20','/EHsc','/W4','/WX','/O2',f'/I{ROOT / "OptiScaler/misc"}',f'/I{ROOT / "OptiScaler/include"}',str(real_cpp),f'/Fe:{real_exe}',f'/Fo:{out / "runtime-real.obj"}',str(ROOT/'OptiScaler/library/detours/detours.lib')],
              [str(real_exe),str(dll)]]
    logs=[]
    for command in commands:
        p=subprocess.run(command,capture_output=True,text=True,timeout=90)
        text=subprocess.list2cmdline(command)+'\n'+p.stdout+p.stderr+f'\nprocess_exit={p.returncode}\n'
        logs.append(text); (out/'runtime.log').write_text('\n'.join(logs),encoding='utf-8');print(text,flush=True)
        if p.returncode: return p.returncode
    return 0
if __name__=='__main__': raise SystemExit(main())
