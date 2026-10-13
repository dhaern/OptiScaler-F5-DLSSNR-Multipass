#include "pch.h"
#include "Reflex_Hooks.h"
#include <Config.h>
#include <Util.h>

#include <nvapi/fakenvapi.h>

#include <native/VkPresentBridge.h>
#include <native/NativeLowLatency.h>
#include <misc/IdentifyGpu.h>
#include <proxies/Streamline_Proxy.h>

#include <magic_enum.hpp>
#include <nvapi/fakenvapi/nvapi_calls.h>

#include <math.h>
#include <imgui/ImGuiNotify.hpp>

#include <intrin.h>
#include <unordered_map>

// Written on the marker's thread, read by XeFG on the threads that tag its frames
static inline std::atomic<uint64_t> _lastFrameId[20] = {};
// Not a reference: dropped by forgetSleepDevice when the device goes
static inline std::atomic<IUnknown*> _lastDev[20] = {};

// #define LOG_REFLEX_CALLS

std::optional<TimingEntry> ReflexHooks::timingData[TimingType::TimingTypeCOUNT] {};

void ReflexHooks::noteMarker(uint64_t frameId, uint32_t markerType, IUnknown* device)
{
    // TODO: reflexFrameId gets constantly changed, up and down depending on the marker
    State::Instance().reflexFrameId = frameId;

    // if (markerType == PRESENT_END)
    if (markerType < std::size(_lastFrameId))
    {
        _lastFrameId[markerType] = frameId;
        _lastDev[markerType] = device;
    }

    // Our own markers (Optical F5Low's low latency) are not the game sending markers
    if (markerType == SIMULATION_START && !_ownCall)
        _lastMarkerFrame = State::Instance().fgLastFrame;
}

ID3D12Device* ReflexHooks::bridgedXellDevice()
{
    const bool xefgOutput = State::Instance().activeFgOutput == FGOutput::XeFG;

    // Lock-free (XeFG's present thread asks while the bridge's teardown holds its lock and waits for that thread)
    ID3D12Device* device = xefgOutput ? VkPresentBridge::Device() : nullptr;
    const bool dxvkWithoutVulkanBridge = VkPresentBridge::DxvkGame() && !VkPresentBridge::DxvkThroughVulkan();
    const bool forward =
        native::xefgreflex::ShouldForward(xefgOutput, device != nullptr, dxvkWithoutVulkanBridge);

    return forward ? device : nullptr;
}

void ReflexHooks::noteBridgedForwarding(bool forwarding)
{
    if (_bridgedForwarding.exchange(forwarding) != forwarding)
    {
        if (forwarding)
            LOG_INFO("The game's Vulkan Reflex now feeds XeFG's XeLL on the bridge's D3D12 device");
        else
            LOG_INFO("The game's Vulkan Reflex no longer feeds XeFG's XeLL");
    }
}

bool ReflexHooks::gameReflexFeedsXell()
{
    return bridgedReflexForwarding() || (State::Instance().activeFgOutput == FGOutput::XeFG && gameCalledReflex());
}

namespace
{
using Side = native::xefgreflex::XefgLatency;
using native::xefgreflex::Call;
using native::xefgreflex::RouteFor;

native::xefgreflex::SleepMode ToMode(const NV_VULKAN_SET_SLEEP_MODE_PARAMS& params)
{
    native::xefgreflex::SleepMode mode;
    mode.lowLatency = params.bLowLatencyMode != 0;
    mode.boost = params.bLowLatencyBoost != 0;
    mode.minimumIntervalUs = params.minimumIntervalUs;
    return mode;
}

native::xefgreflex::SleepMode ToMode(const NV_SET_SLEEP_MODE_PARAMS& params)
{
    native::xefgreflex::SleepMode mode;
    mode.lowLatency = params.bLowLatencyMode != 0;
    mode.boost = params.bLowLatencyBoost != 0;
    mode.minimumIntervalUs = params.minimumIntervalUs;
    mode.useMarkersToOptimize = params.bUseMarkersToOptimize != 0;
    return mode;
}

// `params` with the mode's fields in it (the rest of the call is kept as it was)
NV_SET_SLEEP_MODE_PARAMS WithMode(const NV_SET_SLEEP_MODE_PARAMS& params, const native::xefgreflex::SleepMode& mode)
{
    NV_SET_SLEEP_MODE_PARAMS out = params;
    out.bLowLatencyMode = mode.lowLatency;
    out.bLowLatencyBoost = mode.boost;
    out.minimumIntervalUs = mode.minimumIntervalUs;
    out.bUseMarkersToOptimize = mode.useMarkersToOptimize;
    return out;
}

NV_VULKAN_SET_SLEEP_MODE_PARAMS WithMode(const NV_VULKAN_SET_SLEEP_MODE_PARAMS& params,
                                         const native::xefgreflex::SleepMode& mode)
{
    NV_VULKAN_SET_SLEEP_MODE_PARAMS out = params;
    out.bLowLatencyMode = mode.lowLatency;
    out.bLowLatencyBoost = mode.boost;
    out.minimumIntervalUs = mode.minimumIntervalUs;
    return out;
}

bool ForceDisabled() { return Config::Instance()->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable; }

bool XefgOutput() { return State::Instance().activeFgOutput == FGOutput::XeFG; }

// The Framerate page's Input is Off: the game's sleep mode goes out with low latency off (XefgReflexRule.h)
bool Blocked() { return native::lowlatency::GameLowLatencyBlocked(); }

// What lowers latency with XeFG right now: XeLL, or the split (Reflex / Anti-Lag 2 do it, XeLL only keeps XeFG running).
// Always XeLL when XeFG is not the output.
native::xefgreflex::XefgLatency CurrentSide() { return native::lowlatency::XefgSide(); }

bool Split() { return CurrentSide() != native::xefgreflex::XefgLatency::XeLL; }

// XefgReflexRule.h XellHoldsCap, from frame generation's own state: update()'s fgActive is false on every generated
// frame's present, and the answer must not flip with it. Never in the split: XeLL does not hold the cap there.
bool XellHoldsCap()
{
    static const bool intelGpu = IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Intel;
    return native::xefgreflex::XellHoldsCap(ForceDisabled() || Blocked() || Split(), intelGpu,
                                            native::lowlatency::XefgGenerating());
}

// A D3D game's sleep mode as XeLL (or the plain Reflex output without XeFG) gets it: XefgReflexRule.h SleepModeForOutput
// with the cap in place of the game's interval when `capIntervalUs` is set. `side`: who lowers latency with XeFG; when
// the other side takes the mode (RouteFor), XeLL's has low latency off.
NV_SET_SLEEP_MODE_PARAMS ForOutput(const NV_SET_SLEEP_MODE_PARAMS& params, Side side = Side::XeLL,
                                   uint32_t capIntervalUs = 0)
{
    auto mode = native::xefgreflex::SleepModeForOutput(ToMode(params), capIntervalUs, XefgOutput(), ForceDisabled(),
                                                       Blocked());

    // XeLL in the split: low latency off, it neither sleeps nor holds the cap
    if (RouteFor(side, Call::SetSleepMode).driver)
        mode = native::xefgreflex::XellSleepModeInSplit(mode);

    return WithMode(params, mode);
}

// What the game gets back from a call that went to both XeLL and the other side: the other side's answer, except that an
// Anti-Lag 2 that could not start must not hand its error to the game while XeFG is still falling back to XeLL
// (ReflexHooks::update, native::lowlatency::XefgSide)
NvAPI_Status ResultFor(Side side, NvAPI_Status xell, NvAPI_Status other)
{
    return side == Side::AntiLag2 && other != NVAPI_OK ? xell : other;
}

// A Vulkan game's sleep mode (cap already in) with the Input's Off applied; nothing else changes
void ForOutput(NV_VULKAN_SET_SLEEP_MODE_PARAMS& params)
{
    if (!Blocked())
        return;

    const auto mode = native::xefgreflex::SleepModeForOutput(ToMode(params), 0, false, false, true);
    params.bLowLatencyMode = mode.lowLatency;
    params.bLowLatencyBoost = mode.boost;
    params.minimumIntervalUs = mode.minimumIntervalUs;
}
} // namespace

NvAPI_Status ReflexHooks::xellSetSleepMode(ID3D12Device* xellDevice, const native::xefgreflex::SleepMode& game)
{
    return xellSetSleepMode(xellDevice, game, CurrentSide());
}

NvAPI_Status ReflexHooks::xellSetSleepMode(ID3D12Device* xellDevice, const native::xefgreflex::SleepMode& game, Side side)
{
    auto mode = native::xefgreflex::SleepModeForOutput(game, _minimumIntervalUs, true, ForceDisabled(), Blocked());

    // XeLL in the split: low latency off, it neither sleeps nor holds the cap
    if (side != Side::XeLL)
        mode = native::xefgreflex::XellSleepModeInSplit(mode);

    NV_SET_SLEEP_MODE_PARAMS params {};
    params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
    params.bLowLatencyMode = mode.lowLatency;
    params.bLowLatencyBoost = mode.boost;
    params.minimumIntervalUs = mode.minimumIntervalUs;
    params.bUseMarkersToOptimize = mode.useMarkersToOptimize;

    const auto result = nvapi_calls::NvAPI_D3D_SetSleepMode(xellDevice, &params);

    static bool logged = false;
    if (result != NVAPI_OK && !logged)
    {
        logged = true;
        LOG_WARN("XeLL refused the game's sleep mode: {}", magic_enum::enum_name(result));
    }

    return result;
}

void ReflexHooks::noteLowLatency2SleepMode(const native::xefgreflex::SleepMode* mode)
{
    std::scoped_lock lock(_sleepModeMutex);

    _lastLl2ModeSet = mode != nullptr;
    _lastLl2Mode = mode != nullptr ? *mode : native::xefgreflex::SleepMode {};
}

