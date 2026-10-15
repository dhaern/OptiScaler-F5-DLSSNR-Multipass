#include "pch.h"
#include "OpticalFlow_Dx12.h"

#include "../native/F5LowShaderBytecode.h"

#include <algorithm>
#include <cstring>

namespace
{

constexpr uint32_t kDescriptorsPerPass = 10; // eight SRVs and two UAVs
constexpr uint32_t kPassesPerFrame = 1 + 2 + (OpticalFlowDx12::kLevels - 1) + OpticalFlowDx12::kLevels + 1 + 1 + 1 + 1;
constexpr uint32_t kFramesInFlight = 8;
constexpr DXGI_FORMAT kLumaFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kFlowFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // typed UAV stores of this are required of every device
constexpr DXGI_FORMAT kStateFormat = DXGI_FORMAT_R32_UINT; // typed atomics on this are required of every device
constexpr DXGI_FORMAT kAgeFormat = DXGI_FORMAT_R8_UINT;    // the still age: typed UAV stores of this are required too

// The shaders are compiled ahead of time (motion/OpticalFlow_Hlsl.h -> native/F5LowShaderBytecode.h): a compile here
// ran on the present thread when Optical F5Low started. `lumaMode` is LUMA_MODE's value for a Luma variant.
const F5LowShaderBytecode::Shader* Compile(const char* entry, std::string* error, const char* group = "OpticalFlow",
                                           const char* lumaMode = "")
{
    const auto* shader = F5LowShaderBytecode::Find(group, entry, lumaMode);

    if (shader == nullptr)
        *error = std::string("no bytecode for ") + group + " " + entry;

    return shader;
}

} // namespace

OpticalFlowDx12::~OpticalFlowDx12()
{
    ReleaseTextures();

    for (ID3D12PipelineState** pso : { &_luma, &_lumaSdr, &_lumaScRgb, &_lumaPq, &_down, &_match, &_median, &_smooth,
                                       &_visualise, &_global })
        if (*pso != nullptr)
            (*pso)->Release();

    if (_rootSignature != nullptr)
        _rootSignature->Release();
    if (_heap != nullptr)
        _heap->Release();
}

bool OpticalFlowDx12::Init(ID3D12Device* device)
{
    if (device == nullptr)
    {
        _error = "no device";
        return false;
    }

    _device = device;

    // One table (eight SRVs, two UAVs) and the root constants.
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

    for (const Entry& entry : { Entry { "Luma", &_luma }, Entry { "Down", &_down }, Entry { "Match", &_match },
                                Entry { "Median", &_median }, Entry { "Smooth", &_smooth },
                                Entry { "Visualise", &_visualise }, Entry { "Global", &_global } })
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

    // The luma in the three other ways (see LumaOf): the same entry point built with another LUMA_MODE.
    {
        struct Variant
        {
            const char* mode;
            ID3D12PipelineState** target;
        };

        for (const Variant& variant :
             { Variant { "1", &_lumaSdr }, Variant { "2", &_lumaScRgb }, Variant { "3", &_lumaPq } })
        {
            const auto* code = Compile("Luma", &_error, "OpticalFlow", variant.mode);

            if (code == nullptr)
                return false;

            D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
            pso.pRootSignature = _rootSignature;
            pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };

            const HRESULT hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(variant.target));
            code->Release();

            if (FAILED(hr))
            {
                _error = "creating the pipeline Luma";
                return false;
            }
        }
    }

    if (!_sceneCut.Init(device))
    {
        _error = _sceneCut.Error();
        return false;
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
    return true;
}

bool OpticalFlowDx12::CreateTexture(Tex& tex, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name)
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
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
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

