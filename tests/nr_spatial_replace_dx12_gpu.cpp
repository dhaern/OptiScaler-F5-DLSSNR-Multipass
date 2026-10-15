// GPU harness of [Upscalers] ReplaceGameFsr1 end to end on the real hooks: a small D3D12 "game" loads the built
// OptiScaler.dll as its dxgi.dll and, each frame, draws a moving pattern and its depth at render size into its input,
// runs FSR1's EASU in one of the shapes real games use, and presents:
//   raster_cbv     EASU in a PS by DrawInstanced(3), root CBV in a persistently mapped upload buffer, SRV table, BGRA8
//                  target larger than the region: the game's next pass draws its margin on the bindings EASU used
//   compute_uber   one stream PSO (DXIL) doing EASU then RCAS on a root-constant `pass`, RGBA16F UAV; the RCAS stays
//   compute_table  EASU CS with its CBV in a table, the input rect only in the constants (the texture is larger)
//   privatevtable  raster_cbv through list tables copied to the heap, EASU by DrawIndexedInstanced(6), the depth finder
//                  (turned on by the key alone) patching the same tables
//   lateroot       raster_cbv with its textures, descriptor heaps, views and root signature made on its device before
//                  OptiScaler is loaded (by the first DXGI call; D3D12CreateDevice then gives the same device), as when
//                  OptiScaler takes the device only at the swap chain: bindings inferred, views followed from the copies
// and, on raster_cbv, the lifecycles: replay (a closed list executed twice), replaylater (again two presents later),
// resetnoexec (recorded, reset without executing, recorded again), resize (the output region changes), device
// (another device halfway), backendchange (fsr22 to XeSS halfway, through tests/nr_spatial_replace_control.cpp: a DLL
// built with it, by an MSBuild import, or the scenario is skipped), padded1/padded2 (FSR 4, AutoExposure, linear
// colour, an input 1-2 px larger than the render size: its cropped copies are made by the present's preparation, never
// while recording). Each runs twice in a fresh process, key off (the reference: no spatial hook, no line) and on; the
// target read back at the last frame must hold the upscaler's picture in the region (not EASU's, close to the pattern
// at output size) and the reference's bytes everywhere else, and the 600-present lines give the counts. No [DlssNr] key
// is set: the key is enough.
//
//   vcvars64, then from the repo root:
//   cl /nologo /std:c++20 /EHsc /W4 /O2 /FC tests\nr_spatial_replace_dx12_gpu.cpp user32.lib delayimp.lib /link
//      /delayload:dxgi.dll
//   harness.exe <release folder> <work folder> [gbv] [timings] [backends=fsr21,fsr22,xess,ffx,fsr4]
//               [fsr4dll=<an FSR 4 amd_fidelityfx_upscaler_dx12.dll, copied into the work folder>] [scenario...]
// raster_cbv runs once per backend, the other scenarios with the first. Exit 0 when every check passed.
#include "d3d12_harness.h"
#include <cmath>
#include <regex>
using namespace d3d12_test;
namespace
{
constexpr UINT kFrames = 600, kEvent = 300, kRtW = 1024, kRtH = 600;
// The scene at render pixel (x, y) of frame `frame` (R and G; B is a checker moving with them): kScenePs
void Pattern(float x, float y, UINT frame, float* r, float* g)
{
    const float px = x + 0.7f * float(frame), py = y + 0.37f * 0.7f * float(frame);
    *r = 0.5f + 0.5f * std::sin(px * 0.11f) * std::cos(py * 0.07f);
    *g = 0.5f + 0.5f * std::sin((px + py) * 0.05f);
}
const char* kVs = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2);"
                  " return float4(p * float2(2, -2) + float2(-1, 1), 0.5, 1); }";
const char* kScenePs =
    "cbuffer S : register(b0) { float pan; }; float4 main(float4 pos : SV_Position) : SV_Target {"
    " float2 p = pos.xy + float2(pan, 0.37 * pan);"
    " return float4(0.5 + 0.5 * sin(p.x * 0.11) * cos(p.y * 0.07), 0.5 + 0.5 * sin((p.x + p.y) * 0.05),"
    " fmod(floor(p.x / 24) + floor(p.y / 24), 2) == 0 ? 0.2 : 0.8, 1); }";
const char* kMarginPs = "Texture2D<float4> t0 : register(t0); cbuffer C : register(b0) { uint4 con0; };"
                        "float4 main(float4 pos : SV_Position) : SV_Target"
                        "{ return t0.Load(int3(uint2(pos.xy) % 64, 0)) * asfloat(con0.x); }";
LRESULT CALLBACK WindowProc(HWND w, UINT m, WPARAM p, LPARAM l) { return DefWindowProcW(w, m, p, l); }

struct Game
{
    std::string shape, scenario;
    bool compute = false, uber = false, priv = false, gbv = false, fresh = true; // fresh: the input in RENDER_TARGET
    bool late = false; // lateroot: textures, heaps and root signature made before OptiScaler took the device
    UINT texW = 640, texH = 360, inW = 640, inH = 360, outW = 960, outH = 540;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::unique_ptr<Gpu> gpu;
    HWND window = nullptr;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D12Resource> input, depth, output, sharpened, cb, indices;
    ComPtr<ID3D12DescriptorHeap> views, cpu, rtvs, dsvs;
    ComPtr<ID3D12RootSignature> rs, sceneRs;
    ComPtr<ID3D12PipelineState> pso, scenePso, marginPso;
    float* mapped = nullptr;
    PrivateListVtable* vtable = nullptr;
    void* stubs[9] {};
};
constexpr UINT kSlots[] { 9, 10, 12, 13, 14, 21, 25, 26, 38 };