NvAPI_Status ReflexHooks::xellSleep(ID3D12Device* xellDevice)
{
    const auto result = nvapi_calls::NvAPI_D3D_Sleep(xellDevice);

    static bool logged = false;
    if (result != NVAPI_OK && !logged)
    {
        logged = true;
        LOG_WARN("XeLL Sleep failed: {}", magic_enum::enum_name(result));
    }

    return result;
}

void ReflexHooks::xellMarker(ID3D12Device* xellDevice, uint64_t frameId, uint32_t nvapiMarker)
{
    _updatesWithoutMarker = 0;
    noteMarker(frameId, nvapiMarker, nullptr);

    if (!native::xefgreflex::XellTakesMarker(nvapiMarker))
        return;

    NV_LATENCY_MARKER_PARAMS params {};
    params.version = NV_LATENCY_MARKER_PARAMS_VER;
    params.frameID = frameId;
    params.markerType = (NV_LATENCY_MARKER_TYPE) nvapiMarker;
    nvapi_calls::NvAPI_D3D_SetLatencyMarker(xellDevice, &params);
}

// The other side of the split (native/XefgReflexRule.h): the driver's Reflex for Reflex, fakenvapi's second context for
// Anti-Lag 2. Every sleep mode that goes to a side is noted, so releaseSide can switch it off again.
NvAPI_Status ReflexHooks::sideSetSleepMode(Side side, IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params)
{
    dropSideOnOtherApi(false);

    _sideDev = pDev;
    _sideOfDev = side;
    _sideLastWasVk = false;

    const auto result = side == Side::AntiLag2 ? fakenvapi::companionSetSleepMode(pDev, params)
                                               : o_NvAPI_D3D_SetSleepMode(pDev, params);

    // What the side was asked for, once per change: the frame cap it keeps is the interval
    static NV_SET_SLEEP_MODE_PARAMS logged {};
    static IUnknown* loggedDev = nullptr;
    if (loggedDev != pDev || logged.bLowLatencyMode != params->bLowLatencyMode ||
        logged.bLowLatencyBoost != params->bLowLatencyBoost || logged.minimumIntervalUs != params->minimumIntervalUs)
    {
        logged = *params;
        loggedDev = pDev;
        LOG_INFO("XeFG low latency: {} sleep mode on {:X}: low latency {}, boost {}, interval {} us ({}): {}",
                 side == Side::AntiLag2 ? "Anti-Lag 2" : "NVIDIA Reflex", (uintptr_t) pDev, params->bLowLatencyMode != 0,
                 params->bLowLatencyBoost != 0, params->minimumIntervalUs,
                 params->minimumIntervalUs > 0 ? std::format("{:.1f} fps", 1e6 / params->minimumIntervalUs)
                                               : std::string("no cap"),
                 magic_enum::enum_name(result));
    }

    return result;
}

NvAPI_Status ReflexHooks::sideSleep(Side side, IUnknown* pDev)
{
    if (side == Side::AntiLag2)
        return fakenvapi::companionSleep(pDev);

    return o_NvAPI_D3D_Sleep(pDev);
}

NvAPI_Status ReflexHooks::sideSetLatencyMarker(Side side, IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* params)
{
    if (side == Side::AntiLag2)
        return fakenvapi::companionSetLatencyMarker(pDev, params);

    return o_NvAPI_D3D_SetLatencyMarker(pDev, params);
}

NvAPI_Status ReflexHooks::sideGetLatency(Side side, IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* params)
{
    if (side == Side::AntiLag2)
        return fakenvapi::companionGetLatency(pDev, params);

    return o_NvAPI_D3D_GetLatency(pDev, params);
}

NvAPI_Status ReflexHooks::sideSetAsyncFrameMarker(Side side, ID3D12CommandQueue* queue,
                                                  NV_ASYNC_FRAME_MARKER_PARAMS* params)
{
    if (side == Side::AntiLag2)
        return fakenvapi::companionSetAsyncFrameMarker(queue, params);

    return o_NvAPI_D3D12_SetAsyncFrameMarker(queue, params);
}

NvAPI_Status ReflexHooks::sideSetSleepModeVk(Side side, HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* params)
{
    dropSideOnOtherApi(true);

    _sideVkDev = vkDevice;
    _sideOfVkDev = side;
    _sideLastWasVk = true;

    const auto result = side == Side::AntiLag2 ? fakenvapi::companionSetSleepMode(vkDevice, params)
                                               : o_NvAPI_Vulkan_SetSleepMode(vkDevice, params);

    // As the D3D one: what the side was asked for, once per change, and what it answered
    static NV_VULKAN_SET_SLEEP_MODE_PARAMS logged {};
    static HANDLE loggedDev = nullptr;
    if (loggedDev != vkDevice || logged.bLowLatencyMode != params->bLowLatencyMode ||
        logged.bLowLatencyBoost != params->bLowLatencyBoost || logged.minimumIntervalUs != params->minimumIntervalUs)
    {
        logged = *params;
        loggedDev = vkDevice;
        LOG_INFO("XeFG low latency (Vulkan): {} sleep mode on {:X}: low latency {}, boost {}, interval {} us ({}): {}",
                 side == Side::AntiLag2 ? "Anti-Lag Vk" : "NVIDIA Reflex", (uintptr_t) vkDevice,
                 params->bLowLatencyMode != 0, params->bLowLatencyBoost != 0, params->minimumIntervalUs,
                 params->minimumIntervalUs > 0 ? std::format("{:.1f} fps", 1e6 / params->minimumIntervalUs)
                                               : std::string("no cap"),
                 magic_enum::enum_name(result));
    }

    return result;
}

NvAPI_Status ReflexHooks::sideSetLatencyMarkerVk(Side side, HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* params)
{
    if (side == Side::AntiLag2)
        return fakenvapi::companionSetLatencyMarker(vkDevice, params);

    return o_NvAPI_Vulkan_SetLatencyMarker(vkDevice, params);
}

NvAPI_Status ReflexHooks::sideGetLatencyVk(Side side, HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* params)
{
    if (side == Side::AntiLag2)
        return fakenvapi::companionGetLatency(vkDevice, params);

    return o_NvAPI_Vulkan_GetLatency(vkDevice, params);
}

// A sleep mode now goes to one API (`nowVulkan`): the side's last one on the other API gets everything off and is
// forgotten. Not recursive: the "off" itself is a sleep mode on the other API. Not inside setFPSLimit, which sends the
// cap to both the D3D and the Vulkan device in one call (_keepBothSides): the second would turn the first off.
void ReflexHooks::dropSideOnOtherApi(bool nowVulkan)
{
    static thread_local bool dropping = false;

    if (dropping || _keepBothSides)
        return;

    std::scoped_lock lock(_sleepModeMutex);
    dropping = true;

    if (nowVulkan && _sideDev != nullptr)
    {
        NV_SET_SLEEP_MODE_PARAMS off {};
        off.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        auto* device = _sideDev;
        sideSetSleepMode(_sideOfDev, device, &off);
        _sideDev = nullptr;
    }
    else if (!nowVulkan && _sideVkDev != nullptr)
    {
        NV_VULKAN_SET_SLEEP_MODE_PARAMS off {};
        off.version = NV_VULKAN_SET_SLEEP_MODE_PARAMS_VER;
        auto* device = _sideVkDev;
        sideSetSleepModeVk(_sideOfVkDev, device, &off);
        _sideVkDev = nullptr;
    }

    dropping = false;
}

// The Output went back to XeLL (or Input Off, Force Disable switched the split off): the side that had the sleep mode gets
// one with everything off, or it would go on sleeping and limiting next to XeLL
void ReflexHooks::releaseSide(Side previous)
{
    std::scoped_lock lock(_sleepModeMutex);

    if (previous == Side::XeLL)
        return;

    if (_sideDev != nullptr && _sideOfDev == previous)
    {
        NV_SET_SLEEP_MODE_PARAMS off {};
        off.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        sideSetSleepMode(previous, _sideDev, &off);
        _sideDev = nullptr;
    }

    if (_sideVkDev != nullptr && _sideOfVkDev == previous)
    {
        NV_VULKAN_SET_SLEEP_MODE_PARAMS off {};
        off.version = NV_VULKAN_SET_SLEEP_MODE_PARAMS_VER;
        sideSetSleepModeVk(previous, _sideVkDev, &off);
        _sideVkDev = nullptr;
    }

    // Anti-Lag 2 does not stay initialised beside XeLL once it has been told to stop
    if (previous == Side::AntiLag2)
        fakenvapi::companionRelease();
}

ID3D12Device* ReflexHooks::vkForwardDevice()
{
    auto* device = bridgedXellDevice();
    noteBridgedForwarding(device != nullptr);

    if ((device != nullptr) == _vkNvApiForwarding.load(std::memory_order_acquire))
        return device;

    std::scoped_lock lock(_sleepModeMutex);

    if ((device != nullptr) == _vkNvApiForwarding.load(std::memory_order_relaxed))
        return device;

    _vkNvApiForwarding = device != nullptr;

    // The game set its sleep mode to the other side: its new owner gets it (the old one keeps what it had). The driver
    // gets the cap back with it, as setFPSLimit gives it while the driver is the owner.
    if (_lastVkSleepDev != nullptr)
    {
        if (device != nullptr)
        {
            forwardVkSleepMode(device, _lastVkSleepDev, _lastVkSleepParams);
        }
        else
        {
            NV_VULKAN_SET_SLEEP_MODE_PARAMS params = _lastVkSleepParams;

            if (_minimumIntervalUs != 0)
                params.minimumIntervalUs = _minimumIntervalUs;

            ForOutput(params);
            o_NvAPI_Vulkan_SetSleepMode(_lastVkSleepDev, &params);
        }
    }

    return device;
}

