#pragma once

#include "SysUtils.h"

#include <State.h>

#include <proxies/Ntdll_Proxy.h>
#include <proxies/KernelBase_Proxy.h>

#include <detours/detours.h>

#include <d3d12.h>
#include <misc/D3d12Binding.h>
#include <misc/LoaderBoundary.h>
#include <hooks/LibraryLoad_Hooks.h>

class D3d12Proxy
{
  public:
    typedef decltype(&D3D12CreateDevice) PFN_D3D12CreateDevice;
    typedef decltype(&D3D12SerializeRootSignature) PFN_D3D12SerializeRootSignature;
    typedef decltype(&D3D12CreateRootSignatureDeserializer) PFN_D3D12CreateRootSignatureDeserializer;
    typedef decltype(&D3D12SerializeVersionedRootSignature) PFN_D3D12SerializeVersionedRootSignature;
    typedef decltype(&D3D12CreateVersionedRootSignatureDeserializer) PFN_D3D12CreateVersionedRootSignatureDeserializer;
    typedef decltype(&D3D12GetDebugInterface) PFN_D3D12GetDebugInterface;
    typedef decltype(&D3D12EnableExperimentalFeatures) PFN_D3D12EnableExperimentalFeatures;
    typedef decltype(&D3D12GetInterface) PFN_D3D12GetInterface;

    static bool Init(HMODULE module = nullptr)
    {
        d3d12_binding::Claim claim;
        if (!claim)
            return false;
        const auto bound = Module();
        if (bound != nullptr)
            return module == nullptr || module == bound;

        loader_boundary::OriginalOnlyScope scope;
        bool ownReference = false;
        if (module == nullptr)
        {
            module = KernelBaseProxy::GetModuleHandleW_()(L"d3d12.dll");
            if (module == nullptr)
            {
                module = NtdllProxy::LoadLibraryExW_Ldr(L"d3d12.dll", NULL, 0);
                ownReference = module != nullptr;
            }
        }
        if (module == nullptr || module == dllModule || LibraryLoadHooks::KnownNonExecutable(module))
        {
            if (ownReference)
                NtdllProxy::FreeLibrary_Ldr(module);
            return false;
        }

        HMODULE retained = nullptr;
        const bool acquired =
            KernelBaseProxy::GetModuleHandleExW_()(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                                   reinterpret_cast<LPCWSTR>(module), &retained) != FALSE;
        if (ownReference)
            NtdllProxy::FreeLibrary_Ldr(module);
        if (!acquired)
            return false;

        wchar_t path[32768];
        const auto length = GetModuleFileNameW(retained, path, static_cast<DWORD>(std::size(path)));
        if (retained != module || length == 0 || length >= std::size(path))
        {
            NtdllProxy::FreeLibrary_Ldr(retained);
            return false;
        }

        _D3D12CreateDevice = (PFN_D3D12CreateDevice) KernelBaseProxy::GetProcAddress_()(retained, "D3D12CreateDevice");
        _D3D12SerializeRootSignature = (PFN_D3D12SerializeRootSignature) KernelBaseProxy::GetProcAddress_()(
            retained, "D3D12SerializeRootSignature");
        _D3D12CreateRootSignatureDeserializer =
            (PFN_D3D12CreateRootSignatureDeserializer) KernelBaseProxy::GetProcAddress_()(
                retained, "D3D12CreateRootSignatureDeserializer");
        _D3D12SerializeVersionedRootSignature =
            (PFN_D3D12SerializeVersionedRootSignature) KernelBaseProxy::GetProcAddress_()(
                retained, "D3D12SerializeVersionedRootSignature");
        _D3D12CreateVersionedRootSignatureDeserializer =
            (PFN_D3D12CreateVersionedRootSignatureDeserializer) KernelBaseProxy::GetProcAddress_()(
                retained, "D3D12CreateVersionedRootSignatureDeserializer");
        _D3D12GetDebugInterface =
            (PFN_D3D12GetDebugInterface) KernelBaseProxy::GetProcAddress_()(retained, "D3D12GetDebugInterface");
        _D3D12EnableExperimentalFeatures = (PFN_D3D12EnableExperimentalFeatures) KernelBaseProxy::GetProcAddress_()(
            retained, "D3D12EnableExperimentalFeatures");
        _D3D12GetInterface = (PFN_D3D12GetInterface) KernelBaseProxy::GetProcAddress_()(retained, "D3D12GetInterface");
        if (_D3D12CreateDevice == nullptr)
        {
            NtdllProxy::FreeLibrary_Ldr(retained);
            return false;
        }
        // One independent reference owns all published exports and any subsequently committed detours.
        _dll.store(retained, std::memory_order_release);
        return true;
    }

    static HMODULE Module() { return _dll; }

