#pragma once
#include <d3d12.h>
#include <windows.h>
#include <new>
#include "DescriptorTrackerDx12.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

namespace native
{

struct ListState
{
    static constexpr int kMaxRoot = 64; // root parameters tracked per signature (D3D12 allows up to 64 DWORDs)

    uint32_t generation = 0;                    // bumped by Begin(); lets callers notice a Reset
    spatial_tracking::Device* device = nullptr; // cold creation/first Reset association, preserved across Reset
    bool forwarding = false;
    bool inEnvelope = false;           // true while we record our own work: our calls are not tracked
    ID3D12DescriptorHeap* heaps[2] {}; // as last set (CBV_SRV_UAV, SAMPLER), count in heapCount
    UINT heapCount = 0;
    descriptors::HeapRef heapRefs[2] {};
    ID3D12PipelineState* pso = nullptr;
    ID3D12RootSignature* graphicsSignature = nullptr;
    ID3D12RootSignature* computeSignature = nullptr;

    uint64_t graphicsTableMask = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE graphicsTables[kMaxRoot] {};
    uint64_t graphicsCbvMask = 0, graphicsSrvMask = 0, graphicsUavMask = 0;
    D3D12_GPU_VIRTUAL_ADDRESS graphicsCbv[kMaxRoot] {}, graphicsSrv[kMaxRoot] {}, graphicsUav[kMaxRoot] {};

    uint64_t computeTableMask = 0, computeCbvMask = 0, computeSrvMask = 0, computeUavMask = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE computeTables[kMaxRoot] {};
    D3D12_GPU_VIRTUAL_ADDRESS computeCbv[kMaxRoot] {}, computeSrv[kMaxRoot] {}, computeUav[kMaxRoot] {};

    static constexpr int kMaxConstants = 64; // dwords per parameter, and in all
    uint64_t computeConstantParamMask = 0;
    // Root constants cost one DWORD each: a signature has at most 64 written (parameter, offset) pairs.
    uint32_t computeConstants[kMaxConstants] {};
    uint64_t computeConstantDwordMask[kMaxRoot] {};
    uint32_t computeConstantCount = 0;

    uint64_t graphicsConstantParamMask = 0;
    uint32_t graphicsConstants[kMaxConstants] {};
    uint64_t graphicsConstantDwordMask[kMaxRoot] {};
    uint32_t graphicsConstantCount = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 {}; // first render target as last set (ptr 0 = none)
    bool rtv0Valid = false;
    D3D12_VIEWPORT viewport0 {};
    bool viewport0Valid = false;
    UINT viewportCount = 0;
    bool dsvValid = false;  // a depth-stencil view bound by OMSetRenderTargets or BeginRenderPass
    D3D12_RECT scissor0 {}; // first scissor rectangle as last set
    bool scissor0Valid = false;
    bool predicated = false; // SetPredication with a buffer since Begin, not cleared by one with none since

    struct Transition
    {
        ID3D12Resource* resource = nullptr;
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        D3D12_RESOURCE_STATES after = D3D12_RESOURCE_STATE_COMMON;
        bool enhanced = false;
        bool pending = false;
    } transitions[8] {};
    uint32_t transitionCount = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE renderPassRtv {};
    UINT renderTargetCount = 0;
    bool inRenderPass = false;
    uint32_t barrierCount = 0; // transitions seen since Begin

    bool incomplete = false;

