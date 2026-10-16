#include "pch.h"
#include "DescriptorTrackerDx12.h"
#include <atomic>
#include <cstring>
#include <new>
#include <limits>

namespace native::descriptors
{
struct Heap
{
    ID3D12DescriptorHeap* object;
    spatial_tracking::Device* device;
    bool late = false;
    SIZE_T cpu;
    UINT64 gpu;
    UINT count, increment;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    std::atomic<bool> active { true };
    struct Shadow
    {
        uint64_t words[3];
    }* views;
};
static_assert(sizeof(Heap::Shadow) == 24);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
namespace
{
struct Registry
{
    size_t count;
    Heap** heaps;
};
constinit std::atomic<Registry*> registry {};
constinit std::atomic<Heap*> lastHeap {};
SRWLOCK creationLock = SRWLOCK_INIT;
// Late metadata is bounded and inline. No heap-size allocation, blocking registration or shadow migration at bind.
struct Late
{
    std::atomic<ID3D12DescriptorHeap*> key {};
    std::atomic<bool> ready {};
    Heap heap {};
};
constinit Late lateHeaps[256] {};
constinit std::atomic_flag lateOwner = ATOMIC_FLAG_INIT;
struct LateClaim
{
    bool owned = !lateOwner.test_and_set(std::memory_order_acquire);
    ~LateClaim()
    {
        if (owned)
            lateOwner.clear(std::memory_order_release);
    }
};

// Views written into heaps made before our hooks, by CPU handle. Insert-only; a heap seen later takes precedence.
struct Loose
{
    std::atomic<SIZE_T> cpu;
    Heap::Shadow view;
};
constexpr size_t kLoose = size_t(1) << 16, kProbe = 32;

void Retire(ID3D12DescriptorHeap* object, SIZE_T cpu, size_t bytes, spatial_tracking::Device* device)
{
    const auto retire = [&](Heap* h)
    {
        if (h->device == device &&
            (h->object == object || (cpu < h->cpu + size_t(h->count) * h->increment && h->cpu < cpu + bytes)))
            h->active.store(false, std::memory_order_release);
    };
    const Registry* r = registry.load(std::memory_order_acquire);
    for (size_t i = 0; r && i < r->count; ++i)
        retire(r->heaps[i]);
    for (auto& slot : lateHeaps)
        if (slot.ready.load(std::memory_order_acquire))
            retire(&slot.heap);
}

bool Contains(const Heap* h, SIZE_T cpu, spatial_tracking::Device* device)
{
    return h && h->device == device && spatial_tracking::Available(device) &&
           h->active.load(std::memory_order_acquire) && cpu >= h->cpu &&
           cpu - h->cpu < size_t(h->count) * h->increment && (cpu - h->cpu) % h->increment == 0;
}
Heap* FindCpu(SIZE_T cpu, spatial_tracking::Device* device)
{
    if (!spatial_tracking::Available(device))
        return nullptr;
    Heap* h = lastHeap.load(std::memory_order_acquire);
    if (Contains(h, cpu, device))
        return h;
    const Registry* r = registry.load(std::memory_order_acquire);
    if (!r)
        return nullptr;
    size_t lo = 0, hi = r->count;
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo) / 2;
        if (r->heaps[mid]->device->generation < device->generation ||
            (r->heaps[mid]->device == device && r->heaps[mid]->cpu <= cpu))
            lo = mid + 1;
        else
            hi = mid;
    }
    h = lo ? r->heaps[lo - 1] : nullptr;
    if (!Contains(h, cpu, device))
        return nullptr;
    lastHeap.store(h, std::memory_order_release);
    return h;
}
Heap::Shadow* LooseSlot(SIZE_T cpu, bool create, spatial_tracking::Device* device)
{
    if (!spatial_tracking::Available(device) || device->descriptorFailed.load(std::memory_order_acquire))
        return nullptr;
    device->looseOperations.fetch_add(1, std::memory_order_relaxed);
    Loose* table = static_cast<Loose*>(device->loose.load(std::memory_order_acquire));
    if (!table)
    {
        if (!create)
            return nullptr;
        auto* fresh = static_cast<Loose*>(
            VirtualAlloc(nullptr, kLoose * sizeof(Loose), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!fresh)
            return nullptr;
        void* expected = table;
        if (device->loose.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
            table = fresh;
        else
        {
            VirtualFree(fresh, 0, MEM_RELEASE);
            table = static_cast<Loose*>(expected);
        }
    }
    const size_t start = size_t((uint64_t(cpu) * 0x9E3779B97F4A7C15ull) >> 48);
    for (size_t i = 0; i < kProbe; ++i)
    {
        Loose& e = table[(start + i) & (kLoose - 1)];
        SIZE_T key = e.cpu.load(std::memory_order_acquire);
        if (key == 0)
        {
            if (!create)
                return nullptr;
            if (e.cpu.compare_exchange_strong(key, cpu, std::memory_order_acq_rel))
                return &e.view;
            // Taken meanwhile: key now holds whose it is
        }
        if (key == cpu)
            return &e.view;
    }
    return nullptr;
}
// One descriptor's shadow: its heap's when the heap was seen (and of the type), else the loose table's
Heap::Shadow* Slot(SIZE_T cpu, bool rtv, bool create, spatial_tracking::Device* device)
{
    if (Heap* h = FindCpu(cpu, device))
        return h->views && (h->type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) == rtv ? &h->views[(cpu - h->cpu) / h->increment]
                                                                              : nullptr;
    if (!spatial_tracking::Available(device) || (!create && !device->loose.load(std::memory_order_acquire)))
        return nullptr;
    auto* slot = LooseSlot(cpu, create, device);
    if (create)
        (slot ? device->looseWrites : device->looseFailure).fetch_add(1, std::memory_order_relaxed);
    return slot;
}
View Read(const Heap::Shadow& s)
{
    uint64_t words[3];
    for (UINT i = 0; i < 3; ++i)
        words[i] = std::atomic_ref<const uint64_t>(s.words[i]).load(std::memory_order_relaxed);
    View v;
    v.kind = uint8_t(words[2]);
    v.dimension = uint8_t(words[2] >> 8);
    v.mip = uint8_t(words[2] >> 16);
    v.plane = uint8_t(words[2] >> 24);
    v.firstSlice = uint16_t(words[2] >> 32);
    v.sliceCount = uint16_t(words[2] >> 48);
    if (v.kind == uint8_t(Kind::Cbv))
        v.va = words[0];
    else
        v.resource = reinterpret_cast<ID3D12Resource*>(words[0]);
    v.size = uint32_t(words[1]);
    v.format = DXGI_FORMAT(words[1] >> 32);
    return v;
}
void Write(Heap::Shadow& s, const View& v)
{
    const uint64_t words[] = { v.kind == uint8_t(Kind::Cbv) ? v.va : uint64_t(uintptr_t(v.resource)),
                               uint64_t(v.size) | (uint64_t(v.format) << 32),
                               uint64_t(v.kind) | (uint64_t(v.dimension) << 8) | (uint64_t(v.mip) << 16) |
                                   (uint64_t(v.plane) << 24) | (uint64_t(v.firstSlice) << 32) |
                                   (uint64_t(v.sliceCount) << 48) };
    // Descriptor writes can be volatile; the consumer validates the resource and view before recording work.
    for (UINT i = 0; i < 3; ++i)
        std::atomic_ref<uint64_t>(s.words[i]).store(words[i], std::memory_order_relaxed);
}
View MakeView(Kind kind, ID3D12Resource* resource, const void* desc)
{
    View v;
    v.resource = resource;
    v.kind = uint8_t(kind);
    UINT dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN, mip = 0, plane = 0, first = 0, slices = 1;
    if (kind == Kind::Cbv)
    {
        if (!desc)
            return {};
        const auto& d = *static_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(desc);
        v.resource = nullptr;
        v.va = d.BufferLocation;
        v.size = d.SizeInBytes;
        dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    }
    else if (!desc)
    {
        if (!resource)
            return {};
        const auto d = resource->GetDesc();
        v.format = d.Format;
        dimension = d.Dimension;
        slices = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? 1 : d.DepthOrArraySize;
    }
    else if (kind == Kind::Srv)
    {
        const auto& d = *static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(desc);
        v.format = d.Format;
        switch (d.ViewDimension)
        {
        case D3D12_SRV_DIMENSION_BUFFER:
            dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            mip = d.Texture1D.MostDetailedMip;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            mip = d.Texture1DArray.MostDetailedMip;
            first = d.Texture1DArray.FirstArraySlice;
            slices = d.Texture1DArray.ArraySize;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            mip = d.Texture2D.MostDetailedMip;
            plane = d.Texture2D.PlaneSlice;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            mip = d.Texture2DArray.MostDetailedMip;
            plane = d.Texture2DArray.PlaneSlice;
            first = d.Texture2DArray.FirstArraySlice;
            slices = d.Texture2DArray.ArraySize;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMS:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            first = d.Texture2DMSArray.FirstArraySlice;
            slices = d.Texture2DMSArray.ArraySize;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            mip = d.Texture3D.MostDetailedMip;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            return {}; // A cube is not a single 2D subresource, even when its resource has six slices.
        default:
            return {};
        }
    }
    else if (kind == Kind::Uav)
    {
        const auto& d = *static_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(desc);
        v.format = d.Format;
        switch (d.ViewDimension)
        {
        case D3D12_UAV_DIMENSION_BUFFER:
            dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE1D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            mip = d.Texture1D.MipSlice;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            mip = d.Texture1DArray.MipSlice;
            first = d.Texture1DArray.FirstArraySlice;
            slices = d.Texture1DArray.ArraySize;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            mip = d.Texture2D.MipSlice;
            plane = d.Texture2D.PlaneSlice;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            mip = d.Texture2DArray.MipSlice;
            plane = d.Texture2DArray.PlaneSlice;
            first = d.Texture2DArray.FirstArraySlice;
            slices = d.Texture2DArray.ArraySize;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE3D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            mip = d.Texture3D.MipSlice;
            first = d.Texture3D.FirstWSlice;
            slices = d.Texture3D.WSize;
            break;
        default:
            return {};
        }
    }
    else if (kind == Kind::Rtv)
    {
        const auto& d = *static_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(desc);
        v.format = d.Format;
        switch (d.ViewDimension)
        {
        case D3D12_RTV_DIMENSION_BUFFER:
            dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE1D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            mip = d.Texture1D.MipSlice;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            mip = d.Texture1DArray.MipSlice;
            first = d.Texture1DArray.FirstArraySlice;
            slices = d.Texture1DArray.ArraySize;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            mip = d.Texture2D.MipSlice;
            plane = d.Texture2D.PlaneSlice;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            mip = d.Texture2DArray.MipSlice;
            plane = d.Texture2DArray.PlaneSlice;
            first = d.Texture2DArray.FirstArraySlice;
            slices = d.Texture2DArray.ArraySize;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMS:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            first = d.Texture2DMSArray.FirstArraySlice;
            slices = d.Texture2DMSArray.ArraySize;
            break;
        case D3D12_RTV_DIMENSION_TEXTURE3D:
            dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            mip = d.Texture3D.MipSlice;
            first = d.Texture3D.FirstWSlice;
            slices = d.Texture3D.WSize;
            break;
        default:
            return {};
        }
    }
    else
        return {};
    if (mip > UINT8_MAX || plane > UINT8_MAX || first > UINT16_MAX || slices > UINT16_MAX)
        return {};
    v.dimension = uint8_t(dimension);
    v.mip = uint8_t(mip);
    v.plane = uint8_t(plane);
    v.firstSlice = uint16_t(first);
    v.sliceCount = uint16_t(slices);
    return v;
}
} // namespace

void OnCreateHeap(ID3D12DescriptorHeap* object, const D3D12_DESCRIPTOR_HEAP_DESC& desc, UINT increment,
                  spatial_tracking::Device* device)
{
    if (!device || !device->active.load(std::memory_order_acquire) || !object || !increment || !desc.NumDescriptors ||
        desc.Type >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES)
        return;
    const bool tracked =
        desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (tracked)
        device->increments[desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV].store(increment, std::memory_order_relaxed);
    const SIZE_T cpu = object->GetCPUDescriptorHandleForHeapStart().ptr;
    const size_t bytes = size_t(desc.NumDescriptors) * increment;
    if (bytes / increment != desc.NumDescriptors || cpu > SIZE_MAX - bytes ||
        size_t(desc.NumDescriptors) > SIZE_MAX / sizeof(Heap::Shadow))
        return;
    auto* h = static_cast<Heap*>(VirtualAlloc(nullptr, sizeof(Heap), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!h)
    {
        Retire(object, cpu, bytes, device);
        device->descriptorFailed.store(true, std::memory_order_release);
        return;
    }
    new (h) Heap {};
    if (tracked)
        h->views = static_cast<Heap::Shadow*>(VirtualAlloc(nullptr, size_t(desc.NumDescriptors) * sizeof(Heap::Shadow),
                                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (tracked && !h->views)
    {
        Retire(object, cpu, bytes, device);
        device->descriptorFailed.store(true, std::memory_order_release);
        VirtualFree(h, 0, MEM_RELEASE);
        return;
    }
    h->object = object;
    h->device = device;
    h->cpu = cpu;
    h->count = desc.NumDescriptors;
    h->increment = increment;
    h->type = desc.Type;
    h->gpu =
        (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? object->GetGPUDescriptorHandleForHeapStart().ptr : 0;
    AcquireSRWLockExclusive(&creationLock);
    Registry* old = registry.load(std::memory_order_relaxed);
    const size_t capacity = old ? old->count + 1 : 1;
    auto* next = static_cast<Registry*>(
        VirtualAlloc(nullptr, sizeof(Registry) + capacity * sizeof(Heap*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Retire(object, cpu, bytes, device);
    if (!next)
        device->descriptorFailed.store(true, std::memory_order_release);
    if (next)
    {
        next->heaps = reinterpret_cast<Heap**>(next + 1);
        next->count = 0;
        for (size_t i = 0; old && i < old->count; ++i)
        {
            Heap* previous = old->heaps[i];
            if (previous->active.load(std::memory_order_relaxed))
                next->heaps[next->count++] = previous;
        }
        size_t at = next->count;
        while (at && (next->heaps[at - 1]->device->generation > device->generation ||
                      (next->heaps[at - 1]->device == device && next->heaps[at - 1]->cpu > cpu)))
        {
            next->heaps[at] = next->heaps[at - 1];
            --at;
        }
        next->heaps[at] = h;
        ++next->count;
        // Readers and ListState heap references outlive registration, so published storage is never freed.
        registry.store(next, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&creationLock);
    if (!next)
    {
        if (h->views)
            VirtualFree(h->views, 0, MEM_RELEASE);
        VirtualFree(h, 0, MEM_RELEASE);
    }
}
HeapRef FindHeap(ID3D12DescriptorHeap* object, spatial_tracking::Device* device)
{
    if (!spatial_tracking::Available(device))
        return {};
    const Registry* r = registry.load(std::memory_order_acquire);
    for (size_t i = 0; object && r && i < r->count; ++i)
        if (r->heaps[i]->device == device && r->heaps[i]->object == object &&
            r->heaps[i]->active.load(std::memory_order_acquire))
            return { r->heaps[i], r->heaps[i]->increment };
    return {};
}
UINT Increment(D3D12_DESCRIPTOR_HEAP_TYPE type, spatial_tracking::Device* device)
{
    return spatial_tracking::Available(device) &&
                   (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
               ? device->increments[type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV].load(std::memory_order_relaxed)
               : 0;
}

HeapRef ResolveBoundHeap(ID3D12DescriptorHeap* object, spatial_tracking::Device* device)
{
    if (!object || !spatial_tracking::Available(device) || device->descriptorFailed.load(std::memory_order_acquire))
        return {};
    device->heapAttempts.fetch_add(1, std::memory_order_relaxed);
    LateClaim claim;
    if (!claim.owned)
    {
        device->heapFailure.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const auto result = [&](const Heap& h) -> HeapRef
    {
        if (!h.active.load(std::memory_order_acquire) || h.device != device)
        {
            device->heapFailure.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        (h.type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER || h.type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV ? device->heapIgnored
                                                                                                  : device->heapSuccess)
            .fetch_add(1, std::memory_order_relaxed);
        return { &h, h.increment };
    };
    for (auto& slot : lateHeaps)
        if (slot.key.load(std::memory_order_acquire) == object)
            return slot.ready.load(std::memory_order_acquire) ? result(slot.heap) : HeapRef {};
    for (auto& slot : lateHeaps)
    {
        ID3D12DescriptorHeap* empty = nullptr;
        if (!slot.key.compare_exchange_strong(empty, object, std::memory_order_acq_rel))
            continue;
        Heap& h = slot.heap;
        h.object = object;
        h.device = device;
        h.late = true;
        device->heapQueries.fetch_add(1, std::memory_order_relaxed);
        const auto desc = object->GetDesc();
        ID3D12Device* owner = nullptr;
        const bool matches = SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&owner))) &&
                             owner == device->object.load(std::memory_order_acquire);
        if (owner)
            owner->Release();
        h.type = desc.Type;
        h.count = desc.NumDescriptors;
        h.increment = Increment(desc.Type, device);
        const bool ignored =
            desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER || desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (ignored)
            h.increment = 1; // No descriptor shadow or GPU lookup will use ignored metadata.
        h.cpu = object->GetCPUDescriptorHandleForHeapStart().ptr;
        h.gpu = (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && !ignored
                    ? object->GetGPUDescriptorHandleForHeapStart().ptr
                    : 0;
        h.active.store(matches && h.count && h.increment && UINT64(h.count) * h.increment <= SIZE_MAX - h.cpu,
                       std::memory_order_release);
        slot.ready.store(true, std::memory_order_release);
        return result(h);
    }
    device->heapFailure.fetch_add(1, std::memory_order_relaxed);
    return {};
}
bool Lookup(D3D12_CPU_DESCRIPTOR_HANDLE cpu, View* out, spatial_tracking::Device* device)
{
    if (!spatial_tracking::Available(device))
        return false;
    Heap* h = FindCpu(cpu.ptr, device);
    const Heap::Shadow* s =
        h ? (h->views ? &h->views[(cpu.ptr - h->cpu) / h->increment] : nullptr)
          : (device->loose.load(std::memory_order_acquire) ? LooseSlot(cpu.ptr, false, device) : nullptr);
    if (!s || !out)
        return false;
    *out = Read(*s);
    return out->kind != uint8_t(Kind::None);
}
bool LookupGpu(const HeapRef& ref, D3D12_GPU_DESCRIPTOR_HANDLE gpu, View* out)
{
    const Heap* h = ref.heap;
    if (!h || !spatial_tracking::Available(h->device) || !out || !h->gpu ||
        !h->active.load(std::memory_order_acquire) || gpu.ptr < h->gpu ||
        gpu.ptr - h->gpu >= UINT64(h->count) * h->increment || (gpu.ptr - h->gpu) % h->increment)
        return false;
    const size_t index = size_t((gpu.ptr - h->gpu) / h->increment);
    const auto* shadow =
        h->views ? &h->views[index] : (h->late ? LooseSlot(h->cpu + index * h->increment, false, h->device) : nullptr);
    if (!shadow)
        return false;
    *out = Read(*shadow);
    return out->kind != uint8_t(Kind::None);
}
void OnCreateView(Kind kind, ID3D12Resource* resource, const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                  spatial_tracking::Device* device)
{
    if (Heap::Shadow* s = Slot(cpu.ptr, kind == Kind::Rtv, true, device))
        Write(*s, MakeView(kind, resource, desc));
}
void OnCopySimple(UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                  D3D12_DESCRIPTOR_HEAP_TYPE type, spatial_tracking::Device* device)
{
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        return;
    Heap* d = FindCpu(dst.ptr, device);
    Heap* s = FindCpu(src.ptr, device);
    if (d && s && d->type == type && s->type == type)
    {
        const size_t di = (dst.ptr - d->cpu) / d->increment, si = (src.ptr - s->cpu) / s->increment;
        if (n <= d->count - di && n <= s->count - si)
        {
            for (UINT i = 0; i < n; ++i)
                Write(d->views[di + i], Read(s->views[si + i]));
            return;
        }
    }
    // Either side in a heap made before our hooks: one descriptor at a time
    const bool rtv = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    const size_t step = Increment(type, device);
    if (!step)
        return;
    for (UINT i = 0; i < n; ++i)
    {
        const Heap::Shadow* from = Slot(src.ptr + i * step, rtv, false, device);
        const View v = from ? Read(*from) : View {};
        if (Heap::Shadow* to = Slot(dst.ptr + i * step, rtv, v.kind != uint8_t(Kind::None), device))
            Write(*to, v);
    }
}
void OnCopy(UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dst, const UINT* dn, UINT ns,
            const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* sn, D3D12_DESCRIPTOR_HEAP_TYPE type,
            spatial_tracking::Device* device)
{
    if (!dst || !src || (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV))
        return;
    UINT d = 0, s = 0, di = 0, si = 0;
    while (d < nd && s < ns)
    {
        const UINT dc = dn ? dn[d] : 1, sc = sn ? sn[s] : 1;
        if (di == dc)
        {
            ++d;
            di = 0;
            continue;
        }
        if (si == sc)
        {
            ++s;
            si = 0;
            continue;
        }
        const size_t step = Increment(type, device);
        if (!step)
            return;
        const UINT n = dc - di < sc - si ? dc - di : sc - si;
        OnCopySimple(n, { dst[d].ptr + di * step }, { src[s].ptr + si * step }, type, device);
        di += n;
        si += n;
    }
}
} // namespace native::descriptors
