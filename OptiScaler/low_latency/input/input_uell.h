#pragma once

#include "input_common.h"
#include "UeInputRule.h"

typedef void (*PFN_FLTickExternal)(int64_t frameId, float DeltaSeconds, bool bIdleMode);
typedef void (*PFN_CameraUpdateExternal)(int64_t frameId, float cameraPosition[3],
                                         float cameraRotation[3], // Pitch, Yaw, Roll
                                         float fovAngle);
typedef void (*PFN_setTickCallback)(PFN_FLTickExternal callback);
typedef void (*PFN_setCameraUpdateExternal)(PFN_CameraUpdateExternal callback);

class InputUeLowLatency
{
    // The game's tick thread
    inline static ueinput::Memory memory {};
    inline static std::atomic<int64_t> drivenFrame = INT64_MIN; // the frame whose start went to the output

    inline static InputContext inputContext { .caller = LowLatencyInput::UeLowLatency,
                                              .localContext = false,
                                              .noFrameId = false,
                                              .markerMode = InputMarkerMode::SimOnly };

  public:
    static void tickEnd(int64_t frameId, float DeltaSeconds, bool bIdleMode);
    static void tickStart(int64_t frameId, float DeltaSeconds, bool bIdleMode);
    static void cameraUpdate(int64_t frameId, float cameraPosition[3], float cameraRotation[3], float fovAngle);
};
