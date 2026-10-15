#include "pch.h"
// Standalone: cl /std:c++20 /EHsc /W4 /O2 /I tests\virtual_upscaler_stubs /I external\nvngx_dlss_sdk
// tests\nr_virtual_upscaler_scaled_gpu.cpp OptiScaler\native\VirtualUpscalerDriver.cpp
// OptiScaler\native\DepthFinderCore.cpp Real backends: compile this TU with VUD_REAL_CONTROL into a scratch DLL; run:
// gpu.exe real <backend> <DLL> [padding].
#include "../OptiScaler/native/VirtualUpscalerDriver.h"
#include "../OptiScaler/native/DepthFinderCore.h"

#ifdef VUD_REAL_CONTROL
#include <Config.h>
#include <State.h>
#include <upscalers/IFeature_Dx12.h>

namespace
{
struct Driver
{
    std::unique_ptr<native::VirtualUpscalerDriver> vud = std::make_unique<native::VirtualUpscalerDriver>();
    std::vector<native::Retired> retired;
};
unsigned phase = 0, preparedCopies = 0, recordedCopies = 0;
void Retire(void* context, native::Retired object) { static_cast<Driver*>(context)->retired.push_back(object); }
} // namespace
void VudTestCroppedCopy()
{
    if (phase == 1)
        ++preparedCopies;
    else
        ++recordedCopies;
}
extern "C" __declspec(dllexport) void* VudTestCreate(const char* backend)
{
    auto& cfg = *Config::Instance();
    cfg.FGEnabled = false;
    cfg.DlssNrEnabled = false;
    cfg.DisplayResolution = false;
    cfg.OutputScalingEnabled = false;
    cfg.OverlayMenu = true;
    cfg.RcasEnabled = false;
    cfg.FsrNonLinearPQ = false;
    cfg.FsrNonLinearSRGB = false;
    cfg.FsrNonLinearColorSpace = false;
    cfg.Dx12Upscaler = strcmp(backend, "xess") == 0    ? Upscaler::XeSS
                       : strcmp(backend, "fsr21") == 0 ? Upscaler::FSR21
                       : strcmp(backend, "fsr22") == 0 ? Upscaler::FSR22
                                                       : Upscaler::FFX;
    if (strcmp(backend, "fsr4") == 0)
    {
        cfg.Fsr4ForceModel = FSR4Support::INT8;
        cfg.Fsr4Preset = 0;
    }
    State::Instance().api = API::DX12;
    preparedCopies = recordedCopies = 0;
    auto* driver = new Driver;
    driver->vud->SetRetireSink(Retire, driver);
    return driver;
}
extern "C" __declspec(dllexport) uint64_t VudTestCall(void* object, ID3D12GraphicsCommandList* cmd,
                                                      const native::NativeFrame* frame,
                                                      const native::ScaledTarget* target, unsigned operation)
{
    auto& vud = *static_cast<Driver*>(object)->vud;
    phase = operation == 0 ? 1 : 2;
    if (operation == 0)
        return vud.Prepare(cmd, *frame, *target);
    if (operation == 1)
        return vud.RunScaled(cmd, *frame, *target);
    if (operation == 2)
        return vud.WouldRebuild(*frame, *target);
    if (operation == 3)
        return vud.Builds();
    if (operation == 4)
        return preparedCopies;
    if (operation == 6)
        return State::Instance().currentFeature->AutoExposure();
    return recordedCopies;
}
extern "C" __declspec(dllexport) ID3D12Resource* VudTestOutput(void* object)
{
    return static_cast<Driver*>(object)->vud->Output();
}
extern "C" __declspec(dllexport) bool VudTestVersion(void* object, const char* expected)
{
    auto& vud = *static_cast<Driver*>(object)->vud;
    printf("backend: %s %s\n", vud.BackendName().c_str(), vud.BackendVersion().c_str());
    return vud.BackendVersion().starts_with(expected);
}
extern "C" __declspec(dllexport) void VudTestClose(void* object)
{
    auto* driver = static_cast<Driver*>(object);
    // GPU is idle; the sink retains all objects until driver destruction has finished.
    driver->vud.reset();
    for (const auto& r : driver->retired)
        r.destroy(r.object);
    delete driver;
}
#else
#include "virtual_upscaler_stubs/VudStubs.h"
#include "d3d12_harness.h"
#include <cmath>
#include <DirectXPackedVector.h>

