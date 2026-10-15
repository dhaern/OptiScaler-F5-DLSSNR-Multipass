#pragma once

// Presents native input (the producer's picture, and the depth finder's depth and the optical flow as guides) to a real
// OptiScaler upscaler backend as a synthetic Evaluate call, for a game that never calls one. Everything keyed off a live
// upscaler feature -- the menu's status, backend switching and, above all, frame generation with the Upscaler input -- then
// works as it does for a game's own call. An alternative to DLSS-NR's native input (DlssNr::ApplyNativeInput); the two are
// mutually exclusive.
//
// The call follows the real one (inputs/NVNGX_DLSS_Dx12.cpp, NVSDK_NGX_D3D12_EvaluateFeature and TryEvaluateOptiFeature):
// DLSS-NR's EvaluateBeforeUpscale, currentFeature, UpscalerInputsDx12::UpscaleStart and UpscaleEnd (which feed frame
// generation), Evaluate, then DLSS-NR's EvaluateAfterUpscale. It must run before frame generation presents, on the game's
// queue: native/NativeDriverDx12.cpp drives it from FGHooks::FGPresent when frame generation owns the swapchain.
//
// Jitter is zero and render size equals output size: there is no upscaling. With NR off the backend's pass is skipped (the feature is only counted, for frame generation);
// with NR on it is skipped too and NR runs on the game's picture in place (DlssNr::ApplyVirtualUpscalerInput), unless the
// [DlssNr] NativeUpscalerPass is true (native/VirtualUpscalerMode.h).
//
// The feature is kept out of the real NGX handle tables. Like a game's own feature it makes FeatureProvider_Dx12 record the
// backend it built in Config::Dx12Upscaler, which is also the backend it builds: the user's choice, FFX when there is none.

#include "NativeProducer.h"
#include "VirtualUpscalerHold.h"
#include "VirtualUpscalerParams.h"

#include <d3d12.h>
#include <dxgiformat.h>

#include <atomic>
#include <memory>
#include <string>

class IFeature_Dx12;
struct NVNGX_Parameters;
enum class Upscaler;

namespace native
{

// True when `feature` (State::currentFeature) is Optical F5Low's own virtual upscaler, not a game's.
bool IsVirtualUpscalerFeature(const void* feature);

bool WantedBackendReadsSubrectBase();

struct ScaledTarget
{
    uint32_t outputWidth = 0, outputHeight = 0; // the size the backend writes (the game's EASU output)
    D3D12_RECT renderSubrect {};                // the part of frame.color that is the picture (FsrEasuCon viewport)

    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    bool allowNr = true; // A scaled NR pass may write only a private input copy, never the game picture.

    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource* output = nullptr; // Optional, borrowed full-size typed UAV; never owned or retired by the driver.
    D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; // preserved around RunScaled

    // False: refuse before allocating, retiring, or recording when Prepare is needed.
    bool allowBuild = true;
};

struct Retired
{
    void (*destroy)(void* object) = nullptr;
    void* object = nullptr;
};
using RetireFn = void (*)(void* context, Retired retired);

class VirtualUpscalerDriver
{
  public:
    VirtualUpscalerDriver();
    ~VirtualUpscalerDriver();

    VirtualUpscalerDriver(const VirtualUpscalerDriver&) = delete;
    VirtualUpscalerDriver& operator=(const VirtualUpscalerDriver&) = delete;

    // A NativeProducer::ApplyFn: one synthetic upscaler call on `frame`, recorded on `cmd`, with the picture written back in
    // place. True when the backend evaluated, or when it was only counted (Neural Rendering off, or NR on the picture in
    // place: VirtualUpscalerMode.h). `queue`, `colorSpace` and `epoch` are what DlssNr::ApplyNativeInput takes for the
    // same frame; NR on the picture in place needs them.
    bool Run(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame, ID3D12CommandQueue* queue,
             DXGI_COLOR_SPACE_TYPE colorSpace, uint64_t epoch);

    // Input states are restored; motion rests in NON_PIXEL_SHADER_RESOURCE, depth in target.depthState.
    // The caller restores list bindings and resets history after any skipped frame.
    bool RunScaled(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame, const ScaledTarget& target);

    // Submit on the game queue (and order every consuming queue after it) before RunScaled with allowBuild=false.
    bool Prepare(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame, const ScaledTarget& target);

    // Owned by the driver, valid until the next call; UAV on entry/exit. Null until RunScaled, and on entering Run.
    // Also null when RunScaled borrows target.output; that texture's outputState is preserved.
    ID3D12Resource* Output() const;

    bool WouldRebuild(const NativeFrame& frame, const ScaledTarget& target) const;

    void RebuildAfterDiscardedList();

    // Drops the backend the way a game releasing its feature does: frame generation is told, currentFeature is cleared, and the
    // feature is destroyed once the GPU can no longer be using it. Nothing happens when there is none.
    void Release();

    // The game is calling its own upscaler for now (VirtualUpscalerHold.h): the backend and frame generation's context
    // stay, only currentFeature lets go of it. The next Run resumes, telling frame generation its input changed.
    void Pause();

    // A sink owns deferred destruction until all lists referencing the objects have reached their fences.
    void SetRetireSink(RetireFn fn, void* context);

    bool Active() const { return _feature != nullptr; }