// XeLL gets the game's mode, the driver the same call with low latency off (it still has to run its Sleep). In the split
// it is the other way round: XeLL gets it with low latency off, the driver (or Anti-Lag Vk) the game's own, with the cap.
NvAPI_Status ReflexHooks::forwardVkSleepMode(ID3D12Device* xellDevice, HANDLE vkDevice,
                                             const NV_VULKAN_SET_SLEEP_MODE_PARAMS& game)
{
    const auto side = CurrentSide();
    const auto route = RouteFor(side, Call::SetSleepMode);

    if (route.xell)
        xellSetSleepMode(xellDevice, ToMode(game), side);

    // The other side gets the game's own mode with the cap. Without one the driver is still called (its Sleep has to
    // signal the game's semaphore), with everything off. (RouteFor's "driver" is the other side; the Vulkan driver gets
    // every call anyway.)
    if (route.driver)
    {
        auto real = WithMode(game, native::xefgreflex::DriverSleepModeInSplit(ToMode(game), _minimumIntervalUs));
        return ResultFor(side, NVAPI_OK, sideSetSleepModeVk(side, vkDevice, &real));
    }

    auto driver = WithMode(game, native::xefgreflex::kDriverSleepMode);
    return o_NvAPI_Vulkan_SetSleepMode(vkDevice, &driver);
}