using namespace d3d12_test;
namespace
{
constexpr UINT rw = 640, rh = 360, ow = 960, oh = 540;
constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
Checks checks;
void Barrier(Gpu& gpu, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    const auto b = Transition(resource, before, after);
    gpu.list->ResourceBarrier(1, &b);
}
void Retire(void* context, native::Retired r) { static_cast<std::vector<native::Retired>*>(context)->push_back(r); }
void Stub(Gpu& gpu)
{
    auto color = Texture(gpu.device.Get(), rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, read);
    auto motion = Texture(gpu.device.Get(), rw, rh, DXGI_FORMAT_R16G16_FLOAT, read);
    auto depth = Texture(gpu.device.Get(), rw, rh, DXGI_FORMAT_R32_FLOAT, read);
    if (!checks.Check(color && motion && depth, "scaled inputs"))
        return;
    native::NativeFrame frame;
    frame.color = color.Get();
    frame.colorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    frame.pictureState = read;
    frame.motion = motion.Get();
    frame.depth = depth.Get();
    native::ScaledTarget target;
    target.outputWidth = ow;
    target.outputHeight = oh;
    target.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    target.allowNr = false;
    target.allowBuild = false;
    g_config.DlssNrEnabled = true;
    std::vector<native::Retired> retired;
    {
        native::VirtualUpscalerDriver vud;
        vud.SetRetireSink(Retire, &retired);
        checks.Check(vud.WouldRebuild(frame, target) && !vud.RunScaled(gpu.list.Get(), frame, target) &&
                         vud.Builds() == 0 && VudStubs::g.getFeature == 0,
                     "unprepared recording refuses without building");
        checks.Check(vud.Prepare(gpu.list.Get(), frame, target) && gpu.ExecuteAndWait(), "Prepare submitted");
        checks.Check(!vud.WouldRebuild(frame, target) && vud.Output() == nullptr && VudStubs::g.evaluated == 0 &&
                         VudStubs::g.upscaleStart == 0,
                     "Prepare evaluates and feeds nothing");
        const auto builds = vud.Builds();
        checks.Check(vud.RunScaled(gpu.list.Get(), frame, target) && gpu.ExecuteAndWait() && vud.Builds() == builds,
                     "prepared recording makes nothing");
        auto* output = vud.Output();
        checks.Check(output && output->GetDesc().Width == ow && output->GetDesc().Height == oh &&
                         output->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM,
                     "output has requested size and format");
        checks.Check(VudStubs::g.nrBefore == 0 && VudStubs::g.nrAfter == 0 && VudStubs::g.upscaleStart == 1,
                     "allowNr=false blocks NR even when enabled; FG is fed");
        frame.depth = nullptr;
        checks.Check(!vud.WouldRebuild(frame, target) && vud.RunScaled(gpu.list.Get(), frame, target) &&
                         gpu.ExecuteAndWait() && vud.Builds() == builds,
                     "neutral depth prewarmed, no recording allocation");
        const auto features = VudStubs::g.getFeature, feeds = VudStubs::g.upscaleStart;
        const size_t held = retired.size();
        g_state.changeBackend[0x4E524631] = true;
        g_state.newBackend = Upscaler::FSR22;
        checks.Check(vud.WouldRebuild(frame, target) && !vud.RunScaled(gpu.list.Get(), frame, target) &&
                         vud.Builds() == builds && VudStubs::g.getFeature == features &&
                         VudStubs::g.upscaleStart == feeds && retired.size() == held &&
                         g_state.changeBackend[0x4E524631] && g_state.newBackend == Upscaler::FSR22,
                     "pending backend switch preserved, no build, retirement, or FG feed on recording thread");
        checks.Check(gpu.ExecuteAndWait() && vud.Prepare(gpu.list.Get(), frame, target) && gpu.ExecuteAndWait() &&
                         !g_state.changeBackend[0x4E524631] && g_state.newBackend == Upscaler::Reset &&
                         VudStubs::g.getFeature == features + 1,
                     "Prepare takes switch and retires old backend");
        checks.Check(!retired.empty() && VudStubs::g.delayed == 0, "fence-owned retirement sink receives old objects");
        target.outputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        checks.Check(vud.WouldRebuild(frame, target) && !vud.RunScaled(gpu.list.Get(), frame, target),
                     "format change needs Prepare");
        checks.Check(vud.Prepare(gpu.list.Get(), frame, target) && gpu.ExecuteAndWait() &&
                         vud.RunScaled(gpu.list.Get(), frame, target) && gpu.ExecuteAndWait() &&
                         vud.Output()->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
                     "prepared output format change");
        checks.Check(!vud.Run(nullptr, frame) && vud.Output() == nullptr,
                     "early-return Run clears scaled output ownership");
        checks.Check(!native::GameUpscalerCalledRecently(), "synthetic backend call is not a game upscaler call");
    }
    checks.Check(gpu.WaitIdle(), "retirement fence reached");
    for (const auto& r : retired)
        r.destroy(r.object);
    VudStubs::g_kept.clear();
    checks.Check(VudStubs::g.featuresAlive == 0, "all backend lifetimes released once");
}

struct Real
{
    HMODULE module = nullptr;
    void* (*create)(const char*) = nullptr;
    uint64_t (*call)(void*, ID3D12GraphicsCommandList*, const native::NativeFrame*, const native::ScaledTarget*,
                     unsigned) = nullptr;
    ID3D12Resource* (*output)(void*) = nullptr;
    bool (*version)(void*, const char*) = nullptr;
    void (*close)(void*) = nullptr;
    bool Load(const char* path)
    {
        module = LoadLibraryA(path);
        if (!module)
            return false;
        create = reinterpret_cast<decltype(create)>(GetProcAddress(module, "VudTestCreate"));
        call = reinterpret_cast<decltype(call)>(GetProcAddress(module, "VudTestCall"));
        output = reinterpret_cast<decltype(output)>(GetProcAddress(module, "VudTestOutput"));
        version = reinterpret_cast<decltype(version)>(GetProcAddress(module, "VudTestVersion"));
        close = reinterpret_cast<decltype(close)>(GetProcAddress(module, "VudTestClose"));
        return create && call && output && version && close;
    }
};
double Pattern(double x, double y, unsigned frame)
{
    constexpr double pi = 3.14159265358979323846;
    const double u = x - 2.3 * frame;
    return 0.5 + 0.25 * sin(2 * pi * u / 9.7) + 0.12 * sin(2 * pi * u / 23.1) + 0.06 * sin(2 * pi * y / 13.0);
}
void Upload(Gpu& gpu, ID3D12Resource* texture, const Bytes& data, UINT bytesPerPixel)
{
    const auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 size = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
    auto buffer = Buffer(gpu.device.Get(), size, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t* mapped = nullptr;
    if (!checks.Check(buffer && SUCCEEDED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "map upload"))
        return;
    for (UINT y = 0; y < desc.Height; ++y)
        memcpy(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
               data.data() + size_t(y) * desc.Width * bytesPerPixel, size_t(desc.Width) * bytesPerPixel);
    buffer->Unmap(0, nullptr);
    Barrier(gpu, texture, read, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = buffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(gpu, texture, D3D12_RESOURCE_STATE_COPY_DEST, read);
    checks.Check(gpu.ExecuteAndWait(), "upload completed before release");
}
double RunReal(Gpu& gpu, Real& real, const char* backend, UINT padding, bool motionOn, bool hdr)
{
    const UINT w = rw + padding, h = rh + padding;
    const auto format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const UINT colorBytes = hdr ? 8 : 4;
    auto color = Texture(gpu.device.Get(), w, h, hdr ? format : DXGI_FORMAT_R8G8B8A8_TYPELESS, read);
    auto depth = Texture(gpu.device.Get(), w, h, DXGI_FORMAT_R32_FLOAT, read);
    auto motion = Texture(gpu.device.Get(), w, h, DXGI_FORMAT_R32G32_FLOAT, read);
    if (!checks.Check(color && depth && motion, "real scaled resources"))
        return 1e9;
    Bytes bytes(size_t(w) * h * sizeof(float));
    for (size_t i = 0; i < size_t(w) * h; ++i)
    {
        const float value = 0.5f;
        memcpy(bytes.data() + i * sizeof(float), &value, sizeof(value));
    }
    Upload(gpu, depth.Get(), bytes, 4);
    bytes.resize(size_t(w) * h * 8);
    for (size_t i = 0; i < size_t(w) * h; ++i)
    {
        const float mv[2] { motionOn ? -2.3f / 1.5f : 0.0f, 0.0f };
        memcpy(bytes.data() + i * 8, mv, 8);
    }
    Upload(gpu, motion.Get(), bytes, 8);
    native::NativeFrame frame;
    frame.color = color.Get();
    frame.colorFormat = format;
    frame.space = hdr ? native::ColorSpace::ScRgb : native::ColorSpace::Srgb;
    frame.pictureState = read;
    frame.depth = depth.Get();
    frame.motion = motion.Get();
    native::ScaledTarget target;
    target.outputWidth = ow;
    target.outputHeight = oh;
    target.renderSubrect = { 0, 0, LONG(rw), LONG(rh) };
    target.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    target.allowNr = target.allowBuild = false;
    void* driver = real.create(backend);
    const auto call = [&](unsigned op) { return real.call(driver, gpu.list.Get(), &frame, &target, op); };
    const bool ready =
        checks.Check(call(0) && gpu.ExecuteAndWait() && !call(2), "real backend prepared before recording");
    const char* expected = strcmp(backend, "ffx") == 0     ? "3.1.5"
                           : strcmp(backend, "fsr4") == 0  ? "4."
                           : strcmp(backend, "fsr21") == 0 ? "2.1.2"
                           : strcmp(backend, "fsr22") == 0 ? "2.2.1"
                                                           : "2.";
    checks.Check(real.version(driver, expected),
                 "loaded backend version matches requested backend (no silent FSR4 substitution)");
    const auto builds = call(3);
    double sum = 0;
    uint64_t samples = 0;
    for (unsigned f = 0; ready && f < 40; ++f)
    {
        bytes.resize(size_t(w) * h * colorBytes);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                const auto v =
                    uint8_t(std::lround(std::clamp(Pattern((x + 0.5) * 1.5, (y + 0.5) * 1.5, f), 0.0, 1.0) * 255));
                auto* pixel = bytes.data() + (size_t(y) * w + x) * colorBytes;
                if (hdr)
                {
                    const uint16_t c = DirectX::PackedVector::XMConvertFloatToHalf(v / 255.0f);
                    const uint16_t rgba[4] { c, c, c, 0x3c00 };
                    memcpy(pixel, rgba, sizeof(rgba));
                }
                else
                {
                    pixel[0] = pixel[1] = pixel[2] = v;
                    pixel[3] = 255;
                }
            }
        Upload(gpu, color.Get(), bytes, colorBytes);
        if (!checks.Check(call(1) && gpu.ExecuteAndWait(), "real RunScaled with render-sized motion"))
            break;
        if (f >= 32)
        {
            Bytes result;
            if (!checks.Check(real.output(driver) &&
                                  gpu.Readback(real.output(driver), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, result),
                              "read scaled UAV output"))
                break;
            for (UINT y = 8; y < oh - 8; ++y)
                for (UINT x = 24; x < ow - 8; ++x)
                {
                    const double wanted = std::lround(std::clamp(Pattern(x + 0.5, y + 0.5, f), 0.0, 1.0) * 255);
                    sum += std::abs(result[(size_t(y) * ow + x) * 4] - wanted);
                    ++samples;
                }
        }
    }
    Bytes untouched;
    checks.Check(gpu.Readback(color.Get(), read, untouched) && untouched == bytes,
                 "RunScaled leaves the complete game input, including padding, unchanged");
    checks.Check(samples == uint64_t(8) * (oh - 16) * (ow - 32), "all measured frames completed");
    checks.Check(call(3) == builds, "no driver builds during real recording");
    printf("%s colour: AutoExposure %llu\n", hdr ? "HDR" : "SDR", call(6));
    checks.Check(call(6) == uint64_t(hdr), "SDR uses fixed exposure, HDR keeps AutoExposure");
    if (padding && strcmp(backend, "fsr4") == 0)
    {
        printf("cropped copies: prepare %llu, recording %llu\n", call(4), call(5));
        checks.Check(call(4) == 2 && call(5) == 0,
                     "both alternating FSR4 crops allocated in Prepare, neither in recording");
    }
    checks.Check(gpu.WaitIdle(), "real backend retirement fence");
    real.close(driver);
    return samples ? sum / double(samples) : 1e9;
}
} // namespace
int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const bool warp = (argc > 1 && strcmp(argv[1], "warp") == 0) || (argc > 5 && strcmp(argv[5], "warp") == 0);
    Real real;
    Gpu gpu;
    if (!checks.Check(gpu.Create(warp ? Adapter::Warp : Adapter::Hardware), "debug-layer D3D12 device"))
        return checks.Summary();
    if (argc > 3 && strcmp(argv[1], "real") == 0)
    {
        if (checks.Check(real.Load(argv[3]), "instrumented DLL with real production backend"))
        {
            const UINT padding = argc > 4 ? UINT(atoi(argv[4])) : 0;
            const bool hdr = argc > 5 && strcmp(argv[5], "hdr") == 0;
            const double right = RunReal(gpu, real, argv[2], padding, true, hdr);
            printf("render-pixel MV mean error: %.3f / 255\n", right);
            checks.Check(right < 20, "scaled output follows moving pattern");
            if (strcmp(argv[2], "ffx") == 0 && !padding)
            {
                const double wrong = RunReal(gpu, real, argv[2], padding, false, hdr);
                printf("no-MV mean error: %.3f / 255\n", wrong);
                checks.Check(right < 0.75 * wrong, "render-pixel motion beats absent motion by at least 25 percent");
            }
        }
    }
    else
        Stub(gpu);
    checks.Check(gpu.Messages(D3D12_MESSAGE_SEVERITY_ERROR) == 0, "debug layer: zero errors");
    if (real.module)
    {
        checks.Check(gpu.WaitIdle(), "proxy process cleanup: queue idle, driver objects already retired");
        const int code = checks.Summary();
        // Match the replacement harness: proxy debug producers and the device live until process exit.
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), UINT(code));
        return code;
    }
    return checks.Summary();
}
#endif
