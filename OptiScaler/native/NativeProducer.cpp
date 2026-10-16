#include "pch.h"

#include "NativeProducer.h"

#include <algorithm>
#include <new>

namespace native
{

NativeProducer::~NativeProducer()
{
    // The queue may still be running our list; the owner waits (WaitFor) before destroying when it matters.
    for (auto& allocator : _allocators)
        if (allocator != nullptr)
            allocator->Release();

    for (auto& readback : _cutReadback)
        if (readback != nullptr)
            readback->Release();

    for (auto& slot : _inList)
        if (slot.fence != nullptr)
            slot.fence->Release();
    if (_inListCutReadback != nullptr)
        _inListCutReadback->Release();

    if (_list != nullptr)
        _list->Release();
    if (_fence != nullptr)
        _fence->Release();
    if (_event != nullptr)
        CloseHandle(_event);
}

void NativeProducer::WaitFor(UINT64 value)
{
    if (_fence == nullptr || value == 0 || _fence->GetCompletedValue() >= value)
        return;

    _fence->SetEventOnCompletion(value, _event);
    WaitForSingleObject(_event, 1000);
}

bool NativeProducer::Init(ID3D12Device* device)
{
    _flow = std::make_unique<OpticalFlowDx12>();
    _trust = std::make_unique<TrustMaskDx12>();

    if (!_flow->Init(device))
    {
        _error = _flow->Error();
        _flow.reset();
        _trust.reset();
        return false;
    }

    if (!_trust->Init(device))
    {
        _error = _trust->Error();
        _flow.reset();
        _trust.reset();
        return false;
    }

    for (auto& allocator : _allocators)
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
        {
            _error = "creating a command allocator";
            return false;
        }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[0], nullptr,
                                         IID_PPV_ARGS(&_list))))
    {
        _error = "creating the command list";
        return false;
    }

    _list->Close();

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
    {
        _error = "creating the fence";
        return false;
    }

    // One row of the scene-cut flag per ring slot (a texture copy's rows are 256 bytes apart).
    D3D12_HEAP_PROPERTIES readbackHeap {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDesc {};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (auto& readback : _cutReadback)
        if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        {
            _error = "creating the scene-cut readback";
            return false;
        }

    _event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    _device = device;
    return true;
}

void NativeProducer::Reset()
{
    if (_flow)
        _flow->Reset();
    if (_trust)
        _trust->Reset();
}