    uintptr_t user[8] {}; // scratch for the caller (per-list counters and hints of the hook layer); cleared by Begin
};

namespace liststate_detail
{
inline uint64_t Bit(UINT index) { return uint64_t { 1 } << index; }

inline uint64_t Bits(UINT first, UINT count)
{
    return count >= 64 ? ~uint64_t { 0 } : ((uint64_t { 1 } << count) - 1) << first;
}

inline uint32_t ConstantBase(const ListState& s, UINT index, bool graphics = false)
{
    uint32_t base = 0;
    uint64_t below = (graphics ? s.graphicsConstantParamMask : s.computeConstantParamMask) & (Bit(index) - 1);

    while (below != 0)
    {
        base += (uint32_t) std::popcount(
            (graphics ? s.graphicsConstantDwordMask : s.computeConstantDwordMask)[std::countr_zero(below)]);
        below &= below - 1;
    }

    return base;
}

inline void ClearGraphicsArguments(ListState& s)
{
    s.graphicsTableMask = s.graphicsCbvMask = s.graphicsSrvMask = s.graphicsUavMask = 0;

    for (uint64_t m = s.graphicsConstantParamMask; m != 0; m &= m - 1)
        s.graphicsConstantDwordMask[std::countr_zero(m)] = 0;

    s.graphicsConstantParamMask = 0;
    s.graphicsConstantCount = 0;
}

inline void ClearComputeArguments(ListState& s)
{
    s.computeTableMask = s.computeCbvMask = s.computeSrvMask = s.computeUavMask = 0;

    for (uint64_t m = s.computeConstantParamMask; m != 0; m &= m - 1)
        s.computeConstantDwordMask[std::countr_zero(m)] = 0;

    s.computeConstantParamMask = 0;
    s.computeConstantCount = 0;
}

inline void ClearBindings(ListState& s, ID3D12PipelineState* pso)
{
    s.heaps[0] = s.heaps[1] = nullptr;
    s.heapCount = 0;
    s.heapRefs[0] = s.heapRefs[1] = {};
    s.pso = pso;
    s.graphicsSignature = nullptr;
    s.computeSignature = nullptr;
    ClearGraphicsArguments(s);
    ClearComputeArguments(s);
    s.rtv0 = {};
    s.renderPassRtv = {};
    s.renderTargetCount = 0;
    s.inRenderPass = false;
    s.rtv0Valid = false;
    s.viewport0 = {};
    s.viewport0Valid = false;
    s.viewportCount = 0;
    s.dsvValid = false;
    s.scissor0 = {};
    s.scissor0Valid = false;
}

inline void ClearAll(ListState& s, ID3D12PipelineState* pso)
{
    ClearBindings(s, pso);
    s.inEnvelope = false;
    s.forwarding = false;
    s.transitionCount = 0;
    s.barrierCount = 0;
    s.incomplete = false;
    s.predicated = false;
    std::memset(s.user, 0, sizeof(s.user));
}
} // namespace liststate_detail

// One thread records each list. Find only publishes the entry; reading another thread's live fields is not safe.
class ListStateTable
{
    struct Entry
    {
        std::atomic<const void*> key {};
        Entry* next = nullptr;
        ListState state;
    };
    static constexpr size_t kStride = (sizeof(Entry) + 4095) & ~size_t { 4095 };
    static constexpr size_t kBytes = 64 * 1024 * 1024;
    static uint32_t Home(const void* list)
    {
        return uint32_t((uint64_t(uintptr_t(list)) * 0x9E3779B97F4A7C15ull) >> 54);
    }
    static const void* Tombstone() { return reinterpret_cast<const void*>(uintptr_t { 1 }); }
    Entry* EntryOf(const void* list) const
    {
        if (!list || list == Tombstone())
            return nullptr;
        for (Entry* e = _buckets[Home(list)].load(std::memory_order_acquire); e; e = e->next)
            if (e->key.load(std::memory_order_acquire) == list)
                return e;
        return nullptr;
    }

