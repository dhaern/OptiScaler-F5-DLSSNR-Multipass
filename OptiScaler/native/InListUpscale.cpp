#include "pch.h"
#include "InListUpscale.h"
#include "DepthFinderCore.h"
#include "InListPending.h"
#include "NativeDriver.h"
#include "NativeDriverDx12.h"
#include "NativeProducer.h"
#include "StageTimer_Dx12.h"
#include "VirtualUpscalerDriver.h"
#include "VirtualUpscalerParams.h"
#include <Config.h>
#include <State.h>
#include <resource_tracking/GenericDepth_Dx12.h>
#include <resource_tracking/SpatialPass_Dx12.h>
#include <memory>
#include <new>
#include <string>

namespace native::inlist
{
namespace
{
// ---- What the lists' threads read of the present's objects (ExecuteCommandLists, Reset, Release: any thread). Shared
// by them, exclusive by the present while it changes them; taken after the session, never with another lock inside.
SRWLOCK g_guard = SRWLOCK_INIT;
NativeProducer* g_producer = nullptr;  // the one the tickets in g_pending were handed out by
ID3D12Fence* g_prepareFence = nullptr; // signalled on the present's queue after the last Prepare
uint64_t g_prepareValue = 0;

// The stage timer's ring slots: recorded into a list, then collected at a present once the list has run (Done) or
// dropped (Abandoned); never read before the GPU is done with them.
constexpr uint8_t kTimerFree = 0, kTimerRecorded = 1, kTimerDone = 2, kTimerAbandoned = 3;
constinit std::atomic<uint8_t> g_timerSlots[StageTimerDx12::kRing] {};

struct PendingTicket
{
    InListTicket producer;
    int timer = -1;
};
struct PendingApi : envelope::PendingFencesDx12
{
    using Ticket = PendingTicket;
    static void Complete(const Ticket& ticket, ID3D12Fence* fence, uint64_t value)
    {
        AcquireSRWLockShared(&g_guard);
        if (g_producer != nullptr && ticket.producer.valid)
            g_producer->CompleteInList(ticket.producer, fence, value);
        ReleaseSRWLockShared(&g_guard);
    }
    static void Abandon(const Ticket& ticket)
    {
        AcquireSRWLockShared(&g_guard);
        if (g_producer != nullptr && ticket.producer.valid)
            g_producer->AbandonInList(ticket.producer);
        ReleaseSRWLockShared(&g_guard);
        if (ticket.timer >= 0)
            g_timerSlots[ticket.timer].store(kTimerAbandoned, std::memory_order_release);
    }
};
using Pending = InListPending<PendingApi>;
constinit Session g_session;
constinit Pending g_pending;
constinit ReplayDx12 g_replay;
constinit Ledger g_ledgers[Pending::kLists];
constinit Ledger g_prepareLedger;
SRWLOCK g_submit = SRWLOCK_INIT; // submission only, never a draw or recording lock
struct Fixup
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    uint64_t value = 0; // the queue fence's value after its last submission (UINT64_MAX: submitted, value pending)
};
constinit Fixup g_fixups[Pending::kQueues][Pending::kLists];

// ---- Under the session (the recording thread and the present)
// What the last replacement found to be built (VirtualUpscalerDriver::WouldRebuild): the present builds it (Prepare).
struct Wish
{
    bool wanted = false, hasDepth = false, depthReversed = false;
    D3D12_RESOURCE_DESC color {}, motion {}, depth {};
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES pictureState = D3D12_RESOURCE_STATE_COMMON;
    ColorSpace space = ColorSpace::Srgb;
    ScaledTarget target {};
};
Wish g_wish;
uint64_t g_lastEvaluated = 0;            // the present (claim token) of the last upscale evaluated in a list
bool g_hold = false;                     // this present: work recorded into a game's list may still run
std::unique_ptr<StageTimerDx12> g_timer; // [DlssNr] NativeStageTimings, on the producer's device
uint64_t g_timestampFrequency = 0;       // the present's queue (0: not asked for yet)
uint32_t g_shape[8] {}; // the last replacement's flow, colour, depth and output sizes (the GPU ms line)
bool g_targetLogged = false;
uint64_t g_leaked = 0; // let-go objects of the upscaler the retire queue had no room for: kept for good

// ---- The present only
bool g_leaveIdle = false;
uint64_t g_logPresents = 0;
struct PrepareObjects
{
    ID3D12Device* device = nullptr; // not held: the objects below hold it
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* proxies[3] {}; // the shapes Prepare was given, released once its list has run
};
PrepareObjects g_prepare;

// ---- Any thread: counters for the log, and the switch that stops replacing for good
constinit std::atomic<bool> g_off {}, g_discardedEvaluate {}, g_replayUnsafe {};
constinit std::atomic<uint64_t> g_depthGame {}, g_depthNeutral {}, g_replayed {};
constinit std::atomic<uint64_t> g_refused[size_t(fsr1::Refusal::Count)] {}; // the detection's, by reason

void ReleaseUnknown(void* object) { static_cast<IUnknown*>(object)->Release(); }
void TimerRetired(void* slot)
{
    uint8_t recorded = kTimerRecorded;
    g_timerSlots[reinterpret_cast<uintptr_t>(slot) - 1].compare_exchange_strong(recorded, kTimerDone);
}
// The upscaler's retire sink (VirtualUpscalerDriver::SetRetireSink), under the session
void Retire(void*, Retired retired)
{
    // Freeing it now could pull it from under a list still running
    if (retired.object != nullptr && retired.destroy != nullptr &&
        !g_pending.RetireAfterRecorded(PendingRef { retired.object, retired.destroy }))
        ++g_leaked;
}
void PublishProducer(NativeProducer* producer)
{
    AcquireSRWLockExclusive(&g_guard);
    g_producer = producer;
    ReleaseSRWLockExclusive(&g_guard);
}
void ReleaseProxies()
{
    for (auto*& proxy : g_prepare.proxies)
        SAFE_RELEASE(proxy);
}
bool PrepareRunning() { return g_prepareFence != nullptr && g_prepareFence->GetCompletedValue() < g_prepareValue; }
// Only once its list has run (PrepareRunning false) or the device is gone
void ReleasePrepare()
{
    ReplayDx12::Clear(&g_prepareLedger);
    ReleaseProxies();
    SAFE_RELEASE(g_prepare.list);
    SAFE_RELEASE(g_prepare.allocator);
    g_prepare.device = nullptr;
    AcquireSRWLockExclusive(&g_guard);
    ID3D12Fence* fence = g_prepareFence;
    g_prepareFence = nullptr;
    g_prepareValue = 0;
    ReleaseSRWLockExclusive(&g_guard);
    SAFE_RELEASE(fence);
}
bool EnsurePrepareObjects(ID3D12Device* device)
{
    if (g_prepare.device == device && g_prepare.list != nullptr)
        return true;
    if (PrepareRunning())
        return false;
    ReleasePrepare();
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_prepare.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_prepare.allocator, nullptr,
                                         IID_PPV_ARGS(&g_prepare.list))) ||
        FAILED(g_prepare.list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        SAFE_RELEASE(fence);
        ReleasePrepare();
        return false;
    }
    g_prepare.device = device;
    AcquireSRWLockExclusive(&g_guard);
    g_prepareFence = fence;
    g_prepareValue = 0;
    ReleaseSRWLockExclusive(&g_guard);
    return true;
}
// A texture of the shape `desc` describes, standing in for one the game or the producer owns: Prepare reads only
// descriptions.
ID3D12Resource* MakeProxy(ID3D12Device* device, D3D12_RESOURCE_DESC desc)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Alignment = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    ID3D12Resource* resource = nullptr;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&resource)))
               ? resource
               : nullptr;
}
// The upscaler is built on the present's queue, on a list of ours, never on a game's recording thread; the lists
// recorded after it wait for its fence on their own queues (Execute).
void Prepare(NativeDriver& driver, ID3D12CommandQueue* queue)
{
    VirtualUpscalerDriver* vud = driver.VirtualUpscaler();
    NativeProducer* producer = driver.Producer();
    ID3D12Device* device = nullptr;
    if (vud == nullptr || producer == nullptr || queue == nullptr || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return;
    device->Release(); // the producer holds it
    if (device != producer->Device() || !EnsurePrepareObjects(device) || PrepareRunning())
        return;
    ReleaseProxies();
    g_prepare.proxies[0] = MakeProxy(device, g_wish.color);
    g_prepare.proxies[1] = MakeProxy(device, g_wish.motion);
    g_prepare.proxies[2] = g_wish.hasDepth ? MakeProxy(device, g_wish.depth) : nullptr;
    if (g_prepare.proxies[0] == nullptr || g_prepare.proxies[1] == nullptr ||
        (g_wish.hasDepth && g_prepare.proxies[2] == nullptr) || FAILED(g_prepare.allocator->Reset()) ||
        FAILED(g_prepare.list->Reset(g_prepare.allocator, nullptr)))
    {
        ReleaseProxies();
        LOG_WARN("Spatial pass: could not set up the upscaler's preparation, tried again at the next present");
        return;
    }
    NativeFrame frame;
    frame.color = g_prepare.proxies[0];
    frame.colorFormat = g_wish.colorFormat;
    frame.pictureState = g_wish.pictureState;
    frame.space = g_wish.space;
    frame.motion = g_prepare.proxies[1];
    frame.depth = g_prepare.proxies[2];
    frame.depthReversed = g_wish.depthReversed;
    ReplayDx12::Clear(&g_prepareLedger);
    g_replay.BeginRecord(g_prepareLedger, g_prepare.list, frame.color, nullptr, frame.depth);
    const bool ready = vud->Prepare(g_prepare.list, frame, g_wish.target);
    g_replay.EndRecord();
    g_wish.wanted = false;
    if (FAILED(g_prepare.list->Close()))
    {
        LOG_WARN("Spatial pass: the upscaler's preparation list did not close: {}", vud->Error());
        return;
    }
    ID3D12CommandList* lists[] = { g_prepare.list };
    AcquireSRWLockExclusive(&g_submit);
    queue->ExecuteCommandLists(1, lists);
    AcquireSRWLockExclusive(&g_guard);
    if (SUCCEEDED(queue->Signal(g_prepareFence, g_prepareValue + 1)))
        ++g_prepareValue;
    ReleaseSRWLockExclusive(&g_guard);
    g_replay.Prepared(g_prepareLedger, queue, g_prepareFence, g_prepareValue);
    ReleaseSRWLockExclusive(&g_submit);
    if (ready)
        LOG_INFO("Spatial pass: {} prepared for {}x{} to {}x{}", vud->BackendName(), (uint32_t) g_wish.color.Width,
                 g_wish.color.Height, g_wish.target.outputWidth, g_wish.target.outputHeight);
    else
        LOG_WARN("Spatial pass: the upscaler could not be prepared for {}x{} to {}x{}: {}",
                 (uint32_t) g_wish.color.Width, g_wish.color.Height, g_wish.target.outputWidth,
                 g_wish.target.outputHeight, vud->Error());
}
void Log(NativeDriver& driver)
{
    const Snapshot s = g_session.Take();
    const PendingStats p = g_pending.Stats();
    std::string fallbacks, exits;
    for (size_t i = 0; i < size_t(Fallback::Count); ++i)
        if (s.fallbacks[i] != 0)
            fallbacks +=
                (fallbacks.empty() ? "" : ", ") + std::string(Name(Fallback(i))) + " " + std::to_string(s.fallbacks[i]);
    for (size_t i = 1; i < std::size(g_refused); ++i) // the passes the detection left to the game, with its reasons
        if (const uint64_t count = g_refused[i].exchange(0, std::memory_order_relaxed))
            fallbacks += (fallbacks.empty() ? "" : ", ") + std::string(fsr1::Name(fsr1::Refusal(i))) + " " +
                         std::to_string(count);
    for (size_t i = 1; i < size_t(Exit::Count); ++i)
        if (s.exits[i] != 0)
            exits += (exits.empty() ? "(" : ", ") + std::string(Name(Exit(i))) + " " + std::to_string(s.exits[i]);
    VirtualUpscalerDriver* vud = driver.VirtualUpscaler();
    const std::string backend =
        vud != nullptr && vud->Active() ? vud->BackendName() + " " + vud->BackendVersion() : std::string("none built");
    LOG_INFO("Spatial pass: {}; replaced {} upscale passes in {} presents ({} in the game's list), entered {} times, "
             "left {}; fallbacks: {}; depth from the game {}, neutral {}; backend {}; presents left out (session "
             "busy) {}",
             Name(s.mode), s.replaced, s.presents, s.presentsInList, s.enters, exits.empty() ? "never" : exits + ")",
             fallbacks.empty() ? "none" : fallbacks, g_depthGame.load(), g_depthNeutral.load(), backend,
             NativeMotionDx12::BusyPresents());
    LOG_INFO(
        "Spatial pass: lists holding our work: committed {}, cancelled {}, executed {}, executed again {}, never "
        "executed {}, released {} (+{} let go by the upscaler), now {} recorded and {} waiting for the GPU, {} let "
        "go waiting; not followed (kept for good) {}; queues {}{}; let go without room (kept for good) {}",
        p.committed, p.cancelled, p.completed, p.replays, p.abandoned, p.retired, p.retiredAfter, p.listed, p.retiring,
        p.after, p.untracked, p.queues, p.overflow ? " (a fifth one ran our work)" : "", g_leaked);
    if (g_timer != nullptr && g_timer->Frames() != 0)
        LOG_INFO("Spatial pass: GPU ms (average/maximum per frame, inside the game's list): {}; flow {}x{}, colour "
                 "{}x{}, depth {}, output {}x{}",
                 g_timer->Report(), g_shape[0], g_shape[1], g_shape[2], g_shape[3],
                 g_shape[4] != 0 ? std::to_string(g_shape[4]) + "x" + std::to_string(g_shape[5]) : "none", g_shape[6],
                 g_shape[7]);
}

// The recording thread, under the session: the order of the steps is the contract (InListUpscale.h).
bool InSession(NativeDriver& driver, uint64_t token, ID3D12GraphicsCommandList* list, ListState& s,
               const fsr1::Plan& plan, ListRestoreSink& restore, Fallback* why)
{
    auto refuse = [why](Fallback reason)
    {
        *why = reason;
        return false;
    };
    // The mode moves only at a present, under the session: read again now that it is held. Nothing of the driver is
    // read before this point, nor kept after it.
    if (!g_session.InList())
        return refuse(Fallback::NotInList);
    NativeProducer* producer = driver.Producer();
    VirtualUpscalerDriver* vud = driver.VirtualUpscaler();
    ID3D12Device* device = nullptr;
    if (producer == nullptr || !producer->Ready() || vud == nullptr || g_pending.QueuesFull() || g_off.load() ||
        FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
        return refuse(Fallback::Producer);
    device->Release();
    if (device != producer->Device())
        return refuse(Fallback::Producer);
    if (g_wish.wanted || g_discardedEvaluate.load())
        return refuse(Fallback::Rebuild);
    // Frame generation tearing its swap chain down: UpscaleStart would wait for it. Owner 2, its present, is waited
    // for as a game's own upscaler call does.
    const auto& state = State::Instance();
    if (state.currentFG != nullptr && state.currentFG->Mutex.getOwner() == 1)
        return refuse(Fallback::FgTeardown);
    if (!g_replay.CapacityAvailable())
        return refuse(Fallback::Replay);
    // What the producer makes while recording (its textures sized to the input) is ours too: the present's window and
    // this one never overlap, both under the session.
    struct OwnedResources
    {
        OwnedResources() { g_replay.BeginResources(); }
        ~OwnedResources() { g_replay.EndResources(); }
    } const owned;
    ReserveResult reserve = ReserveResult::Ok;
    const int index = g_pending.Reserve(list, &reserve);
    if (index < 0)
        return refuse(reserve == ReserveResult::OneAlready ? Fallback::OneAlready
                      : reserve == ReserveResult::Overflow ? Fallback::Producer
                                                           : Fallback::Busy);
    envelope::Enter(s);
    // [DlssNr] NativeStageTimings: timestamps from before the depth copy to after the restore, the producer's and the
    // upscaler's steps marked in between; collected once the list has run.
    int timerSlot = -1;
    StageTimerDx12* timer = nullptr;
    for (int i = 0; g_timer != nullptr && g_timer->Ready() && i < int(StageTimerDx12::kRing) && timerSlot < 0; ++i)
    {
        uint8_t free = kTimerFree;
        if (Config::Instance()->DlssNrNativeStageTimings.value_or_default() &&
            g_timerSlots[i].compare_exchange_strong(free, kTimerRecorded))
        {
            timerSlot = i;
            timer = g_timer.get();
            timer->Begin(list, uint32_t(i));
        }
    }
    // The depth: the finder's copy of this frame in this list, or none (the upscaler's neutral depth).
    const GenericDepthDx12::CopyNow depth = GenericDepthDx12::CopyThisFrame(list);
    if (timer != nullptr)
        timer->Mark(list, "depth", "copy");
    g_replay.BeginRecord(g_ledgers[index], list, plan.input.resource, plan.output.resource, depth.resource);
    const bool useDepth = depth.valid && depth.resource != nullptr && depth.width == plan.inputDesc.Width &&
                          depth.height == plan.inputDesc.Height;
    FrameInput in;
    in.api = Api::D3D12;
    in.frame = token;
    in.picture = plan.input.resource;
    in.pictureFormat = plan.input.format;
    in.pictureState = plan.inputState;
    in.colorSpace = ColorSpace::Srgb;
    in.width = (uint32_t) plan.inputDesc.Width;
    in.height = plan.inputDesc.Height;
    const uint64_t jitter = vparams::g_scaledJitter.load(std::memory_order_relaxed);
    std::memcpy(in.jitter, &jitter, sizeof(in.jitter));
    if (useDepth)
    {
        in.depth[0] = depth.resource;
        in.depthCount = 1;
        in.depthView = depth.viewFormat;
        in.depthWidth = depth.width;
        in.depthHeight = depth.height;
        in.depthReversed = depth.reversed;
    }
    const bool direct = envelope::CanWriteDirect(envelope::TargetOf(plan));
    ScaledTarget scaled;
    scaled.outputWidth = plan.box.right;
    scaled.outputHeight = plan.box.bottom;
    scaled.renderSubrect = plan.inputRect;
    scaled.depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // the producer's guide depth
    scaled.allowNr = false;
    scaled.outputFormat = plan.outputFormat;
    scaled.allowBuild = false;
    // The first upscale after a present without one does not continue the backend's history
    const bool gap = g_lastEvaluated == 0 || token != g_lastEvaluated + 1;
    bool evaluated = false, rebuild = false;
    const auto apply = [&](ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)
    {
        if (vud->WouldRebuild(frame, scaled))
        {
            g_wish = { true,
                       frame.depth != nullptr,
                       frame.depthReversed,
                       frame.color->GetDesc(),
                       frame.motion->GetDesc(),
                       frame.depth != nullptr ? frame.depth->GetDesc() : D3D12_RESOURCE_DESC {},
                       frame.colorFormat,
                       frame.pictureState,
                       frame.space,
                       scaled };
            rebuild = true;
            return false;
        }
        NativeFrame f = frame;
        f.reset = f.reset || gap;
        scaled.output = direct ? plan.output.resource : nullptr;
        scaled.outputState = envelope::TargetOf(plan).stateAtCall;
        return evaluated = vud->RunScaled(cmd, f, scaled);
    };
    NativeProducer::Options options;
    options.apply = true;
    options.flowFinestLevel = 1;
    options.smoothRadius = 2;
    options.timer = timer;
    InListTicket ticket;
    const NativeProducer::Result result = producer->RecordInList(list, in, options, apply, ticket);
    ID3D12Resource* output = vud->Output();
    const envelope::Target target = envelope::TargetOf(plan);
    const bool writable = evaluated && (direct || (output != nullptr && envelope::CanWrite(target, output->GetDesc())));
    const bool written = writable && (direct || envelope::RecordWrite(list, target, output));
    if (timer != nullptr && !direct)
        timer->Mark(list, "output", "copy");
    envelope::Restore(s, restore);
    if (timer != nullptr)
    {
        timer->Mark(list, "restore", "barriers");
        timer->End(list);
    }
    g_replay.EndRecord();
    if (result.submitted || ticket.valid)
    {
        // Ours that the list reads or writes, held until the GPU is done with it (the game's input and target never)
        PendingRef refs[Pending::kRefs];
        int count = 0;
        refs[count++] = { &g_ledgers[index], ReplayDx12::Clear };
        if (output != nullptr)
        {
            output->AddRef();
            refs[count++] = { output, ReleaseUnknown };
        }
        if (useDepth)
        {
            depth.resource->AddRef();
            refs[count++] = { depth.resource, ReleaseUnknown };
        }
        if (timerSlot >= 0)
            refs[count++] = { reinterpret_cast<void*>(uintptr_t(timerSlot) + 1), TimerRetired };
        g_pending.Commit(index, PendingTicket { ticket, timerSlot }, refs, count);
        (useDepth ? g_depthGame : g_depthNeutral).fetch_add(1, std::memory_order_relaxed);
        const OpticalFlowDx12* flow = producer->Flow();
        const uint32_t shape[8] { flow != nullptr ? flow->FlowWidth() : 0u,
                                  flow != nullptr ? flow->FlowHeight() : 0u,
                                  in.width,
                                  in.height,
                                  useDepth ? depth.width : 0u,
                                  useDepth ? depth.height : 0u,
                                  scaled.outputWidth,
                                  scaled.outputHeight };
        std::memcpy(g_shape, shape, sizeof(shape));
    }
    else
    {
        ReplayDx12::Clear(&g_ledgers[index]);
        g_pending.Cancel(index);
        if (timerSlot >= 0)
            g_timerSlots[timerSlot].store(kTimerAbandoned, std::memory_order_release);
    }
    if (evaluated)
        g_lastEvaluated = token;
    if (written && !g_targetLogged)
    {
        g_targetLogged = true;
        LOG_INFO("Spatial pass: the game's upscale target is {}x{} format {} ({}), {} unordered access; the upscaler's "
                 "output {}",
                 plan.outputDesc.Width, plan.outputDesc.Height, int(plan.outputDesc.Format),
                 plan.shape == fsr1::Shape::Dispatch ? "written by a dispatch" : "a render target",
                 plan.outputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ? "allows" : "does not allow",
                 direct ? "is written into it directly" : "is copied into it");
    }
    if (written)
        return true;
    if (rebuild)
        return refuse(Fallback::Rebuild);
    if (evaluated && output != nullptr && !writable)
        return refuse(Fallback::RtFormat);
    if (!result.submitted && result.stoppedAt != nullptr && std::strcmp(result.stoppedAt, "busy") == 0)
        return refuse(Fallback::Producer);
    return refuse(Fallback::LateFallback);
}
} // namespace

void NoteRefused(fsr1::Refusal why)
{
    g_session.NoteEasu(false);
    g_refused[size_t(why) < std::size(g_refused) ? size_t(why) : 0].fetch_add(1, std::memory_order_relaxed);
}

bool OnPass(ID3D12GraphicsCommandList* list, ListState& state, const fsr1::Plan& plan, ListRestoreSink& restore)
{
    g_session.NoteEasu(true);
    Fallback why = Fallback::NotInList;
    bool replaced = false;
    if (g_session.InList() && list != nullptr)
    {
        const uint64_t token = g_session.TryClaimPresent();
        if (token == 0)
            why = Fallback::OneAlready;
        else
        {
            {
                const NativeMotionDx12::Session session = NativeMotionDx12::TrySession();
                if (!session)
                    why = Fallback::Busy;
                else
                    replaced = InSession(*session.Driver(), token, list, state, plan, restore, &why);
            }
            if (!replaced)
                g_session.UnclaimPresent(token);
        }
    }
    if (replaced)
        g_session.NoteReplaced();
    else
        g_session.NoteFallback(why);
    return replaced;
}

void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn original)
{
    if (!g_pending.HoldsAny(count, lists))
    {
        original(queue, count, lists);
        return;
    }
    // One submission at a time: the fix-ups are planned against the states the submissions before left.
    AcquireSRWLockExclusive(&g_submit);
    // What the lists recorded since the last Prepare use was built by it, on the present's queue
    AcquireSRWLockShared(&g_guard);
    if (g_prepareFence != nullptr && g_prepareValue != 0)
        queue->Wait(g_prepareFence, g_prepareValue);
    ReleaseSRWLockShared(&g_guard);
    const int queueSlot = g_pending.QueueSlotOf(queue);
    ID3D12Fence* fence = queueSlot >= 0 ? g_pending.QueueFence(queueSlot) : nullptr;
    const uint64_t completed = fence != nullptr ? fence->GetCompletedValue() : 0;
    std::unique_ptr<ID3D12CommandList*[]> expanded(
        count <= UINT_MAX / 2 ? new (std::nothrow) ID3D12CommandList*[size_t(count) * 2] : nullptr);
    Fixup* used[Pending::kLists] {};
    int usedCount = 0;
    UINT expandedCount = 0;
    g_replay.BeginSubmission();
    for (UINT i = 0; i < count; ++i)
    {
        if (const int index = g_pending.EntryOf(lists[i]); index >= 0)
        {
            D3D12_RESOURCE_BARRIER barriers[kLedger];
            UINT barrierCount = 0;
            bool safe = expanded != nullptr && queueSlot >= 0 &&
                        g_replay.Plan(g_ledgers[index], queue, barriers, &barrierCount);
            if (safe && barrierCount != 0)
            {
                Fixup* fixup = nullptr;
                for (auto& candidate : g_fixups[queueSlot])
                    if (candidate.value <= completed)
                    {
                        fixup = &candidate;
                        break;
                    }
                ID3D12Device* device = nullptr;
                if (fixup != nullptr && fixup->list == nullptr)
                    safe = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
                           SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                    IID_PPV_ARGS(&fixup->allocator))) &&
                           SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, fixup->allocator,
                                                               nullptr, IID_PPV_ARGS(&fixup->list))) &&
                           SUCCEEDED(fixup->list->Close());
                SAFE_RELEASE(device);
                safe = safe && fixup != nullptr && SUCCEEDED(fixup->allocator->Reset()) &&
                       SUCCEEDED(fixup->list->Reset(fixup->allocator, nullptr));
                if (safe)
                {
                    fixup->list->ResourceBarrier(barrierCount, barriers);
                    safe = SUCCEEDED(fixup->list->Close());
                }
                if (safe)
                {
                    expanded[expandedCount++] = fixup->list;
                    fixup->value = UINT64_MAX;
                    used[usedCount++] = fixup;
                }
            }
            if (!safe)
            {
                g_session.NoteFallback(Fallback::Replay);
                g_replayUnsafe.store(true, std::memory_order_relaxed);
            }
            g_replay.Accept(g_ledgers[index]);
        }
        if (expanded != nullptr)
            expanded[expandedCount++] = lists[i];
    }
    // Interleaved rather than all first: a later list can need the states an earlier one leaves.
    original(queue, expanded != nullptr ? expandedCount : count, expanded != nullptr ? expanded.get() : lists);
    const Executed done = g_pending.AfterExecute(queue, count, lists);
    fence = done.queueSlot >= 0 ? g_pending.QueueFence(done.queueSlot) : nullptr;
    g_replay.Commit(queue, fence, done.fenceValue);
    for (int i = 0; i < usedCount; ++i)
        used[i]->value = fence != nullptr ? done.fenceValue : UINT64_MAX;
    g_replayed.fetch_add(done.replays, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_submit);
}