NativeProducer::Result NativeProducer::Run(ID3D12CommandQueue* queue, const FrameInput& input, const Options& options,
                                           const ApplyFn& apply, FrameOutput& output)
{
    Result result;
    output = FrameOutput {};

    if (!Ready() || queue == nullptr || input.picture == nullptr || _list == nullptr)
    {
        result.stoppedAt = !Ready()                   ? "not ready"
                           : queue == nullptr         ? "no queue"
                           : input.picture == nullptr ? "no picture"
                                                      : "no list";
        return result;
    }

    // The previous work may still be reading the textures a new size replaces.
    if (input.width != _width || input.height != _height)
    {
        WaitFor(_signalled);
        _width = input.width;
        _height = input.height;
        Reset();
    }

    // The adapter knows this frame does not continue the last one (a loading screen, a resolution change).
    if (input.cutHint)
        Reset();

    const UINT slot = (UINT) (_frame % kRing);
    WaitFor(_values[slot]);

    // The step timings: this slot's last frame is finished, its timestamps can be read.
    if (options.stageTimings && _timer == nullptr)
    {
        _timer = std::make_unique<StageTimerDx12>();

        if (!_timer->Init(_device))
            result.timings = "the timestamp query heap could not be made";
    }
    else if (!options.stageTimings && _timer != nullptr)
    {
        WaitFor(_signalled);
        _timer.reset();
    }

    StageTimerDx12* timer = _timer != nullptr && _timer->Ready() ? _timer.get() : nullptr;

    if (timer != nullptr)
    {
        UINT64 frequency = 0;

        if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)))
            timer->Collect(slot, frequency);

        if (timer->Frames() >= (options.timingsEvery > 0 ? options.timingsEvery : 1u))
            result.timings = timer->Report();
    }

    // The flow's own cut flag of the frames the GPU has finished since (usually the last one): a cut found there resets
    // the consumer's history now, a couple of frames before the trust mask's count could say so. Until the consumer has
    // run with the reset it stays asked for.
    for (UINT s = 0; s < kRing; ++s)
    {
        if (!_cutPending[s] || _fence->GetCompletedValue() < _values[s])
            continue;

        _cutPending[s] = false;
        const D3D12_RANGE range { 0, sizeof(uint32_t) };
        uint32_t* data = nullptr;

        if (SUCCEEDED(_cutReadback[s]->Map(0, &range, (void**) &data)) && data != nullptr)
        {
            const bool cut = *data != 0;
            const D3D12_RANGE none { 0, 0 };
            _cutReadback[s]->Unmap(0, &none);

            if (cut && _frame >= _cutQuietUntil)
            {
                result.sceneCut = true;
                result.distrustedShare = 1.0f;
                _consumerResetPending = true;
                _cutQuietUntil = _frame + kRing + 2;
            }
        }
    }

    const HRESULT allocatorHr = _allocators[slot]->Reset();
    const HRESULT listHr = FAILED(allocatorHr) ? allocatorHr : _list->Reset(_allocators[slot], nullptr);

    if (FAILED(listHr))
    {
        result.stoppedAt = FAILED(allocatorHr) ? "allocator reset" : "list reset";
        result.stoppedHr = (long) listHr;
        result.deviceRemoved = _device != nullptr ? (long) _device->GetDeviceRemovedReason() : 0;
        return result;
    }

    ID3D12GraphicsCommandList* list = _list;

    if (timer != nullptr)
        timer->Begin(list, slot);

    Recording steps { _cutReadback[slot] };
    RecordSteps(list, input, options, apply, timer, result, steps);

    if (timer != nullptr)
        timer->End(list);

    const HRESULT closeHr = list->Close();

    if (SUCCEEDED(closeHr))
    {
        if (input.ready.fence != nullptr)
            queue->Wait(input.ready.fence, input.ready.value);

        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(_fence, ++_signalled);
        _values[slot] = _signalled;
        _cutPending[slot] = steps.cutCopied;
        ++_frame;

        output.done = SyncPoint { _fence, _signalled };
        output.processed = result.nativeRan;
        result.submitted = true;
    }

    if (!result.submitted && timer != nullptr)
        timer->Abandon(slot);

    if (!result.submitted && result.stoppedAt == nullptr)
    {
        result.stoppedAt = "close";
        result.stoppedHr = (long) closeHr;
        result.deviceRemoved = _device != nullptr ? (long) _device->GetDeviceRemovedReason() : 0;

        // A list whose recording failed stays open, and every later Reset of it fails: make a fresh one so the next
        // frame can try again.
        _list->Release();
        _list = nullptr;

        const HRESULT freshHr = _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[slot],
                                                           nullptr, IID_PPV_ARGS(&_list));

        if (SUCCEEDED(freshHr))
            _list->Close();
    }

    return result;
}