ComPtr<ID3D12PipelineState> Raster(Game& g, ID3D12RootSignature* rs, const Bytes& vs, const Bytes& ps, bool depth)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc {};
    desc.pRootSignature = rs;
    desc.VS = { vs.data(), vs.size() };
    desc.PS = { ps.data(), ps.size() };
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 15;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = depth;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DSVFormat = depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = 1;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = g.format;
    ComPtr<ID3D12PipelineState> pso;
    Ok(g.gpu->device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "raster PSO");
    return pso;
}

// FsrEasuCon for the input rect inW x inH of a texW x texH texture to outW x outH
void Constants(Game& g)
{
    const float x = float(g.inW) / float(g.outW), y = float(g.inH) / float(g.outH), iw = 1.0f / float(g.texW),
                ih = 1.0f / float(g.texH);
    const float con[16] { x, y, 0.5f * x - 0.5f, 0.5f * y - 0.5f, iw, ih, iw, -ih, -iw, 2 * ih, iw, 2 * ih, 0, 4 * ih,
                          0, 0 };
    std::memcpy(g.mapped, con, sizeof(con));
}

// EASU's root signature: the input in a table, the constants a root CBV, a table CBV (compute) or root constants (uber)
ComPtr<ID3D12RootSignature> GameRootSignature(ID3D12Device* d, const Game& g)
{
    const D3D12_DESCRIPTOR_RANGE ranges[3] { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 0 } };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { g.compute ? 2u : 1u, ranges };
    params[1].ParameterType = g.uber      ? D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS
                              : g.compute ? D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE
                                          : D3D12_ROOT_PARAMETER_TYPE_CBV;
    if (g.uber)
        params[1].Constants = { 0, 0, 6 };
    else if (g.compute)
        params[1].DescriptorTable = { 1, ranges + 2 };
    else
        params[1].Descriptor = { 0, 0 };
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    return RootSignature(d, { 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT });
}

