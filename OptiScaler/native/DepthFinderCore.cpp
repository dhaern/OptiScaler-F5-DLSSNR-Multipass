// Not built with the precompiled header: no Direct3D in here, so tests/nr_depth_finder_core_smoke.cpp compiles it alone.
#include "DepthFinderCore.h"

#include <algorithm>
#include <chrono>
#include <format>

namespace native
{

namespace
{
thread_local int g_syntheticUpscalerDepth = 0;
std::atomic<int64_t> g_lastGameUpscalerCallMs { 0 }; // steady clock, milliseconds; 0 never

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

SyntheticUpscalerCallScope::SyntheticUpscalerCallScope()
{
    ++g_syntheticUpscalerDepth;
}

SyntheticUpscalerCallScope::~SyntheticUpscalerCallScope()
{
    --g_syntheticUpscalerDepth;
}

void NoteGameUpscalerCall()
{
    if (g_syntheticUpscalerDepth > 0)
        return;

    g_lastGameUpscalerCallMs.store(std::max<int64_t>(NowMs(), 1), std::memory_order_relaxed);
}

bool GameUpscalerCalledRecently()
{
    const int64_t last = g_lastGameUpscalerCallMs.load(std::memory_order_relaxed);
    return last != 0 && NowMs() - last < kGameUpscalerQuietMs;
}

void DepthFinderCore::Start(LogFn log)
{
    _log = std::move(log);
    _active = true;
}

void DepthFinderCore::NoteUpscalerCall()
{
    // Our own synthetic call: the finder must not see it as the game having an upscaler, or it stands down the very pipeline
    // that is feeding the call.
    if (g_syntheticUpscalerDepth > 0)
        return;

    // One relaxed store on every upscaler call: EndPresent decides what it means.
    _lastUpscalerCall.store(std::max<uint64_t>(_presentsNow.load(std::memory_order_relaxed), 1),
                            std::memory_order_relaxed);
}

std::unique_lock<std::mutex> DepthFinderCore::LockForDraw()
{
    std::unique_lock lock(_mutex, std::try_to_lock);

    if (lock.owns_lock())
        return lock;

    const auto start = std::chrono::steady_clock::now();
    lock.lock();
    const auto waited =
        (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
            .count();

    _drawLockWaits.fetch_add(1, std::memory_order_relaxed);
    _drawLockWaitNs.fetch_add(waited, std::memory_order_relaxed);

    uint64_t longest = _drawLockWaitMaxNs.load(std::memory_order_relaxed);

    while (waited > longest && !_drawLockWaitMaxNs.compare_exchange_weak(longest, waited, std::memory_order_relaxed))
    {
    }

    return lock;
}

void DepthFinderCore::AddDraw(DrawStats& s, uint64_t vertices, uint32_t drawcalls, bool indirect)
{
    s.vertices += vertices;
    s.drawcalls += drawcalls;
    if (indirect)
        s.drawcallsIndirect += drawcalls;
}

void DepthFinderCore::OnDraw(uint64_t context, uint64_t vertices, uint32_t instances)
{
    if (!_active.load(std::memory_order_relaxed))
        return;

    _counters.draws.fetch_add(1, std::memory_order_relaxed);

    const auto lock = LockForDraw();

    const auto found = _contexts.find(context);

    if (found == _contexts.end() || found->second.stats == nullptr)
        return;

    auto& state = found->second;
    const uint64_t count = vertices * instances;

    AddDraw(state.stats->total, count, 1, false);
    AddDraw(state.stats->current, count, 1, false);

    // A fullscreen rectangle (two triangles) does not update the viewport the last real draw used.
    if (!(vertices == 6 && instances == 1))
        state.stats->current.lastViewportWidth = state.viewportWidth;
}

void DepthFinderCore::OnIndirect(uint64_t context, uint32_t maxCount)
{
    if (!_active.load(std::memory_order_relaxed))
        return;

    _counters.indirect.fetch_add(1, std::memory_order_relaxed);

    const auto lock = LockForDraw();

    const auto found = _contexts.find(context);

    if (found != _contexts.end() && found->second.stats != nullptr)
    {
        auto* stats = found->second.stats;
        AddDraw(stats->total, 0, maxCount, true);
        AddDraw(stats->current, 0, maxCount, true);
        stats->current.lastViewportWidth = found->second.viewportWidth;
    }
}

void DepthFinderCore::OnViewport(uint64_t context, float width)
{
    // Only the main viewport matters, as in ReShade's add-on.
    if (!_active.load(std::memory_order_relaxed))
        return;

    const auto lock = LockForDraw();
    _contexts[context].viewportWidth = width;
}

SnapshotRequest DepthFinderCore::OnDepthBound(uint64_t context, bool hadDepth, const DepthBuffer* bound)
{
    SnapshotRequest request;

    if (!_active.load(std::memory_order_relaxed))
        return request;

    std::lock_guard lock(_mutex);

    Stats* boundStats = nullptr;

    if (bound != nullptr && bound->id != 0)
    {
        auto& stats = _stats[bound->id];
        stats.width = bound->width;
        stats.height = bound->height;
        stats.format = bound->format;
        stats.resource = bound->id;
        stats.readOnlyDepth = bound->readOnlyDepth;
        boundStats = &stats;
    }

    // The picked buffer is often never cleared again in the frame it was drawn (Witcher 3 clears at the start of the pass), so
    // the clear never offers a snapshot. The moment the context moves off it is the other chance: the busiest stretch of the
    // frame is copied there, in the state the view says the buffer is in.
    Stats* previous = _contexts[context].stats;

    if (previous != nullptr && previous != boundStats && _snapshotsWanted.load(std::memory_order_relaxed) &&
        _pick.valid && previous->resource != 0 && _pick.id == previous->resource && previous->current.drawcalls != 0)
    {
        if (previous->current.vertices >= _snapshotFloor)
        {
            request.take = true;
            request.id = previous->resource;
            request.readOnlyDepth = previous->readOnlyDepth;
            request.stretchVertices = previous->current.vertices;
            request.reversed = _pick.reversed;
            request.where = "unbind";
        }

        previous->current = DrawStats {};
    }

    _contexts[context].stats = boundStats;

    _counters.binds.fetch_add(1, std::memory_order_relaxed);

    if (hadDepth)
    {
        _counters.bindsWithDepth.fetch_add(1, std::memory_order_relaxed);

        if (boundStats == nullptr)
            _counters.bindsUnknownDepth.fetch_add(1, std::memory_order_relaxed);
    }

    return request;
}

SnapshotRequest DepthFinderCore::OnDepthClear(uint64_t context, const DepthBuffer& buffer, float value)
{
    (void) context;
    SnapshotRequest request;

    if (!_active.load(std::memory_order_relaxed) || buffer.id == 0)
        return request;

    std::lock_guard lock(_mutex);

    auto& stats = _stats[buffer.id];
    stats.width = buffer.width;
    stats.height = buffer.height;
    stats.format = buffer.format;

    // Reversed-Z games clear to 0.0 (or anything but 1.0). The first clear of a frame decides afresh; any later one in the same
    // frame can only add reversed, as before.
    if (!stats.clearedThisFrame)
        stats.reversed = value != 1.0f;
    else if (value != 1.0f)
        stats.reversed = true;

    stats.clearedThisFrame = true;

    // A clear with no work before it (the start of a frame) means nothing.
    if (stats.current.drawcalls == 0)
        return request;

    const DrawStats stretch = stats.current;
    stats.current = DrawStats {};

    // A clear after a render into a small viewport (a mirror, a portal) is not the scene; ReShade's rule.
    const bool real = stretch.lastViewportWidth > 1024.0f || stretch.lastViewportWidth == 0.0f || _pictureWidth <= 1024.0f;

    if (!real)
        return request;

    // The busiest stretch of the frame is the one to snapshot; ties go to the later one, so a scene drawn first into a shadow
    // map and then for real picks the real one.
    if (stretch.vertices >= _snapshotFloor)
    {
        stats.bestClear = (int32_t) stats.clears;

        // The overlay's copy: only of the buffer picked last frame, and only at the busiest stretch, so the copy that is left
        // at the end of the frame is the scene's.
        if (_snapshotsWanted.load(std::memory_order_relaxed) && _pick.valid && _pick.id == buffer.id)
        {
            request.take = true;
            request.id = buffer.id;
            request.readOnlyDepth = false; // a clear needs depth-write
            request.stretchVertices = stretch.vertices;
            request.reversed = _pick.reversed;
            request.where = "clear";
        }
    }

    ++stats.clears;
    return request;
}

// A context that ends while still bound to the picked buffer never unbinds it, so the stretch drawn since its last clear
// (Cyberpunk draws the world after the clear and ends the list there) is copied here.
SnapshotRequest DepthFinderCore::OnContextEnd(uint64_t context)
{
    SnapshotRequest request;

    if (!_active.load(std::memory_order_relaxed))
        return request;

    std::lock_guard lock(_mutex);

    const auto found = _contexts.find(context);

    if (found == _contexts.end() || found->second.stats == nullptr)
        return request;

    Stats* stats = found->second.stats;

    if (_snapshotsWanted.load(std::memory_order_relaxed) && _pick.valid && stats->resource != 0 &&
        _pick.id == stats->resource && stats->current.drawcalls != 0)
    {
        if (stats->current.vertices >= _snapshotFloor)
        {
            request.take = true;
            request.id = stats->resource;
            request.readOnlyDepth = stats->readOnlyDepth;
            request.stretchVertices = stats->current.vertices;
            request.reversed = _pick.reversed;
            request.where = "close";
        }

        stats->current = DrawStats {};
    }

    found->second.stats = nullptr;
    return request;
}

SnapshotRequest DepthFinderCore::FlushForPresent(uint64_t context)
{
    SnapshotRequest request;

    if (!_active.load(std::memory_order_relaxed))
        return request;

    std::lock_guard lock(_mutex);

    const auto found = _contexts.find(context);

    if (found == _contexts.end() || found->second.stats == nullptr)
        return request;

    Stats* stats = found->second.stats;

    if (_snapshotsWanted.load(std::memory_order_relaxed) && _pick.valid && stats->resource != 0 &&
        _pick.id == stats->resource && stats->current.drawcalls != 0 && stats->current.vertices >= _snapshotFloor)
    {
        request.take = true;
        request.id = stats->resource;
        request.readOnlyDepth = stats->readOnlyDepth;
        request.stretchVertices = stats->current.vertices;
        request.reversed = _pick.reversed;
        request.where = "present";
        stats->current = DrawStats {};
    }

    return request;
}

uint64_t DepthFinderCore::BeginPresent(uint32_t pictureWidth, uint32_t pictureHeight)
{
    (void) pictureHeight;

    std::lock_guard lock(_mutex);

    _pictureWidth = (float) pictureWidth;

    // Lists are recorded in no fixed order, so "the busiest stretch so far" picked a different copy from frame to frame (the
    // world one, or the first-person weapon's) and the preview flickered. Every stretch that draws a fair share of what the
    // pick drew last frame is copied instead; the one that runs last on the GPU is left, the same every frame.
    _snapshotFloor = 0;

    if (_pick.valid)
    {
        const auto picked = _stats.find(_pick.id);

        if (picked != _stats.end())
            _snapshotFloor = picked->second.total.vertices * 2 / 100;
    }

    _frameCandidates.clear();
    _frameCandidates.reserve(_stats.size());

    for (auto it = _stats.begin(); it != _stats.end();)
    {
        auto& stats = it->second;

        if (stats.total.drawcalls == 0 && stats.clears == 0)
        {
            // Not touched this frame: forget it, and any context still pointing at it.
            for (auto& context : _contexts)
                if (context.second.stats == &stats)
                    context.second.stats = nullptr;

            it = _stats.erase(it);
            continue;
        }

        GenericDepthSelect::Candidate c;
        c.id = it->first;
        c.width = stats.width;
        c.height = stats.height;
        c.format = stats.format;
        c.vertices = stats.total.vertices;
        c.drawcalls = stats.total.drawcalls;
        c.drawcallsIndirect = stats.total.drawcallsIndirect;
        c.clears = stats.clears;
        c.bestClear = stats.bestClear;
        c.reversed = stats.reversed;
        _frameCandidates.push_back(c);

        // The next frame starts from nothing; the bound pointer stays valid because the entry stays.
        stats.total = DrawStats {};
        stats.current = DrawStats {};
        stats.clears = 0;
        stats.bestClear = -1;
        stats.clearedThisFrame = false;
        ++it;
    }

    ++_frames;
    return _presents;
}

bool DepthFinderCore::EndPresent(uint32_t pictureWidth, uint32_t pictureHeight, uint32_t warmupFrames)
{
    {
        std::lock_guard lock(_mutex);

        ++_presents;
        _presentsNow.store(_presents, std::memory_order_relaxed);

        const uint64_t last = _lastUpscalerCall.load(std::memory_order_relaxed);
        const bool calling = last != 0 && _presents - last < kQuietPresents;

        if (calling != _upscalerSeen.load())
        {
            _upscalerSeen = calling;

            if (calling)
                Say("Depth finder: the game is calling an upscaler; the finder stands down while it does");
            else
                Say(std::format("Depth finder: no upscaler call for {} presents; the finder is watching again",
                                kQuietPresents));
        }

        if (calling)
        {
            // Stand down: stop counting, drop the tracking and the pick, and start the warm-up over for when it wakes.
            _active = false;
            _armed = false;
            _stats.clear();
            _contexts.clear();
            _pick = GenericDepthSelect::Pick {};
            _selector.Reset();
            _warmupStart = _presents;
            return true;
        }

        // Counting restarts here; the draws of the frame in which it woke are not seen, which is fine.
        _active = true;

        // Counting has gone on through the warm-up, but nothing is picked or reported until it is over.
        if (_presents - _warmupStart <= warmupFrames)
        {
            _armed = false;
            return false;
        }

        _armed = true;
    }

    const auto pick = _selector.Update(_frameCandidates, pictureWidth, pictureHeight);

    const bool pickChanged = (pick.valid ? pick.id : 0) != _lastLoggedPick;
    const bool dueForLog = _frames - _lastLoggedFrame >= kLogEveryFrames || _lastLoggedFrame == 0;

    if (pickChanged || dueForLog)
    {
        LogCandidates(_frameCandidates, pick, pictureWidth, pictureHeight);
        _lastLoggedFrame = _frames;
        _lastLoggedPick = pick.valid ? pick.id : 0;
    }

    std::lock_guard lock(_mutex);
    _pick = pick;
    return false;
}

DepthFinderCore::Diagnostic DepthFinderCore::Diagnose(uint64_t context) const
{
    std::lock_guard lock(_mutex);
    Diagnostic d;
    d.wanted = _snapshotsWanted.load(std::memory_order_relaxed);

    const auto found = _contexts.find(context);

    if (found == _contexts.end())
        return d;

    d.hasContext = true;

    if (found->second.stats == nullptr)
        return d;

    d.hasBoundBuffer = true;
    d.boundResource = found->second.stats->resource;
    d.currentVertices = found->second.stats->current.vertices;
    d.currentDrawcalls = found->second.stats->current.drawcalls;
    return d;
}

GenericDepthSelect::Pick DepthFinderCore::CurrentPick() const
{
    std::lock_guard lock(_mutex);
    return _pick;
}

uint64_t DepthFinderCore::WarmupStart() const
{
    std::lock_guard lock(_mutex);
    return _warmupStart;
}

uint64_t DepthFinderCore::SnapshotFloor() const
{
    std::lock_guard lock(_mutex);
    return _snapshotFloor;
}

void DepthFinderCore::LogCandidates(const std::vector<GenericDepthSelect::Candidate>& frame,
                                    const GenericDepthSelect::Pick& pick, uint32_t pictureWidth, uint32_t pictureHeight)
{
    auto sorted = frame;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return GenericDepthSelect::Score(a) > GenericDepthSelect::Score(b); });