// A D3D sleep mode with XeFG as the output (the game's, or ours with `capIntervalUs` 0: ours carries its cap): XeLL gets
// it shaped for the side in force, the other side the game's own with the cap (native/XefgReflexRule.h RouteFor)
NvAPI_Status ReflexHooks::xefgSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* params, uint32_t capIntervalUs)
{
    const auto side = CurrentSide();
    const auto route = RouteFor(side, Call::SetSleepMode);

    NvAPI_Status xellResult = NVAPI_OK;

    if (route.xell)
    {
        auto forXefg = ForOutput(*params, side, capIntervalUs);
        xellResult = nvapi_calls::NvAPI_D3D_SetSleepMode(pDev, &forXefg);
    }

    if (!route.driver)
        return xellResult;

    auto real = WithMode(*params, native::xefgreflex::DriverSleepModeInSplit(ToMode(*params), capIntervalUs));
    return ResultFor(side, xellResult, sideSetSleepMode(side, pDev, &real));
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif
    std::scoped_lock lock(_sleepModeMutex);

    if (_routedCall.game && !_ownCall)
    {
        _gameCalledReflex = true;
        _gameCalledSetSleepMode = true;
    }

    // Our own calls (Optical F5Low's low latency) are not stored: update() would hand the cap to Reflex
    // (reflexLimitsFps, setFPSLimit) with no halving for frame generation. They carry the cap themselves when the low
    // latency keeps it (native/NativeLowLatencyRule.h PlanCap) and _minimumIntervalUs is never put into them.
    // With XeFG they take SleepModeForOutput too: our "off" (stopped, switched off) leaves XeLL's low latency on, as
    // XeFG wants it; with no one marking frames or sleeping any more, XeLL then waits for nothing.
    if (_ownCall)
    {
        // The split: XeLL has it with low latency off, the other side ours as it is (it carries the cap)
        if (XefgOutput())
            return xefgSetSleepMode(pDev, pSetSleepModeParams, 0);

        return o_NvAPI_D3D_SetSleepMode(pDev, pSetSleepModeParams);
    }

    // Store for later so we can adjust the fps whenever we want
    memcpy(&_lastSleepParams, pSetSleepModeParams, sizeof(NV_SET_SLEEP_MODE_PARAMS));
    _lastSleepDev = pDev;

    if (State::Instance().gameQuirks & GameQuirk::HitmanReflexHacks)
        _lastSetSleepThread = std::this_thread::get_id();

    // XeFG: XeLL has the game's mode, and in the split the other side too (the cap in place of the game's interval)
    if (XefgOutput())
        return xefgSetSleepMode(pDev, pSetSleepModeParams, _minimumIntervalUs);

    if (_minimumIntervalUs != 0)
        pSetSleepModeParams->minimumIntervalUs = _minimumIntervalUs;

    // The Framerate page's Input is Off: real nvapi, or fakenvapi as the game's nvapi, gets the mode with low latency off
    if (Blocked())
    {
        auto off = ForOutput(*pSetSleepModeParams);
        return o_NvAPI_D3D_SetSleepMode(pDev, &off);
    }

    return o_NvAPI_D3D_SetSleepMode(pDev, pSetSleepModeParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_Sleep(IUnknown* pDev)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    if (_routedCall.game && !_ownCall)
        _gameCalledReflex = true;

    static bool skip = false;
    if (State::Instance().activeFgOutput == FGOutput::DLSSG &&
        Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default() && State::Instance().currentFG &&
        State::Instance().currentFG->IsActive() && !State::Instance().currentFG->IsPaused())
    {
        if (!skip)
        {
            uint32_t frameCount = (uint32_t) _lastFrameId[SIMULATION_START] + 1;

            sl::FrameToken* frameToken;
            StreamlineProxy::GetNewFrameToken()(frameToken, &frameCount);

            LOG_TRACE("Sleep for frame {}", frameCount);

            skip = true;
            StreamlineProxy::ReflexSleep()(*frameToken);
            skip = false;

            return NVAPI_OK;
        }
        else
        {
            _lastSleepDev = pDev;
            return o_NvAPI_D3D_Sleep(pDev);
        }
    }

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        // Our own Sleep with XeFG: noted for the menu's Reflex timing graph only (updateTimingData), as below. With XeLL as
        // the Output it is the one device whose report has the timings: a game with no Reflex of its own (dxvk on the
        // Vulkan bridge) gives the graph nothing else, and it stayed empty.
        if (_ownCall)
            _ownSleepDev = pDev;

        // The split: XeLL never sleeps (its low latency is off), the other side does
        const auto side = CurrentSide();
        const auto route = RouteFor(side, Call::Sleep);

        if (route.driver)
            return ResultFor(side, NVAPI_OK, sideSleep(side, pDev));

        return nvapi_calls::NvAPI_D3D_Sleep(pDev);
    }

    // Our own Sleep (Optical F5Low's low latency) is not a Reflex device of the game's: stored, update() would see one
    // and hand the fps cap to Reflex (reflexLimitsFps) with the game's sleep parameters, which were never set (version
    // 0, low latency off), skipping OptiScaler's own limiter. Our SetSleepMode carries the cap itself
    // (native/NativeLowLatency.cpp).
    // The menu's Reflex timing graph still reads ours (updateTimingData).
    if (_ownCall)
        _ownSleepDev = pDev;
    else
        _lastSleepDev = pDev;

    return o_NvAPI_D3D_Sleep(pDev);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        // The split: the report is the other side's (the driver has the real GPU times); an Anti-Lag 2 that could not
        // start has none, XeLL's goes to the game then
        const auto side = CurrentSide();

        if (RouteFor(side, Call::GetLatency).driver)
        {
            const auto result = sideGetLatency(side, pDev, pGetLatencyParams);

            if (result == NVAPI_OK || side != Side::AntiLag2)
                return result;
        }

        return nvapi_calls::NvAPI_D3D_GetLatency(pDev, pGetLatencyParams);
    }

    return o_NvAPI_D3D_GetLatency(pDev, pGetLatencyParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_SetLatencyMarker(IUnknown* pDev,
                                                       NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    _updatesWithoutMarker = 0;

    if (_routedCall.game && !_ownCall)
        _gameCalledReflex = true;

    // LOG_DEBUG("frameID: {}, markerType: {}", pSetLatencyMarkerParams->frameID,
    //           magic_enum::enum_name(pSetLatencyMarkerParams->markerType));

    // Some games just stop sending any async markers when DLSSG is disabled, so a reset is needed
    if (_lastAsyncMarkerFrameId + 10 < pSetLatencyMarkerParams->frameID)
    {
        _FgNumFramesToGenerate = 0;
    }

    // RTSS reflex markers have a frameid marker in the high bits
    if (pSetLatencyMarkerParams->frameID >> 32)
    {
        if (!State::Instance().rtssReflexInjection && State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            ImGuiToast notification { ImGuiToastType::Warning, 10000 };
            notification.setTitle("RTSS + XeFG detected");
            notification.setContent(
                "RTSS Reflex Injection is known to cause issues.\nEspecially when using XeFG.\nPlease disable it.");
            ImGui::InsertNotification(notification);
        }

        State::Instance().rtssReflexInjection = true;
    }

    noteMarker(pSetLatencyMarkerParams->frameID, pSetLatencyMarkerParams->markerType, pDev);

    static bool skip[20] = {};

    if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
        Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default() && State::Instance().currentFG &&
        State::Instance().currentFG->IsActive() && !State::Instance().currentFG->IsPaused())
    {
        sl::PCLMarker marker {};
        bool noMarker = false;

        switch (pSetLatencyMarkerParams->markerType)
        {
        case SIMULATION_START:
            marker = sl::PCLMarker::eSimulationStart;
            break;

        case SIMULATION_END:
            marker = sl::PCLMarker::eSimulationEnd;
            break;

        case RENDERSUBMIT_START:
            marker = sl::PCLMarker::eRenderSubmitStart;
            break;

        case RENDERSUBMIT_END:
            marker = sl::PCLMarker::eRenderSubmitEnd;
            break;

        case PRESENT_START:
            marker = sl::PCLMarker::ePresentStart;
            break;

        case PRESENT_END:
            marker = sl::PCLMarker::ePresentEnd;
            break;

        case INPUT_SAMPLE:
            marker = sl::PCLMarker::eControllerInputSample;
            break;

        case TRIGGER_FLASH:
            marker = sl::PCLMarker::eTriggerFlash;
            break;

        case PC_LATENCY_PING:
            marker = sl::PCLMarker::eDeltaTCalculation;
            break;

        case OUT_OF_BAND_RENDERSUBMIT_START:
            marker = sl::PCLMarker::eOutOfBandRenderSubmitStart;
            break;

        case OUT_OF_BAND_RENDERSUBMIT_END:
            marker = sl::PCLMarker::eOutOfBandRenderSubmitEnd;
            break;

        case OUT_OF_BAND_PRESENT_START:
            marker = sl::PCLMarker::eOutOfBandPresentStart;
            break;

        case OUT_OF_BAND_PRESENT_END:
            marker = sl::PCLMarker::eOutOfBandPresentEnd;
            break;

        default:
            noMarker = true;
        }

        auto index = (UINT) marker;

        if (!noMarker && !skip[index])
        {
            if (pSetLatencyMarkerParams->markerType == PRESENT_START && State::Instance().currentFG != nullptr)
            {
                auto frameCount = State::Instance().currentFG->FrameCount();
                if (pSetLatencyMarkerParams->frameID - frameCount > 1)
                    LOG_WARN("FrameId Moved too much??? {} -> {}", frameCount, pSetLatencyMarkerParams->frameID);

                if (pSetLatencyMarkerParams->frameID != frameCount)
                    State::Instance().currentFG->SetFrameCount(pSetLatencyMarkerParams->frameID);

                State::Instance().reflexFrameId = pSetLatencyMarkerParams->frameID;
            }

            sl::FrameToken* frameToken;
            uint32_t frameCount = (uint32_t) pSetLatencyMarkerParams->frameID;
            StreamlineProxy::GetNewFrameToken()(frameToken, &frameCount);

            LOG_TRACE("{} for frame {}", magic_enum::enum_name(marker), frameCount);

            skip[index] = true;
            StreamlineProxy::PCLSetMarker()(marker, *frameToken);
            skip[index] = false;

            return NvAPI_Status::NVAPI_OK;
        }
        else
        {
            if (noMarker)
                return NVAPI_OK;
            else
                return o_NvAPI_D3D_SetLatencyMarker(pDev, pSetLatencyMarkerParams);
        }
    }

    if (State::Instance().gameQuirks & GameQuirk::HitmanReflexHacks)
    {
        if ((pSetLatencyMarkerParams->markerType != PRESENT_START &&
             pSetLatencyMarkerParams->markerType != PRESENT_END) &&
            _lastSetSleepThread != std::this_thread::get_id())
        {
            LOG_TRACE("Skipping marker for a hack");
            return NVAPI_OK;
        }

        if (pSetLatencyMarkerParams->markerType == SIMULATION_END)
        {
            NV_LATENCY_MARKER_PARAMS newParams = *pSetLatencyMarkerParams;
            auto previousThread = _lastSetSleepThread;
            _lastSetSleepThread = std::this_thread::get_id();

            newParams.markerType = RENDERSUBMIT_START;
            hkNvAPI_D3D_SetLatencyMarker(pDev, &newParams);

            newParams.markerType = RENDERSUBMIT_END;
            hkNvAPI_D3D_SetLatencyMarker(pDev, &newParams);

            _lastSetSleepThread = previousThread;
        }
    }

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        // The split: XeLL has every marker (its low latency is off, it drops them but XeFG's frame IDs follow them), the
        // other side too
        const auto side = CurrentSide();
        const auto route = RouteFor(side, Call::Marker);
        NvAPI_Status xellResult = NVAPI_OK;

        if (route.xell)
            xellResult = nvapi_calls::NvAPI_D3D_SetLatencyMarker(pDev, pSetLatencyMarkerParams);

        if (route.driver)
            return ResultFor(side, xellResult, sideSetLatencyMarker(side, pDev, pSetLatencyMarkerParams));

        return xellResult;
    }

    return o_NvAPI_D3D_SetLatencyMarker(pDev, pSetLatencyMarkerParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D12_SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue,
                                                            NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    if (_routedCall.game && !_ownCall)
        _gameCalledReflex = true;

    _lastAsyncMarkerFrameId = pSetAsyncFrameMarkerParams->frameID;

    // if (pSetAsyncFrameMarkerParams->markerType == OUT_OF_BAND_PRESENT_START)
    //{
    //     constexpr size_t history_size = 20;
    //     static size_t counter = 0;
    //     static NvU64 previous_frame_ids[history_size] = {};

    //    previous_frame_ids[counter % history_size] = pSetAsyncFrameMarkerParams->frameID;
    //    counter++;

    //    size_t valid_count = (counter < history_size) ? counter : history_size;

    //    size_t max_run_length = 1;
    //    size_t current_run = 1;

    //    for (size_t i = 1; i < valid_count; i++)
    //    {
    //        if (previous_frame_ids[i] == previous_frame_ids[i - 1])
    //        {
    //            current_run++;
    //        }
    //        else
    //        {
    //            max_run_length = std::max(max_run_length, current_run);
    //            current_run = 1;
    //        }
    //    }

    //    max_run_length = std::max(max_run_length, current_run);

    //    int detected_mode = 0;
    //    if (max_run_length >= 4)
    //        detected_mode = 3;
    //    else if (max_run_length == 3)
    //        detected_mode = 2;
    //    else if (max_run_length == 2)
    //        detected_mode = 1;

    //    // --- Stability filter ---
    //    static int candidate_mode = 0; // No FG at start
    //    static int candidate_count = 0;
    //    constexpr int stability_threshold = 4;

    //    if (detected_mode == candidate_mode)
    //    {
    //        candidate_count++;
    //    }
    //    else
    //    {
    //        candidate_mode = detected_mode;
    //        candidate_count = 1;
    //    }

    //    // Redundant
    //    // max_run_length = std::max(max_run_length, current_run);

    //    // Only commit after stable detection
    //    if (candidate_count >= stability_threshold)
    //    {
    //        if (candidate_mode == 0 && _FgNumFramesToGenerate > 0)
    //        {
    //            _FgNumFramesToGenerate = 0;
    //            LOG_DEBUG("DLSS FG no longer detected");
    //        }
    //        else if (_FgNumFramesToGenerate != candidate_mode)
    //        {
    //            _FgNumFramesToGenerate = candidate_mode;

    //            LOG_DEBUG("DLSS FG detected: {}x mode", candidate_mode + 1);
    //        }
    //    }
    //}

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        // The split: the other side gets it too
        const auto side = CurrentSide();
        const auto route = RouteFor(side, Call::Marker);
        NvAPI_Status xellResult = NVAPI_OK;

        if (route.xell)
            xellResult = nvapi_calls::NvAPI_D3D12_SetAsyncFrameMarker(pCommandQueue, pSetAsyncFrameMarkerParams);

        if (route.driver)
            return ResultFor(side, xellResult, sideSetAsyncFrameMarker(side, pCommandQueue, pSetAsyncFrameMarkerParams));

        return xellResult;
    }

    return o_NvAPI_D3D12_SetAsyncFrameMarker(pCommandQueue, pSetAsyncFrameMarkerParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_Vulkan_SetLatencyMarker(HANDLE vkDevice,
                                                          NV_VULKAN_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    if (isGameCall(_ReturnAddress()))
        _gameCalledReflex = true;

    _updatesWithoutMarker = 0;

    // A bridged game: XeFG's XeLL gets the marker (and its frame ID is noted for XeFG to follow), the driver still gets
    // it too (in the split: the driver, or Anti-Lag Vk, is the side that lowers latency). Our own markers are not the
    // game's.
    if (!_ownCall)
    {
        if (auto* xellDevice = vkForwardDevice())
        {
            const auto side = CurrentSide();

            if (RouteFor(side, Call::Marker).xell)
                xellMarker(xellDevice, pSetLatencyMarkerParams->frameID,
                           (uint32_t) pSetLatencyMarkerParams->markerType);

            return sideSetLatencyMarkerVk(side, vkDevice, pSetLatencyMarkerParams);
        }
    }
    else if (const auto side = CurrentSide(); RouteFor(side, Call::Marker).driver)
    {
        // Ours in the split (they go to the game's VkDevice, NativeLowLatencyRule.h PresentTarget): XeLL on the bridge's
        // device has them too, for XeFG's frame IDs
        if (auto* xellDevice = bridgedXellDevice())
        {
            if (RouteFor(side, Call::Marker).xell)
                xellMarker(xellDevice, pSetLatencyMarkerParams->frameID,
                           (uint32_t) pSetLatencyMarkerParams->markerType);

            return sideSetLatencyMarkerVk(side, vkDevice, pSetLatencyMarkerParams);
        }
    }

    State::Instance().reflexFrameId = pSetLatencyMarkerParams->frameID;

    return o_NvAPI_Vulkan_SetLatencyMarker(vkDevice, pSetLatencyMarkerParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_Vulkan_SetSleepMode(HANDLE vkDevice,
                                                      NV_VULKAN_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif
    std::scoped_lock lock(_sleepModeMutex);

    if (isGameCall(_ReturnAddress()))
    {
        _gameCalledReflex = true;
        _gameCalledSetSleepMode = true;
    }

    // Our own calls: as in hkNvAPI_D3D_SetSleepMode, not stored (update() would see a Vulkan sleep device and hand the
    // cap to Reflex); they carry the cap themselves when the low latency keeps it.
    if (_ownCall)
    {
        // The split on the bridge: XeLL has it with low latency off (it would sleep with the one XeFG made it with), the
        // driver, or Anti-Lag Vk, ours as it is
        if (const auto side = CurrentSide(); RouteFor(side, Call::SetSleepMode).driver)
        {
            if (auto* xellDevice = bridgedXellDevice())
            {
                xellSetSleepMode(xellDevice, ToMode(*pSetSleepModeParams), side);
                return ResultFor(side, NVAPI_OK, sideSetSleepModeVk(side, vkDevice, pSetSleepModeParams));
            }
        }

        return o_NvAPI_Vulkan_SetSleepMode(vkDevice, pSetSleepModeParams);
    }

    // A bridged game: the mode goes to XeFG's XeLL, the driver gets it with low latency off. Asked before the new mode
    // is stored: a change of owner hands the previous one over first.
    auto* xellDevice = vkForwardDevice();

    // Store for later so we can adjust the fps whenever we want
    memcpy(&_lastVkSleepParams, pSetSleepModeParams, sizeof(NV_VULKAN_SET_SLEEP_MODE_PARAMS));
    _lastVkSleepDev = vkDevice;

    if (xellDevice != nullptr)
        return forwardVkSleepMode(xellDevice, vkDevice, *pSetSleepModeParams);

    if (_minimumIntervalUs != 0)
        pSetSleepModeParams->minimumIntervalUs = _minimumIntervalUs;

    if (Blocked())
    {
        auto off = *pSetSleepModeParams;
        ForOutput(off);
        return o_NvAPI_Vulkan_SetSleepMode(vkDevice, &off);
    }

    return o_NvAPI_Vulkan_SetSleepMode(vkDevice, pSetSleepModeParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_Vulkan_Sleep(HANDLE vkDevice, NvU64 signalValue)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    if (isGameCall(_ReturnAddress()))
        _gameCalledReflex = true;

    // A bridged game: XeFG's XeLL sleeps here. The driver's Sleep (its mode is off, nothing to wait for) still runs
    // whatever XeLL said, it signals the semaphore the game waits on. In the split XeLL does not sleep: the driver's
    // Sleep (or fakenvapi's, which signals the semaphore for Anti-Lag Vk) is the one that waits.
    if (!_ownCall)
    {
        if (auto* xellDevice = vkForwardDevice();
            xellDevice != nullptr && RouteFor(CurrentSide(), Call::Sleep).xell)
            xellSleep(xellDevice);
    }

    return o_NvAPI_Vulkan_Sleep(vkDevice, signalValue);
}

NvAPI_Status ReflexHooks::hkNvAPI_Vulkan_GetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    // The split on the bridge: the report is the driver's (the real GPU times), or Anti-Lag Vk's
    if (const auto side = CurrentSide(); RouteFor(side, Call::GetLatency).driver && bridgedXellDevice() != nullptr)
    {
        const auto result = sideGetLatencyVk(side, vkDevice, pGetLatencyParams);

        // An Anti-Lag Vk that could not start has no report: the driver's goes to the game
        if (result == NVAPI_OK || side != Side::AntiLag2)
            return result;

        return o_NvAPI_Vulkan_GetLatency(vkDevice, pGetLatencyParams);
    }

    // A bridged game: XeLL's report (fakenvapi's), copied field by field into the Vulkan layout
    if (!_ownCall)
    {
        if (auto* xellDevice = vkForwardDevice())
        {
            std::scoped_lock lock(_sleepModeMutex);

            // Not freed, like the others: static and big
            static NV_LATENCY_RESULT_PARAMS* d3dResults = new NV_LATENCY_RESULT_PARAMS();
            *d3dResults = {};
            d3dResults->version = NV_LATENCY_RESULT_PARAMS_VER;

            const auto result = nvapi_calls::NvAPI_D3D_GetLatency(xellDevice, d3dResults);

            if (result != NVAPI_OK)
                return result;

            for (size_t i = 0; i < std::size(d3dResults->frameReport); i++)
            {
                const auto& from = d3dResults->frameReport[i];
                auto& to = pGetLatencyParams->frameReport[i];

                to = {};
                to.frameID = from.frameID;
                to.inputSampleTime = from.inputSampleTime;
                to.simStartTime = from.simStartTime;
                to.simEndTime = from.simEndTime;
                to.renderSubmitStartTime = from.renderSubmitStartTime;
                to.renderSubmitEndTime = from.renderSubmitEndTime;
                to.presentStartTime = from.presentStartTime;
                to.presentEndTime = from.presentEndTime;
                to.driverStartTime = from.driverStartTime;
                to.driverEndTime = from.driverEndTime;
                to.osRenderQueueStartTime = from.osRenderQueueStartTime;
                to.osRenderQueueEndTime = from.osRenderQueueEndTime;
                to.gpuRenderStartTime = from.gpuRenderStartTime;
                to.gpuRenderEndTime = from.gpuRenderEndTime;
            }

            return NVAPI_OK;
        }
    }

    return o_NvAPI_Vulkan_GetLatency(vkDevice, pGetLatencyParams);
}

void ReflexHooks::hookReflex(PFN_NvApi_QueryInterface& queryInterface)
{
    if (!_inited)
    {
#ifdef _DEBUG
        LOG_FUNC();
#endif

        o_NvAPI_D3D_SetSleepMode = GET_INTERFACE(NvAPI_D3D_SetSleepMode, queryInterface);
        o_NvAPI_D3D_Sleep = GET_INTERFACE(NvAPI_D3D_Sleep, queryInterface);
        o_NvAPI_D3D_GetLatency = GET_INTERFACE(NvAPI_D3D_GetLatency, queryInterface);
        o_NvAPI_D3D_SetLatencyMarker = GET_INTERFACE(NvAPI_D3D_SetLatencyMarker, queryInterface);
        o_NvAPI_D3D12_SetAsyncFrameMarker = GET_INTERFACE(NvAPI_D3D12_SetAsyncFrameMarker, queryInterface);
        o_NvAPI_Vulkan_SetLatencyMarker = GET_INTERFACE(NvAPI_Vulkan_SetLatencyMarker, queryInterface);
        o_NvAPI_Vulkan_SetSleepMode = GET_INTERFACE(NvAPI_Vulkan_SetSleepMode, queryInterface);
        o_NvAPI_Vulkan_Sleep = GET_INTERFACE(NvAPI_Vulkan_Sleep, queryInterface);
        o_NvAPI_Vulkan_GetLatency = GET_INTERFACE(NvAPI_Vulkan_GetLatency, queryInterface);
        o_NvAPI_Vulkan_InitLowLatencyDevice = GET_INTERFACE(NvAPI_Vulkan_InitLowLatencyDevice, queryInterface);

        _inited = o_NvAPI_D3D_SetSleepMode && o_NvAPI_D3D_Sleep && o_NvAPI_D3D_GetLatency &&
                  o_NvAPI_D3D_SetLatencyMarker && o_NvAPI_D3D12_SetAsyncFrameMarker &&
                  o_NvAPI_Vulkan_SetLatencyMarker && o_NvAPI_Vulkan_SetSleepMode && o_NvAPI_Vulkan_Sleep &&
                  o_NvAPI_Vulkan_GetLatency;

        if (_inited)
            LOG_DEBUG("Inited Reflex hooks");
    }
}

uint8_t ReflexHooks::dlssgFrameCountToGenerate() { return _FgNumFramesToGenerate; }

void ReflexHooks::setDlssgFrameCount(uint8_t count) { _FgNumFramesToGenerate = count; }

bool ReflexHooks::isReflexHooked() { return _inited; }

void* ReflexHooks::getHookedReflex(unsigned int InterfaceId)
{
    if (InterfaceId == GET_ID(NvAPI_D3D_SetSleepMode) && o_NvAPI_D3D_SetSleepMode)
    {
        return &hkNvAPI_D3D_SetSleepMode;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D_Sleep) && o_NvAPI_D3D_Sleep)
    {
        return &hkNvAPI_D3D_Sleep;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D_GetLatency) && o_NvAPI_D3D_GetLatency)
    {
        return &hkNvAPI_D3D_GetLatency;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D_SetLatencyMarker) && o_NvAPI_D3D_SetLatencyMarker)
    {
        return &hkNvAPI_D3D_SetLatencyMarker;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D12_SetAsyncFrameMarker) && o_NvAPI_D3D12_SetAsyncFrameMarker)
    {
        return &hkNvAPI_D3D12_SetAsyncFrameMarker;
    }
    if (InterfaceId == GET_ID(NvAPI_Vulkan_SetLatencyMarker) && o_NvAPI_Vulkan_SetLatencyMarker)
    {
        return &hkNvAPI_Vulkan_SetLatencyMarker;
    }
    if (InterfaceId == GET_ID(NvAPI_Vulkan_SetSleepMode) && o_NvAPI_Vulkan_SetSleepMode)
    {
        return &hkNvAPI_Vulkan_SetSleepMode;
    }
    if (InterfaceId == GET_ID(NvAPI_Vulkan_Sleep) && o_NvAPI_Vulkan_Sleep)
    {
        return &hkNvAPI_Vulkan_Sleep;
    }
    if (InterfaceId == GET_ID(NvAPI_Vulkan_GetLatency) && o_NvAPI_Vulkan_GetLatency)
    {
        return &hkNvAPI_Vulkan_GetLatency;
    }

    return nullptr;
}

