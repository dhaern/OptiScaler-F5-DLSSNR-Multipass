#pragma once
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <nvapi/NvApiTypes.h>

#include "Hook_Utils.h"
#include "low_latency/ll_util.h"
#include <native/XefgReflexRule.h>
#include <low_latency/input/RoutedInputRule.h>

enum TimingType : uint32_t
{
    TimeRange, // in ns, value stored in length
    Simulation,
    RenderSubmit,
    Present,
    Driver,
    OsRenderQueue,
    GpuRender,

    TimingTypeCOUNT
};

class ReflexHooks
{
    inline static bool _inited = false;
    inline static uint32_t _minimumIntervalUs = 0;
    inline static NV_SET_SLEEP_MODE_PARAMS _lastSleepParams {};
    inline static IUnknown* _lastSleepDev = nullptr;
    // Optical F5Low's own Sleep device: only the timing graph reads it (never update()'s fps cap hand-over)
    inline static IUnknown* _ownSleepDev = nullptr;
    inline static uint8_t _FgNumFramesToGenerate = 0;
    inline static uint64_t _lastAsyncMarkerFrameId = 0;
    inline static uint64_t _updatesWithoutMarker = 0;

    inline static uint64_t _lastMarkerFrame = 0;

    inline static NV_VULKAN_SET_SLEEP_MODE_PARAMS _lastVkSleepParams {};
    inline static HANDLE _lastVkSleepDev = nullptr;

    inline static std::thread::id _lastSetSleepThread {};

    // A bridged Vulkan game's own Reflex calls going to XeFG's XeLL (native/XefgReflexRule.h): logged when it changes
    // (noteBridgedForwarding), and whether the NvAPI_Vulkan_* calls have been handed to their new owner since it
    // changed (written under _sleepModeMutex)
    inline static std::atomic<bool> _bridgedForwarding = false;
    inline static std::atomic<bool> _vkNvApiForwarding = false;
    // The sleep mode a game set through VK_NV_low_latency2 (null set: none), for the cap hand-over (_sleepModeMutex)
    inline static native::xefgreflex::SleepMode _lastLl2Mode {};
    inline static bool _lastLl2ModeSet = false;

