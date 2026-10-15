#include "pch.h"
// vcvars64, then from the repo root (empty pch.h in the test include directory):
// cl /std:c++20 /EHsc /W4 /O2 /I<test-include> tests\nr_native_producer_inlist_gpu.cpp
// OptiScaler\native\NativeProducer.cpp OptiScaler\motion\OpticalFlow_Dx12.cpp
// OptiScaler\motion\SceneCut_Dx12.cpp OptiScaler\motion\TrustMask_Dx12.cpp d3d12.lib dxgi.lib d3dcompiler.lib
// nr_native_producer_inlist_gpu.exe [warp] [no-flow]

#include "d3d12_harness.h"
#include "../OptiScaler/native/NativeProducer.h"
#include <array>
#include <chrono>
#include <thread>

using namespace d3d12_test;
using native::InListTicket;
using native::NativeProducer;

namespace
{
constexpr UINT kWidth = 384, kHeight = 256;
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kBoth = kRead | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
Checks checks;

unsigned recordingCalls = 0;
template <UINT Slot, typename Function> struct CountForward;
template <UINT Slot, typename Result, typename... Args>
struct CountForward<Slot, Result(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, Args...)>
{
    static Result STDMETHODCALLTYPE Call(ID3D12GraphicsCommandList* list, Args... args)
    {
        ++recordingCalls;
        return PrivateForward<Slot, Result(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, Args...)>::Call(list,
                                                                                                           args...);
    }
};

bool ObserveCalls(ID3D12GraphicsCommandList* list)
{
    auto* table = MakePrivateVtable(list, { 9, 10 });
    if (!table)
        return false;
    using L = ID3D12GraphicsCommandList;
    table->entries[14] =
        reinterpret_cast<void*>(CountForward<14, void(STDMETHODCALLTYPE*)(L*, UINT, UINT, UINT)>::Call);
    table->entries[16] = reinterpret_cast<void*>(
        CountForward<16, void(STDMETHODCALLTYPE*)(L*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                                  const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*)>::Call);
    table->entries[17] =
        reinterpret_cast<void*>(CountForward<17, void(STDMETHODCALLTYPE*)(L*, ID3D12Resource*, ID3D12Resource*)>::Call);
    table->entries[21] =
        reinterpret_cast<void*>(CountForward<21, void(STDMETHODCALLTYPE*)(L*, ID3D12PipelineState*)>::Call);
    table->entries[26] = reinterpret_cast<void*>(
        CountForward<26, void(STDMETHODCALLTYPE*)(L*, UINT, const D3D12_RESOURCE_BARRIER*)>::Call);
    table->entries[28] = reinterpret_cast<void*>(
        CountForward<28, void(STDMETHODCALLTYPE*)(L*, UINT, ID3D12DescriptorHeap* const*)>::Call);
    table->entries[29] =
        reinterpret_cast<void*>(CountForward<29, void(STDMETHODCALLTYPE*)(L*, ID3D12RootSignature*)>::Call);
    table->entries[31] = reinterpret_cast<void*>(
        CountForward<31, void(STDMETHODCALLTYPE*)(L*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>::Call);
    table->entries[35] =
        reinterpret_cast<void*>(CountForward<35, void(STDMETHODCALLTYPE*)(L*, UINT, UINT, const void*, UINT)>::Call);
    table->entries[53] = reinterpret_cast<void*>(
        CountForward<53, void(STDMETHODCALLTYPE*)(L*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT)>::Call);
    table->entries[54] =
        reinterpret_cast<void*>(CountForward<54, void(STDMETHODCALLTYPE*)(L*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT,
                                                                          UINT, ID3D12Resource*, UINT64)>::Call);
    return true;
}

void Change(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
            D3D12_RESOURCE_STATES after)
{
    const auto barrier = Transition(resource, before, after);
    list->ResourceBarrier(1, &barrier);
}

struct Shot
{
    ComPtr<ID3D12Resource> colour, depth;
    UINT width = kWidth, height = kHeight;
};

Shot MakeShot(Gpu& gpu, int shift = 0, bool dark = false, UINT width = kWidth, UINT height = kHeight)
{
    Shot shot { {}, {}, width, height };
    for (int part = 0; part < 2; ++part)
    {
        auto texture =
            Texture(gpu.device.Get(), width, height, part ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
                    D3D12_RESOURCE_STATE_COPY_DEST);
        const auto desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 size = 0;
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        auto upload = Buffer(gpu.device.Get(), size, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* data = nullptr;
        if (!checks.Check(Ok(upload->Map(0, nullptr, reinterpret_cast<void**>(&data)), "upload map"), "shot upload"))
            return {};
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                auto* pixel = data + y * footprint.Footprint.RowPitch + x * 4;
                if (part)
                {
                    const float depth = x < width / 2 ? 0.02f : 0.005f;
                    std::memcpy(pixel, &depth, 4);
                }
                else
                {
                    const auto value = static_cast<uint8_t>(dark ? 8 : 96 + (((x + shift) / 8 * 13 + y / 8 * 7) % 64));
                    pixel[0] = pixel[1] = pixel[2] = value;
                    pixel[3] = 255;
                }
            }
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Change(gpu.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               part ? kRead : D3D12_RESOURCE_STATE_COMMON);
        checks.Check(gpu.ExecuteAndWait(), "shot submitted");
        (part ? shot.depth : shot.colour) = texture;
    }
    return shot;
}

native::FrameInput Input(const Shot& shot, bool withDepth = true, bool cutHint = false)
{
    native::FrameInput input;
    input.picture = shot.colour.Get();
    input.pictureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    input.pictureState = D3D12_RESOURCE_STATE_COMMON;
    input.width = shot.width;
    input.height = shot.height;
    input.cutHint = cutHint;
    if (withDepth)
    {
        input.depth[0] = shot.depth.Get();
        input.depthCount = 1;
        input.depthView = DXGI_FORMAT_R32_FLOAT;
        input.depthWidth = shot.width;
        input.depthHeight = shot.height;
        input.depthReversed = true;
    }
    return input;
}

Bytes Read(Gpu& gpu, ID3D12Resource* resource, D3D12_RESOURCE_STATES state, bool xy = false)
{
    Bytes out;
    if (resource)
        checks.Check(gpu.Readback(resource, state, out) && !out.empty(), "output readback");
    if (xy && resource && resource->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        Bytes pairs;
        for (size_t i = 0; i < out.size(); i += 8)
            pairs.insert(pairs.end(), out.begin() + i, out.begin() + i + 4);
        return pairs;
    }
    return out;
}

struct Consumer
{
    ComPtr<ID3D12Resource> out;
    native::NativeFrame last;
    unsigned calls = 0;
    Consumer(Gpu& gpu, DXGI_FORMAT format = DXGI_FORMAT_R16G16_FLOAT)
        : out(Texture(gpu.device.Get(), kWidth, kHeight, format, D3D12_RESOURCE_STATE_COPY_DEST))
    {
    }
    NativeProducer::ApplyFn Fn()
    {
        return [this](ID3D12GraphicsCommandList* list, const native::NativeFrame& frame)
        {
            last = frame;
            ++calls;
            Change(list, frame.motion, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyResource(out.Get(), frame.motion);
            Change(list, frame.motion, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
            if (frame.timer)
                frame.timer->Mark(list, "upscaler", "backend");
            return true;
        };
    }
};

NativeProducer::Result Frame(Gpu& gpu, NativeProducer& producer, const native::FrameInput& input,
                             const NativeProducer::Options& options = {}, const NativeProducer::ApplyFn& apply = {})
{
    InListTicket ticket;
    const auto result = producer.RecordInList(gpu.list.Get(), input, options, apply, ticket);
    checks.Check(result.submitted && ticket.valid, "frame recorded in caller's list");
    checks.Check(gpu.ExecuteAndWait(), "caller submits");
    producer.CompleteInList(ticket, gpu.fence.Get(), gpu.fenceValue);
    return result;
}

void Discard(Gpu& gpu)
{
    checks.Check(Ok(gpu.list->Close(), "discard close") && Ok(gpu.allocator->Reset(), "discard allocator") &&
                     Ok(gpu.list->Reset(gpu.allocator.Get(), nullptr), "discard reset"),
                 "list discarded without execution");
}

void Parity(Gpu& gpu, const Shot& still, const Shot& shifted, const Shot& dark, bool guideSurface)
{
    NativeProducer a, b;
    if (!checks.Check(a.Init(gpu.device.Get()) && b.Init(gpu.device.Get()), "parity producers ready"))
        return;
    a.Trust()->Tuning().guideSurface = b.Trust()->Tuning().guideSurface = guideSurface;
    Consumer ca(gpu, DXGI_FORMAT_R16G16B16A16_FLOAT), cb(gpu);
    NativeProducer::Options options;
    options.apply = options.flowPreview = true;
    unsigned cutsA = 0, cutsB = 0, directFrames = 0, guideFrames = 0;
    bool same = true, outputs = true, resetOnCut = false, maskOnCut = false, directDepth = true;
    const auto colourBefore = Read(gpu, still.colour.Get(), D3D12_RESOURCE_STATE_COMMON);
    for (unsigned i = 0; i < 12; ++i)
    {
        const auto input = Input(i >= 5 ? dark : i % 2 ? shifted : still, i != 3);
        native::FrameOutput done;
        const auto ra = a.Run(gpu.queue.Get(), input, options, ca.Fn(), done);
        checks.Check(gpu.WaitIdle(), "present work finished");
        const auto rb = Frame(gpu, b, input, options, cb.Fn());
        same = same && ra.submitted == rb.submitted && ra.flowValid == rb.flowValid && ra.trustRan == rb.trustRan &&
               ra.nativeRan == rb.nativeRan && ra.sceneCut == rb.sceneCut && ra.distrustedShare == rb.distrustedShare;
        cutsA += ra.sceneCut;
        cutsB += rb.sceneCut;
        if (rb.sceneCut)
            resetOnCut = rb.nativeRan && cb.last.reset;
        if (!rb.flowValid)
            continue;
        const bool direct = !guideSurface && input.depthCount == 1;
        directFrames += direct;
        guideFrames += !direct;
        directDepth = directDepth && cb.last.motion->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT &&
                      (direct ? cb.last.depth == input.depth[0] && b.Trust()->GuideDepth() == nullptr
                              : input.depthCount == 0 ? cb.last.depth == nullptr && b.Trust()->GuideDepth() == nullptr
                                                      : cb.last.depth != nullptr && cb.last.depth == b.Trust()->GuideDepth());
        const std::pair<ID3D12Resource*, ID3D12Resource*> parts[] = { { a.Flow()->Flow(), b.Flow()->Flow() },
                                                                      { a.Trust()->Mask(), b.Trust()->Mask() },
                                                                      { ca.last.depth, cb.last.depth },
                                                                      { a.Trust()->GuideMotion(),
                                                                        b.Trust()->GuideMotion() },
                                                                      { a.Flow()->Preview(), b.Flow()->Preview() },
                                                                      { ca.out.Get(), cb.out.Get() } };
        const D3D12_RESOURCE_STATES states[] = {
            kBoth, kBoth, kRead, kRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST
        };
        for (size_t p = 0; p < std::size(parts); ++p)
            outputs = (Read(gpu, parts[p].first, states[p], p == 3 || p == 5) ==
                       Read(gpu, parts[p].second, states[p], p == 3 || p == 5)) &&
                      outputs;
        if (i == 5)
        {
            const auto mask = Read(gpu, b.Trust()->Mask(), kBoth);
            maskOnCut = !mask.empty() && std::all_of(mask.begin(), mask.end(), [](uint8_t v) { return v == 255; });
        }
    }
    checks.Check(same && ca.calls == cb.calls, "Run and RecordInList: identical results and consumer calls");
    checks.Check(outputs, "flow, mask, guide xy, preview and consumer copy are bit-identical");
    checks.Check(directDepth && guideFrames > 0 && (guideSurface || directFrames > 0),
                 guideSurface ? "in-list: two-channel motion and surface depth guides (default)"
                              : "in-list: direct readable depth without GuideDepth; missing depth stays absent");
    checks.Check(cutsA == 1 && cutsB == 1 && resetOnCut && maskOnCut,
                 "scene cut: immediate distrust, one report and consumer reset");
    checks.Check(colourBefore == Read(gpu, still.colour.Get(), D3D12_RESOURCE_STATE_COMMON),
                 "picture's state and contents preserved");
    checks.Check(b.LeaveInList(), "parity tickets drained");
}

void Tickets(Gpu& gpu, const Shot& shot, bool noFlow)
{
    NativeProducer producer, other;
    if (!checks.Check(producer.Init(gpu.device.Get()) && other.Init(gpu.device.Get()), "ticket producers ready"))
        return;
    const auto input = Input(shot, true, noFlow);
    InListTicket first, foreign;
    auto* before = producer.Flow();
    producer.Flow()->Tuning().radius = 2;
    producer.Trust()->Tuning().decay = 0.25f;
    native::StageTimerDx12 abandonedTimer;
    NativeProducer::Options timed;
    checks.Check(abandonedTimer.Init(gpu.device.Get()), "abandon timer ready");
    abandonedTimer.Begin(gpu.list.Get(), 0);
    timed.timer = &abandonedTimer;
    checks.Check(producer.RecordInList(gpu.list.Get(), input, timed, {}, first).submitted, "fresh frame recorded");
    abandonedTimer.End(gpu.list.Get());
    Discard(gpu);
    checks.Check(other.RecordInList(gpu.list.Get(), input, {}, {}, foreign).submitted,
                 "foreign ticket at same slot/frame");
    Discard(gpu);
    producer.CompleteInList(foreign, gpu.fence.Get(), 0);
    producer.AbandonInList(foreign);
    producer.CompleteInList(first, nullptr, 0);
    checks.Check(!producer.InListIdle(), "foreign and null-fence completions do not settle our ticket");
    producer.AbandonInList(first);
    abandonedTimer.Abandon(0);
    abandonedTimer.Collect(0, 1);
    checks.Check(abandonedTimer.Frames() == 0, "caller abandons timer slot without reading discarded queries");
    other.AbandonInList(foreign);
    const auto recovered = Frame(gpu, producer, input);
    checks.Check(recovered.abandonSeen && !recovered.flowValid && producer.Flow() != before &&
                     producer.Flow()->Tuning().radius == 2 && producer.Trust()->Tuning().decay == 0.25f &&
                     before->FlowWidth() > 0,
                 "first-touch abandon rebuilds, keeps tuning and retires old resources");
    checks.Check(other.LeaveInList(), "foreign producer repairs its own abandoned ticket");
    Consumer consumer(gpu);
    NativeProducer::Options options;
    options.apply = true;
    for (unsigned i = 0; i < 3; ++i)
        Frame(gpu, producer, input, options, consumer.Fn());
    if (!noFlow)
    {
        options.flowPreview = true;
        InListTicket preview;
        const auto r = producer.RecordInList(gpu.list.Get(), input, options, consumer.Fn(), preview);
        checks.Check(r.nativeRan && producer.PreviewReady(), "first preview and consumer work recorded");
        Discard(gpu);
        producer.AbandonInList(preview);
        checks.Check(!producer.PreviewReady(), "abandoned first preview is unavailable before polling");
        const auto reset = Frame(gpu, producer, input, options, consumer.Fn());
        checks.Check(reset.abandonSeen && !reset.flowValid, "abandoned first preview starts new histories");
        Frame(gpu, producer, input, options, consumer.Fn());
        checks.Check(consumer.last.reset && consumer.last.abandonedBefore,
                     "next consumer recreates state after discarded work");
        InListTicket steady;
        producer.RecordInList(gpu.list.Get(), input, options, consumer.Fn(), steady);
        Discard(gpu);
        producer.AbandonInList(steady);
        Frame(gpu, producer, input, options, consumer.Fn());
        Frame(gpu, producer, input, options, consumer.Fn());
        checks.Check(consumer.last.reset && consumer.last.abandonedBefore, "steady abandon also resets the consumer");
    }
    std::array<InListTicket, 3> lost;
    for (auto& ticket : lost)
    {
        checks.Check(producer.RecordInList(gpu.list.Get(), input, {}, {}, ticket).submitted, "lost ticket recorded");
        Discard(gpu);
    }
    InListTicket busy;
    const auto start = std::chrono::steady_clock::now();
    const auto callsBefore = recordingCalls;
    const auto r = producer.RecordInList(gpu.list.Get(), input, {}, {}, busy);
    checks.Check(!r.submitted && !busy.valid && recordingCalls == callsBefore && r.stoppedAt &&
                     std::string_view(r.stoppedAt) == "busy" &&
                     std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250),
                 "full ring returns busy without waiting");
    const auto health = producer.InListStatus();
    checks.Check(health.inFlight == 3 && health.busyInARow == 1 && health.oldestAge > 0 && !producer.LeaveInList(),
                 "lost-ticket health and leave gate");
    checks.Check(gpu.WaitIdle(), "queue drained before forget");
    producer.ForgetInList();
    producer.CompleteInList(lost[0], gpu.fence.Get(), 0);
    checks.Check(producer.LeaveInList(), "forget repairs lost tickets before returning to Run");
    native::FrameOutput done;
    checks.Check(producer.Run(gpu.queue.Get(), input, {}, {}, done).submitted && gpu.WaitIdle(),
                 "present path resumes after Leave");
    const auto switched = Frame(gpu, producer, input);
    checks.Check(!switched.flowValid, "Run to in-list switch restarts histories");
    checks.Check(producer.LeaveInList(), "ticket test drained");
}

void TimingRing(Gpu& gpu, const Shot& shot, const Shot& shifted, const Shot& smaller, bool noFlow)
{
    NativeProducer producer, reference;
    native::StageTimerDx12 timer;
    if (!checks.Check(producer.Init(gpu.device.Get()) && reference.Init(gpu.device.Get()) &&
                          timer.Init(gpu.device.Get()),
                      "timing producer and caller-owned timer ready"))
        return;
    Consumer consumer(gpu);
    NativeProducer::Options options;
    options.apply = true;
    const auto input = Input(shot, true, noFlow);
    Consumer referenceConsumer(gpu, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (unsigned i = 0; i < 4; ++i)
    {
        native::FrameOutput done;
        reference.Run(gpu.queue.Get(), input, options, referenceConsumer.Fn(), done);
        checks.Check(gpu.WaitIdle(), "reference warm-up");
        if (i == 3)
            options.timer = &timer;
        Frame(gpu, producer, input, options, consumer.Fn());
    }
    checks.Check((noFlow || consumer.last.timer == nullptr) && timer.Frames() == 0,
                 "unbegun caller timer records no timestamps");
    ComPtr<ID3D12Fence> gate;
    checks.Check(Ok(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate"),
                 "queue gate ready");
    UINT64 frequency = 0;
    checks.Check(Ok(gpu.queue->GetTimestampFrequency(&frequency), "frequency") && frequency != 0,
                 "caller timestamp frequency");
    options.timer = &timer;
    for (UINT batch = 0; batch < 2; ++batch)
    {
        std::array<ComPtr<ID3D12CommandAllocator>, 3> allocators;
        std::array<ComPtr<ID3D12GraphicsCommandList>, 3> lists;
        std::array<InListTicket, 3> tickets;
        std::array<Consumer, 3> outputs { Consumer(gpu), Consumer(gpu), Consumer(gpu) };
        std::array<Bytes, 3> expected;
        for (UINT s = 0; s < 3; ++s)
        {
            native::FrameOutput done;
            reference.Run(gpu.queue.Get(), Input(s == 1 ? shifted : shot, true, noFlow), options,
                          referenceConsumer.Fn(), done);
            checks.Check(gpu.WaitIdle(), "reference frame finished");
            if (!noFlow)
                expected[s] = Read(gpu, referenceConsumer.out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, true);
        }
        checks.Check(Ok(gpu.queue->Wait(gate.Get(), batch + 1), "queue wait"), "hold queue while recording");
        for (UINT s = 0; s < 3; ++s)
        {
            checks.Check(gpu.CreateList(allocators[s], lists[s]), "independent caller list ready");
            timer.Begin(lists[s].Get(), s);
            timer.Mark(lists[s].Get(), "depth", "copy");
            const auto r = producer.RecordInList(lists[s].Get(), Input(s == 1 ? shifted : shot, true, noFlow), options,
                                                 outputs[s].Fn(), tickets[s]);
            checks.Check(r.submitted && timer.Recording() && r.timings.empty(),
                         "producer appends to timer without owning its lifetime");
            timer.Mark(lists[s].Get(), "output", "copy");
            timer.Mark(lists[s].Get(), "restore", "barriers");
            timer.End(lists[s].Get());
            checks.Check(Ok(lists[s]->Close(), "close"), "timed list closed by caller");
            ID3D12CommandList* commands[] = { lists[s].Get() };
            gpu.queue->ExecuteCommandLists(1, commands);
            const UINT64 value = ++gpu.fenceValue;
            checks.Check(Ok(gpu.queue->Signal(gpu.fence.Get(), value), "signal"), "caller completion fence queued");
            std::thread complete([&producer, &tickets, &gpu, s, value]
                                 { producer.CompleteInList(tickets[s], gpu.fence.Get(), value); });
            complete.join();
            producer.CompleteInList(tickets[s], gate.Get(), UINT64_MAX);
            producer.AbandonInList(tickets[s]);
        }
        checks.Check(tickets[0].frame != tickets[1].frame && tickets[0].slot != tickets[1].slot &&
                         producer.InListStatus().inFlight == 3 && timer.Frames() == batch * 3,
                     "two lists have distinct tickets; timing ring not collected early");
        InListTicket busy;
        const auto next = timer.NextIndex();
        const auto callsBefore = recordingCalls;
        const auto start = std::chrono::steady_clock::now();
        const auto r = producer.RecordInList(gpu.list.Get(), input, options, consumer.Fn(), busy);
        const auto resize = producer.RecordInList(gpu.list.Get(), Input(smaller, true, noFlow), options, {}, busy);
        checks.Check(!r.submitted && !resize.submitted && !busy.valid && timer.NextIndex() == next &&
                         recordingCalls == callsBefore &&
                         std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250),
                     "ring and size change are busy, with no timer writes or CPU wait");
        checks.Check(Ok(gate->Signal(batch + 1), "release gate") && gpu.WaitIdle(),
                     "release queue and reach external fences");
        for (UINT s = 0; s < 3; ++s)
        {
            timer.Collect(s, frequency);
            timer.Collect(s, frequency);
        }
        checks.Check(timer.Frames() == (batch + 1) * 3, "caller collects once per completed timer slot");
        if (!noFlow)
        {
            checks.Check(outputs[2].last.timer == &timer && !producer.PreviewReady(),
                         "consumer shares caller timer, no unrequested preview");
            for (UINT s = 0; s < 3; ++s)
                checks.Check(expected[s] == Read(gpu, outputs[s].out.Get(), D3D12_RESOURCE_STATE_COPY_DEST),
                             "different caller lists preserve their motion output while in flight");
        }
        if (batch == 1)
        {
            ID3D12CommandList* replay[] = { lists[2].Get() };
            gpu.queue->ExecuteCommandLists(1, replay);
            checks.Check(gpu.WaitIdle(), "closed steady list replayed");
        }
        producer.CompleteInList(tickets[2], gpu.fence.Get(), gpu.fenceValue);
        producer.AbandonInList(tickets[2]);
        timer.Collect(2, frequency);
        checks.Check(producer.InListIdle() && timer.Frames() == (batch + 1) * 3,
                     "replay cannot complete or count its ticket twice");
    }
    const auto report = timer.Report();
    std::printf("caller timings: %s\n", report.c_str());
    for (const char* name : { "depth.copy", "flow.start", "flow.luma", "flow.down", "flow.scenecut", "flow.cutcopy",
                              "output.copy", "restore.barriers", "total" })
        checks.Check(report.find(name) != std::string::npos, name);
    if (!noFlow)
        for (const char* name : { "flow.match", "flow.median", "flow.smooth", "flow.global", "trust.mask",
                                  "trust.guides", "upscaler.backend", "consumer.rest" })
            checks.Check(report.find(name) != std::string::npos, name);
    checks.Check(report.find("flow.preview") == std::string::npos && timer.Frames() == 0,
                 "preview absent and report resets counts");
    options.timer = nullptr;
    options.apply = false;
    const auto resized = Frame(gpu, producer, Input(smaller, true, noFlow), options);
    checks.Check(!resized.flowValid && producer.Flow()->FlowWidth() == (smaller.width + 1) / 2,
                 "size change allowed once tickets drain");
    checks.Check(producer.LeaveInList(), "timing tickets drained");
}
} // namespace

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool warp = argc > 1 && std::string_view(argv[1]) == "warp";
    const bool noFlow = warp && argc > 2 && std::string_view(argv[2]) == "no-flow";
    Gpu gpu;
    if (!gpu.Create(warp ? Adapter::Warp : Adapter::Hardware))
        return 1;
    checks.Check(ObserveCalls(gpu.list.Get()), "caller list records are observed");
    // SDK 28000 diagnoses the base's UAV counter initial state; its buffer promotion remains legal.
    D3D12_MESSAGE_ID ignored = D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED;
    D3D12_INFO_QUEUE_FILTER filter {};
    filter.DenyList.NumIDs = 1;
    filter.DenyList.pIDList = &ignored;
    checks.Check(Ok(gpu.info->AddStorageFilterEntries(&filter), "baseline warning filter"),
                 "only baseline buffer-state warning filtered");
    const auto still = MakeShot(gpu), shifted = MakeShot(gpu, 2), dark = MakeShot(gpu, 0, true);
    const auto smaller = MakeShot(gpu, 0, false, 320, 192);
    if (noFlow)
        checks.Skip("WARP flow-dependent parity/scene-cut/consumer cases: baseline 8313dd6c Smooth JIT access "
                    "violation; first-frame tickets/timer still tested");
    else
    {
        Parity(gpu, still, shifted, dark, false);
        Parity(gpu, still, shifted, dark, true);
    }
    Tickets(gpu, still, noFlow);
    TimingRing(gpu, still, shifted, smaller, noFlow);
    checks.Check(gpu.WaitIdle() && gpu.Messages() == 0, "debug layer: zero new errors or warnings");
    return checks.Summary();
}
