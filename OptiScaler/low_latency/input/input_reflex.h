#pragma once

#include <nvapi/NvApiTypes.h>
#include "input_common.h"

#ifdef LOW_LATENCY_INPUTS
#include <vulkan/vulkan.h>
#endif

// The game's Reflex as a low latency input. The D3D calls that reach the nvapi hooks come in here: the game's own are
// told apart from OptiScaler's Streamline (DLSS frame generation output), InputCommon learns the Reflex input from them
// (sleep mode, markers, which input is active), and ReflexHooks' router delivers them as before. Until InputCommon's
// outputs take over from the router, the game's Reflex runs no output of InputCommon's (InputCommon::routed_input).
class InputReflex
{
    const static inline InputContext inputContext { .caller = LowLatencyInput::Reflex,
                                                    .noFrameId = false,
                                                    .markerMode = InputMarkerMode::FullMarkers };

#ifdef LOW_LATENCY_INPUTS
    // nvapi's Vulkan Reflex, deprecated for VK_NV_low_latency2 (InputReflexVk)
    const static inline InputContext vulkanContext { .caller = LowLatencyInput::Reflex,
                                                     .noFrameId = false,
                                                     .markerMode = InputMarkerMode::FullMarkers,
                                                     .api = API::Vulkan };

    // The game waits on it after each sleep
    static inline VkSemaphore vkSemaphore = VK_NULL_HANDLE;
#endif

  public:
    // The input's entry point for a hooked nvapi interface, nullptr for the ones it does not take
    static void* ForInterface(unsigned int interfaceId);

    static NvAPI_Status D3D_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    static NvAPI_Status D3D_Sleep(IUnknown* pDev);
    static NvAPI_Status D3D_GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams);
    static NvAPI_Status D3D_SetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);
    static NvAPI_Status D3D12_SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue,
                                                  NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams);

#ifdef LOW_LATENCY_INPUTS
    // Vulkan
    static NvAPI_Status Vulkan_InitLowLatencyDevice(HANDLE vkDevice, HANDLE* signalSemaphoreHandle);
    static NvAPI_Status Vulkan_DestroyLowLatencyDevice(HANDLE vkDevice);
    static NvAPI_Status Vulkan_GetSleepStatus(HANDLE vkDevice,
                                              NV_VULKAN_GET_SLEEP_STATUS_PARAMS* pGetSleepStatusParams);
    static NvAPI_Status Vulkan_SetSleepMode(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    static NvAPI_Status Vulkan_Sleep(HANDLE vkDevice, NvU64 signalValue);
    static NvAPI_Status Vulkan_GetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams);
    static NvAPI_Status Vulkan_SetLatencyMarker(HANDLE vkDevice,
                                                NV_VULKAN_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);
    static NvAPI_Status Vulkan_NotifyOutOfBandVkQueue(HANDLE vkDevice, HANDLE queueHandle,
                                                      NV_VULKAN_OUT_OF_BAND_QUEUE_TYPE queueType);
#endif
};
