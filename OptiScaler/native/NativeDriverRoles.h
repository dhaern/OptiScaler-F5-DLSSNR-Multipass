#pragma once

// Who owns native::NativeDriver's producer and virtual upscaler, decided from the configuration alone: a pure table, so
// a host test can go through every combination (tests/nr_native_driver_roles_smoke.cpp). Two owners can ask for them:
// - the present path, [DlssNr] NativeMotion: Acquire on the presented picture, the producer's Run, and its consumer
//   (DLSS-NR's native input, or the virtual upscaler's Run with NativeUpscaler). Unchanged by the in-list pass.
// - the in-list pass, [Upscalers] ReplaceGameFsr1 on D3D12: the producer and the virtual upscaler, recorded into a
//   game's own list where it upscales with FSR1. It needs no [DlssNr] key, and DLSS-NR never runs on it.
// The install gates of the hooks that pass relies on are here too, read once when the device is made.

namespace native::roles
{

struct Keys
{
    bool nativeMotion = false;    // [DlssNr] NativeMotion
    bool nativeInput = false;     // [DlssNr] NativeInput
    bool nativeUpscaler = false;  // [DlssNr] NativeUpscaler
    bool replaceGameFsr1 = false; // [Upscalers] ReplaceGameFsr1
    bool inListApi = false;       // the driver is D3D12's, the only API the in-list pass exists for
};

struct Roles
{
    bool presentPath = false; // the present path runs, as it did before the in-list pass existed
    bool inList = false;      // the in-list pass may use the producer and the virtual upscaler
    bool enabled = false;     // either one: otherwise the virtual upscaler is released and the status is Off

    // The virtual upscaler is made and kept; otherwise it is released.
    bool keepVirtualUpscaler = false;
    // The present path's consumer: the virtual upscaler's Run, or else DLSS-NR's native input. Only NativeUpscaler
    // decides it: the in-list pass keeping the virtual upscaler does not take DLSS-NR away from the present path.
    bool presentUsesVirtualUpscaler = false;
    // The present path gives its frame to that consumer (NativeProducer::Options::apply).
    bool presentApplies = false;

    // The in-list pass alone: a present only makes the producer for the device, it reads nothing and runs nothing.
    bool inListOnly = false;
};

constexpr Roles Decide(const Keys& k)
{
    Roles r;
    r.presentPath = k.nativeMotion;
    r.inList = k.replaceGameFsr1 && k.inListApi;
    r.enabled = r.presentPath || r.inList;
    r.keepVirtualUpscaler = k.nativeUpscaler || r.inList;
    r.presentUsesVirtualUpscaler = k.nativeUpscaler;
    r.presentApplies = k.nativeUpscaler || k.nativeInput;
    r.inListOnly = r.inList && !r.presentPath;
    return r;
}

// The D3D12 depth finder's hooks: the depth the in-list pass reads is the finder's copy of this frame.
constexpr bool DepthFinderWanted(bool nativeDepthFinder, bool replaceGameFsr1)
{
    return nativeDepthFinder || replaceGameFsr1;
}

// Whether the D3D12 depth finder takes its copies: for the depth overlay, the present path, or the in-list pass.
constexpr bool DepthSnapshotsWanted(bool depthOverlay, bool nativeMotion, bool replaceGameFsr1)
{
    return depthOverlay || nativeMotion || replaceGameFsr1;
}

} // namespace native::roles
