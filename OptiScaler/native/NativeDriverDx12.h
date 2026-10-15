#pragma once

// The native input producer's motion step, in a live game (Story 2 of the epic): while the game makes no upscaler call, the
// finished picture is run through OpticalFlowDx12 once per presented frame, on a command list of our own on the swap chain's
// queue, and the flow can be looked at in the DLSS-NR menu. Nothing is changed in the game's picture or lists.
//
// Switched on with [DlssNr] NativeMotion (the menu checkbox applies at once). It waits while the game calls an upscaler, the
// same condition the depth finder stands down for (and runs from the start when the finder is not installed).

#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>

namespace native
{
class NativeDriver;
}

namespace NativeMotionDx12
{
// The session: this driver's native::NativeDriver with its producer and virtual upscaler, and the step's frame source.
// Lock order: the step's run lock, then the session, then frame generation's Mutex or the depth finder's lock. Nothing
// locks it before the in-list pass's hooks are installed ([Upscalers] ReplaceGameFsr1): only the present uses the
// session then, as before the pass existed. Once they are, the present holds it for its whole step (OnPresent,
// OnFGPresent: outside frame generation's present lock); while frame generation exists (State::currentFG, active or
// not) OnPresent only tries it, as its thread can be frame generation's presenter, and leaves the step out when it is
// busy (BusyPresents). The menu only tries it. A game's recording thread (the in-list pass) only tries it, with
// TrySession.
class Session
{
  public:
    Session(Session&& other) noexcept : _owned(other._owned) { other._owned = false; } // not copied, not assigned
    ~Session();

    explicit operator bool() const { return _owned; }

    // The driver, while this session is owned; null otherwise. Producer() and VirtualUpscaler() may be null (no
    // present has made them yet); neither is deleted while the session is owned.
    native::NativeDriver* Driver() const;

  private:
    friend Session TrySession();
    explicit Session(bool owned) : _owned(owned) {}

    bool _owned = false;
};

// Never waits: owned, or not (the present or the menu has it). No allocation, no thread-local storage, no exception:
// an SRW lock.
Session TrySession();

// How many presents OnPresent left out because the session was busy (its try, see Session): the depth finder's frame
// was not closed and the step did not run on them. Any thread; for the in-list pass's log.
uint64_t BusyPresents();

// Both close the depth finder's frame (GenericDepthDx12::OnPresent) and then run the step, so the step reads the frame just
// closed and the finder closes once per game frame.
//
// Once per presented frame, from the wrapped swap chain's present, before DLSS-NR's finished-picture pass (which composes
// from what an upscaler call captured this frame) and before the menu is drawn. `queue` is the swap chain's queue. Does
// nothing while OnFGPresent is being called.
void OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device);

// Once per game frame under frame generation, from FGHooks::FGPresent before frame generation presents and before it takes
// its lock: the picture is the game's own, `queue` the game's. Frame generation's Upscaler input is fed from here.
void OnFGPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device);

// ImGui: one status line for the mode in effect (waiting/failed, NR or the stabiliser on this picture, or why not).
// Call inside the menu, on the Optical F5Low page (dlssnr/DlssNr_Menu.cpp), while NativeMotion
// is on.
void DrawStatus();

// NR is running on the finished picture (the menu header's "Optical F5Low: NR on the finished picture").
bool NrOnlyRunning();

// ImGui: with NativeDebugView, the flow tuning sliders and flow/trust picture previews. The keys themselves are set by
// the menu's mode selector (dlssnr/DlssNr_NativeMode.h). Call inside the menu's Advanced section.
void DrawAdvancedUi();

} // namespace NativeMotionDx12