  public:
    ListState* Find(const void* list)
    {
        Entry* e = EntryOf(list);
        return e ? &e->state : nullptr;
    }
    ListState* Acquire(const void* list)
    {
        if (!list || list == Tombstone())
            return nullptr;
        if (ListState* s = Find(list))
            return s;
        auto& bucket = _buckets[Home(list)];
        for (Entry* e = bucket.load(std::memory_order_acquire); e; e = e->next)
        {
            const void* key = Tombstone();
            if (!e->key.compare_exchange_strong(key, reinterpret_cast<const void*>(uintptr_t { 2 }),
                                                std::memory_order_acq_rel))
                continue;
            liststate_detail::ClearAll(e->state, nullptr);
            ++e->state.generation;
            e->key.store(list, std::memory_order_release);
            _used.fetch_add(1, std::memory_order_relaxed);
            return &e->state;
        }
        auto* slab = _slab.load(std::memory_order_acquire);
        if (!slab)
        {
            auto* reserved = static_cast<unsigned char*>(VirtualAlloc(nullptr, kBytes, MEM_RESERVE, PAGE_READWRITE));
            if (!reserved)
                return Miss();
            if (!_slab.compare_exchange_strong(slab, reserved, std::memory_order_acq_rel))
                VirtualFree(reserved, 0, MEM_RELEASE);
            else
                slab = reserved;
        }
        const size_t at = _next.fetch_add(kStride, std::memory_order_relaxed);
        if (at >= kBytes || !VirtualAlloc(slab + at, kStride, MEM_COMMIT, PAGE_READWRITE))
            return Miss();
        auto* e = new (slab + at) Entry {};
        e->state.generation = 1;
        e->key.store(list, std::memory_order_relaxed);
        Entry* head = bucket.load(std::memory_order_relaxed);
        do
        {
            e->next = head;
        } while (!bucket.compare_exchange_weak(head, e, std::memory_order_release, std::memory_order_relaxed));
        _used.fetch_add(1, std::memory_order_relaxed);
        return &e->state;
    }
    void Begin(const void* list, ID3D12PipelineState* pso = nullptr)
    {
        if (ListState* s = Acquire(list))
        {
            liststate_detail::ClearAll(*s, pso);
            ++s->generation;
        }
    }
    void Forget(const void* list)
    {
        if (Entry* e = EntryOf(list))
        {
            ++e->state.generation;
            e->key.store(Tombstone(), std::memory_order_release);
            _used.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    int Used() const { return _used.load(std::memory_order_relaxed); }
    uint32_t TakeUntracked() { return _untracked.exchange(0, std::memory_order_relaxed); }

  private:
    ListState* Miss()
    {
        _untracked.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    std::atomic<Entry*> _buckets[1024] {};
    std::atomic<unsigned char*> _slab {};
    std::atomic<size_t> _next {};
    std::atomic<int> _used {};
    std::atomic<uint32_t> _untracked {};
};

struct ListRestoreSink
{
    virtual void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps) = 0;
    virtual void SetGraphicsRootSignature(ID3D12RootSignature* signature) = 0;
    virtual void SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) = 0;
    virtual void SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) = 0;
    virtual void SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) = 0;
    virtual void SetGraphicsRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) = 0;
    virtual void SetGraphicsRoot32BitConstants(UINT index, UINT count, const void* data, UINT offset) = 0;
    virtual void SetComputeRootSignature(ID3D12RootSignature* signature) = 0;
    virtual void SetComputeRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) = 0;
    virtual void SetComputeRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) = 0;
    virtual void SetComputeRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) = 0;
    virtual void SetComputeRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) = 0;
    virtual void SetComputeRoot32BitConstants(UINT index, UINT count, const void* data, UINT offset) = 0;
    virtual void SetPipelineState(ID3D12PipelineState* pso) = 0;
    virtual ~ListRestoreSink() = default;
};