void OnResetOrRelease(const void* list)
{
    if (g_pending.OnResetOrRelease(list))
        g_discardedEvaluate.store(true, std::memory_order_relaxed);
}

PresentStep BeginPresent(NativeDriver& driver, ID3D12Device* device)
{
    g_replay.BeginResources();
    NativeProducer* producer = driver.Producer();
    VirtualUpscalerDriver* vud = driver.VirtualUpscaler();
    // A list holding an upscale was dropped unexecuted: the backend's history moved on without the game's frame
    if (!PrepareRunning() && g_discardedEvaluate.exchange(false))
    {
        if (vud != nullptr)
            vud->RebuildAfterDiscardedList();
        g_lastEvaluated = 0;
        g_wish.wanted = g_wish.color.Width != 0;
    }
    PresentFacts facts;
    facts.enabled = driver.DecideRoles().inList;
    facts.gameUpscaler = GameUpscalerCalledRecently();
    facts.deviceChanged = producer != nullptr && device != nullptr && producer->Device() != device;
    facts.leaveIdle = g_leaveIdle;
    facts.producerBusyInARow = producer != nullptr ? producer->InListStatus().busyInARow : 0;
    facts.drained = g_pending.Drained();
    const Decision decision = g_session.OnPresent(facts);
    if (decision.forgetInList && producer != nullptr)
    {
        producer->ForgetInList();
        LOG_WARN("Spatial pass: frames recorded into the game's lists were never completed; the producer forgets them");
    }
    // Back to the present path only once nothing recorded into a game's list can run: a list executed again reads the
    // producer's ring after its ticket was completed, so its slot is not reused until then either.
    g_leaveIdle = decision.leaveInList && facts.drained && (producer == nullptr || producer->LeaveInList());
    if (decision.enterInList)
    {
        SpatialPassDx12::OnSessionEnter();
        LOG_INFO("Spatial pass: replacing the game's FSR1 upscale with the upscaler from now on");
    }
    if (decision.exit != Exit::None)
        LOG_INFO("Spatial pass: back to the game's FSR1 upscale ({})", Name(decision.exit));
    g_hold = decision.mode != Mode::PresentPath || !facts.drained || PrepareRunning();
    if (!g_hold)
    {
        // Nothing recorded into a game's list can run any more: what belongs to another device can go
        if (facts.deviceChanged)
        {
            if (producer != nullptr)
                producer->ForgetInList();
            g_pending.ReleaseAllIdle();
            g_timer.reset();
            for (auto& slot : g_timerSlots)
                slot.store(kTimerFree);
            g_timestampFrequency = 0;
            AcquireSRWLockExclusive(&g_submit);
            for (auto& ring : g_fixups)
                for (auto& fixup : ring)
                {
                    SAFE_RELEASE(fixup.list);
                    SAFE_RELEASE(fixup.allocator);
                    fixup.value = 0;
                }
            ReleaseSRWLockExclusive(&g_submit);
            ReleasePrepare();
            g_wish.wanted = false;
            g_lastEvaluated = 0;
        }
        // The producer may be made again in this present: no ticket can reach it meanwhile
        PublishProducer(nullptr);
    }
    if (vud != nullptr)
        vud->SetRetireSink(g_hold ? Retire : nullptr, nullptr);
    PresentStep step;
    step.runPresentPath = decision.runPresentPath && facts.drained;
    step.keepDevice = g_hold;
    return step;
}

