#include "pch.h"
// python tests\depth_copy_adapter.py <scratch-dir>, then from vcvars64:
// cl /std:c++20 /EHsc /W4 /WX /O2 /I <scratch-dir> tests\nr_depth_copy_thisframe_dx12_gpu.cpp
//    OptiScaler\native\DepthFinderCore.cpp OptiScaler\native\DepthResolveDx12.cpp /link user32.lib
#include "d3d12_harness.h"
#include "depth_copy_adapter.h"
#include <cmath>
#include <future>
#include <thread>

#ifdef DEPTH_COPY_UNAVAILABLE
int main()
{
    std::printf("FAIL CopyThisFrame is not available\n");
    return 1;
}
#else
using namespace d3d12_test;
namespace
{
Checks checks;
constexpr UINT Width = 96, Height = 64;
constexpr auto Rest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
uint64_t Id(const void* p) { return reinterpret_cast<uint64_t>(p); }
struct Swap
{
    HWND window = nullptr;
    ComPtr<IDXGISwapChain1> chain;
    ~Swap()
    {
        chain.Reset();
        if (window)
            DestroyWindow(window);
    }
    bool Create(Gpu& gpu)
    {
        window = CreateWindowW(L"STATIC", L"Depth copy test", WS_POPUP, 0, 0, Width, Height, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
        ComPtr<IDXGIFactory2> factory;
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = Width;
        desc.Height = Height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        return window && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
               SUCCEEDED(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &chain));
    }
};
struct Depth
{
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv {};
    bool Create(Gpu& gpu, UINT samples)
    {
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = Width;
        desc.Height = Height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = samples;
        desc.Format = samples == 1 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G8X24_TYPELESS;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        D3D12_CLEAR_VALUE clear {};
        clear.Format = samples == 1 ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        clear.DepthStencil.Depth = 0.25f;
        texture = Texture(gpu.device.Get(), desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
        // Different stretches deliberately clear to different depths: ignore only the optimized-clear performance note.
        D3D12_MESSAGE_ID note = D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE;
        D3D12_INFO_QUEUE_FILTER filter {};
        filter.DenyList.NumIDs = 1;
        filter.DenyList.pIDList = &note;
        gpu.info->AddStorageFilterEntries(&filter);
        heap = DescriptorHeap(gpu.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
        if (!texture || !heap)
            return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC view {};
        view.Format = samples == 1 ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        view.ViewDimension = samples == 1 ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DMS;
        dsv = heap->GetCPUDescriptorHandleForHeapStart();
        gpu.device->CreateDepthStencilView(texture.Get(), &view, dsv);
        return true;
    }
};
void Select(Depth& depth, ID3D12GraphicsCommandList* list, Swap& swap)
{
    g_installed = true;
    g_core.Start([](const std::string&) {});
    native::DepthBuffer buffer { Id(depth.texture.Get()), Width, Height, uint32_t(depth.texture->GetDesc().Format) };
    for (int frame = 0; frame < 4; ++frame)
    {
        g_core.OnDepthBound(Id(list), true, &buffer);
        g_core.OnViewport(Id(list), float(Width));
        g_core.OnDepthClear(Id(list), buffer, 0.0f);
        for (int draw = 0; draw < 16; ++draw)
            g_core.OnDraw(Id(list), 300, 1);
        g_core.OnDepthBound(Id(list), false, nullptr);
        GenericDepthDx12::OnPresent(swap.chain.Get());
    }
    checks.Check(g_core.Armed() && g_core.CurrentPick().id == buffer.id, "real finder core armed and picked the depth");
}
struct Reader
{
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> seen;
    bool Create(Gpu& gpu)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER table {};
        table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        table.DescriptorTable = { 2, ranges };
        const D3D12_ROOT_SIGNATURE_DESC desc { 1, &table };
        root = RootSignature(gpu.device.Get(), desc);
        Bytes cs;
        if (!root || CompileHlsl("Texture2D<float> copy : register(t0); RWTexture2D<float> seen : register(u0);"
                                 "[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID)"
                                 "{ seen[p.xy] = copy.Load(int3(p.xy,0)); }",
                                 "main", "cs_5_0", cs) != CompileResult::Success)
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline {};
        pipeline.pRootSignature = root.Get();
        pipeline.CS = { cs.data(), cs.size() };
        heap = DescriptorHeap(gpu.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
        seen = Texture(gpu.device.Get(), Width, Height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return heap && seen && SUCCEEDED(gpu.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)));
    }
    void Read(Gpu& gpu, ID3D12GraphicsCommandList* list, const GenericDepthDx12::CopyNow& copy)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = copy.viewFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        gpu.device->CreateShaderResourceView(copy.resource, &srv, cpu);
        cpu.ptr += gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        gpu.device->CreateUnorderedAccessView(seen.Get(), nullptr, &uav, cpu);
        ID3D12DescriptorHeap* heaps[] = { heap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root.Get());
        list->SetPipelineState(pso.Get());
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch(Width / 8, Height / 8, 1);
    }
    bool Matches(Gpu& gpu, float value)
    {
        Bytes bytes;
        if (!gpu.Readback(seen.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, bytes) ||
            bytes.size() != Width * Height * sizeof(float))
            return false;
        for (size_t at = 0; at < bytes.size(); at += sizeof(float))
        {
            float read;
            std::memcpy(&read, bytes.data() + at, sizeof(read));
            if (!std::isfinite(read) || std::fabs(read - value) > 1e-6f)
                return false;
        }
        return true;
    }
};
void Copy(ID3D12GraphicsCommandList* list, Depth& depth, float value, uint64_t vertices)
{
    list->ClearDepthStencilView(depth.dsv, D3D12_CLEAR_FLAG_DEPTH, value, 0, 0, nullptr);
    RecordSnapshot(list, depth.texture.Get(), false, true, "test", vertices);
}
void Lists(Gpu& gpu, Swap& swap, UINT samples, bool across, bool first)
{
    Depth depth;
    Reader readers[2];
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> other;
    if (!checks.Check(depth.Create(gpu, samples) && readers[0].Create(gpu) && readers[1].Create(gpu) &&
                          gpu.CreateList(allocator, other),
                      "copy and reader resources created"))
        return;
    Select(depth, gpu.list.Get(), swap);
    Copy(gpu.list.Get(), depth, 0.25f, 3000);
    if (across)
    {
        Copy(gpu.list.Get(), depth, 0.6f, 100);
        GenericDepthDx12::OnPresent(swap.chain.Get());
    }
    Copy(other.Get(), depth, 0.9f, 300);
    if (!across)
        Copy(gpu.list.Get(), depth, 0.6f, 100);
    auto a = GenericDepthDx12::CopyThisFrame(gpu.list.Get());
    auto b = GenericDepthDx12::CopyThisFrame(other.Get());
    const auto expected = samples == 1 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    const bool valid = a.valid && b.valid && a.resource != b.resource && a.state == Rest && b.state == Rest &&
                       a.width == Width && a.height == Height && a.viewFormat == expected &&
                       a.pick == Id(depth.texture.Get()) && a.vertices == (across ? 100u : 3000u) &&
                       a.ordinal == (across ? 1u : 0u) && a.copiesInList == (across ? 1u : 2u) &&
                       a.acrossPresent == across && !b.acrossPresent;
    checks.Check(valid, "CopyThisFrame returns only this recording's biggest readable stretch, with plane format");
    if (valid)
    {
        readers[0].Read(gpu, gpu.list.Get(), a);
        readers[1].Read(gpu, other.Get(), b);
    }
    OnClose(gpu.list.Get());
    OnClose(other.Get());
    checks.Check(!GenericDepthDx12::CopyThisFrame(gpu.list.Get()).valid,
                 "Close forgets the recording before any Reset");
    checks.Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(other->Close()), "both lists closed");
    ID3D12CommandList* commands[] = { first ? gpu.list.Get() : other.Get(), first ? other.Get() : gpu.list.Get() };
    gpu.queue->ExecuteCommandLists(2, commands);
    if (!checks.Check(gpu.WaitIdle(), "both lists completed"))
        return;
    checks.Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
                 "list reset after the fence");
    checks.Check(valid && readers[0].Matches(gpu, across ? 0.6f : 0.25f) && readers[1].Matches(gpu, 0.9f),
                 "same-list shader reads its own depth in both execution orders");
    checks.Check(gpu.Messages() == 0, "debug layer: zero errors or warnings");
}
void LazyResolves(Gpu& gpu, Swap& swap)
{
    Depth depth;
    Reader reader;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> other;
    if (!checks.Check(depth.Create(gpu, 4) && reader.Create(gpu) && gpu.CreateList(allocator, other), "MSAA setup"))
        return;
    Select(depth, gpu.list.Get(), swap);
    depthConfig.ReplaceGameFsr1.value = 1;
    g_dsv[depth.dsv.ptr] = { depth.texture.Get(), Width, Height, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, false };
    const char* names[] = { "two unbinds: one resolve at the reader", "Clear: resolve recorded before the clear",
                            "other list: immediate resolve", "Close: flushes the pending resolve" };
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        g_copies.Remade(true);
        depthResolves = 0;
        gpu.list->ClearDepthStencilView(depth.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
        RecordSnapshot(gpu.list.Get(), depth.texture.Get(), false, true, "unbind", 300);
        if (scenario == 1)
        {
            OnClear(gpu.list.Get(), depth.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.9f);
            checks.Check(depthResolves == 1, names[scenario]);
            gpu.list->ClearDepthStencilView(depth.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.9f, 0, 0, nullptr);
        }
        if (scenario < 2)
            RecordSnapshot(gpu.list.Get(), depth.texture.Get(), false, true, "unbind", scenario == 1 ? 100 : 300);
        if (scenario == 2)
        {
            RecordSnapshot(other.Get(), depth.texture.Get(), false, true, "unbind", 300);
            checks.Check(depthResolves == 1, names[scenario]);
        }
        if (scenario == 3)
            OnClose(gpu.list.Get());
        auto copy = GenericDepthDx12::CopyThisFrame(gpu.list.Get());
        if (scenario == 3)
            copy = { true, "", g_slots[0].resource, g_backup.view, Rest, Width, Height };
        checks.Check(copy.valid && depthResolves == (scenario == 1 || scenario == 2 ? 2u : 1u), names[scenario]);
        if (copy.valid)
            reader.Read(gpu, gpu.list.Get(), copy);
        checks.Check(gpu.ExecuteAndWait() && copy.valid && reader.Matches(gpu, 0.25f),
                     "delivered depth unchanged, including pre-Clear copy");
        if (scenario == 2)
        {
            OnClose(other.Get());
            checks.Check(SUCCEEDED(other->Close()), "other list closed");
            ID3D12CommandList* lists[] = { other.Get() };
            gpu.queue->ExecuteCommandLists(1, lists);
            checks.Check(gpu.WaitIdle(), "other list completed");
        }
    }
    depthConfig.ReplaceGameFsr1.value = 0;
    checks.Check(gpu.Messages() == 0, "lazy resolve: debug layer clean");
}
void Stats(Gpu& gpu, Swap& swap)
{
    Depth depth;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> other;
    if (!checks.Check(depth.Create(gpu, 1) && gpu.CreateList(allocator, other), "telemetry resources created"))
        return;
    Select(depth, gpu.list.Get(), swap);
    g_copyCounts = {};
    Copy(gpu.list.Get(), depth, 0.25f, 3000);
    Copy(other.Get(), depth, 0.9f, 300);
    Copy(gpu.list.Get(), depth, 0.6f, 100);
    checks.Check(g_copyCounts.copies == 3 && g_copyCounts.repeated == 1,
                 "only the second copy of this buffer in this list is a duplicate");
    OnClose(other.Get());
    checks.Check(SUCCEEDED(other->Close()) && gpu.ExecuteAndWait(), "telemetry copies in the main list completed");
    ID3D12CommandList* commands[] = { other.Get() };
    gpu.queue->ExecuteCommandLists(1, commands);
    checks.Check(gpu.WaitIdle(), "telemetry copy in the other list completed");
    depthLog.clear();
    for (int frame = 0; frame < 600; ++frame)
        GenericDepthDx12::OnPresent(swap.chain.Get());
    std::printf("%s", depthLog.c_str());
    checks.Check(depthLog.find("(1 of 3 copies in 600 frames)") != std::string::npos && g_copyCounts.frames == 0,
                 "real per-frame log includes duplicates and resets the interval");
    checks.Check(gpu.Messages() == 0, "telemetry: debug layer clean");
}
void Refusals(Gpu& gpu)
{
    auto held = [&](bool pending)
    {
        if (pending)
            g_copies.Remade(false);
        std::promise<void> acquired, release;
        auto done = release.get_future();
        std::thread owner(
            [&]
            {
                std::lock_guard lock(g_mutex);
                acquired.set_value();
                done.wait();
            });
        acquired.get_future().wait();
        const auto copy = GenericDepthDx12::CopyThisFrame(gpu.list.Get());
        release.set_value();
        owner.join();
        checks.Check(!copy.valid && std::strcmp(copy.why, pending ? "resolve pending" : "busy") == 0,
                     pending ? "resolve pending returns before the held mutex"
                             : "busy try_lock returns without waiting");
    };
    held(false);
    held(true);
    g_copies.Remade(true);
    checks.Check(std::strcmp(GenericDepthDx12::CopyThisFrame(nullptr).why, "no command list") == 0,
                 "null list refusal");
}
void DeviceChange(Gpu& gpu, Swap& swap, bool warp)
{
    Depth old;
    if (!checks.Check(old.Create(gpu, 1), "old device's depth created"))
        return;
    Select(old, gpu.list.Get(), swap);
    Copy(gpu.list.Get(), old, 0.25f, 300);
    auto before = GenericDepthDx12::CopyThisFrame(gpu.list.Get());
    checks.Check(before.valid && gpu.ExecuteAndWait(), "old device's actual copy completed");
    Gpu next;
    Swap nextSwap;
    Depth depth;
    Reader reader;
    if (!checks.Check(next.Create(warp ? Adapter::Hardware : Adapter::Warp) && nextSwap.Create(next) &&
                          depth.Create(next, 1) && reader.Create(next),
                      "independent second device, same depth geometry"))
        return;
    ComPtr<IUnknown> oldIdentity, newIdentity;
    gpu.device.As(&oldIdentity);
    next.device.As(&newIdentity);
    checks.Check(oldIdentity.Get() != newIdentity.Get(), "device IUnknown identity really changed");
    checks.Check(std::strcmp(GenericDepthDx12::CopyThisFrame(next.list.Get()).why,
                             "the command list is of another device than the copies") == 0,
                 "a foreign device cannot read the old device's slots");
    Select(depth, next.list.Get(), nextSwap);
    Copy(next.list.Get(), depth, 0.7f, 300);
    auto after = GenericDepthDx12::CopyThisFrame(next.list.Get());
    ComPtr<ID3D12Device> owner;
    if (after.resource)
        after.resource->GetDevice(IID_PPV_ARGS(&owner));
    checks.Check(after.valid && owner.Get() == next.device.Get() && g_backup.device == next.device.Get() &&
                     !g_copies.Find(Id(gpu.list.Get()), Id(depth.texture.Get())).valid,
                 "real EnsureBackup remade same-size slots on the new device and cleared old entries");
    if (after.valid)
        reader.Read(next, next.list.Get(), after);
    checks.Check(next.ExecuteAndWait() && after.valid && reader.Matches(next, 0.7f),
                 "new device reads its actual depth copy");
    checks.Check(gpu.Messages() == 0 && next.Messages() == 0, "device change: both debug layers clean");
}
} // namespace
int main(int argc, char** argv)
{
    const bool warp = argc > 1 && std::strcmp(argv[1], "warp") == 0;
    Gpu gpu;
    Swap swap;
    if (!gpu.Create(warp ? Adapter::Warp : Adapter::Hardware) || !swap.Create(gpu))
        return 2;
    for (bool first : { true, false })
    {
        Lists(gpu, swap, 1, false, first);
        Lists(gpu, swap, 1, true, first);
    }
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels {};
    levels.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    levels.SampleCount = 4;
    if (native::DepthResolveSupported(gpu.device.Get()) &&
        SUCCEEDED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) &&
        levels.NumQualityLevels > 0)
    {
        Lists(gpu, swap, 4, false, true);
        LazyResolves(gpu, swap);
    }
    else
        checks.Skip("4x in-list resolve: programmable sample positions tier 1 or D32S8 4x unavailable");
    Refusals(gpu);
    Stats(gpu, swap);
    DeviceChange(gpu, swap, warp);
    return checks.Summary();
}
#endif
