#include "pch.h"
#include "TrustMask_Dx12.h"

#include "../native/F5LowShaderBytecode.h"

#include <algorithm>
#include <cstring>

namespace
{

constexpr uint32_t kDescriptorsPerPass = 10; // eight SRVs, two UAVs
constexpr uint32_t kPassesPerFrame = 7;
constexpr uint32_t kFramesInFlight = 8;
constexpr DXGI_FORMAT kFlowFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kMaskFormat = DXGI_FORMAT_R8_UNORM;

// The shaders are compiled ahead of time (motion/TrustMask_Hlsl.h -> native/F5LowShaderBytecode.h): a compile here ran
// on the present thread when Optical F5Low started.
const F5LowShaderBytecode::Shader* Compile(const char* entry, std::string* error)
{
    const auto* shader = F5LowShaderBytecode::Find("TrustMask", entry);

    if (shader == nullptr)
        *error = std::string("no bytecode for TrustMask ") + entry;

    return shader;
}

} // namespace

TrustMaskDx12::~TrustMaskDx12()
{
    ReleaseTextures();

    for (ID3D12PipelineState** pso :
         { &_depthProxy, &_clearCounter, &_trust, &_copyFlow, &_guideDepthPso, &_guideMotionPso, &_uncoverPso })
        if (*pso != nullptr)
            (*pso)->Release();

    if (_rootSignature != nullptr)
        _rootSignature->Release();
    if (_heap != nullptr)
        _heap->Release();
}

bool TrustMaskDx12::Init(ID3D12Device* device)
{
    if (device == nullptr)
    {
        _error = "no device";
        return false;
    }

    _device = device;

    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 8;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 8;

    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = sizeof(Constants) / 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;

    ID3DBlob* serialized = nullptr;
    ID3DBlob* messages = nullptr;

    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &messages)))
    {
        _error = "serializing the root signature";
        if (messages != nullptr)
            messages->Release();
        return false;
    }

    const HRESULT rootResult = device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                           serialized->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
    serialized->Release();

    if (messages != nullptr)
        messages->Release();

    if (FAILED(rootResult))
    {
        _error = "creating the root signature";
        return false;
    }

    struct Entry
    {
        const char* name;
        ID3D12PipelineState** target;
    };

    for (const Entry& entry : { Entry { "DepthProxy", &_depthProxy }, Entry { "ClearCounter", &_clearCounter },
                                Entry { "Trust", &_trust }, Entry { "CopyFlow", &_copyFlow },
                                Entry { "GuideDepth", &_guideDepthPso }, Entry { "GuideMotion", &_guideMotionPso },
                                Entry { "Uncover", &_uncoverPso } })
    {
        const auto* code = Compile(entry.name, &_error);

        if (code == nullptr)
            return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = _rootSignature;
        pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };

        const HRESULT hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(entry.target));
        code->Release();

        if (FAILED(hr))
        {
            _error = std::string("creating the pipeline ") + entry.name;
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kDescriptorsPerPass * kPassesPerFrame * kFramesInFlight;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&_heap))))
    {
        _error = "creating the descriptor heap";
        return false;
    }

    _descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The counter of fully distrusted pixels, and where it is copied to be read.
    D3D12_HEAP_PROPERTIES defaultHeap {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readHeap {};
    readHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 256;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_counter))))
    {
        _error = "creating the counter";
        return false;
    }

    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;

    for (auto& readback : _readback)
        if (FAILED(device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        {
            _error = "creating a readback buffer";
            return false;
        }

    return true;
}

bool TrustMaskDx12::CreateTexture(Tex& tex, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    tex.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, tex.state, nullptr,
                                                IID_PPV_ARGS(&tex.resource))))
        return false;

    tex.resource->SetName(name);
    tex.width = width;
    tex.height = height;
    return true;
}

