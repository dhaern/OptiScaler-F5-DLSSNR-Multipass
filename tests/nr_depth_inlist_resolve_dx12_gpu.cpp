// GPU test for native/DepthSnapshotDx12.h, the D3D12 depth finder's resolve of a multisampled depth buffer recorded
// into the game's own command list (ResolveSubresourceRegion MIN/MAX into a single-sample slot), no game. A
// multisampled depth buffer is cleared to 0.25 and a triangle at 0.75 is drawn over its upper-left half, so the pixels
// along the diagonal hold both depths. D24S8 is kept on the copy path (native::DepthResolveFormat says no: an RTX 5070
// Ti resolves it to zeros), which the test checks. For D32, D32S8 and D16, with 2, 4 and 8 samples where the device
// has them, quality 0 and the standard pattern, reversed and standard Z:
//  - the slot, made as GenericDepth_Dx12 makes it (the buffer's typeless family, single-sample, resting readable) and
//    read back the way the menu's preview reads it (footprint of subresource 0, decoded per family), equals pixel for
//    pixel the compute resolve the finder used before (native/DepthResolveDx12, the reference),
//  - the same with the game's buffer made DENY_SHADER_RESOURCE, as many games make their depth buffers,
//  - the source and the slot are left in the states they were in,
//  - two lists over one buffer, each taking its own stretch into its own slot, executed in both orders: each slot
//    holds its own stretch (no dependence on recording or execution order between lists),
//  - with the debug layer installed, the runtime reports no error or warning.
// A device without programmable sample positions tier 1 must not resolve a region (the spec removes it; vkd3d-proton
// reports tier 0 and records nothing): there the test only checks that native::DepthResolveSupported says no, which
// is what keeps GenericDepth_Dx12 on the copy path.
// Run on the hardware adapter and on WARP (argument "warp").
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_depth_inlist_resolve_dx12_gpu.cpp OptiScaler\native\DepthResolveDx12.cpp d3d12.lib
//   dxgi.lib d3dcompiler.lib

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../OptiScaler/native/DepthResolveDx12.h"
#include "../OptiScaler/native/DepthSnapshotDx12.h"

using Microsoft::WRL::ComPtr;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

constexpr UINT kWidth = 96, kHeight = 64;
constexpr D3D12_RESOURCE_STATES kRest =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator, allocator2;
    ComPtr<ID3D12GraphicsCommandList> list, list2;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = nullptr;
    ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3DBlob> vs;

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Wait();
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    void Wait()
    {
        queue->Signal(fence.Get(), ++value);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 10000);
    }
};

bool MakeGpu(Gpu& g, bool warp)
{
    ComPtr<ID3D12Debug> debug;

    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();

    ComPtr<IDXGIAdapter> adapter;

    if (warp)
    {
        ComPtr<IDXGIFactory4> factory;

        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))
            return false;
    }

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
        return false;

    g.device.As(&g.info);

    // The order test clears to a value other than the buffer's optimized one, as games do: a performance note, not an
    // error
    if (g.info != nullptr)
    {
        D3D12_MESSAGE_ID hide[] = { D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE };
        D3D12_INFO_QUEUE_FILTER filter {};
        filter.DenyList.NumIDs = 1;
        filter.DenyList.pIDList = hide;
        g.info->AddStorageFilterEntries(&filter);
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g.queue));
    g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator));
    g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator2));
    g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(), nullptr, IID_PPV_ARGS(&g.list));
    g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator2.Get(), nullptr, IID_PPV_ARGS(&g.list2));
    g.list2->Close();
    g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // A triangle at depth z (root constant 0); shape 0: upper-left half, shape 1: a small one near the lower right
    const char* vsSource =
        "cbuffer C : register(b0) { float z; uint shape; };\n"
        "float4 VSMain(uint id : SV_VertexID) : SV_Position {\n"
        "  float2 p = shape == 0 ? (id == 0 ? float2(-1, 1) : id == 1 ? float2(1, 1) : float2(-1, -1))\n"
        "                        : (id == 0 ? float2(0.2, -0.2) : id == 1 ? float2(0.9, -0.2) : float2(0.2, -0.9));\n"
        "  return float4(p, z, 1); }";
    D3DCompile(vsSource, std::strlen(vsSource), "tri", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &g.vs, nullptr);

    D3D12_ROOT_PARAMETER param {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues = 2;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &param;
    ComPtr<ID3DBlob> serialized;
    D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr);
    g.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                  IID_PPV_ARGS(&g.root));
    return g.queue != nullptr && g.list != nullptr && g.list2 != nullptr && g.fence != nullptr && g.vs != nullptr &&
           g.root != nullptr;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