void EndPresent(NativeDriver& driver, ID3D12CommandQueue* queue)
{
    if (VirtualUpscalerDriver* vud = driver.VirtualUpscaler())
        vud->SetRetireSink(g_hold ? Retire : nullptr, nullptr);
    g_pending.Drain();
    if (!PrepareRunning())
        ReleaseProxies();
    if (g_wish.wanted)
    {
        if (g_session.InList())
            Prepare(driver, queue);
        else
            g_wish.wanted = false;
    }
    NativeProducer* producer = driver.Producer();
    PublishProducer(producer);
    const bool timings = Config::Instance()->DlssNrNativeStageTimings.value_or_default();
    // The frequency of the present's direct queue, which the lists recorded here run on too
    if (timings && g_timestampFrequency == 0 && queue != nullptr &&
        FAILED(queue->GetTimestampFrequency(&g_timestampFrequency)))
        g_timestampFrequency = 0;
    if (timings && g_timer == nullptr && producer != nullptr)
    {
        g_timer = std::make_unique<StageTimerDx12>();
        if (!g_timer->Init(producer->Device()))
            g_timer.reset();
    }
    for (uint32_t i = 0; g_timer != nullptr && i < StageTimerDx12::kRing; ++i)
    {
        const uint8_t slot = g_timerSlots[i].load(std::memory_order_acquire);
        if (slot == kTimerDone && g_timestampFrequency != 0)
            g_timer->Collect(i, g_timestampFrequency);
        else if (slot == kTimerDone || slot == kTimerAbandoned)
            g_timer->Abandon(i);
        if (slot == kTimerDone || slot == kTimerAbandoned)
            g_timerSlots[i].store(kTimerFree, std::memory_order_release);
    }
    if (g_pending.TakeQueueOverflowLog())
        LOG_WARN(
            "Spatial pass: a fifth queue ran a list holding our work; the game's upscale is left to it from now on");
    if (const uint64_t replayed = g_replayed.exchange(0))
        LOG_INFO("Spatial pass: replayed a closed list with its private resource states restored ({} since the "
                 "last present)",
                 replayed);
    if (g_replayUnsafe.exchange(false))
        LOG_WARN("Spatial pass: a list is not replay-safe (untracked state, bounded ledger or fix-up allocator "
                 "unavailable); leaving in-list mode without a CPU wait");
    // Lists that ran where they could not be followed keep what they hold for good: past a few, stop replacing
    const PendingStats stats = g_pending.Stats();
    if ((stats.untracked > 16 || g_leaked != 0) && !g_off.exchange(true))
        LOG_WARN("Spatial pass: lists holding our work could not be followed ({} not followed, {} let go without "
                 "room); the game's upscale is left to it from now on",
                 stats.untracked, g_leaked);
    if (++g_logPresents % 600 == 0)
        Log(driver);
    g_replay.EndResources();
}

void Created(ID3D12Resource* resource, D3D12_RESOURCE_STATES initial, D3D12_HEAP_TYPE heap)
{
    g_replay.Created(resource, initial, heap);
}
uint64_t ReleaseToken(IUnknown* object) { return g_replay.ReleaseToken(object); }
void Released(uint64_t token) { g_replay.Released(token); }
void Barriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    g_replay.Barriers(list, count, barriers);
}
} // namespace native::inlist
