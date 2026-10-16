#include "pch.h"
#include "Kernel_Hooks.h"

#include "Gdi32_Hooks.h"
#include "Streamline_Hooks.h"
#include "LibraryLoad_Hooks.h"

#include <fsr4/FSR4ModelSelection.h>

#include <Util.h>
#include <State.h>
#include <Config.h>

#include <cwctype>
#include <misc/IdentifyGpu.h>

#include "Hook_Utils.h"
#include <proxies/XeLL_Proxy.h>

#include "Amdxc64_Hooks.h"
#include <misc/LoaderBoundary.h>
#pragma intrinsic(_ReturnAddress)

static inline void NormalizePath(std::string& path)
{
    while (!path.empty() && (path.back() == '\\' || path.back() == '/'))
        path.pop_back();
}

static inline bool IsInsideWindowsDirectory(const std::string& path)
{
    char windowsDir[MAX_PATH];
    UINT len = GetWindowsDirectoryA(windowsDir, MAX_PATH);

    if (len == 0 || len >= MAX_PATH)
        return false;

    std::string pathToCheck(path);
    std::string windowsPath(windowsDir);

    NormalizePath(pathToCheck);
    NormalizePath(windowsPath);

    to_lower_in_place(pathToCheck);
    to_lower_in_place(windowsPath);

    // Check if pathToCheck starts with windowsPath, while having a slash after that
    if (pathToCheck.compare(0, windowsPath.size(), windowsPath) == 0 &&
        (pathToCheck.size() == windowsPath.size() || pathToCheck[windowsPath.size()] == '\\' ||
         pathToCheck[windowsPath.size()] == '/'))
        return true;

    return false;
}

static inline HMODULE CheckLoad(const std::wstring& name, bool* ownLoadFailed)
{
    do
    {
        if (State::Instance().isShuttingDown || LibraryLoadHooks::IsApiSetName(name))
            break;

        if (State::SkipDllChecks())
        {
            const std::wstring skip = string_to_wstring(State::SkipDllName());

            if (skip.empty() || LibraryLoadHooks::EndsWithInsensitive(name, std::wstring_view(skip)) ||
                LibraryLoadHooks::EndsWithInsensitive(name, std::wstring(skip + L".dll")))
            {
                LOG_TRACE("Skip checks for: {}", wstring_to_string(name.data()));
                break;
            }
        }

        auto moduleHandle = LibraryLoadHooks::LoadLibraryCheckW(name.data(), name.data(), ownLoadFailed);

        // skip loading of dll
        if (moduleHandle == (HMODULE) 1337)
            break;

        if (moduleHandle != nullptr)
        {
            LOG_TRACE("{}, caller: {}", wstring_to_string(name.data()), Util::WhoIsTheCaller(_ReturnAddress()));
            return moduleHandle;
        }
    } while (false);

    return nullptr;
}

