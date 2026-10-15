// GPU check of native/SpatialEnvelopeDx12.h inside a game's list: our compute work recorded between the game's
// bindings and its next pass (another descriptor heap, root signature and PSO of ours), the upscaler's output copied
// into the region of the game's target its FSR1 pass would have written (RecordWrite: barriers on that subresource and
// on the output only, each back where it was), the game's bindings given back (Restore through ListRestoreToList).
// The game's next pass, recorded without binding anything again, must draw exactly what it draws without our work in
// between, the region must hold the output byte for byte and everything else the game's clear. Raster (render target,
// root CBV, SRV table) and compute (UAV target, root constants, SRV+UAV table) shapes, a game without a compute root
// signature, and the same list without the restore (it must be caught). The debug layer must stay silent.
//
//   vcvars64, then from the repo root:
//   cl /nologo /std:c++20 /EHsc /W4 /O2 /FC tests\nr_spatial_envelope_dx12_gpu.cpp
//   nr_spatial_envelope_dx12_gpu.exe [warp] [gbv]
// Exit 0 when every check passes, 1 otherwise.
#include "d3d12_harness.h"
#include "../OptiScaler/native/SpatialEnvelopeDx12.h"

using namespace d3d12_test;
using namespace native;

namespace
{
constexpr UINT kW = 160, kH = 80, kOutW = 64, kOutH = 32, kMarginX = 100;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

const char* kVs = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2);"
                  " return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }";
// The game's passes read its texture and its constants
const char* kGamePs = "Texture2D<float4> t0 : register(t0); cbuffer C : register(b0) { float4 k; };"
                      "float4 main(float4 pos : SV_Position) : SV_Target { return t0.Load(int3(0, 0, 0)) * k; }";
const char* kGameCs = "Texture2D<float4> t0 : register(t0); RWTexture2D<float4> u0 : register(u0); cbuffer K : "
                      "register(b0) { float4 k; };"
                      "[numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID)"
                      "{ u0[id + uint2(k.w, 0)] = t0.Load(int3(0, 0, 0)) * float4(k.xyz, 1); }";
// Ours: the upscaler's output
const char* kOurCs =
    "RWTexture2D<float4> o : register(u0); [numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID)"
    "{ o[id] = float4(id.x / 64.0, id.y / 32.0, 0.25, 1); }";

struct Scene
{
    Gpu& gpu;
    ComPtr<ID3D12Resource> input, target, output;
    ComPtr<ID3D12DescriptorHeap> heap, ours, rtvs;
    ComPtr<ID3D12RootSignature> gameRs, ourRs;
    ComPtr<ID3D12PipelineState> gamePso, ourPso;
    ComPtr<ID3D12Resource> cb;
    bool compute = false;
};

ComPtr<ID3D12RootSignature> Signature(ID3D12Device* device, bool compute, bool ours)
{
    const D3D12_DESCRIPTOR_RANGE ranges[2] { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, ours ? 0u : 1u } };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { ours ? 1u : compute ? 2u : 1u, ours ? ranges + 1 : ranges };
    params[1].ParameterType = compute ? D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS : D3D12_ROOT_PARAMETER_TYPE_CBV;
    if (compute)
        params[1].Constants = { 0, 0, 4 };
    else
        params[1].Descriptor = { 0, 0 };
    const D3D12_ROOT_SIGNATURE_DESC desc { ours ? 1u : 2u, params, 0, nullptr,
                                           D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    return RootSignature(device, desc);
}

ComPtr<ID3D12PipelineState> ComputePso(ID3D12Device* device, ID3D12RootSignature* rs, const char* source)
{
    Bytes cs;
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
    desc.pRootSignature = rs;
    if (CompileHlsl(source, "main", "cs_5_1", cs) != CompileResult::Success)
        return pso;
    desc.CS = { cs.data(), cs.size() };
    Ok(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)), "compute PSO");
    return pso;
}

