#pragma once
// The spatial pass's replacement (resource_tracking/SpatialPass_Dx12.cpp finds the game's FSR1 upscale): our optical
// flow, trust mask and upscaler are recorded into the game's own command list in its place, and the game's next calls
// find their bindings as they left them. [Upscalers] ReplaceGameFsr1 on D3D12; nothing here runs with it off.
// Threads: OnPass on the game's recording thread (it takes the session with a try-lock: never waits); Execute,
// OnResetOrRelease and the resource calls on any thread, TLS or not, lock-free but for the submission lock around the
// game's own ExecuteCommandLists of a list holding our work; BeginPresent/EndPresent on the present, under the session.
#include "Fsr1Pass.h"
#include "ListStateDx12.h"
#include "SpatialEnvelopeDx12.h"

namespace native
{
class NativeDriver;
}

namespace native::inlist
{
struct PresentStep
{
    bool runPresentPath = true; // NativeDriver::RunFrame's runPresentPath
    bool keepDevice = false;    // work recorded into a game's list may still run: keep the producer's device
};
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// An EASU pass the detection refused (it counts against the session's fallbacks in a row; logged with its reason).
void NoteRefused(fsr1::Refusal why);
// An EASU pass fsr1::Resolve planned. True: it was replaced, the game's draw or dispatch must not run.
// `restore` gives the list back the game's bindings (ListRestoreToList).
bool OnPass(ID3D12GraphicsCommandList* list, ListState& state, const fsr1::Plan& plan, ListRestoreSink& restore);
// The queue's ExecuteCommandLists: the lists holding our work are followed to a fence of ours, and a list executed
// again gets a fix-up list of barriers just before it.
void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn original);
// A list's Reset or last Release: what its recording holds of ours is released once the GPU is past it.
void OnResetOrRelease(const void* list);
PresentStep BeginPresent(NativeDriver& driver, ID3D12Device* device);
void EndPresent(NativeDriver& driver, ID3D12CommandQueue* queue);

// The device's and the lists' hooks, for the private states of our resources (InListPending.h, ReplayDx12).
void Created(ID3D12Resource* resource, D3D12_RESOURCE_STATES initial, D3D12_HEAP_TYPE heap);
uint64_t ReleaseToken(IUnknown* object);
void Released(uint64_t token);
void Barriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers);
} // namespace native::inlist
