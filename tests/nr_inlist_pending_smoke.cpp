// Host check of native/InListPending.h: the session's hysteresis (enter after K good presents, leave after M without
// the pass or N fallbacks in a row, step aside for the game's upscaler, a device change or a replay), one replacement
// per present, the table of lists holding our work (per-queue fences, 64 entries, executed again, reset or released
// before or after the GPU, a fifth queue, signals that fail), objects let go by the upscaler, the replay ledger, and
// many recording threads against one present thread.
//
//   vcvars64, then from the repo root:
//   cl /nologo /std:c++20 /EHsc /W4 /WX tests\nr_inlist_pending_smoke.cpp
//   nr_inlist_pending_smoke.exe
// Exit 0 when every check passes, 1 otherwise.
#include "../OptiScaler/native/InListPending.h"

#include <cstdio>
#include <thread>
#include <vector>

using namespace native::inlist;

namespace
{
int g_failures = 0, g_checks = 0;
void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// The fake GPU: a queue signals its fence, the test moves the fence's completed value.
struct FakeQueue
{
    bool signals = true;
};
struct FakeFence
{
    std::atomic<uint64_t> completed { 0 };
};
struct FakeTicket
{
    int id = 0;
};
std::atomic<int> g_completedTickets { 0 }, g_abandonedTickets { 0 }, g_fences { 0 }, g_released { 0 };
struct Api
{
    using Queue = FakeQueue;
    using Fence = FakeFence;
    using Ticket = FakeTicket;
    static bool CreateFence(Queue*, Fence** fence)
    {
        *fence = new FakeFence;
        ++g_fences;
        return true;
    }
    static bool Signal(Queue* queue, Fence*, uint64_t) { return queue->signals; }
    static uint64_t Completed(Fence* fence) { return fence->completed.load(); }
    static void Release(Fence* fence)
    {
        --g_fences;
        delete fence;
    }
    static void Complete(const Ticket& ticket, Fence*, uint64_t)
    {
        if (ticket.id != 0)
            ++g_completedTickets;
    }
    static void Abandon(const Ticket& ticket)
    {
        if (ticket.id != 0)
            ++g_abandonedTickets;
    }
};
using Pending = InListPending<Api>;
void Count(void* counter) { ++*static_cast<std::atomic<int>*>(counter); }
void ReleaseOne(void*) { ++g_released; }

// Everything the queue ran so far is done.
void Finish(Pending& pending, FakeQueue& queue)
{
    if (auto* fence = pending.QueueFence(pending.QueueSlotOf(&queue)))
        fence->completed = UINT64_MAX / 2;
}

PresentFacts Facts(bool enabled = true)
{
    PresentFacts f;
    f.enabled = enabled;
    f.drained = true;
    return f;
}
Decision GoodPresent(Session& s, PresentFacts f = Facts())
{
    s.NoteEasu(true);
    return s.OnPresent(f);
}
void EnterInList(Session& s)
{
    for (uint32_t i = 0; i < Session::kEnterAfter; ++i)
        GoodPresent(s);
}

void SessionEnters()
{
    Session s;
    Check(s.GetMode() == Mode::PresentPath && !s.InList(), "session: starts on the present path");
    Decision d = GoodPresent(s);
    Check(d.runPresentPath && !d.enterInList, "session: the present path runs while it is decided");
    GoodPresent(s);
    s.NoteEasu(false); // a refused pass alone is no candidate
    d = s.OnPresent(Facts());
    Check(!d.enterInList && !s.InList(), "session: a present without a candidate starts the count again");
    for (uint32_t i = 0; i + 1 < Session::kEnterAfter; ++i)
        GoodPresent(s);
    Check(!s.InList(), "session: K-1 good presents are not enough");
    d = GoodPresent(s);
    Check(d.enterInList && s.InList() && d.mode == Mode::InList && !d.runPresentPath,
          "session: in the game's lists after K good presents, the present path stops");
    Check(s.Take().enters == 1, "session: one entry counted");

    const PresentFacts blockers[] { Facts(false),
                                    []
                                    {
                                        auto f = Facts();
                                        f.gameUpscaler = true;
                                        return f;
                                    }(),
                                    []
                                    {
                                        auto f = Facts();
                                        f.deviceChanged = true;
                                        return f;
                                    }() };
    for (const PresentFacts& f : blockers)
    {
        Session other;
        for (int i = 0; i < 10; ++i)
            GoodPresent(other, f);
        Check(!other.InList(), "session: never entered while off, under a game's upscaler or on a device change");
    }
}

void SessionClaims()
{
    Session s;
    EnterInList(s);
    const uint64_t token = s.TryClaimPresent();
    Check(token != 0 && s.TryClaimPresent() == 0, "claim: one replacement per present");
    s.UnclaimPresent(token);
    const uint64_t again = s.TryClaimPresent();
    Check(again == token, "claim: given back by a fallback, the same present can be claimed again");
    GoodPresent(s);
    const uint64_t next = s.TryClaimPresent();
    Check(next == token + 1, "claim: the next present's token follows (the backend's history continues)");
    s.UnclaimPresent(0);
    Check(s.TryClaimPresent() == 0, "claim: a zero token gives nothing back");
}

void SessionLeaves()
{
    struct Case
    {
        const char* what;
        Exit exit;
    };
    for (const Case& c : { Case { "disabled", Exit::Disabled }, Case { "device", Exit::DeviceChanged },
                           Case { "game upscaler", Exit::GameUpscaler }, Case { "replay", Exit::Replay } })
    {
        Session s;
        EnterInList(s);
        PresentFacts f = Facts(c.exit != Exit::Disabled);
        f.deviceChanged = c.exit == Exit::DeviceChanged;
        f.gameUpscaler = c.exit == Exit::GameUpscaler;
        if (c.exit == Exit::Replay)
            s.NoteFallback(Fallback::Replay);
        s.NoteEasu(true);
        const Decision d = s.OnPresent(f);
        Check(d.exit == c.exit && d.mode == Mode::Leaving && d.leaveInList && !d.runPresentPath, c.what);
        Check(s.Take().exits[size_t(c.exit)] == 1, "leave: the exit is counted by reason");
    }

    Session s;
    EnterInList(s);
    for (uint32_t i = 0; i + 1 < Session::kLeaveWithoutEasu; ++i)
        s.OnPresent(Facts());
    Check(s.InList(), "leave: M-1 presents without the pass keep the session");
    Check(s.OnPresent(Facts()).exit == Exit::NoEasu, "leave: M presents without the pass");

    Session f;
    EnterInList(f);
    for (uint32_t i = 0; i < Session::kLeaveAfterFallbacks / 2 - 1; ++i)
    {
        f.NoteEasu(true);
        f.NoteFallback(Fallback::Busy);
        f.NoteFallback(Fallback::Rebuild);
        f.OnPresent(Facts());
    }
    f.NoteEasu(true);
    f.NoteReplaced();
    f.OnPresent(Facts());
    Check(f.InList(), "leave: a replacement starts the fallbacks in a row again");
    Decision d;
    for (uint32_t i = 0; i < Session::kLeaveAfterFallbacks / 2; ++i)
    {
        f.NoteEasu(true);
        f.NoteFallback(Fallback::Busy);
        f.NoteFallback(Fallback::Producer);
        d = f.OnPresent(Facts());
    }
    Check(d.exit == Exit::Fallbacks, "leave: N fallbacks in a row, several per present");
    const Snapshot snap = f.Take();
    Check(snap.fallbacks[size_t(Fallback::Busy)] == Session::kLeaveAfterFallbacks - 1 && snap.replaced == 1 &&
              snap.presentsInList == Session::kLeaveAfterFallbacks,
          "leave: fallbacks counted by reason, replacements and presents in the lists");

    // Leaving: asks the producer every present, back on the present path once it is idle
    PresentFacts leaving = Facts();
    for (uint32_t i = 0; i < Session::kLeaveAfterFallbacks - 1; ++i)
    {
        d = f.OnPresent(leaving);
        Check(!d.forgetInList || i + 1 >= Session::kLeaveAfterFallbacks, "leaving: nothing forgotten early");
    }
    Check(d.mode == Mode::Leaving && d.leaveInList && !d.runPresentPath, "leaving: the present path waits");
    leaving.drained = false;
    d = f.OnPresent(leaving);
    Check(!d.forgetInList, "leaving: never forgotten while a list holding ours can run");
    leaving.drained = true;
    d = f.OnPresent(leaving);
    Check(d.forgetInList && f.Take().forgets == 1, "leaving: frames nothing completes are forgotten after N presents");
    Check(!f.OnPresent(leaving).forgetInList, "leaving: forgotten once");
    leaving.leaveIdle = true;
    d = f.OnPresent(leaving);
    Check(d.mode == Mode::PresentPath && d.runPresentPath && !d.leaveInList, "leaving: back once the producer is idle");
    EnterInList(f);
    Check(f.InList() && f.Take().enters == 2, "leaving: entered again after K good presents");

    Session busy;
    EnterInList(busy);
    busy.OnPresent(Facts(false));
    PresentFacts stuck = Facts();
    stuck.producerBusyInARow = Session::kLeaveAfterFallbacks;
    Check(busy.OnPresent(stuck).forgetInList, "leaving: a producer busy N presents in a row is forgotten at once");
}

// Recorded, executed, reset: released once the GPU passed the last execution, never before (the pending table
// holds the producer's resources and the ledger while a list may run).
void PendingLifetime()
{
    Pending pending;
    FakeQueue queue, other;
    std::atomic<int> released { 0 };
    int list = 0;
    const void* lists[] { &list };
    ReserveResult why = ReserveResult::Ok;
    const int index = pending.Reserve(&list, &why);
    Check(index >= 0 && why == ReserveResult::Ok, "pending: reserved");
    Check(pending.Reserve(&list, &why) < 0 && why == ReserveResult::OneAlready, "pending: one per recording");
    PendingRef refs[] { { &released, Count }, { &released, Count } };
    pending.Commit(index, FakeTicket { 1 }, refs, 2);
    Check(pending.HoldsAny(1, lists) && !pending.Drained(), "pending: the list holds ours");

    Executed e = pending.AfterExecute(&queue, 1, lists);
    Check(e.queueSlot >= 0 && e.fenceValue == 1 && e.first == 1 && g_completedTickets == 1,
          "pending: executed, its ticket completed against the queue's fence");
    e = pending.AfterExecute(&other, 1, lists);
    Check(e.replays == 1 && e.first == 0 && g_completedTickets == 1, "pending: executed again on another queue");
    Check(!pending.OnResetOrRelease(&list), "pending: a reset after execution abandons nothing");
    pending.Drain();
    Check(released == 0, "pending: nothing released before the GPU is past it (release_first)");
    Finish(pending, queue);
    pending.Drain();
    Check(released == 0, "pending: nor before every queue it ran on is");
    Finish(pending, other);
    pending.Drain();
    Check(released == 2 && pending.Drained(), "pending: released once both queues are past it");
    Check(pending.Stats().retired == 1 && pending.Stats().replays == 1, "pending: counted");

    // Reset without execution: the ticket is abandoned, everything released at once
    const int again = pending.Reserve(&list, &why);
    pending.Commit(again, FakeTicket { 2 }, refs, 1);
    Check(pending.OnResetOrRelease(&list) && g_abandonedTickets == 1 && released == 3,
          "pending: reset without execution abandons its ticket and releases at once");
    Check(!pending.OnResetOrRelease(&list), "pending: a list holding nothing is ignored");

    // Cancel: recorded nothing after all
    const int cancelled = pending.Reserve(&list, &why);
    pending.Cancel(cancelled);
    Check(!pending.HoldsAny(1, lists) && pending.Stats().cancelled == 1, "pending: a cancelled entry is free again");
    pending.ReleaseAllIdle();
    Check(g_fences == 0, "pending: the queues' fences released when the device goes");
}

void PendingLimits()
{
    Pending pending;
    std::vector<int> lists(Pending::kLists + 1);
    ReserveResult why = ReserveResult::Ok;
    for (int i = 0; i < Pending::kLists; ++i)
        pending.Commit(pending.Reserve(&lists[i], &why), FakeTicket {}, nullptr, 0);
    Check(pending.Reserve(&lists[Pending::kLists], &why) < 0 && why == ReserveResult::Full,
          "limits: the 65th list is refused, not waited for");
    for (int i = 0; i < Pending::kLists; ++i)
        pending.OnResetOrRelease(&lists[i]);
    Check(pending.Reserve(&lists[Pending::kLists], &why) >= 0, "limits: entries free again after their resets");

    // A fifth queue: its lists cannot be followed, nothing more is replaced
    FakeQueue queues[Pending::kQueues + 1];
    const void* one[] { &lists[Pending::kLists] };
    for (int q = 0; q < Pending::kQueues; ++q)
        Check(pending.AfterExecute(&queues[q], 1, one).queueSlot == q, "limits: one fence per queue");
    const Executed fifth = pending.AfterExecute(&queues[Pending::kQueues], 1, one);
    Check(fifth.queueSlot < 0 && fifth.untracked == 1 && pending.QueuesFull(), "limits: a fifth queue overflows");
    Check(pending.TakeQueueOverflowLog() && !pending.TakeQueueOverflowLog(), "limits: logged once");
    Check(pending.Reserve(&lists[0], &why) < 0 && why == ReserveResult::Overflow, "limits: nothing more reserved");
    pending.OnResetOrRelease(&lists[Pending::kLists]);
    for (FakeQueue& q : queues)
        Finish(pending, q);
    pending.Drain();
    Check(!pending.Drained(), "limits: a list run where it was not followed is kept for good");
    pending.ReleaseAllIdle();
    Check(pending.Drained() && !pending.QueuesFull() && g_fences == 0,
          "limits: released when the device goes, fences too");

    // A queue whose Signal fails: kept, never released early
    Pending failing;
    FakeQueue mute;
    mute.signals = false;
    int list = 0;
    const void* single[] { &list };
    failing.Commit(failing.Reserve(&list, &why), FakeTicket {}, nullptr, 0);
    Check(failing.AfterExecute(&mute, 1, single).untracked == 1, "signal: an unsignalled execution is untracked");
    failing.OnResetOrRelease(&list);
    failing.Drain();
    Check(!failing.Drained() && failing.Stats().untracked == 1, "signal: kept for good");
    failing.ReleaseAllIdle();
}

// Let go by the upscaler while lists recorded before may still use it: freed once those retired
void PendingAfterRecorded()
{
    Pending pending;
    FakeQueue queue;
    int early = 0, late = 0;
    const void* lists[] { &early };
    ReserveResult why = ReserveResult::Ok;
    pending.Commit(pending.Reserve(&early, &why), FakeTicket {}, nullptr, 0);
    const int before = g_released;
    Check(pending.RetireAfterRecorded({ nullptr, ReleaseOne }), "after: queued");
    pending.Commit(pending.Reserve(&late, &why), FakeTicket {}, nullptr, 0); // recorded after: does not hold it
    pending.Drain();
    Check(g_released == before, "after: kept while a list recorded before is live");
    pending.AfterExecute(&queue, 1, lists);
    pending.OnResetOrRelease(&early);
    pending.Drain();
    Check(g_released == before, "after: kept until the GPU is past that list");
    Finish(pending, queue);
    pending.Drain();
    Check(g_released == before + 1 && pending.Stats().retiredAfter == 1, "after: released, the later list regardless");
    pending.OnResetOrRelease(&late);
    for (int i = 0; i < Pending::kAfter; ++i)
        pending.RetireAfterRecorded({ nullptr, ReleaseOne });
    Check(!pending.RetireAfterRecorded({ nullptr, ReleaseOne }), "after: a full queue says so (the caller keeps it)");
    pending.ReleaseAllIdle();
    Check(g_released == before + 1 + Pending::kAfter, "after: everything released when the device goes");
}

void LedgerChains()
{
    ReplayLedger<3> ledger;
    int a = 0, b = 0;
    Check(ledger.Record(&a, 0, 0, 1, 2) == 0 && ledger.Record(&a, 0, 0, 2, 4) == -1 && ledger.entries[0].after == 4,
          "ledger: a chain on one subresource is one entry, found state and left state");
    Check(ledger.Record(&a, 0, 1, 4, 1) == 1 && ledger.safe, "ledger: another subresource is another entry");
    Check(ledger.Record(&b, 1, 0, 8, 8, true) == 2 && ledger.safe, "ledger: a split barrier without a change is fine");
    ReplayLedger<3> broken = ledger;
    broken.Record(&a, 0, 0, 2, 1);
    Check(!broken.safe, "ledger: a transition from a state it was not left in is unsafe");
    Check(ledger.Record(&b, 1, 1, 1, 2) == -2 && !ledger.safe, "ledger: full means unsafe, never an overwrite");
    ReplayLedger<3> split;
    Check(split.Record(&a, 0, 0, 1, 2, true) == -2 && !split.safe, "ledger: a split transition is unsafe");
}

// Recording threads reserve, commit, execute (some twice) and reset; the present drains: each reference is released
// exactly once, none while its fence was not reached.
void Threads()
{
    Pending pending;
    FakeQueue queues[2];
    constexpr int kThreads = 8, kRounds = 4000;
    std::atomic<int> released { 0 }, refused { 0 }, early { 0 }, unexpected { 0 };
    std::atomic<bool> stop { false };
    struct Guard
    {
        std::atomic<int>* released = nullptr;
        std::atomic<int>* early = nullptr;
        std::atomic<bool> done { false };
    };
    std::vector<Guard> guards(size_t(kThreads) * kRounds);
    for (Guard& g : guards)
        g.released = &released, g.early = &early;
    auto release = [](void* p)
    {
        auto* g = static_cast<Guard*>(p);
        if (!g->done.load())
            ++*g->early;
        ++*g->released;
    };
    std::thread present(
        [&]
        {
            while (!stop.load())
            {
                pending.Drain();
                std::this_thread::yield();
            }
        });
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back(
            [&, t]
            {
                int list = 0;
                const void* lists[] { &list };
                ReserveResult why = ReserveResult::Ok;
                for (int r = 0; r < kRounds; ++r)
                {
                    const int index = pending.Reserve(&list, &why);
                    if (index < 0) // the present has not drained the retired ones yet
                    {
                        ++(why == ReserveResult::Full ? refused : unexpected);
                        continue;
                    }
                    Guard& guard = guards[size_t(t) * kRounds + r];
                    const PendingRef refs[] { { &guard, release } };
                    pending.Commit(index, FakeTicket { 1 }, refs, 1);
                    FakeQueue& q = queues[r & 1];
                    const Executed e = pending.AfterExecute(&q, 1, lists);
                    if (r % 7 == 0)
                        pending.AfterExecute(&queues[(r + 1) & 1], 1, lists);
                    // The GPU reaches it at some point after: mark it done, then let the fence say so
                    guard.done = true;
                    if (auto* fence = pending.QueueFence(e.queueSlot))
                        for (uint64_t c = fence->completed.load(); c < e.fenceValue;)
                            fence->completed.compare_exchange_weak(c, e.fenceValue);
                    pending.OnResetOrRelease(&list);
                }
            });
    for (auto& t : threads)
        t.join();
    for (FakeQueue& q : queues)
        Finish(pending, q);
    stop = true;
    present.join();
    pending.Drain();
    const PendingStats s = pending.Stats();
    std::printf("threads: %d recordings ours, %d refused, %d released refs, %llu executed again\n",
                kThreads * kRounds - refused.load(), refused.load(), released.load(), s.replays);
    Check(unexpected == 0 && refused < kThreads * kRounds / 10, "threads: refused only while the table is full");
    Check(released == kThreads * kRounds - refused && early == 0,
          "threads: every reference released once, never early");
    Check(pending.Drained() && s.listed == 0 && s.retiring == 0, "threads: nothing left");
    pending.ReleaseAllIdle();
}
} // namespace

int main()
{
    SessionEnters();
    SessionClaims();
    SessionLeaves();
    PendingLifetime();
    PendingLimits();
    PendingAfterRecorded();
    LedgerChains();
    Threads();
    if (g_failures != 0)
    {
        std::printf("nr_inlist_pending_smoke: %d failure(s) of %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("nr_inlist_pending_smoke: PASS (%d checks)\n", g_checks);
    return 0;
}