void NativeProducer::RecordSteps(ID3D12GraphicsCommandList* list, const FrameInput& input, const Options& options,
                                 const ApplyFn& apply, StageTimerDx12* timer, Result& result, Recording& steps)
{
    const bool inList = steps.touched != nullptr;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = input.picture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = input.pictureState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool needsBarrier = inList ? (input.pictureState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0
                                     : input.pictureState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (needsBarrier)
        list->ResourceBarrier(1, &barrier);

    // The flow's match sees where surfaces end through the depth, when it is one copy of the whole scene (several
    // copies each hold part of it, and one alone would show edges that are not there).
    const bool flowDepth = input.depthCount == 1 && input.depth[0] != nullptr && input.depthView != DXGI_FORMAT_UNKNOWN;
    const OpticalFlowDx12::Encoding encoding = input.colorSpace == ColorSpace::ScRgb ? OpticalFlowDx12::Encoding::ScRgb
                                               : input.colorSpace == ColorSpace::Pq  ? OpticalFlowDx12::Encoding::Pq
                                                                                     : OpticalFlowDx12::Encoding::Srgb;
    // The flow times its own passes into the timer's heap, from the timer's next free entry on.
    if (timer != nullptr)
        _flow->SetTimestampHeap(timer->Heap(), timer->Room() < 64u ? timer->Room() : 64u, timer->NextIndex());

    const bool recorded =
        _flow->Dispatch(list, input.picture, input.pictureFormat, flowDepth ? input.depth[0] : nullptr,
                        flowDepth ? input.depthView : DXGI_FORMAT_UNKNOWN, input.depthReversed, encoding,
                        steps.touched != nullptr ? options.flowFinestLevel : 0,
                        steps.touched != nullptr ? options.smoothRadius : -1, input.jitter[0], input.jitter[1]);

    if (timer != nullptr)
    {
        OpticalFlowDx12* flow = _flow.get();
        timer->Adopt(flow->TimestampCount(), "flow", [flow](uint32_t i) { return flow->TimestampName(i); });
        _flow->SetTimestampHeap(nullptr, 0);
    }

    if (steps.touched != nullptr)
    {
        Touched& touched = *steps.touched;
        const auto pictureDesc = input.picture->GetDesc();
        if (recorded)
        {
            if (touched.pictureWidth != (uint32_t) pictureDesc.Width || touched.pictureHeight != pictureDesc.Height)
            {
                touched.pictureWidth = (uint32_t) pictureDesc.Width;
                touched.pictureHeight = pictureDesc.Height;
                touched.flow = false;
                touched.preview = false;
            }

            steps.firstTouch = steps.firstTouch || !touched.flow;
            touched.flow = true;

            if (_flow->SceneCutFlag() != nullptr)
            {
                steps.firstTouch = steps.firstTouch || !touched.detector;
                touched.detector = true;
            }
        }
        else
        {
            touched.pictureWidth = 0;
            touched.pictureHeight = 0;
        }
    }

    // This frame's cut flag out to the readback, read once the GPU is done with the frame.
    if (ID3D12Resource* flag = recorded ? _flow->SceneCutFlag() : nullptr)
    {
        D3D12_RESOURCE_BARRIER toCopy {};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = flag;
        toCopy.Transition.Subresource = 0;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &toCopy);

        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = steps.cutReadback;
        dst.PlacedFootprint.Offset = steps.cutOffset;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = flag;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        const D3D12_BOX texel0 { 0, 0, 0, 1, 1, 1 };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &texel0);

        std::swap(toCopy.Transition.StateBefore, toCopy.Transition.StateAfter);
        list->ResourceBarrier(1, &toCopy);
        steps.cutCopied = true;

        if (timer != nullptr)
            timer->Mark(list, "flow", "cutcopy");
    }

    TrustMaskDx12::Inputs nativeInputs;
    bool nativeReady = false;
    bool nativeReset = false;

    if (recorded && _flow->FlowValid())
    {
        result.flowValid = true;

        if (options.flowPreview)
        {
            const bool drawn = _flow->Visualise(list, options.previewMaxSpeed);
            steps.previewFirst = drawn && !_previewReady;
            _previewReady = drawn || _previewReady;
            if (drawn && steps.touched != nullptr)
            {
                steps.firstTouch = steps.firstTouch || !steps.touched->preview;
                steps.touched->preview = true;
            }

            if (timer != nullptr)
                timer->Mark(list, "flow", "preview");
        }

        // The trust mask uses the scene's depth as the adapter copied it this frame; without any it runs on flow and luma.
        TrustMaskDx12::Inputs in;
        in.flow = _flow->Flow();
        in.flowWidth = _flow->FlowWidth();
        in.flowHeight = _flow->FlowHeight();
        in.fullPerFlow = (float) input.width / (float) _flow->FlowWidth();
        in.lumaNow = _flow->LumaOfLastFrame();
        in.lumaBefore = _flow->LumaOfFrameBefore();
        in.sceneCut = _flow->SceneCutFlag();

        if (input.depthCount > 0 && input.depthView != DXGI_FORMAT_UNKNOWN)
        {
            in.depthCount = (std::min)(input.depthCount, (int) TrustMaskDx12::Inputs::kMaxDepths);

            for (int i = 0; i < in.depthCount; ++i)
                in.depths[i] = input.depth[i];

            in.depthFormat = input.depthView;
            in.depthWidth = input.depthWidth;
            in.depthHeight = input.depthHeight;
            in.depthReversed = input.depthReversed;
        }

        result.trustRan = _trust->Dispatch(list, in);
        nativeInputs = in;
        if (steps.touched != nullptr)
        {
            Touched& touched = *steps.touched;
            if (result.trustRan)
            {
                if (touched.flowWidth != in.flowWidth || touched.flowHeight != in.flowHeight)
                {
                    touched.flowWidth = in.flowWidth;
                    touched.flowHeight = in.flowHeight;
                    touched.masks = 0;
                }

                if (touched.masks < 2)
                {
                    steps.firstTouch = true;
                    ++touched.masks;
                }
            }
            else
            {
                touched.flowWidth = 0;
                touched.flowHeight = 0;
            }
        }

        if (timer != nullptr)
            timer->Mark(list, "trust", "mask");
        nativeReady = result.trustRan;

        // A hard cut seen by the trust mask's count, a few frames after it: the consumer's history is reset, unless the
        // flow's own flag already asked for that for this cut. With the flow's detector on, the flow and the mask carry
        // on (the cut frame had no motion, and the frames since match the new scene) and the consumer runs on this
        // frame too: skipping it showed one frame without it, a flash over the whole picture. Without the detector, as
        // before: the flow and the mask start over.
        if (_trust->SceneCutSeen() && _frame >= _cutQuietUntil)
        {
            result.sceneCut = true;
            result.distrustedShare = _trust->DistrustedShare();
            nativeReset = true;

            if (_flow->Tuning().sceneCutDetector)
                _cutQuietUntil = _frame + kRing + 2;
            else
            {
                // The consumer is skipped this frame (and the next, which has no flow yet): the reset waits for it.
                Reset();
                nativeReady = false;
                _consumerResetPending = true;
            }
        }
    }

    // Run restores the adapter state here; in-list compute keeps the picture readable through the consumer.
    if (needsBarrier && !inList)
    {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }

    if (nativeReady && options.apply && apply)
    {
        // Enlarging the motion by surface (TrustMask Settings::guideSurface) reads the depth guide: keep it then.
        const bool directDepth =
            inList && !_trust->Tuning().guideSurface && nativeInputs.depthCount == 1 &&
            nativeInputs.depthWidth == input.width && nativeInputs.depthHeight == input.height &&
            (nativeInputs.depthFormat == DXGI_FORMAT_R32_FLOAT || nativeInputs.depthFormat == DXGI_FORMAT_R16_UNORM ||
             nativeInputs.depthFormat == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
             nativeInputs.depthFormat == DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
        const auto motionFormat = inList ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        const bool guides =
            _trust->BuildGuides(list, nativeInputs, input.width, input.height, !directDepth, motionFormat);

        if (timer != nullptr)
            timer->Mark(list, "trust", "guides");

        if (guides)
        {
            NativeFrame frame;
            frame.color = input.picture;
            frame.colorFormat = input.pictureFormat;
            frame.pictureState = inList && needsBarrier ? barrier.Transition.StateAfter : input.pictureState;
            frame.space = input.colorSpace;
            frame.depth = directDepth ? nativeInputs.depths[0] : _trust->GuideDepth();
            frame.motion = _trust->GuideMotion();
            frame.historyDistrust = _trust->Mask();
            frame.depthReversed = true; // the guide is reversed-Z whatever the game's depth is
            frame.reset = nativeReset || _consumerResetPending;
            frame.timer = timer;
            frame.abandonedBefore = steps.touched != nullptr && _consumerAbandonPending;
            steps.consumerCalled = true;
            result.nativeRan = apply(list, frame);

            if (timer != nullptr)
                timer->Mark(list, "consumer", "rest");

            if (result.nativeRan)
            {
                _consumerResetPending = false;
                if (steps.touched != nullptr)
                    _consumerAbandonPending = false;
            }
        }
    }
    if (needsBarrier && inList)
    {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }
}