#define UPDATE_TIMING_ENTRY(name, type)                                                                                \
    if (frameReport.name##EndTime >= frameReport.name##StartTime)                                                      \
    {                                                                                                                  \
        double name##Pos = (double) (frameReport.name##StartTime - start) / rangeNs;                                   \
        double name##Length = (double) (frameReport.name##EndTime - frameReport.name##StartTime) / rangeNs;            \
        timingData[TimingType::type] = TimingEntry { .position = name##Pos, .length = name##Length };                  \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        timingData[TimingType::type].reset();                                                                          \
    }

bool ReflexHooks::updateTimingData()
{
    // Why the Reflex timings are empty, once per reason (the menu's timing graph and the FPS overlay's Reflex timings)
    static const char* lastWhy = nullptr;
    auto noData = [&](const char* why)
    {
        if (lastWhy != why)
        {
            lastWhy = why;
            LOG_INFO("Reflex timings: no data, {}", why);
        }

        return false;
    };

    if (!_inited)
        return noData("the Reflex hooks are not up (NvAPI Reflex functions missing)");

    auto processFrameReport = [&](const auto& frameReport) -> bool
    {
        uint64_t start = UINT64_MAX;
        uint64_t end = 0;

        // Please don't look, just thought it would be least work
        auto pTimes = (const uint64_t*) &frameReport.simStartTime;
        for (auto i = 0; i < 11; i++)
        {
            auto& time = pTimes[i];
            if (time == 0)
                continue;

            if (time < start)
                start = time;

            if (time > end)
                end = time;
        }

        if (end < start)
            return noData("the driver's frame report is empty (no markers reached it on this device)");

        double rangeNs = static_cast<double>(end - start);

        timingData[TimingType::TimeRange] = TimingEntry { .position = 0, .length = rangeNs };
        UPDATE_TIMING_ENTRY(sim, Simulation)
        UPDATE_TIMING_ENTRY(renderSubmit, RenderSubmit)
        UPDATE_TIMING_ENTRY(present, Present)
        UPDATE_TIMING_ENTRY(driver, Driver)
        UPDATE_TIMING_ENTRY(osRenderQueue, OsRenderQueue)
        UPDATE_TIMING_ENTRY(gpuRender, GpuRender)

        if (frameReport.frameID != 0)
            State::Instance().reflexFrameId = frameReport.frameID;

        return true;
    };

    std::scoped_lock lock(_sleepModeMutex);

    // The game's Reflex device, else Optical F5Low's own Sleep device
    // A game that only marks frames for XeFG's XeLL sets no sleep device: its markers' device asks
    auto* markersDev = XefgOutput() && gameIsSendingMarkers() ? _lastDev[SIMULATION_START].load() : nullptr;
    // In the split the XeFG branch of the Sleep hook stores no device (XeLL's or ours): the side's device has the report
    // Both a D3D and a Vulkan side can be registered for a moment across an Output change: the one set last has the report
    const bool sideOnVulkan = XefgOutput() && Split() && _sideLastWasVk && _sideVkDev != nullptr;
    auto* sideDev = XefgOutput() && Split() && !sideOnVulkan ? _sideDev : nullptr;

    if (auto sleepDev = sideDev != nullptr ? sideDev
                        : _lastSleepDev != nullptr
                            ? _lastSleepDev
                            : (_ownSleepDev != nullptr ? _ownSleepDev : markersDev);
        sleepDev && !sideOnVulkan && o_NvAPI_D3D_GetLatency)
    {
        // With XeFG, hkNvAPI_D3D_GetLatency answers from XeFG's XeLL through fakenvapi (the driver's report in the split)
        _timingEstimated = (XefgOutput() && !Split()) || fakenvapi::isUsingAsMainNvapi();

        // Not calling free on this but it's static so hopefully fine
        static NV_LATENCY_RESULT_PARAMS* results = new NV_LATENCY_RESULT_PARAMS();
        results->version = NV_LATENCY_RESULT_PARAMS_VER;

        if (auto result = hkNvAPI_D3D_GetLatency(sleepDev, results); result != NVAPI_OK)
        {
            LOG_WARN("NvAPI_D3D_GetLatency failed: {}", magic_enum::enum_name(result));
            return false;
        }

        static IUnknown* loggedDev = nullptr;
        if (loggedDev != sleepDev)
        {
            loggedDev = sleepDev;
            LOG_INFO("Reflex timings: reading the D3D report of the {} device {:X}",
                     sideDev != nullptr ? "split side" : _lastSleepDev != nullptr ? "game's" : "own", (size_t) sleepDev);
        }

        // 64th element has the latest data
        return processFrameReport(results->frameReport[63]);
    }

    // The split on the Vulkan bridge: Optical F5Low's Reflex runs on the side's VkDevice (a game with no Reflex of its
    // own, dxvk for one, never sets _lastVkSleepDev), and its report is the one with the timings
    auto* vkDev = XefgOutput() && Split() && _sideVkDev != nullptr ? _sideVkDev : _lastVkSleepDev;

    if (vkDev && o_NvAPI_Vulkan_GetLatency)
    {
        // Not calling free on this but it's static so hopefully fine
        static NV_VULKAN_LATENCY_RESULT_PARAMS* results = new NV_VULKAN_LATENCY_RESULT_PARAMS();
        results->version = NV_VULKAN_LATENCY_RESULT_PARAMS_VER;

        const auto result = hkNvAPI_Vulkan_GetLatency(vkDev, results);

        // Bridged, hkNvAPI_Vulkan_GetLatency answered from XeFG's XeLL through fakenvapi (asked after the call: it
        // hands the game's Reflex over when the owner changed)
        _timingEstimated = (bridgedReflexForwarding() && !Split()) || fakenvapi::isUsingAsMainNvapi();

        if (result != NVAPI_OK)
        {
            LOG_WARN("NvAPI_Vulkan_GetLatency failed: {}", magic_enum::enum_name(result));
            return false;
        }

        // 64th element has the latest data
        return processFrameReport(results->frameReport[63]);
    }

    return noData(vkDev == nullptr
                      ? "no Reflex device: no game, side or own sleep device (and no Vulkan one)"
                      : "no NvAPI_Vulkan_GetLatency");
}

// For updating information about Reflex hooks
void ReflexHooks::update(bool fgActive, bool isVulkan)
{
    if (State::Instance().externalFrameGeneration)
    {
        State::Instance().reflexLimitsFps = false;
        State::Instance().reflexShowWarning = false;
        return;
    }
    // We can still use just the markers to limit the fps with Reflex disabled
    // But need to fallback in case a game stops sending them for some reason
    _updatesWithoutMarker++;

    State::Instance().reflexShowWarning = false;

    // Only the game's own Reflex calls say it is forwarded (vkForwardDevice, VK_NV_low_latency2); with no bridge to
    // forward to it no longer is, whenever the game calls next
    auto* bridgedXell = isVulkan ? bridgedXellDevice() : nullptr;

    if (bridgedXell == nullptr)
        noteBridgedForwarding(false);

    if (_updatesWithoutMarker > 20 || !_inited)
    {
        State::Instance().reflexLimitsFps = false;
        return;
    }

    // The split (XeFG with Reflex or Anti-Lag 2 as the Output): the other side holds the cap, not XeLL, with the game's own
    // sleep mode, unless OptiScaler's limiter keeps it where the driver was seen not to hold it (SplitLimiterKeepsCap, the
    // same rule our own calls take), or the game turned its Reflex off (fakenvapi might override that, as below). A bridged Vulkan game's
    // is its NvAPI_Vulkan one: one on VK_NV_low_latency2 gets no cap from us (no swapchain to send it on) and one that only
    // marks frames has no mode of the other side's, so the limiter keeps it for both. The Vulkan present and the bridge's
    // D3D12 present both ask: the answer must be the same for both, or it flips every frame.
    const auto splitHoldsCap = [&]() -> bool
    {
        if (native::lowlatency::SplitLimiterKeepsCap(CurrentSide(), State::Instance().swapchainInteropApi ==
                                                                        SwapchainInteropApi::Dx11wDx12))
            return false;

        if (bridgedXellDevice() != nullptr)
            return _lastVkSleepDev != nullptr && (fakenvapi::isUsingAsMainNvapi() || _lastVkSleepParams.bLowLatencyMode);

        // A D3D game that marks frames and never sleeps: only Anti-Lag 2 (fakenvapi) acts on the markers alone
        return fakenvapi::isUsingAsMainNvapi() || (_lastSleepDev != nullptr && _lastSleepParams.bLowLatencyMode);
    };

    if (bridgedXell != nullptr && (_lastVkSleepDev || _lastLl2ModeSet || gameIsSendingMarkers()))
    {
        // XeFG's XeLL holds the cap, with the game's sleep mode or one we send (setFPSLimit) once markers come, as on
        // D3D; while it cannot (XellHoldsCap) OptiScaler's limiter does. The Vulkan present and the bridge's D3D12
        // present both come here: the answer must not depend on fgActive, or it flips every frame.
        State::Instance().reflexLimitsFps = Split() ? splitHoldsCap() : XellHoldsCap();
    }
    else if (isVulkan && _lastVkSleepDev)
    {
        // fgActive doesn't matter for vulkan
        // isUsingAsMainNvapi() because fakenvapi might override the reflex' setting and we don't know it
        State::Instance().reflexLimitsFps = fakenvapi::isUsingAsMainNvapi() || _lastVkSleepParams.bLowLatencyMode;
    }
    else if (!isVulkan && XefgOutput() && (_lastSleepDev || gameIsSendingMarkers()))
    {
        // The game's Reflex goes to XeFG's XeLL with low latency on (SleepModeForOutput), with a mode from us when it
        // never sets one: XeLL holds the cap while it can (XellHoldsCap), OptiScaler's limiter otherwise.
        State::Instance().reflexLimitsFps = Split() ? splitHoldsCap() : XellHoldsCap();
    }
    else if (_lastSleepDev)
    {
        // Don't use when: Real Reflex markers + FSR FG + Reflex disabled, causes huge input latency
        State::Instance().reflexLimitsFps =
            fakenvapi::isUsingAsMainNvapi() || !fgActive || _lastSleepParams.bLowLatencyMode;
        State::Instance().reflexShowWarning = State::Instance().activeFgOutput == FGOutput::FSRFG &&
                                              !fakenvapi::isUsingAsMainNvapi() && fgActive &&
                                              _lastSleepParams.bLowLatencyMode;
    }
    else
    {
        State::Instance().reflexLimitsFps = false;
    }

    // The Framerate page's Input is Off: the game's Reflex goes out with low latency off and no interval, so it keeps no
    // cap; OptiScaler's limiter does. A change of the Input sends the stored sleep mode again, in the other shape.
    const bool blocked = Blocked();
    static bool lastBlocked = false;
    const bool blockChanged = blocked != lastBlocked;
    lastBlocked = blocked;

    if (blocked)
    {
        State::Instance().reflexLimitsFps = false;
        State::Instance().reflexShowWarning = false;
    }

    // Disable reflex fps limiting when reflex is force disabled
    // TODO: It changes without pressing apply, XeLL with XeFG can't actually be disabled so this is wrong
    //
    // Wait for fakenvapi to be merged into Opti for better integration
    //
    // if (State::Instance().reflexLimitsFps && (fakenvapi::isUsingAsMainNvapi() ||
    // fakenvapi::isUsingOnNvidia())
    // &&
    //    Config::Instance()->FN_ForceReflex.value_or_default() == 1)
    //{
    //    State::Instance().reflexLimitsFps = false;
    //}

    // The side that lowers latency with XeFG changed (the Output, or Input Off / Force Disable switched the split on or
    // off): the side that had the sleep mode lets it go, and the stored one is sent again in the new shape. Also when XeFG
    // made a new XeLL context where XeLL must not sleep (the split, Input Off, Force Disable): it starts with low latency
    // on (XeFG_Dx12), and the stored mode has to be sent again to turn it off.
    const auto side = CurrentSide();
    static Side lastSide = Side::XeLL;
    static uint32_t lastXellGeneration = 0;
    const auto xellGeneration = fakenvapi::forcedModeGeneration();
    const bool sideChanged = side != lastSide;
    const bool xellRemade = (side != Side::XeLL || Blocked() || ForceDisabled()) && xellGeneration != lastXellGeneration;

    if (sideChanged)
    {
        releaseSide(lastSide);

        if (side == Side::XeLL)
            LOG_INFO("XeFG low latency: XeLL lowers latency");
        else
            LOG_INFO("XeFG low latency: {} lowers latency, XeLL stays for XeFG with low latency off",
                     side == Side::Reflex ? "NVIDIA Reflex" : "Anti-Lag 2");
    }

    lastSide = side;
    lastXellGeneration = xellGeneration;

    static float lastFps = 0;
    static bool lastReflexLimitsFps = State::Instance().reflexLimitsFps;
    static LowLatencyMode lastLowLatencyMode = fakenvapi::getCurrentMode();

    // Reset required when toggling Reflex
    if (State::Instance().reflexLimitsFps != lastReflexLimitsFps || blockChanged || sideChanged || xellRemade)
    {
        lastReflexLimitsFps = State::Instance().reflexLimitsFps;
        lastFps = 0;
        setFPSLimit(0);
    }
    else if (fakenvapi::isUsingAsMainNvapi() && lastLowLatencyMode != fakenvapi::getCurrentMode())
    {
        lastLowLatencyMode = fakenvapi::getCurrentMode();
        lastFps = 0;
        setFPSLimit(0);
    }

    // With XeFG, a game that only marks frames gets its sleep mode from setFPSLimit alone: once more when its markers
    // start, or with no cap change and no change of the cap's owner XeLL would never get one and never sleep
    static bool lastXefgMarkers = false;

    if (const bool xefgMarkers = XefgOutput() && gameIsSendingMarkers(); xefgMarkers != lastXefgMarkers)
    {
        lastXefgMarkers = xefgMarkers;

        if (xefgMarkers)
            setFPSLimit(lastFps);
    }

    if (!State::Instance().reflexLimitsFps)
        return;

    float currentFps = Config::Instance()->FramerateLimit.value_or_default();
    static uint8_t lastFgNumFramesToGenerate = 0;

    if (lastFgNumFramesToGenerate != _FgNumFramesToGenerate)
    {
        if (fakenvapi::isUsingAsMainNvapi())
        {
            // Don't reload when using Dynamic MFG
            if (State::Instance().dlssgLastSetMode != sl::DLSSGMode::eDynamic &&
                !Config::Instance()->FGDLSSGForceDMFG.value_or_default())
            {
                State::Instance().fakenvapiReloadLowLatency = true;
            }

            // fakenvapi's latency techs fall apart with more than 1 fake frame
            if (Config::Instance()->FN_ForceReflex.value_or_default() != ForceReflex::ForceEnable &&
                (State::Instance().dlssgLastSetMode == sl::DLSSGMode::eDynamic || _FgNumFramesToGenerate > 1))
            {
                Config::Instance()->FN_ForceReflex.set_volatile_value(ForceReflex::ForceDisable);
            }
        }

        lastFgNumFramesToGenerate = _FgNumFramesToGenerate;
        setFPSLimit(currentFps);

        if (_FgNumFramesToGenerate == 0)
            LOG_DEBUG("DLSS FG no longer detected");
        else
            LOG_DEBUG("DLSS FG detected, mode: {}x", _FgNumFramesToGenerate + 1);
    }

    // TODO: replace fgActive with _FgNumFramesToGenerate
    // _FgNumFramesToGenerate needs to be correctly updated
    if (fgActive && State::Instance().activeFgOutput == FGOutput::FSRFG)
    {
        currentFps /= 2;
    }
    else if (side != Side::XeLL)
    {
        // XeFG with Reflex or Anti-Lag 2 as the Output: the other side paces the game's own frames and XeFG shows several
        // per frame. Stable while XeFG generates (not per present, as fgActive is).
        currentFps = native::lowlatency::CapPerRenderedFrame(currentFps, native::lowlatency::XefgShownPerRendered());
    }
    else if (_FgNumFramesToGenerate > 0 && fakenvapi::isUsingAsMainNvapi() &&
             fakenvapi::getCurrentMode() != LowLatencyMode::XeLL)
    {
        currentFps /= (_FgNumFramesToGenerate + 1);
    }

    if (currentFps != lastFps)
    {
        setFPSLimit(currentFps);
        lastFps = currentFps;
    }
}

// 0 - disables the fps cap
void ReflexHooks::setFPSLimit(float fps)
{
    if (State::Instance().externalFrameGeneration)
        return;
    LOG_INFO("Set FPS Limit to: {}", fps);
    if (fps == 0.0)
        _minimumIntervalUs = 0;
    else
        _minimumIntervalUs = static_cast<uint32_t>(std::round(1'000'000 / fps));

    std::scoped_lock lock(_sleepModeMutex);

    struct KeepBothSides
    {
        KeepBothSides() { _keepBothSides = true; }
        ~KeepBothSides() { _keepBothSides = false; }
    } keepBothSides;

    const auto side = CurrentSide();

    if (_lastSleepDev != nullptr)
    {
        NV_SET_SLEEP_MODE_PARAMS temp {};
        memcpy(&temp, &_lastSleepParams, sizeof(NV_SET_SLEEP_MODE_PARAMS));
        temp.minimumIntervalUs = _minimumIntervalUs;

        if (XefgOutput())
        {
            // XeLL's (low latency off in the split), then the other side the game's own with the cap. The cap replaces the
            // game's interval, 0 included (the limiter keeps the cap then).
            auto game = ToMode(_lastSleepParams);
            game.minimumIntervalUs = 0;
            auto forXefg = ForOutput(temp, side, _minimumIntervalUs);
            nvapi_calls::NvAPI_D3D_SetSleepMode(_lastSleepDev, &forXefg);

            if (RouteFor(side, Call::SetSleepMode).driver)
            {
                auto real = WithMode(temp, native::xefgreflex::DriverSleepModeInSplit(game, _minimumIntervalUs));
                sideSetSleepMode(side, _lastSleepDev, &real);
            }
        }
        else
        {
            temp = ForOutput(temp); // unchanged unless the Framerate page's Input is Off
            o_NvAPI_D3D_SetSleepMode(_lastSleepDev, &temp);
        }
    }
    else if (auto* markerDev = _lastDev[SIMULATION_START].load();
             XefgOutput() && markerDev != nullptr && gameIsSendingMarkers() && bridgedXellDevice() == nullptr)
    {
        // A D3D game that marks frames but never sets a sleep mode: XeLL gets one from us, or it never sleeps
        NV_SET_SLEEP_MODE_PARAMS temp {};
        temp.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        temp.minimumIntervalUs = _minimumIntervalUs;
        auto forXefg = ForOutput(temp, side);
        nvapi_calls::NvAPI_D3D_SetSleepMode(markerDev, &forXefg);

        // The split: a game that marks frames and never sleeps has nothing for the driver's Reflex to wait in, as without
        // us (the limiter keeps the cap, splitHoldsCap). Anti-Lag 2 sleeps on the markers by itself, with the cap.
        if (side == Side::AntiLag2)
        {
            temp.bLowLatencyMode = true;
            sideSetSleepMode(side, markerDev, &temp);
        }
    }

    // A bridged Vulkan game: the owner of its NvAPI_Vulkan mode is vkForwardDevice's, handed over here if it changed
    // since the game's last call, so the cap never goes to one side while the other has the game's mode
    auto* xellDevice = _lastVkSleepDev != nullptr ? vkForwardDevice() : bridgedXellDevice();

    if (xellDevice != nullptr)
    {
        // The cap goes into XeLL (the driver's mode is off and stays so). A game that marks frames but never sets a
        // sleep mode gets one from us: low latency on, with the cap.
        native::xefgreflex::SleepMode mode;

        if (_lastVkSleepDev != nullptr)
            mode = ToMode(_lastVkSleepParams);
        else if (_lastLl2ModeSet)
            mode = _lastLl2Mode;
        else
            mode.lowLatency = true;

        // VK_NV_low_latency2 has no Anti-Lag 2 side (AMD has no such extension): its calls stay XeLL's, which does the work
        const auto xellSide = _lastVkSleepDev == nullptr && _lastLl2ModeSet && side == Side::AntiLag2 ? Side::XeLL : side;

        if (_lastVkSleepDev != nullptr || _lastLl2ModeSet || gameIsSendingMarkers())
            xellSetSleepMode(xellDevice, mode, xellSide);

        // The split: the other side of the NvAPI_Vulkan mode gets the game's own with the cap (xellDevice is also non-null
        // for a game on VK_NV_low_latency2, which has no such mode here). When the split ends, update() has releaseSide
        // switch it off.
        if (_lastVkSleepDev != nullptr && RouteFor(side, Call::SetSleepMode).driver)
        {
            auto game = ToMode(_lastVkSleepParams);
            game.minimumIntervalUs = 0; // the cap replaces the game's interval, 0 included
            auto real = WithMode(_lastVkSleepParams, native::xefgreflex::DriverSleepModeInSplit(game, _minimumIntervalUs));
            sideSetSleepModeVk(side, _lastVkSleepDev, &real);
        }
    }
    else if (_lastVkSleepDev != nullptr)
    {
        NV_VULKAN_SET_SLEEP_MODE_PARAMS temp {};
        memcpy(&temp, &_lastVkSleepParams, sizeof(NV_VULKAN_SET_SLEEP_MODE_PARAMS));
        temp.minimumIntervalUs = _minimumIntervalUs;
        ForOutput(temp);
        o_NvAPI_Vulkan_SetSleepMode(_lastVkSleepDev, &temp);
    }
}

uint64_t ReflexHooks::lastMarkerFrameId(NV_LATENCY_MARKER_TYPE type)
{
    return (size_t) type < std::size(_lastFrameId) ? _lastFrameId[type].load(std::memory_order_relaxed) : 0;
}

uint32_t ReflexHooks::gameXellFrameId()
{
    if (!gameCalledReflex() || !gameIsSendingMarkers())
        return 0;

    if (const auto renderSubmit = lastMarkerFrameId(RENDERSUBMIT_START); renderSubmit != 0)
        return (uint32_t) renderSubmit;

    return (uint32_t) lastMarkerFrameId(SIMULATION_START);
}

bool ReflexHooks::gameIsSendingMarkers()
{
    return _lastMarkerFrame != 0 && ((State::Instance().fgLastFrame < _lastMarkerFrame) ||
                                     (State::Instance().fgLastFrame - _lastMarkerFrame) < 5);
}

bool ReflexHooks::ensureTable(PFN_NvApi_QueryInterface queryInterface)
{
    if (!_inited && queryInterface != nullptr)
        hookReflex(queryInterface);

    return _inited;
}

namespace
{
struct OwnCallScope
{
    explicit OwnCallScope(bool& flag) : _flag(flag) { _flag = true; }
    ~OwnCallScope() { _flag = false; }
    bool& _flag;
};
} // namespace

NvAPI_Status ReflexHooks::ownSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams)
{
    std::scoped_lock lock(_sleepModeMutex);

    // The game's own SetSleepMode is the last word, even one that came in just before ours. With XeFG so is any mode
    // its side set (Streamline's calls are not the game's module) or that we sent for a game that only marks frames
    // (setFPSLimit): our stop would turn XeLL's low latency and cap off under it.
    if (_gameCalledSetSleepMode || (XefgOutput() && (_lastSleepDev != nullptr || gameIsSendingMarkers())))
        return NVAPI_OK;

    OwnCallScope scope(_ownCall);
    return hkNvAPI_D3D_SetSleepMode(pDev, pParams);
}

// Streamline's own modules (sl.interposer, sl.reflex, sl.pcl, sl.dlss_g ...) and the DLSS frame generation module
static bool IsStreamlineModule(HMODULE module)
{
    if (module == nullptr)
        return false;

    static std::mutex cacheMutex;
    static std::unordered_map<HMODULE, bool> cache;

    std::scoped_lock lock(cacheMutex);

    if (auto it = cache.find(module); it != cache.end())
        return it->second;

    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(module, path, MAX_PATH);

    auto name = std::filesystem::path(path).filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);

    const bool streamline = name.starts_with(L"sl.") || name == L"nvngx_dlssg.dll";
    cache[module] = streamline;
    return streamline;
}

