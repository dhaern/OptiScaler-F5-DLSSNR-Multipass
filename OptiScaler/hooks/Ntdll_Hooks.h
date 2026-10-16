#pragma once
#include "SysUtils.h"
#include <Util.h>
#include <State.h>
#include <Config.h>
#include <DllNames.h>

#include "LibraryLoad_Hooks.h"
#include "Hook_Utils.h"
#include <misc/LoaderBoundary.h>

#include <cwctype>

#pragma intrinsic(_ReturnAddress)

class NtdllHooks
{
  private:
    inline static std::mutex hookMutex;

    inline static NtdllProxy::PFN_RtlGetVersion o_RtlGetVersion = nullptr;
    inline static NtdllProxy::PFN_NtLoadDll o_NtLoadDll = nullptr;
    inline static NtdllProxy::PFN_LdrLoadDll o_LdrLoadDll = nullptr;
    inline static NtdllProxy::PFN_LdrUnloadDll o_LdrUnloadDll = nullptr;

    static NTSTATUS NTAPI hkRtlGetVersion(PRTL_OSVERSIONINFOW lpVersionInformation)
    {
        if (ThreadHasNoTls())
            return o_RtlGetVersion(lpVersionInformation);

        if (lpVersionInformation == nullptr)
            return STATUS_INVALID_PARAMETER;

        auto result = o_RtlGetVersion(lpVersionInformation);

        if (lpVersionInformation->dwMajorVersion <= 10 && lpVersionInformation->dwBuildNumber < 21996 &&
            State::Instance().activeFgOutput == FGOutput::FSRFG && State::Instance().isRunningOnLinux &&
            Util::GetCallerModule(_ReturnAddress()) ==
                KernelBaseProxy::GetModuleHandleW_()(L"amd_fidelityfx_framegeneration_dx12.dll"))
        {
            lpVersionInformation->dwMajorVersion = 10;
            lpVersionInformation->dwBuildNumber = 21996;
        }

        return result;
    }

    static NTSTATUS NTAPI hkLdrLoadDll(PWSTR PathToFile, PULONG Flags, PUNICODE_STRING ModuleFileName,
                                       PHANDLE ModuleHandle)
    {
        const DWORD entryError = GetLastError();
        if (loader_boundary::OriginalOnly())
        {
            SetLastError(entryError);
            return o_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);
        }
        if (ThreadHasNoTls())
        {
            auto result = o_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);
            if (NT_SUCCESS(result) && ModuleHandle != nullptr)
                LibraryLoadHooks::RecordDeferredLoad(static_cast<HMODULE>(*ModuleHandle), false);
            return result;
        }

        const auto original = [&]()
        {
            loader_boundary::OriginalOnlyScope scope;
            SetLastError(entryError);
            return o_LdrLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);
        };

        // Preserve the historical local null-output status; original-only branches forward untouched inputs.
        if (ModuleHandle == nullptr)
            return STATUS_INVALID_PARAMETER;

        if (ModuleFileName == nullptr || ModuleFileName->Length == 0 || ModuleFileName->Buffer == nullptr ||
            ModuleFileName->Length % sizeof(wchar_t) != 0 || ModuleFileName->Length > ModuleFileName->MaximumLength)
            return original();

        // Native flag values are not Win32 dwFlags. Unrecognized native modes are original-only.
        if (Flags != nullptr && *Flags != 0)
            return original();
        const std::wstring_view view(ModuleFileName->Buffer, ModuleFileName->Length / sizeof(wchar_t));
        if (State::Instance().isShuttingDown || LibraryLoadHooks::IsApiSetName(view) ||
            view.find(L'\0') != std::wstring_view::npos)
            return original();

        std::wstring name;
        if (!loader_boundary::CopyName(view, name))
            return original();

#ifdef _DEBUG
        // LOG_DEBUG("{}, caller: {}", wstring_to_string(name.data()), Util::WhoIsTheCaller(_ReturnAddress()));
#endif

        if (State::SkipDllChecks())
        {
            const std::wstring skip = string_to_wstring(State::SkipDllName());

            if (skip.empty() || LibraryLoadHooks::EndsWithInsensitive(name, std::wstring_view(skip)) ||
                LibraryLoadHooks::EndsWithInsensitive(name, std::wstring(skip + L".dll")))
            {
                LOG_TRACE("Skip checks for: {}", wstring_to_string(name.data()));
                return original();
            }
        }

        bool ownLoadFailed = false;

        // if (originalModule != Util::GetCallerModule(_ReturnAddress()))
        {
            auto moduleHandle = LibraryLoadHooks::LoadLibraryCheckW(name.data(), name.data(), &ownLoadFailed);

            // skip loading of dll
            if (moduleHandle == (HMODULE) 1337)
            {
                return STATUS_DLL_NOT_FOUND;
            }

            if (moduleHandle != nullptr)
            {
                LOG_TRACE("{}, caller: {}", wstring_to_string(name.data()), Util::WhoIsTheCaller(_ReturnAddress()));
                *ModuleHandle = (HANDLE) moduleHandle;
                return (NTSTATUS) 0x00000000L;
            }
        }

        SetLastError(entryError);
        const NTSTATUS status = original();
        const DWORD originalError = GetLastError();

        // Arbitrary native recovery has no supported caller-context/provenance contract.
        SetLastError(originalError);
        return status;
    }

    static NTSTATUS NTAPI hkNtLoadDll(PUNICODE_STRING PathToFile, PULONG Flags, PUNICODE_STRING ModuleFileName,
                                      PHANDLE ModuleHandle)
    {
        const DWORD entryError = GetLastError();
        if (loader_boundary::OriginalOnly())
        {
            SetLastError(entryError);
            return o_NtLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);
        }
        if (ThreadHasNoTls())
        {
            auto result = o_NtLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);
            if (NT_SUCCESS(result) && ModuleHandle != nullptr)
                LibraryLoadHooks::RecordDeferredLoad(static_cast<HMODULE>(*ModuleHandle), false);
            return result;
        }

        const auto original = [&]()
        {
            loader_boundary::OriginalOnlyScope scope;
            SetLastError(entryError);
            return o_NtLoadDll(PathToFile, Flags, ModuleFileName, ModuleHandle);
        };

        // Preserve the historical local null-output status; original-only branches forward untouched inputs.
        if (ModuleHandle == nullptr)
            return STATUS_INVALID_PARAMETER;

        if (ModuleFileName == nullptr || ModuleFileName->Length == 0 || ModuleFileName->Buffer == nullptr ||
            ModuleFileName->Length % sizeof(wchar_t) != 0 || ModuleFileName->Length > ModuleFileName->MaximumLength)
            return original();

        // Native flag values are not Win32 dwFlags. Unrecognized native modes are original-only.
        if (Flags != nullptr && *Flags != 0)
            return original();
        const std::wstring_view view(ModuleFileName->Buffer, ModuleFileName->Length / sizeof(wchar_t));
        if (State::Instance().isShuttingDown || LibraryLoadHooks::IsApiSetName(view) ||
            view.find(L'\0') != std::wstring_view::npos)
            return original();

        std::wstring name;
        if (!loader_boundary::CopyName(view, name))
            return original();

