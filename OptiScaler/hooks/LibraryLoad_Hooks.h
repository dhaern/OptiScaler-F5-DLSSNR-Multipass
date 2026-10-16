#pragma once
#include "SysUtils.h"
#include <misc/DeferredModuleScan.h>
#include <misc/LoaderBoundary.h>

class LibraryLoadHooks
{
  private:
    inline static constinit DeferredModuleScan _deferredScan;
    inline static constinit loader_boundary::KnownNoInitImages _noInit;
    inline static constinit std::atomic<HMODULE> _deferredImages[32] {};
    inline static constinit std::atomic_bool _workerClosed { false };
    // ownLoadFailed: set when a branch matched but OptiScaler's own load of the file failed. The caller then lets the
    // original load run and calls this again with its module (loadedModule), so init and hooks still happen.
    static HMODULE HandleLibraryLoad(std::wstring libName, LPCWSTR lpLibFullPath, HMODULE loadedModule,
                                     bool* ownLoadFailed = nullptr);
    static void ScanLoadedModules();
    static void CheckModulesInMemory();
    static bool StartsWithInsensitive(std::wstring_view str, std::wstring_view prefix);

  public:
    // Only ordinary Win32 loads are candidates for new recovery. Native-only marks remain informational.
    static void RecordDeferredLoad(HMODULE module, bool ordinaryWin32 = true);
    static void CatchUpDeferredLoads();

    static HMODULE LoadLibraryCheckA(std::string libName, LPCSTR lpLibFullPath);
    static HMODULE LoadLibraryCheckW(std::wstring libName, LPCWSTR lpLibFullPath, bool* ownLoadFailed = nullptr);

    // Init and hooks for a module the caller loaded itself, after OptiScaler's own load of it failed
    enum class RecoveryResult
    {
        Ready,
        Retry,
        Unsupported,
        NotApplicable
    };
    static RecoveryResult HandleCallerModule(const std::wstring& libName, LPCWSTR lpLibFullPath, HMODULE module);
    static RecoveryResult ObserveOrdinaryModule(HMODULE module);
    static void RecordNonExecutable(HMODULE module, HMODULE previous);
    static bool KnownNonExecutable(HMODULE module);
    static std::optional<NTSTATUS> FreeLibrary(PVOID library);
    // True for the modules FreeLibrary keeps loaded. Reads plain globals only, so it is safe on threads without a
    // static-TLS array (misc/ThreadTls.h), where FreeLibrary itself can't run.
    static bool IsKeptLoaded(PVOID library);

    static HMODULE LoadNvApi();
    // static HMODULE LoadFfxapiVk(std::wstring originalPath);
    // static HMODULE LoadFfxapiDx12(std::wstring originalPath);
    // static HMODULE LoadLibxessDx11(std::wstring originalPath);
    // static HMODULE LoadLibxess(std::wstring originalPath);
    static HMODULE LoadNvngxDlss(std::wstring originalPath);

    static bool IsApiSetName(const std::wstring_view& n);
    static bool EndsWithInsensitive(std::wstring_view text, std::wstring_view suffix);
    static bool EndsWithInsensitive(const UNICODE_STRING& text, std::wstring_view suffix);
};
