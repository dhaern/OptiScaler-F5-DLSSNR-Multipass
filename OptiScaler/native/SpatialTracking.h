#pragma once
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <new>
#include <iterator>

namespace native::spatial_tracking
{
// Process-lifetime metadata, not device ownership. Slots are never reused: old lists cannot acquire a new generation.
struct Device
{
    std::atomic<ID3D12Device*> object {};
    uint64_t generation = 0;
    std::atomic<bool> published {}, active { true }, ready { false }, descriptorFailed {}, rootFailed {};
    std::atomic<UINT> increments[2] {};
    std::atomic<void*> loose {};
    std::atomic<uint64_t> heapAttempts {}, heapQueries {}, heapSuccess {}, heapIgnored {}, heapFailure {},
        looseOperations {}, looseWrites {}, looseFailure {}, rootAttempts {}, rootSuccess {}, rootFailure {};
};
inline constinit Device devices[64] {};
inline Device* Find(ID3D12Device* object)
{
    for (auto& d : devices)
        if (object && d.object.load(std::memory_order_acquire) == object &&
            d.published.load(std::memory_order_acquire) && d.active.load(std::memory_order_acquire))
            return &d;
    return nullptr;
}
inline bool Available(const Device* d)
{
    return d && d->active.load(std::memory_order_acquire) && d->ready.load(std::memory_order_acquire);
}
namespace detail
{
// Attached only at the device's cold installation boundary. It holds no device reference.
inline constexpr GUID cookieId { 0x519c6a14, 0xb69d, 0x4190, { 0x8a, 0x62, 0x4f, 0x52, 0x14, 0x28, 0x2e, 0x9c } };
struct Cookie final : IUnknown
{
    std::atomic<ULONG> refs { 1 };
    Device* device;
    explicit Cookie(Device* d) : device(d) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!left)
        {
            device->active.store(false, std::memory_order_release);
            delete this;
        }
        return left;
    }
};
} // namespace detail
inline Device* Observe(ID3D12Device* object)
{
    if (auto* found = Find(object))
        return found;
    if (!object)
        return nullptr;
    for (size_t i = 0; i < std::size(devices); ++i)
    {
        auto& d = devices[i];
        ID3D12Device* empty = nullptr;
        // Reserve the identity itself: another observer must stop at this slot, not attach a second cookie.
        if (!d.object.compare_exchange_strong(empty, object, std::memory_order_acq_rel))
        {
            if (empty == object && d.active.load(std::memory_order_acquire))
                return Find(object); // Published generation or safe nonblocking fallback while its owner finishes.
            continue;
        }
        d.generation = i + 1;
        d.increments[0].store(object->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
        d.increments[1].store(object->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
        auto* cookie = new (std::nothrow) detail::Cookie(&d);
        const bool attached = cookie && SUCCEEDED(object->SetPrivateDataInterface(detail::cookieId, cookie));
        if (cookie)
            cookie->Release();
        if (!attached)
            d.active.store(false, std::memory_order_release);
        d.published.store(true, std::memory_order_release);
        return attached ? &d : nullptr;
    }
    return nullptr;
}
} // namespace native::spatial_tracking