    // NR ran on the game's picture itself in the last frame and the backend does not process it: it only feeds frame
    // generation. Read by the menu thread for the status line.
    bool NrInPlace() const { return _nrInPlace.load(std::memory_order_relaxed); }
    // The backend's pass ran over the picture in the last frame ([DlssNr] NativeUpscalerPass true). Read by the menu thread.
    bool BackendPass() const { return _backendPass.load(std::memory_order_relaxed); }
    uint64_t Builds() const { return _builds; }

    std::string Error() const;
    std::string BackendName() const;
    std::string BackendVersion() const;

  private:
    struct Key
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t outWidth = 0; // output: Run's is the render size
        uint32_t outHeight = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;    // typed and UAV-capable: the input copy's
        DXGI_FORMAT outFormat = DXGI_FORMAT_UNKNOWN; // the output's: Run's is `format`
        bool lowResMV = false; // RunScaled's: its motion is render-sized (Run's guides are output-sized)
        bool depthReversed = false;
        bool hdr = false;
        bool fixedExposure = false;
        int backend = 0; // an Upscaler

        bool operator==(const Key&) const = default;
    };

    static bool SameShape(const Key& a, const Key& b);

    // What Run's NR on the picture in place takes (DlssNr::ApplyVirtualUpscalerInput); RunScaled passes none.
    struct RunNr
    {
        ID3D12CommandQueue* queue = nullptr;
        DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        uint64_t epoch = 0;
    };
    bool Upscale(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame, const ScaledTarget* target,
                 const RunNr& run);
    Key PictureKey(const NativeFrame& frame, const D3D12_RESOURCE_DESC& colorDesc) const;
    const char* ScaledKey(const NativeFrame& frame, const ScaledTarget& target, Key& key, vparams::Subrect& rect) const;
    static Key OutputKey(const Key& key);
    bool Start(ID3D12GraphicsCommandList* cmd, bool scaled);
    bool TakeBackendSwitch(Key& key);
    void LetGo(ID3D12Resource*& texture);
    void LetGo(std::unique_ptr<IFeature_Dx12> feature);
    void Refuse(const char* why);
    void SetError(std::string error, bool refusal = false);
    void ClearError();
    void SetBackendName(std::string name);

    // `realDepth`: this frame has the game's depth (not the all-far stand-in).
    bool EnsureFeature(ID3D12GraphicsCommandList* cmd, const Key& key, bool rebuild, bool realDepth);
    bool CreateFeature(ID3D12GraphicsCommandList* cmd, const Key& key, Upscaler backend);
    void DropFeature(bool destroyFgContext);
    static bool TextureMissing(const ID3D12Resource* texture, const Key& made, const Key& key);
    bool EnsureTexture(ID3D12Resource*& texture, Key& made, const Key& key, D3D12_RESOURCE_FLAGS flags,
                       D3D12_RESOURCE_STATES restState, const char* what);

    ID3D12Resource* NeutralDepth(ID3D12GraphicsCommandList* cmd, const Key& key);
    bool EnsureFixedExposure(ID3D12GraphicsCommandList* cmd);
    const char* BuildNeeded(const NativeFrame& frame, const Key& key, bool copyIn, const vparams::Subrect& rect) const;

    ID3D12Device* _device = nullptr;
    NVNGX_Parameters* _params = nullptr; // one block for the driver's life: features read it at Init only
    NVNGX_Parameters* _scaledParams = nullptr; // RunScaled's own block: its render subrect must never reach a Run call
    std::unique_ptr<IFeature_Dx12> _feature;
    Key _featureKey;
    std::string _backendName;
    std::string _backendVersion;
    bool _failed = false;
    Key _failedKey;
    bool _paused = false;
    bool _featureRealDepth = false;   // the backend was built on a frame with the game's depth (its direction is known)
    bool _freshFeature = false;       // the backend built last has not evaluated yet (RunScaled asks it for a reset)
    bool _modeSwitched = false;       // and it replaced one built for the other mode (Run asks for a reset then too)
    hold::RebuildSettle<Key> _settle; // a flip of the depth direction or HDR must hold before a rebuild

    ID3D12Resource* _output = nullptr; // rests in UNORDERED_ACCESS
    Key _outputKey;
    vparams::OutputOwner _outputOwner; // whether _output is RunScaled's (Output())
    ID3D12Resource* _input = nullptr; // a typed copy of the picture when the picture itself is typeless; rests in COPY_DEST
    Key _inputKey;
    ID3D12Resource* _neutralDepth = nullptr; // all far, for frames with no depth; rests in NON_PIXEL_SHADER_RESOURCE
    Key _neutralKey;
    bool _neutralFilled = false;
    bool _neutralReversed = false;
    ID3D12DescriptorHeap* _rtvHeap = nullptr;
    ID3D12Resource* _exposure = nullptr; // 1x1 R32_FLOAT, fixed 1.0, rests in NON_PIXEL_SHADER_RESOURCE

    RetireFn _retire = nullptr;
    void* _retireContext = nullptr;
    uint64_t _builds = 0;

    uint64_t _evaluations = 0;
    int _routeLogged = -1; // the VuNrRoute the log last said (-1: none yet)
    std::atomic<bool> _nrInPlace { false };
    std::atomic<bool> _backendPass { false };
    uint32_t _zeroSizeFrames = 0;
    const char* _lastRefusal = nullptr; // RunScaled's last reason not to go on, logged when it changes
    bool _errorIsRefusal = false;       // _error is that reason, cleared by RunScaled's next evaluation
    std::string _error;
    mutable SRWLOCK _textLock = SRWLOCK_INIT; // _error and _backendName against Error()/BackendName() on other threads
};

} // namespace native
