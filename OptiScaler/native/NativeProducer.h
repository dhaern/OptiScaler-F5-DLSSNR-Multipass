#pragma once

// The API-neutral half of the native input producer: given a FrameInput (a finished picture and the scene's depth
// copies, as D3D12 resources) it records optical flow, the trust mask and the guides on its own or a caller's command
// list, and hands the picture and the guides to a consumer. It knows nothing of the game's API
// (native/FrameContract.h); an adapter supplies the frames.
//
// The consumer is not linked in here: the caller passes it in as a function (DLSS-NR's DlssNr::ApplyNativeInput, or an
// upscaler backend through native::VirtualUpscalerDriver, in the game; a stand-in in tests/nr_native_producer_gpu.cpp),
// so this class and everything it uses build and run without either.

#include "FrameContract.h"
#include "StageTimer_Dx12.h"

#include "../motion/OpticalFlow_Dx12.h"
#include "../motion/TrustMask_Dx12.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace native
{

struct InListTicket
{
    bool valid = false;
    uint32_t slot = 0;       // ring entry the frame used: not reused before its fence value is reached
    uint64_t frame = 0;      // the producer's frame number
    bool cutCopied = false;  // the scene-cut flag was copied to this slot's readback
    uint32_t generation = 0; // the producer that handed it out (never 0): another producer's ticket changes nothing
};

// What the producer hands its consumer for one frame. The consumer works on the picture in place.
struct NativeFrame
{
    ID3D12Resource* color = nullptr;                          // the picture: in pictureState, and left in it
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;            // a typed format to view it as (the resource may be typeless)
    D3D12_RESOURCE_STATES pictureState = D3D12_RESOURCE_STATE_COMMON;
    ColorSpace space = ColorSpace::Srgb;
    ID3D12Resource* depth = nullptr;  // the guides (TrustMaskDx12::BuildGuides): picture-sized, unjittered, and in
    ID3D12Resource* motion = nullptr; // NON_PIXEL_SHADER_RESOURCE, which the consumer must leave them in. depth
                                      // is null on a frame without depth.
    ID3D12Resource* historyDistrust = nullptr; // the trust mask (TrustMaskDx12::Mask()): how little the last frame's
                                               // history can be trusted at each pixel, R8_UNORM at the flow's size,
                                               // in a state readable by a non-pixel shader, which the consumer leaves
    bool depthReversed = true;                 // always: the guide depth is reversed-Z (TrustMaskDx12::BuildGuides)
    bool reset = false;               // the frame does not continue the last one
    StageTimerDx12* timer = nullptr;  // with [DlssNr] NativeStageTimings: the consumer marks its own steps on it
    bool abandonedBefore = false;     // in-list consumer work was discarded; recreate its private state
};

// How a hard scene cut is handled, end to end. Three signals can report one cut; Run() makes it one Result::sceneCut
// and one reset of the consumer.
//
// Signal 1, the flow's detector (OpticalFlowDx12, Tuning().sceneCutDetector, on by default). On the GPU it compares the
// brightness histogram of this picture with the last one's; past sceneCutThreshold it sets a flag on that very frame.
// The flow then matches nothing on that frame (zero motion, no confidence) and the trust mask, given the flag in
// Inputs::sceneCut, is distrust everywhere. The consumer sees neither (it takes the guides only), so its history still
// holds the last scene until signal 2 resets it.
//
// Signal 2, the flag's readback. Run() copies the flag into a readback slot of the frame's ring entry and reads it once
// the GPU has finished that frame, usually in the next Run(). A set flag outside the quiet window (below) reports the
// cut (sceneCut, distrustedShare 1) and asks for the consumer's reset (_consumerResetPending): the next time the
// consumer runs, NativeFrame::reset is set, and the request stays until it has run. This restarts the consumer's
// history a couple of frames before signal 3 could.
//
// Signal 3, the mask's late count. TrustMaskDx12 counts on the GPU the frames in which at least cutShare of the pixels
// are fully distrusted (only frames that had a history count) and reports it through SceneCutSeen() a couple of frames
// later. Seen outside the quiet window it reports the cut and asks for the consumer's reset for this frame. With the
// detector on, the flow and the mask carry on, the consumer runs on this frame too (skipping it showed one frame
// without it, a flash over the whole picture) and a quiet window opens. With the detector off, the flow and the mask
// are reset instead and the consumer does not run until they have a flow again; the reset is then kept asked for
// (_consumerResetPending) until it does. This is the only signal then.
//
// The quiet window (_cutQuietUntil, kRing + 2 frames after a reported cut) is what keeps the late count of a cut that
// signal 2 already reported from being a second cut: a signal inside it is ignored. A cut hint from the adapter
// (FrameInput::cutHint) and a size change are not cuts here: they reset the flow and the mask only, with no report. The
// caller counts Result::sceneCut and logs it (native/NativeDriver.cpp).
class NativeProducer
{
  public:
    // Runs the consumer on `frame`, on `cmd`. True when it ran.
    using ApplyFn = std::function<bool(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)>;

    struct Options
    {
        bool apply = false;           // run the consumer (needs flow; depth is optional); off: flow and trust mask only
        bool flowPreview = false;     // also draw the flow picture for the menu
        float previewMaxSpeed = 24.0f; // pixels per frame that show as full brightness
        bool stageTimings = false;     // time each step on the GPU (StageTimerDx12), reported in Result::timings
        uint32_t timingsEvery = 600;   // frames per report
        int flowFinestLevel = 0;
        int smoothRadius = -1;
        // RecordInList only: caller begins/ends/collects/abandons it and keeps it alive until the list is retired.
        // The producer only appends marks to an already recording timer, including the consumer's marks.
        StageTimerDx12* timer = nullptr;
    };

    struct Result
    {
        bool submitted = false; // Run: submitted to the queue; RecordInList: recorded, not yet executed
        bool flowValid = false;
        bool trustRan = false;   // the trust mask ran in this frame (depth or not; it degrades gracefully with none)
        bool sceneCut = false;   // the mask saw a hard cut: the histories were reset
        float distrustedShare = 0.0f;
        bool nativeRan = false;  // the consumer ran on the picture
        const char* stoppedAt = nullptr; // when nothing was submitted: the step that gave up (for the log)
        long stoppedHr = 0;              // and the HRESULT it got, when it had one
        long deviceRemoved = 0;          // the device's removed reason at that moment (0: not removed)
        std::string timings;             // Run only: steps' GPU times every timingsEvery frames
        bool abandonSeen = false;        // in-list histories were restarted after a discarded frame
    };

    struct InListHealth
    {
        uint32_t busyInARow = 0;
        uint32_t inFlight = 0;
        uint64_t oldestAge = 0; // recording/leave calls since the oldest unfinished ticket
        bool rebuildFailed = false;
        uint32_t rebuildFailures = 0;
    };
    static constexpr uint32_t kRebuildRetry = 30;

    NativeProducer() = default;
    ~NativeProducer();

    NativeProducer(const NativeProducer&) = delete;
    NativeProducer& operator=(const NativeProducer&) = delete;

    // Makes the flow, the mask and the command objects on `device`. False with a reason in Error().
    bool Init(ID3D12Device* device);
    bool Ready() const { return _flow != nullptr && _trust != nullptr; }
    ID3D12Device* Device() const { return _device; }
    const std::string& Error() const { return _error; }

    // One frame. `queue` is a direct queue of Device(); the work goes onto it after waiting for input.ready, and output.done is
    // the point it finishes at. The picture is processed in place.
    Result Run(ID3D12CommandQueue* queue, const FrameInput& input, const Options& options, const ApplyFn& apply,
               FrameOutput& output);

    // Open direct list of Device(): no reset, submit or wait; input.ready is ignored (same-list GPU order).
    // Picture enters/leaves in input.pictureState; depth must be non-pixel-readable and is not transitioned.
    // Flow/mask rest readable by both shader stages; guides rest NON_PIXEL_SHADER_RESOURCE, preview
    // PIXEL_SHADER_RESOURCE. Heaps, compute roots and PSO are changed; caller restores bindings. Busy records nothing,
    // with no valid ticket. Three tickets at most; no slot/size/rebuild reuse before its external fence completes.
    // Descriptor rings are larger. Complete/Abandon may publish concurrently; all other calls require the caller's
    // per-frame serialization.
    Result RecordInList(ID3D12GraphicsCommandList* cmd, const FrameInput& input, const Options& options,
                        const ApplyFn& apply, InListTicket& ticket);
    // Exactly once per ticket: after submission, or after Reset/Release proves the list cannot execute.
    // Complete holds the fence until polled; stale/repeated/foreign tickets (and null fences) change nothing.
    void CompleteInList(const InListTicket& ticket, ID3D12Fence* fence, uint64_t value);
    // Discarded CPU-side histories/states are reset; fresh textures are rebuilt after outstanding work drains.
    void AbandonInList(const InListTicket& ticket);
    // Run/RecordInList cannot overlap: Leave must return true before Run. Switching never waits here.
    bool LeaveInList();
    // Lost-ticket recovery only after the queue drains AND its lists can no longer run, never on a timeout.
    void ForgetInList();
    bool InListIdle() const;
    InListHealth InListStatus() const;

    // Forget the previous frames (the game calls an upscaler, a resolution change, a scene cut).
    void Reset();

    // For the menu.
    OpticalFlowDx12* Flow() { return _flow.get(); }
    TrustMaskDx12* Trust() { return _trust.get(); }
    // Rebuilds replace Flow/Trust; do not cache pointers/views across frames. Destroy only after InListIdle().
    bool PreviewReady() const;

  private:
    static constexpr uint32_t kRing = 3;

    void WaitFor(UINT64 value);
    static uint32_t NextGeneration();

    std::unique_ptr<OpticalFlowDx12> _flow;
    std::unique_ptr<TrustMaskDx12> _trust;
    ID3D12Device* _device = nullptr;
    ID3D12CommandAllocator* _allocators[kRing] = {};
    ID3D12GraphicsCommandList* _list = nullptr;
    ID3D12Fence* _fence = nullptr;
    HANDLE _event = nullptr;
    UINT64 _values[kRing] = {};
    UINT64 _signalled = 0;
    uint64_t _frame = 0;
    uint64_t _cutQuietUntil = 0; // a cut's consumer reset was asked for: the late count of the same cut is not another

    // The flow's scene-cut flag of each frame in the ring, copied out to be read once the GPU has finished that frame.
    ID3D12Resource* _cutReadback[kRing] = {};
    bool _cutPending[kRing] = {};
    bool _consumerResetPending = false; // a cut was found and the consumer has not run since
    uint32_t _width = 0;
    uint32_t _height = 0;
    bool _previewReady = false;
    std::string _error;

    // [DlssNr] NativeStageTimings: made the first time it is asked for, dropped when it is turned off.
    std::unique_ptr<StageTimerDx12> _timer;
    enum InListState : uint64_t
    {
        kFree = 0,       // nothing in flight
        kRecorded = 1,   // recorded into a caller's list, not yet completed or abandoned
        kCompleting = 2, // CompleteInList is filling in the fence
        kSubmitted = 3,  // executed: done once fence reaches value
        kAbandoned = 4   // never to be executed: RecordInList drops what it left
    };

    static constexpr uint64_t kStateBits = 3;
    static constexpr uint64_t kStateMask = (1u << kStateBits) - 1;
    static constexpr uint64_t Tag(uint64_t frame, uint64_t state) { return (frame << kStateBits) | state; }

    struct InListSlot
    {
        std::atomic<uint64_t> tag { 0 };
        ID3D12Fence* fence = nullptr; // written by CompleteInList before it publishes kSubmitted; one reference held
        uint64_t value = 0;
        uint64_t tick = 0;         // _inListTicks when it was recorded (InListStatus)
        bool cutPending = false;   // the flag was copied and is still to be read
        bool firstTouch = false;   // the frame moved a fresh texture to its resting state
        bool previewFirst = false; // the frame drew the first preview (it turned PreviewReady() on)
        bool consumerRan = false;  // the consumer ran in the frame (NativeFrame::abandonedBefore)
    };

    struct Touched
    {
        uint32_t pictureWidth = 0; // the picture size the flow's textures were made for (0: unknown, so fresh)
        uint32_t pictureHeight = 0;
        bool flow = false;      // OpticalFlowDx12's flow (made in NON_PIXEL_SHADER_RESOURCE, rests readable by both)
        bool preview = false;   // the flow's preview (rests in PIXEL_SHADER_RESOURCE)
        bool detector = false;  // SceneCutDx12's state (made readable, rests as a UAV); made by the flow's Init only
        uint32_t flowWidth = 0; // the flow size the mask's textures were made for (0: unknown, so fresh)
        uint32_t flowHeight = 0;
        uint32_t masks = 0; // mask dispatches since: TrustMaskDx12's two masks take turns, each rests readable by both
    };

    bool PollInList(Result& result);
    bool MakeInListReadback();
    bool RebuildParts();
    bool TryRebuild();
    void Retire(std::unique_ptr<OpticalFlowDx12> flow, std::unique_ptr<TrustMaskDx12> trust);
    void FreeRetired();

    // Placed texture-copy footprints must start at a 512-byte boundary.
    static constexpr uint32_t kFlagStride = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

    struct Retired
    {
        std::unique_ptr<OpticalFlowDx12> flow;
        std::unique_ptr<TrustMaskDx12> trust;
        uint64_t frame = 0;
    };

    static constexpr uint32_t kRetired = 4;

    InListSlot _inList[kRing];
    ID3D12Resource* _inListCutReadback = nullptr; // the scene-cut flag of each in-list slot, kFlagStride bytes apart
    UINT64 _inListRunMark = 0;                    // Run()'s _signalled when RecordInList last went on from it
    uint32_t _inListPictureWidth = 0;             // the picture's own size (its desc) in the last in-list frame
    uint32_t _inListPictureHeight = 0;
    bool _inListSwitch = false;  // Run() submitted since: busy until everything has finished, then reset
    bool _inListRebuild = false; // an abandoned frame moved a fresh texture: make the flow and the mask again
    bool _inListEntered = false; // a frame was recorded since the last LeaveInList that returned true
    bool _mixLogged = false;
    bool _consumerAbandonPending = false; // a frame the consumer ran in was abandoned and it has not run since
    bool _rebuildFailed = false;          // the last RebuildParts failed (InListHealth::rebuildFailed)
    uint32_t _rebuildFailures = 0;
    uint64_t _rebuildRetryAt = 0; // the _inListTicks from which a failed rebuild is tried again
    uint64_t _inListTicks = 0;    // calls of RecordInList and LeaveInList
    uint32_t _inListBusyRun = 0;  // RecordInList calls in a row that answered "busy"
    uint64_t _inListDoneMark = 0; // one past the latest in-list frame the GPU has finished (0: none yet)
    const uint32_t _generation = NextGeneration(); // InListTicket::generation
    Touched _inListTouched;
    Retired _retired[kRetired];

    struct Recording
    {
        ID3D12Resource* cutReadback;
        UINT64 cutOffset = 0;
        Touched* touched = nullptr;
        bool cutCopied = false;
        bool firstTouch = false;
        bool previewFirst = false;
        bool consumerCalled = false;
    };
    void RecordSteps(ID3D12GraphicsCommandList* list, const FrameInput& input, const Options& options,
                     const ApplyFn& apply, StageTimerDx12* timer, Result& result, Recording& steps);
};

} // namespace native