// In native mode these Win32 hooks add only the supported D3D12 recovery boundary and no-init veto.
// Forward other ordinary loads without suppressing the existing native provider policy.
template <class C> static bool NativeLoadOnly(const C* path, DWORD flags = 0)
{
    if (ThreadHasNoTls() || !Config::Instance()->UseNtdllHooks.value_or_default() ||
        loader_boundary::NonExecutable(flags))
        return false;
    std::basic_string_view<C> name = path ? path : std::basic_string_view<C> {};
    for (size_t i = name.size(); i != 0; --i)
        if (name[i - 1] == '/' || name[i - 1] == '\\')
        {
            name.remove_prefix(i);
            break;
        }
    constexpr char target[] = "d3d12.dll";
    if (name.size() != 5 && name.size() != 9)
        return true;
    for (size_t i = 0; i < name.size(); ++i)
    {
        auto c = name[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<C>(c + ('a' - 'A'));
        if (c != target[i])
            return true;
    }
    return false;
}

VALIDATE_HOOK(hk_K32_GetProcAddress, Kernel32Proxy::PFN_GetProcAddress)
static FARPROC ResolveAmdMarker(LPCSTR name, Kernel32Proxy::PFN_GetProcAddress getProc)
{
    if (reinterpret_cast<ULONG_PTR>(name) >= 0xF000 && lstrcmpA(name, "AmdExtD3DCreateInterface") == 0)
        return reinterpret_cast<FARPROC>(&Amdxc64Hooks::hkAmdExtD3DCreateInterface);
    return getProc(KernelBaseProxy::GetModuleHandleW_()(L"amdxc64.dll"), name);
}

FARPROC WINAPI KernelHooks::hk_K32_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
    if (hModule == amdxc64Mark)
        return ResolveAmdMarker(lpProcName, o_K32_GetProcAddress);
    if (ThreadHasNoTls())
        return o_K32_GetProcAddress(hModule, lpProcName);

    if ((size_t) lpProcName < 0x000000000000F000)
    {
        if (hModule == dllModule)
            LOG_TRACE("Ordinal call: {:X}", (size_t) lpProcName);

        return o_K32_GetProcAddress(hModule, lpProcName);
    }

    // if (hModule == dllModule && lpProcName != nullptr)
    //{
    //     LOG_TRACE("Trying to get process address of {}, caller: {}", lpProcName,
    //               Util::WhoIsTheCaller(_ReturnAddress()));
    // }

    // FSR 4 Init in case of missing amdxc64.dll
    // 2nd check is amdxcffx64.dll trying to queue amdxc64 but amdxc64 not being loaded.
    // Also skip the internal call of amdxc64
    if (lpProcName != nullptr && (hModule == amdxc64Mark || hModule == nullptr) &&
        lstrcmpA(lpProcName, "AmdExtD3DCreateInterface") == 0 &&
        IdentifyGpu::getPrimaryGpu().fsr4Support != FSR4Support::None &&
        Util::GetCallerModule(_ReturnAddress()) != KernelBaseProxy::GetModuleHandleW_()(L"amdxc64.dll"))
    {
        LOG_TRACE("Giving hkAmdExtD3DCreateInterface");
        return (FARPROC) &Amdxc64Hooks::hkAmdExtD3DCreateInterface;
    }

    return o_K32_GetProcAddress(hModule, lpProcName);
}

VALIDATE_HOOK(hk_K32_GetModuleHandleA, Kernel32Proxy::PFN_GetModuleHandleA)
HMODULE WINAPI KernelHooks::hk_K32_GetModuleHandleA(LPCSTR lpModuleName)
{
    if (ThreadHasNoTls())
        return o_K32_GetModuleHandleA(lpModuleName);

    if (lpModuleName != NULL)
    {
        if (strcmp(lpModuleName, "nvngx_dlssg.dll") == 0)
        {
            LOG_TRACE("Trying to get module handle of {}, caller: {}", lpModuleName,
                      Util::WhoIsTheCaller(_ReturnAddress()));
            return dllModule;
        }
        else if (strcmp(lpModuleName, "amdxc64.dll") == 0)
        {
            // Libraries like FFX SDK or AntiLag 2 SDK do not load amdxc64 themselves
            // so most likely amdxc64 is getting loaded by the driver itself.
            // Therefore it should be safe for us to return a custom implementation when it's not loaded
            // This can get removed if Proton starts to ship amdxc64

            // For system with amdxc64 loaded - we should've caught the load and hooked it
            // For systems without - we provide that app with a fake handle and track the usage that way

            auto original = o_K32_GetModuleHandleA(lpModuleName);

            auto primaryGpu = IdentifyGpu::getPrimaryGpu();

            if (original == nullptr && primaryGpu.fsr4Support != FSR4Support::None)
            {
                LOG_INFO("giving a fake HMODULE for amdxc64.dll");
                return amdxc64Mark;
            }

            return original;
        }
    }

    return o_K32_GetModuleHandleA(lpModuleName);
}