bool ReflexHooks::isGameCall(void* returnAddress)
{
    if (_ownCall)
        return false;

    // A game that started Streamline itself may run its own Reflex through it (NBA 2K27): Streamline's calls are its
    if (State::Instance().gameCalledSlInit)
        return true;

    // Otherwise Streamline is the one OptiScaler loaded for its own DLSS frame generation output: its Reflex calls are
    // OptiScaler's, not the game's
    return !IsStreamlineModule(Util::GetCallerModule(returnAddress));
}

void ReflexHooks::forgetSleepDevice(IUnknown* pDev)
{
    std::scoped_lock lock(_sleepModeMutex);

    if (pDev != nullptr && _lastSleepDev == pDev)
        _lastSleepDev = nullptr;

    if (pDev != nullptr && _ownSleepDev == pDev)
        _ownSleepDev = nullptr;

    // The other side of the split lets go of it too (Anti-Lag 2's context belongs to the device)
    if (pDev != nullptr && _sideDev == pDev)
        _sideDev = nullptr;

    if (pDev != nullptr)
        fakenvapi::companionDeviceGone(pDev);

    // The markers' devices too: GetLatency and setFPSLimit fall back to the last simulation start's (the next marker
    // puts a live one back)
    for (auto& dev : _lastDev)
    {
        if (auto* expected = pDev; expected != nullptr)
            dev.compare_exchange_strong(expected, nullptr);
    }
}