bool Build(Scene& s)
{
    ID3D12Device* device = s.gpu.device.Get();
    const D3D12_CLEAR_VALUE red { kFormat, { 1, 0, 0, 1 } }, blue { kFormat, { 0.2f, 0.6f, 1, 1 } };
    s.input = Texture(device, 16, 16, kFormat, D3D12_RESOURCE_STATE_RENDER_TARGET,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &blue);
    s.target = Texture(device, kW, kH, kFormat, D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &red);
    s.output = Texture(device, kOutW, kOutH, kFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    s.heap = DescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
    s.ours = DescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    s.rtvs = DescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
    s.cb = Buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD);
    if (!s.input || !s.target || !s.output || !s.heap || !s.ours || !s.rtvs || !s.cb)
        return false;
    const float k[4] { 1, 0.5f, 1, 1 };
    void* mapped = nullptr;
    if (!Ok(s.cb->Map(0, nullptr, &mapped), "constants"))
        return false;
    std::memcpy(mapped, k, sizeof(k));
    s.cb->Unmap(0, nullptr);
    const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto h0 = s.heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(s.input.Get(), nullptr, h0);
    device->CreateUnorderedAccessView(s.target.Get(), nullptr, nullptr, { h0.ptr + stride });
    device->CreateUnorderedAccessView(s.output.Get(), nullptr, nullptr, s.ours->GetCPUDescriptorHandleForHeapStart());
    const auto r0 = s.rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(s.input.Get(), nullptr, r0);
    device->CreateRenderTargetView(
        s.target.Get(), nullptr, { r0.ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) });
    s.gameRs = Signature(device, s.compute, false);
    s.ourRs = Signature(device, true, true);
    if (!s.gameRs || !s.ourRs)
        return false;
    s.ourPso = ComputePso(device, s.ourRs.Get(), kOurCs);
    if (s.compute)
        s.gamePso = ComputePso(device, s.gameRs.Get(), kGameCs);
    else
    {
        Bytes vs, ps;
        if (CompileHlsl(kVs, "main", "vs_5_1", vs) != CompileResult::Success ||
            CompileHlsl(kGamePs, "main", "ps_5_1", ps) != CompileResult::Success)
            return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc {};
        desc.pRootSignature = s.gameRs.Get();
        desc.VS = { vs.data(), vs.size() };
        desc.PS = { ps.data(), ps.size() };
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 15;
        desc.RasterizerState = { D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE };
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.SampleMask = UINT_MAX;
        desc.SampleDesc.Count = 1;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = kFormat;
        Ok(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&s.gamePso)), "raster PSO");
    }
    return s.gamePso && s.ourPso;
}

enum class Run
{
    Reference,    // the game's passes only
    Envelope,     // our work in between, the bindings given back
    NoRestore,    // our work in between, nothing given back
    NoComputeSig, // raster game without a compute signature: ours is replaced by its graphics one
};

