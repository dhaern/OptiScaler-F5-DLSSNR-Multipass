#include "pch.h"

#include "VirtualUpscalerDriver.h"

#include "DepthFinderCore.h"
#include "VirtualUpscalerMode.h"

#include <atomic>
#include <new>
#include <NVNGX_Parameter.h>
#include <State.h>
#include <Config.h>
#include <Util.h>
#include <upscalers/FeatureProvider_Dx12.h>
#include <upscalers/IFeature_Dx12.h>
#include <inputs/FG/Upscaler_Inputs_Dx12.h>
#include <dlssnr/DlssNr.h>
#include <dlssnr/DlssNrFeature_Dx12.h>

namespace native
{

namespace
{
// The feature the virtual upscaler is running now, for IsVirtualUpscalerFeature (read by the menu thread).
std::atomic<const void*> g_virtualFeature { nullptr };

// Our feature's handle id: never in the real NGX handle tables (HandleToFeature, Dx12Contexts), so no game call finds it. It is
// in State::changeBackend, so the menu's backend switch reaches it like any other feature.
constexpr UINT kVirtualHandleId = 0x4E524631; // 'NRF1'

// Releases a texture once the GPU can no longer be using it, by the same delay Util::DelayedDestroy gives features.
void ReleaseTexture(ID3D12Resource*& texture)
{
    Util::DelayedRelease(texture);
    texture = nullptr;
}

struct ReleaseLater
{
    explicit ReleaseLater(ID3D12Resource* resource) : resource(resource) {}
    ~ReleaseLater() { resource->Release(); }
    ReleaseLater(const ReleaseLater&) = delete;
    ReleaseLater& operator=(const ReleaseLater&) = delete;
    ID3D12Resource* resource;
};

void ReleaseObject(void* object) { static_cast<ID3D12Resource*>(object)->Release(); }

void DeleteFeature(void* object) { delete static_cast<IFeature_Dx12*>(object); }

void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}

bool IsUnrealGame()
{
    const auto& state = State::Instance();
    return state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL || state.gameEngine == GameEngineType::Unreal ||
           state.gameQuirks & GameQuirk::ForceUnrealEngine;
}

// The FSR and XeSS backends take each input in the state its *ResourceBarrier setting names (and hand it back in it), and in
// an Unreal game set colour and motion to RENDER_TARGET and UNORDERED_ACCESS on their first Evaluate. Setting those here,
// before UpscaleStart, makes frame generation's copies (which read the same settings) agree with the backend from the first
// frame. DLSS takes everything in its working state.
bool UsesBarrierSettings(Upscaler backend) { return backend != Upscaler::DLSS && backend != Upscaler::DLSSD; }

D3D12_RESOURCE_STATES SettingOr(const CustomOptional<int32_t, NoDefault>& setting, D3D12_RESOURCE_STATES working)
{
    return setting.has_value() ? (D3D12_RESOURCE_STATES) setting.value() : working;
}

Upscaler WantedBackend()
{
    const auto& cfg = *Config::Instance();
    const Upscaler wanted = cfg.Dx12Upscaler.has_value() ? cfg.Dx12Upscaler.value() : Upscaler::FFX;

    // Ray Reconstruction needs inputs we do not have; the config stores it as DLSS anyway.
    return wanted == Upscaler::DLSSD ? Upscaler::DLSS : wanted;
}

// The backends that read NGX's input subrect base: DLSS (NGX itself) and XeSS
// (upscalers/xess/XeSSFeature_Dx12.cpp:289-294). The FFX backend's dispatch only takes a render size, read from the
// texture's corner (upscalers/ffx/FFXFeature_Dx12.cpp:134), and the FSR 2 backends read no base either.
bool ReadsSubrectBase(Upscaler backend)
{
    return backend == Upscaler::DLSS || backend == Upscaler::DLSSD || backend == Upscaler::XeSS;
}

// Holds an SRWLOCK for a scope: no allocation, no TLS, no exceptions.
class SrwGuard
{
  public:
    SrwGuard(SRWLOCK& lock, bool exclusive) : _lock(lock), _exclusive(exclusive)
    {
        if (_exclusive)
            AcquireSRWLockExclusive(&_lock);
        else
            AcquireSRWLockShared(&_lock);
    }

    ~SrwGuard()
    {
        if (_exclusive)
            ReleaseSRWLockExclusive(&_lock);
        else
            ReleaseSRWLockShared(&_lock);
    }

    SrwGuard(const SrwGuard&) = delete;
    SrwGuard& operator=(const SrwGuard&) = delete;

  private:
    SRWLOCK& _lock;
    bool _exclusive;
};
} // namespace

VirtualUpscalerDriver::VirtualUpscalerDriver() = default;

bool IsVirtualUpscalerFeature(const void* feature) { return feature != nullptr && g_virtualFeature.load() == feature; }

bool WantedBackendReadsSubrectBase() { return ReadsSubrectBase(WantedBackend()); }

std::string VirtualUpscalerDriver::Error() const
{
    SrwGuard lock(_textLock, false);
    return _error;
}

std::string VirtualUpscalerDriver::BackendName() const
{
    SrwGuard lock(_textLock, false);
    return _backendName;
}

void VirtualUpscalerDriver::SetError(std::string error, bool refusal)
{
    _errorIsRefusal = refusal;
    SrwGuard lock(_textLock, true);
    _error = std::move(error);
}

void VirtualUpscalerDriver::ClearError()
{
    _errorIsRefusal = false;
    SrwGuard lock(_textLock, true);
    _error.clear();
}

void VirtualUpscalerDriver::SetBackendName(std::string name)
{
    std::string version;

    if (_feature != nullptr && !name.empty())
    {
        const auto v = _feature->Version();
        version = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    }

    SrwGuard lock(_textLock, true);
    _backendName = std::move(name);
    _backendVersion = std::move(version);
}

std::string VirtualUpscalerDriver::BackendVersion() const
{
    SrwGuard lock(_textLock, false);
    return _backendVersion;
}

VirtualUpscalerDriver::~VirtualUpscalerDriver()
{
    Release();
    LetGo(_output);
    LetGo(_input);
    LetGo(_neutralDepth);
    LetGo(_exposure);

    if (_rtvHeap != nullptr)
        _rtvHeap->Release();

    delete _params;
    delete _scaledParams;
}