ComPtr<ID3D12Resource> Texture(Gpu& g, DXGI_FORMAT format, UINT samples, D3D12_RESOURCE_FLAGS flags,
                               D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, UINT quality = 0)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = samples;
    desc.SampleDesc.Quality = quality;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> tex;
    g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&tex));
    return tex;
}

// A game's depth buffer, its DSV and a pipeline drawing into it
struct GameDepth
{
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12PipelineState> pso;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv {};
};

bool MakeGameDepth(Gpu& g, GameDepth& d, DXGI_FORMAT typeless, DXGI_FORMAT dsvFormat, UINT samples, bool deny,
                   UINT quality = 0)
{
    D3D12_CLEAR_VALUE clear {};
    clear.Format = dsvFormat;
    clear.DepthStencil.Depth = 0.25f;
    auto flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    if (deny)
        flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    d.resource = Texture(g, typeless, samples, flags, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, quality);

    if (d.resource == nullptr)
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heapDesc.NumDescriptors = 1;
    g.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&d.dsvHeap));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv {};
    dsv.Format = dsvFormat;
    dsv.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    d.dsv = d.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateDepthStencilView(d.resource.Get(), &dsv, d.dsv);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso {};
    pso.pRootSignature = g.root.Get();
    pso.VS = { g.vs->GetBufferPointer(), g.vs->GetBufferSize() };
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.MultisampleEnable = samples > 1;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.DSVFormat = dsvFormat;
    pso.SampleDesc.Count = samples;
    pso.SampleDesc.Quality = quality;
    return SUCCEEDED(g.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&d.pso)));
}

// Records into `list`: clear to `clearDepth` and a triangle `shape` at `z`. The buffer stays in DEPTH_WRITE.
void Draw(Gpu& g, ID3D12GraphicsCommandList* list, GameDepth& d, float clearDepth, float z, UINT shape)
{
    const D3D12_VIEWPORT viewport { 0, 0, (float) kWidth, (float) kHeight, 0, 1 };
    const D3D12_RECT scissor { 0, 0, (LONG) kWidth, (LONG) kHeight };
    list->ClearDepthStencilView(d.dsv, D3D12_CLEAR_FLAG_DEPTH, clearDepth, 0, 0, nullptr);
    list->OMSetRenderTargets(0, nullptr, FALSE, &d.dsv);
    list->SetGraphicsRootSignature(g.root.Get());
    list->SetPipelineState(d.pso.Get());
    const UINT constants[2] = { *(const UINT*) &z, shape };
    list->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
}

// Reads `resource`'s first subresource back as floats. `family` is how the finder decodes it: the preview's footprint
// of subresource 0 of a desc in that format (GenericDepth_Dx12 EnsureBackup) and its per-family decode
// (DrawDepthPreview).
std::vector<float> Read(Gpu& g, ID3D12Resource* resource, DXGI_FORMAT family)
{
    D3D12_RESOURCE_DESC desc = resource->GetDesc();
    desc.Format = family;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&readback));

    const auto in = Transition(resource, kRest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->ResourceBarrier(1, &in);
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto out = Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, kRest);
    g.list->ResourceBarrier(1, &out);
    g.Submit();

    std::vector<float> values;
    void* mapped = nullptr;
    D3D12_RANGE range { 0, (SIZE_T) total };

    if (FAILED(readback->Map(0, &range, &mapped)))
        return values;

    values.reserve(kWidth * kHeight);

    for (UINT y = 0; y < kHeight; ++y)
    {
        const uint8_t* line = (const uint8_t*) mapped + footprint.Offset + (size_t) y * footprint.Footprint.RowPitch;

        for (UINT x = 0; x < kWidth; ++x)
        {
            switch (family)
            {
            case DXGI_FORMAT_R16_TYPELESS:
                values.push_back(((const uint16_t*) line)[x] / 65535.0f);
                break;
            case DXGI_FORMAT_R24G8_TYPELESS:
                values.push_back((((const uint32_t*) line)[x] & 0xFFFFFFu) / 16777215.0f);
                break;
            default:
                values.push_back(((const float*) line)[x]);
                break;
            }
        }
    }

    D3D12_RANGE none { 0, 0 };
    readback->Unmap(0, &none);
    return values;
}

// Largest difference, and how many pixels differ by more than half a step of the format
float Compare(const std::vector<float>& a, const std::vector<float>& b, float step, int* differ)
{
    *differ = 0;
    float worst = 0.0f;

    if (a.size() != b.size() || a.empty())
    {
        *differ = -1;
        return 1.0f;
    }

    for (size_t i = 0; i < a.size(); ++i)
    {
        const float d = std::fabs(a[i] - b[i]);
        worst = d > worst ? d : worst;
        *differ += d > step * 0.5f ? 1 : 0;
    }

    return worst;
}

