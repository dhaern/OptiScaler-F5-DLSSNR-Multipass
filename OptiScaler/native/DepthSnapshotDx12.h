#pragma once

// The D3D12 depth finder's copy of a multisampled depth buffer, recorded straight into the game's command list as a
// resolve: ResolveSubresourceRegion with MIN or MAX keeps each pixel's nearest sample, which is what the compute
// resolve (native/DepthResolveDx12) gives from a full multisampled copy, without that copy (a 4K 4x D32S8 buffer is
// ~265 MB) or the extra pass. Header only and free of OptiScaler headers, so the GPU test runs the same code the finder
// records.
//
// The target is a single-sample texture of the source's typeless family (R32G8X24_TYPELESS for D32S8, and so on); its
// first subresource is the depth plane, read through `view`. `view` must be the depth plane's read format
// (R32_FLOAT_X8X24_TYPELESS, R32_FLOAT, R24_UNORM_X8_TYPELESS, R16_UNORM), never the DSV format: with the DSV format
// some drivers write zeros or garbage without a debug-layer message.

#include <d3d12.h>

namespace native
{

// Whether `device` may record ResolveSubresourceRegion: the spec ties it to programmable sample positions tier 1 and
// removes a device without it that calls it. Translation layers report tier 0 when they cannot do a depth MIN/MAX
// resolve (vkd3d-proton records nothing then, with no error to the caller), so this is also what keeps them on the copy
// path.
inline bool DepthResolveSupported(ID3D12Device* device)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options {};

    return device != nullptr &&
           SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options, sizeof(options))) &&
           options.ProgrammableSamplePositionsTier >= D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_1;
}

// Whether a buffer of this typeless family is resolved in the list. D24S8 (R24G8) is not: an RTX 5070 Ti (driver
// 32.0.16.1714, tier 2) writes zeros for it with no debug-layer message, through the plane format and the DSV format
// alike, and no format support bit says so (it reports no MULTISAMPLE_RESOLVE for R32_FLOAT_X8X24 either, which
// resolves right). It keeps the copy and compute resolve.
inline bool DepthResolveFormat(DXGI_FORMAT typeless)
{
    return typeless == DXGI_FORMAT_R32_TYPELESS || typeless == DXGI_FORMAT_R32G8X24_TYPELESS ||
           typeless == DXGI_FORMAT_R16_TYPELESS;
}

// Records barriers to the resolve states, the resolve and barriers back into `list`. Nothing else is recorded: no
// pipeline, root signature, heap or render target change. Returns nullptr when recorded, else why it was not.
inline const char* RecordDepthResolve(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                                      D3D12_RESOURCE_STATES sourceState, ID3D12Resource* target,
                                      D3D12_RESOURCE_STATES targetRest, DXGI_FORMAT view, bool reversed)
{
    if (list == nullptr || source == nullptr || target == nullptr)
        return "nothing to resolve";

    // A depth buffer is only ever bound on a direct list, and resolves are only allowed there
    if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return "not a direct command list";

    ID3D12GraphicsCommandList1* list1 = nullptr;

    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list1))) || list1 == nullptr)
        return "the command list has no ResolveSubresourceRegion (ID3D12GraphicsCommandList1)";

    auto barrier = [](ID3D12Resource* r, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = subresource;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };

    // Only the source's depth plane (subresource 0) is read: its stencil plane may rest in another state (a view with
    // read-only depth and writable stencil), so it is left alone. The target is ours and rests whole in targetRest.
    constexpr UINT all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_RESOURCE_BARRIER in[2] = { barrier(source, 0, sourceState, D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
                                     barrier(target, all, targetRest, D3D12_RESOURCE_STATE_RESOLVE_DEST) };
    list1->ResourceBarrier(2, in);

    // Nearest: the largest value with reversed Z, the smallest with standard Z (DepthResolve_Hlsl.h does the same)
    list1->ResolveSubresourceRegion(target, 0, 0, 0, source, 0, nullptr, view,
                                    reversed ? D3D12_RESOLVE_MODE_MAX : D3D12_RESOLVE_MODE_MIN);

    D3D12_RESOURCE_BARRIER out[2] = { barrier(source, 0, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, sourceState),
                                      barrier(target, all, D3D12_RESOURCE_STATE_RESOLVE_DEST, targetRest) };
    list1->ResourceBarrier(2, out);

    list1->Release();
    return nullptr;
}

} // namespace native
