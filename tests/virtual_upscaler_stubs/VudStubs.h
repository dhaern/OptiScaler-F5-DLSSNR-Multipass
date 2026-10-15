#pragma once

// Stand-ins for the OptiScaler pieces native/VirtualUpscalerDriver.cpp includes, so a test can build that file alone
// with a real D3D12 device (tests/nr_virtual_upscaler_prepare_gpu.cpp). Each header of the same name here includes this
// one; the test puts this folder first on the include path. The backend is a fake that counts what is made, evaluated
// and destroyed; DLSS-NR's two passes only count their calls; Util::DelayedDestroy keeps what it is given until exit.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <nvsdk_ngx_defs.h>

#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace native
{
void NoteGameUpscalerCall();
}

namespace VudStubs
{
struct Counters
{
    int getFeature = 0; // backends asked for
    int featuresAlive = 0;
    int evaluated = 0;
    int nrBefore = 0; // DlssNr::EvaluateBeforeUpscale
    int nrAfter = 0;  // DlssNr::EvaluateAfterUpscale
    int delayed = 0;  // Util::DelayedDestroy
    int upscaleStart = 0;
    bool verbose = false;
};

inline Counters g;
inline std::vector<std::shared_ptr<void>> g_kept; // what DelayedDestroy was given, freed at exit

inline void Log(const char* level, const std::string& text)
{
    if (g.verbose)
        std::printf("  [%s] %s\n", level, text.c_str());
}
} // namespace VudStubs

#define LOG_INFO(...) VudStubs::Log("info", std::format(__VA_ARGS__))
#define LOG_WARN(...) VudStubs::Log("warn", std::format(__VA_ARGS__))
#define LOG_ERROR(...) VudStubs::Log("error", std::format(__VA_ARGS__))

enum API
{
    NotSelected,
    DX11,
    DX12,
    Vulkan
};

enum class Upscaler
{
    XeSS,
    FSR21,
    FSR22,
    FFX,
    DLSS,
    DLSSD,
    Reset
};

inline std::string UpscalerDisplayName(Upscaler upscaler, API = NotSelected)
{
    return upscaler == Upscaler::FFX ? "FFX (fake)" : std::format("backend {} (fake)", (int) upscaler);
}

enum class FGInput : uint32_t
{
    NoFG,
    Upscaler
};

enum class FGOutput : uint32_t
{
    NoFG,
    XeFG
};

enum class GameEngineType : uint32_t
{
    Other,
    Unreal
};

enum class GameQuirk : uint64_t
{
    ForceUnrealEngine
};

struct QuirkSet
{
    bool operator&(GameQuirk) const { return false; }
};

struct NoDefault
{
};

template <typename T, typename Tag = NoDefault> class CustomOptional
{
  public:
    CustomOptional() = default;
    CustomOptional(T def) : _def(def) {}

    bool has_value() const { return _value.has_value(); }
    T value() const { return *_value; }
    T value_or(T other) const { return _value.value_or(other); }
    T value_or_default() const { return _value.value_or(_def); }
    void set_volatile_value(T v) { _value = v; }
    void reset() { _value.reset(); }

    CustomOptional& operator=(T v)
    {
        _value = v;
        return *this;
    }

  private:
    std::optional<T> _value;
    T _def {};
};

struct Config
{
    CustomOptional<Upscaler> Dx12Upscaler;
    CustomOptional<bool> DisplayResolution;
    CustomOptional<bool> FGEnabled { false };
    CustomOptional<bool> DlssNrEnabled { false };
    CustomOptional<int32_t, NoDefault> ColorResourceBarrier;
    CustomOptional<int32_t, NoDefault> MVResourceBarrier;
    CustomOptional<int32_t, NoDefault> DepthResourceBarrier;
    CustomOptional<int32_t, NoDefault> OutputResourceBarrier;
    CustomOptional<int32_t, NoDefault> ExposureResourceBarrier;

    static Config* Instance();
};

inline Config g_config;
inline Config* Config::Instance() { return &g_config; }

struct FakeFG
{
    bool IsLowResMV() { return true; }
    void DestroyFGContext() {}
    void UpdateTarget() {}
};

