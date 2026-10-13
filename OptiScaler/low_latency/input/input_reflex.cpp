#include "pch.h"

#include "input_reflex.h"
#include "RoutedInputRule.h"
#include <hooks/Reflex_Hooks.h>

#include <intrin.h>
#include <type_traits>

#pragma intrinsic(_ReturnAddress)

namespace
{
SleepMode ToSleepMode(const NV_SET_SLEEP_MODE_PARAMS& params)
{
    SleepMode sleepMode {};
    sleepMode.low_latency_enabled = params.bLowLatencyMode;
    sleepMode.low_latency_boost = params.bLowLatencyBoost;
    sleepMode.minimum_interval_us = params.minimumIntervalUs;
    sleepMode.use_markers_to_optimize = params.bUseMarkersToOptimize;
    sleepMode.use_min_queue_time = params.bUseMinQueueTime;
    return sleepMode;
}
// The game calls these through the pointers ForInterface hands out
static_assert(std::is_same_v<decltype(&InputReflex::D3D_SetSleepMode), decltype(&NvAPI_D3D_SetSleepMode)>);
static_assert(std::is_same_v<decltype(&InputReflex::D3D_Sleep), decltype(&NvAPI_D3D_Sleep)>);
static_assert(std::is_same_v<decltype(&InputReflex::D3D_GetLatency), decltype(&NvAPI_D3D_GetLatency)>);
static_assert(std::is_same_v<decltype(&InputReflex::D3D_SetLatencyMarker), decltype(&NvAPI_D3D_SetLatencyMarker)>);
static_assert(
    std::is_same_v<decltype(&InputReflex::D3D12_SetAsyncFrameMarker), decltype(&NvAPI_D3D12_SetAsyncFrameMarker)>);

// InputCommon hears of the game's call, once: not of one made from inside the router (RoutedInputRule.h)
bool TellsInputCommon(bool game) { return routedinput::TellsInputCommon(game, ReflexHooks::inRoutedCall()); }
} // namespace

void* InputReflex::ForInterface(unsigned int interfaceId)
{
    if (interfaceId == GET_ID(NvAPI_D3D_SetSleepMode))
        return &D3D_SetSleepMode;
    if (interfaceId == GET_ID(NvAPI_D3D_Sleep))
        return &D3D_Sleep;
    if (interfaceId == GET_ID(NvAPI_D3D_GetLatency))
        return &D3D_GetLatency;
    if (interfaceId == GET_ID(NvAPI_D3D_SetLatencyMarker))
        return &D3D_SetLatencyMarker;
    if (interfaceId == GET_ID(NvAPI_D3D12_SetAsyncFrameMarker))
        return &D3D12_SetAsyncFrameMarker;

    return nullptr;
}

// Each call goes to the router first, with nothing in front of the game's call, and its answer is the game's.
// InputCommon then learns of it (only the game's: OptiScaler's Streamline is no input), its results are its own.

NvAPI_Status InputReflex::D3D_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams)
{
    if (pSetSleepModeParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    const bool game = ReflexHooks::isGameCall(_ReturnAddress());
    const bool tell = TellsInputCommon(game);

    // The game's own mode: the router puts the frame cap into the call it passes on
    auto sleepMode = ToSleepMode(*pSetSleepModeParams);
    const auto result = ReflexHooks::routeSetSleepMode(pDev, pSetSleepModeParams, game);

    if (tell && pDev != nullptr)
        InputCommon::set_sleep_mode(inputContext, pDev, &sleepMode);

    return result;
}

NvAPI_Status InputReflex::D3D_Sleep(IUnknown* pDev)
{
    const bool game = ReflexHooks::isGameCall(_ReturnAddress());
    const bool tell = TellsInputCommon(game);
    const auto result = ReflexHooks::routeSleep(pDev, game);

    if (tell && pDev != nullptr)
        InputCommon::sleep(inputContext, pDev);

    return result;
}

NvAPI_Status InputReflex::D3D_GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams)
{
    return ReflexHooks::routeGetLatency(pDev, pGetLatencyParams);
}

