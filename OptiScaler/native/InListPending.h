#pragma once
// The in-list pass's bookkeeping (native/InListUpscale.cpp), with no game or OptiScaler header so that a host test can
// drive it (tests/nr_inlist_pending_smoke.cpp):
// - Session: whether the game's FSR1 upscale is replaced (InList) or the present path runs (PresentPath), moved only at
//   a present, with hysteresis.
// - InListPending: the game's lists holding our work, followed through ExecuteCommandLists to a fence of our own on
//   each queue: what they hold is released once the GPU is done with it, from the present, never by time.
// - ReplayLedger/ReplayDx12: the private states our work leaves our resources in, so that a list executed again
//   without a Reset finds them as its recording did (a fix-up list of barriers just before it, in GPU order).
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace native::inlist
{
enum class Fallback : uint8_t
{
    NotInList,    // the session is not InList (PresentPath, Leaving)
    OneAlready,   // an upscale was replaced already in this present, or this list holds one
    Busy,         // the session lock, the producer's ring or the pending table is taken
    FgTeardown,   // frame generation is tearing its swap chain down (its mutex held by owner 1)
    Rebuild,      // the upscaler has to be built first: the present does it, never the recording thread
    Producer,     // the producer cannot record (not ready, another device, a fifth queue ran our work)
    LateFallback, // our work was recorded but the upscaler did not evaluate: the game's pass ran
    Replay,       // a list holding a replacement was executed again and could not be made safe for it
    RtFormat,     // the upscaler's output cannot be copied to the game's target (family or size)
    Count
};
inline const char* Name(Fallback why)
{
    constexpr const char* names[] { "not-in-list", "one-already",   "busy",   "fg-teardown", "rebuild",
                                    "producer",    "late-fallback", "replay", "rt-format" };
    return why < Fallback::Count ? names[size_t(why)] : "?";
}

enum class Mode : uint8_t
{
    PresentPath, // the present path runs (when it is on); the passes are only counted
    InList,      // the passes are replaced inside the game's lists
    Leaving      // left InList, waiting for NativeProducer::LeaveInList(): nothing replaced, the present path waits
};
inline const char* Name(Mode mode)
{
    constexpr const char* names[] { "present path", "in list", "leaving" };
    return names[size_t(mode) % 3];
}
enum class Exit : uint8_t
{
    None,
    Disabled,
    DeviceChanged,
    GameUpscaler,
    Replay,
    NoEasu,    // kLeaveWithoutEasu presents without the upscale pass (render scale 100 %, another upscaler)
    Fallbacks, // kLeaveAfterFallbacks fallbacks in a row, no replacement in between
    Count
};
inline const char* Name(Exit exit)
{
    constexpr const char* names[] { "none",
                                    "option off",
                                    "device changed",
                                    "game upscaler",
                                    "list executed again",
                                    "no upscale pass",
                                    "fallbacks in a row" };
    return exit < Exit::Count ? names[size_t(exit)] : "?";
}
struct PresentFacts
{
    bool enabled = false;       // [Upscalers] ReplaceGameFsr1 on, on the device the hooks were installed for
    bool gameUpscaler = false;  // native::GameUpscalerCalledRecently()
    bool deviceChanged = false; // the producer's device is not the present's
    bool leaveIdle = false;     // the last present's NativeProducer::LeaveInList() returned true, nothing pending
    bool drained = false;       // nothing recorded into a game's list can still run (InListPending::Drained())
    uint32_t producerBusyInARow = 0;
};
struct Decision
{
    Mode mode = Mode::PresentPath;
    bool enterInList = false;    // entered now (for the log)
    bool leaveInList = false;    // call NativeProducer::LeaveInList() now; its result is the next present's leaveIdle
    bool forgetInList = false;   // call NativeProducer::ForgetInList(): frames in flight that nothing will complete
    bool runPresentPath = false; // the present path may run in this present
    Exit exit = Exit::None;      // InList was left now, and why
};
struct Snapshot
{
    Mode mode = Mode::PresentPath;
    uint64_t presents = 0, presentsInList = 0, replaced = 0, enters = 0, forgets = 0;
    uint64_t fallbacks[size_t(Fallback::Count)] {};
    uint64_t exits[size_t(Exit::Count)] {};
};

// Any thread notes; only the present (OnPresent) moves the mode.
class Session
{
  public:
    static constexpr uint32_t kEnterAfter = 3;            // K: good presents in a row before InList
    static constexpr uint32_t kLeaveWithoutEasu = 60;     // M
    static constexpr uint32_t kLeaveAfterFallbacks = 120; // N; also the presents Leaving before ForgetInList
    constexpr Session() = default;

    Mode GetMode() const { return _mode.load(std::memory_order_acquire); }
    bool InList() const { return GetMode() == Mode::InList; }
    // An EASU pass; a candidate is one the detection let through (fsr1::Resolve), the others count as fallbacks.
    void NoteEasu(bool candidate)
    {
        _seen[kEasu].fetch_add(1, std::memory_order_relaxed);
        _seen[candidate ? kCandidates : kFallbacks].fetch_add(1, std::memory_order_relaxed);
    }
    void NoteFallback(Fallback why)
    {
        if (why >= Fallback::Count)
            return;
        _fallbacks[size_t(why)].fetch_add(1, std::memory_order_relaxed);
        _seen[kFallbacks].fetch_add(1, std::memory_order_relaxed);
        if (why == Fallback::Replay)
            _seen[kReplay].store(1, std::memory_order_relaxed);
    }
    void NoteReplaced()
    {
        _replaced.fetch_add(1, std::memory_order_relaxed);
        _seen[kReplaced].fetch_add(1, std::memory_order_relaxed);
    }
    // One replacement per present: the present's token, or 0 when it was taken.
    uint64_t TryClaimPresent()
    {
        const uint64_t present = _presentIndex.load(std::memory_order_acquire);
        uint64_t claimed = _claimed.load(std::memory_order_relaxed);
        while (claimed != present)
            if (_claimed.compare_exchange_weak(claimed, present, std::memory_order_acq_rel, std::memory_order_relaxed))
                return present;
        return 0;
    }
    void UnclaimPresent(uint64_t token)
    {
        if (token != 0)
            _claimed.compare_exchange_strong(token, 0, std::memory_order_acq_rel, std::memory_order_relaxed);
    }
    Decision OnPresent(const PresentFacts& facts)
    {
        uint64_t seen[kSeen];
        for (int i = 0; i < kSeen; ++i)
            seen[i] = _seen[i].exchange(0, std::memory_order_relaxed);
        Mode mode = _mode.load(std::memory_order_relaxed);
        Decision decision;
        _presents.fetch_add(1, std::memory_order_relaxed);
        if (mode == Mode::InList)
            _presentsInList.fetch_add(1, std::memory_order_relaxed);
        if (mode == Mode::PresentPath)
        {
            const bool good = facts.enabled && !facts.gameUpscaler && !facts.deviceChanged && seen[kEasu] != 0 &&
                              seen[kCandidates] != 0;
            _goodInARow = good ? _goodInARow + 1 : 0;
            if (_goodInARow >= kEnterAfter)
            {
                mode = Mode::InList;
                _goodInARow = _noEasuInARow = _fallbacksInARow = 0;
                _enters.fetch_add(1, std::memory_order_relaxed);
                decision.enterInList = true;
            }
        }
        else if (mode == Mode::InList)
        {
            Exit exit = !facts.enabled        ? Exit::Disabled
                        : facts.deviceChanged ? Exit::DeviceChanged
                        : facts.gameUpscaler  ? Exit::GameUpscaler
                        : seen[kReplay] != 0  ? Exit::Replay
                                              : Exit::None;
            if (exit == Exit::None)
            {
                _noEasuInARow = seen[kEasu] != 0 ? 0 : _noEasuInARow + 1;
                _fallbacksInARow = seen[kReplaced] != 0 ? 0 : _fallbacksInARow + uint32_t(seen[kFallbacks]);
                exit = _noEasuInARow >= kLeaveWithoutEasu         ? Exit::NoEasu
                       : _fallbacksInARow >= kLeaveAfterFallbacks ? Exit::Fallbacks
                                                                  : Exit::None;
            }
            if (exit != Exit::None)
            {
                mode = Mode::Leaving;
                _leavingPresents = 0;
                _forgetAsked = false;
                _exits[size_t(exit)].fetch_add(1, std::memory_order_relaxed);
                decision.exit = exit;
            }
        }
        else if (_leaveAsked && facts.leaveIdle)
        {
            mode = Mode::PresentPath;
            _goodInARow = 0;
        }
        else
        {
            ++_leavingPresents;
            if (!_forgetAsked && facts.drained &&
                (facts.producerBusyInARow >= kLeaveAfterFallbacks || _leavingPresents >= kLeaveAfterFallbacks))
            {
                _forgetAsked = true;
                _forgets.fetch_add(1, std::memory_order_relaxed);
                decision.forgetInList = true;
            }
        }
        _mode.store(mode, std::memory_order_release);
        _presentIndex.fetch_add(1, std::memory_order_acq_rel);
        decision.mode = mode;
        decision.leaveInList = _leaveAsked = mode == Mode::Leaving;
        decision.runPresentPath = mode == Mode::PresentPath;
        return decision;
    }
    Snapshot Take() const
    {
        Snapshot s;
        s.mode = GetMode();
        s.presents = _presents.load(std::memory_order_relaxed);
        s.presentsInList = _presentsInList.load(std::memory_order_relaxed);
        s.replaced = _replaced.load(std::memory_order_relaxed);
        s.enters = _enters.load(std::memory_order_relaxed);
        s.forgets = _forgets.load(std::memory_order_relaxed);
        for (size_t i = 0; i < size_t(Fallback::Count); ++i)
            s.fallbacks[i] = _fallbacks[i].load(std::memory_order_relaxed);
        for (size_t i = 0; i < size_t(Exit::Count); ++i)
            s.exits[i] = _exits[i].load(std::memory_order_relaxed);
        return s;
    }

  private:
    enum : int
    {
        kEasu,
        kCandidates,
        kFallbacks,
        kReplaced,
        kReplay,
        kSeen
    };
    std::atomic<Mode> _mode { Mode::PresentPath };
    std::atomic<uint64_t> _seen[kSeen] {};
    std::atomic<uint64_t> _presentIndex { 1 }, _claimed { 0 };
    std::atomic<uint64_t> _fallbacks[size_t(Fallback::Count)] {}, _exits[size_t(Exit::Count)] {};
    std::atomic<uint64_t> _replaced { 0 }, _presents { 0 }, _presentsInList { 0 }, _enters { 0 }, _forgets { 0 };
    uint32_t _goodInARow = 0, _noEasuInARow = 0, _fallbacksInARow = 0, _leavingPresents = 0; // the present's
    bool _leaveAsked = false, _forgetAsked = false;
};

struct PendingRef
{
    void* object = nullptr;
    void (*release)(void* object) = nullptr;
};
enum class ReserveResult : uint8_t
{
    Ok,
    OneAlready, // the list holds one already (a Reset retires it)
    Full,       // no free entry
    Overflow    // a fifth queue executed our work: nothing more is replaced
};
struct Executed
{
    int queueSlot = -1; // the queue whose fence was signalled after this submission, and the value
    uint64_t fenceValue = 0;
    uint32_t first = 0, replays = 0; // lists executed the first time (tickets completed), and again without a Reset
    uint32_t untracked = 0;          // executed where no value could be signalled: kept for good
};
struct PendingStats
{
    uint64_t committed = 0, cancelled = 0, completed = 0, replays = 0, abandoned = 0, retired = 0, retiredAfter = 0,
             untracked = 0;
    int listed = 0, retiring = 0, after = 0, queues = 0;
    bool overflow = false;
};

#pragma warning(push)
#pragma warning(disable : 4324) // padded on purpose: each entry and each queue starts a cache line
// Api: Queue, Fence, Ticket; CreateFence, Signal, Completed, Release (fences); Complete, Abandon (tickets, a default
// one included). Reserve/Commit/Cancel: the recording thread, under the session. AfterExecute, OnResetOrRelease: any
// thread, lock-free but for one queue's SRW lock around its Signal. Drain, Drained, ReleaseAllIdle: the present.
template <class Api> class InListPending
{
  public:
    using Queue = typename Api::Queue;
    using Fence = typename Api::Fence;
    using Ticket = typename Api::Ticket;
    static constexpr int kQueues = 4, kLists = 64, kRefs = 4, kAfter = 64;
    constexpr InListPending() = default;

    // The entry for our work in `list`'s recording, or -1 and why not.
    int Reserve(const void* list, ReserveResult* why)
    {
        *why = list == nullptr || list == Claiming() || list == RetiringKey() ? ReserveResult::Full
               : _overflow.load(std::memory_order_relaxed)                    ? ReserveResult::Overflow
               : FindLive(list) >= 0                                          ? ReserveResult::OneAlready
                                                                              : ReserveResult::Ok;
        for (int i = 0; *why == ReserveResult::Ok && i < kLists; ++i)
        {
            Entry& e = _entries[i];
            const void* expected = nullptr;
            if (!e.key.compare_exchange_strong(expected, Claiming(), std::memory_order_acq_rel))
                continue;
            e.seq = _nextSeq.fetch_add(1, std::memory_order_relaxed) + 1;
            _listed.fetch_add(1, std::memory_order_relaxed);
            e.key.store(list, std::memory_order_release);
            return i;
        }
        if (*why == ReserveResult::Ok)
            *why = ReserveResult::Full;
        return -1;
    }
    // What the recording holds of ours: released once every queue it ran on is past it.
    void Commit(int index, const Ticket& ticket, const PendingRef* refs, int count)
    {
        Entry& e = _entries[index];
        e.ticket = ticket;
        e.refCount = count < kRefs ? count : kRefs;
        for (int i = 0; i < e.refCount; ++i)
            e.refs[i] = refs[i];
        _committed.fetch_add(1, std::memory_order_relaxed);
    }
    void Cancel(int index)
    {
        _cancelled.fetch_add(1, std::memory_order_relaxed);
        _listed.fetch_sub(1, std::memory_order_relaxed);
        ReleaseRefs(_entries[index]);
        Free(_entries[index]);
    }
    // After the original ExecuteCommandLists: one value signalled on the queue's fence covers the lists holding ours.
    template <class ListPtr> Executed AfterExecute(Queue* queue, uint32_t count, ListPtr const* lists)
    {
        Executed result;
        uint64_t value = 0;
        const int slot = SlotOf(queue);
        Fence* fence = slot >= 0 ? SignalOn(slot, &value) : nullptr;
        result.queueSlot = fence != nullptr ? slot : -1;
        result.fenceValue = fence != nullptr ? value : 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const int index = FindLive(lists[i]);
            if (index < 0)
                continue;
            Entry& e = _entries[index];
            if (++e.executions, fence == nullptr)
            {
                e.kept = true;
                ++result.untracked;
                continue;
            }
            if (e.executions == 1)
                Api::Complete(e.ticket, fence, value);
            ++(e.executions == 1 ? result.first : result.replays);
            e.values[slot] = value;
        }
        _completed.fetch_add(result.first, std::memory_order_relaxed);
        _replays.fetch_add(result.replays, std::memory_order_relaxed);
        _untracked.fetch_add(result.untracked, std::memory_order_relaxed);
        return result;
    }
    template <class ListPtr> bool HoldsAny(uint32_t count, ListPtr const* lists) const
    {
        for (uint32_t i = 0; lists != nullptr && _listed.load(std::memory_order_acquire) != 0 && i < count; ++i)
            if (FindLive(lists[i]) >= 0)
                return true;
        return false;
    }
    int EntryOf(const void* list) const { return FindLive(list); }
    int QueueSlotOf(Queue* queue) { return SlotOf(queue); }
    Fence* QueueFence(int slot) const
    {
        return slot >= 0 ? _queues[slot].fence.load(std::memory_order_acquire) : nullptr;
    }
    // A list's Reset or last Release. True: its recording held ours and never ran (its ticket is abandoned).
    bool OnResetOrRelease(const void* list)
    {
        const int index = _listed.load(std::memory_order_acquire) != 0 ? FindLive(list) : -1;
        if (index < 0)
            return false;
        Entry& e = _entries[index];
        _listed.fetch_sub(1, std::memory_order_relaxed);
        if (e.executions != 0)
        {
            _retiring.fetch_add(1, std::memory_order_relaxed);
            e.key.store(RetiringKey(), std::memory_order_release);
            return false;
        }
        Api::Abandon(e.ticket);
        _abandoned.fetch_add(1, std::memory_order_relaxed);
        ReleaseRefs(e);
        Free(e);
        return true;
    }
    // Ours that recordings made so far may use (the upscaler's let-go objects): released once those lists retired.
    bool RetireAfterRecorded(const PendingRef& ref)
    {
        for (AfterItem& item : _after)
        {
            uint32_t expected = kFree;
            if (!item.state.compare_exchange_strong(expected, kWriting, std::memory_order_acq_rel))
                continue;
            item.ref = ref;
            item.afterSeq = _nextSeq.load(std::memory_order_acquire);
            item.state.store(kReady, std::memory_order_release);
            return true;
        }
        return false;
    }
    void Drain()
    {
        uint64_t completed[kQueues];
        CompletedValues(completed);
        uint64_t oldest = UINT64_MAX; // the oldest recording a list may still hold
        for (Entry& e : _entries)
        {
            const void* key = e.key.load(std::memory_order_acquire);
            if (key == RetiringKey() && !e.kept && Reached(e, completed))
            {
                ReleaseRefs(e);
                Free(e);
                _retiring.fetch_sub(1, std::memory_order_relaxed);
                _retired.fetch_add(1, std::memory_order_relaxed);
            }
            else if (key != nullptr)
                oldest = key == Claiming() ? 0 : e.seq < oldest ? e.seq : oldest;
        }
        for (AfterItem& item : _after)
            if (item.state.load(std::memory_order_acquire) == kReady && item.afterSeq < oldest)
            {
                item.ref.release(item.ref.object);
                item.state.store(kFree, std::memory_order_release);
                _retiredAfter.fetch_add(1, std::memory_order_relaxed);
            }
    }
    // Nothing recorded into a game's list can run any more (nor be executed again).
    bool Drained() const
    {
        uint64_t completed[kQueues];
        CompletedValues(completed);
        for (const Entry& e : _entries)
        {
            const void* key = e.key.load(std::memory_order_acquire);
            if (key != nullptr && (key != RetiringKey() || e.kept || !Reached(e, completed)))
                return false;
        }
        return true;
    }
    bool QueuesFull() const { return _overflow.load(std::memory_order_relaxed); }
    bool TakeQueueOverflowLog()
    {
        return _overflow.load(std::memory_order_relaxed) && !_overflowLogged.exchange(true, std::memory_order_relaxed);
    }
    PendingStats Stats() const
    {
        PendingStats s { _committed.load(), _cancelled.load(), _completed.load(),    _replays.load(),
                         _abandoned.load(), _retired.load(),   _retiredAfter.load(), _untracked.load() };
        s.listed = _listed.load(std::memory_order_relaxed);
        s.retiring = _retiring.load(std::memory_order_relaxed);
        s.overflow = _overflow.load(std::memory_order_relaxed);
        for (const AfterItem& item : _after)
            s.after += item.state.load(std::memory_order_relaxed) == kReady ? 1 : 0;
        for (const QueueSlot& q : _queues)
            s.queues += q.fence.load(std::memory_order_relaxed) != nullptr ? 1 : 0;
        return s;
    }
    // Only once nothing of ours can run (another device, after Drained).
    void ReleaseAllIdle()
    {
        for (Entry& e : _entries)
            if (e.key.load(std::memory_order_acquire) != nullptr)
            {
                ReleaseRefs(e);
                Free(e);
            }
        for (AfterItem& item : _after)
            if (item.state.load(std::memory_order_acquire) == kReady)
            {
                item.ref.release(item.ref.object);
                item.state.store(kFree, std::memory_order_release);
            }
        for (QueueSlot& q : _queues)
        {
            if (Fence* fence = q.fence.exchange(nullptr, std::memory_order_acq_rel))
                Api::Release(fence);
            q.value = 0;
            q.queue.store(nullptr, std::memory_order_release);
        }
        _listed = _retiring = 0;
        _overflow = _overflowLogged = false;
    }

  private:
    static const void* Claiming() { return reinterpret_cast<const void*>(uintptr_t { 1 }); }
    static const void* RetiringKey() { return reinterpret_cast<const void*>(uintptr_t { 2 }); }
    static constexpr uint32_t kFree = 0, kWriting = 1, kReady = 2;
    struct alignas(64) Entry
    {
        std::atomic<const void*> key { nullptr };
        uint64_t seq = 0; // the order of the reservations (RetireAfterRecorded)
        uint32_t executions = 0;
        bool kept = false; // ran where it could not be followed: never released before ReleaseAllIdle
        int refCount = 0;
        Ticket ticket {};
        PendingRef refs[kRefs] {};
        uint64_t values[kQueues] {}; // the last value signalled after it on each queue (0: never executed there)
    };
    struct alignas(64) QueueSlot
    {
        std::atomic<Queue*> queue { nullptr };
        std::atomic<Fence*> fence { nullptr };
        SRWLOCK lock = SRWLOCK_INIT;
        uint64_t value = 0; // the last value signalled, under lock
    };
    struct AfterItem
    {
        std::atomic<uint32_t> state { kFree };
        uint64_t afterSeq = 0;
        PendingRef ref {};
    };
    int FindLive(const void* list) const
    {
        for (int i = 0; list != nullptr && i < kLists; ++i)
            if (_entries[i].key.load(std::memory_order_acquire) == list)
                return i;
        return -1;
    }
    static void ReleaseRefs(Entry& e)
    {
        for (int i = 0; i < e.refCount; ++i)
            e.refs[i].release(e.refs[i].object);
        e.refCount = 0;
    }
    static void Free(Entry& e)
    {
        e.executions = 0;
        e.kept = false;
        e.ticket = Ticket {};
        for (uint64_t& value : e.values)
            value = 0;
        e.key.store(nullptr, std::memory_order_release);
    }
    static bool Reached(const Entry& e, const uint64_t* completed)
    {
        for (int q = 0; q < kQueues; ++q)
            if (completed[q] < e.values[q])
                return false;
        return true;
    }
    void CompletedValues(uint64_t* completed) const
    {
        for (int q = 0; q < kQueues; ++q)
        {
            Fence* fence = _queues[q].fence.load(std::memory_order_acquire);
            completed[q] = fence != nullptr ? Api::Completed(fence) : 0;
        }
    }
    int SlotOf(Queue* queue)
    {
        for (int i = 0; queue != nullptr && i < kQueues; ++i)
        {
            Queue* expected = nullptr;
            if (_queues[i].queue.load(std::memory_order_acquire) == queue ||
                _queues[i].queue.compare_exchange_strong(expected, queue, std::memory_order_acq_rel) ||
                expected == queue)
                return i;
        }
        _overflow.store(queue != nullptr, std::memory_order_relaxed);
        return -1;
    }
    Fence* SignalOn(int slot, uint64_t* value)
    {
        QueueSlot& q = _queues[slot];
        Queue* queue = q.queue.load(std::memory_order_relaxed);
        AcquireSRWLockExclusive(&q.lock);
        Fence* fence = q.fence.load(std::memory_order_relaxed);
        if (fence == nullptr && Api::CreateFence(queue, &fence))
            q.fence.store(fence, std::memory_order_release);
        const bool signalled = fence != nullptr && Api::Signal(queue, fence, *value = q.value + 1);
        q.value += signalled ? 1 : 0;
        ReleaseSRWLockExclusive(&q.lock);
        return signalled ? fence : nullptr;
    }
    Entry _entries[kLists] {};
    QueueSlot _queues[kQueues] {};
    AfterItem _after[kAfter] {};
    alignas(64) std::atomic<int> _listed { 0 };
    std::atomic<int> _retiring { 0 };
    std::atomic<uint64_t> _nextSeq { 0 };
    std::atomic<bool> _overflow { false }, _overflowLogged { false };
    std::atomic<uint64_t> _committed { 0 }, _cancelled { 0 }, _completed { 0 }, _replays { 0 }, _abandoned { 0 },
        _retired { 0 }, _retiredAfter { 0 }, _untracked { 0 };
};
#pragma warning(pop)