// Records the game's frame on gpu.list, the tracked state mirrored by hand as the hooks would; reads the target back
bool Frame(Scene& s, Run run, Bytes& image, UINT64* messages)
{
    auto* l = s.gpu.list.Get();
    ListState state;
    const auto r0 = s.rtvs->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE rt { r0.ptr + s.gpu.device->GetDescriptorHandleIncrementSize(
                                                        D3D12_DESCRIPTOR_HEAP_TYPE_RTV) };
    const float red[] { 1, 0, 0, 1 }, blue[] { 0.2f, 0.6f, 1, 1 };
    l->ClearRenderTargetView(r0, blue, 0, nullptr);
    l->ClearRenderTargetView(rt, red, 0, nullptr);
    D3D12_RESOURCE_BARRIER b[2] {
        Transition(s.input.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(s.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    l->ResourceBarrier(s.compute ? 2 : 1, b);
    const auto targetState = s.compute ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_RENDER_TARGET;
    // The game binds what its FSR1 pass and the pass after it use
    ID3D12DescriptorHeap* heaps[] { s.heap.Get() };
    l->SetDescriptorHeaps(1, heaps);
    state.heaps[0] = heaps[0]; // what OnSetDescriptorHeaps keeps (without the descriptor tracker's lookup)
    state.heapCount = 1;
    const auto table = s.heap->GetGPUDescriptorHandleForHeapStart();
    const float k[4] { 1, 0.5f, 1, float(kMarginX) };
    if (s.compute)
    {
        l->SetComputeRootSignature(s.gameRs.Get());
        OnSetComputeRootSignature(state, s.gameRs.Get());
        l->SetComputeRootDescriptorTable(0, table);
        OnSetComputeRootDescriptorTable(state, 0, table);
        l->SetComputeRoot32BitConstants(1, 4, k, 0);
        OnSetComputeRoot32BitConstants(state, 1, 4, k, 0);
    }
    else
    {
        l->SetGraphicsRootSignature(s.gameRs.Get());
        OnSetGraphicsRootSignature(state, s.gameRs.Get());
        l->SetGraphicsRootDescriptorTable(0, table);
        OnSetGraphicsRootDescriptorTable(state, 0, table);
        l->SetGraphicsRootConstantBufferView(1, s.cb->GetGPUVirtualAddress());
        OnSetGraphicsRootConstantBufferView(state, 1, s.cb->GetGPUVirtualAddress());
        l->OMSetRenderTargets(1, &rt, FALSE, nullptr);
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (run != Run::NoComputeSig) // a game that used compute before in this list
        {
            l->SetComputeRootSignature(s.ourRs.Get());
            OnSetComputeRootSignature(state, s.ourRs.Get());
        }
    }
    l->SetPipelineState(s.gamePso.Get());
    OnSetPipelineState(state, s.gamePso.Get());
    // The replaced FSR1 pass: our work, the copy into the region, the bindings back
    if (run != Run::Reference)
    {
        envelope::Enter(state);
        ID3D12DescriptorHeap* ours[] { s.ours.Get() };
        l->SetDescriptorHeaps(1, ours);
        l->SetComputeRootSignature(s.ourRs.Get());
        l->SetPipelineState(s.ourPso.Get());
        l->SetComputeRootDescriptorTable(0, s.ours->GetGPUDescriptorHandleForHeapStart());
        l->Dispatch(kOutW / 8, kOutH / 8, 1);
        fsr1::Plan plan;
        plan.output.resource = s.target.Get();
        plan.output.format = kFormat;
        plan.shape = s.compute ? fsr1::Shape::Dispatch : fsr1::Shape::Draw;
        plan.box = { 16, 8, 0, 16 + kOutW, 8 + kOutH, 1 };
        const envelope::Target target = envelope::TargetOf(plan);
        const D3D12_RESOURCE_BARRIER uav { D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                           D3D12_RESOURCE_BARRIER_FLAG_NONE,
                                           { { s.output.Get() } } };
        l->ResourceBarrier(1, &uav);
        if (!envelope::CanWrite(target, s.output->GetDesc()) || !envelope::RecordWrite(l, target, s.output.Get()))
            return false;
        if (run != Run::NoRestore)
        {
            ListRestoreToList restore(l);
            envelope::Restore(state, restore);
        }
    }
    // The game's next pass, nothing bound again but where it draws
    if (s.compute)
        l->Dispatch(48 / 8, 32 / 8, 1);
    else
    {
        const D3D12_VIEWPORT vp { float(kMarginX), 0, 48, 32, 0, 1 };
        const D3D12_RECT sc { LONG(kMarginX), 0, LONG(kMarginX + 48), 32 };
        l->RSSetViewports(1, &vp);
        l->RSSetScissorRects(1, &sc);
        l->DrawInstanced(3, 1, 0, 0);
    }
    b[0] = Transition(s.input.Get(), b[0].Transition.StateAfter, D3D12_RESOURCE_STATE_RENDER_TARGET);
    l->ResourceBarrier(1, b);
    const bool read = s.gpu.Readback(s.target.Get(), targetState, image);
    if (s.compute && read)
    {
        b[0] = Transition(s.target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);
        s.gpu.list->ResourceBarrier(1, b);
    }
    *messages = s.gpu.Messages();
    return read && s.gpu.ExecuteAndWait();
}

void Host(Checks& checks)
{
    fsr1::Plan plan;
    int fake = 0;
    plan.output.resource = reinterpret_cast<ID3D12Resource*>(&fake);
    plan.output.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    plan.box = { 0, 0, 0, 128, 64, 1 };
    plan.shape = fsr1::Shape::DrawIndexed;
    envelope::Target t = envelope::TargetOf(plan);
    checks.Check(t.stateAtCall == D3D12_RESOURCE_STATE_RENDER_TARGET && t.subresource == 0,
                 "target: a draw's is its render target, subresource 0");
    plan.shape = fsr1::Shape::Dispatch;
    checks.Check(envelope::TargetOf(plan).stateAtCall == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 "target: a dispatch's is its UAV");
    D3D12_RESOURCE_DESC out {};
    out.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    out.Width = 128;
    out.Height = 64;
    out.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    checks.Check(envelope::CanWrite(t, out), "can write: the same family, the box's size");
    out.Width = 127;
    checks.Check(!envelope::CanWrite(t, out), "can write: not another size");
    out.Width = 128;
    out.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    checks.Check(!envelope::CanWrite(t, out), "can write: not another family");
    out.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    out.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    checks.Check(!envelope::CanWrite(t, out), "can write: not a buffer");
    out.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.box.right = 0;
    checks.Check(!envelope::CanWrite(t, out) && !envelope::RecordWrite(nullptr, t, plan.output.resource),
                 "can write: not an empty box, nothing recorded");
    t = envelope::TargetOf(plan);
    checks.Check(!envelope::RecordWrite(nullptr, t, plan.output.resource), "record: never the target onto itself");
}
} // namespace

int main(int argc, char** argv)
{
    bool warp = false, gbv = false;
    for (int i = 1; i < argc; ++i)
        warp = warp || std::strcmp(argv[i], "warp") == 0, gbv = gbv || std::strcmp(argv[i], "gbv") == 0;
    Checks checks;
    Host(checks);
    Gpu gpu;
    if (!checks.Check(gpu.Create(warp ? Adapter::Warp : Adapter::Hardware, gbv), "a device with the debug layer"))
        return checks.Summary();
    for (bool compute : { false, true })
    {
        Scene s { gpu };
        s.compute = compute;
        const char* shape = compute ? "compute" : "raster";
        if (!checks.Check(Build(s), compute ? "compute game built" : "raster game built"))
            continue;
        Bytes reference, enveloped, unrestored, output;
        UINT64 refMessages = 0, envMessages = 0, noRestoreMessages = 0;
        const bool ran = Frame(s, Run::Reference, reference, &refMessages) &&
                         Frame(s, Run::Envelope, enveloped, &envMessages) &&
                         Frame(s, Run::NoRestore, unrestored, &noRestoreMessages) &&
                         gpu.Readback(s.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output);
        gpu.Messages();
        if (!checks.Check(ran && reference.size() == size_t(kW) * kH * 4 && output.size() == size_t(kOutW) * kOutH * 4,
                          "frames recorded, executed and read back"))
            continue;
        bool region = true, rest = true, margin = false;
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x)
            {
                const size_t i = (size_t(y) * kW + x) * 4;
                const bool inBox = x >= 16 && x < 16 + kOutW && y >= 8 && y < 8 + kOutH;
                if (inBox)
                    region =
                        region && std::memcmp(&enveloped[i], &output[(size_t(y - 8) * kOutW + x - 16) * 4], 4) == 0;
                else
                    rest = rest && std::memcmp(&enveloped[i], &reference[i], 4) == 0;
                margin = margin ||
                         (x >= kMarginX && std::memcmp(&enveloped[i], &reference[i], 4) == 0 && reference[i] != 255);
            }
        std::printf("%s: debug messages reference %llu, envelope %llu, without the restore %llu\n", shape, refMessages,
                    envMessages, noRestoreMessages);
        checks.Check(region, compute ? "compute: the region is the output, byte for byte"
                                     : "raster: the region is the output, byte for byte");
        checks.Check(rest && margin, compute ? "compute: the game's next pass and the rest are the game's own"
                                             : "raster: the game's next pass and the rest are the game's own");
        checks.Check(refMessages == 0 && envMessages == 0, compute ? "compute: the debug layer has nothing to say"
                                                                   : "raster: the debug layer has nothing to say");
        checks.Check(noRestoreMessages != 0 || unrestored != reference,
                     compute ? "compute: without the restore the game's next pass breaks"
                             : "raster: without the restore the game's next pass breaks");
        if (!compute)
        {
            Bytes noSig;
            UINT64 noSigMessages = 0;
            checks.Check(Frame(s, Run::NoComputeSig, noSig, &noSigMessages) && noSigMessages == 0 &&
                             std::memcmp(noSig.data(), enveloped.data(), noSig.size()) == 0,
                         "raster: a game without a compute signature gets its graphics one, silently");
        }
    }
    checks.Check(gpu.Messages() == 0, "the debug layer reported nothing at the end");
    return checks.Summary();
}