// Heaps invalidate tables; each signature precedes its arguments. Restore both sides before the PSO.
inline void EmitRestore(const ListState& s, ListRestoreSink& sink)
{
    const bool tables = s.heapCount > 0;

    if (tables)
        sink.SetDescriptorHeaps(s.heapCount, s.heaps);

    if (s.graphicsSignature != nullptr)
    {
        sink.SetGraphicsRootSignature(s.graphicsSignature);

        for (uint64_t m = tables ? s.graphicsTableMask : 0; m != 0; m &= m - 1)
        {
            const UINT i = (UINT) std::countr_zero(m);
            sink.SetGraphicsRootDescriptorTable(i, s.graphicsTables[i]);
        }

        for (uint64_t m = s.graphicsCbvMask; m != 0; m &= m - 1)
        {
            const UINT i = (UINT) std::countr_zero(m);
            sink.SetGraphicsRootConstantBufferView(i, s.graphicsCbv[i]);
        }
        for (uint64_t m = s.graphicsSrvMask; m != 0; m &= m - 1)
        {
            const UINT i = (UINT) std::countr_zero(m);
            sink.SetGraphicsRootShaderResourceView(i, s.graphicsSrv[i]);
        }

        for (uint64_t m = s.graphicsUavMask; m != 0; m &= m - 1)
        {
            const UINT i = (UINT) std::countr_zero(m);
            sink.SetGraphicsRootUnorderedAccessView(i, s.graphicsUav[i]);
        }

        uint32_t at = 0;

        for (uint64_t p = s.graphicsConstantParamMask; p != 0; p &= p - 1)
        {
            const UINT index = (UINT) std::countr_zero(p);

            for (uint64_t d = s.graphicsConstantDwordMask[index]; d != 0;)
            {
                const UINT first = (UINT) std::countr_zero(d);
                const UINT run = (UINT) std::countr_one(d >> first);
                sink.SetGraphicsRoot32BitConstants(index, run, &s.graphicsConstants[at], first);
                at += run;
                d &= ~liststate_detail::Bits(first, run);
            }
        }
    }

    if (s.computeSignature != nullptr)
    {
        sink.SetComputeRootSignature(s.computeSignature);

        for (uint64_t m = tables ? s.computeTableMask : 0; m != 0; m &= m - 1)
        {
            const UINT i = (UINT) std::countr_zero(m);
            sink.SetComputeRootDescriptorTable(i, s.computeTables[i]);
        }

        for (uint64_t m = s.computeCbvMask; m != 0; m &= m - 1)
        {
            const UINT i = (UINT) std::countr_zero(m);
            sink.SetComputeRootConstantBufferView(i, s.computeCbv[i]);
        }

        for (uint64_t m = s.computeSrvMask; m != 0; m &= m - 1)
        {
            const UINT i = (UINT) std::countr_zero(m);
            sink.SetComputeRootShaderResourceView(i, s.computeSrv[i]);
        }

        for (uint64_t m = s.computeUavMask; m != 0; m &= m - 1)
        {
            const UINT i = (UINT) std::countr_zero(m);
            sink.SetComputeRootUnorderedAccessView(i, s.computeUav[i]);
        }

        uint32_t at = 0;

        for (uint64_t p = s.computeConstantParamMask; p != 0; p &= p - 1)
        {
            const UINT index = (UINT) std::countr_zero(p);

            for (uint64_t d = s.computeConstantDwordMask[index]; d != 0;)
            {
                const UINT first = (UINT) std::countr_zero(d);
                const UINT run = (UINT) std::countr_one(d >> first);
                sink.SetComputeRoot32BitConstants(index, run, &s.computeConstants[at], first);
                at += run;
                d &= ~liststate_detail::Bits(first, run);
            }
        }
    }

    if (s.pso != nullptr)
        sink.SetPipelineState(s.pso);
}

class ListRestoreToList final : public ListRestoreSink
{
  public:
    explicit ListRestoreToList(ID3D12GraphicsCommandList* list) : _list(list) {}

    void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps) override
    {
        _list->SetDescriptorHeaps(count, heaps);
    }
    void SetGraphicsRootSignature(ID3D12RootSignature* signature) override
    {
        _list->SetGraphicsRootSignature(signature);
    }
    void SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) override
    {
        _list->SetGraphicsRootDescriptorTable(index, table);
    }
    void SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) override
    {
        _list->SetGraphicsRootConstantBufferView(index, address);
    }
    void SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) override
    {
        _list->SetGraphicsRootShaderResourceView(index, address);
    }
    void SetGraphicsRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) override
    {
        _list->SetGraphicsRootUnorderedAccessView(index, address);
    }
    void SetGraphicsRoot32BitConstants(UINT index, UINT count, const void* data, UINT offset) override
    {
        _list->SetGraphicsRoot32BitConstants(index, count, data, offset);
    }
    void SetComputeRootSignature(ID3D12RootSignature* signature) override { _list->SetComputeRootSignature(signature); }
    void SetComputeRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) override
    {
        _list->SetComputeRootDescriptorTable(index, table);
    }
    void SetComputeRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) override
    {
        _list->SetComputeRootConstantBufferView(index, address);
    }
    void SetComputeRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) override
    {
        _list->SetComputeRootShaderResourceView(index, address);
    }
    void SetComputeRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) override
    {
        _list->SetComputeRootUnorderedAccessView(index, address);
    }
    void SetComputeRoot32BitConstants(UINT index, UINT count, const void* data, UINT offset) override
    {
        _list->SetComputeRoot32BitConstants(index, count, data, offset);
    }
    void SetPipelineState(ID3D12PipelineState* pso) override { _list->SetPipelineState(pso); }

  private:
    ID3D12GraphicsCommandList* _list;
};