VALIDATE_HOOK(hk_K32_GetModuleHandleW, Kernel32Proxy::PFN_GetModuleHandleW)
HMODULE WINAPI KernelHooks::hk_K32_GetModuleHandleW(LPCWSTR lpModuleName)
{
    if (ThreadHasNoTls())
        return o_K32_GetModuleHandleW(lpModuleName);

    if (lpModuleName != NULL)
    {
        if (wcscmp(lpModuleName, L"amdxc64.dll") == 0)
        {
            LOG_TRACE("amdxc64.dll call");

            // See comments in hk_K32_GetModuleHandleA

            auto original = o_K32_GetModuleHandleW(lpModuleName);

            auto primaryGpu = IdentifyGpu::getPrimaryGpu();

            if (original == nullptr && primaryGpu.fsr4Support != FSR4Support::None)
            {
                LOG_INFO("giving a fake HMODULE for amdxc64.dll");
                return amdxc64Mark;
            }

            return original;
        }
    }

    return o_K32_GetModuleHandleW(lpModuleName);
}

// "libxell.dll" without case, ASCII only. No CRT locale (_stricmp can take its lock): the GetModuleHandleEx hooks can
// run under the loader lock. Stops at the first mismatch, so it never reads past a shorter name.
template <class C> static bool IsXellName(const C* name)
{
    static constexpr char xell[] = "libxell.dll";

    for (size_t i = 0; i < sizeof(xell); ++i)
    {
        C c = name[i];

        if (c >= 'A' && c <= 'Z')
            c = static_cast<C>(c - 'A' + 'a');

        if (c != static_cast<C>(xell[i]))
            return false;
    }

    return true;
}

VALIDATE_HOOK(hk_K32_GetModuleHandleExA, Kernel32Proxy::PFN_GetModuleHandleExA)
// XeFG finds the XeLL it pairs with by asking for libxell.dll by name. It gets the XeLL OptiScaler loaded, so XeFG and
// OptiScaler share one XeLL (R1, R6) and no other copy has to be patched (XL16). Both A and W are answered: which one a
// given XeFG build calls is not known here, and names are compared without case.
bool KernelHooks::AnswerXellLookup(DWORD dwFlags, HMODULE* phModule, BOOL* result)
{
    if (dwFlags != 0 || phModule == nullptr)
        return false;

#ifdef LOW_LATENCY_INPUTS
    *phModule = dllModule;
    *result = TRUE;
    return true;
#else
    const auto module = XeLLProxy::Module();

    if (module == nullptr)
        return false;

    static std::atomic_bool logged { false };

    if (!logged.exchange(true))
    {
        LOG_INFO("libxell.dll lookup answered with OptiScaler's XeLL: one XeLL for XeFG and OptiScaler");

        // The game's own libxell.dll loaded first: an XeFG build that reaches XeLL another way than this lookup would
        // bind to that copy and get a context made by ours. Said once, so a log shows it.
        const HMODULE first = GetModuleHandleW(L"libxell.dll");

        if (first != nullptr && first != module)
            LOG_WARN("Another libxell.dll is loaded besides OptiScaler's XeLL; XeFG is steered to OptiScaler's only "
                     "through this lookup");
    }

    // By address, through the system call: neither hook compares an address as a name, and it does not depend on one
    // particular original (A or W) having been detoured
    *result = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), phModule);
    return true;
#endif
}

