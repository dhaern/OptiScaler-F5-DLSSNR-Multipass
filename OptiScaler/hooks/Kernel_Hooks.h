#pragma once
#include "SysUtils.h"
#include "Logger.h"

#include <proxies/Kernel32_Proxy.h>
#include <proxies/KernelBase_Proxy.h>

// Enables hooking of GetModuleHandle
// which might create issues, not tested very well
// #define HOOK_GET_MODULE

// #define HOOK_OUTPUT_DEBUG

#ifdef HOOK_GET_MODULE
// Handle nvngx.dll calls on GetModule handle
// #define GET_MODULE_NVNGX

// Handle Opti dll calls on GetModule handle
#define GET_MODULE_DLL
#endif

class KernelHooks
{
  private:
    inline static Kernel32Proxy::PFN_FreeLibrary o_K32_FreeLibrary = nullptr;
    inline static Kernel32Proxy::PFN_LoadLibraryA o_K32_LoadLibraryA = nullptr;
    inline static Kernel32Proxy::PFN_LoadLibraryW o_K32_LoadLibraryW = nullptr;
    inline static Kernel32Proxy::PFN_LoadLibraryExA o_K32_LoadLibraryExA = nullptr;
    inline static Kernel32Proxy::PFN_LoadLibraryExW o_K32_LoadLibraryExW = nullptr;
    inline static Kernel32Proxy::PFN_GetProcAddress o_K32_GetProcAddress = nullptr;
    inline static Kernel32Proxy::PFN_GetModuleHandleA o_K32_GetModuleHandleA = nullptr;
    inline static Kernel32Proxy::PFN_GetModuleHandleW o_K32_GetModuleHandleW = nullptr;
    inline static Kernel32Proxy::PFN_GetModuleHandleExA o_K32_GetModuleHandleExA = nullptr;
    inline static Kernel32Proxy::PFN_GetModuleHandleExW o_K32_GetModuleHandleExW = nullptr;
    inline static Kernel32Proxy::PFN_GetFileAttributesW o_K32_GetFileAttributesW = nullptr;
    inline static Kernel32Proxy::PFN_CreateFileW o_K32_CreateFileW = nullptr;
    inline static Kernel32Proxy::PFN_OutputDebugStringW o_K32_OutputDebugStringW = nullptr;
    inline static Kernel32Proxy::PFN_OutputDebugStringA o_K32_OutputDebugStringA = nullptr;
    inline static Kernel32Proxy::PFN_SetUnhandledExceptionFilter o_K32_SetUnhandledExceptionFilter = nullptr;
    inline static Kernel32Proxy::PFN_ExitProcess o_K32_ExitProcess = nullptr;

    inline static KernelBaseProxy::PFN_FreeLibrary o_KB_FreeLibrary = nullptr;
    inline static KernelBaseProxy::PFN_LoadLibraryA o_KB_LoadLibraryA = nullptr;
    inline static KernelBaseProxy::PFN_LoadLibraryW o_KB_LoadLibraryW = nullptr;
    inline static KernelBaseProxy::PFN_LoadLibraryExA o_KB_LoadLibraryExA = nullptr;
    inline static KernelBaseProxy::PFN_LoadLibraryExW o_KB_LoadLibraryExW = nullptr;
    inline static KernelBaseProxy::PFN_GetProcAddress o_KB_GetProcAddress = nullptr;