void TrustMaskDx12::ReleaseTextures()
{
    for (Tex* tex : { &_depth[0], &_depth[1], &_mask[0], &_mask[1], &_flowBefore, &_guideFlow, &_guideDepth, &_guideMotion })
    {
        if (tex->resource != nullptr)
            tex->resource->Release();
        *tex = Tex {};
    }

    if (_counter != nullptr)
    {
        _counter->Release();
        _counter = nullptr;
    }

    for (auto& readback : _readback)
        if (readback != nullptr)
        {
            readback->Release();
            readback = nullptr;
        }
}

bool TrustMaskDx12::EnsureSize(ID3D12Device*, uint32_t width, uint32_t height)
{
    if (width == _width && height == _height && _mask[0].resource != nullptr)
        return true;

    // The textures are replaced; the owner has waited for the GPU as it does for any size change.
    for (Tex* tex : { &_depth[0], &_depth[1], &_mask[0], &_mask[1], &_flowBefore, &_guideFlow })
    {
        if (tex->resource != nullptr)
            tex->resource->Release();
        *tex = Tex {};
    }

    _haveHistory = false;
    _width = width;
    _height = height;
    _share = -1.0f;
    std::fill(std::begin(_readbackFrame), std::end(_readbackFrame), 0ull);

    return CreateTexture(_depth[0], width, height, kDepthFormat, L"TrustMask_Depth0") &&
           CreateTexture(_depth[1], width, height, kDepthFormat, L"TrustMask_Depth1") &&
           CreateTexture(_mask[0], width, height, kMaskFormat, L"TrustMask_Mask0") &&
           CreateTexture(_mask[1], width, height, kMaskFormat, L"TrustMask_Mask1") &&
           CreateTexture(_flowBefore, width, height, kFlowFormat, L"TrustMask_FlowBefore") &&
           CreateTexture(_guideFlow, width, height, kFlowFormat, L"TrustMask_GuideFlow");
}

void TrustMaskDx12::Transition(ID3D12GraphicsCommandList* list, Tex& tex, D3D12_RESOURCE_STATES state)
{
    if (tex.state == state)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex.resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = tex.state;
    barrier.Transition.StateAfter = state;
    list->ResourceBarrier(1, &barrier);
    tex.state = state;
}