BOOL WINAPI KernelHooks::hk_K32_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule)
{
    if (ThreadHasNoTls())
        return o_K32_GetModuleHandleExA(dwFlags, lpModuleName, phModule);

    // Flags first: with GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS lpModuleName is an address, not a name to compare
    if (dwFlags == 0 && lpModuleName && IsXellName(lpModuleName))
    {
        BOOL result = FALSE;

        if (AnswerXellLookup(dwFlags, phModule, &result))
            return result;
    }

    return o_K32_GetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

VALIDATE_HOOK(hk_K32_GetModuleHandleExW, Kernel32Proxy::PFN_GetModuleHandleExW)
BOOL WINAPI KernelHooks::hk_K32_GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule)
{
    if (ThreadHasNoTls())
        return o_K32_GetModuleHandleExW(dwFlags, lpModuleName, phModule);

    if (lpModuleName && dwFlags == GET_MODULE_HANDLE_EX_FLAG_PIN && lstrcmpW(L"nvapi64.dll", lpModuleName) == 0 &&
        phModule)
    {
        LOG_TRACE("Suspected SpecialK call for nvapi64");
        *phModule = LibraryLoadHooks::LoadNvApi();
        return true;
    }

    // Flags first: with GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS lpModuleName is an address, not a name to compare
    if (dwFlags == 0 && lpModuleName && IsXellName(lpModuleName))
    {
        BOOL result = FALSE;

        if (AnswerXellLookup(dwFlags, phModule, &result))
            return result;
    }

    return o_K32_GetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

VALIDATE_HOOK(hk_KB_GetProcAddress, KernelBaseProxy::PFN_GetProcAddress)
FARPROC WINAPI KernelHooks::hk_KB_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
    if (hModule == amdxc64Mark)
        return ResolveAmdMarker(lpProcName, o_KB_GetProcAddress);
    if (ThreadHasNoTls())
        return o_KB_GetProcAddress(hModule, lpProcName);

    if ((size_t) lpProcName < 0x000000000000F000)
    {
        if (hModule == dllModule)
            LOG_TRACE("Ordinal call: {:X}", (size_t) lpProcName);

        return o_KB_GetProcAddress(hModule, lpProcName);
    }

    // if (hModule == dllModule && lpProcName != nullptr)
    //{
    //     LOG_TRACE("Trying to get process address of {}, caller: {}", lpProcName,
    //               Util::WhoIsTheCaller(_ReturnAddress()));
    // }

    return o_KB_GetProcAddress(hModule, lpProcName);
}

VALIDATE_HOOK(hk_K32_GetFileAttributesW, Kernel32Proxy::PFN_GetFileAttributesW)
DWORD WINAPI KernelHooks::hk_K32_GetFileAttributesW(LPCWSTR lpFileName)
{
    if (ThreadHasNoTls())
        return o_K32_GetFileAttributesW(lpFileName);

    if (!State::Instance().nvngxExists && State::Instance().nvngxReplacement.has_value() &&
        (Config::Instance()->DxgiSpoofing.value_or_default() ||
         Config::Instance()->StreamlineSpoofing.value_or_default()))
    {
        auto path = wstring_to_string(std::wstring(lpFileName));
        to_lower_in_place(path);

        // "nvngx.dll_" excludes OUR neural-rendering forwarder, named nvngx.dll_dlssnr.dll -- it
        // contains the substring "nvngx.dll", so without this the spoof redirects an attempt to open
        // the forwarder to the signed driver nvngx.dll instead, and the wrong image is loaded as the
        // forwarder. That crashed Dragon's Dogma 2 the moment Neural Rendering first initialised.
        if (path.contains("nvngx.dll") && !path.contains("_nvngx.dll") && !path.contains("nvngx.dll_") &&
            !IsInsideWindowsDirectory(path)) // apply the override to just one path
        {
            LOG_DEBUG("Overriding GetFileAttributesW for nvngx");
            return FILE_ATTRIBUTE_ARCHIVE;
        }
    }

    return o_K32_GetFileAttributesW(lpFileName);
}

VALIDATE_HOOK(hk_K32_CreateFileW, Kernel32Proxy::PFN_CreateFileW)
HANDLE WINAPI KernelHooks::hk_K32_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                              LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                              DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (ThreadHasNoTls())
        return o_K32_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
                                 dwFlagsAndAttributes, hTemplateFile);

    if (!State::Instance().nvngxExists && State::Instance().nvngxReplacement.has_value() &&
        (Config::Instance()->DxgiSpoofing.value_or_default() ||
         Config::Instance()->StreamlineSpoofing.value_or_default()))
    {
        auto path = wstring_to_string(std::wstring(lpFileName));
        to_lower_in_place(path);

        // See the note in hk_K32_GetFileAttributesW: exclude our nvngx.dll_dlssnr.dll forwarder.
        if (path.contains("nvngx.dll") && !path.contains("_nvngx.dll") && !path.contains("nvngx.dll_") &&
            !IsInsideWindowsDirectory(path))
        {
            static auto& signedDll = State::Instance().nvngxReplacement;

            if (signedDll.has_value())
            {
                LOG_DEBUG("Overriding CreateFileW for nvngx with a signed dll, original path: {}", path);
                return o_K32_CreateFileW(signedDll.value().c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                         dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            }
        }
    }

    return o_K32_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
                             dwFlagsAndAttributes, hTemplateFile);
}