NvAPI_Status ReflexHooks::ownSleep(IUnknown* pDev)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_D3D_Sleep(pDev);
}

NvAPI_Status ReflexHooks::ownSetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pParams)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_D3D_SetLatencyMarker(pDev, pParams);
}

NvAPI_Status ReflexHooks::ownGetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pParams)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_D3D_GetLatency(pDev, pParams);
}

NvAPI_Status ReflexHooks::routeSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams, bool game)
{
    routedinput::RoutedCallScope scope(_routedCall, game);
    return hkNvAPI_D3D_SetSleepMode(pDev, pParams);
}

NvAPI_Status ReflexHooks::routeSleep(IUnknown* pDev, bool game)
{
    routedinput::RoutedCallScope scope(_routedCall, game);
    return hkNvAPI_D3D_Sleep(pDev);
}

NvAPI_Status ReflexHooks::routeGetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pParams)
{
    routedinput::RoutedCallScope scope(_routedCall, false);
    return hkNvAPI_D3D_GetLatency(pDev, pParams);
}

NvAPI_Status ReflexHooks::routeSetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pParams, bool game)
{
    routedinput::RoutedCallScope scope(_routedCall, game);
    return hkNvAPI_D3D_SetLatencyMarker(pDev, pParams);
}

NvAPI_Status ReflexHooks::routeSetAsyncFrameMarker(ID3D12CommandQueue* pQueue, NV_ASYNC_FRAME_MARKER_PARAMS* pParams,
                                                   bool game)
{
    routedinput::RoutedCallScope scope(_routedCall, game);
    return hkNvAPI_D3D12_SetAsyncFrameMarker(pQueue, pParams);
}