int Count(const std::vector<float>& v, float value, float step)
{
    int n = 0;

    for (float x : v)
        n += std::fabs(x - value) <= step * 0.5f ? 1 : 0;

    return n;
}

int DebugMessages(Gpu& g)
{
    if (g.info == nullptr)
        return 0;

    int count = 0;

    for (UINT64 i = 0; i < g.info->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        g.info->GetMessage(i, nullptr, &size);
        auto* message = (D3D12_MESSAGE*) malloc(size);

        if (message != nullptr && SUCCEEDED(g.info->GetMessage(i, message, &size)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
             message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
             message->Severity == D3D12_MESSAGE_SEVERITY_WARNING))
        {
            std::printf("      debug layer: %s\n", message->pDescription);
            ++count;
        }

        free(message);
    }

    g.info->ClearStoredMessages();
    return count;
}

struct Format
{
    const char* name;
    DXGI_FORMAT typeless, dsv, view;
    float step; // one step of the format (0 for float: exact)
};

const Format kFormats[] = {
    { "D32", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 1e-7f },
    { "D32S8", DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
      1e-7f },
    { "D24S8", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
      1.0f / 16777215.0f },
    { "D16", DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, 1.0f / 65535.0f },
};

bool Supported(Gpu& g, DXGI_FORMAT dsv, UINT samples)
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels {};
    levels.Format = dsv;
    levels.SampleCount = samples;
    return SUCCEEDED(
               g.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) &&
           levels.NumQualityLevels > 0;
}

// The slot GenericDepth_Dx12 TakeSlot makes for a buffer resolved in the list: its typeless family, single-sample, no
// flags, resting readable.
ComPtr<ID3D12Resource> Slot(Gpu& g, DXGI_FORMAT typeless)
{
    return Texture(g, typeless, 1, D3D12_RESOURCE_FLAG_NONE, kRest, nullptr);
}

void Matrix(Gpu& g)
{
    native::DepthResolveDx12 reference;

    for (const auto& f : kFormats)
    {
        if (!native::DepthResolveFormat(f.typeless))
        {
            char what[160];
            std::snprintf(what, sizeof(what),
                          "%s: DepthResolveFormat says no, so the finder keeps the copy and compute resolve", f.name);
            Check(f.typeless == DXGI_FORMAT_R24G8_TYPELESS, what);
            continue;
        }

        for (UINT samples : { 2u, 4u, 8u })
        {
            if (!Supported(g, f.dsv, samples))
            {
                std::printf("skip  %s %ux: not supported by this device\n", f.name, samples);
                continue;
            }

            for (UINT quality : { 0u, (UINT) D3D12_STANDARD_MULTISAMPLE_PATTERN })
                for (bool reversed : { true, false })
                {
                    char what[256];
                    const char* dir = reversed ? "reversed" : "standard";
                    const char* pattern = quality == 0 ? "" : " standard-pattern";

                    // Reference: the compute resolve of the buffer itself (the finder's previous path, minus its copy)
                    GameDepth depth;

                    if (!MakeGameDepth(g, depth, f.typeless, f.dsv, samples, false, quality))
                    {
                        std::snprintf(what, sizeof(what), "%s %ux%s %s: test buffer made", f.name, samples, pattern,
                                      dir);
                        Check(false, what);
                        continue;
                    }

                    Draw(g, g.list.Get(), depth, 0.25f, 0.75f, 0);
                    const auto toRead = Transition(depth.resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kRest);
                    g.list->ResourceBarrier(1, &toRead);
                    g.Submit();

                    auto r32 = Texture(g, DXGI_FORMAT_R32_TYPELESS, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       kRest, nullptr);
                    const native::DepthResolveDx12::Job job { depth.resource.Get(), f.view, r32.Get() };
                    const char* refFailed = reference.Run(g.queue.Get(), &job, 1, reversed, kRest);
                    g.Wait();
                    const auto expected =
                        refFailed == nullptr ? Read(g, r32.Get(), DXGI_FORMAT_R32_TYPELESS) : std::vector<float> {};

                    // In the list, from DEPTH_WRITE as a game's list leaves it bound, on a fresh buffer of each kind
                    for (bool deny : { false, true })
                    {
                        GameDepth game;

                        if (!MakeGameDepth(g, game, f.typeless, f.dsv, samples, deny, quality))
                            continue;

                        auto slot = Slot(g, f.typeless);
                        Draw(g, g.list.Get(), game, 0.25f, 0.75f, 0);
                        const char* failed = native::RecordDepthResolve(g.list.Get(), game.resource.Get(),
                                                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, slot.Get(),
                                                                        kRest, f.view, reversed);
                        // Back where the source was: one more barrier from DEPTH_WRITE is legal only if it is there
                        const auto after = Transition(game.resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                      D3D12_RESOURCE_STATE_DEPTH_READ);
                        g.list->ResourceBarrier(1, &after);
                        g.Submit();

                        const auto got = Read(g, slot.Get(), f.typeless);
                        int differ = 0;
                        const float worst = Compare(got, expected, f.step, &differ);
                        const int nearValue = Count(got, reversed ? 0.75f : 0.25f, f.step);
                        const int messages = DebugMessages(g);

                        std::snprintf(
                            what, sizeof(what),
                            "%s %ux%s %s%s: same as the compute resolve (%d pixels differ, worst %.3g), %d nearest "
                            "pixels, %d debug messages",
                            f.name, samples, pattern, dir, deny ? " DENY_SHADER_RESOURCE" : "", differ, worst,
                            nearValue, messages);
                        Check(failed == nullptr && refFailed == nullptr && differ == 0 && nearValue > 0 &&
                                  messages == 0,
                              what);
                    }
                }
        }
    }

    reference.Release();
}