// The views, written into the heaps (lateroot: after OptiScaler took the device, into heaps made before)
void Views(Game& g, ID3D12Device* d)
{
    const UINT stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto c0 = g.cpu->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = g.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(g.input.Get(), &srv, c0);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = g.format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    if (g.compute)
        d->CreateUnorderedAccessView(g.output.Get(), nullptr, &uav, { c0.ptr + stride });
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv { g.cb->GetGPUVirtualAddress(), 256 };
    d->CreateConstantBufferView(&cbv, { c0.ptr + 2 * stride });
    if (g.uber)
    {
        d->CreateShaderResourceView(g.output.Get(), &srv, { c0.ptr + 3 * stride });
        d->CreateUnorderedAccessView(g.sharpened.Get(), nullptr, &uav, { c0.ptr + 4 * stride });
    }
    d->CopyDescriptorsSimple(6, g.views->GetCPUDescriptorHandleForHeapStart(), c0,
                             D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto r0 = g.rtvs->GetCPUDescriptorHandleForHeapStart();
    d->CreateRenderTargetView(g.input.Get(), nullptr, r0);
    if (!g.compute)
        d->CreateRenderTargetView(g.output.Get(), nullptr,
                                  { r0.ptr + d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) });
    d->CreateDepthStencilView(g.depth.Get(), nullptr, g.dsvs->GetCPUDescriptorHandleForHeapStart());
}
// Textures, descriptor heaps and the root signature
int Resources(Game& g, ID3D12Device* d)
{
    const D3D12_CLEAR_VALUE clear { g.format, { 0.25f, 0.5f, 0.75f, 1 } }, red { g.format, { 1, 0, 0, 1 } },
        farthest { DXGI_FORMAT_D32_FLOAT, { 1, 0 } };
    const auto outputState = g.compute ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.input = Texture(d, g.texW, g.texH, g.format, D3D12_RESOURCE_STATE_RENDER_TARGET,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &clear);
    g.depth = Texture(d, g.texW, g.texH, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                      D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, &farthest);
    g.output = Texture(d, g.compute ? g.outW : kRtW, g.compute ? g.outH : kRtH, g.format, outputState,
                       g.compute ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       g.compute ? nullptr : &red);
    if (g.uber)
        g.sharpened = Texture(d, g.outW, g.outH, g.format, outputState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g.views = DescriptorHeap(d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, true);
    g.cpu = DescriptorHeap(d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6);
    g.rtvs = DescriptorHeap(d, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
    g.dsvs = DescriptorHeap(d, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
    g.cb = Buffer(d, 256, D3D12_HEAP_TYPE_UPLOAD);
    g.indices = Buffer(d, 256, D3D12_HEAP_TYPE_UPLOAD);
    void* indexData = nullptr;
    if (!g.input || !g.depth || !g.output || (g.uber && !g.sharpened) || !g.views || !g.cpu || !g.rtvs || !g.dsvs ||
        !g.cb || !g.indices || !Ok(g.cb->Map(0, nullptr, reinterpret_cast<void**>(&g.mapped)), "constants") ||
        !Ok(g.indices->Map(0, nullptr, &indexData), "indices"))
        return 2;
    const uint16_t triangle[] { 0, 1, 2, 0, 1, 2 };
    std::memcpy(indexData, triangle, sizeof(triangle));
    g.indices->Unmap(0, nullptr);
    Constants(g);
    if (!g.late)
        Views(g, d);
    g.rs = GameRootSignature(d, g);
    return 0;
}

int Setup(Game& g, const fs::path& repo)
{
    ID3D12Device* d = g.gpu->device.Get();
    ComPtr<IDXGIFactory2> factory;
    DXGI_SWAP_CHAIN_DESC1 sd {};
    sd.Width = kRtW;
    sd.Height = kRtH;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (!Ok(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory") ||
        !Ok(factory->CreateSwapChainForHwnd(g.gpu->queue.Get(), g.window, &sd, nullptr, nullptr, &g.swap), "swapchain"))
        return 2;
    if (!Ok(g.gpu->list->Close(), "initial close") || !Ok(g.gpu->allocator->Reset(), "initial allocator reset") ||
        !Ok(g.gpu->list->Reset(g.gpu->allocator.Get(), nullptr), "tracked generation after swapchain creation"))
        return 2;
    if (g.late) // resources made before OptiScaler was loaded; a game maps its constants again (per frame) after
    {
        void* remapped = nullptr;
        if (!Ok(g.cb->Map(0, nullptr, &remapped), "constants after the swap chain"))
            return 2;
        Views(g, d);
    }
    else if (const int made = Resources(g, d))
        return made;
    D3D12_ROOT_PARAMETER pan {};
    pan.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    pan.Constants = { 0, 0, 1 };
    g.sceneRs = RootSignature(d, { 1, &pan, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT });
    Bytes shader, vs, scene, margin;
    const auto compiled = CompileHlsl(FsrSource(repo, g.uber ? FsrPass::Uber : FsrPass::Easu, g.compute), "main",
                                      g.uber      ? "cs_6_0"
                                      : g.compute ? "cs_5_1"
                                                  : "ps_5_1",
                                      shader);
    if (compiled == CompileResult::Skipped)
        return 77;
    if (compiled != CompileResult::Success || !g.rs || !g.sceneRs ||
        CompileHlsl(kVs, "main", "vs_5_1", vs) != CompileResult::Success ||
        CompileHlsl(kScenePs, "main", "ps_5_1", scene) != CompileResult::Success ||
        CompileHlsl(kMarginPs, "main", "ps_5_1", margin) != CompileResult::Success)
        return 2;
    if (g.uber)
    {
        struct alignas(void*) Root
        {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
            ID3D12RootSignature* value;
        };
        struct alignas(void*) Code
        {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
            D3D12_SHADER_BYTECODE value;
        };
        struct Stream
        {
            Root root;
            Code code;
        } stream { { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, g.rs.Get() },
                   { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, { shader.data(), shader.size() } } };
        D3D12_PIPELINE_STATE_STREAM_DESC desc { sizeof(stream), &stream };
        ComPtr<ID3D12Device2> device2;
        if (!Ok(g.gpu->device.As(&device2), "device2") ||
            !Ok(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&g.pso)), "stream PSO"))
            return 2;
    }
    else if (g.compute)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
        desc.pRootSignature = g.rs.Get();
        desc.CS = { shader.data(), shader.size() };
        if (!Ok(d->CreateComputePipelineState(&desc, IID_PPV_ARGS(&g.pso)), "compute PSO"))
            return 2;
    }
    else
    {
        g.pso = Raster(g, g.rs.Get(), vs, shader, false);
        g.marginPso = Raster(g, g.rs.Get(), vs, margin, false);
    }
    // Scene coverage must survive jitter at the viewport edge: the unchanged margin pass samples that input.
    Bytes sceneVs;
    if (CompileHlsl("float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2);"
                    " return float4(p * float2(4, -4) + float2(-2, 2), 0.5, 1); }", "main", "vs_5_1", sceneVs) !=
        CompileResult::Success)
        return 2;
    g.scenePso = Raster(g, g.sceneRs.Get(), sceneVs, scene, true);
    if (!g.pso || !g.scenePso || (!g.compute && !g.marginPso))
        return 2;
    if (g.priv)
    {
        if ((g.vtable = MakePrivateVtable(g.gpu->list.Get())) == nullptr)
            return 2;
        for (UINT i = 0; i < 9; ++i)
            g.stubs[i] = g.vtable->entries[kSlots[i]];
    }
    return 0;
}

// One frame of the game on `l`: the scene and its depth into the input, then FSR1 (and the pass after it)
void Record(Game& g, UINT frame, ID3D12GraphicsCommandList* l)
{
    const auto readable =
        g.compute ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto r0 = g.rtvs->GetCPUDescriptorHandleForHeapStart();
    const auto dsv = g.dsvs->GetCPUDescriptorHandleForHeapStart();
    auto b = Transition(g.input.Get(), readable, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!g.fresh)
        l->ResourceBarrier(1, &b);
    g.fresh = false;
    l->OMSetRenderTargets(1, &r0, FALSE, &dsv);
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    l->SetGraphicsRootSignature(g.sceneRs.Get());
    l->SetPipelineState(g.scenePso.Get());
    const float pan = 0.7f * float(frame);
    l->SetGraphicsRoot32BitConstants(0, 1, &pan, 0);
    const D3D12_VIEWPORT scene { 0, 0, float(g.inW), float(g.inH), 0, 1 };
    const D3D12_RECT sceneRect { 0, 0, LONG(g.inW), LONG(g.inH) };
    l->RSSetViewports(1, &scene);
    l->RSSetScissorRects(1, &sceneRect);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->DrawInstanced(3, 2, 0, 0); // two instances: geometry, as scene jitter moves it
    b = Transition(g.input.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, readable);
    l->ResourceBarrier(1, &b);
    ID3D12DescriptorHeap* heaps[] { g.views.Get() };
    l->SetDescriptorHeaps(1, heaps);
    l->SetPipelineState(g.pso.Get());
    const auto table = g.views->GetGPUDescriptorHandleForHeapStart();
    const UINT stride = g.gpu->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT groupsX = (g.outW + 7) / 8, groupsY = (g.outH + 7) / 8;
    if (g.compute)
    {
        l->SetComputeRootSignature(g.rs.Get());
        l->SetComputeRootDescriptorTable(0, table);
        const float sizes[] { float(g.inW), float(g.inH), float(g.outW), float(g.outH), 0, 0 };
        if (g.uber)
            l->SetComputeRoot32BitConstants(1, 6, sizes, 0);
        else
            l->SetComputeRootDescriptorTable(1, { table.ptr + 2 * stride });
        l->Dispatch(groupsX, groupsY, 1);
        if (g.uber) // the game's RCAS, same PSO, output as input: never replaced
        {
            b = Transition(g.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable);
            l->ResourceBarrier(1, &b);
            l->SetComputeRootDescriptorTable(0, { table.ptr + 3 * stride });
            const float rcas[] { float(g.outW), float(g.outH), float(g.outW), float(g.outH), 0, 1 };
            l->SetComputeRoot32BitConstants(1, 6, rcas, 0);
            l->Dispatch(groupsX, groupsY, 1);
            b = Transition(g.output.Get(), readable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            l->ResourceBarrier(1, &b);
        }
        return;
    }
    l->SetGraphicsRootSignature(g.rs.Get());
    l->SetGraphicsRootDescriptorTable(0, table);
    l->SetGraphicsRootConstantBufferView(1, g.cb->GetGPUVirtualAddress());
    const D3D12_VIEWPORT vp { 0, 0, float(g.outW), float(g.outH), 0, 1 };
    const D3D12_RECT sc { 0, 0, LONG(g.outW), LONG(g.outH) };
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sc);
    const D3D12_CPU_DESCRIPTOR_HANDLE out { r0.ptr + g.gpu->device->GetDescriptorHandleIncrementSize(
                                                         D3D12_DESCRIPTOR_HEAP_TYPE_RTV) };
    const float background[] { 1, 0, 0, 1 };
    l->ClearRenderTargetView(out, background, 0, nullptr);
    l->OMSetRenderTargets(1, &out, FALSE, nullptr);
    if (g.priv)
    {
        const D3D12_INDEX_BUFFER_VIEW ib { g.indices->GetGPUVirtualAddress(), 12, DXGI_FORMAT_R16_UINT };
        l->IASetIndexBuffer(&ib);
        l->DrawIndexedInstanced(6, 1, 0, 0, 0);
    }
    else
        l->DrawInstanced(3, 1, 0, 0);
    // The margin: the game's next pass, on the bindings it set for EASU (given back after a replacement)
    l->SetPipelineState(g.marginPso.Get());
    const D3D12_VIEWPORT right { float(g.outW), 0, float(kRtW - g.outW), float(kRtH), 0, 1 };
    const D3D12_RECT rightRect { LONG(g.outW), 0, LONG(kRtW), LONG(kRtH) };
    l->RSSetViewports(1, &right);
    l->RSSetScissorRects(1, &rightRect);
    l->DrawInstanced(3, 1, 0, 0);
}

// Another device on the same adapter (not D3D12CreateDevice's per-adapter singleton), the game made again on it
int ChangeDevice(Game& g, const fs::path& repo)
{
    ComPtr<ID3D12DeviceFactory> devices;
    ComPtr<IDXGIFactory4> dxgi;
    ComPtr<IDXGIAdapter1> adapter;
    const LUID luid = g.gpu->device->GetAdapterLuid();
    const void* old = g.gpu->device.Get();
    if (!g.gpu->WaitIdle() || !Ok(D3D12GetInterface(CLSID_D3D12DeviceFactory, IID_PPV_ARGS(&devices)), "factory") ||
        !Ok(devices->InitializeFromGlobalState(), "factory state") ||
        !Ok(devices->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON), "independent") ||
        !Ok(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi)), "dxgi") ||
        !Ok(dxgi->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "adapter"))
        return 2;
    Game fresh;
    fresh.shape = g.shape, fresh.scenario = g.scenario, fresh.window = g.window, fresh.gbv = g.gbv;
    g = std::move(fresh); // everything of the first device released, its swap chain too
    g.gpu = std::make_unique<Gpu>();
    Gpu& gpu = *g.gpu;
    D3D12_COMMAND_QUEUE_DESC qd {};
    if (!Ok(devices->CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)), "device") ||
        !Ok(gpu.device.As(&gpu.info), "info queue") ||
        !Ok(gpu.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&gpu.queue)), "queue") ||
        !gpu.CreateList(gpu.allocator, gpu.list) ||
        !Ok(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)), "fence") ||
        (gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) == nullptr)
        return 2;
    std::printf("device changed (%p -> %p)\n", old, static_cast<void*>(gpu.device.Get()));
    return Setup(g, repo);
}