// The same heaps, even in another order, preserve tables; changing heaps invalidates both sides.
inline void OnSetDescriptorHeaps(ListState& s, UINT count, ID3D12DescriptorHeap* const* heaps)
{
    if (s.inEnvelope)
        return;

    if (heaps == nullptr)
        count = 0;

    if (count > 2)
    {
        s.incomplete = true; // at most one heap per shader-visible type exists
        count = 2;
    }

    auto contains = [](ID3D12DescriptorHeap* const* set, UINT n, ID3D12DescriptorHeap* heap)
    { return (n > 0 && set[0] == heap) || (n > 1 && set[1] == heap); };

    bool same = count == s.heapCount;

    for (UINT i = 0; i < count && same; ++i)
        same = contains(s.heaps, s.heapCount, heaps[i]) && contains(heaps, count, s.heaps[i]);

    if (same)
        return;

    s.heaps[0] = count > 0 ? heaps[0] : nullptr;
    s.heaps[1] = count > 1 ? heaps[1] : nullptr;
    s.heapCount = count;
    for (UINT i = 0; i < 2; ++i)
    {
        s.heapRefs[i] = descriptors::FindHeap(s.heaps[i], s.device);
        if (!s.heapRefs[i].heap)
            s.heapRefs[i] = descriptors::ResolveBoundHeap(s.heaps[i], s.device);
    }
    s.graphicsTableMask = 0;
    s.computeTableMask = 0;
}

inline void OnSetPipelineState(ListState& s, ID3D12PipelineState* pso)
{
    if (!s.inEnvelope)
        s.pso = pso;
}

inline void OnSetGraphicsRootSignature(ListState& s, ID3D12RootSignature* signature)
{
    if (s.inEnvelope || signature == s.graphicsSignature)
        return;

    s.graphicsSignature = signature;
    liststate_detail::ClearGraphicsArguments(s);
}

inline void OnSetComputeRootSignature(ListState& s, ID3D12RootSignature* signature)
{
    if (s.inEnvelope || signature == s.computeSignature)
        return;

    s.computeSignature = signature;
    liststate_detail::ClearComputeArguments(s);
}

