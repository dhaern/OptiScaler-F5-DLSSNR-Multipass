#pragma once

// The API-neutral half of the depth finder: the counting, the pick, the warm-up and the stand-down, for the native input
// producer (DLSS-NR in a game that makes no upscaler call). It knows nothing of Direct3D: an adapter for a game API (today
// resource_tracking/GenericDepth_Dx12.cpp; D3D11, D3D10 and D3D9 later) hooks its API, turns what it sees into the events below
// (plain numbers, no API types), and does the physical copies of the picked depth that this class asks for. See
// docs/NATIVE-INPUT-ADAPTERS.md for what an adapter must do.
//
// The rules (vertices weighed over draw calls, the viewport and workload tests on clears, the best stretch to copy, the
// reversed-Z hint from the clear value) are adapted from ReShade's Generic Depth add-on, Copyright (C) 2021 Patrick Mours,
// BSD-3-Clause; see Licenses/ReShade_GenericDepth_LICENSE.txt. The heuristic itself is in GenericDepth_Select.h.
//
// Contexts: a "context" is whatever records draws in order and can bind one depth buffer at a time: a D3D12 command list, or
// the immediate context of D3D11, D3D10 and D3D9 (one context for the whole game). The adapter identifies it with a number.
//
// Threads: every event may come from any game thread; the class locks inside. The adapter must not call it while holding a
// lock that a snapshot copy also takes.