    // Original methods
    static PFN_D3D12CreateDevice D3D12CreateDevice_() { return Module() != nullptr ? _D3D12CreateDevice : nullptr; }
    static PFN_D3D12SerializeRootSignature D3D12SerializeRootSignature_()
    {
        return Module() != nullptr ? _D3D12SerializeRootSignature : nullptr;
    }
    static PFN_D3D12CreateRootSignatureDeserializer D3D12CreateRootSignatureDeserializer_()
    {
        return Module() != nullptr ? _D3D12CreateRootSignatureDeserializer : nullptr;
    }
    static PFN_D3D12SerializeVersionedRootSignature D3D12SerializeVersionedRootSignature_()
    {
        return Module() != nullptr ? _D3D12SerializeVersionedRootSignature : nullptr;
    }
    static PFN_D3D12CreateVersionedRootSignatureDeserializer D3D12CreateVersionedRootSignatureDeserializer_()
    {
        return Module() != nullptr ? _D3D12CreateVersionedRootSignatureDeserializer : nullptr;
    }
    static PFN_D3D12GetDebugInterface D3D12GetDebugInterface_()
    {
        return Module() != nullptr ? _D3D12GetDebugInterface : nullptr;
    }
    static PFN_D3D12EnableExperimentalFeatures D3D12EnableExperimentalFeatures_()
    {
        return Module() != nullptr ? _D3D12EnableExperimentalFeatures : nullptr;
    }
    static PFN_D3D12GetInterface D3D12GetInterface_() { return Module() != nullptr ? _D3D12GetInterface : nullptr; }

    // Hooked methods
    static PFN_D3D12CreateDevice D3D12CreateDevice_Hooked()
    {
        return (PFN_D3D12CreateDevice) KernelBaseProxy::GetProcAddress_()(_dll, "D3D12CreateDevice");
    }
    static PFN_D3D12SerializeRootSignature D3D12SerializeRootSignature_Hooked()
    {
        return (PFN_D3D12SerializeRootSignature) KernelBaseProxy::GetProcAddress_()(_dll,
                                                                                    "D3D12SerializeRootSignature");
    }
    static PFN_D3D12CreateRootSignatureDeserializer D3D12CreateRootSignatureDeserializer_Hooked()
    {
        return (PFN_D3D12CreateRootSignatureDeserializer) KernelBaseProxy::GetProcAddress_()(
            _dll, "D3D12CreateRootSignatureDeserializer");
    }
    static PFN_D3D12SerializeVersionedRootSignature D3D12SerializeVersionedRootSignature_Hooked()
    {
        return (PFN_D3D12SerializeVersionedRootSignature) KernelBaseProxy::GetProcAddress_()(
            _dll, "D3D12SerializeVersionedRootSignature");
    }
    static PFN_D3D12CreateVersionedRootSignatureDeserializer D3D12CreateVersionedRootSignatureDeserializer_Hooked()
    {
        return (PFN_D3D12CreateVersionedRootSignatureDeserializer) KernelBaseProxy::GetProcAddress_()(
            _dll, "D3D12CreateVersionedRootSignatureDeserializer");
    }
    static PFN_D3D12GetDebugInterface D3D12GetDebugInterface_Hooked()
    {
        return (PFN_D3D12GetDebugInterface) KernelBaseProxy::GetProcAddress_()(_dll, "D3D12GetDebugInterface");
    }
    static PFN_D3D12EnableExperimentalFeatures D3D12EnableExperimentalFeatures_Hooked()
    {
        return (PFN_D3D12EnableExperimentalFeatures) KernelBaseProxy::GetProcAddress_()(
            _dll, "D3D12EnableExperimentalFeatures");
    }
    static PFN_D3D12GetInterface D3D12GetInterface_Hooked()
    {
        return (PFN_D3D12GetInterface) KernelBaseProxy::GetProcAddress_()(_dll, "D3D12GetInterface");
    }

