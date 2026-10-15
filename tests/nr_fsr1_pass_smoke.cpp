// cl /nologo /std:c++20 /EHsc /W4 /O2 tests\nr_fsr1_pass_smoke.cpp /Fe:pass.exe
#include <cstdio>
#if __has_include("../OptiScaler/native/Fsr1Pass.h")
#include "../OptiScaler/native/Fsr1Pass.h"
#include <bit>
#include <cstring>
using namespace native::fsr1;
using native::descriptors::Kind;
static int failures = 0;
static void Check(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
    failures += !ok;
}
static D3D12_RESOURCE_DESC Texture(UINT width, UINT height)
{
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    return d;
}
static Facts Raster()
{
    Facts f;
    f.shader.pass = Pass::Easu;
    f.shader.stage = Stage::Pixel;
    f.shader.textures = 1;
    f.input.resource = reinterpret_cast<ID3D12Resource*>(1);
    f.output.resource = reinterpret_cast<ID3D12Resource*>(2);
    f.input.kind = uint8_t(Kind::Srv);
    f.output.kind = uint8_t(Kind::Rtv);
    f.input.dimension = f.output.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    f.input.sliceCount = f.output.sliceCount = 1;
    f.input.format = f.output.format = f.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    f.inputDesc = Texture(64, 32);
    f.outputDesc = Texture(128, 64);
    f.outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    f.inputStateKnown = true;
    f.inputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    f.viewportValid = f.scissorValid = true;
    f.viewport = { 0, 0, 128, 64, 0, 1 };
    f.scissor = { 0, 0, 128, 64 };
    f.count = 3;
    return f;
}
static void Easu(Facts& f, float width, float height)
{
    const float x = width / 128, y = height / 64;
    const float iw = 1.0f / float(f.inputDesc.Width), ih = 1.0f / float(f.inputDesc.Height);
    const float c[16] { x, y, 0.5f * x - 0.5f, 0.5f * y - 0.5f, iw, ih, iw, -ih, -iw, 2 * ih, iw, 2 * ih, 0, 4 * ih,
                        0, 0 };
    std::memcpy(f.constants, c, sizeof(c));
    f.constantCount = 16;
}
int main()
{
    Plan p {};
    auto f = Raster();
    Easu(f, 64, 32);
    Check(Resolve(f, &p) == Refusal::None && p.constantsUsed && p.inputRect.right == 64 && p.box.right == 128 &&
              p.ratio == 2,
          "raster_cbv: mapped EASU constants and exact input/output plan");
    f = Raster();
    f.shape = Shape::Dispatch;
    f.shader.stage = Stage::Compute;
    f.shader.uber = true;
    f.shader.uavs = 1;
    f.shader.numthreads[0] = f.shader.numthreads[1] = 8;
    f.shader.numthreads[2] = 1;
    f.output.kind = uint8_t(Kind::Uav);
    f.outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    f.groups[0] = 16;
    f.groups[1] = 8;
    f.groups[2] = 1;
    float sizes[4] { 64, 32, 128, 64 };
    std::memcpy(f.constants, sizes, sizeof(sizes));
    f.constantCount = 6;
    Check(Resolve(f, &p) == Refusal::None && p.shape == Shape::Dispatch && p.constantsUsed && p.inputRect.bottom == 32,
          "compute_uber: root sizes and dispatch plan");
    f.shader.uber = false;
    f.inputDesc = Texture(80, 40);
    Easu(f, 64, 32);
    Check(Resolve(f, &p) == Refusal::None && p.constantsUsed && p.inputRect.right == 64 && p.inputDesc.Width == 80,
          "compute_table: padded texture refined by table CBV");
    struct Rejection
    {
        Refusal why;
        void (*change)(Facts&);
    };
    const Rejection rejections[] {
        { Refusal::NoTls, +[](Facts& f) { f.noTls = true; } },
        { Refusal::GameUpscaler, +[](Facts& f) { f.gameUpscaler = true; } },
        { Refusal::Settings, +[](Facts& f) { f.overlayMenu = false; } },
        { Refusal::Indirect, +[](Facts& f) { f.indirect = true; } },
        { Refusal::Bundle, +[](Facts& f) { f.listType = D3D12_COMMAND_LIST_TYPE_BUNDLE; } },
        { Refusal::NotDirect, +[](Facts& f) { f.listType = D3D12_COMMAND_LIST_TYPE_COMPUTE; } },
        { Refusal::Predicated, +[](Facts& f) { f.predicated = true; } },
        { Refusal::Blend, +[](Facts& f) { f.blend = true; } },
        { Refusal::WriteMask, +[](Facts& f) { f.writeMask = 7; } },
        { Refusal::RenderTargets, +[](Facts& f) { f.boundTargets = 2; } },
        { Refusal::DepthWrite, +[](Facts& f) { f.depthStencilWrite = true; } },
        { Refusal::InputUnresolved, +[](Facts& f) { f.input.resource = nullptr; } },
        { Refusal::InputState, +[](Facts& f) { f.inputStateKnown = false; } },
        { Refusal::EnhancedBarriers, +[](Facts& f) { f.enhanced = true; } },
        { Refusal::AfterBundle, +[](Facts& f) { f.afterBundle = true; } },
        { Refusal::RenderPass, +[](Facts& f) { f.inRenderPass = true; } },
        { Refusal::OutputUnresolved, +[](Facts& f) { f.output.resource = nullptr; } },
        { Refusal::Bindless, +[](Facts& f) { f.bindless = true; } },
        { Refusal::Ambiguous, +[](Facts& f) { f.ambiguous = true; } },
        { Refusal::RectUnknown,
          +[](Facts& f)
          {
              f.constantCount = 0;
              f.inputDesc.Height = 24;
          } },
        { Refusal::Ratio, +[](Facts& f) { Easu(f, 30, 15); } },
        { Refusal::Offset,
          +[](Facts& f)
          {
              f.inputDesc = Texture(80, 40);
              Easu(f, 64, 32);
              float x = std::bit_cast<float>(f.constants[2]) + 1;
              f.constants[2] = std::bit_cast<uint32_t>(x);
          } },
        { Refusal::InputShape, +[](Facts& f) { f.inputDesc.MipLevels = 2; } },
        { Refusal::OutputShape, +[](Facts& f) { f.outputDesc.SampleDesc.Count = 4; } },
        { Refusal::ViewFormat, +[](Facts& f) { f.rtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; } },
        { Refusal::Srgb, +[](Facts& f) { f.input.format = f.inputDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; } },
        { Refusal::Viewport, +[](Facts& f) { f.viewport.TopLeftX = 1; } },
        { Refusal::Scissor, +[](Facts& f) { f.scissor.right = 127; } },
        { Refusal::UberRcas,
          +[](Facts& f)
          {
              f.shader.uber = true;
              f.constantCount = 0;
              f.inputDesc = Texture(128, 64);
          } },
        { Refusal::Incomplete, +[](Facts& f) { f.tracked = false; } },
        { Refusal::DrawShape, +[](Facts& f) { f.count = 12; } },
        { Refusal::DispatchGrid,
          +[](Facts& f)
          {
              f.shape = Shape::Dispatch;
              f.shader.stage = Stage::Compute;
              f.shader.uavs = 1;
              f.shader.numthreads[0] = f.shader.numthreads[1] = 8;
              f.shader.numthreads[2] = 1;
              f.groups[0] = 15;
              f.groups[1] = 8;
              f.groups[2] = 1;
              f.output.kind = uint8_t(Kind::Uav);
              f.outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
          } },
        { Refusal::Constants,
          +[](Facts& f)
          {
              f.inputDesc = Texture(80, 40);
              Easu(f, 64, 32);
              f.inputDesc = Texture(64, 32);
          } },
        { Refusal::Pipeline, +[](Facts& f) { f.shader.pass = Pass::None; } }
    };
    bool covered[size_t(Refusal::Count)] {};
    for (const auto& test : rejections)
    {
        f = Raster();
        Easu(f, 64, 32);
        test.change(f);
        const Plan before = p;
        const auto actual = Resolve(f, &p);
        Check(actual == test.why && std::memcmp(&p, &before, sizeof(p)) == 0, Name(test.why));
        if (actual != test.why)
            std::printf("      got %s\n", Name(actual));
        covered[size_t(test.why)] = true;
    }
    for (size_t i = 1; i < size_t(Refusal::Count); ++i)
        if (!covered[i])
            Check(false, Name(Refusal(i)));
    std::printf("refusal coverage: %zu/%zu\n", std::size(rejections), size_t(Refusal::Count) - 1);
    f = Raster();
    Check(Resolve(f, &p) == Refusal::None && !p.constantsUsed, "unreadable constants: uniform texture geometry");
    f.constants[0] = 0xFFFFFFFF;
    f.constantCount = 16;
    Check(Resolve(f, &p) == Refusal::None && !p.constantsUsed, "readable non-EASU constants: optional refinement");
    f = Raster();
    f.shape = Shape::DrawIndexed;
    f.count = 6;
    Check(Resolve(f, &p) == Refusal::None && p.shape == Shape::DrawIndexed, "privatevtable: indexed full-screen pass");
    f = Raster();
    f.outputDesc = Texture(160, 80);
    f.outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    Easu(f, 64, 32);
    Check(Resolve(f, &p) == Refusal::None && p.box.right == 128 && p.box.bottom == 64,
          "viewport smaller than target: decode against written geometry");
    std::printf("%s: FSR1 pass (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
#else
int main()
{
    std::puts("FAIL: generic FSR1 pass resolver is not implemented");
    return 1;
}
#endif