    Say(std::format("Depth finder: frame {}, picture {}x{}, {} depth buffer(s) in use{}", _frames, pictureWidth,
                    pictureHeight, sorted.size(), pick.valid ? "" : ", none qualifies"));

    const int bundleSame = _counters.bundleSameDraw.load();
    Say(std::format("Depth finder:   hooks so far: {} depth views created, {} binds of render targets ({} with a depth "
                    "buffer, {} of those unknown to us), {} draws, {} indirect, {} bundle executions (bundle draw code is "
                    "the direct list's: {}), {} dispatches",
                    _counters.depthViewsCreated.load(), _counters.binds.load(), _counters.bindsWithDepth.load(),
                    _counters.bindsUnknownDepth.load(), _counters.draws.load(), _counters.indirect.load(),
                    _counters.bundles.load(), bundleSame < 0 ? "unknown" : bundleSame ? "yes" : "no",
                    _counters.dispatches.load()));

    const uint64_t draws = (uint64_t) _counters.draws.load();
    Say(std::format(
        "Depth finder:   draws that waited for the finder's lock since the last report: {} of {}, {:.2f} ms in all, "
        "longest {:.1f} us",
        _drawLockWaits.exchange(0), draws - _drawsAtLastReport, _drawLockWaitNs.exchange(0) / 1e6,
        _drawLockWaitMaxNs.exchange(0) / 1e3));
    _drawsAtLastReport = draws;

    const size_t shown = std::min<size_t>(sorted.size(), 8);

    for (size_t i = 0; i < shown; ++i)
    {
        const auto& c = sorted[i];
        Say(std::format("Depth finder:   {}{:X}  {}x{}  format {}  {} vertices, {} draws ({} indirect), {} clear(s), best "
                        "clear {}{}",
                        pick.valid && pick.id == c.id ? "-> " : "   ", c.id, c.width, c.height, c.format, c.vertices,
                        c.drawcalls, c.drawcallsIndirect, c.clears, c.bestClear, c.reversed ? ", reversed-Z" : ""));
    }
}

} // namespace native