int Child(const std::string& shape, const std::string& scenario, const fs::path& repo, bool gbv)
{
    Checks checks;
    Game g;
    g.shape = shape, g.scenario = scenario, g.gbv = gbv;
    g.compute = shape.starts_with("compute"), g.uber = shape == "compute_uber", g.priv = shape == "privatevtable";
    if (g.compute)
        g.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (shape == "compute_table")
        g.texW = 704, g.texH = 396;
    if (scenario.starts_with("padded"))
        g.texW += scenario == "padded1" ? 1 : 2, g.texH += scenario == "padded1" ? 1 : 2;
    WNDCLASSW wc {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Fsr1Replacement";
    RegisterClassW(&wc);
    g.window = CreateWindowW(wc.lpszClassName, L"FSR1 replacement", WS_OVERLAPPEDWINDOW, 40, 40, kRtW, kRtH, nullptr,
                             nullptr, wc.hInstance, nullptr);
    if (!g.window)
        return 2;
    ShowWindow(g.window, SW_SHOWNOACTIVATE);
    g.gpu = std::make_unique<Gpu>();
    ComPtr<ID3D12Device> early;
    if (scenario == "lateroot")
    {
        // Resources made before OptiScaler is loaded (dxgi.dll, delay-loaded: by the first DXGI call), on a
        // device the next D3D12CreateDevice gives again, the adapter's. The debug layer is on before both.
        ComPtr<ID3D12Debug> debug;
        if (!Ok(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug layer"))
            return 2;
        debug->EnableDebugLayer();
        if (gbv)
        {
            ComPtr<ID3D12Debug1> debug1;
            if (!Ok(debug.As(&debug1), "early GPU-based validation"))
                return 2;
            debug1->SetEnableGPUBasedValidation(TRUE);
            std::puts("lateroot: GPU-based validation enabled before its early device");
        }
        if (!Ok(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&early)), "early device"))
            return 2;
        g.late = true;
        if (const int made = Resources(g, early.Get()))
            return made;
    }
    if (!g.gpu->Create(Adapter::Hardware, gbv && !early, !early) ||
        (early && !Ok(g.gpu->device.As(&g.gpu->info), "info queue")))
        return 2;
    if (early && early.Get() != g.gpu->device.Get())
    {
        std::printf("lateroot: D3D12CreateDevice gave another device\n");
        return 2;
    }
    Sleep(2000); // OptiScaler's GPU detection thread, before the swap chain (it races CreateSwapChainForHwnd)
    if (const int setup = Setup(g, repo))
        return setup;
    const auto control =
        reinterpret_cast<bool (*)(int, int)>(GetProcAddress(GetModuleHandleW(L"dxgi.dll"), "SpatialTestControl"));
    if (scenario == "backendchange" && control == nullptr)
        return 78;
    ComPtr<ID3D12CommandAllocator> laterAllocator;
    ComPtr<ID3D12GraphicsCommandList> later;
    Bytes frameBytes;
    UINT64 messages = 0;
    for (UINT frame = 0; frame < kFrames; ++frame)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        if (scenario == "device" && frame == kEvent)
        {
            messages += g.gpu->Messages(D3D12_MESSAGE_SEVERITY_ERROR);
            if (const int changed = ChangeDevice(g, repo))
                return changed;
        }
        if (scenario == "resize" && frame == kEvent)
            g.outW = 768, g.outH = 432;
        if (scenario == "backendchange" && frame == kEvent && !control(2, 1))
            return 2;
        Constants(g);
        // Two frames of constants that do not decode: the game's EASU, dumped once (without a layout, nothing to find
        // the bindings by: left unresolved)
        if (shape == "raster_cbv" && scenario != "lateroot" && (frame == 10 || frame == 11))
            g.mapped[0] = 0.0f;
        auto* l = g.gpu->list.Get();
        Record(g, frame, l);
        ID3D12CommandList* lists[] { l };
        bool ok = true;
        if (frame + 1 == kFrames)
            ok = g.gpu->Readback(g.uber ? g.sharpened.Get() : g.output.Get(),
                                 g.compute ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_RENDER_TARGET,
                                 frameBytes);
        else if (scenario == "resetnoexec" && frame == kEvent)
        {
            ok = Ok(l->Close(), "close") && Ok(l->Reset(g.gpu->allocator.Get(), nullptr), "reset unexecuted");
            Record(g, frame, l);
            ok = ok && g.gpu->ExecuteAndWait();
            std::printf("list reset without executing at frame %u\n", frame);
        }
        else if (scenario == "replay" && frame == kEvent)
        {
            ok = Ok(l->Close(), "close");
            for (int i = 0; ok && i < 2; ++i) // the second waits for the first, as games do
                g.gpu->queue->ExecuteCommandLists(1, lists), ok = g.gpu->WaitIdle();
            ok = ok && Ok(g.gpu->allocator->Reset(), "allocator") &&
                 Ok(l->Reset(g.gpu->allocator.Get(), nullptr), "reset");
            std::printf("closed list executed twice at frame %u\n", frame);
        }
        else if (scenario == "replaylater" && frame == kEvent)
        {
            // The game goes on with a new list and allocator, this closed one kept
            ok = Ok(l->Close(), "close");
            g.gpu->queue->ExecuteCommandLists(1, lists);
            ok = ok && g.gpu->WaitIdle();
            later = g.gpu->list, laterAllocator = g.gpu->allocator;
            ok = ok && g.gpu->CreateList(g.gpu->allocator, g.gpu->list);
        }
        else
            ok = g.gpu->ExecuteAndWait();
        if (later && frame == kEvent + 2) // two presents later, the backend's history moved on
        {
            ID3D12CommandList* again[] { later.Get() };
            g.gpu->queue->ExecuteCommandLists(1, again);
            ok = ok && g.gpu->WaitIdle();
            later.Reset(), laterAllocator.Reset();
            std::printf("closed list executed again two presents later at frame %u\n", frame);
        }
        if (!ok || !Ok(g.swap->Present(0, 0), "present"))
            return 2;
    }
    if (g.priv)
    {
        UINT patched = 0;
        for (UINT i = 0; i < 9; ++i)
            patched += g.vtable->entries[kSlots[i]] != g.stubs[i];
        std::printf("privatevtable: %u/9 forwarding entries patched\n", patched);
    }
    std::ofstream("frame.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(frameBytes.data()), std::streamsize(frameBytes.size()));
    messages += g.gpu->Messages(D3D12_MESSAGE_SEVERITY_ERROR);
    std::printf("child done: %u frames, %llu debug layer errors\n", kFrames, messages);
    checks.Check(messages == 0, "debug layer: zero errors");
    const int result = checks.Summary();
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), UINT(result)); // no teardown: as a game killed at exit
    return result;
}

