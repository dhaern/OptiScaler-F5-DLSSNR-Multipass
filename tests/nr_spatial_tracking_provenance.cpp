// Production helper contract. The legacy adapter keeps the pre-change failure an executable assertion, not a compile
// error.
#include "pch.h"
#include "../OptiScaler/native/DescriptorTrackerDx12.h"
#include "../OptiScaler/native/RootSignatureMap.h"
#include "../OptiScaler/native/ListStateDx12.h"
#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>
#if __has_include("../OptiScaler/native/SpatialTracking.h")
#include "../OptiScaler/native/SpatialTracking.h"
using Context = native::spatial_tracking::Device;
#define SCOPED_TRACKING 1
#else
struct Context
{
};
#endif
static int failures = 0;
extern bool trackingContractHot, trackingContractFailAllocation;
extern unsigned trackingContractHotAllocations, trackingContractHotLocks;
static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
struct Identity final : IUnknown
{
    ULONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
};
struct Heap final : ID3D12DescriptorHeap
{
    SIZE_T cpu = 0x100000;
    UINT64 gpu = 0x200000;
    D3D12_DESCRIPTOR_HEAP_DESC desc { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    UINT descCalls = 0, deviceCalls = 0;
    IUnknown* owner = nullptr; // Only IUnknown's first three slots are needed for this GetDevice/Release fixture.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** out) override
    {
        ++deviceCalls;
        *out = owner;
        if (owner)
            owner->AddRef();
        return owner ? S_OK : E_NOTIMPL;
    }
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override
    {
        ++descCalls;
        return desc;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override { return { cpu }; }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override { return { gpu }; }
};
static void Created(Context* c, Heap& h, UINT increment)
{
#ifdef SCOPED_TRACKING
    native::descriptors::OnCreateHeap(&h, h.desc, increment, c);
#else
    (void) c;
    native::descriptors::OnCreateHeap(&h, h.desc, increment);
#endif
}
static void Cbv(Context* c, SIZE_T cpu, UINT64 va)
{
    D3D12_CONSTANT_BUFFER_VIEW_DESC v { va, 256 };
#ifdef SCOPED_TRACKING
    native::descriptors::OnCreateView(native::descriptors::Kind::Cbv, nullptr, &v, { cpu }, c);
#else
    (void) c;
    native::descriptors::OnCreateView(native::descriptors::Kind::Cbv, nullptr, &v, { cpu });
#endif
}
static bool Lookup(Context* c, SIZE_T cpu, native::descriptors::View* out)
{
#ifdef SCOPED_TRACKING
    return native::descriptors::Lookup({ cpu }, out, c);
#else
    (void) c;
    return native::descriptors::Lookup({ cpu }, out);
#endif
}
#ifdef SCOPED_TRACKING
// COM-shaped device seam: exactly the two methods Observe uses, with normal private-interface ownership.
struct ObservedDevice
{
    void** table;
    IUnknown* cookie = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    bool pause = false, entered = false, resume = false, failAttach = false;
    unsigned attachments = 0;
    static UINT STDMETHODCALLTYPE Stride(ObservedDevice* d, D3D12_DESCRIPTOR_HEAP_TYPE)
    {
        std::unique_lock lock(d->mutex);
        if (d->pause && !d->entered)
        {
            d->entered = true;
            d->changed.notify_all();
            d->changed.wait(lock, [&] { return d->resume; });
        }
        return 32;
    }
    static HRESULT STDMETHODCALLTYPE Attach(ObservedDevice* d, REFGUID, const IUnknown* value)
    {
        std::lock_guard lock(d->mutex);
        if (d->failAttach)
            return E_FAIL;
        auto* next = const_cast<IUnknown*>(value);
        if (next) next->AddRef();
        if (d->cookie) d->cookie->Release();
        d->cookie = next;
        ++d->attachments;
        return S_OK;
    }
    ID3D12Device* Object() { return reinterpret_cast<ID3D12Device*>(this); }
    void Retire() { if (cookie) { cookie->Release(); cookie = nullptr; } }
    ~ObservedDevice() { Retire(); }
};
static void ObserveRace()
{
    namespace tracking = native::spatial_tracking;
    std::array<void*,44> table {};
    table[5] = reinterpret_cast<void*>(ObservedDevice::Attach);
    table[15] = reinterpret_cast<void*>(ObservedDevice::Stride);
    ObservedDevice object { table.data() };
    object.pause = true;
    Context* first = nullptr;
    std::thread owner([&] { first = tracking::Observe(object.Object()); });
    { std::unique_lock lock(object.mutex); object.changed.wait(lock, [&] { return object.entered; }); }
    Context* second = tracking::Observe(object.Object()); // Must return without waiting for the paused owner.
    native::ListState list;
    Heap heap;
    auto publish = [&](Context* d) {
        if (!d) return;
        d->ready.store(true);
        list.device = d;
        Created(d,heap,32);
        Cbv(d,heap.cpu,0xabcdef00);
    };
    publish(second); // Frozen code publishes metadata for the duplicate before the owner replaces its cookie.
    { std::lock_guard lock(object.mutex); object.resume = true; object.changed.notify_all(); }
    owner.join();
    if (!second) publish(first);
    native::descriptors::View view;
    Check(first && (!second || first == second), "simultaneous first observation never publishes two generations");
    Check(object.attachments == 1, "one lifetime cookie attached per live object");
    Check(tracking::Available(list.device) && Lookup(list.device,heap.cpu,&view) && view.va == 0xabcdef00,
          "creation/list metadata survives the concurrent observer");
    Check(tracking::Observe(object.Object()) == list.device, "retry finds the published canonical generation");
    const auto old = list.device;
    object.Retire();
    Check(!tracking::Available(old) && !tracking::Find(object.Object()), "marker release retires the old generation");
    auto* reused = tracking::Observe(object.Object());
    Check(reused && reused != old && reused->generation != old->generation, "same address obtains a fresh generation");
    ObservedDevice independent { table.data() };
    auto* other = tracking::Observe(independent.Object());
    Check(other && other != reused, "independent device keeps independent metadata");
    ObservedDevice failed { table.data() }; failed.failAttach = true;
    Check(!tracking::Observe(failed.Object()) && !tracking::Find(failed.Object()), "failed cookie attachment is not published live");
    std::array<ObservedDevice,65> churn {};
    unsigned accepted = 0;
    for (auto& d : churn) { d.table=table.data(); if (tracking::Observe(d.Object())) ++accepted; }
    Check(accepted > 0 && accepted < churn.size(), "device table exhausts without recycling old generations");
    Check(tracking::Find(object.Object()) == reused && tracking::Find(independent.Object()) == other,
          "table exhaustion preserves existing independent identities");
}
#endif
int main()
{
    Context a {}, b {};
    Identity identity;
#ifdef SCOPED_TRACKING
    a.active.store(true);
    b.active.store(true);
    a.ready.store(true);
    b.ready.store(true);
    a.generation = 100;
    b.generation = 101;
    a.object.store(reinterpret_cast<ID3D12Device*>(&identity));
#endif
    Heap one, two;
    Created(&a, one, 32);
    Cbv(&a, one.cpu, 0x111100);
    Created(&b, two, 64);
    Cbv(&b, two.cpu, 0x222200);
    native::descriptors::View v;
    Check(Lookup(&a, one.cpu, &v) && v.va == 0x111100, "device A survives equal CPU range on device B");
    Check(Lookup(&b, two.cpu, &v) && v.va == 0x222200, "device B owns its own descriptor stride and shadow");
#ifdef SCOPED_TRACKING
    Check(native::descriptors::Increment(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, &a) == 32 &&
              native::descriptors::Increment(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, &b) == 64,
          "strides are per device");
    Check(a.looseOperations.load() == 0 && a.heapAttempts.load() == 0 && a.rootAttempts.load() == 0 &&
              b.looseOperations.load() == 0 && b.heapAttempts.load() == 0,
          "primary path enters no fallback helper");
    auto* rs = reinterpret_cast<ID3D12RootSignature*>(uintptr_t(0x33000));
    native::rootsig::OnCreate(rs, nullptr, 0, &a);
    Check(!native::rootsig::MayInfer(rs, &a), "known failed root parse is not an old unknown object");
    Check(!native::rootsig::MayInfer(rs, &b), "another device cannot infer a known foreign root");
    auto* unknown = reinterpret_cast<ID3D12RootSignature*>(uintptr_t(0x44000));
    Check(native::rootsig::MayInfer(unknown, &a), "unknown old root remains eligible for fallible inference");
    a.rootFailed.store(true);
    Check(!native::rootsig::MayInfer(unknown, &a), "registration failure disables unknown-root fallback");
    Heap late, sampler;
    late.cpu = 0x900000;
    late.gpu = 0xa00000;
    late.owner = &identity;
    sampler.cpu = 0xb00000;
    sampler.gpu = 0xc00000;
    sampler.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    Created(&a, sampler, 16);
    Cbv(&a, late.cpu, 0x333300); // Evidenced old CPU heap: lazy loose storage, before recording.
    native::ListState list;
    list.device = &a;
    ID3D12DescriptorHeap* bound[] = { &late, &sampler };
    trackingContractHot = true;
    for (UINT i = 0; i < 128; ++i)
    {
        native::liststate_detail::ClearAll(list, nullptr);
        native::OnSetDescriptorHeaps(list, 2, bound);
        Check(native::descriptors::LookupGpu(list.heapRefs[0], { late.gpu }, &v) && v.va == 0x333300,
              "late inline metadata reads the current loose view");
    }
    trackingContractHot = false;
    Check(trackingContractHotAllocations == 0 && trackingContractHotLocks == 0,
          "bind/reset/late GPU lookup allocates and blocks zero times");
    Check(late.descCalls == 1 && late.deviceCalls == 1 && identity.refs == 1,
          "late metadata queries once and releases its temporary device reference");
    Check(sampler.descCalls == 0 && sampler.deviceCalls == 0, "normal sampler never enters late metadata");
    Cbv(&a, late.cpu, 0x444400);
    Check(native::descriptors::LookupGpu(list.heapRefs[0], { late.gpu }, &v) && v.va == 0x444400,
          "late fallback does not cache a frame's constants or texture");
    const auto lateRef = list.heapRefs[0];
    Created(&a, late, 32); // Same object address observed at creation: old inline/loose metadata must not win.
    Check(!native::descriptors::LookupGpu(lateRef, { late.gpu }, &v), "observed creation retires old late reference");
    Check(!Lookup(&a, late.cpu, &v), "fresh primary shadow does not inherit stale loose descriptors");
    Cbv(&a, late.cpu, 0x555500);
    Check(Lookup(&a, late.cpu, &v) && v.va == 0x555500, "recreated heap uses fresh primary metadata");
    trackingContractFailAllocation = true;
    Created(&a, late, 32);
    trackingContractFailAllocation = false;
    Check(a.descriptorFailed.load() && !native::descriptors::FindHeap(&late, &a).heap,
          "failed registration retires stale metadata and records unavailable tracking");
    const auto queries = a.heapQueries.load();
    Check(!native::descriptors::ResolveBoundHeap(&late, &a).heap && a.heapQueries.load() == queries,
          "known registration failure is not retried as an unknown old heap");
    auto* cookie = new native::spatial_tracking::detail::Cookie(&b);
    cookie->Release();
    Check(!b.active.load() && !Lookup(&b, two.cpu, &v), "device lifetime cookie retires its generation");
    a.active.store(false);
    Check(!Lookup(&a, one.cpu, &v), "retired device generation cannot expose stale views");
    ObserveRace();
#endif
    std::printf("%s: scoped tracking (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
