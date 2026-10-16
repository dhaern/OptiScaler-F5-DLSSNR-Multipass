#include "pch.h"
#include "SpatialPass_Dx12.h"
#include <Config.h>
#include <State.h>
#include <Util.h>
#include <framegen/IFGFeature_Dx12.h>
#include <hooks/Hook_Utils.h>
#include <hooks/ListVtables.h>
#include <misc/ThreadTls.h>
#include <native/DepthFinderCore.h>
#include <native/Fsr1Pass.h>
#include <native/InListUpscale.h>
#include <native/ListStateDx12.h>
#include <native/NativeLowLatencyRule.h>
#include <native/RootSignatureMap.h>
#include <native/VirtualUpscalerParams.h>
#include <detours/detours.h>
#include <atomic>
#include <tuple>

namespace
{
using namespace native;
using namespace native::fsr1;
using List = ID3D12GraphicsCommandList;
struct SpatialTag;
constinit ListVtables<SpatialTag> g_tables;
constinit ListStateTable g_lists;
constinit std::atomic<bool> g_installed {};
constexpr UINT kCreationSlots[] = { 10, 11, 12, 14, 16, 17, 18, 19, 20, 23, 24 };
constinit std::atomic<void*> g_creationCode[std::size(kCreationSlots)] {};
constinit std::atomic_flag g_installOwner = ATOMIC_FLAG_INIT;
struct InstallClaim
{
    bool owned = !g_installOwner.test_and_set(std::memory_order_acquire);
    ~InstallClaim()
    {
        if (owned)
            g_installOwner.clear(std::memory_order_release);
    }
};
constinit std::atomic<uint64_t> g_presents {}, g_fgPresents {}, g_easuFrame {}, g_rcasFrame {}, g_calls {}, g_binds {};
constinit std::atomic<uint64_t> g_shapes[3] {}, g_refusals[size_t(Refusal::Count)] {}, g_constants {}, g_textureRect {};
constinit std::atomic<uint32_t> g_examined {}, g_overflow {}, g_untracked {}, g_patchLogs {}, g_planLogs {};
constinit std::atomic<bool> g_dumped {};
constinit std::atomic<const void*> g_seenLists[8] {};
constinit std::atomic<UINT> g_inW {}, g_inH {}, g_outW {}, g_outH {}, g_inFormat {}, g_outFormat {}, g_thread {};
// Scene jitter: the frame's sub-pixel offset (two floats, render pixels; 0 = off), set at the present while the pass is
// replaced. Passes with a depth-stencil view and a viewport of the render size are drawn with it.
constinit std::atomic<uint64_t> g_jitter {}, g_jitteredDraws {}, g_sceneViewports {}, g_applied {}, g_stale {};
constinit std::atomic<uint32_t> g_phase {};
constinit std::atomic<uint64_t> g_otherDraws {};
constinit std::atomic<int> g_jitterOff {}; // 1: motion vectors
constinit std::atomic<bool> g_motionVectorsDecided {};

constinit std::atomic<uint64_t> g_lastPassPresent {}, g_hudless {}, g_hudlessRefused {}, g_midList {};
constinit std::atomic<bool> g_passSinceAdvance {}, g_movedLastFrame {};
constinit std::atomic<double> g_fgPresentMs {};
constinit std::atomic<uint64_t> g_swapPresents {}, g_beforeScene {};
constinit std::atomic<int> g_frameSource {};
constinit std::atomic<ID3D12Resource*> g_hudlessSource {};
constinit std::atomic<uint64_t> g_hudlessFrame {}, g_hudlessMissed {}, g_hudlessOther {}, g_hudlessSize {},
    g_hudlessInPass {};
constinit std::atomic<int> g_hudlessIndex {};
constinit std::atomic<bool> g_hudlessMatch[2] {};
constinit std::atomic<uint32_t> g_otherViewport {}; // last depth-bound viewport not of the render size, w << 16 | h
constinit std::atomic<int> g_constantParameter { -1 };
constexpr UINT kMaxPipelines = 64, kMaxMapped = 4096;
struct Pipeline
{
    ID3D12PipelineState* pso = nullptr;
    ID3D12RootSignature* signature = nullptr;
    Facts facts;
    rootsig::Slot input {}, output {}, constants {};
};
constinit std::atomic<Pipeline*> g_pipelines[kMaxPipelines] {};
Pipeline* FindPipeline(ID3D12PipelineState* pso)
{
    if (pso)
        for (auto& slot : g_pipelines)
            if (auto* p = slot.load(std::memory_order_acquire); p && p->pso == pso)
                return p;
    return nullptr;
}
void BindPipeline(ListState& s, ID3D12PipelineState* pso)
{
    OnSetPipelineState(s, pso);
    s.user[1] = reinterpret_cast<uintptr_t>(FindPipeline(pso));
    if (s.user[1])
        g_binds.fetch_add(1, std::memory_order_relaxed);
}
struct Mapped
{
    std::atomic<ID3D12Resource*> resource {};
    std::atomic<const uint8_t*> cpu {};
    std::atomic<UINT64> va {}, size {};
    std::atomic<UINT> maps {};
};
constinit Mapped g_mapped[kMaxMapped] {};
constinit std::atomic<uint64_t> g_mapCalls {}, g_mapsNotKept {};
bool ReadMapped(UINT64 va, void* out, size_t bytes)
{
    for (auto& m : g_mapped)
    {
        auto* resource = m.resource.load(std::memory_order_acquire);
        if (!resource || resource == reinterpret_cast<ID3D12Resource*>(1))
            continue;
        const UINT64 start = m.va.load(), size = m.size.load();
        if (va < start || va - start > size || bytes > size - (va - start))
            continue;
        auto* cpu = m.cpu.load();
        SIZE_T read = 0;
        // Release/Unmap may race this optional refinement. ReadProcessMemory fails instead of faulting on retired
        // pages.
        return ReadProcessMemory(GetCurrentProcess(), cpu + (va - start), out, bytes, &read) && read == bytes &&
               m.resource.load(std::memory_order_acquire) == resource && m.va.load() == start && m.cpu.load() == cpu;
    }
    return false;
}
void DropMapped(IUnknown* object, bool unmap = false)
{
    for (auto& m : g_mapped)
        if (m.resource.load(std::memory_order_acquire) == object && (!unmap || m.maps.fetch_sub(1) == 1))
            m.resource.store(nullptr, std::memory_order_release);
}
#define ORIGINAL(Name, Type)                                                                                           \
    using PFN_##Name = rewrite_signature<decltype(&Type::Name)>::type;                                                 \
    PFN_##Name o_##Name = nullptr
ORIGINAL(CreateGraphicsPipelineState, ID3D12Device);
ORIGINAL(CreateComputePipelineState, ID3D12Device);
ORIGINAL(CreateCommandList, ID3D12Device);
ORIGINAL(CreateCommittedResource, ID3D12Device);
ORIGINAL(CreatePlacedResource, ID3D12Device);
ORIGINAL(CreateDescriptorHeap, ID3D12Device);
ORIGINAL(CreateRootSignature, ID3D12Device);
ORIGINAL(CreateConstantBufferView, ID3D12Device);
ORIGINAL(CreateShaderResourceView, ID3D12Device);
ORIGINAL(CreateUnorderedAccessView, ID3D12Device);
ORIGINAL(CreateRenderTargetView, ID3D12Device);
ORIGINAL(CopyDescriptors, ID3D12Device);
ORIGINAL(CopyDescriptorsSimple, ID3D12Device);
ORIGINAL(CreatePipelineLibrary, ID3D12Device1);
ORIGINAL(CreatePipelineState, ID3D12Device2);
ORIGINAL(CreateCommandList1, ID3D12Device4);
ORIGINAL(LoadGraphicsPipeline, ID3D12PipelineLibrary);
ORIGINAL(LoadComputePipeline, ID3D12PipelineLibrary);
ORIGINAL(LoadPipeline, ID3D12PipelineLibrary1);
ORIGINAL(Map, ID3D12Resource);
ORIGINAL(Unmap, ID3D12Resource);
ORIGINAL(Release, IUnknown);
PFN_Release o_ResourceRelease = nullptr;
ORIGINAL(Reset, List);
ORIGINAL(Close, List);
ORIGINAL(ExecuteCommandLists, ID3D12CommandQueue);
#undef ORIGINAL
spatial_tracking::Device* TrackingDevice(ID3D12Device* device)
{
    auto* tracking = spatial_tracking::Find(device);
    if (spatial_tracking::Available(tracking) || !g_installed.load(std::memory_order_acquire))
        return tracking;
    // Device factories can bypass the D3D12CreateDevice export. Observe at an already-hooked cold creation boundary,
    // never from draw/bind. Install's same-code check establishes coverage; it does not imply old objects were seen.
    SpatialPassDx12::Install(device);
    return spatial_tracking::Find(device);
}
void RegisterPipeline(ID3D12Device* device, ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& code, Facts facts,
                      ID3D12RootSignature* signature, const char* source)
{
    if (!pso)
        return;
    for (auto& slot : g_pipelines)
        if (auto* old = slot.load(std::memory_order_acquire); old && old->pso == pso)
            slot.store(nullptr, std::memory_order_release);
    g_examined.fetch_add(1, std::memory_order_relaxed);
    facts.shader = Classify(code.pShaderBytecode, code.BytecodeLength);
    if (facts.shader.pass == Pass::None)
        return;
    auto* p = static_cast<Pipeline*>(VirtualAlloc(nullptr, sizeof(Pipeline), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p)
    {
        g_overflow.fetch_add(1);
        return;
    }
    new (p) Pipeline {};
    p->pso = pso;
    p->facts = facts;
    p->signature = signature;
    if (!signature && RootSignaturePart(code.pShaderBytecode, code.BytecodeLength).ptr)
    {
        // A PSO can take its signature from RTS0. The API accepts the shader container; the raw RTS0 payload is not a
        // blob.
        if (SUCCEEDED(o_CreateRootSignature(device, 0, code.pShaderBytecode, code.BytecodeLength,
                                            IID_PPV_ARGS(&p->signature))))
            rootsig::OnCreate(p->signature, code.pShaderBytecode, code.BytecodeLength, TrackingDevice(device));
    }
    const bool compute = facts.shader.stage == Stage::Compute;
    p->input = rootsig::Find(p->signature, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, facts.shader.textureRegister,
                             facts.shader.textureSpace, compute);
    p->output = rootsig::Find(p->signature, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, facts.shader.outputRegister,
                              facts.shader.outputSpace, compute);
    p->constants = rootsig::Find(p->signature, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, facts.shader.constantRegister,
                                 facts.shader.constantSpace, compute);
    for (auto& slot : g_pipelines)
    {
        Pipeline* empty = nullptr;
        if (!slot.compare_exchange_strong(empty, p, std::memory_order_release, std::memory_order_relaxed))
            continue;
        const auto& shader = facts.shader;
        LOG_INFO("Spatial pass: pipeline {:X} is FSR1 {} ({}), texture t{} space {}, constants b{} space {}, root "
                 "signature {:X}, render target format {}, blend {} ({}); {}{}",
                 size_t(pso), shader.pass == Pass::Easu ? "EASU" : "RCAS",
                 shader.precision == Precision::Half ? "half" : "fp32", shader.textureRegister, shader.textureSpace,
                 shader.constantRegister, shader.constantSpace, size_t(p->signature), int(facts.rtvFormat),
                 facts.blend ? "on" : "off", shader.why, source, shader.uber ? ", uber" : "");
        return;
    }
    g_overflow.fetch_add(1, std::memory_order_relaxed);
    LOG_WARN("Spatial pass: more than {} FSR1 pipelines, {:X} is not followed", kMaxPipelines, size_t(pso));
    if (!signature && p->signature)
        p->signature->Release();
    VirtualFree(p, 0, MEM_RELEASE);
}
Facts GraphicsFacts(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d)
{
    Facts f;
    f.blend = d.BlendState.RenderTarget[0].BlendEnable || d.BlendState.RenderTarget[0].LogicOpEnable;
    f.writeMask = d.BlendState.RenderTarget[0].RenderTargetWriteMask;
    f.renderTargets = d.NumRenderTargets;
    f.depthStencilWrite =
        (d.DepthStencilState.DepthEnable && d.DepthStencilState.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO) ||
        (d.DepthStencilState.StencilEnable && d.DepthStencilState.StencilWriteMask);
    f.rtvFormat = d.RTVFormats[0];
    return f;
}
template <class T> struct alignas(void*) StreamPart
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T value;
};
void RegisterStream(ID3D12Device* device, ID3D12PipelineState* pso, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                    const char* source)
{
    if (!desc || !desc->pPipelineStateSubobjectStream)
        return;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d {};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = 15;
    d.DepthStencilState.DepthEnable = TRUE;
    d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    D3D12_SHADER_BYTECODE cs {};
    const auto* bytes = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
    UINT64 seen = 0;
    for (size_t at = 0; at < desc->SizeInBytes;)
    {
        if (desc->SizeInBytes - at < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))
            return;
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        memcpy(&type, bytes + at, sizeof(type));
        if (UINT(type) >= 64 || (seen & (1ull << UINT(type))))
            return;
        seen |= 1ull << UINT(type);
        size_t size = 0;
#define PART(Enum, Type, Read)                                                                                         \
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##Enum:                                                                   \
    {                                                                                                                  \
        size = sizeof(StreamPart<Type>);                                                                               \
        if (size > desc->SizeInBytes - at)                                                                             \
            return;                                                                                                    \
        Type value;                                                                                                    \
        memcpy(&value, bytes + at + offsetof(StreamPart<Type>, value), sizeof(value));                                 \
        Read;                                                                                                          \
        break;                                                                                                         \
    }
        switch (type)
        {
            PART(ROOT_SIGNATURE, ID3D12RootSignature*, d.pRootSignature = value);
            PART(PS, D3D12_SHADER_BYTECODE, d.PS = value);
            PART(CS, D3D12_SHADER_BYTECODE, cs = value);
            PART(BLEND, D3D12_BLEND_DESC, d.BlendState = value);
            PART(RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY, d.NumRenderTargets = value.NumRenderTargets;
                 memcpy(d.RTVFormats, value.RTFormats, sizeof(d.RTVFormats)));
            PART(DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC, d.DepthStencilState = value);
            PART(DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1,
                 memcpy(&d.DepthStencilState, &value, sizeof(d.DepthStencilState)));
            PART(DEPTH_STENCIL2, D3D12_DEPTH_STENCIL_DESC2, d.DepthStencilState.DepthEnable = value.DepthEnable;
                 d.DepthStencilState.DepthWriteMask = value.DepthWriteMask;
                 d.DepthStencilState.StencilEnable = value.StencilEnable;
                 d.DepthStencilState.StencilWriteMask =
                     UINT8(value.FrontFace.StencilWriteMask | value.BackFace.StencilWriteMask));
            PART(SAMPLE_DESC, DXGI_SAMPLE_DESC, d.SampleDesc = value);
            PART(VS, D3D12_SHADER_BYTECODE, (void) value);
            PART(DS, D3D12_SHADER_BYTECODE, (void) value);
            PART(HS, D3D12_SHADER_BYTECODE, (void) value);
            PART(GS, D3D12_SHADER_BYTECODE, (void) value);
            PART(AS, D3D12_SHADER_BYTECODE, (void) value);
            PART(MS, D3D12_SHADER_BYTECODE, (void) value);
            PART(STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC, (void) value);
            PART(SAMPLE_MASK, UINT, (void) value);
            PART(RASTERIZER, D3D12_RASTERIZER_DESC, (void) value);
            PART(RASTERIZER1, D3D12_RASTERIZER_DESC1, (void) value);
            PART(RASTERIZER2, D3D12_RASTERIZER_DESC2, (void) value);
            PART(INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC, (void) value);
            PART(IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE, (void) value);
            PART(PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE, (void) value);
            PART(DEPTH_STENCIL_FORMAT, DXGI_FORMAT, (void) value);
            PART(NODE_MASK, UINT, (void) value);
            PART(CACHED_PSO, D3D12_CACHED_PIPELINE_STATE, (void) value);
            PART(FLAGS, D3D12_PIPELINE_STATE_FLAGS, (void) value);
            PART(VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC, (void) value);
        default:
            return;
        }
#undef PART
        at += size;
    }
    if (cs.pShaderBytecode)
        RegisterPipeline(device, pso, cs, {}, d.pRootSignature, source);
    else
        RegisterPipeline(device, pso, d.PS, GraphicsFacts(d), d.pRootSignature, source);
}
HRESULT STDMETHODCALLTYPE hkCreateGraphicsPipelineState(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                        REFIID iid, void** out)
{
    if (ThreadHasNoTls())
        return o_CreateGraphicsPipelineState(d, desc, iid, out);
    const auto hr = o_CreateGraphicsPipelineState(d, desc, iid, out);
    if (SUCCEEDED(hr) && desc && out)
        RegisterPipeline(d, static_cast<ID3D12PipelineState*>(*out), desc->PS, GraphicsFacts(*desc),
                         desc->pRootSignature, "raster");
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateComputePipelineState(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                                       REFIID iid, void** out)
{
    if (ThreadHasNoTls())
        return o_CreateComputePipelineState(d, desc, iid, out);
    const auto hr = o_CreateComputePipelineState(d, desc, iid, out);
    if (SUCCEEDED(hr) && desc && out)
        RegisterPipeline(d, static_cast<ID3D12PipelineState*>(*out), desc->CS, {}, desc->pRootSignature, "compute");
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreatePipelineState(ID3D12Device2* d, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                                REFIID iid, void** out)
{
    if (ThreadHasNoTls())
        return o_CreatePipelineState(d, desc, iid, out);
    const auto hr = o_CreatePipelineState(d, desc, iid, out);
    if (SUCCEEDED(hr) && out)
        RegisterStream(d, static_cast<ID3D12PipelineState*>(*out), desc, "stream");
    return hr;
}
#define LIBRARY_HOOK(Name, Interface, Desc, Register)                                                                  \
    HRESULT STDMETHODCALLTYPE hk##Name(Interface* l, LPCWSTR name, const Desc* desc, REFIID iid, void** out)           \
    {                                                                                                                  \
        if (ThreadHasNoTls())                                                                                          \
            return o_##Name(l, name, desc, iid, out);                                                                  \
        const auto hr = o_##Name(l, name, desc, iid, out);                                                             \
        if (SUCCEEDED(hr) && desc && out)                                                                              \
        {                                                                                                              \
            Microsoft::WRL::ComPtr<ID3D12Device> d;                                                                    \
            if (SUCCEEDED(l->GetDevice(IID_PPV_ARGS(&d))))                                                             \
            {                                                                                                          \
                Register;                                                                                              \
            }                                                                                                          \
        }                                                                                                              \
        return hr;                                                                                                     \
    }
LIBRARY_HOOK(LoadGraphicsPipeline, ID3D12PipelineLibrary, D3D12_GRAPHICS_PIPELINE_STATE_DESC,
             RegisterPipeline(d.Get(), static_cast<ID3D12PipelineState*>(*out), desc->PS, GraphicsFacts(*desc),
                              desc->pRootSignature, "library raster"))
LIBRARY_HOOK(LoadComputePipeline, ID3D12PipelineLibrary, D3D12_COMPUTE_PIPELINE_STATE_DESC,
             RegisterPipeline(d.Get(), static_cast<ID3D12PipelineState*>(*out), desc->CS, {}, desc->pRootSignature,
                              "library compute"))
LIBRARY_HOOK(LoadPipeline, ID3D12PipelineLibrary1, D3D12_PIPELINE_STATE_STREAM_DESC,
             RegisterStream(d.Get(), static_cast<ID3D12PipelineState*>(*out), desc, "library stream"))
#undef LIBRARY_HOOK
HRESULT STDMETHODCALLTYPE hkCreatePipelineLibrary(ID3D12Device1* d, const void* blob, SIZE_T size, REFIID iid,
                                                  void** out)
{
    if (ThreadHasNoTls())
        return o_CreatePipelineLibrary(d, blob, size, iid, out);
    return o_CreatePipelineLibrary(d, blob, size, iid, out);
}
// Root signatures, descriptor heaps, views and copies are followed on threads without TLS too: what they keep is
// lock-free or VirtualAlloc storage only, nothing that needs one
HRESULT STDMETHODCALLTYPE hkCreateRootSignature(ID3D12Device* d, UINT node, const void* blob, SIZE_T size, REFIID iid,
                                                void** out)
{
    const auto hr = o_CreateRootSignature(d, node, blob, size, iid, out);
    if (SUCCEEDED(hr) && out)
        rootsig::OnCreate(static_cast<ID3D12RootSignature*>(*out), blob, size, TrackingDevice(d));
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateDescriptorHeap(ID3D12Device* d, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID iid,
                                                 void** out)
{
    const auto hr = o_CreateDescriptorHeap(d, desc, iid, out);
    if (SUCCEEDED(hr) && desc && out)
        descriptors::OnCreateHeap(static_cast<ID3D12DescriptorHeap*>(*out), *desc,
                                  d->GetDescriptorHandleIncrementSize(desc->Type), TrackingDevice(d));
    return hr;
}
void STDMETHODCALLTYPE hkCreateConstantBufferView(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    o_CreateConstantBufferView(d, desc, dst);
    descriptors::OnCreateView(descriptors::Kind::Cbv, nullptr, desc, dst, TrackingDevice(d));
}
#define VIEW_HOOK(Name, Type, ViewKind)                                                                                \
    void STDMETHODCALLTYPE hk##Name(ID3D12Device* d, ID3D12Resource* r, const Type* desc,                              \
                                    D3D12_CPU_DESCRIPTOR_HANDLE dst)                                                   \
    {                                                                                                                  \
        o_##Name(d, r, desc, dst);                                                                                     \
        descriptors::OnCreateView(descriptors::Kind::ViewKind, r, desc, dst, TrackingDevice(d));                       \
    }
VIEW_HOOK(CreateShaderResourceView, D3D12_SHADER_RESOURCE_VIEW_DESC, Srv)
VIEW_HOOK(CreateRenderTargetView, D3D12_RENDER_TARGET_VIEW_DESC, Rtv)
#undef VIEW_HOOK
void STDMETHODCALLTYPE hkCreateUnorderedAccessView(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* counter,
                                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    o_CreateUnorderedAccessView(d, r, counter, desc, dst);
    descriptors::OnCreateView(descriptors::Kind::Uav, counter ? nullptr : r, desc, dst, TrackingDevice(d));
}
void STDMETHODCALLTYPE hkCopyDescriptorsSimple(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                               D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    o_CopyDescriptorsSimple(d, n, dst, src, type);
    descriptors::OnCopySimple(n, dst, src, type, TrackingDevice(d));
}
void STDMETHODCALLTYPE hkCopyDescriptors(ID3D12Device* d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dst,
                                         const UINT* dn, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* src,
                                         const UINT* sn, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    o_CopyDescriptors(d, nd, dst, dn, ns, src, sn, type);
    descriptors::OnCopy(nd, dst, dn, ns, src, sn, type, TrackingDevice(d));
}
HRESULT STDMETHODCALLTYPE hkMap(ID3D12Resource* r, UINT sub, const D3D12_RANGE* range, void** out)
{
    if (ThreadHasNoTls())
        return o_Map(r, sub, range, out);
    const auto hr = o_Map(r, sub, range, out);
    g_mapCalls.fetch_add(1, std::memory_order_relaxed);
    if (FAILED(hr) || sub || !out || !*out)
        return hr;
    const auto desc = r->GetDesc();
    D3D12_HEAP_PROPERTIES heap;
    D3D12_HEAP_FLAGS flags;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || FAILED(r->GetHeapProperties(&heap, &flags)) ||
        heap.Type != D3D12_HEAP_TYPE_UPLOAD)
        return hr;
    for (auto& m : g_mapped)
        if (m.resource.load(std::memory_order_acquire) == r)
        {
            m.maps.fetch_add(1);
            return hr;
        }
    for (auto& m : g_mapped)
    {
        ID3D12Resource* empty = nullptr;
        if (!m.resource.compare_exchange_strong(empty, reinterpret_cast<ID3D12Resource*>(1)))
            continue;
        m.cpu.store(static_cast<const uint8_t*>(*out));
        m.va.store(r->GetGPUVirtualAddress());
        m.size.store(desc.Width);
        m.maps.store(1);
        m.resource.store(r, std::memory_order_release);
        return hr;
    }
    g_mapsNotKept.fetch_add(1, std::memory_order_relaxed);
    return hr;
}
void STDMETHODCALLTYPE hkUnmap(ID3D12Resource* r, UINT sub, const D3D12_RANGE* range)
{
    if (ThreadHasNoTls())
    {
        o_Unmap(r, sub, range);
        return;
    }
    if (!sub)
        DropMapped(r, true);
    o_Unmap(r, sub, range);
}
bool Examine(List* list)
{
    bool first = false;
    const bool ok = g_tables.Examine(list, &first);
    if (first && g_patchLogs.fetch_add(1, std::memory_order_relaxed) < 6)
        LOG_INFO("Spatial pass: a game command list has a vtable of its own ({:X}); its entries now count through us",
                 size_t(*reinterpret_cast<void***>(list)));
    return ok;
}
ListState* Track(List* list)
{
    const bool complete = Examine(list);
    auto* s = g_lists.Find(list);
    if (!s)
        g_untracked.fetch_add(1, std::memory_order_relaxed);
    else if (!s->forwarding && !s->inEnvelope)
    {
        s->incomplete |= !complete;
        ++s->user[0];
    }
    return s;
}
void Created(void* object, ID3D12PipelineState* initial, ID3D12Device* device)
{
    if (!object)
        return;
    List* list = nullptr;
    if (FAILED(static_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&list))))
        return;
    List* real = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, list, reinterpret_cast<IUnknown**>(&real)))
        real = list;
    const bool complete = Examine(real);
    g_lists.Begin(real, initial);
    if (auto* s = g_lists.Find(real))
    {
        s->device = TrackingDevice(device);
        s->incomplete = !complete || !spatial_tracking::Available(s->device);
        BindPipeline(*s, initial);
    }
    list->Release();
}
HRESULT STDMETHODCALLTYPE hkCreateCommandList(ID3D12Device* d, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                              ID3D12CommandAllocator* a, ID3D12PipelineState* p, REFIID iid, void** out)
{
    if (ThreadHasNoTls())
        return o_CreateCommandList(d, node, type, a, p, iid, out);
    const auto hr = o_CreateCommandList(d, node, type, a, p, iid, out);
    if (SUCCEEDED(hr) && out)
        Created(*out, p, d);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateCommandList1(ID3D12Device4* d, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                               D3D12_COMMAND_LIST_FLAGS flags, REFIID iid, void** out)
{
    if (ThreadHasNoTls())
        return o_CreateCommandList1(d, node, type, flags, iid, out);
    const auto hr = o_CreateCommandList1(d, node, type, flags, iid, out);
    if (SUCCEEDED(hr) && out)
        Created(*out, nullptr, d);
    return hr;
}
HRESULT STDMETHODCALLTYPE Reset(PFN_Reset original, List* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p)
{
    if (ThreadHasNoTls())
    {
        const auto hr = original(l, a, p);
        if (SUCCEEDED(hr))
            inlist::OnResetOrRelease(l);
        return hr;
    }
    const bool complete = Examine(l);
    auto* s = g_lists.Find(l);
    if (s && s->forwarding)
        return original(l, a, p);
    if (s)
        s->forwarding = true;
    const auto hr = original(l, a, p);
    if (s)
        s->forwarding = false;
    if (SUCCEEDED(hr))
    {
        g_lists.Begin(l, p);
        if (auto* state = g_lists.Find(l))
        {
            if (!spatial_tracking::Available(state->device))
            {
                Microsoft::WRL::ComPtr<ID3D12Device> owner;
                if (SUCCEEDED(l->GetDevice(IID_PPV_ARGS(&owner))))
                    state->device = spatial_tracking::Find(owner.Get());
            }
            state->incomplete = !complete || !spatial_tracking::Available(state->device);
            BindPipeline(*state, p);
        }
        inlist::OnResetOrRelease(l);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Close(PFN_Close original, List* l)
{
    if (ThreadHasNoTls())
        return original(l);
    if (auto* s = Track(l); s && !s->forwarding)
    {
        g_calls.fetch_add(s->user[0], std::memory_order_relaxed);
        s->user[0] = 0;
        if (const uint64_t now = g_jitter.load(std::memory_order_relaxed); s->user[5] && now && s->user[5] != now)
            g_midList.fetch_add(1, std::memory_order_relaxed); // presented since the list took its offset; it kept it
        s->forwarding = true;
        const auto hr = original(l);
        s->forwarding = false;
        return hr;
    }
    return original(l);
}
ULONG STDMETHODCALLTYPE Release(PFN_Release original, IUnknown* object)
{
    const uint64_t owned = inlist::ReleaseToken(object);
    if (ThreadHasNoTls())
    {
        const auto left = original(object);
        if (!left)
        {
            inlist::Released(owned);
            inlist::OnResetOrRelease(object);
        }
        return left;
    }
    auto* s = g_lists.Find(object);
    if (s)
    {
        if (s->forwarding)
            return original(object);
        s->forwarding = true;
        // Runtime-internal destruction can bypass Release; a resource can later reuse that tracked list address.
        List* list = nullptr;
        if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&list))))
        {
            Examine(list);
            list->Release();
        }
        else
        {
            g_lists.Forget(object);
            s = nullptr;
        }
    }
    const auto left = original(object);
    if (s)
        s->forwarding = false;
    if (!left)
    {
        inlist::Released(owned);
        g_lists.Forget(object);
        inlist::OnResetOrRelease(object);
        DropMapped(object);
    }
    return left;
}
ULONG STDMETHODCALLTYPE hkResourceRelease(IUnknown* object) { return Release(o_ResourceRelease, object); }
// Any thread, TLS or not: the lists holding our work are followed to the GPU (InListUpscale.h)
void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    inlist::Execute(q, n, lists, o_ExecuteCommandLists);
}
HRESULT STDMETHODCALLTYPE hkCreateCommittedResource(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* heap,
                                                    D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc,
                                                    D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE* clear,
                                                    REFIID iid, void** out)
{
    const auto hr = o_CreateCommittedResource(d, heap, flags, desc, initial, clear, iid, out);
    if (SUCCEEDED(hr) && out && heap)
        inlist::Created(static_cast<ID3D12Resource*>(*out), initial, heap->Type);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreatePlacedResource(ID3D12Device* d, ID3D12Heap* heap, UINT64 offset,
                                                 const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initial,
                                                 const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out)
{
    const auto hr = o_CreatePlacedResource(d, heap, offset, desc, initial, clear, iid, out);
    if (SUCCEEDED(hr) && out && heap)
        inlist::Created(static_cast<ID3D12Resource*>(*out), initial, heap->GetDesc().Properties.Type);
    return hr;
}
bool TableView(const ListState& s, D3D12_GPU_DESCRIPTOR_HANDLE base, UINT offset, View* view)
{
    for (UINT h = 0; h < s.heapCount; ++h)
    {
        const auto ref = s.heapRefs[h];
        const UINT64 delta = UINT64(offset) * ref.increment;
        if (delta <= UINT64_MAX - base.ptr && descriptors::LookupGpu(ref, { base.ptr + delta }, view))
            return true;
    }
    return false;
}
bool Binding(const ListState& s, rootsig::Slot slot, bool compute, View* view)
{
    if (slot.kind != rootsig::Slot::Table || slot.count == UINT_MAX)
        return false;
    const UINT i = slot.parameter;
    const auto mask = compute ? s.computeTableMask : s.graphicsTableMask;
    return (mask & (1ull << i)) && TableView(s, compute ? s.computeTables[i] : s.graphicsTables[i], slot.offset, view);
}
// Only an unknown old root signature may use this fallible fallback; failed parses stay refused.
// has no layout to find the pass's bindings in; its EASU constants find them. They are the one block bound to the
// pass (root constants, a root CBV, a CBV in a bound table) that decodes against the output; the input is then the one
// texture in the bound tables of the decoded input size, and a dispatch's output the one UAV there the block decodes
// against. Only textures among the list's last transitions, in the state the pass uses them in, are looked at: they
// are alive (another descriptor in a heap may name a released resource) and the only ones the pass may use. None
// leaves the binding unresolved; more than one refuses the pass.
constexpr UINT kTableScan = 16; // descriptors looked at from the start of each bound table
constinit std::atomic<bool> g_inferLogged {};
int TryInferLateBindings(const ListState& s, bool compute, Facts& f)
{
    s.device->rootAttempts.fetch_add(1, std::memory_order_relaxed);
    const auto* tables = compute ? s.computeTables : s.graphicsTables;
    const auto each = [&](auto&& fn)
    {
        for (uint64_t m = compute ? s.computeTableMask : s.graphicsTableMask; m; m &= m - 1)
            for (UINT k = 0; k < kTableScan; ++k)
                if (View v; TableView(s, tables[std::countr_zero(m)], k, &v))
                    fn(std::countr_zero(m), v);
    };
    const auto texture = [&](const View& v, descriptors::Kind kind, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_DESC* d)
    {
        ListState::Transition t;
        if (v.kind != uint8_t(kind) || !v.resource || v.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            !FindTransition(s, v.resource, 0, &t) || !(t.after & state))
            return false;
        *d = v.resource->GetDesc();
        return true;
    };
    struct Target
    {
        View view;
        D3D12_RESOURCE_DESC desc;
        UINT w, h;
    } targets[4] {};
    UINT targetCount = 0;
    bool overflow = false;
    if (!compute && f.output.resource) // the render target at the viewport's size, as Resolve takes it
    {
        LONG w = LONG(f.outputDesc.Width), h = LONG(f.outputDesc.Height), vw = 0, vh = 0;
        const auto& vp = f.viewport;
        if (f.viewportValid && vp.TopLeftX == 0 && vp.TopLeftY == 0 && pass_detail::Whole(vp.Width, &vw) &&
            pass_detail::Whole(vp.Height, &vh) && vw > 0 && vh > 0 && vw <= w && vh <= h)
            w = vw, h = vh;
        targets[targetCount++] = { f.output, f.outputDesc, UINT(w), UINT(h) };
    }
    if (compute)
        each(
            [&](int, const View& v)
            {
                D3D12_RESOURCE_DESC d;
                for (UINT i = 0; i < targetCount; ++i)
                    if (targets[i].view.resource == v.resource)
                        return;
                if (texture(v, descriptors::Kind::Uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &d))
                {
                    if (targetCount == std::size(targets))
                        overflow = true;
                    else
                        targets[targetCount++] = { v, d, UINT(d.Width), d.Height };
                }
            });
    uint32_t block[16], found[16] {};
    int parameter = -1, target = -1;
    UINT matches = 0;
    const auto consider = [&](int root)
    {
        for (UINT i = 0; i < targetCount; ++i)
            if (DecodeEasu(block, targets[i].w, targets[i].h, nullptr) &&
                !(matches && target == int(i) && !std::memcmp(block, found, sizeof(found)))) // the same block again
            {
                ++matches, parameter = root, target = int(i);
                std::memcpy(found, block, sizeof(found));
            }
    };
    for (uint64_t m = compute ? s.computeCbvMask : s.graphicsCbvMask; m; m &= m - 1)
        if (ReadMapped((compute ? s.computeCbv : s.graphicsCbv)[std::countr_zero(m)], block, sizeof(block)))
            consider(std::countr_zero(m));
    for (uint64_t m = compute ? s.computeConstantParamMask : s.graphicsConstantParamMask; m; m &= m - 1)
    {
        const UINT root = UINT(std::countr_zero(m));
        UINT n = 0;
        while (n < 16 &&
               (compute ? ReadComputeConstant(s, root, n, &block[n]) : ReadGraphicsConstant(s, root, n, &block[n])))
            ++n;
        if (n == 16)
            consider(int(root));
    }
    each(
        [&](int root, const View& v)
        {
            if (v.kind == uint8_t(descriptors::Kind::Cbv) && v.size >= sizeof(block) &&
                ReadMapped(v.va, block, sizeof(block)))
                consider(root);
        });
    if (matches != 1 || overflow)
    {
        f.ambiguous = matches > 1 || (matches && overflow);
        return -1;
    }
    EasuConstants con;
    DecodeEasu(found, targets[target].w, targets[target].h, &con);
    View input {};
    UINT inputs = 0;
    each(
        [&](int, const View& v)
        {
            D3D12_RESOURCE_DESC d;
            if (v.resource != input.resource &&
                texture(v, descriptors::Kind::Srv,
                        compute ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        &d) &&
                d.Width == con.inputWidth && d.Height == con.inputHeight)
                input = v, ++inputs;
        });
    f.ambiguous = inputs > 1;
    if (inputs == 1)
        f.input = input;
    if (compute)
        f.output = targets[target].view, f.outputDesc = targets[target].desc;
    std::memcpy(f.constants, found, sizeof(found));
    f.constantCount = 16;
    return parameter;
}
bool FgTakesHudless()
{
    const auto& state = State::Instance();
    auto* fg = state.currentFG;
    const auto* cfg = Config::Instance();
    return fg && state.activeFgInput == FGInput::Upscaler && fg->IsActive() && !fg->IsPaused() && cfg->IsFGEnabled() &&
           state.currentSwapchain && !cfg->FGDisableHudless.value_or_default();
}
void MatchHudless(ListState& s, const Facts& f)
{
    auto* source = g_hudlessSource.load(std::memory_order_acquire);
    if (!source)
        return;
    const auto frame = g_presents.load(std::memory_order_relaxed);
    const bool resource = f.input.resource == source;
    if (!resource && s.user[7] != frame + 1)
        g_hudlessOther.fetch_add(1, std::memory_order_relaxed);
    else if (g_hudlessSource.compare_exchange_strong(source, nullptr))
    {
        if (g_hudlessFrame.load(std::memory_order_relaxed) != frame)
            g_hudlessMissed.fetch_add(1, std::memory_order_relaxed);
        else
        {
            s.user[4] = reinterpret_cast<uintptr_t>(f.output.resource);
            s.user[6] = uintptr_t(g_hudlessIndex.load(std::memory_order_relaxed) + 1);
            if (!g_hudlessMatch[resource].exchange(true))
                LOG_INFO("Spatial pass: the sharpening pass that takes the upscale is matched by {}",
                         resource ? "the resource it reads" : "the list of the upscale pass");
        }
    }
}
void ResolveViews(const ListState& s, const Pipeline* p, bool compute, Facts* facts)
{
    auto& f = *facts;
    auto* rs = compute ? s.computeSignature : s.graphicsSignature;
    const auto input = rs == p->signature ? p->input
                                          : rootsig::Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, f.shader.textureRegister,
                                                          f.shader.textureSpace, compute);
    const auto output = rs == p->signature ? p->output
                                           : rootsig::Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, f.shader.outputRegister,
                                                           f.shader.outputSpace, compute);
    f.bindless = f.shader.textureRegister < 0 || input.count == UINT_MAX ||
                 (compute && (f.shader.outputRegister < 0 || output.count == UINT_MAX));
    Binding(s, input, compute, &f.input);
    if (compute)
        Binding(s, output, true, &f.output);
    else if (s.rtv0Valid)
        descriptors::Lookup(s.rtv0, &f.output, s.device);
    if (f.output.resource)
        f.outputDesc = f.output.resource->GetDesc();
}
bool Detect(List* list, ListState& s, Facts f)
{
    auto* p = reinterpret_cast<Pipeline*>(s.user[1]);
    if (!p || !spatial_tracking::Available(s.device) || !Config::Instance()->ReplaceGameFsr1.value_or_default())
        return false;
    if (p->facts.shader.pass == Pass::Rcas)
    {
        g_rcasFrame.fetch_add(1, std::memory_order_relaxed);
        if (!g_hudlessSource.load(std::memory_order_acquire))
            return false;
    }
    const bool compute = f.shape == Shape::Dispatch;
    f.shader = p->facts.shader;
    f.blend = p->facts.blend;
    f.writeMask = p->facts.writeMask;
    f.renderTargets = p->facts.renderTargets;
    f.rtvFormat = p->facts.rtvFormat;
    f.depthStencilWrite = p->facts.depthStencilWrite;
    f.boundTargets = s.renderTargetCount;
    f.tracked = !s.incomplete;
    f.afterBundle = s.user[2] != 0;
    f.inRenderPass = s.inRenderPass;
    f.listType = list->GetType();
    f.predicated = s.predicated;
    f.viewportValid = s.viewport0Valid;
    f.viewport = s.viewport0;
    f.scissorValid = s.scissor0Valid;
    f.scissor = s.scissor0;
    const auto* cfg = Config::Instance();
    f.overlayMenu = cfg->OverlayMenu.value_or_default();
    f.outputScaling = cfg->OutputScalingEnabled.value_or_default();
    f.magnifier = cfg->MagnifierEnabled.value_or_default();
    f.gameUpscaler = GameUpscalerCalledRecently();
    const auto backend = cfg->Dx12Upscaler.value_or_default();
    f.backendReadsBase = backend == Upscaler::DLSS || backend == Upscaler::XeSS;
    auto* rs = compute ? s.computeSignature : s.graphicsSignature;
    const auto constants = rs == p->signature
                               ? p->constants
                               : rootsig::Find(rs, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, f.shader.constantRegister,
                                               f.shader.constantSpace, compute);
    ResolveViews(s, p, compute, &f);
    if (f.shader.pass == Pass::Rcas)
    {
        MatchHudless(s, f);
        return false;
    }
    int parameter = int(constants.parameter);
    if (!f.shader.uber && rootsig::MayInfer(rs, s.device))
    {
        parameter = TryInferLateBindings(s, compute, f);
        (parameter >= 0 && f.input.resource && !f.ambiguous ? s.device->rootSuccess : s.device->rootFailure)
            .fetch_add(1, std::memory_order_relaxed);
        if (!g_inferLogged.exchange(true))
            LOG_INFO("Spatial pass: root signature not seen at creation; bindings inferred from constants (root "
                     "signature {:X}: constants at root parameter {}, input {:X}{})",
                     size_t(rs), parameter, size_t(f.input.resource), f.ambiguous ? ", ambiguous" : "");
    }
    if (f.input.resource)
        f.inputDesc = f.input.resource->GetDesc();
    ListState::Transition transition;
    if (f.input.resource && FindTransition(s, f.input.resource, 0, &transition))
    {
        f.inputStateKnown = true;
        f.inputState = transition.after;
        f.enhanced = transition.enhanced;
        f.pendingTransition = transition.pending;
    }
    if (f.output.resource && FindTransition(s, f.output.resource, 0, &transition))
        f.enhanced |= transition.enhanced;
    UINT64 va = 0;
    UINT bytes = sizeof(f.constants);
    if (constants.kind == rootsig::Slot::Constants)
        for (UINT i = 0; i < (std::min) (constants.count, 16u); ++i)
        {
            if (!(compute ? ReadComputeConstant(s, constants.parameter, i, &f.constants[i])
                          : ReadGraphicsConstant(s, constants.parameter, i, &f.constants[i])))
                break;
            ++f.constantCount;
        }
    else
    {
        if (constants.kind == rootsig::Slot::RootDescriptor)
        {
            if ((compute ? s.computeCbvMask : s.graphicsCbvMask) & (1ull << constants.parameter))
                va = (compute ? s.computeCbv : s.graphicsCbv)[constants.parameter];
        }
        else
        {
            View cb;
            if (Binding(s, constants, compute, &cb) && cb.kind == uint8_t(descriptors::Kind::Cbv))
            {
                va = cb.va;
                bytes = (std::min) (bytes, cb.size);
            }
        }
        if (va && bytes && ReadMapped(va, f.constants, bytes))
            f.constantCount = bytes / 4;
    }
    if (!f.shader.uber && f.constantCount && !DecodeEasu(f.constants, 0, 0, nullptr) && !g_dumped.exchange(true))
        LOG_INFO("Spatial pass: an EASU draw's constant buffer did not decode (output {}x{}): {:08X} {:08X} {:08X} "
                 "{:08X} | {:08X} {:08X} {:08X} {:08X} | {:08X} {:08X} {:08X} {:08X} | {:08X} {:08X} {:08X} {:08X}",
                 f.outputDesc.Width, f.outputDesc.Height, f.constants[0], f.constants[1], f.constants[2],
                 f.constants[3], f.constants[4], f.constants[5], f.constants[6], f.constants[7], f.constants[8],
                 f.constants[9], f.constants[10], f.constants[11], f.constants[12], f.constants[13], f.constants[14],
                 f.constants[15]);
    Plan plan;
    const auto why = Resolve(f, &plan);
    if (why == Refusal::UberRcas)
    {
        g_rcasFrame.fetch_add(1, std::memory_order_relaxed);
        MatchHudless(s, f);
        return false;
    }
    g_easuFrame.fetch_add(1, std::memory_order_relaxed);
    g_shapes[size_t(f.shape)].fetch_add(1, std::memory_order_relaxed);
    for (auto& slot : g_seenLists)
    {
        const void* empty = nullptr;
        if (slot.load() == list || slot.compare_exchange_strong(empty, list))
            break;
    }
    if (why != Refusal::None)
    {
        g_refusals[size_t(why)].fetch_add(1, std::memory_order_relaxed);
        inlist::NoteRefused(why);
        g_jitter.store(0, std::memory_order_relaxed);
        return false;
    }
    g_constants.fetch_add(plan.constantsUsed, std::memory_order_relaxed);
    g_textureRect.fetch_add(!plan.constantsUsed, std::memory_order_relaxed);
    g_constantParameter.store(plan.constantsUsed ? parameter : -1);
    g_thread.store(GetCurrentThreadId());
    g_inW.store(plan.inputRect.right - plan.inputRect.left);
    g_inH.store(plan.inputRect.bottom - plan.inputRect.top);
    g_outW.store(plan.box.right);
    g_outH.store(plan.box.bottom);
    g_inFormat.store(plan.input.format);
    g_outFormat.store(plan.output.format);

    if (g_planLogs.fetch_add(1, std::memory_order_relaxed) < 6)
        LOG_INFO("Spatial pass: EASU {}: input {}x{} format {} to output {}x{} format {}; resources {:X} "
                 "-> {:X}, input rect ({},{})-({},{}), box (0,0)-({},{}), ratio {:.3f}, constants {}",
                 compute                         ? "dispatch"
                 : f.shape == Shape::DrawIndexed ? "indexed"
                                                 : "draw",
                 g_inW.load(), g_inH.load(), int(plan.input.format), plan.box.right, plan.box.bottom,
                 int(plan.output.format), size_t(plan.input.resource), size_t(plan.output.resource),
                 plan.inputRect.left, plan.inputRect.top, plan.inputRect.right, plan.inputRect.bottom, plan.box.right,
                 plan.box.bottom, plan.ratio, plan.constantsUsed ? "used" : "unreadable");
    ListRestoreToList restore(list);
    {
        // Prefer this list's scene offset; otherwise use the offset taken by the frame's lists.
        uint64_t bits = s.user[5] ? uint64_t(s.user[5]) : g_applied.load(std::memory_order_relaxed);
        if (!bits && g_movedLastFrame.load(std::memory_order_relaxed))
        {
            bits = g_jitter.load(std::memory_order_relaxed);
            g_beforeScene.fetch_add(1, std::memory_order_relaxed);
        }
        if (bits != 0 && bits != g_jitter.load(std::memory_order_relaxed))
            g_stale.fetch_add(1, std::memory_order_relaxed);
        vparams::g_scaledJitter.store(bits, std::memory_order_relaxed);
    }
    const bool replaced = inlist::OnPass(list, s, plan, restore);
    if (replaced)
    {
        g_lastPassPresent.store(g_presents.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_passSinceAdvance.store(true, std::memory_order_relaxed);
        s.user[7] = g_presents.load(std::memory_order_relaxed) + 1;
        if (FgTakesHudless())
        {
            g_hudlessIndex.store(State::Instance().currentFG->GetIndex(), std::memory_order_relaxed);
            g_hudlessFrame.store(s.user[7] - 1, std::memory_order_relaxed);
            g_hudlessSource.store(plan.output.resource, std::memory_order_release);
        }
    }
    else
        g_jitter.store(0, std::memory_order_relaxed);
    return replaced;
}
// The game's sharpening pass after a replaced upscale has drawn the frame without its HUD: frame generation takes a
// copy of it (made in the list, now) as the HUD-less colour, so it interpolates the scene and keeps the HUD as drawn
void HandHudless(List* list, const ListState& s, ID3D12Resource* resource, D3D12_RESOURCE_STATES resourceState,
                 int index)
{
    auto& state = State::Instance();
    auto* fg = state.currentFG;
    if (!resource || !FgTakesHudless())
        return;
    const auto desc = resource->GetDesc();
    if (s.inRenderPass || desc.Width != state.currentSwapchainDesc.BufferDesc.Width ||
        desc.Height != state.currentSwapchainDesc.BufferDesc.Height)
    {
        (s.inRenderPass ? g_hudlessInPass : g_hudlessSize).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Dx12Resource hudless {};
    hudless.type = FG_ResourceType::HudlessColor;
    hudless.resource = resource;
    hudless.width = desc.Width;
    hudless.height = desc.Height;
    hudless.cmdList = list;
    hudless.state = resourceState;
    hudless.validity = FG_ResourceValidity::ValidButMakeCopy;
    hudless.frameIndex = index;
    (fg->SetResource(&hudless) ? g_hudless : g_hudlessRefused).fetch_add(1, std::memory_order_relaxed);
}
// Scene jitter, decided per draw: geometry (indexed, more than a quad, or instanced) drawn with a depth-stencil view
// under the single viewport of the render size is moved by the list's offset (the frame's at the list's first such
// draw, kept until Reset); a full-screen pass (a triangle or 4-vertex strip, once) is not: it does not go through the
// camera and moving it would only resample its input. The viewport is set again only when that changes; user[3] holds
// whether the list's viewport is the moved one.
template <class List> void JitterForDraw(List* object, ListState& s, bool geometry)
{
    uint64_t bits = s.user[5];
    bool want = false;
    if (geometry && s.dsvValid && s.viewportCount == 1 && s.viewport0Valid)
    {
        const float rw = float(g_inW.load(std::memory_order_relaxed)),
                    rh = float(g_inH.load(std::memory_order_relaxed));
        want = std::abs(s.viewport0.Width - rw) <= 1.f && std::abs(s.viewport0.Height - rh) <= 1.f;
        if (!want)
            g_otherViewport.store(UINT(s.viewport0.Width) << 16 | (UINT(s.viewport0.Height) & 0xFFFF),
                                  std::memory_order_relaxed);
        else if (!bits && (bits = g_jitter.load(std::memory_order_relaxed)) != 0)
            s.user[5] = uintptr_t(bits), g_applied.store(bits, std::memory_order_relaxed);
        want = want && bits;
    }
    const uintptr_t applied = want ? uintptr_t(bits) : 0;
    if (s.user[3] == applied)
        return;
    s.user[3] = applied;
    if (!s.viewport0Valid) // cleared since: nothing of ours left to undo
        return;
    D3D12_VIEWPORT viewport = s.viewport0;
    if (want)
    {
        float jitter[2];
        std::memcpy(jitter, &bits, sizeof(jitter));
        viewport.TopLeftX += jitter[0];
        viewport.TopLeftY += jitter[1];
    }
    s.forwarding = true; // through the list's own entry, past this layer
    object->RSSetViewports(1, &viewport);
    s.forwarding = false;
}
template <class Handle> void CheckMotionVectors(const ListState& s, UINT count, Handle handle)
{
    if (!s.dsvValid || !g_jitter.load(std::memory_order_relaxed) ||
        g_motionVectorsDecided.load(std::memory_order_relaxed) || g_jitterOff.load(std::memory_order_relaxed))
        return;
    for (UINT i = 0; i < count; ++i)
    {
        descriptors::View view {};
        if (!descriptors::Lookup(handle(i), &view, s.device) || !view.resource)
            continue;
        const auto desc = view.resource->GetDesc();
        const auto format = view.format == DXGI_FORMAT_UNKNOWN ? desc.Format : view.format;
        if ((format != DXGI_FORMAT_R16G16_FLOAT && format != DXGI_FORMAT_R16G16_UNORM &&
             format != DXGI_FORMAT_R16G16_SNORM && format != DXGI_FORMAT_R16G16_TYPELESS &&
             format != DXGI_FORMAT_R32G32_FLOAT) ||
            std::abs(double(desc.Width) - g_inW.load(std::memory_order_relaxed)) > 1.0 ||
            std::abs(double(desc.Height) - g_inH.load(std::memory_order_relaxed)) > 1.0)
            continue;
        int undecided = 0;
        if (g_jitterOff.compare_exchange_strong(undecided, 1))
        {
            g_jitter.store(0, std::memory_order_relaxed);
            LOG_INFO("Spatial pass: scene jitter is off for this session: the game draws motion vectors "
                     "({}x{} format {}) with a depth buffer; it has temporal effects of its own",
                     desc.Width, desc.Height, UINT(format));
        }
        return;
    }
}
constinit std::atomic<DWORD> g_forwardingThreads[64] {}, g_barrierThread { 0 };
int EnterForward()
{
    const DWORD thread = GetCurrentThreadId();
    for (auto& slot : g_forwardingThreads)
        if (slot.load(std::memory_order_relaxed) == thread)
            return -1;
    for (int i = 0; i < 64; ++i)
    {
        DWORD empty = 0;
        if (g_forwardingThreads[i].compare_exchange_strong(empty, thread, std::memory_order_relaxed))
            return i;
    }
    g_untracked.fetch_add(1, std::memory_order_relaxed);
    return -1;
}
template <int Slot, class Method, auto Update> struct StateHook;
template <int Slot, class This, class... Args, auto Update> struct StateHook<Slot, void (This::*)(Args...), Update>
{
    using Fn = void(STDMETHODCALLTYPE*)(This*, Args...);
    static inline Fn original = nullptr;
    static void STDMETHODCALLTYPE Body(Fn previous, This* object, Args... args)
    {
        if (ThreadHasNoTls())
        {
            previous(object, args...);
            return;
        }
        auto* list = static_cast<List*>(object);
        auto* s = Track(list);
        if (!s || s->forwarding || s->inEnvelope)
        {
            // Our barriers, for the replay ledger (one thread records ours at a time, under the session): through a
            // list table of the game's own the runtime's entry runs again, noted by the outer call only
            const bool outer = Slot == 26 && s && s->inEnvelope && g_barrierThread.load() != GetCurrentThreadId();
            if constexpr (Slot == 26)
                if (outer)
                    g_barrierThread.store(GetCurrentThreadId()), inlist::Barriers(list, args...);
            previous(object, args...);
            if (outer)
                g_barrierThread.store(0);
            return;
        }
        int forwarding = -1;
        if constexpr (Slot == 12 || Slot == 13 || Slot == 27 || Slot == 59)
            if (s->user[5] || g_jitter.load(std::memory_order_relaxed))
            {
                bool geometry = true;
                // (vertices or indices, instances, ...): a full-screen pass is one triangle (or a 4-vertex strip),
                // once; an indexed quad is world geometry (walls, water planes, decals, particles) and moves with the
                // scene
                if constexpr (Slot == 12 || Slot == 13)
                {
                    const std::tuple<Args...> draw(args...);
                    geometry = std::get<0>(draw) > (Slot == 12 ? 4u : 3u) || std::get<1>(draw) > 1;
                }
                JitterForDraw(object, *s, geometry);
                if (s->user[3])
                {
                    g_jitteredDraws.fetch_add(1, std::memory_order_relaxed);
                    if constexpr (Slot == 27 || Slot == 59)
                        g_otherDraws.fetch_add(1, std::memory_order_relaxed);
                }
            }
        if constexpr (Slot == 12 || Slot == 13 || Slot == 14 || Slot == 59)
        {
            if (!s->user[1])
            {
                previous(object, args...);
                return;
            }
            // The debug runtime can forward a draw through a second list identity with only some setters forwarded.
            forwarding = EnterForward();
            if (forwarding < 0)
            {
                previous(object, args...);
                return;
            }
        }
        s->forwarding = true;
        bool replaced = false;
        if constexpr (std::is_same_v<decltype(Update(list, *s, args...)), bool>)
            replaced = Update(list, *s, args...);
        else
            Update(list, *s, args...);
        if constexpr (Slot == 21)
            s->user[3] = 0; // the game's own viewport, as given
        if (!replaced)
            previous(object, args...);
        s->forwarding = false;
        if (forwarding >= 0)
            g_forwardingThreads[forwarding].store(0, std::memory_order_relaxed);
        if constexpr (Slot == 12 || Slot == 13 || Slot == 14)
            if (s->user[4])
            {
                HandHudless(list, *s, reinterpret_cast<ID3D12Resource*>(s->user[4]),
                            Slot == 14 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_RENDER_TARGET,
                            int(s->user[6]) - 1);
                s->user[4] = s->user[6] = 0;
            }
    }
    using Adapter = ListVtableHook<g_tables, Slot, &original, Body>;
};
#define STATE(Name, Slot, Update) using Name##Hook = StateHook<Slot, decltype(&List::Name), Update>
STATE(
    DrawInstanced, 12,
    +[](List* l, ListState& s, UINT n, UINT instances, UINT first, UINT start)
    {
        Facts f;
        f.count = n;
        f.instances = instances;
        f.first = first;
        f.firstInstance = start;
        return Detect(l, s, f);
    });
STATE(
    DrawIndexedInstanced, 13,
    +[](List* l, ListState& s, UINT n, UINT instances, UINT first, INT base, UINT start)
    {
        Facts f;
        f.shape = Shape::DrawIndexed;
        f.count = n;
        f.instances = instances;
        f.first = first;
        f.baseVertex = base;
        f.firstInstance = start;
        return Detect(l, s, f);
    });
STATE(
    Dispatch, 14,
    +[](List* l, ListState& s, UINT x, UINT y, UINT z)
    {
        Facts f;
        f.shape = Shape::Dispatch;
        f.groups[0] = x;
        f.groups[1] = y;
        f.groups[2] = z;
        return Detect(l, s, f);
    });
STATE(
    ClearState, 11,
    +[](List*, ListState& s, ID3D12PipelineState* p)
    {
        OnClearState(s, p);
        BindPipeline(s, p);
    });
STATE(SetPipelineState, 25, +[](List*, ListState& s, ID3D12PipelineState* p) { BindPipeline(s, p); });
STATE(
    ExecuteBundle, 27,
    +[](List*, ListState& s, List*)
    {
        OnExecuteBundle(s);
        s.user[1] = 0;
        s.user[2] = 1;
    });
STATE(
    SetPredication, 55,
    +[](List*, ListState& s, ID3D12Resource* r, UINT64, D3D12_PREDICATION_OP) { OnSetPredication(s, r); });
STATE(
    ExecuteIndirect, 59,
    +[](List* l, ListState& s, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64)
    {
        Facts f;
        f.indirect = true;
        return Detect(l, s, f);
    });
STATE(
    OMSetRenderTargets, 46,
    +[](List*, ListState& s, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* r, BOOL range,
        const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
    {
        OnOMSetRenderTargets(s, n, r, range, dsv);
        const UINT rtvIncrement = descriptors::Increment(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, s.device);
        if (r)
            CheckMotionVectors(
                s, n, [=](UINT i)
                { return range ? D3D12_CPU_DESCRIPTOR_HANDLE { r->ptr + SIZE_T(i) * rtvIncrement } : r[i]; });
    });
STATE(RSSetViewports, 21, +[](List*, ListState& s, UINT n, const D3D12_VIEWPORT* vp) { OnRSSetViewports(s, n, vp); });
STATE(RSSetScissorRects, 22, +[](List*, ListState& s, UINT n, const D3D12_RECT* sc) { OnRSSetScissorRects(s, n, sc); });
STATE(
    ResourceBarrier, 26,
    +[](List*, ListState& s, UINT n, const D3D12_RESOURCE_BARRIER* b) { OnResourceBarrier(s, n, b); });
STATE(
    SetDescriptorHeaps, 28,
    +[](List*, ListState& s, UINT n, ID3D12DescriptorHeap* const* h) { OnSetDescriptorHeaps(s, n, h); });
#define ROOT(Name, Slot, Type)                                                                                         \
    STATE(Name, Slot, +[](List*, ListState& s, UINT i, Type value) { On##Name(s, i, value); })
ROOT(SetComputeRootDescriptorTable, 31, D3D12_GPU_DESCRIPTOR_HANDLE);
ROOT(SetGraphicsRootDescriptorTable, 32, D3D12_GPU_DESCRIPTOR_HANDLE);
#undef ROOT
#undef STATE
using SetComputeRoot32BitConstantHook = StateHook<33, decltype(&List::SetComputeRoot32BitConstant),
                                                  +[](List*, ListState& s, UINT i, UINT value, UINT offset)
                                                  { OnSetComputeRoot32BitConstant(s, i, value, offset); }>;
using SetGraphicsRoot32BitConstantHook = StateHook<34, decltype(&List::SetGraphicsRoot32BitConstant),
                                                   +[](List*, ListState& s, UINT i, UINT value, UINT offset)
                                                   { OnSetGraphicsRoot32BitConstant(s, i, value, offset); }>;
#define SIMPLE(Name, Slot, Type)                                                                                       \
    using Name##Hook =                                                                                                 \
        StateHook<Slot, decltype(&List::Name), +[](List*, ListState& s, Type value) { On##Name(s, value); }>
SIMPLE(SetComputeRootSignature, 29, ID3D12RootSignature*);
SIMPLE(SetGraphicsRootSignature, 30, ID3D12RootSignature*);
#undef SIMPLE
#define ROOT(Name, Slot)                                                                                               \
    using Name##Hook =                                                                                                 \
        StateHook<Slot, decltype(&List::Name),                                                                         \
                  +[](List*, ListState& s, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { On##Name(s, i, va); }>
ROOT(SetComputeRootConstantBufferView, 37);
ROOT(SetGraphicsRootConstantBufferView, 38);
ROOT(SetComputeRootShaderResourceView, 39);
ROOT(SetGraphicsRootShaderResourceView, 40);
ROOT(SetComputeRootUnorderedAccessView, 41);
ROOT(SetGraphicsRootUnorderedAccessView, 42);
#undef ROOT
#define CONSTANTS(Name, Slot)                                                                                          \
    using Name##Hook = StateHook<Slot, decltype(&List::Name),                                                          \
                                 +[](List*, ListState& s, UINT i, UINT n, const void* data, UINT offset)               \
                                 { On##Name(s, i, n, data, offset); }>
CONSTANTS(SetComputeRoot32BitConstants, 35);
CONSTANTS(SetGraphicsRoot32BitConstants, 36);
#undef CONSTANTS
using BeginRenderPassHook = StateHook<68, decltype(&ID3D12GraphicsCommandList4::BeginRenderPass),
                                      +[](List*, ListState& s, UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* r,
                                          const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* dsv, D3D12_RENDER_PASS_FLAGS)
                                      {
                                          OnBeginRenderPass(s, n, r, dsv);
                                          if (r)
                                              CheckMotionVectors(s, n, [=](UINT i) { return r[i].cpuDescriptor; });
                                      }>;
using EndRenderPassHook = StateHook<69, decltype(&ID3D12GraphicsCommandList4::EndRenderPass),
                                    +[](List*, ListState& s) { OnEndRenderPass(s); }>;
using BarrierHook =
    StateHook<80, decltype(&ID3D12GraphicsCommandList7::Barrier),
              +[](List*, ListState& s, UINT n, const D3D12_BARRIER_GROUP* groups)
              {
                  if (!groups)
                      return;
                  for (UINT i = 0; i < n; ++i)
                      if (groups[i].Type == D3D12_BARRIER_TYPE_TEXTURE)
                          for (UINT j = 0; j < groups[i].NumBarriers; ++j)
                              OnTransition(s, groups[i].pTextureBarriers[j].pResource,
                                           D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, true);
              }>;
using ReleaseHook = ListVtableHook<g_tables, 2, &o_Release, Release>;
using ResetHook = ListVtableHook<g_tables, 10, &o_Reset, Reset>;
using CloseHook = ListVtableHook<g_tables, 9, &o_Close, Close>;
struct Hook
{
    PVOID* original;
    PVOID detour;
};
Hook g_hooks[64] {};
UINT g_hookCount = 0;
template <class Fn> void DeviceHook(void** table, UINT slot, Fn& original, Fn detour)
{
    original = reinterpret_cast<Fn>(table[slot]);
    g_hooks[g_hookCount++] = { reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(detour) };
}
template <class H> void ListHook(void** table, UINT slot)
{
    DeviceHook(table, slot, H::original, H::Adapter::Detour);
    g_tables.Hook(slot, reinterpret_cast<void*>(H::Adapter::Table));
}
} // namespace
namespace SpatialPassDx12
{
bool Installed() { return g_installed.load(std::memory_order_acquire); }
void OnSessionEnter()
{
    g_jitterOff.store(0, std::memory_order_relaxed);
    g_motionVectorsDecided.store(false, std::memory_order_relaxed);
}
void Install(ID3D12Device* device)
{
    if (ThreadHasNoTls() || !device || !Config::Instance()->ReplaceGameFsr1.value_or_default())
        return;
    ID3D12Device* real = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, device, reinterpret_cast<IUnknown**>(&real)))
        real = device;
    auto* tracking = spatial_tracking::Observe(real);
    if (!tracking)
        return;
    auto** deviceTable = *reinterpret_cast<void***>(real);
    InstallClaim claim;
    if (!claim.owned)
        return; // No wait; a later ordinary install boundary may retry.
    if (Installed())
    {
        bool sameCode = true;
        for (size_t i = 0; i < std::size(kCreationSlots); ++i)
            sameCode &= deviceTable[kCreationSlots[i]] == g_creationCode[i].load(std::memory_order_acquire);
        tracking->ready.store(sameCode, std::memory_order_release);
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<List> list;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    D3D12_COMMAND_QUEUE_DESC q {};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = 1;
    bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(real->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(real->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&list))) ||
        FAILED(real->CreateCommandQueue(&q, IID_PPV_ARGS(&queue))) ||
        FAILED(real->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&buffer))))
    {
        LOG_WARN("Spatial pass: could not make the objects whose tables are read, not installed");
        return;
    }
    List* realList = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, list.Get(), reinterpret_cast<IUnknown**>(&realList)))
        realList = list.Get();
    auto** dt = *reinterpret_cast<void***>(real);
    auto** lt = *reinterpret_cast<void***>(realList);
    auto** bt = *reinterpret_cast<void***>(buffer.Get());
    g_hookCount = 0;

    g_tables.SetLog(+[](const char* why) { LOG_WARN("Spatial pass: {}", why); });
    g_tables.Install(lt);
#define DEVICE(Name, Slot) DeviceHook(dt, Slot, o_##Name, hk##Name)
    DEVICE(CreateGraphicsPipelineState, 10);
    DEVICE(CreateComputePipelineState, 11);
    DEVICE(CreateCommandList, 12);
    DEVICE(CreateCommittedResource, 27);
    DEVICE(CreatePlacedResource, 29);
    DEVICE(CreateDescriptorHeap, 14);
    DEVICE(CreateRootSignature, 16);
    DEVICE(CreateConstantBufferView, 17);
    DEVICE(CreateShaderResourceView, 18);
    DEVICE(CreateUnorderedAccessView, 19);
    DEVICE(CreateRenderTargetView, 20);
    DEVICE(CopyDescriptors, 23);
    DEVICE(CopyDescriptorsSimple, 24);
#undef DEVICE
    Microsoft::WRL::ComPtr<ID3D12Device1> d1;
    Microsoft::WRL::ComPtr<ID3D12Device2> d2;
    Microsoft::WRL::ComPtr<ID3D12Device4> d4;
    Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> library;
    if (SUCCEEDED(real->QueryInterface(IID_PPV_ARGS(&d1))))
    {
        auto** t = *reinterpret_cast<void***>(d1.Get());
        DeviceHook(t, 44, o_CreatePipelineLibrary, hkCreatePipelineLibrary);
        if (SUCCEEDED(d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library))))
        {
            auto** lib = *reinterpret_cast<void***>(library.Get());
            DeviceHook(lib, 9, o_LoadGraphicsPipeline, hkLoadGraphicsPipeline);
            DeviceHook(lib, 10, o_LoadComputePipeline, hkLoadComputePipeline);
            Microsoft::WRL::ComPtr<ID3D12PipelineLibrary1> l1;
            if (SUCCEEDED(library.As(&l1)))
                DeviceHook(*reinterpret_cast<void***>(l1.Get()), 13, o_LoadPipeline, hkLoadPipeline);
        }
        else
            LOG_INFO("Spatial pass: this driver does not support pipeline libraries; library hooks not installed");
    }
    if (SUCCEEDED(real->QueryInterface(IID_PPV_ARGS(&d2))))
        DeviceHook(*reinterpret_cast<void***>(d2.Get()), 47, o_CreatePipelineState, hkCreatePipelineState);
    if (SUCCEEDED(real->QueryInterface(IID_PPV_ARGS(&d4))))
        DeviceHook(*reinterpret_cast<void***>(d4.Get()), 51, o_CreateCommandList1, hkCreateCommandList1);
    DeviceHook(bt, 8, o_Map, hkMap);
    DeviceHook(bt, 9, o_Unmap, hkUnmap);
    if (bt[2] != lt[2])
        DeviceHook(bt, 2, o_ResourceRelease, hkResourceRelease);
    DeviceHook(*reinterpret_cast<void***>(queue.Get()), 10, o_ExecuteCommandLists, hkExecuteCommandLists);
    DeviceHook(lt, 2, o_Release, ReleaseHook::Detour);
    g_tables.Hook(2, reinterpret_cast<void*>(ReleaseHook::Table));
    DeviceHook(lt, 9, o_Close, CloseHook::Detour);
    g_tables.Hook(9, reinterpret_cast<void*>(CloseHook::Table));
    DeviceHook(lt, 10, o_Reset, ResetHook::Detour);
    g_tables.Hook(10, reinterpret_cast<void*>(ResetHook::Table));
#define LIST(Name, Slot) ListHook<Name##Hook>(lt, Slot)
    LIST(ClearState, 11);
    LIST(DrawInstanced, 12);
    LIST(DrawIndexedInstanced, 13);
    LIST(Dispatch, 14);
    LIST(RSSetViewports, 21);
    LIST(RSSetScissorRects, 22);
    LIST(SetPipelineState, 25);
    LIST(ResourceBarrier, 26);
    LIST(ExecuteBundle, 27);
    LIST(SetDescriptorHeaps, 28);
    LIST(SetComputeRootSignature, 29);
    LIST(SetGraphicsRootSignature, 30);
    LIST(SetComputeRootDescriptorTable, 31);
    LIST(SetGraphicsRootDescriptorTable, 32);
    LIST(SetComputeRoot32BitConstant, 33);
    LIST(SetGraphicsRoot32BitConstant, 34);
    LIST(SetComputeRoot32BitConstants, 35);
    LIST(SetGraphicsRoot32BitConstants, 36);
    LIST(SetComputeRootConstantBufferView, 37);
    LIST(SetGraphicsRootConstantBufferView, 38);
    LIST(SetComputeRootShaderResourceView, 39);
    LIST(SetGraphicsRootShaderResourceView, 40);
    LIST(SetComputeRootUnorderedAccessView, 41);
    LIST(SetGraphicsRootUnorderedAccessView, 42);
    LIST(OMSetRenderTargets, 46);
    LIST(SetPredication, 55);
    LIST(ExecuteIndirect, 59);
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> l4;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> l7;
    if (SUCCEEDED(list.As(&l4)))
    {
        LIST(BeginRenderPass, 68);
        LIST(EndRenderPass, 69);
    }
    if (SUCCEEDED(list.As(&l7)))
        LIST(Barrier, 80);
    LOG_INFO("Spatial pass: mesh draws are not followed (private list tables have no interface-length gate)");
#undef LIST
    list->Close();
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG result = NO_ERROR;
    for (UINT i = 0; i < g_hookCount && result == NO_ERROR; ++i)
        result = DetourAttach(g_hooks[i].original, g_hooks[i].detour);
    if (result != NO_ERROR)
        DetourTransactionAbort();
    else
        result = DetourTransactionCommit();
    if (result != NO_ERROR)
    {
        LOG_ERROR("Spatial pass: hooking failed ({:X}), not installed", UINT(result));
        return;
    }
    for (size_t i = 0; i < std::size(kCreationSlots); ++i)
        g_creationCode[i].store(deviceTable[kCreationSlots[i]], std::memory_order_release);
    tracking->ready.store(true, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    LOG_INFO("Spatial pass: watching for FSR1 upscale passes to replace with the upscaler (raster, compute, stream "
             "and library pipelines); buffers {} Release of their own",
             o_ResourceRelease ? "have a" : "share the lists'");
}
static void EndFrame()
{
    const auto presents = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    g_movedLastFrame.store(g_applied.exchange(0, std::memory_order_relaxed) != 0, std::memory_order_relaxed);
    if (g_hudlessSource.exchange(nullptr))
        g_hudlessMissed.fetch_add(1, std::memory_order_relaxed);
    if (g_jitterOff.load(std::memory_order_relaxed) || presents - g_lastPassPresent.load(std::memory_order_relaxed) > 4)
        g_jitter.store(0, std::memory_order_relaxed);
    else if (g_passSinceAdvance.exchange(false, std::memory_order_relaxed) && g_inW.load() && g_inH.load())
    {
        // Halton (2, 3), as many phases as FSR asks for this ratio
        const double ratio = double(g_outW.load()) / double(g_inW.load());
        const uint32_t count = (std::max) (1u, uint32_t(8.0 * ratio * ratio));
        const uint32_t phase = g_phase.fetch_add(1, std::memory_order_relaxed) % count + 1;
        const auto halton = [](uint32_t i, uint32_t base)
        {
            float f = 1.f, r = 0.f;
            for (; i > 0; i /= base)
            {
                f /= float(base);
                r += f * float(i % base);
            }
            return r;
        };
        const float jitter[2] { halton(phase, 2) - 0.5f, halton(phase, 3) - 0.5f };
        uint64_t bits = 0;
        std::memcpy(&bits, jitter, sizeof(bits));
        g_jitter.store(bits, std::memory_order_relaxed);
        g_sceneViewports.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_jitterOff.load(std::memory_order_relaxed))
        g_jitter.store(0, std::memory_order_relaxed);
    const auto easu = g_easuFrame.exchange(0, std::memory_order_relaxed),
               rcas = g_rcasFrame.exchange(0, std::memory_order_relaxed);
    static constinit uint64_t totalEasu = 0, totalRcas = 0, one = 0, minimum = UINT64_MAX, maximum = 0;
    totalEasu += easu;
    totalRcas += rcas;
    one += easu == 1;
    minimum = (std::min) (minimum, easu);
    maximum = (std::max) (maximum, easu);
    if (presents % 600)
        return;
    for (auto& device : spatial_tracking::devices)
        if (auto* object = device.object.load(std::memory_order_acquire);
            object && object != reinterpret_cast<ID3D12Device*>(1))
            LOG_INFO("Spatial tracking: device {:X}, generation {}; heap attempts {}, success {}, ignored {}, failure "
                     "{}, queries {}; "
                     "loose operations {}, writes {}, failure {}; root attempts {}, resolved {}, failure {}; "
                     "unavailable {}/{}; active {} ready {}",
                     size_t(object), device.generation, device.heapAttempts.exchange(0), device.heapSuccess.exchange(0),
                     device.heapIgnored.exchange(0), device.heapFailure.exchange(0), device.heapQueries.exchange(0),
                     device.looseOperations.exchange(0), device.looseWrites.exchange(0),
                     device.looseFailure.exchange(0), device.rootAttempts.exchange(0), device.rootSuccess.exchange(0),
                     device.rootFailure.exchange(0), device.descriptorFailed.load(), device.rootFailed.load(),
                     device.active.load(), device.ready.load());
    const auto decoded = g_constants.exchange(0), fromTexture = g_textureRect.exchange(0);
    UINT lists = 0, mapped = 0;
    UINT64 mappedBytes = 0;
    for (auto& slot : g_seenLists)
        lists += slot.exchange(nullptr) != nullptr;
    for (auto& m : g_mapped)
        if (auto* r = m.resource.load(std::memory_order_acquire); r && r != reinterpret_cast<ID3D12Resource*>(1))
        {
            ++mapped;
            mappedBytes += m.size.load();
        }
    LOG_INFO(
        "Spatial pass: {} frames, EASU draws {} ({}-{} a frame, exactly one in {} frames) on {} lists, RCAS draws {}; "
        "constants decoded {}/{} (root parameter {}, {}x{} format {} to {}x{} format {}); recorded on thread {}",
        600, totalEasu, minimum, maximum, one, lists, totalRcas, decoded, totalEasu, g_constantParameter.load(),
        g_inW.load(), g_inH.load(), g_inFormat.load(), g_outW.load(), g_outH.load(), g_outFormat.load(),
        g_thread.load());
    LOG_INFO("Spatial pass: {:.0f} tracked list calls a frame, {} lists in the table ({} calls on lists it had no room "
             "for); {} mapped buffers known ({:.1f} MB), {:.1f} Map calls a frame ({} not kept, the table full); {} "
             "pipelines examined; frame generation presented {} of these frames; the swap chain presented {} times",
             double(g_calls.exchange(0)) / 600, g_lists.Used(), g_untracked.exchange(0) + g_lists.TakeUntracked(),
             mapped, double(mappedBytes) / 1048576, double(g_mapCalls.exchange(0)) / 600, g_mapsNotKept.exchange(0),
             g_examined.load(), g_fgPresents.exchange(0), g_swapPresents.exchange(0));
    const auto other = g_otherViewport.exchange(0);
    const auto jitterFrames = g_sceneViewports.exchange(0);
    if (jitterFrames)
        g_motionVectorsDecided.store(true, std::memory_order_relaxed);
    LOG_INFO("Spatial pass: scene jitter on {} of these frames, {} draws moved ({} of them indirect, bundled or mesh), "
             "{} upscales given an offset older than "
             "the frame's, {} given the frame's offset before any of its draws; last depth-bound geometry viewport "
             "of another size {}x{}",
             jitterFrames, g_jitteredDraws.exchange(0), g_otherDraws.exchange(0), g_stale.exchange(0),
             g_beforeScene.exchange(0), other >> 16, other & 0xFFFF);
    LOG_INFO("Spatial pass: scene jitter: {} lists were recorded across the game's present, each keeping the offset "
             "it began with",
             g_midList.exchange(0));
    LOG_INFO("Spatial pass: frame generation given the sharpened frame as its HUD-less colour {} times ({} refused, "
             "{} not the swap chain's size, {} inside a render pass); {} replaced frames without a sharpening pass "
             "taking the upscale, {} sharpening passes read something else",
             g_hudless.exchange(0), g_hudlessRefused.exchange(0), g_hudlessSize.exchange(0),
             g_hudlessInPass.exchange(0), g_hudlessMissed.exchange(0), g_hudlessOther.exchange(0));
    const auto tables = g_tables.TakeCounts();
    LOG_INFO("Spatial pass: {} FSR1 pipeline binds; list tables of their own {} ({} patched, {} repatched, {} foreign, "
             "{} failed)",
             g_binds.exchange(0), tables.tables, tables.patched, tables.repatched, tables.foreign, tables.failed);
    LOG_INFO("Spatial pass: raster draw {}, indexed {}, dispatch {}; input rect from the texture {}; pipelines not "
             "followed {}; planned {} passes",
             g_shapes[0].exchange(0), g_shapes[1].exchange(0), g_shapes[2].exchange(0), fromTexture,
             g_overflow.exchange(0), decoded + fromTexture);
    for (size_t i = 1; i < size_t(Refusal::Count); ++i)
        if (auto count = g_refusals[i].exchange(0))
            LOG_INFO("Spatial pass: fallback '{}' {}", Name(Refusal(i)), count);
    totalEasu = totalRcas = one = maximum = 0;
    minimum = UINT64_MAX;
}
void OnFGPresent()
{
    if (ThreadHasNoTls() || !Installed())
        return;
    g_fgPresentMs.store(Util::MillisecondsNow(), std::memory_order_relaxed);
    g_fgPresents.fetch_add(1, std::memory_order_relaxed);
    if (g_frameSource.exchange(2) != 2)
        LOG_INFO("Spatial pass: the game's frames end at its present into frame generation's swap chain");
    EndFrame();
}
void OnPresent()
{
    if (ThreadHasNoTls() || !Installed())
        return;
    g_swapPresents.fetch_add(1, std::memory_order_relaxed);
    if (native::lowlatency::SwapChainPresentIgnored(Util::MillisecondsNow(),
                                                    g_fgPresentMs.load(std::memory_order_relaxed)))
        return;
    if (g_frameSource.exchange(1) != 1)
        LOG_INFO("Spatial pass: the game's frames end at its swap chain present");
    EndFrame();
}
} // namespace SpatialPassDx12