class IFeature_Dx12;

struct State
{
    NVSDK_NGX_EngineType NVNGX_Engine = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    GameEngineType gameEngine = GameEngineType::Other;
    QuirkSet gameQuirks;
    FakeFG* currentFG = nullptr;
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::NoFG;
    bool fgChanged = false;
    bool clearCapturedHudlesses = false;
    bool skipHeapCapture = false;
    IFeature_Dx12* currentFeature = nullptr;
    std::unordered_map<unsigned int, bool> changeBackend;
    Upscaler newBackend = Upscaler::Reset;
    API api = NotSelected;

    static State& Instance();
};

inline State g_state;
inline State& State::Instance() { return g_state; }

class ScopedSkipHeapCapture
{
  public:
    ScopedSkipHeapCapture() : _previous(State::Instance().skipHeapCapture) { State::Instance().skipHeapCapture = true; }
    ~ScopedSkipHeapCapture() { State::Instance().skipHeapCapture = _previous; }

  private:
    bool _previous;
};

namespace Util
{
template <typename T> void DelayedDestroy(std::unique_ptr<T> ptr)
{
    ++VudStubs::g.delayed;
    VudStubs::g_kept.emplace_back(ptr.release(), [](void* p) { delete static_cast<T*>(p); });
}
} // namespace Util

struct NVNGX_Parameters
{
    NVNGX_Parameters(API, bool) {}
    void Set(const char*, unsigned int) {}
    void Set(const char*, int) {}
    void Set(const char*, float) {}
    void Set(const char*, ID3D12Resource*) {}
};

// The fake backend: builds and evaluates nothing on the GPU; counts its life.
class IFeature_Dx12
{
  public:
    explicit IFeature_Dx12(Upscaler upscaler) : _upscaler(upscaler) { ++VudStubs::g.featuresAlive; }
    virtual ~IFeature_Dx12() { --VudStubs::g.featuresAlive; }

    bool Init(ID3D12Device*, ID3D12GraphicsCommandList*, NVNGX_Parameters*) { return true; }

    bool Evaluate(ID3D12GraphicsCommandList*, NVNGX_Parameters*)
    {
        native::NoteGameUpscalerCall(); // as the real call path does: the driver's scope must hide it
        ++VudStubs::g.evaluated;
        return true;
    }

    std::string Name() const { return UpscalerDisplayName(_upscaler); }
    Upscaler GetUpscalerType() const { return _upscaler; }
    bool LowResMV() { return true; }
    bool PostPassBuffersReady(ID3D12Resource*, bool) { return true; }
    bool InputBuffersReady(ID3D12Resource*, uint32_t, uint32_t, bool) { return true; }
    struct Version_
    {
        unsigned int major = 1, minor = 0, patch = 0;
    };
    Version_ Version() { return {}; }

  private:
    Upscaler _upscaler;
};

struct FeatureProvider_Dx12
{
    static bool GetFeature(Upscaler upscaler, UINT, NVNGX_Parameters*, std::unique_ptr<IFeature_Dx12>* feature)
    {
        native::NoteGameUpscalerCall();
        ++VudStubs::g.getFeature;
        *feature = std::make_unique<IFeature_Dx12>(upscaler);
        return true;
    }
};

struct UpscalerInputsDx12
{
    static void Init(ID3D12Device*) {}
    static void Reset() {}
    static void UpscaleStart(ID3D12GraphicsCommandList*, NVNGX_Parameters*, IFeature_Dx12*)
    {
        ++VudStubs::g.upscaleStart;
    }
    static void UpscaleEnd(ID3D12GraphicsCommandList*, NVNGX_Parameters*, IFeature_Dx12*) {}
};

namespace DlssNr
{
inline void EvaluateBeforeUpscale(ID3D12GraphicsCommandList*, NVNGX_Parameters*) { ++VudStubs::g.nrBefore; }
inline void EvaluateAfterUpscale(ID3D12GraphicsCommandList*, NVNGX_Parameters*) { ++VudStubs::g.nrAfter; }
} // namespace DlssNr