namespace liststate_detail
{
template <class T> inline void SetArgument(ListState& s, UINT index, T value, T* values, uint64_t& mask)
{
    if (s.inEnvelope)
        return;
    if (index >= (UINT) ListState::kMaxRoot)
    {
        s.incomplete = true;
        return;
    }
    values[index] = value;
    mask |= Bit(index);
}

inline void SetConstants(ListState& s, UINT index, UINT count, const void* data, UINT offset, bool graphics)
{
    using namespace liststate_detail;

    if (s.inEnvelope || count == 0)
        return;

    if (index >= (UINT) ListState::kMaxRoot || data == nullptr || offset >= (UINT) ListState::kMaxConstants)
    {
        s.incomplete = true;
        return;
    }

    if (count > (UINT) ListState::kMaxConstants - offset)
    {
        s.incomplete = true;
        count = (UINT) ListState::kMaxConstants - offset;
    }

    const auto* bytes = static_cast<const unsigned char*>(data); // the game's pointer need not be 4-byte aligned
    auto* values = graphics ? s.graphicsConstants : s.computeConstants;
    auto* masks = graphics ? s.graphicsConstantDwordMask : s.computeConstantDwordMask;
    auto& total = graphics ? s.graphicsConstantCount : s.computeConstantCount;
    auto& params = graphics ? s.graphicsConstantParamMask : s.computeConstantParamMask;
    uint64_t& mask = masks[index];
    const uint32_t base = ConstantBase(s, index, graphics);
    const uint64_t wanted = Bits(offset, count);

    if ((mask & wanted) == wanted)
    {
        std::memcpy(&values[base + std::popcount(mask & (Bit(offset) - 1))], bytes, count * 4u);
        return;
    }

    for (UINT j = 0; j < count; ++j)
    {
        const UINT dword = offset + j;
        const uint32_t at = base + (uint32_t) std::popcount(mask & (Bit(dword) - 1));

        if ((mask & Bit(dword)) == 0)
        {
            if (total >= (uint32_t) ListState::kMaxConstants)
            {
                s.incomplete = true;
                continue;
            }

            std::memmove(&values[at + 1], &values[at], (total - at) * sizeof(uint32_t));
            ++total;
            mask |= Bit(dword);
            params |= Bit(index);
        }

        std::memcpy(&values[at], bytes + j * 4u, 4);
    }
}

inline bool ReadConstant(const ListState& s, UINT index, UINT offset, uint32_t* value, bool graphics)
{
    using namespace liststate_detail;

    if (value == nullptr || index >= (UINT) ListState::kMaxRoot || offset >= (UINT) ListState::kMaxConstants)
        return false;

    const uint64_t mask = (graphics ? s.graphicsConstantDwordMask : s.computeConstantDwordMask)[index];

    if ((mask & Bit(offset)) == 0)
        return false;

    *value =
        (graphics ? s.graphicsConstants
                  : s.computeConstants)[ConstantBase(s, index, graphics) + std::popcount(mask & (Bit(offset) - 1))];
    return true;
}

} // namespace liststate_detail

inline void OnSetGraphicsRootDescriptorTable(ListState& s, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table)
{
    liststate_detail::SetArgument(s, index, table, s.graphicsTables, s.graphicsTableMask);
}

inline void OnSetGraphicsRootConstantBufferView(ListState& s, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    liststate_detail::SetArgument(s, index, address, s.graphicsCbv, s.graphicsCbvMask);
}

inline void OnSetGraphicsRootShaderResourceView(ListState& s, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    liststate_detail::SetArgument(s, index, address, s.graphicsSrv, s.graphicsSrvMask);
}

inline void OnSetGraphicsRootUnorderedAccessView(ListState& s, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    liststate_detail::SetArgument(s, index, address, s.graphicsUav, s.graphicsUavMask);
}

inline void OnSetGraphicsRoot32BitConstants(ListState& s, UINT index, UINT count, const void* data, UINT offset)
{
    liststate_detail::SetConstants(s, index, count, data, offset, true);
}
inline void OnSetGraphicsRoot32BitConstant(ListState& s, UINT index, UINT value, UINT offset)
{
    OnSetGraphicsRoot32BitConstants(s, index, 1, &value, offset);
}
inline bool ReadGraphicsConstant(const ListState& s, UINT index, UINT offset, uint32_t* value)
{
    return liststate_detail::ReadConstant(s, index, offset, value, true);
}

inline void OnSetComputeRootDescriptorTable(ListState& s, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table)
{
    liststate_detail::SetArgument(s, index, table, s.computeTables, s.computeTableMask);
}

inline void OnSetComputeRootConstantBufferView(ListState& s, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    liststate_detail::SetArgument(s, index, address, s.computeCbv, s.computeCbvMask);
}

inline void OnSetComputeRootShaderResourceView(ListState& s, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    liststate_detail::SetArgument(s, index, address, s.computeSrv, s.computeSrvMask);
}

inline void OnSetComputeRootUnorderedAccessView(ListState& s, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    liststate_detail::SetArgument(s, index, address, s.computeUav, s.computeUavMask);
}

inline void OnSetComputeRoot32BitConstants(ListState& s, UINT index, UINT count, const void* data, UINT offset)
{
    liststate_detail::SetConstants(s, index, count, data, offset, false);
}
inline void OnSetComputeRoot32BitConstant(ListState& s, UINT index, UINT value, UINT offset)
{
    OnSetComputeRoot32BitConstants(s, index, 1, &value, offset);
}
inline bool ReadComputeConstant(const ListState& s, UINT index, UINT offset, uint32_t* value)
{
    return liststate_detail::ReadConstant(s, index, offset, value, false);
}

