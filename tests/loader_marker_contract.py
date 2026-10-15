"""Compile the actual KernelBase/Kernel32 marker wrappers with explicit safe API stand-ins."""
from pathlib import Path
import subprocess, sys
from loader_wrapper_contract import ROOT, function_body
PRE = r'''
#include <windows.h>
#include <intrin.h>
#include <string>
#include <cstdio>
#define LOG_TRACE(...) ((void)0)
#define LOG_INFO(...) ((void)0)
static bool noTls = false;
static bool ThreadHasNoTls() { return noTls; }
static HMODULE amdxc64Mark = reinterpret_cast<HMODULE>(0x1337), dllModule = nullptr;
static HMODULE realAmd = reinterpret_cast<HMODULE>(0x10000), passed = nullptr;
static LPCSTR passedName = nullptr;
static int calls = 0, failures = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)
struct Amdxc64Hooks { static void hkAmdExtD3DCreateInterface() {} };
inline HMODULE WINAPI ModuleW(LPCWSTR) { return realAmd; }
inline FARPROC WINAPI Proc(HMODULE m, LPCSTR name) { passed=m; passedName=name; ++calls; return reinterpret_cast<FARPROC>(0x7777); }
struct Kernel32Proxy { using PFN_GetProcAddress = decltype(&Proc); };
struct KernelBaseProxy { static auto GetModuleHandleW_() { return &ModuleW; } };
struct LibraryLoadHooks { static void CatchUpDeferredLoads() {} };
enum class FSR4Support { None, INT8 };
struct Gpu { FSR4Support fsr4Support = FSR4Support::INT8; };
struct IdentifyGpu { static Gpu getPrimaryGpu() { return {}; } };
struct Util { static HMODULE GetCallerModule(void*) { return nullptr; } };
struct KernelHooks {
    inline static auto o_K32_GetProcAddress = Proc, o_KB_GetProcAddress = Proc;
    inline static auto o_K32_GetModuleHandleW = ModuleW;
    static FARPROC WINAPI hk_K32_GetProcAddress(HMODULE, LPCSTR);
    static FARPROC WINAPI hk_KB_GetProcAddress(HMODULE, LPCSTR);
};
'''
MAIN = r'''
int main() {
    for (bool tls : {false,true}) {
        noTls = !tls;
        for (auto get : {KernelHooks::hk_K32_GetProcAddress, KernelHooks::hk_KB_GetProcAddress}) {
            calls=0;
            CHECK(get(amdxc64Mark,"AmdExtD3DCreateInterface") == reinterpret_cast<FARPROC>(&Amdxc64Hooks::hkAmdExtD3DCreateInterface));
            CHECK(calls == 0);
            for (auto name : {"Other", reinterpret_cast<LPCSTR>(7), static_cast<LPCSTR>(nullptr)}) {
                calls=0; CHECK(get(amdxc64Mark,name) == reinterpret_cast<FARPROC>(0x7777));
                CHECK(calls==1); CHECK(passed==realAmd); CHECK(passedName==name);
            }
        }
    }
    if (!failures) printf("marker_contract: both ABI routes and TLS states, names/ordinals/null preserved\n");
    return failures ? 1 : 0;
}
'''
def main():
    out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
    rev=sys.argv[2] if len(sys.argv)>2 else None
    source=subprocess.check_output(['git','-C',str(ROOT),'show',f'{rev}:OptiScaler/hooks/Kernel_Hooks.cpp'],text=True) if rev else (ROOT/'OptiScaler/hooks/Kernel_Hooks.cpp').read_text(encoding='utf-8-sig')
    parts=[PRE]
    if 'static FARPROC ResolveAmdMarker(' in source: parts.append(function_body(source,'static FARPROC ResolveAmdMarker('))
    parts += [function_body(source,'FARPROC WINAPI KernelHooks::'+name+'(') for name in ['hk_K32_GetProcAddress','hk_KB_GetProcAddress']]
    cpp=out/'marker.generated.cpp';cpp.write_text('\n'.join(parts+[MAIN]),encoding='utf-8');exe=out/'marker.exe'
    commands=[['cl','/nologo','/std:c++20','/EHsc','/W4','/WX','/O2',str(cpp),f'/Fe:{exe}',f'/Fo:{out / "marker.obj"}'],[str(exe)]]
    logs=[]
    for command in commands:
        p=subprocess.run(command,capture_output=True,text=True,timeout=60);text=subprocess.list2cmdline(command)+'\n'+p.stdout+p.stderr+f'\nprocess_exit={p.returncode}\n';print(text,flush=True);logs.append(text);(out/'marker.log').write_text('\n'.join(logs),encoding='utf-8')
        if p.returncode:return p.returncode
    return 0
if __name__=='__main__':raise SystemExit(main())