VALIDATE_HOOK(hk_K32_SetUnhandledExceptionFilter, Kernel32Proxy::PFN_SetUnhandledExceptionFilter)
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI
KernelHooks::hk_K32_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpFilter)
{
    // Logger.cpp keeps its own filter on top and calls the game's after it (the real API is only reached from there)
    return CrashFilterFromGame(lpFilter, o_K32_SetUnhandledExceptionFilter);
}

VALIDATE_HOOK(hk_K32_ExitProcess, Kernel32Proxy::PFN_ExitProcess)
VOID WINAPI KernelHooks::hk_K32_ExitProcess(UINT uExitCode)
{
    // No log drain on a thread without a TLS array: posting the flush marker goes through spdlog's thread_local
    // thread id and would fault. The last queued lines can be lost on that path.
    if (ThreadHasNoTls())
    {
        o_K32_ExitProcess(uExitCode);
        return;
    }

    // Other threads (the log's writer) are stopped by ExitProcess and the loader's detach is skipped for it: this is
    // the last chance for the queued log lines to reach the disk. Bounded.
    DrainLogForExit();
    o_K32_ExitProcess(uExitCode);
}

VALIDATE_HOOK(hk_K32_OutputDebugStringW, Kernel32Proxy::PFN_OutputDebugStringW)
VOID WINAPI KernelHooks::hk_K32_OutputDebugStringW(LPCWSTR lpOutputString)
{
    if (ThreadHasNoTls())
        return o_K32_OutputDebugStringW(lpOutputString);

    o_K32_OutputDebugStringW(lpOutputString);

    std::wstring result(lpOutputString);

    while (!result.empty() && (result.back() == L'\n' || result.back() == L'\r'))
    {
        result.pop_back();
    }

    LOG_TRACE(L"{}", result);
}

VALIDATE_HOOK(hk_K32_OutputDebugStringA, Kernel32Proxy::PFN_OutputDebugStringA)
VOID WINAPI KernelHooks::hk_K32_OutputDebugStringA(LPCSTR lpOutputString)
{
    if (ThreadHasNoTls())
        return o_K32_OutputDebugStringA(lpOutputString);

    o_K32_OutputDebugStringA(lpOutputString);

    std::string result(lpOutputString);

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    {
        result.pop_back();
    }

    LOG_TRACE("{}", result);
}

// Load Library checks

VALIDATE_HOOK(hk_K32_LoadLibraryW, Kernel32Proxy::PFN_LoadLibraryW)
HMODULE KernelHooks::hk_K32_LoadLibraryW(LPCWSTR lpLibFileName)
{
    const DWORD entryError = GetLastError();
    if (NativeLoadOnly(lpLibFileName))
    {
        SetLastError(entryError);
        return o_K32_LoadLibraryW(lpLibFileName);
    }
    const auto original = [&]()
    {
        loader_boundary::OriginalOnlyScope scope;
        SetLastError(entryError);
        return o_K32_LoadLibraryW(lpLibFileName);
    };

    if (loader_boundary::OriginalOnly())
        return original();

    if (ThreadHasNoTls())
    {
        const auto module = original();
        const auto error = GetLastError();
        LibraryLoadHooks::RecordDeferredLoad(module);
        SetLastError(error);
        return module;
    }

    // Retain the historical local null-name return; do not manufacture a new original-call contract here.
    if (lpLibFileName == nullptr)
    {
        SetLastError(entryError);
        return nullptr;
    }

    if (State::Instance().isShuttingDown)
        return original();

    std::wstring name;
    if (!loader_boundary::CopyName(lpLibFileName, name))
        return original();

    bool ownLoadFailed = false;
    const auto handled = CheckLoad(name, &ownLoadFailed);
    if (handled != nullptr)
        return handled;

    const auto result = original();
    const DWORD originalError = GetLastError();
    if (ownLoadFailed && result != nullptr)
        LibraryLoadHooks::HandleCallerModule(name, name.c_str(), result);
    SetLastError(originalError);
    return result;
}