void VirtualUpscalerDriver::DropFeature(bool destroyFgContext)
{
    auto& state = State::Instance();

    // What NVSDK_NGX_D3D12_ReleaseFeature (destroyFgContext) and FeatureProvider_Dx12::ChangeFeature (not) do for a game's
    // feature.
    if (state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
    {
        state.fgChanged = true;
        state.clearCapturedHudlesses = true;

        if (destroyFgContext)
            state.currentFG->DestroyFGContext();
    }

    UpscalerInputsDx12::Reset();

    if (state.currentFeature == _feature.get())
        state.currentFeature = nullptr;

    if (g_virtualFeature.load() == _feature.get())
        g_virtualFeature = nullptr;

    LetGo(std::move(_feature));
}

void VirtualUpscalerDriver::SetRetireSink(RetireFn fn, void* context)
{
    _retire = fn;
    _retireContext = fn != nullptr ? context : nullptr;
}

void VirtualUpscalerDriver::LetGo(ID3D12Resource*& texture)
{
    if (texture == nullptr)
        return;

    if (_retire != nullptr)
        _retire(_retireContext, Retired { ReleaseObject, texture });
    else
        Util::DelayedDestroy(std::make_unique<ReleaseLater>(texture));

    texture = nullptr;
}

void VirtualUpscalerDriver::LetGo(std::unique_ptr<IFeature_Dx12> feature)
{
    if (feature == nullptr)
        return;

    if (_retire != nullptr)
        _retire(_retireContext, Retired { DeleteFeature, feature.release() });
    else
        Util::DelayedDestroy(std::move(feature));
}

void VirtualUpscalerDriver::Release()
{
    if (_feature == nullptr)
        return;

    _paused = false;

    LOG_INFO("Virtual upscaler: releasing the {} backend", _backendName);
    DropFeature(true);
    State::Instance().changeBackend.erase(kVirtualHandleId);
    SetBackendName({});
}

void VirtualUpscalerDriver::RebuildAfterDiscardedList()
{
    auto& changes = State::Instance().changeBackend;
    if (const auto change = changes.find(kVirtualHandleId); _feature != nullptr && change != changes.end())
        change->second = true;
}

void VirtualUpscalerDriver::Pause()
{
    if (_feature == nullptr || _paused)
        return;

    _paused = true;

    auto& state = State::Instance();

    if (state.currentFeature == _feature.get())
        state.currentFeature = nullptr;

    LOG_INFO("Virtual upscaler: paused, the game is calling its own upscaler (the {} backend stays)", _backendName);
}

bool VirtualUpscalerDriver::CreateFeature(ID3D12GraphicsCommandList* cmd, const Key& key, Upscaler backend)
{
    // Each mode builds from its own block (see _scaledParams).
    NVNGX_Parameters* params = key.lowResMV ? _scaledParams : _params;

    vparams::Shape shape;
    shape.width = key.width;
    shape.height = key.height;
    shape.outWidth = key.outWidth;
    shape.outHeight = key.outHeight;
    shape.lowResMV = key.lowResMV;
    shape.depthReversed = key.depthReversed;
    shape.hdr = key.hdr;

    shape.fixedExposure = key.fixedExposure;
    vparams::SetCreate(*params, shape);

    // As TryCreateOptiFeature does: the menu and some backends read the API of the feature being run from here.
    State::Instance().api = API::DX12;

    std::unique_ptr<IFeature_Dx12> feature;

    // A backend library can throw (the FSR upscaler does on a size it cannot use): not into the game's present.
    try
    {
        if (!FeatureProvider_Dx12::GetFeature(backend, kVirtualHandleId, params, &feature) || feature == nullptr)
        {
            SetError(std::format("could not create the {} backend", UpscalerDisplayName(backend)));
            LOG_WARN("Virtual upscaler: {}", _error);
            return false;
        }

        if (!feature->Init(_device, cmd, params))
        {
            SetError(std::format("the {} backend failed to initialise", feature->Name()));
            LOG_WARN("Virtual upscaler: {}", _error);
            LetGo(std::move(feature));
            return false;
        }
    }
    catch (...)
    {
        SetError(std::format("the {} backend threw an exception while starting", UpscalerDisplayName(backend)));
        LOG_ERROR("Virtual upscaler: {}", _error);

        if (feature != nullptr)
            LetGo(std::move(feature));

        return false;
    }

    // The motion's size is decided when the feature is made (upscalers/IFeature.cpp:61-69): one that took RunScaled's
    // render-sized motion as output-sized would read it at the wrong place. ScaledKey already refuses the one user
    // setting that does this; this catches anything else.
    if (key.lowResMV && !feature->LowResMV())
    {
        SetError(
            std::format("the {} backend takes the motion as output-sized; it is render-sized here", feature->Name()));
        LOG_WARN("Virtual upscaler: {}", _error);
        LetGo(std::move(feature));
        return false;
    }

    _feature = std::move(feature);
    g_virtualFeature = _feature.get();
    ++_builds;
    return true;
}

bool VirtualUpscalerDriver::SameShape(const Key& a, const Key& b)
{
    return a.width == b.width && a.height == b.height && a.outWidth == b.outWidth && a.outHeight == b.outHeight &&
           a.format == b.format && a.outFormat == b.outFormat && a.lowResMV == b.lowResMV && a.backend == b.backend;
}

bool VirtualUpscalerDriver::EnsureFeature(ID3D12GraphicsCommandList* cmd, const Key& key, bool rebuild, bool realDepth)
{
    if (_feature != nullptr && !rebuild)
    {
        // A change of mode (Run, RunScaled) is a change of shape: rebuilt at once, never carried over.
        const bool flagsOnly = SameShape(key, _featureKey);

        // Built before the game's depth came, the direction was a guess: the first real depth corrects it at once
        if (!_settle.Now(_featureKey, key, flagsOnly, !_featureRealDepth && realDepth))
        {
            // A guess that was right: from now on a flip is a real one, and waits
            if (realDepth && key == _featureKey)
                _featureRealDepth = true;

            return true;
        }
    }

    // Tried and failed for exactly this: not again every frame.
    if (_feature == nullptr && !rebuild && _failed && key == _failedKey)
        return false;

    const bool replacing = _feature != nullptr;

    if (replacing)
    {
        LOG_INFO("Virtual upscaler: rebuilding the {} backend", _backendName);
        DropFeature(false);
    }

    const auto backend = (Upscaler) key.backend;

    // Like the real path's fallback when a chosen backend will not initialise; FFX needs nothing from the game.
    if (!CreateFeature(cmd, key, backend) && (backend == Upscaler::FFX || !CreateFeature(cmd, key, Upscaler::FFX)))
    {
        _failed = true;
        _failedKey = key;
        return false;
    }

    _featureRealDepth = realDepth;
    // Built because the mode changed (Run, RunScaled): its first frame resets, whichever mode it is.
    _modeSwitched = replacing && _featureKey.lowResMV != key.lowResMV;
    _freshFeature = true;
    _failed = false;
    auto& state = State::Instance();

    // GetFeature recorded what it built in Dx12Upscaler, which WantedBackend reads: the key matches it from the next frame on.
    _featureKey = key;
    _featureKey.backend = (int) WantedBackend();
    SetBackendName(_feature->Name());
    ClearError();

    state.changeBackend[kVirtualHandleId] = false;

    if (replacing && state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
        state.currentFG->UpdateTarget();

    // RunScaled's all-far depth is made with the backend, on a frame WouldRebuild told the caller about, not inside a
    // game's list on the first frame that comes without depth (made and cleared here whether this one has depth or
    // not). Run's is made as it always was, on its first frame without depth.
    if (key.lowResMV && NeutralDepth(cmd, key) == nullptr)
        LOG_WARN("Virtual upscaler: no neutral depth for frames without depth ({}); such a frame tries again", _error);

    if (key.lowResMV)
        LOG_INFO("Virtual upscaler: {} {} ready, {}x{} to {}x{}, depth reversed {}, HDR {}", _backendName,
                 _backendVersion, key.width, key.height, key.outWidth, key.outHeight, key.depthReversed, key.hdr);
    else
        LOG_INFO("Virtual upscaler: {} ready, {}x{}, depth reversed {}, HDR {}", _backendName, key.width, key.height,
                 key.depthReversed, key.hdr);

    return true;
}

bool VirtualUpscalerDriver::TextureMissing(const ID3D12Resource* texture, const Key& made, const Key& key)
{
    return texture == nullptr || made.width != key.width || made.height != key.height || made.format != key.format;
}

bool VirtualUpscalerDriver::EnsureTexture(ID3D12Resource*& texture, Key& made, const Key& key,
                                          D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES restState, const char* what)
{
    if (!TextureMissing(texture, made, key))
        return true;

    LetGo(texture);

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = key.width;
    desc.Height = key.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = key.format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;

    const HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, restState, nullptr,
                                                        IID_PPV_ARGS(&texture));

    if (FAILED(hr))
    {
        texture = nullptr;
        SetError(std::format("creating the {}x{} {} texture (format {}) failed: {:X}", key.width, key.height, what,
                             (int) key.format, (UINT) hr));
        LOG_WARN("Virtual upscaler: {}", _error);
        return false;
    }

    made = key;
    ++_builds;
    return true;
}

VirtualUpscalerDriver::Key VirtualUpscalerDriver::OutputKey(const Key& key)
{
    Key wanted = key;
    wanted.width = key.outWidth;
    wanted.height = key.outHeight;
    wanted.format = key.outFormat;
    return wanted;
}

ID3D12Resource* VirtualUpscalerDriver::NeutralDepth(ID3D12GraphicsCommandList* cmd, const Key& key)
{
    constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    Key wanted = key;
    wanted.format = DXGI_FORMAT_R32_FLOAT;
    ID3D12Resource* before = _neutralDepth;

    if (!EnsureTexture(_neutralDepth, _neutralKey, wanted, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RENDER_TARGET, "neutral depth"))
        return nullptr;

    if (_neutralDepth != before)
        _neutralFilled = false;

    if (_neutralFilled && _neutralReversed == key.depthReversed)
        return _neutralDepth;

    if (_rtvHeap == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;

        if (FAILED(_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&_rtvHeap))))
        {
            _rtvHeap = nullptr;
            SetError("creating the neutral depth's descriptor heap failed");
            return nullptr;
        }

        ++_builds;
    }

    if (_neutralFilled)
        Transition(cmd, _neutralDepth, kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const auto rtv = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
    _device->CreateRenderTargetView(_neutralDepth, nullptr, rtv);
    const float farDepth[4] = { key.depthReversed ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f };
    cmd->ClearRenderTargetView(rtv, farDepth, 0, nullptr);
    Transition(cmd, _neutralDepth, D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);

    _neutralFilled = true;
    _neutralReversed = key.depthReversed;
    return _neutralDepth;
}