void ReflexHooks::forgetSleepDeviceVulkan(HANDLE vkDevice)
{
    std::scoped_lock lock(_sleepModeMutex);

    if (vkDevice != nullptr && _lastVkSleepDev == vkDevice)
        _lastVkSleepDev = nullptr;

    if (vkDevice != nullptr && _sideVkDev == vkDevice)
        _sideVkDev = nullptr;

    if (vkDevice != nullptr)
        fakenvapi::companionDeviceGone(vkDevice);
}

NvAPI_Status ReflexHooks::ownSetSleepModeVulkan(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pParams)
{
    std::scoped_lock lock(_sleepModeMutex);

    // The game's own SetSleepMode is the last word, even one that came in just before ours
    if (_gameCalledSetSleepMode)
        return NVAPI_OK;

    OwnCallScope scope(_ownCall);
    return hkNvAPI_Vulkan_SetSleepMode(vkDevice, pParams);
}

NvAPI_Status ReflexHooks::ownSleepVulkan(HANDLE vkDevice, NvU64 signalValue)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_Vulkan_Sleep(vkDevice, signalValue);
}

NvAPI_Status ReflexHooks::ownSetLatencyMarkerVulkan(HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* pParams)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_Vulkan_SetLatencyMarker(vkDevice, pParams);
}

NvAPI_Status ReflexHooks::ownGetLatencyVulkan(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pParams)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_Vulkan_GetLatency(vkDevice, pParams);
}

NvAPI_Status ReflexHooks::ownInitLowLatencyDeviceVulkan(HANDLE vkDevice, HANDLE* signalSemaphore)
{
    if (o_NvAPI_Vulkan_InitLowLatencyDevice == nullptr)
        return NVAPI_NO_IMPLEMENTATION;

    return o_NvAPI_Vulkan_InitLowLatencyDevice(vkDevice, signalSemaphore);
}

void ReflexHooks::noteGameVulkanLowLatency2(bool setSleepMode)
{
    std::scoped_lock lock(_sleepModeMutex);

    _gameCalledReflex = true;

    if (setSleepMode)
        _gameCalledSetSleepMode = true;
}