namespace
{

void LogMixedOnce(bool& logged)
{
#ifndef NDEBUG
    if (!logged)
        OutputDebugStringA(
            "NativeProducer: Run() submitted while frames recorded by RecordInList() were in flight; mixing "
            "the two on one producer is not supported\n");
#endif
    logged = true;
}

std::atomic<uint32_t> g_generations { 0 };

} // namespace

uint32_t NativeProducer::NextGeneration()
{
    uint32_t generation = g_generations.fetch_add(1, std::memory_order_relaxed) + 1;

    if (generation == 0) // wrapped: 0 is no producer's
        generation = g_generations.fetch_add(1, std::memory_order_relaxed) + 1;

    return generation;
}

bool NativeProducer::InListIdle() const
{
    for (const auto& slot : _inList)
    {
        const uint64_t state = slot.tag.load(std::memory_order_acquire) & kStateMask;

        if (state == kFree || state == kAbandoned)
            continue;

        if (state != kSubmitted || slot.fence->GetCompletedValue() < slot.value)
            return false;
    }

    return true;
}

NativeProducer::InListHealth NativeProducer::InListStatus() const
{
    InListHealth health;
    health.busyInARow = _inListBusyRun;
    health.rebuildFailed = _rebuildFailed;
    health.rebuildFailures = _rebuildFailures;
    uint64_t oldest = _inListTicks;

    for (const auto& slot : _inList)
    {
        const uint64_t state = slot.tag.load(std::memory_order_acquire) & kStateMask;

        if (state == kFree || state == kAbandoned ||
            (state == kSubmitted && slot.fence->GetCompletedValue() >= slot.value))
            continue;

        ++health.inFlight;
        oldest = (std::min) (oldest, slot.tick);
    }

    health.oldestAge = health.inFlight > 0 ? _inListTicks - oldest : 0;
    return health;
}