NvAPI_Status InputReflex::D3D_SetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams)
{
    if (pSetLatencyMarkerParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    const bool game = ReflexHooks::isGameCall(_ReturnAddress());
    const bool tell = TellsInputCommon(game);

    MarkerParams markerParams {};
    markerParams.frame_id = pSetLatencyMarkerParams->frameID;
    markerParams.marker_type = (MarkerType) pSetLatencyMarkerParams->markerType; // the enums match

    const auto result = ReflexHooks::routeSetLatencyMarker(pDev, pSetLatencyMarkerParams, game);

    // Tracked only: the router delivered it
    if (tell && pDev != nullptr)
        InputCommon::set_marker(inputContext, pDev, markerParams, false);

    return result;
}

NvAPI_Status InputReflex::D3D12_SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue,
                                                    NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams)
{
    if (pSetAsyncFrameMarkerParams == nullptr)
        return NVAPI_INVALID_ARGUMENT;

    return ReflexHooks::routeSetAsyncFrameMarker(pCommandQueue, pSetAsyncFrameMarkerParams,
                                                 ReflexHooks::isGameCall(_ReturnAddress()));
}

// Upstream's Vulkan side of the input: built only with LOW_LATENCY_INPUTS, which is not on yet (the Vulkan phase).
#ifdef LOW_LATENCY_INPUTS
#include <nvapi/NvApiHooks.h>
#include <hooks/Vulkan_Hooks.h>
#include <low_latency/low_latency_tech/ll_reflex_vk.h>

static NvAPI_Status ToNvApi(InputResult result, const char* call)
{
    if (result == InputResult::Ok || result == InputResult::UsingDifferentInput ||
        result == InputResult::NotEnoughReports)
    {
        return NVAPI_OK;
    }

    LOG_ERROR("{} result: {}", call, magic_enum::enum_name(result));

    return result == InputResult::NoReadyOutput ? NVAPI_OK : NVAPI_ERROR;
}

// The game gets its own timeline semaphore, the output's sleep doesn't need the driver's nvapi Vulkan Reflex
NvAPI_Status InputReflex::Vulkan_InitLowLatencyDevice(HANDLE vkDevice, HANDLE* signalSemaphoreHandle)
{
    if (!vkDevice || !signalSemaphoreHandle)
        return NVAPI_INVALID_ARGUMENT;

    if (vkSemaphore == VK_NULL_HANDLE)
    {
        VkSemaphoreTypeCreateInfo timelineInfo {};
        timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineInfo.initialValue = 0;

        VkSemaphoreCreateInfo createInfo {};
        createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        createInfo.pNext = &timelineInfo;

        if (VulkanHooks::o_vkCreateSemaphore == nullptr ||
            VulkanHooks::o_vkCreateSemaphore((VkDevice) vkDevice, &createInfo, nullptr, &vkSemaphore) != VK_SUCCESS)
        {
            LOG_ERROR("Can't create the Reflex semaphore");
            return NVAPI_ERROR;
        }
    }

    *signalSemaphoreHandle = (HANDLE) vkSemaphore;
    return NVAPI_OK;
}

NvAPI_Status InputReflex::Vulkan_DestroyLowLatencyDevice(HANDLE vkDevice)
{
    if (!vkDevice)
        return NVAPI_INVALID_ARGUMENT;

    if (vkSemaphore != VK_NULL_HANDLE)
    {
        if (auto destroy =
                (PFN_vkDestroySemaphore) VulkanHooks::GetDeviceProcAddr((VkDevice) vkDevice, "vkDestroySemaphore"))
        {
            destroy((VkDevice) vkDevice, vkSemaphore, nullptr);
        }

        vkSemaphore = VK_NULL_HANDLE;
    }

    return NVAPI_OK;
}

NvAPI_Status InputReflex::Vulkan_GetSleepStatus(HANDLE vkDevice,
                                                NV_VULKAN_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams)
{
    if (!vkDevice || !pGetSleepStatusParams)
        return NVAPI_INVALID_ARGUMENT;

    SleepParams sleepParams {};

    auto result = InputCommon::get_sleep_status(vulkanContext, (IUnknown*) vkDevice, &sleepParams);

    if (result == InputResult::Ok || result == InputResult::UsingDifferentInput)
    {
        pGetSleepStatusParams->bLowLatencyMode = sleepParams.low_latency_enabled;
        return NVAPI_OK;
    }

    return ToNvApi(result, "get_sleep_status");
}

