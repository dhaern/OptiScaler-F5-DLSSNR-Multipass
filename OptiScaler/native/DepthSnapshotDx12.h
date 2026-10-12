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

    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };

    D3D12_RESOURCE_BARRIER in[2] = { barrier(source, sourceState, D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
                                     barrier(target, targetRest, D3D12_RESOURCE_STATE_RESOLVE_DEST) };
    list1->ResourceBarrier(2, in);

    // Nearest: the largest value with reversed Z, the smallest with standard Z (DepthResolve_Hlsl.h does the same)
    list1->ResolveSubresourceRegion(target, 0, 0, 0, source, 0, nullptr, view,
                                    reversed ? D3D12_RESOLVE_MODE_MAX : D3D12_RESOLVE_MODE_MIN);

    D3D12_RESOURCE_BARRIER out[2] = { barrier(source, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, sourceState),
                                      barrier(target, D3D12_RESOURCE_STATE_RESOLVE_DEST, targetRest) };
    list1->ResourceBarrier(2, out);

    list1->Release();
    return nullptr;
}

} // namespace native