bool NativeProducer::PreviewReady() const
{
    if (!_previewReady)
        return false;

    for (const auto& slot : _inList)
        if (slot.previewFirst && (slot.tag.load(std::memory_order_acquire) & kStateMask) == kAbandoned)
            return false;

    return true;
}

void NativeProducer::CompleteInList(const InListTicket& ticket, ID3D12Fence* fence, uint64_t value)
{
    if (!ticket.valid || ticket.generation != _generation || ticket.slot >= kRing || fence == nullptr)
        return;

    InListSlot& slot = _inList[ticket.slot];
    uint64_t expected = Tag(ticket.frame, kRecorded);

    // Publish the fence only after AddRef; polling must not read it while kCompleting.
    if (!slot.tag.compare_exchange_strong(expected, Tag(ticket.frame, kCompleting), std::memory_order_acquire))
        return;

    fence->AddRef();
    slot.fence = fence;
    slot.value = value;
    slot.tag.store(Tag(ticket.frame, kSubmitted), std::memory_order_release);
}

void NativeProducer::AbandonInList(const InListTicket& ticket)
{
    if (!ticket.valid || ticket.generation != _generation || ticket.slot >= kRing)
        return;

    uint64_t expected = Tag(ticket.frame, kRecorded);
    _inList[ticket.slot].tag.compare_exchange_strong(expected, Tag(ticket.frame, kAbandoned),
                                                     std::memory_order_acq_rel);
}