// Channel c (0 R, 1 G, 2 B) of pixel i in [0, 1]: BGRA8 or RGBA16F
float Channel(const std::string& frame, bool half, size_t i, int c)
{
    if (!half)
        return uint8_t(frame[i * 4 + size_t(2 - c)]) / 255.0f;
    uint16_t h = 0;
    std::memcpy(&h, &frame[i * 8 + size_t(c) * 2], 2);
    const int e = (h >> 10) & 31, m = h & 1023;
    return (h & 0x8000 ? -1.0f : 1.0f) * (e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m + 1024), e - 25));
}
struct Compare
{
    double regionDiffers = 0, maePattern = 0, maeReference = 0;
    bool outsideSame = true;
};
// The region outW x outH of `on` against the reference's and the pattern at output size; the rest byte for byte
Compare CompareFrames(const std::string& on, const std::string& ref, bool half, UINT width, UINT height, UINT outW,
                      UINT outH)
{
    Compare c;
    const size_t px = half ? 8 : 4;
    uint64_t differs = 0, inner = 0;
    const float scale = 640.0f / float(outW);
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
        {
            const size_t i = size_t(y) * width + x;
            if (x >= outW || y >= outH)
            {
                c.outsideSame = c.outsideSame && std::memcmp(&on[i * px], &ref[i * px], px) == 0;
                continue;
            }
            bool off = false;
            for (int k = 0; k < 3; ++k)
                off = off || std::fabs(Channel(on, half, i, k) - Channel(ref, half, i, k)) > 2.5f / 255.0f;
            differs += off;
            if (x < 16 || y < 16 || x + 16 >= outW || y + 16 >= outH) // the borders are clamped differently
                continue;
            float r = 0, g = 0;
            Pattern((float(x) + 0.5f) * scale, (float(y) + 0.5f) * scale, kFrames - 1, &r, &g);
            c.maePattern += std::fabs(Channel(on, half, i, 0) - r) + std::fabs(Channel(on, half, i, 1) - g);
            c.maeReference += std::fabs(Channel(ref, half, i, 0) - r) + std::fabs(Channel(ref, half, i, 1) - g);
            ++inner;
        }
    c.regionDiffers = double(differs) / (double(outW) * outH);
    c.maePattern /= 2.0 * double(inner ? inner : 1);
    c.maeReference /= 2.0 * double(inner ? inner : 1);
    return c;
}
long long Number(const std::string& log, const char* pattern)
{
    std::smatch m;
    return std::regex_search(log, m, std::regex(pattern)) ? std::stoll(m[1].str()) : -1;
}
// The shipped ini without [DlssNr] (the key needs none of it), the keys of this run set
std::string Ini(const std::string& base, bool on, const std::string& backend, const std::string& scenario, bool timings)
{
    std::string ini = std::regex_replace(base, std::regex(R"((^|\n)\[DlssNr\][\s\S]*?(?=\n\[|$))"), "$1");
    auto set = [&ini](const char* key, const std::string& value)
    {
        ini = std::regex_replace(ini, std::regex(std::string("(^|\\n)") + key + "=[^\\r\\n]*"),
                                 "$1" + std::string(key) + "=" + value, std::regex_constants::format_first_only);
    };
    set("ReplaceGameFsr1", on ? "true" : "false");
    set("Dx12Upscaler", backend == "fsr4" ? "ffx" : on ? backend : "auto");
    if (backend == "fsr4")
        for (const char* key : { "NonLinearColorSpace", "NonLinearSRGB", "NonLinearPQ" })
            set(key, "false");
    if (backend == "fsr4")
        set("Fsr4ForceModel", "2"), set("Fsr4Preset", "0"), set("AutoExposure", "true");
    set("LogToFile", "true");
    set("LogLevel", scenario.starts_with("padded") ? "1" : "2"); // the cropped copies are logged at debug
    return timings ? ini + "\n[DlssNr]\nNativeStageTimings=true\n" : ini;
}
} // namespace