    static FARPROC WINAPI hk_K32_GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
    static HMODULE WINAPI hk_K32_GetModuleHandleA(LPCSTR lpModuleName);
    static HMODULE WINAPI hk_K32_GetModuleHandleW(LPCWSTR lpModuleName);
    static BOOL WINAPI hk_K32_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule);
    static BOOL WINAPI hk_K32_GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule);
    // A by-name libxell.dll lookup (XeFG pairing with its XeLL): true and *result set when OptiScaler answers it
    static bool AnswerXellLookup(DWORD dwFlags, HMODULE* phModule, BOOL* result);
    static FARPROC WINAPI hk_KB_GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
    static DWORD WINAPI hk_K32_GetFileAttributesW(LPCWSTR lpFileName);
    static HANDLE WINAPI hk_K32_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                            LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                            DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
    static VOID WINAPI hk_K32_OutputDebugStringW(LPCWSTR lpOutputString);
    static VOID WINAPI hk_K32_OutputDebugStringA(LPCSTR lpOutputString);
    static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hk_K32_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpFilter);
    static VOID WINAPI hk_K32_ExitProcess(UINT uExitCode);

    static HMODULE hk_K32_LoadLibraryW(LPCWSTR lpLibFileName);
    static HMODULE hk_K32_LoadLibraryA(LPCSTR lpLibFileName);
    static HMODULE hk_K32_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
    static HMODULE hk_K32_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
    static HMODULE hk_KB_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
    static BOOL hk_K32_FreeLibrary(HMODULE lpLibrary);

    static inline std::mutex hookMutex32;
    static inline std::mutex hookMutexBase;

  public:
    static constexpr HMODULE amdxc64Mark = HMODULE(0xFFFFFFFF13372137);

    static void Hook()
    {
        std::lock_guard<std::mutex> lock(hookMutex32);

        LOG_DEBUG("");

        if (o_K32_GetProcAddress == nullptr)
            o_K32_GetProcAddress = Kernel32Proxy::Hook_GetProcAddress(hk_K32_GetProcAddress);

        if (o_K32_GetModuleHandleA == nullptr)
            o_K32_GetModuleHandleA = Kernel32Proxy::Hook_GetModuleHandleA(hk_K32_GetModuleHandleA);

        if (o_K32_GetModuleHandleW == nullptr)
            o_K32_GetModuleHandleW = Kernel32Proxy::Hook_GetModuleHandleW(hk_K32_GetModuleHandleW);

        if (o_K32_GetModuleHandleExA == nullptr)
            o_K32_GetModuleHandleExA = Kernel32Proxy::Hook_GetModuleHandleExA(hk_K32_GetModuleHandleExA);

        if (o_K32_GetModuleHandleExW == nullptr)
            o_K32_GetModuleHandleExW = Kernel32Proxy::Hook_GetModuleHandleExW(hk_K32_GetModuleHandleExW);

        if (o_K32_GetFileAttributesW == nullptr)
            o_K32_GetFileAttributesW = Kernel32Proxy::Hook_GetFileAttributesW(hk_K32_GetFileAttributesW);

        if (o_K32_CreateFileW == nullptr)
            o_K32_CreateFileW = Kernel32Proxy::Hook_CreateFileW(hk_K32_CreateFileW);

        // The log's crash filter must stay the top one and its queue must be written before the process exits
        if (o_K32_SetUnhandledExceptionFilter == nullptr)
        {
            o_K32_SetUnhandledExceptionFilter =
                Kernel32Proxy::Hook_SetUnhandledExceptionFilter(hk_K32_SetUnhandledExceptionFilter);

            if (o_K32_SetUnhandledExceptionFilter != nullptr)
                CrashFilterApiHooked(o_K32_SetUnhandledExceptionFilter);
        }

        if (o_K32_ExitProcess == nullptr)
            o_K32_ExitProcess = Kernel32Proxy::Hook_ExitProcess(hk_K32_ExitProcess);

#ifdef HOOK_OUTPUT_DEBUG
        if (o_K32_OutputDebugStringW == nullptr)
            o_K32_OutputDebugStringW = Kernel32Proxy::Hook_OutputDebugStringW(hk_K32_OutputDebugStringW);

        if (o_K32_OutputDebugStringA == nullptr)
            o_K32_OutputDebugStringA = Kernel32Proxy::Hook_OutputDebugStringA(hk_K32_OutputDebugStringA);
#endif

        if (!Config::Instance()->UseNtdllHooks.value_or_default())
        {
            if (o_K32_FreeLibrary == nullptr)
                o_K32_FreeLibrary = Kernel32Proxy::Hook_FreeLibrary(hk_K32_FreeLibrary);
        }

        if (o_K32_LoadLibraryA == nullptr)
            o_K32_LoadLibraryA = Kernel32Proxy::Hook_LoadLibraryA(hk_K32_LoadLibraryA);

        if (o_K32_LoadLibraryW == nullptr)
            o_K32_LoadLibraryW = Kernel32Proxy::Hook_LoadLibraryW(hk_K32_LoadLibraryW);

        if (o_K32_LoadLibraryExA == nullptr)
            o_K32_LoadLibraryExA = Kernel32Proxy::Hook_LoadLibraryExA(hk_K32_LoadLibraryExA);

        // Forwarded exports share one target with KernelBase; do not detour that target twice.
        if (o_K32_LoadLibraryExW == nullptr &&
            Kernel32Proxy::LoadLibraryExW_Hooked() != KernelBaseProxy::LoadLibraryExW_Hooked())
            o_K32_LoadLibraryExW = Kernel32Proxy::Hook_LoadLibraryExW(hk_K32_LoadLibraryExW);
    }

    static void HookBase()
    {
        std::lock_guard<std::mutex> lock(hookMutexBase);

        LOG_DEBUG("");

        if (o_KB_GetProcAddress == nullptr)
            o_KB_GetProcAddress = KernelBaseProxy::Hook_GetProcAddress(hk_KB_GetProcAddress);

        // Win32 loads reach this boundary even with native hooks selected. Its native-mode handling is
        // limited to D3D12 recovery/no-execution classification; other loads still reach the native hooks.
        if (o_KB_LoadLibraryExW == nullptr)
            o_KB_LoadLibraryExW = KernelBaseProxy::Hook_LoadLibraryExW(hk_KB_LoadLibraryExW);
    }
};
