#pragma once
// Inside a game's list, around our work: Enter stops the hooks from taking our calls for the game's, Restore gives the
// list back the bindings the game set (ListStateDx12.h), and RecordWrite copies the upscaler's output into the region
// of the game's target its FSR1 pass would have written. The list is the one the hooks were given: our calls on it
// reach the game's driver through them, untracked. fsr1::Resolve (Fsr1Pass.h) has refused every target that is
// not one single-sample subresource of a typed, non-sRGB-mismatched format, so the target is its subresource 0.
#include "Fsr1Pass.h"
#include "ListStateDx12.h"

namespace native::envelope
{
struct Target
{
    ID3D12Resource* resource = nullptr;
    UINT subresource = 0;
    D3D12_RESOURCE_STATES stateAtCall = D3D12_RESOURCE_STATE_RENDER_TARGET; // where the game's pass found it
    D3D12_BOX box {};                                                       // the region written (front 0, back 1)
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;                           // the format the game writes it through
};
// A raster pass draws into its render target; a compute pass writes its UAV.
inline Target TargetOf(const fsr1::Plan& plan)
{
    return { plan.output.resource, 0,
             plan.shape == fsr1::Shape::Dispatch ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                 : D3D12_RESOURCE_STATE_RENDER_TARGET,
             plan.box, plan.output.format };
}
// The upscaler's output can be copied into the target: the same format family, the region within both.
inline bool CanWrite(const Target& target, const D3D12_RESOURCE_DESC& output)
{
    using fsr1::Family;
    const D3D12_BOX& box = target.box;
    return target.resource != nullptr && output.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           Family(output.Format) != DXGI_FORMAT_UNKNOWN && Family(output.Format) == Family(target.viewFormat) &&
           box.left < box.right && box.top < box.bottom && box.front == 0 && box.back == 1 &&
           box.right - box.left == output.Width && box.bottom - box.top == output.Height;
}
inline bool CanWriteDirect(const Target& target)
{
    if (target.resource == nullptr)
        return false;
    const auto desc = target.resource->GetDesc();
    const auto& box = target.box;
    return (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 &&
           desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 && desc.MipLevels == 1 &&
           desc.DepthOrArraySize == 1 && target.subresource == 0 && desc.Format == target.viewFormat &&
           fsr1::FormatForOutput(desc.Format) == desc.Format && desc.Format != DXGI_FORMAT_UNKNOWN && box.left == 0 &&
           box.top == 0 && box.front == 0 && box.right == desc.Width && box.bottom == desc.Height && box.back == 1;
}
inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, subresource, before, after };
    return b;
}
// Barriers on the target's one subresource and on the output only, each back where it was; the copy is the box.
inline bool RecordWrite(ID3D12GraphicsCommandList* list, const Target& target, ID3D12Resource* output,
                        D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
{
    const D3D12_BOX& box = target.box;
    if (target.resource == nullptr || output == nullptr || target.resource == output || box.left >= box.right ||
        box.top >= box.bottom)
        return false;
    D3D12_RESOURCE_BARRIER before[2], after[2];
    UINT count = 0;
    if (target.stateAtCall != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        before[count] =
            Transition(target.resource, target.subresource, target.stateAtCall, D3D12_RESOURCE_STATE_COPY_DEST);
        after[count++] =
            Transition(target.resource, target.subresource, D3D12_RESOURCE_STATE_COPY_DEST, target.stateAtCall);
    }
    if (outputState != D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        before[count] = Transition(output, 0, outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        after[count++] = Transition(output, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, outputState);
    }
    if (count != 0)
        list->ResourceBarrier(count, before);
    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = target.resource;
    dst.Type = src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = target.subresource;
    src.pResource = output;
    const D3D12_BOX from { 0, 0, 0, box.right - box.left, box.bottom - box.top, 1 };
    list->CopyTextureRegion(&dst, box.left, box.top, 0, &src, &from);
    if (count != 0)
        list->ResourceBarrier(count, after);
    return true;
}
inline void Enter(ListState& state) { state.inEnvelope = true; }
// Our compute work leaves its root signature bound: a game that set none gets its graphics one there instead, so that
// nothing of ours stays bound in its list.
inline void Restore(ListState& state, ListRestoreSink& sink)
{
    state.inEnvelope = true;
    if (state.computeSignature == nullptr && state.graphicsSignature != nullptr)
        sink.SetComputeRootSignature(state.graphicsSignature);
    EmitRestore(state, sink);
    state.inEnvelope = false;
}
// InListPending's fences, one per queue.
struct PendingFencesDx12
{
    using Queue = ID3D12CommandQueue;
    using Fence = ID3D12Fence;
    static bool CreateFence(ID3D12CommandQueue* queue, ID3D12Fence** fence)
    {
        ID3D12Device* device = nullptr;
        if (queue == nullptr || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
            return false;
        const HRESULT made = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence));
        device->Release();
        return SUCCEEDED(made);
    }
    static bool Signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value)
    {
        return SUCCEEDED(queue->Signal(fence, value));
    }
    static uint64_t Completed(ID3D12Fence* fence) { return fence->GetCompletedValue(); }
    static void Release(ID3D12Fence* fence) { fence->Release(); }
};
} // namespace native::envelope