// Two lists over one D32S8 4x buffer, the way a game splits its frame: the world list draws the edge triangle at 0.75
// over a 0.25 clear and takes its stretch into slot A; the other resets the buffer (clear to 0.1) and draws a small
// triangle at 0.9, taking its stretch into slot B. Recorded B first, executed in both orders: each slot holds its own
// list's stretch.
void Order(Gpu& g)
{
    const Format& f = kFormats[1];

    if (!Supported(g, f.dsv, 4))
        return;

    for (bool worldFirst : { true, false })
    {
        GameDepth game;
        MakeGameDepth(g, game, f.typeless, f.dsv, 4, true);
        auto slotA = Slot(g, f.typeless);
        auto slotB = Slot(g, f.typeless);

        // Recorded weapon list first, world list second, whatever order they run in
        g.allocator2->Reset();
        g.list2->Reset(g.allocator2.Get(), nullptr);
        Draw(g, g.list2.Get(), game, 0.1f, 0.9f, 1);
        native::RecordDepthResolve(g.list2.Get(), game.resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, slotB.Get(),
                                   kRest, f.view, true);
        g.list2->Close();

        Draw(g, g.list.Get(), game, 0.25f, 0.75f, 0);
        native::RecordDepthResolve(g.list.Get(), game.resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, slotA.Get(),
                                   kRest, f.view, true);
        g.list->Close();

        ID3D12CommandList* lists[2] = { g.list.Get(), g.list2.Get() };

        if (!worldFirst)
            std::swap(lists[0], lists[1]);

        g.queue->ExecuteCommandLists(2, lists);
        g.Wait();
        g.allocator->Reset();
        g.list->Reset(g.allocator.Get(), nullptr);

        const auto a = Read(g, slotA.Get(), f.typeless);
        const auto b = Read(g, slotB.Get(), f.typeless);
        const int aWorld = Count(a, 0.75f, f.step), aBack = Count(a, 0.25f, f.step);
        const int bWeapon = Count(b, 0.9f, f.step), bBack = Count(b, 0.1f, f.step);
        const int total = (int) (kWidth * kHeight);
        const int messages = DebugMessages(g);
        char what[256];
        std::snprintf(what, sizeof(what),
                      "two lists, %s first: slot A holds the world stretch (%d+%d of %d), slot B the reset and the "
                      "weapon (%d+%d of %d), %d debug messages",
                      worldFirst ? "world" : "weapon", aWorld, aBack, total, bWeapon, bBack, total, messages);
        Check(aWorld > total / 3 && aWorld + aBack == total && bWeapon > 0 && bWeapon < total / 4 &&
                  bWeapon + bBack == total && messages == 0,
              what);
    }
}
} // namespace

int main(int argc, char** argv)
{
    const bool warp = argc > 1 && std::strcmp(argv[1], "warp") == 0;
    Gpu g;

    if (!MakeGpu(g, warp))
    {
        std::printf("no D3D12 device\n");
        return 2;
    }

    std::printf("%s, debug layer: %s\n", warp ? "WARP" : "hardware adapter",
                g.info != nullptr ? "on" : "not installed (its check is skipped)");

    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options, sizeof(options));
    std::printf("programmable sample positions tier %d\n", (int) options.ProgrammableSamplePositionsTier);

    if (!native::DepthResolveSupported(g.device.Get()))
    {
        Check(options.ProgrammableSamplePositionsTier == D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED,
              "no tier 1: DepthResolveSupported says no, so the finder keeps the copy and compute resolve");
        std::printf("%s (%d failed)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    Check(true, "tier 1 or higher: DepthResolveSupported says yes");
    Matrix(g);
    Order(g);
    Check(DebugMessages(g) == 0, "the debug layer reported nothing at the end");
    std::printf("%s (%d failed)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
