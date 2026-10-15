"""Busy initial binding: exact adapter body and capability pre-publication seam.

The capability seam ends before its GPU/vendor work; it asserts that unavailable
exports cannot mark that work complete. No graphics provider or TLS is falsified.
"""
from pathlib import Path
import subprocess
import sys
from loader_wrapper_contract import ROOT, function_body
from loader_runtime_contract import PRE


def main():
    out = Path(sys.argv[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    revision = sys.argv[2] if len(sys.argv) > 2 else None
    def source(path):
        if revision:
            return subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{revision}:{path}'], text=True)
        return (ROOT / path).read_text(encoding='utf-8-sig')
    pre = PRE.replace('#include "LoaderBoundary.h"', source('OptiScaler/misc/LoaderBoundary.h').replace('#pragma once', ''))
    pre = pre.replace('#include "D3d12Binding.h"', source('OptiScaler/misc/D3d12Binding.h').replace('#pragma once', ''))
    proxy = source('OptiScaler/proxies/D3D12_Proxy.h')
    proxy = proxy[proxy.index('class D3d12Proxy'):]
    identify = source('OptiScaler/misc/IdentifyGpu.cpp')
    adapter = function_body(identify, 'void IdentifyGpu::getHardwareAdapter(')
    capability = function_body(identify, 'void IdentifyGpu::updateD3d12Capabilities(')
    capability = capability[:capability.index('    struct D3d12Result')] + '    ++capabilityWork;\n}\n'
    extra = r'''
#include <dxgi1_6.h>
#define LOG_FUNC(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
struct ScopedSkipDxgiLoadChecks {};
struct GpuInformation { LUID luid; };
struct IdentifyGpu {
    inline static bool hasD3d12Capabilities = false;
    inline static unsigned capabilityWork = 0;
    inline static std::mutex mutex;
    static std::vector<GpuInformation> getAllGpus() {
        IDXGIFactory1* f=nullptr; IDXGIAdapter1* a=nullptr; DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return {};
        if (SUCCEEDED(f->EnumAdapters1(0,&a))) { a->GetDesc1(&desc); a->Release(); }
        f->Release(); return {{desc.AdapterLuid}};
    }
    static void getHardwareAdapter(IDXGIFactory*, IDXGIAdapter**, D3D_FEATURE_LEVEL);
    static void updateD3d12Capabilities(D3d12Proxy::PFN_D3D12CreateDevice = nullptr);
};
'''
    main_cpp = r'''
static bool faulted = false;
static void Invoke(IDXGIFactory* f, IDXGIAdapter** a) {
    __try { IdentifyGpu::getHardwareAdapter(f,a,D3D_FEATURE_LEVEL_11_0); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { faulted=true; }
}
int main() {
    IDXGIFactory1* f=nullptr; CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))));
    if (!f) return 2;
    IDXGIAdapter* a=nullptr;
    HMODULE runtime=LoadLibraryW(L"d3d12.dll"); CHECK(runtime);
    {
        d3d12_binding::Claim pausedInstaller; CHECK(bool(pausedInstaller));
        std::thread t([&] { Invoke(f,&a); IdentifyGpu::updateD3d12Capabilities(); }); t.join();
        CHECK(!faulted && !a);
        CHECK(!IdentifyGpu::hasD3d12Capabilities && !IdentifyGpu::capabilityWork);
        CHECK(!D3d12Proxy::Module());
    }
    if (a) { a->Release(); a=nullptr; }
    Invoke(f,&a); CHECK(!faulted && a && D3d12Proxy::D3D12CreateDevice_());
    IdentifyGpu::updateD3d12Capabilities();
    CHECK(IdentifyGpu::hasD3d12Capabilities && IdentifyGpu::capabilityWork==1);
    IdentifyGpu::updateD3d12Capabilities(); CHECK(IdentifyGpu::capabilityWork==1);
    if (a) a->Release(); f->Release(); if (runtime) FreeLibrary(runtime);
    printf("busy_consumer: fault=%d capability_work=%u failures=%d\n",faulted,IdentifyGpu::capabilityWork,failures);
    return failures ? 1 : 0;
}
'''
    cpp = out / 'consumer.cpp'
    cpp.write_text(pre + proxy + extra + adapter + '\n' + capability + main_cpp)
    # Frozen consumer's unused EnumAdapterByLuid result is an inherited warning;
    # allow that warning ONLY on the frozen behavioral-red compile.
    flags = ['/W4', '/WX'] if not revision else ['/W4', '/WX-']
    commands = [ ['cl','/nologo','/std:c++20','/EHsc',*flags,'/O2',str(cpp),f'/Fo{out}/consumer.obj',f'/Fe{out}/consumer.exe','dxgi.lib'], [str(out/'consumer.exe')] ]
    for command in commands:
        p = subprocess.run(command,capture_output=True,text=True,timeout=90)
        text = subprocess.list2cmdline(command)+'\n'+p.stdout+p.stderr+f'\nprocess_exit={p.returncode}\n'
        with (out/'consumer.log').open('a') as log: log.write(text)
        print(text,flush=True)
        if p.returncode: return p.returncode
    return 0

if __name__ == '__main__': raise SystemExit(main())
