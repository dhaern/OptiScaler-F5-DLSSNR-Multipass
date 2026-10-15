#pragma once

// GPU timestamps between the steps of one command list, read back once the GPU has finished it, summed and reported as
// the time per frame of each step. The native input producer uses it ([DlssNr] NativeStageTimings) to show where its
// GPU time goes: the optical flow's passes, the trust mask, the guides and the consumer (the upscaler backend and its
// copies). No game headers: NativeProducer.cpp, built without the precompiled header, includes it.
//
// Each ring slot has its own range of queries and of the readback buffer. Begin() starts a slot's frame with a first
// timestamp, Mark() records one after the work recorded so far and names the step that ends there, End() resolves the
// slot's timestamps into the readback. Collect(), called once the GPU has finished that slot's frame, adds each step's
// time (its timestamp minus the one before it) to the step's sum; a step named several times in one frame (the flow's
// match, once per level) counts once per frame, with its parts added up. A timestamp lands when the GPU gets to it, so
// with no barrier between two steps they can overlap and the split between them is approximate; the total of a frame
// (last timestamp minus first) is exact.

#include <d3d12.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace native
{

class StageTimerDx12
{
  public:
    static constexpr uint32_t kRing = 3;   // as many as the producer's ring
    static constexpr uint32_t kMarks = 96; // timestamps per frame, the first one included

    StageTimerDx12() = default;
    ~StageTimerDx12() { Release(); }

    StageTimerDx12(const StageTimerDx12&) = delete;
    StageTimerDx12& operator=(const StageTimerDx12&) = delete;

    bool Init(ID3D12Device* device)
    {
        Release();

        if (device == nullptr)
            return false;

        D3D12_QUERY_HEAP_DESC heapDesc {};
        heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heapDesc.Count = kRing * kMarks;

        if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&_heap))))
        {
            Release();
            return false;
        }

        D3D12_HEAP_PROPERTIES props {};
        props.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = (UINT64) kRing * kMarks * sizeof(uint64_t);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&_readback))))
        {
            Release();
            return false;
        }

        return true;
    }

    void Release()
    {
        if (_heap != nullptr)
            _heap->Release();
        if (_readback != nullptr)
            _readback->Release();

        _heap = nullptr;
        _readback = nullptr;
        _recording = false;

        for (auto& resolved : _resolved)
            resolved = false;
    }

    bool Ready() const { return _heap != nullptr && _readback != nullptr; }
    bool Recording() const { return _recording; }

    // For a part that records its own timestamps (OpticalFlowDx12::SetTimestampHeap): the heap, the index of the next
    // free entry and how many are left in this slot.
    ID3D12QueryHeap* Heap() const { return _heap; }
    uint32_t NextIndex() const { return _slot * kMarks + _count[_slot]; }
    uint32_t Room() const { return _recording ? kMarks - _count[_slot] : 0; }

    void Begin(ID3D12GraphicsCommandList* list, uint32_t slot)
    {
        _slot = slot % kRing;
        _count[_slot] = 0;
        _resolved[_slot] = false;
        _recording = Ready() && list != nullptr;
        Mark(list, "", "start");
    }

    // A timestamp after everything recorded so far; the step that ends here is group.name.
    void Mark(ID3D12GraphicsCommandList* list, const char* group, const char* name)
    {
        if (!_recording || list == nullptr || _count[_slot] >= kMarks)
            return;

        list->EndQuery(_heap, D3D12_QUERY_TYPE_TIMESTAMP, NextIndex());
        _groups[_slot][_count[_slot]] = group;
        _names[_slot][_count[_slot]] = name;
        ++_count[_slot];
    }

    // The part's own timestamps were written from NextIndex() on: they become this frame's next steps.
    template <class NameOf> void Adopt(uint32_t count, const char* group, NameOf nameOf)
    {
        if (!_recording)
            return;

        for (uint32_t i = 0; i < count && _count[_slot] < kMarks; ++i)
        {
            _groups[_slot][_count[_slot]] = group;
            _names[_slot][_count[_slot]] = nameOf(i);
            ++_count[_slot];
        }
    }

    void End(ID3D12GraphicsCommandList* list)
    {
        if (!_recording)
            return;

        _recording = false;

        if (_count[_slot] < 2 || list == nullptr)
            return;

        list->ResolveQueryData(_heap, D3D12_QUERY_TYPE_TIMESTAMP, _slot * kMarks, _count[_slot], _readback,
                               (UINT64) _slot * kMarks * sizeof(uint64_t));
        _resolved[_slot] = true;
    }

    // The list was never submitted: its timestamps are not there to read.
    void Abandon(uint32_t slot)
    {
        _recording = false;
        _resolved[slot % kRing] = false;
    }

    // Once the GPU has finished the slot's frame. `frequency` is the queue's
    // (ID3D12CommandQueue::GetTimestampFrequency).
    void Collect(uint32_t slot, uint64_t frequency)
    {
        slot %= kRing;

        if (!_resolved[slot] || frequency == 0 || !Ready())
            return;

        _resolved[slot] = false;
        const uint32_t count = _count[slot];
        const D3D12_RANGE range { (SIZE_T) slot * kMarks * sizeof(uint64_t),
                                  (SIZE_T) (slot * kMarks + count) * sizeof(uint64_t) };
        uint64_t* data = nullptr;

        if (FAILED(_readback->Map(0, &range, (void**) &data)) || data == nullptr)
            return;

        const uint64_t* stamps = data + (size_t) slot * kMarks;
        bool ordered = true;

        for (uint32_t i = 1; i < count && ordered; ++i)
            ordered = stamps[i] >= stamps[i - 1];

        if (ordered)
        {
            const double toMs = 1000.0 / (double) frequency;

            for (auto& stage : _stages)
                stage.frameMs = -1.0;

            for (uint32_t i = 1; i < count; ++i)
            {
                Stage& stage = Find(_groups[slot][i], _names[slot][i]);
                stage.frameMs =
                    (stage.frameMs < 0.0 ? 0.0 : stage.frameMs) + (double) (stamps[i] - stamps[i - 1]) * toMs;
            }

            Stage& total = Find("", "total");
            total.frameMs = (double) (stamps[count - 1] - stamps[0]) * toMs;

            for (auto& stage : _stages)
            {
                if (stage.frameMs < 0.0)
                    continue;

                stage.sumMs += stage.frameMs;
                stage.maxMs = stage.frameMs > stage.maxMs ? stage.frameMs : stage.maxMs;
                ++stage.frames;
            }

            ++_frames;
        }

        const D3D12_RANGE none { 0, 0 };
        _readback->Unmap(0, &none);
    }

    uint32_t Frames() const { return _frames; }

    // "group.name avg/max" for every step seen since the last report, in the order they were first seen, the frame's
    // total last; then starts over. A step's average is over the frames it ran in.
    std::string Report()
    {
        std::string out;
        char line[160];

        for (int pass = 0; pass < 2; ++pass)
            for (const auto& stage : _stages)
            {
                const bool isTotal = stage.key == "total";

                if (stage.frames == 0 || isTotal != (pass == 1))
                    continue;

                std::snprintf(line, sizeof(line), "%s%s %.3f/%.3f", out.empty() ? "" : " | ", stage.key.c_str(),
                              stage.sumMs / stage.frames, stage.maxMs);
                out += line;
            }

        for (auto& stage : _stages)
        {
            stage.sumMs = 0.0;
            stage.maxMs = 0.0;
            stage.frames = 0;
        }

        _frames = 0;
        return out;
    }

  private:
    struct Stage
    {
        std::string key;
        double sumMs = 0.0;
        double maxMs = 0.0;
        double frameMs = -1.0; // this frame's time while collecting; < 0: not in this frame
        uint32_t frames = 0;
    };

    Stage& Find(const char* group, const char* name)
    {
        std::string key = group != nullptr && group[0] != '\0' ? std::string(group) + "." : std::string();
        key += name != nullptr && name[0] != '\0' ? name : "?";

        for (auto& stage : _stages)
            if (stage.key == key)
                return stage;

        _stages.push_back(Stage {});
        _stages.back().key = std::move(key);
        return _stages.back();
    }

    ID3D12QueryHeap* _heap = nullptr;
    ID3D12Resource* _readback = nullptr;
    bool _recording = false;
    uint32_t _slot = 0;
    uint32_t _count[kRing] = {};
    bool _resolved[kRing] = {};
    const char* _groups[kRing][kMarks] = {};
    const char* _names[kRing][kMarks] = {};
    std::vector<Stage> _stages;
    uint32_t _frames = 0;
};

} // namespace native
