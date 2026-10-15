#pragma once
#include <d3d12.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include "SpatialTracking.h"

namespace native::rootsig
{
struct Slot
{
    enum Kind : uint8_t
    {
        None,
        Table,
        RootDescriptor,
        Constants
    } kind = None;
    uint8_t parameter = 0;
    // Finite tables return the remaining descriptors; an unbounded range stays UINT32_MAX.
    uint32_t offset = 0, count = 0;
};
namespace detail
{
struct Record
{
    ID3D12RootSignature* signature;
    spatial_tracking::Device* device = nullptr;
    Record* next;
    ID3D12VersionedRootSignatureDeserializer* deserializer = nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    std::atomic<bool> active { true };
};
inline constinit std::atomic<Record*> buckets[256] {};
inline UINT Hash(ID3D12RootSignature* rs) { return UINT((uint64_t(uintptr_t(rs)) * 0x9E3779B97F4A7C15ull) >> 56); }
inline D3D12_DESCRIPTOR_RANGE_TYPE Type(D3D12_ROOT_PARAMETER_TYPE type)
{
    return type == D3D12_ROOT_PARAMETER_TYPE_SRV   ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
           : type == D3D12_ROOT_PARAMETER_TYPE_UAV ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                                                   : D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
}
} // namespace detail

inline void OnCreate(ID3D12RootSignature* rs, const void* blob, size_t size, spatial_tracking::Device* device = nullptr)
{
    if (!rs)
        return;
    auto& bucket = detail::buckets[detail::Hash(rs)];
    for (auto* r = bucket.load(std::memory_order_acquire); r; r = r->next)
        if (r->signature == rs)
            r->active.store(false, std::memory_order_relaxed);
    if (size > SIZE_MAX - sizeof(detail::Record))
    {
        if (device)
            device->rootFailed.store(true, std::memory_order_release);
        return;
    }
    auto* storage = VirtualAlloc(nullptr, sizeof(detail::Record) + size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!storage)
    {
        if (device)
            device->rootFailed.store(true, std::memory_order_release);
        return;
    }
    auto* r = new (storage) detail::Record {};
    r->signature = rs;
    r->device = device;
    // Keep the bytes and deserializer alive: descriptions reference their storage, not the caller's blob.
    if (blob && size)
    {
        void* copy = r + 1;
        std::memcpy(copy, blob, size);
        auto module = GetModuleHandleW(L"d3d12.dll");
        auto create = module ? reinterpret_cast<PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER>(
                                   GetProcAddress(module, "D3D12CreateVersionedRootSignatureDeserializer"))
                             : nullptr;
        if (create && SUCCEEDED(create(copy, size, IID_PPV_ARGS(&r->deserializer))))
            r->desc = r->deserializer->GetUnconvertedRootSignatureDesc();
    }
    auto* head = bucket.load(std::memory_order_relaxed);
    do
    {
        r->next = head;
    } while (!bucket.compare_exchange_weak(head, r, std::memory_order_release, std::memory_order_relaxed));
}

// Absence and failed parsing are deliberately different: only an absent old signature may use inference.
inline bool MayInfer(ID3D12RootSignature* rs, spatial_tracking::Device* device)
{
    if (!rs || !spatial_tracking::Available(device) || device->rootFailed.load(std::memory_order_acquire))
        return false;
    for (auto* r = detail::buckets[detail::Hash(rs)].load(std::memory_order_acquire); r; r = r->next)
        if (r->signature == rs && r->active.load(std::memory_order_acquire))
            return false;
    return true;
}

// The layout of a signature seen at creation. Null alone is NOT evidence of an unknown old object.
inline const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* Layout(ID3D12RootSignature* rs)
{
    auto* r = rs ? detail::buckets[detail::Hash(rs)].load(std::memory_order_acquire) : nullptr;
    for (; r && r->signature != rs; r = r->next)
    {
    }
    return r && r->active.load(std::memory_order_acquire) && (!r->device || spatial_tracking::Available(r->device))
               ? r->desc
               : nullptr;
}

inline Slot Find(ID3D12RootSignature* rs, D3D12_DESCRIPTOR_RANGE_TYPE type, UINT reg, UINT space, bool compute)
{
    const auto* layout = Layout(rs);
    if (!layout)
        return {};
    const auto& desc = *layout;
    const bool v1 = desc.Version == D3D_ROOT_SIGNATURE_VERSION_1_0;
    if (!v1 && desc.Version != D3D_ROOT_SIGNATURE_VERSION_1_1)
        return {};
    const auto flags = v1 ? desc.Desc_1_0.Flags : desc.Desc_1_1.Flags;
    if (!compute && (flags & D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS))
        return {};
    const UINT n = v1 ? desc.Desc_1_0.NumParameters : desc.Desc_1_1.NumParameters;
    if (n > 64)
        return {};
    Slot result;
    for (UINT i = 0; i < n; ++i)
    {
        const auto visibility =
            v1 ? desc.Desc_1_0.pParameters[i].ShaderVisibility : desc.Desc_1_1.pParameters[i].ShaderVisibility;
        if (visibility != D3D12_SHADER_VISIBILITY_ALL && (compute || visibility != D3D12_SHADER_VISIBILITY_PIXEL))
            continue;
        const auto parameterType =
            v1 ? desc.Desc_1_0.pParameters[i].ParameterType : desc.Desc_1_1.pParameters[i].ParameterType;
        Slot match;
        if (parameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        {
            const UINT ranges = v1 ? desc.Desc_1_0.pParameters[i].DescriptorTable.NumDescriptorRanges
                                   : desc.Desc_1_1.pParameters[i].DescriptorTable.NumDescriptorRanges;
            uint64_t append = 0;
            for (UINT j = 0; j < ranges; ++j)
            {
                D3D12_DESCRIPTOR_RANGE range;
                if (v1)
                    range = desc.Desc_1_0.pParameters[i].DescriptorTable.pDescriptorRanges[j];
                else
                {
                    const auto& x = desc.Desc_1_1.pParameters[i].DescriptorTable.pDescriptorRanges[j];
                    range = { x.RangeType, x.NumDescriptors, x.BaseShaderRegister, x.RegisterSpace,
                              x.OffsetInDescriptorsFromTableStart };
                }
                const uint64_t at = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                        ? append
                                        : range.OffsetInDescriptorsFromTableStart;
                const uint64_t delta = uint64_t(reg) - range.BaseShaderRegister;
                if (range.RangeType == type && range.RegisterSpace == space && reg >= range.BaseShaderRegister &&
                    delta < range.NumDescriptors && at + delta <= UINT32_MAX)
                {
                    if (match.kind != Slot::None)
                        return {};
                    match = { Slot::Table, uint8_t(i), uint32_t(at + delta),
                              range.NumDescriptors == UINT32_MAX ? UINT32_MAX
                                                                 : range.NumDescriptors - uint32_t(delta) };
                }
                append = at + range.NumDescriptors;
            }
        }
        else if (parameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
        {
            const auto& x = v1 ? desc.Desc_1_0.pParameters[i].Constants : desc.Desc_1_1.pParameters[i].Constants;
            if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV && x.ShaderRegister == reg && x.RegisterSpace == space)
                match = { Slot::Constants, uint8_t(i), 0, x.Num32BitValues };
        }
        else if (detail::Type(parameterType) == type)
        {
            const UINT shaderRegister = v1 ? desc.Desc_1_0.pParameters[i].Descriptor.ShaderRegister
                                           : desc.Desc_1_1.pParameters[i].Descriptor.ShaderRegister;
            const UINT registerSpace = v1 ? desc.Desc_1_0.pParameters[i].Descriptor.RegisterSpace
                                          : desc.Desc_1_1.pParameters[i].Descriptor.RegisterSpace;
            if (shaderRegister == reg && registerSpace == space)
                match = { Slot::RootDescriptor, uint8_t(i), 0, 1 };
        }
        if (match.kind != Slot::None)
        {
            if (result.kind != Slot::None)
                return {};
            result = match;
        }
    }
    return result;
}
} // namespace native::rootsig