NvAPI_Status InputReflex::Vulkan_SetSleepMode(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams)
{
    if (!vkDevice || !pSetSleepModeParams)
        return NVAPI_INVALID_ARGUMENT;

    SleepMode sleepMode {};
    sleepMode.low_latency_enabled = pSetSleepModeParams->bLowLatencyMode;
    sleepMode.low_latency_boost = pSetSleepModeParams->bLowLatencyBoost;
    sleepMode.minimum_interval_us = pSetSleepModeParams->minimumIntervalUs;
    sleepMode.use_markers_to_optimize = true;

    return ToNvApi(InputCommon::set_sleep_mode(vulkanContext, (IUnknown*) vkDevice, &sleepMode), "set_sleep_mode");
}

NvAPI_Status InputReflex::Vulkan_Sleep(HANDLE vkDevice, NvU64 signalValue)
{
    if (!vkDevice)
        return NVAPI_INVALID_ARGUMENT;

    auto result = InputCommon::sleep(vulkanContext, (IUnknown*) vkDevice);

    // The game waits for it whatever the output did
    if (vkSemaphore != VK_NULL_HANDLE && VulkanHooks::o_vkSignalSemaphore != nullptr)
    {
        VkSemaphoreSignalInfo signalInfo {};
        signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        signalInfo.semaphore = vkSemaphore;
        signalInfo.value = signalValue;

        VulkanHooks::o_vkSignalSemaphore((VkDevice) vkDevice, &signalInfo);
    }

    return ToNvApi(result, "sleep");
}

NvAPI_Status InputReflex::Vulkan_GetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams)
{
    if (!vkDevice || !pGetLatencyParams)
        return NVAPI_INVALID_ARGUMENT;

    if (pGetLatencyParams->version != NV_VULKAN_LATENCY_RESULT_PARAMS_VER1)
    {
        LOG_ERROR("Unsupported version {}", pGetLatencyParams->version);
        return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    }

    FrameReport reports[NVAPI_BUFFER_SIZE] {};
    auto result = InputCommon::get_frame_reports(vulkanContext, (IUnknown*) vkDevice, reports);

    // The Vulkan reports are the D3D ones without the GPU times
    for (size_t i = 0; i < NVAPI_BUFFER_SIZE; i++)
    {
        auto& reportOut = pGetLatencyParams->frameReport[i];
        std::memset(&reportOut, 0, sizeof(reportOut));
        std::memcpy(&reportOut, &reports[i], offsetof(FrameReport, gpuActiveRenderTimeUs));
    }

    return ToNvApi(result, "get_frame_reports");
}

NvAPI_Status InputReflex::Vulkan_SetLatencyMarker(HANDLE vkDevice,
                                                  NV_VULKAN_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams)
{
    if (!vkDevice || !pSetLatencyMarkerParams)
        return NVAPI_INVALID_ARGUMENT;

    MarkerParams markerParams {};
    markerParams.frame_id = pSetLatencyMarkerParams->frameID;
    markerParams.marker_type = (MarkerType) pSetLatencyMarkerParams->markerType; // requires enums to match

    return ToNvApi(InputCommon::set_marker(vulkanContext, (IUnknown*) vkDevice, markerParams), "set_marker");
}

NvAPI_Status InputReflex::Vulkan_NotifyOutOfBandVkQueue(HANDLE vkDevice, HANDLE queueHandle,
                                                        NV_VULKAN_OUT_OF_BAND_QUEUE_TYPE queueType)
{
    if (!vkDevice || !queueHandle)
        return NVAPI_INVALID_ARGUMENT;

    // Only the Reflex output has a use for it, the queue types match
    if (auto reflex = InputCommon::reflex_vk_output())
        reflex->notify_out_of_band((VkQueue) queueHandle, (VkOutOfBandQueueTypeNV) queueType);

    return NVAPI_OK;
}

#endif // LOW_LATENCY_INPUTS