void TrustMaskDx12::Pass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* const (&srv)[8],
                         const DXGI_FORMAT (&formats)[8], Tex& dst, DXGI_FORMAT dstFormat, uint32_t groupsX,
                         uint32_t groupsY, const Constants& constants)
{
    Transition(list, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const UINT total = kDescriptorsPerPass * kPassesPerFrame * kFramesInFlight;
    const UINT first = _heapCursor;
    _heapCursor = (_heapCursor + kDescriptorsPerPass) % total;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T) first * _descriptorSize;
    gpu.ptr += (UINT64) first * _descriptorSize;

    for (int i = 0; i < 8; ++i)
    {
        // A slot a pass does not use still needs a valid view: it repeats the first.
        ID3D12Resource* resource = srv[i] != nullptr ? srv[i] : srv[0];

        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.Format = srv[i] != nullptr ? formats[i] : formats[0];
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(resource, &view, cpu);
        cpu.ptr += _descriptorSize;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = dstFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _device->CreateUnorderedAccessView(dst.resource, nullptr, &uav, cpu);
    cpu.ptr += _descriptorSize;

    D3D12_UNORDERED_ACCESS_VIEW_DESC counter {};
    counter.Format = DXGI_FORMAT_R32_TYPELESS;
    counter.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    counter.Buffer.NumElements = 64;
    counter.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    _device->CreateUnorderedAccessView(_counter, nullptr, &counter, cpu);

    ID3D12DescriptorHeap* heaps[] = { _heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(_rootSignature);
    list->SetPipelineState(pso);
    list->SetComputeRootDescriptorTable(0, gpu);
    list->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &constants, 0);
    list->Dispatch(groupsX, groupsY, 1);

    Transition(list, dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // A pass that reads the counter's result next, or writes it again, sees the previous pass's writes.
    D3D12_RESOURCE_BARRIER uavBarrier {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = _counter;
    list->ResourceBarrier(1, &uavBarrier);
}

bool TrustMaskDx12::Dispatch(ID3D12GraphicsCommandList* list, const Inputs& in)
{
    // in.depthCount == 0 is a frame without depth: the mask then rests on flow and luma alone.
    if (_device == nullptr || _trust == nullptr || list == nullptr || in.flow == nullptr || in.lumaNow == nullptr ||
        in.lumaBefore == nullptr || in.flowWidth == 0 || in.flowHeight == 0 ||
        (in.depthCount > 0 && in.depths[0] == nullptr))
        return false;

    if (!EnsureSize(_device, in.flowWidth, in.flowHeight))
    {
        _error = "creating the textures";
        return false;
    }

    ++_frame;

    // Counts from frames a few back (the GPU is done with them by now): the newest one is the share to report.
    uint64_t newest = 0;

    for (int i = 0; i < kReadbacks; ++i)
    {
        if (_readbackFrame[i] == 0 || _frame - _readbackFrame[i] < 3 || _readbackFrame[i] < newest)
            continue;

        D3D12_RANGE range { 0, 4 };
        uint32_t* data = nullptr;

        if (SUCCEEDED(_readback[i]->Map(0, &range, (void**) &data)) && data != nullptr)
        {
            _share = (float) *data / (float) ((uint64_t) _width * _height);
            D3D12_RANGE none { 0, 0 };
            _readback[i]->Unmap(0, &none);
            newest = _readbackFrame[i];
        }
    }

    const int write = 1 - _depthIndex;
    const int maskWrite = 1 - _maskIndex;
    const DXGI_FORMAT depthIn = kDepthFormat;
    const uint32_t groupsX = (in.flowWidth + 7) / 8;
    const uint32_t groupsY = (in.flowHeight + 7) / 8;

    Constants constants {};
    constants.sizeX = in.flowWidth;
    constants.sizeY = in.flowHeight;
    constants.depthX = in.depthWidth != 0 ? in.depthWidth : in.flowWidth;
    constants.depthY = in.depthHeight != 0 ? in.depthHeight : in.flowHeight;
    constants.depthTolerance = _settings.depthTolerance;
    constants.flowTolerance = _settings.flowTolerance;
    constants.lumaTolerance = _settings.lumaTolerance;
    constants.decay = _settings.decay;
    constants.reversed = in.depthReversed ? 1 : 0;
    constants.hasHistory = _haveHistory ? 1 : 0;
    constants.fullPerFlow = in.fullPerFlow;
    constants.revealTolerance = _settings.revealTolerance;
    constants.depthCount = (uint32_t) (std::min)(in.depthCount, (int) Inputs::kMaxDepths);
    constants.debugView = (uint32_t) _settings.debugView;
    // Depth coming or going compares a real depth with the all-far proxy, which would distrust every pixel.
    constants.depthBoth = in.depthCount > 0 && _hadDepth ? 1 : 0;
    constants.sceneCutEnabled = in.sceneCut != nullptr ? 1 : 0;

    // 1. the depth proxy at the flow's size
    {
        // Eight depth slots (t0..t7): the copies given, the first one again for any not given (every slot needs a
        // view). With no depth the flow stands in; the shader reads none.
        ID3D12Resource* srv[8];
        DXGI_FORMAT formats[8];

        for (int i = 0; i < 8; ++i)
        {
            srv[i] = i < in.depthCount ? in.depths[i] : (in.depthCount > 0 ? in.depths[0] : in.flow);
            formats[i] = in.depthCount > 0 ? in.depthFormat : kFlowFormat;
        }
        Pass(list, _depthProxy, srv, formats, _depth[write], kDepthFormat, groupsX, groupsY, constants);
    }

    // 2. the counter back to zero
    {
        ID3D12Resource* const srv[8] = { _flowBefore.resource, _flowBefore.resource, _flowBefore.resource,
                                         _flowBefore.resource, _flowBefore.resource, _flowBefore.resource,
                                         _flowBefore.resource };
        const DXGI_FORMAT formats[8] = { kFlowFormat, kFlowFormat, kFlowFormat, kFlowFormat,
                                         kFlowFormat, kFlowFormat, kFlowFormat };
        Pass(list, _clearCounter, srv, formats, _mask[maskWrite], kMaskFormat, 1, 1, constants);
    }

    // 3. the mask
    {
        ID3D12Resource* const srv[8] = {
            in.flow,    _flowBefore.resource, _depth[write].resource,     _depth[_depthIndex].resource,
            in.lumaNow, in.lumaBefore,        _mask[_maskIndex].resource, in.sceneCut
        };
        const DXGI_FORMAT formats[8] = { kFlowFormat,  kFlowFormat,           kDepthFormat,
                                         kDepthFormat, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                         kMaskFormat,  DXGI_FORMAT_R32_UINT };
        Pass(list, _trust, srv, formats, _mask[maskWrite], kMaskFormat, groupsX, groupsY, constants);
    }

    // The counter to a readback slot, to be read a few frames on.
    {
        const int slot = (int) (_frame % kReadbacks);

        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = _counter;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(_readback[slot], 0, _counter, 0, 4);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);

        // Only frames that had a history count toward a scene cut.
        _readbackFrame[slot] = _haveHistory ? _frame : 0;
    }

    // 4. this frame's flow, for the next frame's consistency check
    {
        ID3D12Resource* const srv[8] = { in.flow,
                                         _depth[write].resource,
                                         _depth[write].resource,
                                         _depth[write].resource,
                                         _depth[write].resource,
                                         _depth[write].resource,
                                         _depth[write].resource };
        const DXGI_FORMAT formats[8] = { kFlowFormat, kDepthFormat, kDepthFormat, kDepthFormat,
                                         kDepthFormat, kDepthFormat, kDepthFormat };
        Pass(list, _copyFlow, srv, formats, _flowBefore, kFlowFormat, groupsX, groupsY, constants);
    }

    (void) depthIn;
    _depthIndex = write;
    _maskIndex = maskWrite;

    // The newest mask can be read by a pixel shader too (a menu preview).
    Transition(list, _mask[_maskIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    _haveHistory = true;
    _hadDepth = in.depthCount > 0;
    // After a Reset the other proxy is another scene's: not a frame before this one.
    _depthBoth = constants.depthBoth != 0 && constants.hasHistory != 0;
    return true;
}

bool TrustMaskDx12::BuildGuides(ID3D12GraphicsCommandList* list, const Inputs& in, uint32_t width, uint32_t height,
                                bool copyDepth, DXGI_FORMAT motionFormat)
{
    // in.depthCount == 0 still builds a motion-only guide (GuideMotion(), from the flow alone); the depth guide
    // pass is skipped below and GuideDepth() reports null rather than a previous frame's depth.
    if (_device == nullptr || _guideDepthPso == nullptr || list == nullptr || in.flow == nullptr || width == 0 ||
        height == 0 || (in.depthCount > 0 && in.depths[0] == nullptr))
        return false;

    auto ensureGuide = [&](Tex& tex, DXGI_FORMAT format, const wchar_t* name)
    {
        if (tex.resource != nullptr && tex.width == width && tex.height == height &&
            tex.resource->GetDesc().Format == format)
            return true;
        if (tex.resource != nullptr)
            tex.resource->Release();
        tex = Tex {};
        return CreateTexture(tex, width, height, format, name);
    };
    if (!ensureGuide(_guideMotion, motionFormat, L"TrustMask_GuideMotion") ||
        (copyDepth && in.depthCount > 0 && !ensureGuide(_guideDepth, kDepthFormat, L"TrustMask_GuideDepth")))
    {
        _error = "creating the guide textures";
        return false;
    }

    Constants constants {};
    constants.sizeX = width;
    constants.sizeY = height;
    constants.depthX = in.depthWidth != 0 ? in.depthWidth : width;
    constants.depthY = in.depthHeight != 0 ? in.depthHeight : height;
    constants.reversed = in.depthReversed ? 1 : 0;
    constants.depthCount = (uint32_t) (std::min)(in.depthCount, (int) Inputs::kMaxDepths);

    const uint32_t groupsX = (width + 7) / 8;
    const uint32_t groupsY = (height + 7) / 8;

    if (copyDepth && in.depthCount > 0)
    {
        ID3D12Resource* srv[8];
        DXGI_FORMAT formats[8];

        for (int i = 0; i < 8; ++i)
        {
            srv[i] = i < in.depthCount ? in.depths[i] : in.depths[0];
            formats[i] = in.depthFormat;
        }

        Pass(list, _guideDepthPso, srv, formats, _guideDepth, kDepthFormat, groupsX, groupsY, constants);
        _guideDepthValid = true;
    }
    else
    {
        // Nothing written this call: leave the texture as it was (same rest state, no barrier needed) and do
        // not publish it -- GuideDepth() returns null until a frame with depth runs this again.
        _guideDepthValid = false;
    }

    // Background just uncovered takes its surface's motion, at the flow's size, before it is enlarged. Needs this
    // frame's depth proxy and last frame's (both left by Dispatch, which ran on these inputs); the flow is read in its
    // combined read state.
    ID3D12Resource* flow = in.flow;

    if (_settings.uncoverBackground && _uncoverPso != nullptr && _depthBoth && in.depthCount > 0 &&
        _guideFlow.resource != nullptr && _guideFlow.width == in.flowWidth && _guideFlow.height == in.flowHeight)
    {
        Constants uncover = constants;
        uncover.sizeX = in.flowWidth;
        uncover.sizeY = in.flowHeight;
        uncover.fullPerFlow = in.fullPerFlow;
        uncover.revealTolerance = _settings.revealTolerance;
        uncover.uncoverReach = _settings.uncoverReach;

        ID3D12Resource* const srv[8] = { in.flow, in.flow, _depth[_depthIndex].resource, _depth[1 - _depthIndex].resource,
                                         in.flow, in.flow, in.flow,                       in.flow };
        const DXGI_FORMAT formats[8] = { kFlowFormat, kFlowFormat, kDepthFormat, kDepthFormat,
                                         kFlowFormat, kFlowFormat, kFlowFormat, kFlowFormat };
        Pass(list, _uncoverPso, srv, formats, _guideFlow, kFlowFormat, (in.flowWidth + 7) / 8, (in.flowHeight + 7) / 8,
             uncover);
        flow = _guideFlow.resource;
    }

    {
        // The flow sits in its combined read state between frames; the pass reads it as a texture. By surface it also
        // reads the depth guide just written (t2) and this frame's depth proxy at the flow's size (t3, left by Dispatch).
        const bool surface = _settings.guideSurface && _guideDepthValid && _hadDepth;
        constants.guideSurface = surface ? 1 : 0;

        ID3D12Resource* const depthHere = surface ? _guideDepth.resource : flow;
        ID3D12Resource* const depthFlow = surface ? _depth[_depthIndex].resource : flow;
        const DXGI_FORMAT depthFormat = surface ? kDepthFormat : kFlowFormat;

        ID3D12Resource* const srv[8] = { flow, flow, depthHere, depthFlow, flow, flow, flow, flow };
        const DXGI_FORMAT formats[8] = { kFlowFormat, kFlowFormat, depthFormat, depthFormat,
                                         kFlowFormat, kFlowFormat, kFlowFormat, kFlowFormat };
        Pass(list, _guideMotionPso, srv, formats, _guideMotion, motionFormat, groupsX, groupsY, constants);
    }

    return true;
}