VALIDATE_HOOK(hk_K32_LoadLibraryA, Kernel32Proxy::PFN_LoadLibraryA)
HMODULE KernelHooks::hk_K32_LoadLibraryA(LPCSTR lpLibFileName)
{
    const DWORD entryError = GetLastError();
    if (NativeLoadOnly(lpLibFileName))
    {
        SetLastError(entryError);
        return o_K32_LoadLibraryA(lpLibFileName);
    }
    const auto original = [&]()
    {
        loader_boundary::OriginalOnlyScope scope;
        SetLastError(entryError);
        return o_K32_LoadLibraryA(lpLibFileName);
    };

    if (loader_boundary::OriginalOnly())
        return original();

    if (ThreadHasNoTls())
    {
        const auto module = original();
        const auto error = GetLastError();
        LibraryLoadHooks::RecordDeferredLoad(module);
        SetLastError(error);
        return module;
    }

    // Retain the historical local null-name return; do not manufacture a new original-call contract here.
    if (lpLibFileName == nullptr)
    {
        SetLastError(entryError);
        return nullptr;
    }

    if (State::Instance().isShuttingDown)
        return original();

    std::wstring name;
    try
    {
        name = string_to_wstring(lpLibFileName);
    }
    catch (const std::bad_alloc&)
    {
        return original();
    }

    bool ownLoadFailed = false;
    const auto handled = CheckLoad(name, &ownLoadFailed);
    if (handled != nullptr)
        return handled;

    const auto result = original();
    const DWORD originalError = GetLastError();
    if (ownLoadFailed && result != nullptr)
        LibraryLoadHooks::HandleCallerModule(name, name.c_str(), result);
    SetLastError(originalError);
    return result;
}

VALIDATE_HOOK(hk_K32_LoadLibraryExW, Kernel32Proxy::PFN_LoadLibraryExW)
HMODULE KernelHooks::hk_K32_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    const DWORD entryError = GetLastError();
    if (NativeLoadOnly(lpLibFileName, dwFlags))
    {
        SetLastError(entryError);
        return o_K32_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    }
    const auto original = [&]()
    {
        loader_boundary::OriginalOnlyScope scope;
        SetLastError(entryError);
        return o_K32_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    };

    if (loader_boundary::NonExecutable(dwFlags))
    {
        // Preserve legacy preloaded-image behavior; a known fresh no-init image is vetoed thereafter.
        const auto previous = GetModuleHandleW(lpLibFileName);
        const auto module = original();
        const auto error = GetLastError();
        LibraryLoadHooks::RecordNonExecutable(module, previous);
        SetLastError(error);
        return module;
    }

    if (loader_boundary::OriginalOnly())
        return original();

    if (ThreadHasNoTls())
    {
        const auto module = original();
        const auto error = GetLastError();
        LibraryLoadHooks::RecordDeferredLoad(module);
        SetLastError(error);
        return module;
    }

    // Retain the historical local null-name return; do not manufacture a new original-call contract here.
    if (lpLibFileName == nullptr)
    {
        SetLastError(entryError);
        return nullptr;
    }

    if (State::Instance().isShuttingDown)
        return original();

    std::wstring name;
    if (!loader_boundary::CopyName(lpLibFileName, name))
        return original();

    bool ownLoadFailed = false;
    const auto handled = CheckLoad(name, &ownLoadFailed);
    if (handled != nullptr)
        return handled;

    const auto result = original();
    const DWORD originalError = GetLastError();
    if (ownLoadFailed && result != nullptr)
        LibraryLoadHooks::HandleCallerModule(name, name.c_str(), result);
    SetLastError(originalError);
    return result;
}