#include "../resource_tracking/GenericDepth_Select.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace native
{

// Marks the current thread's calls into an upscaler as our own synthetic one (native input presented to a real upscaler
// backend as if the game had called it), so NoteUpscalerCall() below does not stand the finder down over its own call.
// Construct one around the whole synthetic Evaluate sequence; nesting on the same thread is fine.
class SyntheticUpscalerCallScope
{
  public:
    SyntheticUpscalerCallScope();
    ~SyntheticUpscalerCallScope();
    SyntheticUpscalerCallScope(const SyntheticUpscalerCallScope&) = delete;
    SyntheticUpscalerCallScope& operator=(const SyntheticUpscalerCallScope&) = delete;
};

// Whether the game itself calls an upscaler, known whether or not a depth finder is installed: the menu's native modes
// and the frame sources ask it. Every upscaler call notes itself (the adapters' NoteUpscalerCall, before their own
// installed check); our synthetic call inside a SyntheticUpscalerCallScope does not. A call counts for
// kGameUpscalerQuietMs after it, about the finder's quiet window at 60 frames a second.
constexpr int64_t kGameUpscalerQuietMs = 2000;
void NoteGameUpscalerCall();
bool GameUpscalerCalledRecently();

// A depth buffer as the adapter identifies it. `id` is stable for the buffer's life (a pointer will do).
struct DepthBuffer
{
    uint64_t id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;        // DXGI_FORMAT or the API's own number, only reported
    bool readOnlyDepth = false; // bound read-only for depth (D3D12 puts the buffer in the depth-read state while bound)
};

// What the core asks of the adapter after an event: copy the picked buffer now (the adapter makes the copy into its own slot).
struct SnapshotRequest
{
    bool take = false;
    uint64_t id = 0;
    bool readOnlyDepth = false;
    uint64_t stretchVertices = 0; // what the stretch being copied drew
    bool reversed = false;        // the pick's depth direction when the copy was asked (decides a resolve's sample)
    const char* where = "";       // "clear", "close" or "unbind", for the log
};

// What the hooks have seen since start, for the log: tells "the game has no depth buffer at this point" (a menu, a video)
// from "the hooks are blind". The adapter bumps the ones only it can see.
struct HookCounters
{
    std::atomic<uint64_t> depthViewsCreated { 0 };
    std::atomic<uint64_t> binds { 0 };           // every bind of render targets
    std::atomic<uint64_t> bindsWithDepth { 0 };  // ... that carried a depth buffer
    std::atomic<uint64_t> bindsUnknownDepth { 0 }; // ... of which the depth buffer was not one the adapter knew
    std::atomic<uint64_t> draws { 0 };           // every draw, whatever was bound
    std::atomic<uint64_t> indirect { 0 };        // every indirect draw, whatever was bound
    std::atomic<uint64_t> bundles { 0 };         // every bundle execution (D3D12): draws recorded in a bundle may be unseen
    std::atomic<uint64_t> dispatches { 0 };      // every compute dispatch
    std::atomic<int> bundleSameDraw { -1 };      // 1 the bundle's draw functions are the direct list's, 0 not, -1 unknown
};

class DepthFinderCore
{
  public:
    using LogFn = std::function<void(const std::string&)>;

    // Arms the hooks' counting. `log` receives the "Depth finder" lines.
    void Start(LogFn log);
    bool Active() const { return _active.load(std::memory_order_relaxed); }

    // Whether the adapter should copy the picked depth at all (the overlay or the motion step wants it).
    void SetSnapshotsWanted(bool wanted) { _snapshotsWanted = wanted; }

    // ---- the game's upscaler: a game that has one needs no finder ---------------------------------------------------------
    // One relaxed store, callable from anywhere on every upscaler call. OnPresent decides what it means. A no-op inside a
    // SyntheticUpscalerCallScope on this thread.
    void NoteUpscalerCall();
    bool GameCallsUpscaler() const { return _upscalerSeen.load(); }
    bool Armed() const { return _armed.load() && !_upscalerSeen.load(); }

    // ---- events from the adapter's hooks ------------------------------------------------------------------------------------
    void OnDraw(uint64_t context, uint64_t vertices, uint32_t instances);
    void OnIndirect(uint64_t context, uint32_t maxCount);
    void OnViewport(uint64_t context, float width); // the main viewport's width, as the context sets it

    // The context now has `bound` as its depth buffer (nullptr: none or one the adapter does not know; `hadDepth` says a depth
    // buffer was given at all). `leaving` receives the request to copy the buffer it moves off, if one is due.
    SnapshotRequest OnDepthBound(uint64_t context, bool hadDepth, const DepthBuffer* bound);

    // A depth clear of `buffer` to `value`. Returns the request to copy the buffer before it is cleared, if one is due.
    SnapshotRequest OnDepthClear(uint64_t context, const DepthBuffer& buffer, float value);

    // The context ends (a D3D12 command list closes) while it may still be bound to the picked buffer.
    SnapshotRequest OnContextEnd(uint64_t context);

    // A context that is never unbound, never cleared again and never closes (a D3D11 immediate context the whole game uses):
    // takes the stretch drawn into the picked buffer so far, without forgetting that this context is bound to it, so a game
    // that only calls OMSetRenderTargets once keeps being tracked after the first copy. Call once per presented frame, before
    // BeginPresent, so a frame that drew something into the picked buffer is never missed just because the game neither
    // cleared nor rebound it.
    SnapshotRequest FlushForPresent(uint64_t context);

    // ---- once per presented frame, in two parts around the adapter's own end-of-frame work -----------------------------------
    // Part one: closes the frame's counts and finds the stretch floor. Returns the present count before this frame.
    uint64_t BeginPresent(uint32_t pictureWidth, uint32_t pictureHeight);
    // Part two: counts the present, stands down or wakes, and (after warm-up) picks. True when it stood down this frame (the
    // adapter then drops its copies).
    bool EndPresent(uint32_t pictureWidth, uint32_t pictureHeight, uint32_t warmupFrames);

    // The pick as of the last present.
    GenericDepthSelect::Pick CurrentPick() const;
    uint64_t Presents() const { return _presentsNow.load(std::memory_order_relaxed); }
    uint64_t WarmupStart() const;                     // the present count the current warm-up began at
    uint64_t SnapshotFloor() const;                   // the least a stretch must draw to be copied

    // Diagnostics only (logged by an adapter when a valid pick still gets no copy, to say which check is failing): what the
    // context is bound to right now, and how much it has drawn into it since the last clear/unbind/flush.
    struct Diagnostic
    {
        bool hasContext = false;
        bool hasBoundBuffer = false;
        uint64_t boundResource = 0;
        uint64_t currentVertices = 0;
        uint32_t currentDrawcalls = 0;
        bool wanted = false;
    };
    Diagnostic Diagnose(uint64_t context) const;
    HookCounters& Counters() { return _counters; }

  private:
    struct DrawStats
    {
        uint64_t vertices = 0;
        uint32_t drawcalls = 0;
        uint32_t drawcallsIndirect = 0;
        float lastViewportWidth = 0.0f;
    };

    // One depth buffer's counts for the frame being recorded.
    struct Stats
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;
        uint64_t resource = 0; // set when a context binds it (0: only seen cleared)
        bool readOnlyDepth = false;
        DrawStats total;
        DrawStats current;      // since the last clear
        uint32_t clears = 0;    // clears that came after real work
        int32_t bestClear = -1; // the clear a snapshot would be taken at
        // Cleared to something other than 1.0. Kept across frames: a frame whose clear the hooks miss would otherwise report
        // the same buffer as not reversed, and a consumer that latches it (an upscaler's creation flags) rebuilds.
        bool reversed = false;
        bool clearedThisFrame = false;
    };

    struct ContextState
    {
        Stats* stats = nullptr;     // the depth buffer this context draws into now
        float viewportWidth = 0.0f; // its main viewport
    };

    static void AddDraw(DrawStats& s, uint64_t vertices, uint32_t drawcalls, bool indirect);
    void LogCandidates(const std::vector<GenericDepthSelect::Candidate>& frame, const GenericDepthSelect::Pick& pick,
                       uint32_t pictureWidth, uint32_t pictureHeight);
    void Say(const std::string& line) const
    {
        if (_log)
            _log(line);
    }

    // The lock for a per-draw hook (OnDraw, OnIndirect, OnViewport), measuring how often and how long a draw waited for
    // it: reported with the candidates, to decide whether the draw path needs to go lock-free.
    std::unique_lock<std::mutex> LockForDraw();

    mutable std::mutex _mutex;
    std::atomic<uint64_t> _drawLockWaits { 0 };     // draws that found the lock taken, since the last report
    std::atomic<uint64_t> _drawLockWaitNs { 0 };    // their total wait
    std::atomic<uint64_t> _drawLockWaitMaxNs { 0 }; // the longest one
    uint64_t _drawsAtLastReport = 0;
    LogFn _log;
    HookCounters _counters;

    // Whether the hooks count. Cleared while the game is seen making an upscaler call, so a game that has one pays a relaxed
    // load per call and nothing more.
    std::atomic<bool> _active { false };
    std::atomic<bool> _upscalerSeen { false };       // an upscaler call was seen within the last kQuietPresents presents
    std::atomic<uint64_t> _presentsNow { 0 };        // _presents, readable without the lock
    std::atomic<uint64_t> _lastUpscalerCall { 0 };   // the present count when the game last called an upscaler (0 never)
    std::atomic<bool> _armed { false };
    std::atomic<bool> _snapshotsWanted { false };

    std::unordered_map<uint64_t, ContextState> _contexts;
    std::unordered_map<uint64_t, Stats> _stats; // node-stable: _contexts holds pointers into it
    std::vector<GenericDepthSelect::Candidate> _frameCandidates;
    uint64_t _snapshotFloor = 0;
    float _pictureWidth = 0.0f;
    uint64_t _presents = 0;
    uint64_t _warmupStart = 0;
    uint64_t _frames = 0;
    uint64_t _lastLoggedFrame = 0;
    uint64_t _lastLoggedPick = 0;
    GenericDepthSelect::Selector _selector;
    GenericDepthSelect::Pick _pick;

    static constexpr uint64_t kLogEveryFrames = 600;
    // The finder stands down while the game is calling an upscaler and wakes again once it has stopped for this many presents
    // (a game's settings menu turning its upscaler off: Cyberpunk creates its Ray Reconstruction feature at startup, long
    // before anyone reaches the setting). About two seconds at 60 fps.
    static constexpr uint64_t kQuietPresents = 120;
};

} // namespace native