bool VirtualUpscalerDriver::EnsureFixedExposure(ID3D12GraphicsCommandList* cmd)
{
    if (_exposure != nullptr)
        return true;
    if (_rtvHeap == nullptr)
        return false;
    Key key, made;
    key.width = key.height = 1;
    key.format = DXGI_FORMAT_R32_FLOAT;
    if (!EnsureTexture(_exposure, made, key, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RENDER_TARGET, "fixed exposure"))
        return false;
    const auto rtv = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
    _device->CreateRenderTargetView(_exposure, nullptr, rtv);
    const float exposure[4] { 1.0f, 0.0f, 0.0f, 0.0f };
    cmd->ClearRenderTargetView(rtv, exposure, 0, nullptr);
    Transition(cmd, _exposure, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

bool VirtualUpscalerDriver::Run(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame, ID3D12CommandQueue* queue,
                                DXGI_COLOR_SPACE_TYPE colorSpace, uint64_t epoch)
{
    // Whatever happens next, an early return included, the output texture is no longer RunScaled's.
    _outputOwner.EnterRun();
    return Upscale(cmd, frame, nullptr, RunNr { queue, colorSpace, epoch });
}

bool VirtualUpscalerDriver::RunScaled(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame,
                                      const ScaledTarget& target)
{
    return Upscale(cmd, frame, &target, RunNr {});
}

bool VirtualUpscalerDriver::Prepare(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame,
                                    const ScaledTarget& target)
{
    if (cmd == nullptr || frame.color == nullptr || frame.motion == nullptr)
        return false;

    if (!Start(cmd, true))
        return false;

    Key key;
    vparams::Subrect rect;

    if (const char* why = ScaledKey(frame, target, key, rect))
    {
        Refuse(why);
        return false;
    }

    _lastRefusal = nullptr;

    if (_errorIsRefusal)
        ClearError();

    // GetFeature tells the depth finder a game is calling an upscaler: not ours.
    SyntheticUpscalerCallScope guard;

    const auto nr = vparams::PlanNr(frame.color->GetDesc().Format != key.format, true,
                                    Config::Instance()->DlssNrEnabled.value_or_default(), target.allowNr,
                                    key.backend == (int) Upscaler::FFX);
    // Retire a changed crop with its feature: submitted lists may still reference the old copies.
    const bool changedInputs = _feature != nullptr && !_feature->InputBuffersReady(nr.copyIn ? _input : frame.color,
                                                                                   rect.width, rect.height, false);
    const bool switched = TakeBackendSwitch(key);

    // What Upscale does before it records anything, in the same order, with nothing evaluated.
    if (!EnsureFeature(cmd, key, switched || changedInputs, frame.depth != nullptr))
        return false;
    if (key.fixedExposure && !EnsureFixedExposure(cmd))
        return false;

    if ((rect.x != 0 || rect.y != 0) && !ReadsSubrectBase(_feature->GetUpscalerType()))
    {
        Refuse("the render subrect does not start at 0,0 and the backend reads no subrect base");
        return false;
    }

    if (!EnsureTexture(_output, _outputKey, OutputKey(key), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "output"))
        return false;

    if (!_feature->PostPassBuffersReady(_output, true))
    {
        SetError("RCAS's buffer for the output could not be made");
        return false;
    }

    if (nr.copyIn &&
        !EnsureTexture(_input, _inputKey, key, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, "input"))
        return false;

    if (!_feature->InputBuffersReady(nr.copyIn ? _input : frame.color, rect.width, rect.height, true))
    {
        SetError("the upscaler input copies could not be made");
        return false;
    }

    // Made with the backend already; again here only if that failed, and only for a frame without depth, as Upscale
    // does: a frame with depth can bring a depth direction the backend is not built for yet (a flip still settling),
    // and a clear for it would leave the next frame without depth to clear again, inside a game's list.
    if (frame.depth == nullptr && NeutralDepth(cmd, key) == nullptr)
        return false;

    return true;
}

ID3D12Resource* VirtualUpscalerDriver::Output() const { return _outputOwner.Scaled() ? _output : nullptr; }

VirtualUpscalerDriver::Key VirtualUpscalerDriver::PictureKey(const NativeFrame& frame,
                                                             const D3D12_RESOURCE_DESC& colorDesc) const
{
    Key key;
    key.width = (uint32_t) colorDesc.Width;
    key.height = colorDesc.Height;
    key.outWidth = key.width;
    key.outHeight = key.height;
    key.format = vparams::UavFormat(frame.colorFormat != DXGI_FORMAT_UNKNOWN ? frame.colorFormat : colorDesc.Format);
    key.outFormat = key.format;
    // A frame with no depth keeps the convention the backend was built with, so it is not rebuilt for it.
    key.depthReversed = frame.depth != nullptr || _feature == nullptr ? frame.depthReversed : _featureKey.depthReversed;
    key.hdr = frame.space == ColorSpace::ScRgb; // PQ is a perceptual encoding, as sRGB is: passed as it comes
    key.backend = (int) WantedBackend();
    return key;
}

const char* VirtualUpscalerDriver::ScaledKey(const NativeFrame& frame, const ScaledTarget& target, Key& key,
                                             vparams::Subrect& rect) const
{
    if (frame.color == nullptr || frame.motion == nullptr)
        return "the frame has no picture or no motion";

    const D3D12_RESOURCE_DESC colorDesc = frame.color->GetDesc();
    const D3D12_RESOURCE_DESC motionDesc = frame.motion->GetDesc();

    key = PictureKey(frame, colorDesc);
    key.outWidth = target.outputWidth;
    key.outHeight = target.outputHeight;
    key.outFormat = vparams::ScaledOutputFormat(target.outputFormat, key.format);
    key.lowResMV = true;
    key.fixedExposure = frame.space == ColorSpace::Srgb && vparams::SdrFormat(key.format);

    vparams::ScaledInputs in;
    in.colorWidth = key.width;
    in.colorHeight = key.height;
    in.motionWidth = (uint32_t) motionDesc.Width;
    in.motionHeight = motionDesc.Height;
    in.outputWidth = target.outputWidth;
    in.outputHeight = target.outputHeight;
    in.left = target.renderSubrect.left;
    in.top = target.renderSubrect.top;
    in.right = target.renderSubrect.right;
    in.bottom = target.renderSubrect.bottom;

    if (frame.depth != nullptr)
    {
        const D3D12_RESOURCE_DESC depthDesc = frame.depth->GetDesc();
        in.depthWidth = (uint32_t) depthDesc.Width;
        in.depthHeight = depthDesc.Height;
    }

    if (const char* why = vparams::CheckScaled(in, rect))
        return why;

    // [InitFlags] DisplayResolution=true makes every feature take its motion as output-sized
    // (upscalers/IFeature.cpp:61-64), whatever MVLowRes says: this motion is render-sized. Its other value (false) says
    // what MVLowRes already does.
    const auto& cfg = *Config::Instance();
    if (cfg.JitterCancellation.value_or(false))
        return "[InitFlags] JitterCancellation is on: the replaced FSR1 pass's motion has no jitter in it";

    if (cfg.DisplayResolution.value_or(false))
        return "[InitFlags] DisplayResolution is on: the backend would take the render-sized motion as output-sized";

    // An XeFG context made for display-sized motion would take this motion as such (see the header). Only while
    // frame generation is on: off, it reads no motion, and the feature built here saves HighResMV=false for the next
    // XeFG swapchain (upscalers/IFeature.cpp:94-103). activeFgOutput is fixed at start (dllmain.cpp:1931), so
    // currentFG is then an XeFG_Dx12, whose flags are those its swapchain context was made with (XeFG_Dx12.cpp:419-420,
    // 587-588; IFGFeature.cpp:169).
    const auto& state = State::Instance();
    const bool xefg = state.activeFgOutput == FGOutput::XeFG && cfg.IsFGEnabled();

    if (const char* why =
            vparams::CheckFrameGeneration(xefg, state.activeFgInput == FGInput::Upscaler,
                                          xefg && state.currentFG != nullptr && !state.currentFG->IsLowResMV()))
        return why;

    if ((rect.x != 0 || rect.y != 0) && !ReadsSubrectBase((Upscaler) key.backend))
        return "the render subrect does not start at 0,0 and the backend reads no subrect base";

    return nullptr;
}

void VirtualUpscalerDriver::Refuse(const char* why)
{
    // Once per reason, not every frame: the lock behind SetError is for a change.
    if (!_errorIsRefusal || why != _lastRefusal)
        SetError(why, true);

    if (why != _lastRefusal)
    {
        LOG_WARN("Virtual upscaler: not upscaling this frame: {}", why);
        _lastRefusal = why;
    }
}

bool VirtualUpscalerDriver::WouldRebuild(const NativeFrame& frame, const ScaledTarget& target) const
{
    Key key;
    vparams::Subrect rect;

    // RunScaled would refuse these before building anything.
    if (ScaledKey(frame, target, key, rect) != nullptr)
        return false;

    // Tried and failed for exactly this: RunScaled does not try again.
    if (_feature == nullptr)
        return !(_failed && key == _failedKey);

    const bool copyIn = vparams::PlanNr(frame.color->GetDesc().Format != key.format, true,
                                        Config::Instance()->DlssNrEnabled.value_or_default(), target.allowNr,
                                        key.backend == (int) Upscaler::FFX)
                            .copyIn;

    return BuildNeeded(frame, key, copyIn, rect) != nullptr;
}

const char* VirtualUpscalerDriver::BuildNeeded(const NativeFrame& frame, const Key& key, bool copyIn,
                                               const vparams::Subrect& rect) const
{
    // Start's RunScaled parameter block comes with the first backend built for RunScaled.
    if (_feature == nullptr || _scaledParams == nullptr)
        return "allowBuild is false and no backend is built for RunScaled yet";

    const auto& state = State::Instance();

    if (const auto change = state.changeBackend.find(kVirtualHandleId);
        change != state.changeBackend.end() && change->second)
        return "allowBuild is false and a backend switch is pending";

    // As EnsureFeature decides it.
    if (_settle.WouldNow(_featureKey, key, SameShape(key, _featureKey), !_featureRealDepth && frame.depth != nullptr))
        return "allowBuild is false and the backend would be built again for these inputs";

    // The textures Upscale makes without a rebuild, as it decides them (EnsureTexture, NeutralDepth).
    if (TextureMissing(_output, _outputKey, OutputKey(key)))
        return "allowBuild is false and the output texture would be made";

    if (key.fixedExposure && _exposure == nullptr)
        return "allowBuild is false and the fixed exposure would be made";

    if (copyIn && TextureMissing(_input, _inputKey, key))
        return "allowBuild is false and the copy of the picture would be made";

    if (!_feature->InputBuffersReady(copyIn ? _input : frame.color, rect.width, rect.height, false))
        return "allowBuild is false and the upscaler input copies would be made";

    // OptiScaler's RCAS after the backend makes its buffer on the first frame it runs for an output
    if (!_feature->PostPassBuffersReady(_output, false))
        return "allowBuild is false and RCAS's buffer would be made";

    Key neutralKey = key;
    neutralKey.format = DXGI_FORMAT_R32_FLOAT;
    // A frame without depth reads the neutral depth, cleared for the backend's depth direction (the key's then).
    if (frame.depth == nullptr && (TextureMissing(_neutralDepth, _neutralKey, neutralKey) || _rtvHeap == nullptr ||
                                   !_neutralFilled || _neutralReversed != key.depthReversed))
        return "allowBuild is false and the neutral depth would be made or cleared";

    return nullptr;
}

// Run when `target` is null: everything it sets and records is what Run did before RunScaled existed. RunScaled
// otherwise: the same steps, on its own parameter block, with the render subrect, without the copy back.
//
// Output() (RunScaled): on entry, if there is one, in UNORDERED_ACCESS; on exit, the same texture or a new one of the
// target's size, in UNORDERED_ACCESS, holding this frame's picture when this returns true (the backend's working
// states in between are recorded on `cmd` only).
bool VirtualUpscalerDriver::Upscale(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame,
                                    const ScaledTarget* target, const RunNr& run)
{
    if (cmd == nullptr || frame.color == nullptr || frame.motion == nullptr)
        return false;

    // RunScaled with allowBuild false: Start (which can make the parameter blocks) waits for the check below.
    const bool mayBuild = target == nullptr || target->allowBuild;

    if (mayBuild && !Start(cmd, target != nullptr))
        return false;

    // RunScaled's parameters never reach a Run call, nor Run's a RunScaled one: a block keeps what was set in it.
    NVNGX_Parameters* params = target != nullptr ? _scaledParams : _params;

    const D3D12_RESOURCE_DESC colorDesc = frame.color->GetDesc();

    Key key;
    vparams::Subrect rect;

    if (target == nullptr)
    {
        key = PictureKey(frame, colorDesc);

        // No usable size yet (a swapchain still being made or resized; the FSR backend throws below a few pixels): wait
        // for the next frame, build nothing.
        if (key.width < 16 || key.height < 16)
        {
            if (_zeroSizeFrames++ == 0)
                LOG_WARN("Virtual upscaler: the picture is {}x{}, too small, waiting for a real size", key.width,
                         key.height);

            return false;
        }

        _zeroSizeFrames = 0;
    }
    else
    {
        if (const char* why = ScaledKey(frame, *target, key, rect))
        {
            Refuse(why);
            return false;
        }
    }

    auto& state = State::Instance();
    auto& cfg = *Config::Instance();

    // A typed copy for the backend when the picture's format is not the one it reads, and for RunScaled whenever
    // DLSS-NR is on and allowed, so that DLSS-NR's pass before the upscale writes into our copy, never into the game's
    // picture (vparams::PlanNr). Run's choice is the format alone, as it always was, and DLSS-NR runs around it as it
    // always did. Decided once, before anything is made: the allowBuild check below and what follows see the same.
    const bool allowNr = target == nullptr || target->allowNr;
    const bool typedView = key.backend == (int) Upscaler::FFX;
    const auto nr = vparams::PlanNr(colorDesc.Format != key.format, target != nullptr,
                                    cfg.DlssNrEnabled.value_or_default(), allowNr, typedView);
    const bool copyIn = nr.copyIn;

    if (target != nullptr)
    {
        // allowBuild false: refused before anything is made, let go, taken or recorded, when anything would be. What
        // follows decides from the same key and plan on this thread, so it then makes nothing.
        if (!mayBuild)
        {
            if (const char* why = BuildNeeded(frame, key, copyIn, rect))
            {
                Refuse(why);
                return false;
            }
        }

        _lastRefusal = nullptr;

        if (_errorIsRefusal)
            ClearError();
    }

    // Our own call, for all of it: GetFeature and Evaluate both tell the depth finder a game is calling an upscaler.
    SyntheticUpscalerCallScope guard;

    // allowBuild false: a switch the menu asks for after the check above stays pending for Prepare.
    const bool rebuild = mayBuild && TakeBackendSwitch(key);

    if (!EnsureFeature(cmd, key, rebuild, frame.depth != nullptr))
        return false;
    if (key.fixedExposure && !EnsureFixedExposure(cmd))
        return false;

    // The backend built may not be the one asked for (a fallback to FFX, or the menu's switch).
    if (target != nullptr && (rect.x != 0 || rect.y != 0) && !ReadsSubrectBase(_feature->GetUpscalerType()))
    {
        Refuse("the render subrect does not start at 0,0 and the backend reads no subrect base");
        return false;
    }

    // What the backend writes: the picture's size and format for Run, the target's for RunScaled.
    if (!EnsureTexture(_output, _outputKey, OutputKey(key), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "output"))
        return false;

    // Run left it nobody's on entry.
    if (target != nullptr)
        _outputOwner.ScaledTextureReady();
    if (target != nullptr && target->output != nullptr)
        _outputOwner.EnterRun();

    if (copyIn && !EnsureTexture(_input, _inputKey, key, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, "input"))
        return false;

    // The backend and frame generation need depth, NR does not: with none, they get an all-far depth (render-sized) and
    // NR gets none.
    ID3D12Resource* depth = frame.depth != nullptr ? frame.depth : NeutralDepth(cmd, key);

    if (depth == nullptr)
        return false;

    // The game owns the optional UAV: borrow it for recording, never for retirement.
    struct BorrowOutput
    {
        ID3D12Resource*& output;
        ID3D12Resource* owned;
        ~BorrowOutput() { output = owned; }
    } const borrow { _output, _output };
    const auto outputRest =
        target != nullptr && target->output != nullptr ? target->outputState : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (target != nullptr && target->output != nullptr)
        _output = target->output;
    Transition(cmd, _output, outputRest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Frame generation's Upscaler input only has a device once NVSDK_NGX_D3D12_Init gave it one, which a game with no upscaler
    // never calls.
    UpscalerInputsDx12::Init(_device);

    const auto backend = _feature->GetUpscalerType();
    const bool usesSettings = UsesBarrierSettings(backend);

    // The picture is a render target in a D3D12 Unreal game; a D3D11 game's shared picture is not (no
    // ALLOW_RENDER_TARGET), and moving it to that state makes the list invalid.
    if (usesSettings && IsUnrealGame())
    {
        // Set either way: the FSR backends add the render-target state themselves for an Unreal game when it is unset.
        if (!cfg.ColorResourceBarrier.has_value())
            cfg.ColorResourceBarrier.set_volatile_value((colorDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0
                                                            ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                            : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (!cfg.MVResourceBarrier.has_value())
            cfg.MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Where the depth rests: the guides' state, our neutral depth's, or (RunScaled) the state the caller names.
    const auto depthRest = target != nullptr && frame.depth != nullptr ? target->depthState : kRead;

    // Frame generation's copies (UpscaleStart/UpscaleEnd) read the settings whatever the backend.
    const auto fgMotion = SettingOr(cfg.MVResourceBarrier, kRead);
    const auto fgDepth = SettingOr(cfg.DepthResourceBarrier, kRead);
    const auto fgOutput = SettingOr(cfg.OutputResourceBarrier, kWrite);

    const auto inColor = usesSettings ? SettingOr(cfg.ColorResourceBarrier, kRead) : kRead;
    const auto inMotion = usesSettings ? fgMotion : kRead;
    const auto inDepth = usesSettings ? fgDepth : kRead;
    const auto inOutput = usesSettings ? fgOutput : kWrite;
    const auto inExposure = usesSettings ? SettingOr(cfg.ExposureResourceBarrier, kRead) : kRead;

    // What NR does with this frame (VirtualUpscalerMode.h): nothing (off), runs on the picture in place with the
    // backend only counted, or runs around the backend's pass as for a game's call. RunScaled always runs the backend:
    // NR in place is a Run route only.
    const auto route = target != nullptr
                           ? VuNrRoute::BackendPass
                           : DecideVuNrRoute(cfg.DlssNrEnabled.value_or_default(),
                                             VuPassSettingFromIni(cfg.DlssNrNativeUpscalerPass.has_value(),
                                                                  cfg.DlssNrNativeUpscalerPass.value_or_default()));

    // NR on the picture itself comes first, before anything copies the picture: frame generation's HUD-less picture (the
    // copy into Output below, captured at UpscaleEnd) must be the one that is presented, NR included. Built from a picture
    // without NR, the generated frames would differ from the real ones in NR's changes, and the HUD fix would take those
    // changes for HUD. The picture is in the state it arrived in and the guides where the producer keeps them, as NR wants.
    bool nrRan = false;

    if (route == VuNrRoute::InPlace)
        nrRan = DlssNr::ApplyVirtualUpscalerInput(run.queue, cmd, frame.color, frame.depth, frame.motion,
                                                  frame.historyDistrust, frame.depthReversed, frame.reset,
                                                  run.colorSpace, frame.pictureState, run.epoch);
    else if (target == nullptr)
        DlssNr::NativeInputSkipped(); // NR off or on the backend's output: in place, its history starts over next time

    // Only when NR really ran: the status line says so, and otherwise shows why it did not (FinishedPictureStatus).
    _nrInPlace.store(nrRan, std::memory_order_relaxed);

    // The colour the backend reads: the picture itself, or a typed copy of it. For Run, the picture stays in
    // COPY_SOURCE until the result is copied back over it.
    ID3D12Resource* color = frame.color;
    D3D12_RESOURCE_STATES pictureNow = frame.pictureState;

    // [DlssNr] NativeStageTimings: each step's GPU time, under "upscaler".
    const auto mark = [&](const char* step)
    {
        if (frame.timer != nullptr)
            frame.timer->Mark(cmd, "upscaler", step);
    };

    mark("setup");

    if (copyIn)
    {
        Transition(cmd, frame.color, pictureNow, D3D12_RESOURCE_STATE_COPY_SOURCE);
        pictureNow = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd->CopyResource(_input, frame.color);
        Transition(cmd, _input, D3D12_RESOURCE_STATE_COPY_DEST, inColor);
        color = _input;
        mark("copy_in");
    }
    else
    {
        Transition(cmd, frame.color, pictureNow, inColor);
        pictureNow = inColor;
    }

    params->Set(NVSDK_NGX_Parameter_Color, color);
    if (target != nullptr)
        params->Set("OptiScaler.ColorFormat", (unsigned int) key.format);
    if (_featureKey.fixedExposure)
        Transition(cmd, _exposure, kRead, inExposure);
    params->Set(NVSDK_NGX_Parameter_ExposureTexture, _featureKey.fixedExposure ? _exposure : nullptr);
    params->Set(NVSDK_NGX_Parameter_Depth, frame.depth);
    params->Set(NVSDK_NGX_Parameter_MotionVectors, frame.motion);
    params->Set(NVSDK_NGX_Parameter_Output, _output);

    // A backend built for this call has no history to keep; RunScaled says so (a change of mode or size builds one),
    // and Run does on the first frame after RunScaled (otherwise Run passes the frame's own reset, as it always did).
    const bool reset =
        target != nullptr ? frame.reset || _freshFeature : frame.reset || (_freshFeature && _modeSwitched);

    if (target == nullptr)
        vparams::SetFrame(*params, reset);
    else
        vparams::SetScaledFrame(*params, rect, reset);

    const auto transitionInputs = [&](D3D12_RESOURCE_STATES motionBefore, D3D12_RESOURCE_STATES motionAfter,
                                      D3D12_RESOURCE_STATES depthBefore, D3D12_RESOURCE_STATES depthAfter,
                                      D3D12_RESOURCE_STATES outputBefore, D3D12_RESOURCE_STATES outputAfter)
    {
        if (target == nullptr)
        {
            Transition(cmd, frame.motion, motionBefore, motionAfter);
            Transition(cmd, depth, depthBefore, depthAfter);
            Transition(cmd, _output, outputBefore, outputAfter);
            return;
        }
        const D3D12_RESOURCE_TRANSITION_BARRIER transitions[] {
            { frame.motion, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, motionBefore, motionAfter },
            { depth, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, depthBefore, depthAfter },
            { _output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, outputBefore, outputAfter }
        };
        D3D12_RESOURCE_BARRIER barriers[3] {};
        UINT count = 0;
        for (const auto& transition : transitions)
            if (transition.StateBefore != transition.StateAfter)
            {
                barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[count++].Transition = transition;
            }
        if (count != 0)
            cmd->ResourceBarrier(count, barriers);
    };
    // From here the resources are where a game's would be at its call: in the states the backend takes them in.
    transitionInputs(kRead, inMotion, depthRest, inDepth, kWrite, inOutput);

    // DLSS-NR hangs off NVSDK_NGX_D3D12_EvaluateFeature around the upscaler, not off the feature: before it (Run before SR,
    // or the finished-picture capture of depth and motion) and after it (over Output). Not with the backend's pass off:
    // NR already ran on the picture itself, above, and would otherwise run twice.
    if (nr.before && route != VuNrRoute::InPlace) // RunScaled: only on a colour of ours; never without allowNr
        DlssNr::EvaluateBeforeUpscale(cmd, params);

    params->Set(NVSDK_NGX_Parameter_Depth, depth);
    mark("nr_before");

    transitionInputs(inMotion, fgMotion, inDepth, fgDepth, inOutput, fgOutput);

    // Back from a pause (the game called its own upscaler for a moment): frame generation's input changed, as when a
    // game switches features, which it takes as a short pause, not a new context
    if (_paused)
    {
        _paused = false;

        if (state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
        {
            state.fgChanged = true;
            state.clearCapturedHudlesses = true;
        }

        LOG_INFO("Virtual upscaler: resumed with the {} backend", _backendName);
    }

    state.currentFeature = _feature.get();

    // NR off, or NR on the picture itself: the call is here for frame generation, which takes motion and depth from it, not
    // for an upscale. The picture is already full size and antialiased, so the backend's pass (1.5-3 ms at 1440p-4K on a
    // mid-range card, more with FSR 4 or XeSS off Intel) is skipped and the picture goes on as the game made it, with NR's
    // changes when NR ran on it above. The HUD fix still reads Output as its HUD-less picture (UpscaleEnd, recorded before
    // the backend's pass would run): the picture is copied there for it, as it will be presented.
    const bool passThrough = route != VuNrRoute::BackendPass;
    _backendPass.store(!passThrough, std::memory_order_relaxed);

    if (passThrough && cfg.FGHUDFix.value_or_default())
    {
        const auto colorNow = color == _input ? inColor : pictureNow;
        Transition(cmd, color, colorNow, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(cmd, _output, fgOutput, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(_output, color);
        Transition(cmd, _output, D3D12_RESOURCE_STATE_COPY_DEST, fgOutput);
        Transition(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, colorNow);
    }

    if (target == nullptr)
    {
        UpscalerInputsDx12::UpscaleStart(cmd, params, _feature.get());
        UpscalerInputsDx12::UpscaleEnd(cmd, params, _feature.get());
    }
    else
    {
        // Frame generation takes RunScaled's motion and depth as render-sized because the feature is low-res
        // (inputs/FG/Upscaler_Inputs_Dx12.cpp:112-113, 175-179, 214, 232).
        static constinit std::atomic<bool> hudFixNoted {};
        if (Config::Instance()->FGHUDFix.value_or_default() && !hudFixNoted.exchange(true))
            LOG_INFO(
                "Spatial pass: FGHUDFix is not used with the replaced FSR1 pass; frame generation's HUD-less colour "
                "is taken after the game's sharpening pass");
        UpscalerInputsDx12::UpscaleStart(cmd, params, _feature.get(), false);
        UpscalerInputsDx12::UpscaleEnd(cmd, params, _feature.get(), false);
        mark("fg_inputs");
    }

    transitionInputs(fgMotion, inMotion, fgDepth, inDepth, fgOutput, inOutput);

    bool evaluated = false;

    if (passThrough)
    {
        // FSR frame generation waits for the feature's first counted frame
        _feature->CountFrame();
    }
    else
    {
        ScopedSkipHeapCapture skip {};
        try
        {
            evaluated = _feature->Evaluate(cmd, params);
        }
        catch (...)
        {
            LOG_ERROR("Virtual upscaler: the {} backend threw an exception while evaluating", _backendName);
            evaluated = false;
        }
    }

    params->Set(NVSDK_NGX_Parameter_Depth, frame.depth);
    mark("backend");

    if (evaluated)
    {
        _freshFeature = false;

        if (nr.after)
        {
            DlssNr::EvaluateAfterUpscale(cmd, params);
            mark("nr_after");
        }
    }

    if ((int) route != _routeLogged || _evaluations == 0)
    {
        _routeLogged = (int) route;
        LOG_INFO("Virtual upscaler: {}",
                 route == VuNrRoute::None    ? "Neural Rendering is off: the picture goes to frame generation as it is, "
                                               "no upscaler pass"
                 : route == VuNrRoute::InPlace ? "Neural Rendering runs on the game's picture; the backend only feeds "
                                                 "frame generation, no upscaler pass"
                                              : "upscaler pass on");
    }

    if (_evaluations < 8 || _evaluations % 300 == 0)
    {
        if (target == nullptr)
            LOG_INFO("Virtual upscaler: Evaluate {} -> {}, {} {}x{}, colour format {}{}, reset {}, depth reversed {}",
                     _evaluations,
                     route == VuNrRoute::None      ? "pass-through"
                     : route == VuNrRoute::InPlace ? (nrRan ? "no pass (NR in place)" : "no pass (NR did not run)")
                     : evaluated                   ? "true"
                                                   : "false",
                     _backendName, key.width, key.height, (int) key.format, copyIn ? " (copied)" : "", reset,
                     _featureKey.depthReversed);
        else
            LOG_INFO("Virtual upscaler: Evaluate {} -> {}, {} {}x{} (subrect {}x{} at {},{}) to {}x{}, colour format "
                     "{}{}, reset {}, depth reversed {}",
                     _evaluations, evaluated, _backendName, key.width, key.height, rect.width, rect.height, rect.x,
                     rect.y, key.outWidth, key.outHeight, (int) key.format, copyIn ? " (copied)" : "", reset,
                     _featureKey.depthReversed);
    }

    ++_evaluations;

    // The backend hands its inputs back in the states it took them in.
    transitionInputs(inMotion, kRead, inDepth, depthRest, inOutput, inOutput);

    if (copyIn)
        Transition(cmd, _input, inColor, D3D12_RESOURCE_STATE_COPY_DEST);

    if (evaluated && target == nullptr)
    {
        Transition(cmd, _output, inOutput, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(cmd, frame.color, pictureNow, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(frame.color, _output);
        Transition(cmd, frame.color, D3D12_RESOURCE_STATE_COPY_DEST, frame.pictureState);
        Transition(cmd, _output, D3D12_RESOURCE_STATE_COPY_SOURCE, kWrite);
        mark("copy_out");
    }
    else
    {
        // RunScaled leaves the result in Output(), the picture as it came.
        Transition(cmd, frame.color, pictureNow, frame.pictureState);
        Transition(cmd, _output, inOutput, kWrite);
    }

    if (_featureKey.fixedExposure)
        Transition(cmd, _exposure, inExposure, kRead);
    Transition(cmd, _output, kWrite, outputRest);
    return evaluated || passThrough;
}

bool VirtualUpscalerDriver::Start(ID3D12GraphicsCommandList* cmd, bool scaled)
{
    if (_device == nullptr)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;

        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
        {
            SetError("could not get the D3D12 device from the command list");
            return false;
        }

        _device = device.Get(); // not kept alive by us: lives as long as the game's device
    }

    if (_params == nullptr)
        _params = new NVNGX_Parameters(API::DX12, false);

    if (scaled && _scaledParams == nullptr)
    {
        _scaledParams = new (std::nothrow) NVNGX_Parameters(API::DX12, false);

        if (_scaledParams == nullptr)
        {
            SetError("could not allocate the parameter block");
            return false;
        }
    }

    return true;
}

bool VirtualUpscalerDriver::TakeBackendSwitch(Key& key)
{
    // A backend switch from the menu, or a backend asking to be rebuilt (FeatureProvider_Dx12::ChangeFeature's job for
    // a game's feature).
    auto& state = State::Instance();
    const auto change = state.changeBackend.find(kVirtualHandleId);

    if (_feature == nullptr || change == state.changeBackend.end() || !change->second)
        return false;

    if (state.newBackend != Upscaler::Reset)
    {
        key.backend = (int) (state.newBackend == Upscaler::DLSSD ? Upscaler::DLSS : state.newBackend);
        state.newBackend = Upscaler::Reset;
    }

    change->second = false;
    return true;
}

} // namespace native