    // Detours updates the actual callback trampoline slot as part of its commit, not a later local-to-global copy.
    template <class T> static bool AttachExport(T address, PVOID method, T* original)
    {
        if (address == nullptr || DetourTransactionBegin() != NO_ERROR)
            return false;
        *original = address;
        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
            DetourAttach(reinterpret_cast<PVOID*>(original), method) != NO_ERROR)
        {
            DetourTransactionAbort();
            *original = nullptr;
            return false;
        }
        if (DetourTransactionCommit() != NO_ERROR)
        {
            *original = nullptr;
            return false;
        }
        return true;
    }

    // Hook methods
    static bool Hook_D3D12CreateDevice(PVOID method, PFN_D3D12CreateDevice* original)
    {
        return AttachExport(D3D12CreateDevice_ForHook(), method, original);
    }

    static bool Hook_D3D12SerializeRootSignature(PVOID method, PFN_D3D12SerializeRootSignature* original)
    {
        return AttachExport(D3D12SerializeRootSignature_ForHook(), method, original);
    }

    static PFN_D3D12CreateRootSignatureDeserializer Hook_D3D12CreateRootSignatureDeserializer(PVOID method)
    {
        auto addr = D3D12CreateRootSignatureDeserializer_ForHook();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook: {:X}", detourResult);
            return nullptr;
        }

        //_D3D12CreateRootSignatureDeserializer = addr;
        return addr;
    }

    static bool Hook_D3D12SerializeVersionedRootSignature(PVOID method,
                                                          PFN_D3D12SerializeVersionedRootSignature* original)
    {
        return AttachExport(D3D12SerializeVersionedRootSignature_ForHook(), method, original);
    }

    static PFN_D3D12CreateVersionedRootSignatureDeserializer
    Hook_D3D12CreateVersionedRootSignatureDeserializer(PVOID method)
    {
        auto addr = D3D12CreateVersionedRootSignatureDeserializer_ForHook();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook: {:X}", detourResult);
            return nullptr;
        }

        //_D3D12CreateVersionedRootSignatureDeserializer = addr;
        return addr;
    }

    static PFN_D3D12GetDebugInterface Hook_D3D12GetDebugInterface(PVOID method)
    {
        auto addr = D3D12GetDebugInterface_ForHook();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook: {:X}", detourResult);
            return nullptr;
        }

        //_D3D12GetDebugInterface = addr;
        return addr;
    }

    static PFN_D3D12EnableExperimentalFeatures Hook_D3D12EnableExperimentalFeatures(PVOID method)
    {
        auto addr = D3D12EnableExperimentalFeatures_ForHook();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook: {:X}", detourResult);
            return nullptr;
        }

        //_D3D12EnableExperimentalFeatures = addr;
        return addr;
    }

    static PFN_D3D12GetInterface Hook_D3D12GetInterface(PVOID method)
    {
        auto addr = D3D12GetInterface_ForHook();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook: {:X}", detourResult);
            return nullptr;
        }

        //_D3D12GetInterface = addr;
        return addr;
    }

  private:
    inline static constinit std::atomic<HMODULE> _dll { nullptr };

    inline static PFN_D3D12CreateDevice _D3D12CreateDevice = nullptr;
    inline static PFN_D3D12SerializeRootSignature _D3D12SerializeRootSignature = nullptr;
    inline static PFN_D3D12CreateRootSignatureDeserializer _D3D12CreateRootSignatureDeserializer = nullptr;
    inline static PFN_D3D12SerializeVersionedRootSignature _D3D12SerializeVersionedRootSignature = nullptr;
    inline static PFN_D3D12CreateVersionedRootSignatureDeserializer _D3D12CreateVersionedRootSignatureDeserializer =
        nullptr;
    inline static PFN_D3D12GetDebugInterface _D3D12GetDebugInterface = nullptr;
    inline static PFN_D3D12EnableExperimentalFeatures _D3D12EnableExperimentalFeatures = nullptr;
    inline static PFN_D3D12GetInterface _D3D12GetInterface = nullptr;

    inline static HMODULE HookModule()
    {
        if (State::Instance().workingMode == WorkingMode::D3d12)
            return dllModule;

        return _dll;
    }

    static PFN_D3D12CreateDevice D3D12CreateDevice_ForHook()
    {
        return (PFN_D3D12CreateDevice) KernelBaseProxy::GetProcAddress_()(HookModule(), "D3D12CreateDevice");
    }
    static PFN_D3D12SerializeRootSignature D3D12SerializeRootSignature_ForHook()
    {
        return (PFN_D3D12SerializeRootSignature) KernelBaseProxy::GetProcAddress_()(HookModule(),
                                                                                    "D3D12SerializeRootSignature");
    }
    static PFN_D3D12CreateRootSignatureDeserializer D3D12CreateRootSignatureDeserializer_ForHook()
    {
        return (PFN_D3D12CreateRootSignatureDeserializer) KernelBaseProxy::GetProcAddress_()(
            HookModule(), "D3D12CreateRootSignatureDeserializer");
    }
    static PFN_D3D12SerializeVersionedRootSignature D3D12SerializeVersionedRootSignature_ForHook()
    {
        return (PFN_D3D12SerializeVersionedRootSignature) KernelBaseProxy::GetProcAddress_()(
            HookModule(), "D3D12SerializeVersionedRootSignature");
    }
    static PFN_D3D12CreateVersionedRootSignatureDeserializer D3D12CreateVersionedRootSignatureDeserializer_ForHook()
    {
        return (PFN_D3D12CreateVersionedRootSignatureDeserializer) KernelBaseProxy::GetProcAddress_()(
            HookModule(), "D3D12CreateVersionedRootSignatureDeserializer");
    }
    static PFN_D3D12GetDebugInterface D3D12GetDebugInterface_ForHook()
    {
        return (PFN_D3D12GetDebugInterface) KernelBaseProxy::GetProcAddress_()(HookModule(), "D3D12GetDebugInterface");
    }
    static PFN_D3D12EnableExperimentalFeatures D3D12EnableExperimentalFeatures_ForHook()
    {
        return (PFN_D3D12EnableExperimentalFeatures) KernelBaseProxy::GetProcAddress_()(
            HookModule(), "D3D12EnableExperimentalFeatures");
    }
    static PFN_D3D12GetInterface D3D12GetInterface_ForHook()
    {
        return (PFN_D3D12GetInterface) KernelBaseProxy::GetProcAddress_()(HookModule(), "D3D12GetInterface");
    }
};