bool NativeProducer::PollInList(Result& result)
{
    bool idle = true;

    for (uint32_t s = 0; s < kRing; ++s)
    {
        InListSlot& slot = _inList[s];
        const uint64_t tag = slot.tag.load(std::memory_order_acquire);
        const uint64_t state = tag & kStateMask;

        if (state == kFree)
            continue;

        if (state == kAbandoned)
        {
            Reset();
            _consumerResetPending = true;
            result.abandonSeen = true;

            if (slot.consumerRan)
                _consumerAbandonPending = true;

            if (slot.previewFirst)
                _previewReady = false;

            if (slot.firstTouch)
                _inListRebuild = true;
        }
        else if (state == kSubmitted && slot.fence->GetCompletedValue() >= slot.value)
        {
            if (slot.cutPending)
            {
                const SIZE_T offset = (SIZE_T) s * kFlagStride;
                const D3D12_RANGE range { offset, offset + sizeof(uint32_t) };
                uint8_t* data = nullptr;

                if (SUCCEEDED(_inListCutReadback->Map(0, &range, (void**) &data)) && data != nullptr)
                {
                    const bool cut = *(const uint32_t*) (data + offset) != 0;
                    const D3D12_RANGE none { 0, 0 };
                    _inListCutReadback->Unmap(0, &none);

                    if (cut && _frame >= _cutQuietUntil)
                    {
                        result.sceneCut = true;
                        result.distrustedShare = 1.0f;
                        _consumerResetPending = true;
                        _cutQuietUntil = _frame + kRing + 2;
                    }
                }
            }

            _inListDoneMark = (std::max) (_inListDoneMark, (tag >> kStateBits) + 1);
            slot.fence->Release();
            slot.fence = nullptr;
        }
        else
        {
            idle = false;
            continue;
        }

        slot.value = 0;
        slot.cutPending = false;
        slot.firstTouch = false;
        slot.previewFirst = false;
        slot.consumerRan = false;
        slot.tag.store(Tag(tag >> kStateBits, kFree), std::memory_order_release);
    }

    FreeRetired();
    return idle;
}

bool NativeProducer::MakeInListReadback()
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (UINT64) kRing * kFlagStride;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&_inListCutReadback))))
    {
        _inListCutReadback = nullptr;
        _error = "creating the in-list scene-cut readback";
        return false;
    }

    return true;
}

bool NativeProducer::RebuildParts()
{
    std::unique_ptr<OpticalFlowDx12> flow(new (std::nothrow) OpticalFlowDx12());
    std::unique_ptr<TrustMaskDx12> trust(new (std::nothrow) TrustMaskDx12());

    if (flow == nullptr || trust == nullptr)
    {
        _error = "making the flow and the mask again";
        return false;
    }

    if (!flow->Init(_device))
    {
        _error = flow->Error();
        return false;
    }

    if (!trust->Init(_device))
    {
        _error = trust->Error();
        return false;
    }

    flow->Tuning() = _flow->Tuning();
    trust->Tuning() = _trust->Tuning();
    Retire(std::move(_flow), std::move(_trust));
    _flow = std::move(flow);
    _trust = std::move(trust);
    _previewReady = false;
    return true;
}

bool NativeProducer::TryRebuild()
{
    if (!_inListRebuild)
        return true;

    if (_rebuildFailed && _inListTicks < _rebuildRetryAt)
        return false;

    // An unexecuted first transition leaves the GPU in the creation state, not the CPU-tracked resting state.
    if (!RebuildParts())
    {
        _rebuildFailed = true;
        ++_rebuildFailures;
        _rebuildRetryAt = _inListTicks + kRebuildRetry;
        return false;
    }

    _rebuildFailed = false;
    _inListRebuild = false;
    _inListTouched = Touched {};
    _consumerResetPending = true;
    return true;
}

void NativeProducer::Retire(std::unique_ptr<OpticalFlowDx12> flow, std::unique_ptr<TrustMaskDx12> trust)
{
    // Keep replaced textures alive for menu descriptors until later work completes; repeated abandons stay bounded.
    Retired* entry = nullptr;

    for (auto& retired : _retired)
    {
        if (retired.flow == nullptr && retired.trust == nullptr)
        {
            entry = &retired;
            break;
        }

        if (entry == nullptr || retired.frame < entry->frame)
            entry = &retired;
    }

    entry->flow = std::move(flow);
    entry->trust = std::move(trust);
    entry->frame = _frame;
}

void NativeProducer::FreeRetired()
{
    for (auto& retired : _retired)
        if ((retired.flow != nullptr || retired.trust != nullptr) && _inListDoneMark >= retired.frame + 2)
        {
            retired.flow.reset();
            retired.trust.reset();
        }
}

bool NativeProducer::LeaveInList()
{
    ++_inListTicks;

    Result polled;

    if (!PollInList(polled) || !TryRebuild())
        return false;

    if (_inListEntered)
    {
        Reset();
        _consumerResetPending = true;
        _inListEntered = false;
    }

    return true;
}