VALIDATE_HOOK(hk_K32_LoadLibraryExA, Kernel32Proxy::PFN_LoadLibraryExA)
HMODULE KernelHooks::hk_K32_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    const DWORD entryError = GetLastError();
    if (NativeLoadOnly(lpLibFileName, dwFlags))
    {
        SetLastError(entryError);
        return o_K32_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    }
    const auto original = [&]()
    {
        loader_boundary::OriginalOnlyScope scope;
        SetLastError(entryError);
        return o_K32_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    };

    if (loader_boundary::NonExecutable(dwFlags))
    {
        // Preserve legacy preloaded-image behavior; a known fresh no-init image is vetoed thereafter.
        const auto previous = GetModuleHandleA(lpLibFileName);
        const auto module = original();
        const auto error = GetLastError();
        LibraryLoadHooks::RecordNonExecutable(module, previous);
        SetLastError(error);
        return module;
    }

    if (loader_boundary::OriginalOnly())
        return original();

    if (ThreadHasNoTls())
    {
        const auto module = original();
        const auto error = GetLastError();
        LibraryLoadHooks::RecordDeferredLoad(module);
        SetLastError(error);
        return module;
    }

    // Retain the historical local null-name return; do not manufacture a new original-call contract here.
    if (lpLibFileName == nullptr)
    {
        SetLastError(entryError);
        return nullptr;
    }

    if (State::Instance().isShuttingDown)
        return original();

    std::wstring name;
    try
    {
        name = string_to_wstring(lpLibFileName);
    }
    catch (const std::bad_alloc&)
    {
        return original();
    }

    bool ownLoadFailed = false;
    const auto handled = CheckLoad(name, &ownLoadFailed);
    if (handled != nullptr)
        return handled;

    const auto result = original();
    const DWORD originalError = GetLastError();
    if (ownLoadFailed && result != nullptr)
        LibraryLoadHooks::HandleCallerModule(name, name.c_str(), result);
    SetLastError(originalError);
    return result;
}

VALIDATE_HOOK(hk_KB_LoadLibraryExW, KernelBaseProxy::PFN_LoadLibraryExW)
HMODULE KernelHooks::hk_KB_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    const DWORD entryError = GetLastError();
    if (NativeLoadOnly(lpLibFileName, dwFlags))
    {
        SetLastError(entryError);
        return o_KB_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    }
    const auto original = [&]()
    {
        loader_boundary::OriginalOnlyScope scope;
        SetLastError(entryError);
        return o_KB_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    };

    if (loader_boundary::NonExecutable(dwFlags))
    {
        // Preserve legacy preloaded-image behavior; a known fresh no-init image is vetoed thereafter.
        const auto previous = GetModuleHandleW(lpLibFileName);
        const auto module = original();
        const auto error = GetLastError();
        LibraryLoadHooks::RecordNonExecutable(module, previous);
        SetLastError(error);
        return module;
    }

    if (loader_boundary::OriginalOnly())
        return original();

    if (ThreadHasNoTls())
    {
        const auto module = original();
        const auto error = GetLastError();
        LibraryLoadHooks::RecordDeferredLoad(module);
        SetLastError(error);
        return module;
    }

    // Retain the historical local null-name return; do not manufacture a new original-call contract here.
    if (lpLibFileName == nullptr)
    {
        SetLastError(entryError);
        return nullptr;
    }

    if (State::Instance().isShuttingDown)
        return original();

    std::wstring name;
    if (!loader_boundary::CopyName(lpLibFileName, name))
        return original();

    bool ownLoadFailed = false;
    const auto handled = CheckLoad(name, &ownLoadFailed);
    if (handled != nullptr)
        return handled;

    const auto result = original();
    const DWORD originalError = GetLastError();
    if (ownLoadFailed && result != nullptr)
        LibraryLoadHooks::HandleCallerModule(name, name.c_str(), result);
    SetLastError(originalError);
    return result;
}

VALIDATE_HOOK(hk_K32_FreeLibrary, Kernel32Proxy::PFN_FreeLibrary)
BOOL KernelHooks::hk_K32_FreeLibrary(HMODULE lpLibrary)
{
    if (ThreadHasNoTls())
    {
        // Still keep OptiScaler and the proxies loaded (their detours are live), the bookkeeping in modulesToFree
        // is skipped: it needs State
        if (LibraryLoadHooks::IsKeptLoaded(lpLibrary))
            return TRUE;

        return o_K32_FreeLibrary(lpLibrary);
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
            return result.value() == TRUE;
    }

    return o_K32_FreeLibrary(lpLibrary);
}
