#include "pch.h"
// cl /nologo /std:c++20 /EHsc /W4 /O2 /I <scratch-pch> tests\nr_list_tracking_smoke.cpp
//    OptiScaler\native\DescriptorTrackerDx12.cpp /link d3d12.lib
#include "../OptiScaler/native/ListStateDx12.h"
#include "../OptiScaler/native/RootSignatureMap.h"
#include <cstdio>
#include <vector>
#include <chrono>

static int failures = 0;
static native::spatial_tracking::Device tracking;
static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
template <class T> static T* Pointer(uintptr_t value) { return reinterpret_cast<T*>(value); }

struct FakeHeap final : ID3D12DescriptorHeap
{
    UINT descCalls = 0, deviceCalls = 0;
    D3D12_DESCRIPTOR_HEAP_DESC desc {};
    SIZE_T cpu;
    UINT64 gpu;
    FakeHeap(SIZE_T c, UINT64 g, UINT n, D3D12_DESCRIPTOR_HEAP_TYPE t = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        : cpu(c), gpu(g)
    {
        desc.Type = t;
        desc.NumDescriptors = n;
        desc.Flags = g ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { ++deviceCalls; return E_NOTIMPL; }
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { ++descCalls; return desc; }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override { return { cpu }; }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override { return { gpu }; }
};

struct Event
{
    UINT op, index, count, offset;
    UINT64 value;
};
struct Restore final : native::ListRestoreSink
{
    std::vector<Event> events;
    void Add(UINT op, UINT i = 0, UINT64 v = 0, UINT n = 0, UINT off = 0) { events.push_back({ op, i, n, off, v }); }
    void SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* h) override { Add(0, 0, uintptr_t(h[0]), n); }
    void SetGraphicsRootSignature(ID3D12RootSignature* rs) override { Add(1, 0, uintptr_t(rs)); }
    void SetGraphicsRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override { Add(2, i, h.ptr); }
    void SetGraphicsRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) override { Add(3, i, v); }
    void SetGraphicsRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) override { Add(4, i, v); }
    void SetGraphicsRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) override { Add(5, i, v); }
    void SetGraphicsRoot32BitConstants(UINT i, UINT n, const void* data, UINT off) override
    {
        for (UINT j = 0; j < n; ++j)
            Add(6, i, static_cast<const UINT*>(data)[j], n, off + j);
    }
    void SetComputeRootSignature(ID3D12RootSignature* rs) override { Add(7, 0, uintptr_t(rs)); }
    void SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override { Add(8, i, h.ptr); }
    void SetComputeRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) override { Add(9, i, v); }
    void SetComputeRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) override { Add(10, i, v); }
    void SetComputeRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) override { Add(11, i, v); }
    void SetComputeRoot32BitConstants(UINT i, UINT n, const void* data, UINT off) override
    {
        for (UINT j = 0; j < n; ++j)
            Add(12, i, static_cast<const UINT*>(data)[j], n, off + j);
    }
    void SetPipelineState(ID3D12PipelineState* p) override { Add(13, 0, uintptr_t(p)); }
};