void OpticalFlowDx12::ReleaseTextures()
{
    auto release = [](Tex& tex)
    {
        if (tex.resource != nullptr)
            tex.resource->Release();
        tex = Tex {};
    };

    for (auto& set : _pyramid)
        for (auto& tex : set)
            release(tex);

    for (auto& set : _levelFlow)
        for (auto& tex : set)
            release(tex);

    release(_flowMedian);
    release(_globalFlow);
    release(_flow);
    release(_preview);

    for (Tex& age : _age)
        release(age);
}

bool OpticalFlowDx12::EnsureSize(uint32_t width, uint32_t height, int finestLevel)
{
    finestLevel = std::clamp(finestLevel, 0, kLevels - 1);
    if (width == _width && height == _height && finestLevel == _finestLevel && _flow.resource != nullptr)
        return true;

    // The previous work may still be reading these; a size change is rare (a resolution change) and the caller is expected
    // to have waited, as it does before replacing its own targets.
    ReleaseTextures();
    _havePrevious = false;
    _globalReady = false;
    _sceneCut.Reset();
    _sceneCutRan = false;
    _ageValid = false;
    _flowValid = false;
    _width = width;
    _height = height;
    _finestLevel = finestLevel;
    const uint32_t divisor = 2u << finestLevel;
    const uint32_t flowWidth = (width + divisor - 1) / divisor, flowHeight = (height + divisor - 1) / divisor;

    uint32_t w = (width + 1) / 2;
    uint32_t h = (height + 1) / 2;

    for (int level = 0; level < kLevels; ++level)
    {
        for (int set = 0; set < 2; ++set)
            if (!CreateTexture(_pyramid[set][level], w, h, kLumaFormat, L"OpticalFlow_Luma"))
                return false;

        for (int set = 0; set < 2; ++set)
            if (!CreateTexture(_levelFlow[set][level], w, h, kFlowFormat, L"OpticalFlow_LevelFlow"))
                return false;

        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }

    for (Tex& age : _age)
        if (!CreateTexture(age, flowWidth, flowHeight, kAgeFormat, L"OpticalFlow_StillAge"))
            return false;

    return CreateTexture(_flowMedian, flowWidth, flowHeight, kFlowFormat, L"OpticalFlow_FlowMedian") &&
           CreateTexture(_globalFlow, 1, 1, kFlowFormat, L"OpticalFlow_Global") &&
           CreateTexture(_flow, flowWidth, flowHeight, kFlowFormat, L"OpticalFlow_Flow") &&
           CreateTexture(_preview, flowWidth, flowHeight, DXGI_FORMAT_R8G8B8A8_UNORM, L"OpticalFlow_Preview");
}

void OpticalFlowDx12::Transition(ID3D12GraphicsCommandList* list, Tex& tex, D3D12_RESOURCE_STATES state)
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

