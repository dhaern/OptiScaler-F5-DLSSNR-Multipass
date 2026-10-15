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
constinit std::atomic<UINT> increments[2] {}; // CBV_SRV_UAV, RTV: per device, the same for every heap of a type

// Views written into heaps made before our hooks, by CPU handle. Insert-only; a heap seen later takes precedence.
struct Loose
{
    std::atomic<SIZE_T> cpu;
    Heap::Shadow view;
};
constexpr size_t kLoose = size_t(1) << 16, kProbe = 32;
constinit std::atomic<Loose*> looseTable {};

void Retire(ID3D12DescriptorHeap* object, SIZE_T cpu, size_t bytes)
{
    const Registry* r = registry.load(std::memory_order_acquire);
    for (size_t i = 0; r && i < r->count; ++i)
    {
        Heap* h = r->heaps[i];
        if (h->object == object || (cpu < h->cpu + size_t(h->count) * h->increment && h->cpu < cpu + bytes))
            h->active.store(false, std::memory_order_relaxed);
    }
}

bool Contains(const Heap* h, SIZE_T cpu)
{
    return h && h->active.load(std::memory_order_relaxed) && cpu >= h->cpu &&
           cpu - h->cpu < size_t(h->count) * h->increment && (cpu - h->cpu) % h->increment == 0;
}
Heap* FindCpu(SIZE_T cpu)
{
    Heap* h = lastHeap.load(std::memory_order_acquire);
    if (Contains(h, cpu))
        return h;
    const Registry* r = registry.load(std::memory_order_acquire);
    if (!r)
        return nullptr;
    size_t lo = 0, hi = r->count;
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo) / 2;
        if (r->heaps[mid]->cpu <= cpu)
            lo = mid + 1;
        else
            hi = mid;
    }
    h = lo ? r->heaps[lo - 1] : nullptr;
    if (!Contains(h, cpu))
        return nullptr;
    lastHeap.store(h, std::memory_order_release);
    return h;
}
Heap::Shadow* LooseSlot(SIZE_T cpu, bool create)
{
    Loose* table = looseTable.load(std::memory_order_acquire);
    if (!table)
    {
        if (!create)
            return nullptr;
        auto* fresh =
            static_cast<Loose*>(VirtualAlloc(nullptr, kLoose * sizeof(Loose), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!fresh)
            return nullptr;
        if (looseTable.compare_exchange_strong(table, fresh, std::memory_order_acq_rel, std::memory_order_acquire))
            table = fresh;
        else
            VirtualFree(fresh, 0, MEM_RELEASE);
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
Heap::Shadow* Slot(SIZE_T cpu, bool rtv, bool create)
{
    if (Heap* h = FindCpu(cpu))
        return (h->type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) == rtv ? &h->views[(cpu - h->cpu) / h->increment] : nullptr;
    return LooseSlot(cpu, create);
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

void OnCreateHeap(ID3D12DescriptorHeap* object, const D3D12_DESCRIPTOR_HEAP_DESC& desc, UINT increment)
{
    if (!object || !increment || !desc.NumDescriptors ||
        (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV))
        return;
    increments[desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV].store(increment, std::memory_order_relaxed);
    const SIZE_T cpu = object->GetCPUDescriptorHandleForHeapStart().ptr;
    const size_t bytes = size_t(desc.NumDescriptors) * increment;
    if (bytes / increment != desc.NumDescriptors || cpu > SIZE_MAX - bytes ||
        size_t(desc.NumDescriptors) > SIZE_MAX / sizeof(Heap::Shadow))
        return;
    auto* h = static_cast<Heap*>(VirtualAlloc(nullptr, sizeof(Heap), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!h)
    {
        Retire(object, cpu, bytes);
        return;
    }
    new (h) Heap {};
    h->views = static_cast<Heap::Shadow*>(VirtualAlloc(nullptr, size_t(desc.NumDescriptors) * sizeof(Heap::Shadow),
                                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!h->views)
    {
        Retire(object, cpu, bytes);
        VirtualFree(h, 0, MEM_RELEASE);
        return;
    }
    h->object = object;
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
    Retire(object, cpu, bytes);
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
        while (at && next->heaps[at - 1]->cpu > cpu)
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
        VirtualFree(h->views, 0, MEM_RELEASE);
        VirtualFree(h, 0, MEM_RELEASE);
    }
}
HeapRef FindHeap(ID3D12DescriptorHeap* object)
{
    const Registry* r = registry.load(std::memory_order_acquire);
    for (size_t i = 0; object && r && i < r->count; ++i)
        if (r->heaps[i]->object == object && r->heaps[i]->active.load(std::memory_order_relaxed))
            return { r->heaps[i], r->heaps[i]->increment };
    return {};
}
UINT Increment(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    return type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV
               ? increments[type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV].load(std::memory_order_relaxed)
               : 0;
}
void NoteIncrement(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT increment)
{
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        increments[type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV].store(increment, std::memory_order_relaxed);
}
HeapRef Adopt(ID3D12DescriptorHeap* object)
{
    HeapRef ref = FindHeap(object);
    if (ref.heap || !object)
        return ref;
    // Made before our hooks: shader-visible heaps are seen here when bound; their descriptors are copied in afterwards
    const auto desc = object->GetDesc();
    ID3D12Device* device = nullptr;
    if (!(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) || FAILED(object->GetDevice(IID_PPV_ARGS(&device))))
        return ref;
    const UINT increment = device->GetDescriptorHandleIncrementSize(desc.Type);
    device->Release();
    OnCreateHeap(object, desc, increment);
    ref = FindHeap(object);
    // What was written into it before, kept by CPU handle until now
    if (auto* h = const_cast<Heap*>(ref.heap); h && looseTable.load(std::memory_order_acquire))
        for (UINT i = 0; i < h->count; ++i)
            if (const Heap::Shadow* loose = LooseSlot(h->cpu + size_t(i) * h->increment, false))
                Write(h->views[i], Read(*loose));
    return ref;
}
bool Lookup(D3D12_CPU_DESCRIPTOR_HANDLE cpu, View* out)
{
    Heap* h = FindCpu(cpu.ptr);
    const Heap::Shadow* s = h ? &h->views[(cpu.ptr - h->cpu) / h->increment] : LooseSlot(cpu.ptr, false);
    if (!s || !out)
        return false;
    *out = Read(*s);
    return out->kind != uint8_t(Kind::None);
}
bool LookupGpu(const HeapRef& ref, D3D12_GPU_DESCRIPTOR_HANDLE gpu, View* out)
{
    const Heap* h = ref.heap;
    if (!h || !out || !h->gpu || !h->active.load(std::memory_order_relaxed) || gpu.ptr < h->gpu ||
        gpu.ptr - h->gpu >= UINT64(h->count) * h->increment || (gpu.ptr - h->gpu) % h->increment)
        return false;
    *out = Read(h->views[(gpu.ptr - h->gpu) / h->increment]);
    return out->kind != uint8_t(Kind::None);
}
void OnCreateView(Kind kind, ID3D12Resource* resource, const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
    if (Heap::Shadow* s = Slot(cpu.ptr, kind == Kind::Rtv, true))
        Write(*s, MakeView(kind, resource, desc));
}
void OnCopySimple(UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                  D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        return;
    Heap* d = FindCpu(dst.ptr);
    Heap* s = FindCpu(src.ptr);
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
    const size_t step = increments[rtv].load(std::memory_order_relaxed);
    if (!step)
        return;
    for (UINT i = 0; i < n; ++i)
    {
        const Heap::Shadow* from = Slot(src.ptr + i * step, rtv, false);
        const View v = from ? Read(*from) : View {};
        if (Heap::Shadow* to = Slot(dst.ptr + i * step, rtv, v.kind != uint8_t(Kind::None)))
            Write(*to, v);
    }
}
void OnCopy(UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dst, const UINT* dn, UINT ns,
            const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* sn, D3D12_DESCRIPTOR_HEAP_TYPE type)
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
        const size_t step = increments[type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV].load(std::memory_order_relaxed);
        if (!step)
            return;
        const UINT n = dc - di < sc - si ? dc - di : sc - si;
        OnCopySimple(n, { dst[d].ptr + di * step }, { src[s].ptr + si * step }, type);
        di += n;
        si += n;
    }
}
} // namespace native::descriptors