    // The device the NvAPI_Vulkan_* calls go to XeLL on, else null; the first call after a change gives the game's last
    // sleep mode to its new owner
    static ID3D12Device* vkForwardDevice();
    // The marker bookkeeping every game marker does: the frame IDs XeFG follows (gameXellFrameId) and the menu read
    static void noteMarker(uint64_t frameId, uint32_t markerType, IUnknown* device);
    static NvAPI_Status forwardVkSleepMode(ID3D12Device* xellDevice, HANDLE vkDevice,
                                           const NV_VULKAN_SET_SLEEP_MODE_PARAMS& game);
    // A D3D sleep mode with XeFG as the output: where it goes is native/XefgReflexRule.h RouteFor
    static NvAPI_Status xefgSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params, uint32_t capIntervalUs);

    // XeFG with Reflex or Anti-Lag 2 as the low latency Output (native/XefgReflexRule.h): XeLL (nvapi_calls, XeFG's
    // context) gets every call with its low latency off and never sleeps, and "the other side" gets the real ones: the
    // driver's Reflex (o_NvAPI_*), or fakenvapi's Anti-Lag 2 / Anti-Lag Vk context beside XeLL's (fakenvapi::companion*).
    // The side that lowers latency is passed in (native::lowlatency::XefgSide), asked once per call.
    using Side = native::xefgreflex::XefgLatency;
    static NvAPI_Status sideSetSleepMode(Side side, IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params);
    static NvAPI_Status sideSleep(Side side, IUnknown* pDev);
    static NvAPI_Status sideSetLatencyMarker(Side side, IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* params);
    static NvAPI_Status sideGetLatency(Side side, IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* params);
    static NvAPI_Status sideSetAsyncFrameMarker(Side side, ID3D12CommandQueue* queue,
                                                NV_ASYNC_FRAME_MARKER_PARAMS* params);
    static NvAPI_Status sideSetSleepModeVk(Side side, HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* params);
    static NvAPI_Status sideSetLatencyMarkerVk(Side side, HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* params);
    static NvAPI_Status sideGetLatencyVk(Side side, HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* params);
    // The side's sleep mode goes out with everything off to the device it was last set on: the Output went back to XeLL
    // (or Input Off, Force Disable), and the side must stop sleeping and limiting (_sleepModeMutex)
    static void releaseSide(Side previous);
    // The side lives on one API at a time (the bridge's D3D12 device, or the game's VkDevice): a sleep mode going to one
    // switches off what the other was left with, so a leftover cannot keep sleeping, limiting or answering the timings
    // (_sleepModeMutex)
    static void dropSideOnOtherApi(bool nowVulkan);
    // setFPSLimit sends the side's sleep mode to its D3D and its Vulkan device in one call on purpose: no drop in between
    inline static thread_local bool _keepBothSides = false;
    // The devices the side's last sleep mode went to (null: none), and the side that took it
    inline static IUnknown* _sideDev = nullptr;
    inline static HANDLE _sideVkDev = nullptr;
    inline static Side _sideOfDev = Side::XeLL;
    inline static Side _sideOfVkDev = Side::XeLL;
    // Which of the two the side's sleep mode went to last: the one with the current report (updateTimingData)
    inline static bool _sideLastWasVk = false;

    // The game's own Reflex calls versus OptiScaler's (ownSetSleepMode and friends, Optical F5Low's low latency): ours
    // go through the same wrappers, so they take the same routing, but they are not "the game sends markers".
    inline static thread_local bool _ownCall = false;
    // Set by route*: whether the D3D call InputReflex handed over is the game's (its caller told apart there), and how
    // deep the router is nested on this thread
    inline static thread_local routedinput::RoutedCall _routedCall {};
    inline static std::atomic<bool> _gameCalledReflex = false;
    inline static std::atomic<bool> _timingEstimated = false;
    inline static std::atomic<bool> _gameCalledSetSleepMode = false;
    // SetSleepMode and the stored device: the game's call and ours never interleave, and a released device is dropped
    inline static std::recursive_mutex _sleepModeMutex;

    // D3D
    inline static decltype(&NvAPI_D3D_SetSleepMode) o_NvAPI_D3D_SetSleepMode = nullptr;
    inline static decltype(&NvAPI_D3D_GetSleepStatus) o_NvAPI_D3D_GetSleepStatus = nullptr;
    inline static decltype(&NvAPI_D3D_Sleep) o_NvAPI_D3D_Sleep = nullptr;
    inline static decltype(&NvAPI_D3D_GetLatency) o_NvAPI_D3D_GetLatency = nullptr;
    inline static decltype(&NvAPI_D3D_SetLatencyMarker) o_NvAPI_D3D_SetLatencyMarker = nullptr;
    inline static decltype(&NvAPI_D3D12_SetAsyncFrameMarker) o_NvAPI_D3D12_SetAsyncFrameMarker = nullptr;

    static NvAPI_Status hkNvAPI_D3D_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    static NvAPI_Status hkNvAPI_D3D_Sleep(IUnknown* pDev);
    static NvAPI_Status hkNvAPI_D3D_GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams);
    static NvAPI_Status hkNvAPI_D3D_SetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);
    static NvAPI_Status hkNvAPI_D3D12_SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue,
                                                          NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams);

    // Vulkan
    inline static decltype(&NvAPI_Vulkan_SetLatencyMarker) o_NvAPI_Vulkan_SetLatencyMarker = nullptr;
    inline static decltype(&NvAPI_Vulkan_SetSleepMode) o_NvAPI_Vulkan_SetSleepMode = nullptr;
    inline static decltype(&NvAPI_Vulkan_Sleep) o_NvAPI_Vulkan_Sleep = nullptr;
    inline static decltype(&NvAPI_Vulkan_GetLatency) o_NvAPI_Vulkan_GetLatency = nullptr;
    // Not part of the hooked table (only our own low latency calls it); resolved with the rest, and optional
    inline static decltype(&NvAPI_Vulkan_InitLowLatencyDevice) o_NvAPI_Vulkan_InitLowLatencyDevice = nullptr;

    static NvAPI_Status hkNvAPI_Vulkan_SetLatencyMarker(HANDLE vkDevice,
                                                        NV_VULKAN_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);
    static NvAPI_Status hkNvAPI_Vulkan_SetSleepMode(HANDLE vkDevice,
                                                    NV_VULKAN_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    static NvAPI_Status hkNvAPI_Vulkan_Sleep(HANDLE vkDevice, NvU64 signalValue);
    static NvAPI_Status hkNvAPI_Vulkan_GetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams);

    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_SetSleepMode, decltype(&NvAPI_D3D_SetSleepMode))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_Sleep, decltype(&NvAPI_D3D_Sleep))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_GetLatency, decltype(&NvAPI_D3D_GetLatency))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_SetLatencyMarker, decltype(&NvAPI_D3D_SetLatencyMarker))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D12_SetAsyncFrameMarker, decltype(&NvAPI_D3D12_SetAsyncFrameMarker))
    VALIDATE_MEMBER_HOOK(hkNvAPI_Vulkan_SetLatencyMarker, decltype(&NvAPI_Vulkan_SetLatencyMarker))
    VALIDATE_MEMBER_HOOK(hkNvAPI_Vulkan_SetSleepMode, decltype(&NvAPI_Vulkan_SetSleepMode))
    VALIDATE_MEMBER_HOOK(hkNvAPI_Vulkan_Sleep, decltype(&NvAPI_Vulkan_Sleep))
    VALIDATE_MEMBER_HOOK(hkNvAPI_Vulkan_GetLatency, decltype(&NvAPI_Vulkan_GetLatency))

  public:
    static std::optional<TimingEntry> timingData[TimingType::TimingTypeCOUNT];

    static void hookReflex(PFN_NvApi_QueryInterface& queryInterface);
    static uint8_t dlssgFrameCountToGenerate();
    static void setDlssgFrameCount(uint8_t count);
    static bool isReflexHooked();
    static void* getHookedReflex(unsigned int InterfaceId);
    static bool updateTimingData();

    // The D3D Reflex calls that reach the nvapi hooks enter as a low latency input
    // (low_latency/input/input_reflex.cpp), which tells the game's apart (`game`: not OptiScaler's own Streamline) and
    // hands them here. The routing to the driver, fakenvapi, XeFG's XeLL and the split's other side is the same as
    // before InputCommon knew them.
    static NvAPI_Status routeSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams, bool game);
    static NvAPI_Status routeSleep(IUnknown* pDev, bool game);
    static NvAPI_Status routeGetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pParams);
    static NvAPI_Status routeSetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pParams, bool game);
    static NvAPI_Status routeSetAsyncFrameMarker(ID3D12CommandQueue* pQueue, NV_ASYNC_FRAME_MARKER_PARAMS* pParams,
                                                 bool game);
    // The router is running a call on this thread: a Reflex call that comes in now is made from inside it
    static bool inRoutedCall() { return _routedCall.depth > 0; }
    // The last timings came from fakenvapi (XeFG's XeLL, or fakenvapi as the nvapi): its report has no GPU, driver or
    // queue times, only the markers' own
    static bool timingEstimated() { return _timingEstimated.load(std::memory_order_relaxed); }
    static bool gameIsSendingMarkers();
    // The frame ID the last marker of this type carried (the game's, or ours while it does not call Reflex)
    static uint64_t lastMarkerFrameId(NV_LATENCY_MARKER_TYPE type);
    // While the game's own Reflex feeds XeLL (through fakenvapi with its IDs): the XeLL frame ID of the frame being
    // rendered, the one whose render submit started last (its simulation may be a frame ahead), or its last simulation
    // start's when it sends no render submit markers. 0 when the game sends none.
    static uint32_t gameXellFrameId();

    // Calls OptiScaler makes itself (native/NativeLowLatency.cpp). Call ensureTable first. They run the hooked
    // wrappers, so XeFG's routing and fakenvapi's modes apply as for the game's calls, without counting as the game's.
    // OptiScaler's stored fps cap is never put into them: our SetSleepMode carries its own.
    static bool ensureTable(PFN_NvApi_QueryInterface queryInterface);
    static NvAPI_Status ownSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams);
    static NvAPI_Status ownSleep(IUnknown* pDev);
    static NvAPI_Status ownSetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pParams);
    static NvAPI_Status ownGetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pParams);
    // Same four, for a native Vulkan game (native/NativeDriverVk.cpp): vkDevice is the game's VkDevice, reinterpreted as
    // the HANDLE the Vulkan Reflex entry points take.
    static NvAPI_Status ownSetSleepModeVulkan(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pParams);
    static NvAPI_Status ownSleepVulkan(HANDLE vkDevice, NvU64 signalValue);
    static NvAPI_Status ownSetLatencyMarkerVulkan(HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* pParams);
    static NvAPI_Status ownGetLatencyVulkan(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pParams);
    // NvAPI_Vulkan_InitLowLatencyDevice: initialises the device as a low latency device and returns the timeline
    // semaphore its Sleep signals (the app has to wait for it). NVAPI_NO_IMPLEMENTATION when the interface is missing.
    static NvAPI_Status ownInitLowLatencyDeviceVulkan(HANDLE vkDevice, HANDLE* signalSemaphore);
    // The game calls Reflex through VK_NV_low_latency2 (vkSetLatencySleepModeNV, vkLatencySleepNV, vkSetLatencyMarkerNV):
    // the game's own Reflex, as far as our low latency is concerned (it stands aside). Not for our own calls.
    static void noteGameVulkanLowLatency2(bool setSleepMode);
    // A Vulkan game on the present bridge with XeFG as the output: its own Reflex (NvAPI_Vulkan_*, VK_NV_low_latency2)
    // feeds XeFG's XeLL on the bridge's D3D12 device, and the driver gets the sleep mode with low latency off. Returns
    // the device while that holds, else null (the calls go to the driver as before). Any thread, lock-free, no side
    // effects.
    static ID3D12Device* bridgedXellDevice();
    // The game's own Vulkan Reflex calls go to XeLL now (`forwarding`) or no longer do: said by the game's calls
    // (vkForwardDevice, VK_NV_low_latency2), logged when it changes
    static void noteBridgedForwarding(bool forwarding);
    static bool bridgedReflexForwarding() { return _bridgedForwarding.load(std::memory_order_relaxed); }
    // The game's own Reflex calls reach XeFG's XeLL: forwarded on the bridge, or a D3D game's with XeFG as the output
    // (the D3D hooks). The menu's status and the low latency's log say so.
    static bool gameReflexFeedsXell();
    // The pieces VK_NV_low_latency2 (hooks/Vulkan_Hooks.cpp) shares with the NvAPI_Vulkan_* calls. The mode gets the
    // frame cap in place of its interval when one is set. A marker goes to XeLL when it takes it, and is noted either
    // way.
    static NvAPI_Status xellSetSleepMode(ID3D12Device* xellDevice, const native::xefgreflex::SleepMode& game);
    // Same with the side that lowers latency given: VK_NV_low_latency2 calls are XeLL's (it does the work) when the side
    // is Anti-Lag 2, which has no such surface; only a Reflex side takes them in the split
    static NvAPI_Status xellSetSleepMode(ID3D12Device* xellDevice, const native::xefgreflex::SleepMode& game,
                                         native::xefgreflex::XefgLatency side);
    static NvAPI_Status xellSleep(ID3D12Device* xellDevice);
    // The mode the game set (null: it reset it), so the cap hand-over (setFPSLimit) sends XeLL that one with the cap
    static void noteLowLatency2SleepMode(const native::xefgreflex::SleepMode* mode);
    static void xellMarker(ID3D12Device* xellDevice, uint64_t frameId, uint32_t nvapiMarker);

    // The device is gone, or our Reflex mode is off: the fps cap no longer goes through it (setFPSLimit, update)
    static void forgetSleepDevice(IUnknown* pDev);
    static void forgetSleepDeviceVulkan(HANDLE vkDevice);

    // A Reflex call made by the game: not one of ours, and not from the Streamline OptiScaler loaded for its own DLSS
    // frame generation (`returnAddress`: the hooked function's caller)
    static bool isGameCall(void* returnAddress);

    // The game itself has called Reflex (SetSleepMode, Sleep, a marker or an async marker), ever; and SetSleepMode
    // in particular (a game that only sends markers has Reflex off).
    static bool gameCalledReflex() { return _gameCalledReflex.load(std::memory_order_relaxed); }
    static bool gameCalledSetSleepMode() { return _gameCalledSetSleepMode.load(std::memory_order_relaxed); }

    // For updating information about Reflex hooks
    static void update(bool optiFg_FgState, bool isVulkan);

    // 0 - disables the fps cap
    static void setFPSLimit(float fps);
};