static int Parent(int argc, char** argv)
{
    if (argc < 3)
    {
        std::puts("usage: harness <release folder> <work folder> [gbv] [timings] [backends=a,b] [fsr4dll=path] "
                  "[scenario...]");
        return 2;
    }
    const fs::path release = fs::absolute(argv[1]), work = fs::absolute(argv[2]);
    const fs::path repo = fs::path(__FILE__).parent_path().parent_path();
    bool gbv = false, timings = false;
    fs::path fsr4dll;
    std::vector<std::string> backends { "fsr21", "fsr22", "xess", "ffx" }, scenarios;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "gbv" || a == "timings")
            (a == "gbv" ? gbv : timings) = true;
        else if (a.starts_with("fsr4dll="))
            fsr4dll = fs::absolute(a.substr(8));
        else if (a.starts_with("backends="))
        {
            backends.clear();
            std::stringstream list(a.substr(9));
            for (std::string b; std::getline(list, b, ',');)
                backends.push_back(b);
        }
        else
            scenarios.push_back(a);
    }
    if (scenarios.empty())
        scenarios = { "raster_cbv",  "compute_uber", "compute_table", "privatevtable", "replay",
                      "replaylater", "resetnoexec",  "resize",        "device",        "backendchange",
                      "padded1",     "padded2",      "lateroot" };
    const std::string baseIni = ReadText(release / "OptiScaler.ini");
    Checks checks;
    for (const auto& scenario : scenarios)
        for (const auto& backend : scenario == "raster_cbv" ? backends : std::vector<std::string> { backends[0] })
        {
            const bool padded = scenario.starts_with("padded");
            const std::string run = padded                        ? "fsr4"
                                    : scenario == "backendchange" ? "fsr22"
                                                                  : backend,
                              name = scenario + " / " + run;
            const std::string shape =
                scenario.starts_with("compute") || scenario == "privatevtable" ? scenario : "raster_cbv";
            if (run == "fsr4" && fsr4dll.empty())
            {
                checks.Skip((name + ": needs fsr4dll=").c_str());
                continue;
            }
            std::string reference;
            for (bool on : { false, true })
            {
                const auto install = work / (scenario + "_" + run + (on ? "_on" : "_off"));
                std::error_code ec;
                fs::create_directories(install);
                fs::copy_file(release / "OptiScaler.dll", install / "dxgi.dll", fs::copy_options::overwrite_existing);
                fs::copy_file(fs::absolute(argv[0]), install / "harness.exe", fs::copy_options::overwrite_existing);
                fs::copy(release / "OptiScaler", install / "OptiScaler",
                         fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
                if (run == "fsr4") // the FSR 4 upscaler in place of the shipped one, in this folder only
                    fs::copy_file(fsr4dll, install / "OptiScaler/amd_fidelityfx_upscaler_dx12.dll",
                                  fs::copy_options::overwrite_existing, ec);
                std::ofstream(install / "OptiScaler.ini", std::ios::binary)
                    << Ini(baseIni, on, run, scenario, timings && on);
                fs::remove(install / "OptiScaler.log", ec);
                fs::remove(install / "frame.bin", ec);
                const auto args = L"child " + fs::path(shape).wstring() + L" " + fs::path(scenario).wstring() + L" \"" +
                                  repo.wstring() + L"\"" + (gbv ? L" gbv" : L"");
                const auto child = Launch(install / "harness.exe", args, install, install / "child.txt", 1800000);
                std::printf("%s %s: child_exit=%lu\n", name.c_str(), on ? "on" : "off", child.exitCode);
                if (child.exitCode == 77 || child.exitCode == 78)
                {
                    checks.Skip(child.exitCode == 77 ? "DXIL compiler unavailable"
                                                     : "backendchange: needs a DLL built with its control");
                    break;
                }
                const auto Check = [&](bool ok, const std::string& what)
                { return checks.Check(ok, (name + (on ? ": " : ": key off: ") + what).c_str()); };
                Check(child.started && !child.timedOut && child.exitCode == 0, "ran, no debug layer error");
                if (child.exitCode)
                    std::printf("%s\n", child.output.substr(0, 3000).c_str());
                const auto frame = ReadText(install / "frame.bin"), log = ReadText(install / "OptiScaler.log");
                if (!on)
                {
                    reference = frame;
                    Check(log.find("Spatial pass:") == std::string::npos, "no spatial hook installed, no line");
                    continue;
                }
                std::smatch line;
                const bool found = std::regex_search(
                    log, line,
                    std::regex(R"(Spatial pass: [^;\n]*; replaced (\d+) upscale passes[^\n]*fallbacks: ([^;\n]*); )"
                               R"(depth from the game (\d+), neutral (\d+); backend ([^;\n]*);)"));
                const long long replaced = found ? std::stoll(line[1].str()) : -1;
                const std::regex tracking(
                    R"(Spatial tracking: device [^;]+; heap attempts (\d+), success (\d+), ignored (\d+), failure (\d+), queries (\d+); loose operations (\d+), writes (\d+), failure (\d+); root attempts (\d+), resolved (\d+), failure (\d+);)");
                uint64_t operations[11] {};
                UINT summaries = 0;
                for (auto i = std::sregex_iterator(log.begin(), log.end(), tracking); i != std::sregex_iterator(); ++i)
                {
                    ++summaries;
                    for (UINT k = 0; k < std::size(operations); ++k)
                        operations[k] += std::stoull((*i)[k + 1].str());
                }
                Check(summaries > 0, "actual fallback-helper counters at 600 presents");
                if (scenario == "lateroot")
                    Check(operations[0] > 0 && operations[4] > 0 && operations[5] > 0 && operations[8] >= 595 &&
                              operations[9] >= 595,
                          "old objects positively exercise bounded heap, loose and root fallback");
                else if (scenario == "raster_cbv" || scenario == "compute_uber" || scenario == "compute_table" ||
                         scenario == "privatevtable")
                    Check(operations[0] == 0 && operations[4] == 0 && operations[5] == 0 && operations[8] == 0,
                          "primary tracked path enters zero fallback helpers, including sampler heaps");
                std::printf("      replaced %lld/600; fallbacks: %s; depth from the game %s, neutral %s; backend %s\n",
                            replaced, line[2].str().c_str(), line[3].str().c_str(), line[4].str().c_str(),
                            line[5].str().c_str());
                const bool half = shape.starts_with("compute");
                const UINT outW = scenario == "resize" ? 768 : 960, outH = scenario == "resize" ? 432 : 540;
                const UINT width = half ? 960 : kRtW, height = half ? 540 : kRtH;
                if (!Check(found && reference.size() == frame.size() &&
                               frame.size() == size_t(width) * height * (half ? 8 : 4),
                           "the session's line at 600 presents, frames read back"))
                    continue;
                const Compare cmp = CompareFrames(frame, reference, half, width, height, outW, outH);
                std::printf("      region off EASU's %.1f%%, error against the pattern %.4f (EASU %.4f)\n",
                            cmp.regionDiffers * 100.0, cmp.maePattern, cmp.maeReference);
                const bool steady = scenario == "raster_cbv" || scenario == "lateroot" ||
                                    scenario == "compute_uber" || scenario == "compute_table" ||
                                    scenario == "privatevtable";
                const long long least = scenario == "device" || scenario == "backendchange" ? 580
                                        : steady                                            ? 595
                                                                                            : 590;
                Check(replaced >= least, "at least " + std::to_string(least) + " of 600 upscale passes replaced");
                Check(cmp.regionDiffers > 0.25, "the region is the upscaler's, not EASU's");
                Check(cmp.maePattern < 0.06, "the region is close to the pattern at output size");
                Check(cmp.outsideSame, "the margin's pass and the rest of the target are the game's, byte for byte");
                Check(std::regex_search(log, std::regex(R"(Spatial pass: \S[^\n]* prepared for \d+x\d+ to \d+x\d+)")),
                      "the upscaler was prepared by the present, not while recording");
                Check(log.find(shape.starts_with("compute")
                                   ? "unordered access; the upscaler's output is written into it directly"
                                   : "unordered access; the upscaler's output is copied into it") != std::string::npos,
                      "whether the target allows unordered access is logged");
                const std::string fmt = std::to_string(half ? 10 : 87);
                Check(log.find("input 640x360 format " + fmt + " to output 960x540 format " + fmt) != std::string::npos,
                      "the pass resolved: its input rect and output");
                if (!scenario.starts_with("re") && scenario != "device")
                {
                    Check(log.find("600 frames, EASU draws 600 (1-1 a frame, exactly one in 600 frames)") !=
                              std::string::npos,
                          "one pass a present, no forwarding duplicates");
                    std::smatch jitter;
                    Check(std::regex_search(log, jitter,
                                            std::regex(R"(scene jitter on (\d+) of these frames, (\d+) draws moved)")) &&
                              std::stoll(jitter[1].str()) >= 590 && std::stoll(jitter[2].str()) >= 590 &&
                              log.find("0 upscales given an offset older than the frame's") != std::string::npos &&
                              log.find("scene jitter: 0 lists were recorded across the game's present") != std::string::npos,
                          "scene jitter follows the frame, without stale offsets or mid-list changes");
                }
                Check(log.find("scene jitter is off for this session") == std::string::npos,
                      "no automatic jitter-off on a scene without motion vectors");
                if (run == "fsr4")
                    Check(line[5].str().find(" 4.") != std::string::npos, "FSR 4 was loaded");
                if (timings)
                {
                    const auto at = log.find("Spatial pass: GPU ms");
                    const std::string ms = at == std::string::npos ? "" : log.substr(at, log.find('\n', at) - at);
                    std::printf("      %s\n", ms.c_str());
                    for (const char* step : { "depth.copy", "flow.", "trust.mask", "trust.guides", "upscaler.",
                                              "restore.barriers", "total" })
                        Check(ms.find(step) != std::string::npos, std::string("GPU ms names ") + step);
                    Check((ms.find("output.copy") == std::string::npos) == shape.starts_with("compute"),
                          "UAV target has no output.copy; render target without UAV still has it");
                }
                const auto inferred = log.find("root signature not seen at creation; bindings inferred from constants");
                Check((inferred != std::string::npos) == (scenario == "lateroot") &&
                          log.find("root signature not seen at creation", inferred + 1) == std::string::npos,
                      "bindings inferred from the constants (logged once) only for a root signature made before");
                if (shape == "raster_cbv" && scenario != "lateroot") // without a layout no block is the constants
                {
                    const auto first = log.find("constant buffer did not decode");
                    Check(first != std::string::npos &&
                              log.find("constant buffer did not decode", first + 1) == std::string::npos,
                          "constants that do not decode dumped once");
                }
                if (shape == "compute_uber")
                    Check(log.find("RCAS draws 600") != std::string::npos, "the game's RCAS ran untouched");
                if (shape == "privatevtable")
                    Check(child.output.find("privatevtable: 9/9 forwarding entries patched") != std::string::npos &&
                              log.find("Depth finder:") != std::string::npos,
                          "all private entries patched, with the depth finder the key turned on");
                if (scenario.starts_with("replay"))
                    Check(log.find("replayed a closed list with its private resource states restored") !=
                                  std::string::npos &&
                              log.find("not replay-safe") == std::string::npos,
                          "replayed with its private states restored, without leaving the lists");
                if (scenario == "resetnoexec")
                    Check(Number(log, R"(never executed (\d+))") >= 1, "the list reset unexecuted was abandoned");
                if (scenario == "resize")
                    Check(log.find("prepared for 640x360 to 768x432") != std::string::npos,
                          "the new output prepared by the present");
                if (scenario == "device")
                    Check(log.find("FSR1 upscale (device changed)") != std::string::npos,
                          "the session left on the device change, and came back");
                if (scenario == "backendchange")
                    Check(line[5].str().find("XeSS") != std::string::npos &&
                              log.find("XeSS prepared for") != std::string::npos,
                          "the switch rebuilt the upscaler by the present, and reached XeSS");
                if (padded)
                {
                    // The copies made by the last preparation (logged before its line), nothing after it by a recording
                    const auto prepared = log.rfind("prepared for 64");
                    int before = 0, after = 0;
                    for (auto at = log.find("WithSize Created new one:"); at < prepared;
                         at = log.find("WithSize Created new one:", at + 1))
                        ++before;
                    for (auto at = log.find("Created new one:", prepared); at != std::string::npos;
                         at = log.find("Created new one:", at + 1))
                        ++after;
                    std::printf("      cropped copies: prepare %d, recording %d\n", before, after);
                    Check(prepared != std::string::npos && before == 2 && after == 0,
                          "cropped copies: prepare 2, recording 0");
                }
            }
        }
    return checks.Summary();
}
int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 5 && std::strcmp(argv[1], "child") == 0)
    {
        const int result = Child(argv[2], argv[3], argv[4], argc >= 6 && std::strcmp(argv[5], "gbv") == 0);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), UINT(result)); // a setup failure, its objects released
        return result;
    }
    return Parent(argc, argv);
}
