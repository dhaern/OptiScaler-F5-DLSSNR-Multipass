#pragma once

#include <d3d12.h>

#include "GenericDepth_Select.h"

// Finds the scene's depth buffer in a DirectX 12 game by watching how the game uses its depth-stencil buffers, with no
// ReShade and no help from an upscaler call. The first stage of the native input producer (DLSS-NR in a game that has
// no DLSS/FSR/XeSS call); see GenericDepth_Select.h for the heuristic itself.
//
// Off unless [DlssNr] NativeDepthFinder or [Upscalers] ReplaceGameFsr1 is set (startup only); off, nothing is hooked.
// On, the hooks only observe: CreateDepthStencilView and the descriptor copies (which resource a depth descriptor is),
// OMSetRenderTargets (which depth buffer a list draws into), ClearDepthStencilView and the draws (how much): no change.
namespace GenericDepthDx12
{
// Hooks the device and a command list once; a no-op when the key is off or it has run already.
void Install(ID3D12Device* device);

// Once per presented frame: closes the frame's counts, picks, and logs. The swap chain gives the picture's size.
void OnPresent(IDXGISwapChain* swapChain);

// The pick as of the last present: valid == false until a buffer qualifies.
GenericDepthSelect::Pick CurrentPick();

// Copies of the picked depth buffer from the frame that was just presented, for the producer's later steps. A game that
// spreads its scene over several command lists, in a different split each frame, leaves each copy holding only part of
// the scene; the frame's copies are all given (copies[]), and the nearest surface over all of them is the scene
// whatever the split was. resource is the copy of the stretch that drew the most. Made only while the debug overlay,
// [DlssNr] NativeMotion or [Upscalers] ReplaceGameFsr1 is on, and the copy is recorded into the game's own lists. The
// texture rests in NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE; it is overwritten by the next frame's copies,
// which run later on the same queue than anything recorded at present.
struct Snapshot
{
    static constexpr int kMaxCopies = 8;

    bool valid = false;
    ID3D12Resource* resource = nullptr;            // the copy of the stretch that drew the most (the menu's preview)
    ID3D12Resource* copies[kMaxCopies] = {};       // every copy of the frame, in no particular order
    int copyCount = 0;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN; // a typed format that reads its depth: R32_FLOAT, R16_UNORM, ...
    uint32_t width = 0;
    uint32_t height = 0;
    bool reversed = false;                        // near is 1.0
    uint64_t frame = 0;
};

Snapshot BestSnapshot();

// The same, with a multisampled buffer's copies resolved (once per frame) on a list of our own submitted to `queue`,
// which must be the queue the copies are read on, so that its order puts the resolve before their reader. What the
// producer reads.
Snapshot ResolvedSnapshot(ID3D12CommandQueue* queue);

struct CopyNow
{
    bool valid = false;
    const char* why = "";
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t width = 0, height = 0;
    bool reversed = false;
    uint64_t frame = 0;
    uint32_t ordinal = 0;
    uint64_t vertices = 0;
    uint32_t copiesInList = 0;
    uint64_t pick = 0;
    bool acrossPresent = false;
};

// Most vertices among this recording's readable copies of the current pick; ties take the later copy. The list must
// be the pointer seen by the hooks, on a thread with TLS. A deferred MSAA resolve returns "resolve pending" before
// the mutex; otherwise one try_lock returns "busy" rather than waiting. Only the list's own device may use the slots.
// Close forgets the recording (including Reset without execution); remaking slots forgets all recordings. An open
// list may keep a single-sample copy across a present until its slot is reused. Each frame has eight distinct slots;
// further copies are skipped, never overwrite another list's slot. This assumes execution on one direct queue.
// The resource is borrowed, readable in `state` (NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE), and must be
// restored to that state before returning the list. Retain it through the reader's fence, not a number of presents.
// Releasing an open list is not tracked: reusing its address before the slots are replaced can leave a stale entry.
CopyNow CopyThisFrame(ID3D12GraphicsCommandList* list);

// Records the copy the menu's preview shows onto the menu's own list after Reset. No-op unless the overlay is on.
void RecordPreviewCopy(ID3D12GraphicsCommandList* list);

bool Installed();

// The key was on at this start but Install() gave up (the log says why).
bool InstallFailed();

// Called when the game creates an upscaler feature (every D3D12 input goes through FeatureProvider_Dx12::GetFeature): a game
// with an upscaler of its own needs no depth finder, so it stands down for good.
void NoteUpscalerCall();

// The game has called an upscaler within the last couple of seconds, whether or not the finder is installed
// (native::GameUpscalerCalledRecently, or the installed finder's own present-counted window).
bool GameCallsUpscaler();

// The warm-up is over and no upscaler call was seen: the finder is allowed to pick and report.
bool Armed();

// ImGui: the menu's one depth line (off, needs a restart, could not start, stood down, watching, picked). No checkbox.
// Call inside the menu, on the Optical F5Low page (dlssnr/DlssNr_Menu.cpp).
void DrawStatus();

// ImGui: the "Use the game's depth" and "Show the picked depth here" checkboxes, and with [DlssNr] NativeDepthOverlay
// the picked depth buffer as a grayscale image. [DlssNr] NativeDepthFinder is also turned on and off by the menu's mode
// selector (dlssnr/DlssNr_NativeMode.h); this is for setting it on its own. Call inside the menu's Advanced section.
void DrawAdvancedUi();
} // namespace GenericDepthDx12