void OpticalFlowDx12::Pass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* src0,
                           DXGI_FORMAT format0, ID3D12Resource* src1, DXGI_FORMAT format1, ID3D12Resource* src2,
                           DXGI_FORMAT format2, Tex& dst, DXGI_FORMAT dstFormat, const Constants& constants,
                           ID3D12Resource* src3, DXGI_FORMAT format3, ID3D12Resource* src4, DXGI_FORMAT format4,
                           ID3D12Resource* src5, DXGI_FORMAT format5, ID3D12Resource* src6, DXGI_FORMAT format6)
{
    Transition(list, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    StampBegin(list);

    const UINT first = _heapCursor;
    _heapCursor = (_heapCursor + kDescriptorsPerPass) % (kDescriptorsPerPass * kPassesPerFrame * kFramesInFlight);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T) first * _descriptorSize;
    gpu.ptr += (UINT64) first * _descriptorSize;

    ID3D12Resource* sources[8] = { src0,
                                   src1 != nullptr ? src1 : src0,
                                   src2 != nullptr ? src2 : src0,
                                   src3 != nullptr ? src3 : src0,
                                   src4 != nullptr ? src4 : src0,
                                   src5 != nullptr ? src5 : src0,
                                   src6 != nullptr ? src6 : src0,
                                   _passAgeIn != nullptr ? _passAgeIn : src0 };
    const DXGI_FORMAT formats[8] = { format0,
                                     src1 != nullptr ? format1 : format0,
                                     src2 != nullptr ? format2 : format0,
                                     src3 != nullptr ? format3 : format0,
                                     src4 != nullptr ? format4 : format0,
                                     src5 != nullptr ? format5 : format0,
                                     src6 != nullptr ? format6 : format0,
                                     _passAgeIn != nullptr ? kAgeFormat : format0 };

    for (int i = 0; i < 8; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = formats[i];
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(sources[i], &srv, cpu);
        cpu.ptr += _descriptorSize;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = dstFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _device->CreateUnorderedAccessView(dst.resource, nullptr, &uav, cpu);
    cpu.ptr += _descriptorSize;

    // The second UAV is the still age the finest match writes; every other pass gets a view of nothing.
    D3D12_UNORDERED_ACCESS_VIEW_DESC ageUav {};
    ageUav.Format = kAgeFormat;
    ageUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _device->CreateUnorderedAccessView(_passAgeOut, nullptr, &ageUav, cpu);

    ID3D12DescriptorHeap* heaps[] = { _heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(_rootSignature);
    list->SetPipelineState(pso);
    list->SetComputeRootDescriptorTable(0, gpu);
    list->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &constants, 0);
    list->Dispatch((dst.width + 7) / 8, (dst.height + 7) / 8, 1);

    // Anything that reads it next reads it as a texture.
    Transition(list, dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    StampEnd(list, pso);
}

// The test's timing: a timestamp before the first pass of a frame, then one after each (the barrier at the end of a
// pass is in it).
void OpticalFlowDx12::StampBegin(ID3D12GraphicsCommandList* list)
{
    if (_timeHeap != nullptr && _timeCount == 0 && _timeCapacity >= 2 && _timeCapacity <= 64)
    {
        list->EndQuery(_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, _timeBase + _timeCount);
        _timeNames[_timeCount++] = "start";
    }
}

void OpticalFlowDx12::StampEnd(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const char* name)
{
    if (_timeHeap == nullptr || _timeCount == 0 || _timeCount >= _timeCapacity || _timeCount >= 64)
        return;

    list->EndQuery(_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, _timeBase + _timeCount);
    _timeNames[_timeCount++] = name != nullptr ? name
                               : pso == _luma || pso == _lumaSdr || pso == _lumaScRgb || pso == _lumaPq ? "luma"
                               : pso == _down                                                         ? "down"
                               : pso == _match                                                        ? "match"
                               : pso == _median                                                       ? "median"
                               : pso == _smooth                                                       ? "smooth"
                               : pso == _global                                                       ? "global"
                                                                                                      : "other";
}

bool OpticalFlowDx12::Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* color, DXGI_FORMAT colorFormat,
                               ID3D12Resource* depth, DXGI_FORMAT depthFormat, bool depthReversed, Encoding encoding,
                               int finestLevel, int smoothRadius, float jitterX, float jitterY)
{
    if (_device == nullptr || _match == nullptr || list == nullptr || color == nullptr)
        return false;

    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();

    if (!EnsureSize((uint32_t) colorDesc.Width, colorDesc.Height, finestLevel))
    {
        _error = "creating the textures";
        return false;
    }

    // Build the current frame's pyramid.
    auto& current = _pyramid[_current];
    auto& previous = _pyramid[1 - _current];

    Constants constants {};
    constants.sizeX = current[0].width;
    constants.sizeY = current[0].height;
    constants.auxX = (uint32_t) colorDesc.Width;
    constants.auxY = colorDesc.Height;
    constants.whiteNits = (std::max)(_settings.hdrWhiteNits, 1.0f);
    constants.jitterX = jitterX;
    constants.jitterY = jitterY;
    Pass(list,
         !_settings.perceptualLuma     ? _luma
         : encoding == Encoding::ScRgb ? _lumaScRgb
         : encoding == Encoding::Pq    ? _lumaPq
                                       : _lumaSdr,
         color, colorFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN, current[0], kLumaFormat,
         constants);
    constants.jitterX = constants.jitterY = 0.f;

    // Is this frame a hard cut from the last one? Decided here, on the GPU, so the match and the trust mask can act on
    // it in this very frame.
    _sceneCutRan = false;

    if (_settings.sceneCutDetector)
    {
        StampBegin(list);
        _sceneCutRan = _sceneCut.Detect(list, current[0].resource, _settings.sceneCutThreshold);
        StampEnd(list, nullptr, "scenecut");
    }
    else
        _sceneCut.Reset();

    for (int level = 1; level < kLevels; ++level)
    {
        constants.sizeX = current[level].width;
        constants.sizeY = current[level].height;
        constants.auxX = current[level - 1].width;
        constants.auxY = current[level - 1].height;
        Pass(list, _down, current[level - 1].resource, kLumaFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr,
             DXGI_FORMAT_UNKNOWN, current[level], kLumaFormat, constants);
    }

    const bool history = _flowValid; // the last frame made a flow: its levels are candidates
    _flowValid = false;
    auto& levelNow = _levelFlow[_current];
    auto& levelBefore = _levelFlow[1 - _current];

    // Depth-aware matching, when there is depth.
    const bool depthMatching = _settings.depthMatching && depth != nullptr && depthFormat != DXGI_FORMAT_UNKNOWN;
    _usedDepth = depthMatching;
    const D3D12_RESOURCE_DESC depthDesc = depthMatching ? depth->GetDesc() : D3D12_RESOURCE_DESC {};

    bool wroteAge = false; // the finest match wrote this frame's ages

    if (_havePrevious)
    {
        for (int level = kLevels - 1; level >= _finestLevel; --level)
        {
            const bool coarsest = level == kLevels - 1;

            constants = Constants {};
            constants.sizeX = current[level].width;
            constants.sizeY = current[level].height;
            constants.radius = coarsest ? _settings.coarseRadius : _settings.radius;
            constants.hasPrediction = coarsest ? 0 : 1;
            constants.lambda = _settings.lambda;
            constants.hasHistory = (history && _settings.useHistory) ? 1 : 0;
            constants.coarseCells = (uint32_t) std::clamp(_settings.coarseCells, 1, 9);
            constants.knee = _settings.confidenceKnee;
            constants.depthMatching = depthMatching ? 1 : 0;
            constants.depthX = depthMatching ? (uint32_t) depthDesc.Width : 1;
            constants.depthY = depthMatching ? depthDesc.Height : 1;
            constants.reversed = depthReversed ? 1 : 0;
            constants.hasGlobal = _globalReady ? 1 : 0;
            constants.inverseRefinement = _settings.inverseRefinement ? 1 : 0;
            constants.sceneCutEnabled = _sceneCutRan ? 1 : 0;
            constants.zeroMargin = level == _finestLevel ? _settings.zeroMargin : 0.0f;
            constants.zeroReach = (uint32_t) std::clamp(_settings.zeroReach, 0, 64);
            constants.scale = 1.0f / (float) (2 << level); // full-resolution pixels in this level's
            constants.stillFrames = level == _finestLevel ? (uint32_t) std::clamp(_settings.stillFrames, 0, 255) : 0;
            constants.hasAge = _ageValid ? 1 : 0;
            constants.stillEpsilon = _settings.stillEpsilon;
            constants.stillMargin = _settings.stillMargin;

            // The brightness weights, on the finest level (with depth on top of the depth weights).
            constants.lookWeights = level == _finestLevel && _settings.lookWeights ? 1 : 0;
            constants.lookRange = (std::max)(_settings.lookRange, 1e-4f);
            constants.lookDistance = (std::max)(_settings.lookDistance, 0.5f);

            if (!coarsest)
            {
                constants.auxX = current[level + 1].width;
                constants.auxY = current[level + 1].height;
            }

            // The finest level reads the last frame's ages and writes this frame's (no pass of its own).
            const bool keepAge = constants.stillFrames != 0;

            if (keepAge)
            {
                _passAgeIn = _age[1 - _current].resource;
                _passAgeOut = _age[_current].resource;
                Transition(list, _age[_current], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }

            Pass(list, _match, current[level].resource, kLumaFormat, previous[level].resource, kLumaFormat,
                 coarsest ? nullptr : levelNow[level + 1].resource, kFlowFormat, levelNow[level], kFlowFormat,
                 constants, (history && _settings.useHistory) ? levelBefore[level].resource : nullptr, kFlowFormat,
                 depthMatching ? depth : nullptr, depthFormat, _globalReady ? _globalFlow.resource : nullptr,
                 kFlowFormat, _sceneCutRan ? _sceneCut.Flag() : nullptr, kStateFormat);

            if (keepAge)
            {
                Transition(list, _age[_current], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                _passAgeIn = _passAgeOut = nullptr;
                wroteAge = true;
            }
        }

        constants = Constants {};
        constants.sizeX = _flow.width;
        constants.sizeY = _flow.height;
        constants.scale = float(2u << _finestLevel);
        Pass(list, _median, levelNow[_finestLevel].resource, kFlowFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr,
             DXGI_FORMAT_UNKNOWN, _flowMedian, kFlowFormat, constants);

        constants = Constants {};
        constants.sizeX = _flow.width;
        constants.sizeY = _flow.height;
        constants.radius = std::clamp(smoothRadius < 0 ? _settings.smoothRadius : smoothRadius, 0,
                                      4); // the shader's tile holds at most four pixels around a group
        Pass(list, _smooth, _flowMedian.resource, kFlowFormat, current[_finestLevel].resource, kLumaFormat, nullptr,
             DXGI_FORMAT_UNKNOWN, _flow, kFlowFormat, constants);

        if (_settings.globalCandidate)
        {
            // What the whole frame did, for the next one to try everywhere (see Global).
            constants = Constants {};
            constants.sizeX = 1;
            constants.sizeY = 1;
            constants.auxX = _flow.width;
            constants.auxY = _flow.height;
            constants.depthMatching = depthMatching ? 1 : 0;
            constants.depthX = depthMatching ? (uint32_t) depthDesc.Width : 1;
            constants.depthY = depthMatching ? depthDesc.Height : 1;
            constants.reversed = depthReversed ? 1 : 0;
            Pass(list, _global, _flow.resource, kFlowFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN,
                 _globalFlow, kFlowFormat, constants, nullptr, DXGI_FORMAT_UNKNOWN, depthMatching ? depth : nullptr,
                 depthFormat);
        }

        _globalReady = _settings.globalCandidate;
        _flowValid = true;
    }

    // The flow can be read by a pixel shader too (the menu preview, a consumer's sampling).
    Transition(list, _flow,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // The ages this frame wrote are next frame's last ones, if the match wrote them (taken from what ran, not from the
    // settings, which the menu can change meanwhile).
    _ageValid = wroteAge;

    _current = 1 - _current;
    _havePrevious = true;
    return true;
}

bool OpticalFlowDx12::Visualise(ID3D12GraphicsCommandList* list, float maxSpeed)
{
    if (!_flowValid || _preview.resource == nullptr || list == nullptr)
        return false;

    // The flow rests in the combined read state; the pass wants the non-pixel one it was written to.
    Transition(list, _flow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Constants constants {};
    constants.sizeX = _preview.width;
    constants.sizeY = _preview.height;
    constants.scale = maxSpeed;
    Pass(list, _visualise, _flow.resource, kFlowFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN,
         _preview, DXGI_FORMAT_R8G8B8A8_UNORM, constants);

    Transition(list, _flow,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(list, _preview, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}