#ifdef _DEBUG
        // LOG_DEBUG("{}, caller: {}", wstring_to_string(name.data()), Util::WhoIsTheCaller(_ReturnAddress()));
#endif

        // PathToFile is the native search path, not the requested module filename.
        const auto& path = name;

        if (State::SkipDllChecks())
        {
            const std::wstring skip = string_to_wstring(State::SkipDllName());

            if (skip.empty() || LibraryLoadHooks::EndsWithInsensitive(name, std::wstring_view(skip)) ||
                LibraryLoadHooks::EndsWithInsensitive(name, std::wstring(skip + L".dll")))
            {
                LOG_TRACE("Skip checks for: {}", wstring_to_string(name.data()));
                return original();
            }
        }

        bool ownLoadFailed = false;

        // Do not interfere with original modules calls
        if (originalModule != Util::GetCallerModule(_ReturnAddress()))
        {
            auto moduleHandle = LibraryLoadHooks::LoadLibraryCheckW(name.data(), path.data(), &ownLoadFailed);

            // skip loading of dll
            if (moduleHandle == (HMODULE) 1337)
            {
                return STATUS_DLL_NOT_FOUND;
            }

            if (moduleHandle != nullptr)
            {
                LOG_TRACE("{}, caller: {}", wstring_to_string(name.data()), Util::WhoIsTheCaller(_ReturnAddress()));
                *ModuleHandle = (HANDLE) moduleHandle;
                return (NTSTATUS) 0x00000000L;
            }
        }

        SetLastError(entryError);
        const NTSTATUS status = original();
        const DWORD originalError = GetLastError();

        // Arbitrary native recovery has no supported caller-context/provenance contract.
        SetLastError(originalError);
        return status;
    }

    static NTSTATUS NTAPI hkLdrUnloadDll(PVOID lpLibrary)
    {
        if (ThreadHasNoTls())
        {
            // See KernelHooks::hk_K32_FreeLibrary
            if (LibraryLoadHooks::IsKeptLoaded(lpLibrary))
                return (NTSTATUS) 0x00000000L;

            return o_LdrUnloadDll(lpLibrary);
        }

        if (lpLibrary == nullptr)
            return STATUS_INVALID_PARAMETER;

#ifdef _DEBUG
        // LOG_TRACE("{:X}", (size_t) lpLibrary);
#endif

        if (!State::Instance().isShuttingDown)
        {
            auto result = LibraryLoadHooks::FreeLibrary(lpLibrary);

            if (result.has_value())
                return result.value();
        }

        return o_LdrUnloadDll(lpLibrary);
    }

    VALIDATE_MEMBER_HOOK(hkRtlGetVersion, NtdllProxy::PFN_RtlGetVersion)
    VALIDATE_MEMBER_HOOK(hkNtLoadDll, NtdllProxy::PFN_NtLoadDll)
    VALIDATE_MEMBER_HOOK(hkLdrLoadDll, NtdllProxy::PFN_LdrLoadDll)
    VALIDATE_MEMBER_HOOK(hkLdrUnloadDll, NtdllProxy::PFN_LdrUnloadDll)

  public:
    static void Hook()
    {
        std::lock_guard lock(hookMutex);

        LOG_FUNC();

        if (!Config::Instance()->UseNtdllHooks.value_or_default())
            return;

        if (NtdllProxy::Module() == nullptr)
            return;

        if (o_RtlGetVersion == nullptr)
            o_RtlGetVersion = NtdllProxy::Hook_RtlGetVersion(hkRtlGetVersion);

        // if (o_NtLoadDll == nullptr)
        //     o_NtLoadDll = NtdllProxy::Hook_NtLoadDll(hkNtLoadDll);

        if (o_LdrLoadDll == nullptr)
            o_LdrLoadDll = NtdllProxy::Hook_LdrLoadDll(hkLdrLoadDll);

        if (o_LdrUnloadDll == nullptr)
            o_LdrUnloadDll = NtdllProxy::Hook_LdrUnloadDll(hkLdrUnloadDll);
    }
};