static void Lists()
{
    using namespace native;
    ListStateTable table;
    const void* list = Pointer<void>(0x1000);
    auto* initial = Pointer<ID3D12PipelineState>(0x1200);
    table.Begin(list, initial);
    ListState& s = *table.Find(list);
    s.device = &tracking;
    FakeHeap heap(0x700000, 0x800000, 16);
    descriptors::OnCreateHeap(&heap, heap.desc, 32, &tracking);
    ID3D12DescriptorHeap* heaps[] = { &heap };
    OnSetDescriptorHeaps(s, 1, heaps);
    Check(s.heapRefs[0].heap != nullptr, "per-list heap reference");
    auto* rs = Pointer<ID3D12RootSignature>(0x1300);
    OnSetGraphicsRootSignature(s, rs);
    OnSetComputeRootSignature(s, rs);
    OnSetGraphicsRootDescriptorTable(s, 0, { heap.gpu });
    OnSetComputeRootDescriptorTable(s, 0, { heap.gpu + 32 });
    OnSetGraphicsRootConstantBufferView(s, 1, 0x1010);
    OnSetComputeRootConstantBufferView(s, 1, 0x2010);
    OnSetGraphicsRootShaderResourceView(s, 2, 0x1020);
    OnSetComputeRootShaderResourceView(s, 2, 0x2020);
    OnSetGraphicsRootUnorderedAccessView(s, 3, 0x1030);
    OnSetComputeRootUnorderedAccessView(s, 3, 0x2030);
    const UINT values[] = { 11, 22, 33 };
    OnSetGraphicsRoot32BitConstants(s, 4, 3, values, 1);
    OnSetGraphicsRoot32BitConstant(s, 4, 99, 2);
    OnSetComputeRoot32BitConstants(s, 4, 3, values, 1);
    OnSetComputeRoot32BitConstant(s, 4, 88, 5);
    UINT got = 0;
    Check(ReadGraphicsConstant(s, 4, 2, &got) && got == 99, "graphics partial rewrite");
    Check(!ReadGraphicsConstant(s, 4, 0, &got), "unwritten graphics constant");
    Restore sink;
    EmitRestore(s, sink);
    const UINT order[] = { 0, 1, 2, 3, 4, 5, 6, 6, 6, 7, 8, 9, 10, 11, 12, 12, 12, 12, 13 };
    Check(sink.events.size() == std::size(order), "restore event count");
    for (size_t i = 0; i < sink.events.size() && i < std::size(order); ++i)
        Check(sink.events[i].op == order[i], "heaps/signatures/arguments/PSO restore order");
    if (sink.events.size() == std::size(order))
    {
        Check(sink.events[7].value == 99 && sink.events[7].offset == 2, "restored graphics partial value");
        Check(sink.events[17].offset == 5 && sink.events[17].count == 1, "sparse constants restored as runs");
    }
    OnSetGraphicsRootSignature(s, rs);
    OnSetComputeRootSignature(s, rs);
    Check(s.graphicsConstantCount == 3 && s.computeConstantCount == 4, "same signature keeps arguments");
    s.inEnvelope = true;
    OnSetGraphicsRootShaderResourceView(s, 2, 0xdead);
    OnSetComputeRoot32BitConstant(s, 4, 0, 1);
    Check(s.graphicsSrv[2] == 0x1020 && ReadComputeConstant(s, 4, 1, &got) && got == 11, "envelope calls ignored");
    s.inEnvelope = false;
    OnSetDescriptorHeaps(s, 0, nullptr);
    Check(s.graphicsTableMask == 0 && s.computeTableMask == 0, "changed heaps invalidate both tables");
    OnSetGraphicsRootSignature(s, Pointer<ID3D12RootSignature>(0x1400));
    Check(s.graphicsConstantCount == 0 && s.graphicsSrvMask == 0 && s.computeConstantCount == 4,
          "changed graphics signature clears only graphics");
    D3D12_RENDER_PASS_RENDER_TARGET_DESC rt {};
    rt.cpuDescriptor.ptr = 0x7700;
    OnBeginRenderPass(s, 1, &rt);
    Check(s.inRenderPass && s.renderPassRtv.ptr == 0x7700 && s.rtv0.ptr == 0x7700, "render pass RTV0");
    OnEndRenderPass(s);
    Check(!s.inRenderPass && !s.rtv0Valid, "render pass ended");
    auto* input = Pointer<ID3D12Resource>(0x1600);
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { input, 0, D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    OnResourceBarrier(s, 1, &b);
    OnTransition(s, Pointer<ID3D12Resource>(0x1700), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ListState::Transition t;
    Check(FindTransition(s, input, 0, &t) && t.after == b.Transition.StateAfter,
          "input state remains exact after another resource transition");
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    OnResourceBarrier(s, 1, &b);
    Check(FindTransition(s, input, 0, &t) && t.pending, "split BEGIN is not readable");
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
    OnResourceBarrier(s, 1, &b);
    Check(FindTransition(s, input, 0, &t) && !t.pending, "split END state known");
    OnTransition(s, input, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, true);
    Check(FindTransition(s, input, 0, &t) && t.enhanced, "enhanced transition marker");
    OnClearState(s, initial);
    Check(FindTransition(s, input, 0, &t) && t.enhanced, "ClearState preserves transition history");
    for (UINT i = 0; i < 8; ++i)
        OnTransition(s, Pointer<ID3D12Resource>(0x2000 + i * 16), 0, D3D12_RESOURCE_STATE_COMMON);
    Check(!FindTransition(s, input, 0, &t), "transition ring evicts old state");
    s.forwarding = true;
    const UINT generation = s.generation;
    table.Begin(list, initial);
    Check(s.generation > generation && !s.forwarding && s.transitionCount == 0, "Reset clears flags and transitions");
    OnSetGraphicsRootSignature(s, rs);
    OnSetComputeRootSignature(s, rs);
    OnExecuteBundle(s);
    Check(s.incomplete && !s.pso && !s.graphicsSignature && !s.computeSignature,
          "bundle invalidates inherited PSO and roots");
    table.Forget(list);
    Check(!table.Find(list), "final release forgets list");
    for (UINT i = 0; i < 2048; ++i)
        table.Begin(Pointer<void>(0x1000000 + i * 16));
    Check(table.Used() == 2048, "list table grows beyond 512");
    for (UINT i = 0; i < 2048; ++i)
        table.Forget(Pointer<void>(0x1000000 + i * 16));
    Check(table.Used() == 0 && table.TakeUntracked() == 0, "growing list entries retired");
    std::puts("lists: restore, constants, render pass, transitions, growth checked");
}

static void Descriptors()
{
    using namespace native::descriptors;
    FakeHeap sampler(0x400000, 0xb00000, 16, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    OnCreateHeap(&sampler, sampler.desc, 16, &tracking);
    for (UINT i = 0; i < 128; ++i)
        Check(FindHeap(&sampler, &tracking).heap != nullptr, "ignored sampler is known metadata");
    Check(sampler.descCalls == 0 && sampler.deviceCalls == 0, "known sampler needs no late object queries");
    FakeHeap cpu(0x100000, 0, 16), gpu(0x200000, 0x900000, 16), rtv(0x300000, 0, 16, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    OnCreateHeap(&cpu, cpu.desc, 32, &tracking);
    OnCreateHeap(&gpu, gpu.desc, 32, &tracking);
    OnCreateHeap(&rtv, rtv.desc, 16, &tracking);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    auto* resource = Pointer<ID3D12Resource>(0xabc0);
    OnCreateView(Kind::Srv, resource, &srv, { cpu.cpu }, &tracking);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv { 0x123400, 256 };
    OnCreateView(Kind::Cbv, nullptr, &cbv, { cpu.cpu + 32 }, &tracking);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray = { 2, 3, 4, 1 };
    OnCreateView(Kind::Uav, resource, &uav, { cpu.cpu + 64 }, &tracking);
    OnCopySimple(3, { gpu.cpu }, { cpu.cpu }, cpu.desc.Type, &tracking);
    View v;
    Check(Lookup({ gpu.cpu }, &v, &tracking) && v.resource == resource && v.format == srv.Format,
          "CPU to visible simple copy");
    auto ref = FindHeap(&gpu, &tracking);
    Check(LookupGpu(ref, { gpu.gpu + 32 }, &v) && v.va == cbv.BufferLocation && !v.resource && v.size == 256,
          "CBV GPU lookup");
    Check(LookupGpu(ref, { gpu.gpu + 64 }, &v) && v.kind == uint8_t(Kind::Uav) && v.mip == 2 && v.plane == 1 &&
              v.firstSlice == 3 && v.sliceCount == 4,
          "UAV view metadata copied");
    Check(!Lookup({ cpu.cpu + 1 }, &v, &tracking) && !Lookup({ cpu.cpu + 16 * 32 }, &v, &tracking),
          "CPU handles aligned and end exclusive");
    Check(!LookupGpu(ref, { gpu.gpu + 16 * 32 }, &v) && !LookupGpu(ref, { gpu.gpu + 1 }, &v),
          "GPU bounds and alignment");
    const D3D12_CPU_DESCRIPTOR_HANDLE dst[] = { { gpu.cpu + 4 * 32 }, { gpu.cpu + 8 * 32 } };
    const D3D12_CPU_DESCRIPTOR_HANDLE src[] = { { cpu.cpu }, { cpu.cpu + 2 * 32 } };
    const UINT dn[] = { 1, 2 }, sn[] = { 2, 1 };
    OnCopy(2, dst, dn, 2, src, sn, cpu.desc.Type, &tracking);
    Check(Lookup(dst[0], &v, &tracking) && v.kind == uint8_t(Kind::Srv), "ranged CPU to visible first range");
    Check(Lookup(dst[1], &v, &tracking) && v.va == cbv.BufferLocation, "ranged copy across source boundary");
    Check(Lookup({ gpu.cpu + 9 * 32 }, &v, &tracking) && v.mip == 2, "ranged copy across destination boundary");
    const D3D12_CPU_DESCRIPTOR_HANDLE dstUnit[] = { { gpu.cpu + 4 * 32 }, { gpu.cpu + 5 * 32 } };
    const D3D12_CPU_DESCRIPTOR_HANDLE srcUnit[] = { { cpu.cpu }, { cpu.cpu + 32 } };
    OnCopy(2, dstUnit, nullptr, 2, srcUnit, nullptr, cpu.desc.Type, &tracking);
    Check(Lookup(dstUnit[1], &v, &tracking) && v.va == cbv.BufferLocation, "null range sizes mean one descriptor");
    D3D12_RENDER_TARGET_VIEW_DESC rd {};
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rd.Texture2D.MipSlice = 1;
    OnCreateView(Kind::Rtv, resource, &rd, { rtv.cpu }, &tracking);
    OnCopySimple(1, { rtv.cpu + 16 }, { rtv.cpu }, rtv.desc.Type, &tracking);
    Check(Lookup({ rtv.cpu + 16 }, &v, &tracking) && v.kind == uint8_t(Kind::Rtv) && v.mip == 1, "RTV shadow copy");
    OnCopySimple(1, { gpu.cpu + 6 * 32 }, { 0xdead }, cpu.desc.Type, &tracking);
    Check(!Lookup({ gpu.cpu + 6 * 32 }, &v, &tracking), "unknown source clears destination");
    const D3D12_CPU_DESCRIPTOR_HANDLE unknown[] = { { 0xdead } };
    const D3D12_CPU_DESCRIPTOR_HANDLE stale[] = { { gpu.cpu + 32 } };
    OnCopy(1, stale, nullptr, 1, unknown, nullptr, cpu.desc.Type, &tracking);
    Check(!Lookup(stale[0], &v, &tracking), "unknown ranged source clears destination");
    OnCreateView(Kind::Srv, nullptr, &srv, { cpu.cpu + 10 * 32 }, &tracking);
    Check(Lookup({ cpu.cpu + 10 * 32 }, &v, &tracking) && !v.resource,
          "null descriptor does not preserve stale resource");
    constexpr UINT iterations = 1000000;
    volatile uintptr_t observed = 0;
    auto start = std::chrono::steady_clock::now();
    for (UINT i = 0; i < iterations; ++i)
    {
        Lookup({ cpu.cpu }, &v, &tracking);
        observed = uintptr_t(v.resource);
    }
    double lookupNs =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations;
    start = std::chrono::steady_clock::now();
    for (UINT i = 0; i < iterations; ++i)
        OnCopySimple(1, { gpu.cpu }, { cpu.cpu }, cpu.desc.Type, &tracking);
    double copyNs =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations;
    std::printf("bench: Lookup %.2f ns, OnCopySimple(1) %.2f ns (informative < 50 ns), observed %zx\n", lookupNs,
                copyNs, size_t(observed));
    FakeHeap replacement(gpu.cpu + 32, 0xa00000, 4);
    OnCreateHeap(&replacement, replacement.desc, 32, &tracking);
    Check(!LookupGpu(ref, { gpu.gpu }, &v), "overlapping heap invalidates old GPU reference");
    Check(!Lookup({ gpu.cpu }, &v, &tracking) && !Lookup({ gpu.cpu + 10 * 32 }, &v, &tracking),
          "overlap retires entire old CPU range");
    OnCreateView(Kind::Srv, resource, &srv, { replacement.cpu }, &tracking);
    Check(LookupGpu(FindHeap(&replacement, &tracking), { replacement.gpu }, &v) && v.resource == resource,
          "replacement heap works");
    std::puts("descriptors: simple/ranged copies, metadata, overlap checked");
}

static void Roots()
{
    using namespace native::rootsig;
    D3D12_DESCRIPTOR_RANGE ranges[] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 3, 2, 5 },
                                        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 1, 2,
                                          D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND } };
    D3D12_ROOT_PARAMETER p[4] {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable = { 2, ranges };
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = { 4, 3, 4 };
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[2].Descriptor = { 2, 0 };
    p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[3].Descriptor = { 1, 0 };
    p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC desc { 4, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error);
    Check(SUCCEEDED(hr), "serialize RS 1.0");
    if (error)
        error->Release();
    if (!blob)
        return;
    auto* rs = Pointer<ID3D12RootSignature>(0x33000);
    OnCreate(rs, blob->GetBufferPointer(), blob->GetBufferSize());
    std::memset(blob->GetBufferPointer(), 0, blob->GetBufferSize());
    blob->Release();
    auto slot = Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 2, true);
    Check(slot.kind == Slot::Table && slot.parameter == 0 && slot.offset == 8 && slot.count == 2,
          "RS 1.0 APPEND offset");
    slot = Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 4, 3, false);
    Check(slot.kind == Slot::Constants && slot.parameter == 1 && slot.count == 4, "pixel constants");
    Check(Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 4, 3, true).kind == Slot::None, "pixel hidden from compute");
    Check(Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, false).kind == Slot::None &&
              Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, true).kind == Slot::None,
          "vertex hidden from pixel and compute");
    slot = Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2, 0, true);
    Check(slot.kind == Slot::RootDescriptor && slot.parameter == 2, "ALL root CBV visible to compute");
    Check(Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 3, false).kind == Slot::None, "wrong register space missing");
    D3D12_DESCRIPTOR_RANGE1 ranges1[2] {};
    for (UINT i = 0; i < 2; ++i)
        ranges1[i] = { ranges[i].RangeType,
                       ranges[i].NumDescriptors,
                       ranges[i].BaseShaderRegister,
                       ranges[i].RegisterSpace,
                       D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,
                       ranges[i].OffsetInDescriptorsFromTableStart };
    D3D12_ROOT_PARAMETER1 p1[3] {};
    p1[0].ParameterType = p[0].ParameterType;
    p1[0].DescriptorTable = { 2, ranges1 };
    p1[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p1[1].ParameterType = p[1].ParameterType;
    p1[1].Constants = p[1].Constants;
    p1[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p1[2].ParameterType = p[2].ParameterType;
    p1[2].Descriptor = { 2, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
    p1[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd {};
    vd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vd.Desc_1_1 = { 3, p1, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    blob = error = nullptr;
    hr = D3D12SerializeVersionedRootSignature(&vd, &blob, &error);
    Check(SUCCEEDED(hr), "serialize RS 1.1");
    if (error)
        error->Release();
    if (!blob)
        return;
    OnCreate(rs, blob->GetBufferPointer(), blob->GetBufferSize());
    blob->Release();
    slot = Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 2, false);
    Check(slot.kind == Slot::Table && slot.offset == 6 && slot.count == 1, "RS 1.1 replacement table");
    Check(Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 2, true).kind == Slot::None,
          "RS 1.1 PIXEL table hidden from compute");
    slot = Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 4, 3, true);
    Check(slot.kind == Slot::Constants && slot.count == 4, "RS 1.1 ALL constants");
    Check(Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2, 0, false).kind == Slot::RootDescriptor &&
              Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2, 0, true).kind == Slot::None,
          "RS 1.1 PIXEL root CBV");
    OnCreate(rs, nullptr, 0);
    Check(Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 4, 3, true).kind == Slot::None,
          "missing blob retires stale signature map");
    std::puts("root signatures: RS 1.0/1.1, APPEND, constants, CBV, visibility checked");
}

int main()
{
    tracking.ready.store(true);
    tracking.generation = 1;
    Lists();
    Descriptors();
    Roots();
    std::printf("%s: list tracking (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