void NativeProducer::ForgetInList()
{
    bool forgot = false;

    for (auto& slot : _inList)
    {
        uint64_t tag = slot.tag.load(std::memory_order_acquire);
        const uint64_t state = tag & kStateMask;

        if (state == kRecorded)
        {
            if (slot.tag.compare_exchange_strong(tag, Tag(tag >> kStateBits, kAbandoned), std::memory_order_acq_rel))
                forgot = true;
        }
        else if (state == kSubmitted && slot.fence->GetCompletedValue() < slot.value)
        {
            slot.fence->Release();
            slot.fence = nullptr;
            slot.tag.store(Tag(tag >> kStateBits, kAbandoned), std::memory_order_release);
            forgot = true;
        }
    }

    if (forgot)
        _inListRebuild = true;
}

NativeProducer::Result NativeProducer::RecordInList(ID3D12GraphicsCommandList* cmd, const FrameInput& input,
                                                    const Options& options, const ApplyFn& apply, InListTicket& ticket)
{
    Result result;
    ticket = InListTicket {};
    ++_inListTicks;

    if (!Ready() || cmd == nullptr || input.picture == nullptr || cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        result.stoppedAt = !Ready()                   ? "not ready"
                           : cmd == nullptr           ? "no list"
                           : input.picture == nullptr ? "no picture"
                                                      : "not a direct list";
        return result;
    }

    if (_inListCutReadback == nullptr && !MakeInListReadback())
    {
        result.stoppedAt = "cut readback";
        return result;
    }

    const bool idle = PollInList(result);

    if (_signalled != _inListRunMark)
    {
        if (!idle)
            LogMixedOnce(_mixLogged);

        _inListRunMark = _signalled;
        _inListSwitch = true;
    }

    const D3D12_RESOURCE_DESC pictureDesc = input.picture->GetDesc();
    const bool sizeChange =
        input.width != _width || input.height != _height || (uint32_t) pictureDesc.Width != _inListPictureWidth ||
        pictureDesc.Height != _inListPictureHeight || _flow->FinestLevel() != options.flowFinestLevel;
    const bool runDone = _fence == nullptr || _fence->GetCompletedValue() >= _signalled;

    if ((_inListSwitch && !runDone) || ((_inListSwitch || _inListRebuild || sizeChange) && !idle))
    {
        result.stoppedAt = "busy";
        ++_inListBusyRun;
        return result;
    }

    if (_inListSwitch)
    {
        Reset();
        _consumerResetPending = true;
        _inListTouched = Touched {}; // Run() may have made textures again: take every one as fresh
        _inListSwitch = false;
    }

    if (!TryRebuild())
    {
        result.stoppedAt = "rebuild";
        return result;
    }

    if (sizeChange)
    {
        _width = input.width;
        _height = input.height;
        _inListPictureWidth = (uint32_t) pictureDesc.Width;
        _inListPictureHeight = pictureDesc.Height;
        _inListTouched = Touched {};
        Reset();
    }

    if (input.cutHint)
        Reset();

    const uint32_t slot = (uint32_t) (_frame % kRing);
    InListSlot& entry = _inList[slot];

    if ((entry.tag.load(std::memory_order_acquire) & kStateMask) != kFree)
    {
        result.stoppedAt = "busy";
        ++_inListBusyRun;
        return result;
    }

    StageTimerDx12* timer = options.timer != nullptr && options.timer->Recording() ? options.timer : nullptr;
    Recording steps { _inListCutReadback, (UINT64) slot * kFlagStride, &_inListTouched };
    RecordSteps(cmd, input, options, apply, timer, result, steps);

    entry.fence = nullptr;
    entry.value = 0;
    entry.tick = _inListTicks;
    entry.cutPending = steps.cutCopied;
    entry.firstTouch = steps.firstTouch;
    entry.previewFirst = steps.previewFirst;
    entry.consumerRan = steps.consumerCalled;
    entry.tag.store(Tag(_frame, kRecorded), std::memory_order_release);

    ticket.valid = true;
    ticket.slot = slot;
    ticket.frame = _frame;
    ticket.cutCopied = steps.cutCopied;
    ticket.generation = _generation;
    ++_frame;
    _inListBusyRun = 0;
    _inListEntered = true;
    result.submitted = true;
    return result;
}

} // namespace native