inline void OnOMSetRenderTargets(ListState& s, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleRange,
                                 const D3D12_CPU_DESCRIPTOR_HANDLE* dsv = nullptr)
{
    (void) singleRange;

    if (s.inEnvelope)
        return;

    s.dsvValid = dsv != nullptr && dsv->ptr != 0;
    s.renderTargetCount = count;
    s.rtv0Valid = count > 0 && rtvs != nullptr && rtvs[0].ptr != 0;
    s.rtv0 = s.rtv0Valid ? rtvs[0] : D3D12_CPU_DESCRIPTOR_HANDLE {};
}

inline void OnRSSetViewports(ListState& s, UINT count, const D3D12_VIEWPORT* viewports)
{
    if (s.inEnvelope)
        return;

    s.viewport0Valid = count > 0 && viewports != nullptr;
    s.viewport0 = s.viewport0Valid ? viewports[0] : D3D12_VIEWPORT {};
    s.viewportCount = viewports != nullptr ? count : 0;
}

inline void OnRSSetScissorRects(ListState& s, UINT count, const D3D12_RECT* rects)
{
    if (s.inEnvelope)
        return;

    s.scissor0Valid = count > 0 && rects != nullptr;
    s.scissor0 = s.scissor0Valid ? rects[0] : D3D12_RECT {};
}

inline void OnSetPredication(ListState& s, ID3D12Resource* buffer)
{
    if (!s.inEnvelope)
        s.predicated = buffer != nullptr;
}

inline void OnTransition(ListState& s, ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES after,
                         bool enhanced = false, bool pending = false)
{
    if (s.inEnvelope || !resource)
        return;
    s.transitions[s.transitionCount++ & 7] = { resource, subresource, after, enhanced, pending };
    ++s.barrierCount;
}

inline void OnResourceBarrier(ListState& s, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    if (s.inEnvelope || !barriers)
        return;
    for (UINT i = 0; i < count; ++i)
    {
        const auto& b = barriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
            OnTransition(s, b.Transition.pResource, b.Transition.Subresource, b.Transition.StateAfter, false,
                         (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) != 0);
    }
}

inline bool FindTransition(const ListState& s, ID3D12Resource* resource, UINT subresource,
                           ListState::Transition* result)
{
    for (UINT i = 0; i < 8 && i < s.transitionCount; ++i)
    {
        const auto& t = s.transitions[(s.transitionCount - 1 - i) & 7];
        if (t.resource == resource &&
            (t.subresource == subresource || t.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
        {
            *result = t;
            return true;
        }
    }
    return false;
}

inline void OnBeginRenderPass(ListState& s, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rtvs,
                              const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* dsv = nullptr)
{
    if (s.inEnvelope)
        return;
    s.inRenderPass = true;
    s.renderPassRtv = count && rtvs ? rtvs[0].cpuDescriptor : D3D12_CPU_DESCRIPTOR_HANDLE {};
    OnOMSetRenderTargets(s, count, &s.renderPassRtv, FALSE, dsv != nullptr ? &dsv->cpuDescriptor : nullptr);
}

inline void OnEndRenderPass(ListState& s)
{
    if (s.inEnvelope)
        return;
    s.inRenderPass = false;
    s.renderPassRtv = {};
    OnOMSetRenderTargets(s, 0, nullptr, FALSE);
}

inline void OnClearState(ListState& s, ID3D12PipelineState* pso)
{
    if (!s.inEnvelope)
        liststate_detail::ClearBindings(s, pso);
}

inline void OnExecuteBundle(ListState& s)
{
    if (!s.inEnvelope)
    {
        s.incomplete = true;
        s.pso = nullptr;
        s.graphicsSignature = s.computeSignature = nullptr;
        liststate_detail::ClearGraphicsArguments(s);
        liststate_detail::ClearComputeArguments(s);
    }
}

} // namespace native