// The transitions one recording made on our resources, per subresource: the state it found and the one it left.
template <int Capacity> struct ReplayLedger
{
    struct Entry
    {
        void* resource = nullptr;
        int owner = -1;
        uint32_t subresource = 0, before = 0, after = 0;
    };
    Entry entries[Capacity] {};
    int count = 0;
    bool safe = true;
    // The new entry's index, -1 when it extends one, -2 when the recording cannot be replayed (unsafe).
    int Record(void* resource, int owner, uint32_t subresource, uint32_t before, uint32_t after, bool split = false)
    {
        if (split && before != after)
        {
            safe = false;
            return -2;
        }
        for (int i = 0; i < count; ++i)
            if (entries[i].resource == resource && entries[i].subresource == subresource)
            {
                safe = safe && entries[i].after == before;
                entries[i].after = after;
                return -1;
            }
        if (count == Capacity)
        {
            safe = false;
            return -2;
        }
        entries[count] = { resource, owner, subresource, before, after };
        return count++;
    }
};
constexpr int kLedger = 256, kOwned = 512, kSubresources = 64;
using Ledger = ReplayLedger<kLedger>;

// Our resources (made on the present thread between BeginResources and EndResources) and the state each subresource
// is in after the submissions so far. Created/Released: hooks, any thread; Barriers: the recording thread between
// BeginRecord and EndRecord (under the session); BeginSubmission/Plan/Accept/Commit: one submission at a time.
class ReplayDx12
{
  public:
    constexpr ReplayDx12() = default;
    void BeginResources() { _resourceThread.store(GetCurrentThreadId(), std::memory_order_release); }
    void EndResources() { _resourceThread.store(0, std::memory_order_release); }
    void Created(ID3D12Resource* resource, D3D12_RESOURCE_STATES initial, D3D12_HEAP_TYPE heap)
    {
        if (resource == nullptr || _resourceThread.load(std::memory_order_acquire) != GetCurrentThreadId() ||
            heap == D3D12_HEAP_TYPE_UPLOAD || heap == D3D12_HEAP_TYPE_READBACK || Find(resource) >= 0)
            return;
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        UINT planes = 1;
        ID3D12Device* device = nullptr;
        D3D12_FEATURE_DATA_FORMAT_INFO info { desc.Format, 0 };
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&device))))
        {
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))))
                planes = info.PlaneCount;
            device->Release();
        }
        const UINT subresources =
            desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                ? 1
                : desc.MipLevels * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize) *
                      planes;
        for (Owned& owned : _owned)
        {
            void* empty = nullptr;
            if (!owned.resource.compare_exchange_strong(empty, reinterpret_cast<void*>(uintptr_t(1)),
                                                        std::memory_order_acq_rel))
                continue;
            owned.serial = ++_serial;
            const int used = int(&owned - _owned) + 1;
            for (int high = _high.load(); high < used && !_high.compare_exchange_weak(high, used);)
            {
            }
            owned.subresources = subresources;
            // Buffers and simultaneous-access textures decay to COMMON at the end of each submission.
            owned.decay = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
                          (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
            owned.supported = subresources > 0 && subresources <= kSubresources;
            owned.known = true;
            owned.queue = nullptr;
            owned.fence = nullptr;
            owned.value = 0;
            for (auto& state : owned.state)
                state = initial;
            (owned.supported ? _states : _unsupported).fetch_add(owned.supported ? int(subresources) : 1);
            _live.fetch_add(1, std::memory_order_relaxed);
            owned.resource.store(resource, std::memory_order_release);
            return;
        }
        _unsupported.fetch_add(1, std::memory_order_relaxed);
    }
    // Before the original Release of any object; Released after it, when that was the last reference.
    uint64_t ReleaseToken(IUnknown* object) const
    {
        const int index = _live.load(std::memory_order_relaxed) != 0 ? Find(object) : -1;
        return index < 0 ? 0 : (_owned[index].serial << 10) | uint64_t(index + 1);
    }
    void Released(uint64_t token)
    {
        if (token == 0)
            return;
        Owned& owned = _owned[(token & 1023) - 1];
        if (owned.serial != (token >> 10))
            return;
        (owned.supported ? _states : _unsupported).fetch_sub(owned.supported ? int(owned.subresources) : 1);
        if (owned.fence != nullptr)
            owned.fence->Release();
        owned.fence = nullptr;
        _live.fetch_sub(1, std::memory_order_relaxed);
        owned.resource.store(nullptr, std::memory_order_release);
    }
    bool CapacityAvailable() const
    {
        return _unsupported.load(std::memory_order_relaxed) == 0 &&
               _states.load(std::memory_order_relaxed) <= kLedger - kSubresources;
    }
    // The game's input, target and the depth copy are not ours: their transitions are the game's to replay.
    void BeginRecord(Ledger& ledger, ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* target,
                     ID3D12Resource* depth)
    {
        _record = &ledger;
        _list = list;
        _excluded[0] = input;
        _excluded[1] = target;
        _excluded[2] = depth;
        _recordThread.store(GetCurrentThreadId(), std::memory_order_release);
    }
    void EndRecord()
    {
        _recordThread.store(0, std::memory_order_release);
        _record = nullptr;
        _list = nullptr;
    }
    void Barriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
    {
        if (_recordThread.load(std::memory_order_acquire) != GetCurrentThreadId() || list != _list || !barriers)
            return;
        for (UINT i = 0; i < count; ++i)
        {
            if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                continue;
            const auto& t = barriers[i].Transition;
            if (t.pResource == _excluded[0] || t.pResource == _excluded[1] || t.pResource == _excluded[2])
                continue;
            const int index = Find(t.pResource);
            const bool all = t.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            if (index < 0 || !_owned[index].supported || (!all && t.Subresource >= _owned[index].subresources))
            {
                _record->safe = false; // untracked resources never receive speculative fix-ups
                continue;
            }
            for (UINT sub = all ? 0 : t.Subresource; sub < (all ? _owned[index].subresources : t.Subresource + 1);
                 ++sub)
                if (_record->Record(t.pResource, index, sub, t.StateBefore, t.StateAfter,
                                    barriers[i].Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) >= 0)
                    t.pResource->AddRef();
        }
    }
    static void Clear(void* object)
    {
        Ledger& ledger = *static_cast<Ledger*>(object);
        for (int i = 0; i < ledger.count; ++i)
            static_cast<ID3D12Resource*>(ledger.entries[i].resource)->Release();
        ledger.count = 0;
        ledger.safe = true;
    }
    void BeginSubmission() { std::memset(_used, 0, sizeof(_used)); }
    // The barriers that bring our resources from where the submissions so far leave them to where the ledger's
    // recording found them; false: the list cannot be replayed safely.
    bool Plan(const Ledger& ledger, ID3D12CommandQueue* queue, D3D12_RESOURCE_BARRIER* barriers, UINT* count)
    {
        *count = 0;
        if (!ledger.safe)
            return false;
        for (int i = 0; i < ledger.count; ++i)
        {
            const Ledger::Entry& e = ledger.entries[i];
            Owned& owned = _owned[e.owner];
            if (!owned.known || owned.resource.load(std::memory_order_acquire) != e.resource)
                return false;
            if (owned.queue != nullptr && owned.queue != queue && owned.fence != nullptr &&
                owned.value > owned.fence->GetCompletedValue() && FAILED(queue->Wait(owned.fence, owned.value)))
                return false;
            Touch(e.owner);
            const auto before = _planned[e.owner][e.subresource];
            if (before == D3D12_RESOURCE_STATES(e.before))
                continue;
            auto& barrier = barriers[(*count)++];
            barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = { static_cast<ID3D12Resource*>(e.resource), e.subresource, before,
                                   D3D12_RESOURCE_STATES(e.before) };
        }
        return true;
    }
    void Accept(const Ledger& ledger)
    {
        for (int i = 0; i < ledger.count; ++i)
        {
            Touch(ledger.entries[i].owner);
            _planned[ledger.entries[i].owner][ledger.entries[i].subresource] =
                D3D12_RESOURCE_STATES(ledger.entries[i].after);
        }
    }
    void Commit(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value)
    {
        for (int i = 0; i < kOwned; ++i)
        {
            if (!_used[i])
                continue;
            Owned& owned = _owned[i];
            for (UINT sub = 0; sub < owned.subresources && sub < kSubresources; ++sub)
                owned.state[sub] = owned.decay ? D3D12_RESOURCE_STATE_COMMON : _planned[i][sub];
            if (fence != nullptr)
                fence->AddRef();
            if (owned.fence != nullptr)
                owned.fence->Release();
            owned.fence = fence;
            owned.value = value;
            owned.queue = queue;
            owned.known = fence != nullptr;
        }
    }
    void Prepared(const Ledger& ledger, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value)
    {
        BeginSubmission();
        Accept(ledger);
        Commit(queue, fence, value);
    }

  private:
    struct Owned
    {
        std::atomic<void*> resource { nullptr };
        uint64_t serial = 0;
        UINT subresources = 0;
        bool decay = false, supported = true, known = true;
        D3D12_RESOURCE_STATES state[kSubresources] {};
        ID3D12CommandQueue* queue = nullptr; // identity, not dereferenced
        ID3D12Fence* fence = nullptr;
        uint64_t value = 0;
    };
    int Find(const void* resource) const
    {
        for (int i = 0, high = _high.load(std::memory_order_acquire); i < high; ++i)
            if (_owned[i].resource.load(std::memory_order_acquire) == resource)
                return i;
        return -1;
    }
    void Touch(int index)
    {
        if (!_used[index])
            std::memcpy(_planned[index], _owned[index].state, sizeof(_planned[index]));
        _used[index] = true;
    }
    Owned _owned[kOwned] {};
    std::atomic<uint64_t> _serial { 0 };
    std::atomic<int> _states { 0 }, _unsupported { 0 }, _live { 0 }, _high { 0 }; // _high: slots ever used
    std::atomic<DWORD> _resourceThread { 0 }, _recordThread { 0 };
    Ledger* _record = nullptr;
    ID3D12GraphicsCommandList* _list = nullptr;
    ID3D12Resource* _excluded[3] {};
    bool _used[kOwned] {};
    D3D12_RESOURCE_STATES _planned[kOwned][kSubresources] {};
};
} // namespace native::inlist
