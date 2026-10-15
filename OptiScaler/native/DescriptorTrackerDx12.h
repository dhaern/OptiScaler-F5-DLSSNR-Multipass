#pragma once
#include <d3d12.h>
#include <cstdint>
#include "SpatialTracking.h"

namespace native::descriptors
{
enum class Kind : uint8_t
{
    None,
    Cbv,
    Srv,
    Uav,
    Rtv
};
struct View
{
    ID3D12Resource* resource = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    uint32_t size = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // SRV, UAV and RTV dimension enums differ; this is their canonical D3D12_RESOURCE_DIMENSION.
    uint8_t kind = 0, dimension = 0, mip = 0, plane = 0;
    uint16_t firstSlice = 0, sliceCount = 0;
};
struct Heap;
struct HeapRef
{
    const Heap* heap = nullptr;
    UINT increment = 0;
};

void OnCreateHeap(ID3D12DescriptorHeap*, const D3D12_DESCRIPTOR_HEAP_DESC&, UINT increment, spatial_tracking::Device*);
HeapRef FindHeap(ID3D12DescriptorHeap*, spatial_tracking::Device*);
// Known metadata first. Unknown old heaps use bounded inline metadata, never a hot shadow allocation or migration.
HeapRef ResolveBoundHeap(ID3D12DescriptorHeap*, spatial_tracking::Device*);
// The handle increment of a CBV_SRV_UAV or RTV heap type, as the device gives it (0 until known)
UINT Increment(D3D12_DESCRIPTOR_HEAP_TYPE, spatial_tracking::Device*);
void OnCreateView(Kind, ID3D12Resource*, const void* viewDesc, D3D12_CPU_DESCRIPTOR_HANDLE, spatial_tracking::Device*);
void OnCopySimple(UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE,
                  spatial_tracking::Device*);
void OnCopy(UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE*,
            const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE, spatial_tracking::Device*);
bool Lookup(D3D12_CPU_DESCRIPTOR_HANDLE, View*, spatial_tracking::Device*);
bool LookupGpu(const HeapRef&, D3D12_GPU_DESCRIPTOR_HANDLE, View*);
} // namespace native::descriptors
