#include "pch.h"

#include "GenericDepth_Dx12.h"

#include <native/DepthFinderCore.h>
#include <native/DepthResolveDx12.h>
#include <native/DepthSnapshotDx12.h>

#include <Config.h>
#include <Util.h>
#include <dlssnr/DlssNr_NativeMode.h>

#include <detours/detours.h>

#include <menu/menu_overlay_dx.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef STDMETHODCALLTYPE
#include <Unknwn.h>
#endif

// The counting rules (vertices weighed over draw calls, the viewport and workload tests on clears, the best snapshot, the
// reversed-Z hint from the clear value) are adapted from ReShade's Generic Depth add-on, Copyright (C) 2021 Patrick Mours,
// BSD-3-Clause; see Licenses/ReShade_GenericDepth_LICENSE.txt. This file only observes: it copies nothing yet.

namespace
{
typedef void(STDMETHODCALLTYPE* PFN_CreateDepthStencilView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                            const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescriptorsSimple)(ID3D12Device* This, UINT NumDescriptors,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                                           D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescriptors)(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                                     const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                                     const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                                     const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                                     const UINT* pSrcDescriptorRangeSizes,
                                                     D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                                        BOOL RTsSingleHandleToDescriptorRange,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_ClearDepthStencilView)(ID3D12GraphicsCommandList* This,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
                                                           D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil,
                                                           UINT NumRects, const D3D12_RECT* pRects);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance,
                                                   UINT InstanceCount, UINT StartVertexLocation,
                                                   UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D12GraphicsCommandList* This, UINT NumViewports,
                                                    const D3D12_VIEWPORT* pViewports);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX, UINT ThreadGroupCountY,
                                              UINT ThreadGroupCountZ);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteBundle)(ID3D12GraphicsCommandList* This,
                                                   ID3D12GraphicsCommandList* pCommandList);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteIndirect)(ID3D12GraphicsCommandList* This,
                                                     ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount,
                                                     ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset,
                                                     ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset);

PFN_CreateDepthStencilView o_CreateDepthStencilView = nullptr;
PFN_CopyDescriptorsSimple o_CopyDescriptorsSimple = nullptr;
PFN_CopyDescriptors o_CopyDescriptors = nullptr;
PFN_OMSetRenderTargets o_OMSetRenderTargets = nullptr;
PFN_ClearDepthStencilView o_ClearDepthStencilView = nullptr;
PFN_DrawInstanced o_DrawInstanced = nullptr;
PFN_DrawIndexedInstanced o_DrawIndexedInstanced = nullptr;
PFN_RSSetViewports o_RSSetViewports = nullptr;
PFN_ExecuteIndirect o_ExecuteIndirect = nullptr;
PFN_ExecuteBundle o_ExecuteBundle = nullptr;
PFN_Dispatch o_Dispatch = nullptr;
typedef HRESULT(STDMETHODCALLTYPE* PFN_Close)(ID3D12GraphicsCommandList* This);
PFN_Close o_Close = nullptr;

struct DsvInfo
{
    ID3D12Resource* resource = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool readOnlyDepth = false; // the view is read-only for depth, so the buffer is in the depth-read state while bound
};

// The API-neutral half of the finder: counting, picking, warm-up, stand-down. This file is the D3D12 adapter around it.
native::DepthFinderCore g_core;
std::atomic<bool> g_overlayOn { false };

// The geometry of the copies of the picked depth buffer, and the readback of the chosen one for the menu's preview (guarded
// by g_mutex like the rest).
struct Backup
{
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource* readback = nullptr;                 // the same copy in CPU-readable memory, for the on-screen preview
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};    // where the first plane sits in it
    UINT64 readbackBytes = 0;                           // its size: the last row is not padded to RowPitch
    ID3D12Device* device = nullptr; // the device they were made on (not held): another needs its own
    // A multisampled buffer. On a device that can resolve a region (DepthResolveSupported) and a buffer with quality 0
    // or the standard pattern: the game's list resolves it to its nearest sample straight into a single-sample slot of
    // the same format family (native/DepthSnapshotDx12.h), so the copies are read like a single-sample buffer's.
    // Otherwise: the list copies it as it is into a multisampled slot of this family and these samples, and the resolve
    // (native/DepthResolveDx12) turns that into the R32 copy described above (typeless R32_TYPELESS, view R32_FLOAT).
    DXGI_SAMPLE_DESC samples { 1, 0 };
    bool resolveInList = false;
    DXGI_FORMAT multisampledTypeless = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT multisampledView = DXGI_FORMAT_UNKNOWN;
};

std::mutex g_mutex;
std::unordered_map<SIZE_T, DsvInfo> g_dsv;                       // CPU descriptor handle -> what it views
Backup g_backup;
uint64_t g_previewFrame = 0;         // the frame the preview's readback was last filled in

// A frame's copies of the picked buffer. Game command lists are recorded in no fixed order, so one shared target ended up
// holding whichever copy ran last on the GPU (the world, or the first-person weapon's pass) and the picture flickered. Each
// stretch with a real share of the frame's draws gets a target of its own, and at present the one whose stretch drew the most
// is chosen: that choice does not depend on the order the lists ran in.
constexpr int kSnapshotSlots = 8;

struct SnapshotSlot
{
    ID3D12Resource* resource = nullptr;
    ID3D12Resource* multisampled = nullptr; // a multisampled buffer's copy as it is; `resource` is its resolve
    uint64_t vertices = 0;           // what the stretch it holds had drawn
    bool reversed = false; // the pick's depth direction when it was taken (picks which sample a resolve keeps)
};

SnapshotSlot g_slots[kSnapshotSlots];
int g_slotsUsed = 0;                 // taken this frame
GenericDepthDx12::Snapshot g_best;   // the frame just closed's choice
int g_bestSlots[kSnapshotSlots] = {}; // which slots g_best.copies are, in the same order
uint64_t g_resolvedFrame = ~0ull;     // the g_best.frame whose multisampled copies were resolved
ID3D12Device* g_resolveDevice = nullptr; // the device g_deviceResolves was asked of (not held)
bool g_deviceResolves = false;          // programmable sample positions tier 1: may resolve in the game's list
// Made once and never destroyed: at unload the device may already be gone, and its destructor waits on the GPU.
native::DepthResolveDx12& Resolver()
{
    static auto* resolve = new native::DepthResolveDx12;
    return *resolve;
}
std::vector<std::pair<ID3D12Resource*, uint64_t>> g_retired; // replaced copies, released a few frames later
UINT g_dsvIncrement = 0;
bool g_installed = false;
bool g_installFailed = false;

// Under g_mutex. Why a copy of the picked buffer, or the preview's readback of it, was not made: said once per reason, since
// each fails the same way every frame and the preview otherwise only says it is waiting.
void SayOnce(const char* key, const std::string& line)
{
    static std::vector<const char*> said;

    if (std::find(said.begin(), said.end(), key) != said.end())
        return;

    said.push_back(key);
    LOG_INFO("Depth finder: {}", line);
}

void OnDraw(ID3D12GraphicsCommandList* list, uint64_t vertices, uint32_t instances)
{
    g_core.OnDraw((uint64_t) (size_t) list, vertices, instances);
}

// The typeless format a copy of a depth format is made in, and the format its depth plane is read through.
bool BackupFormats(DXGI_FORMAT depth, DXGI_FORMAT* typeless, DXGI_FORMAT* view)
{
    switch (depth)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        *typeless = DXGI_FORMAT_R32_TYPELESS;
        *view = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        *typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
        *view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        *typeless = DXGI_FORMAT_R24G8_TYPELESS;
        *view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        *typeless = DXGI_FORMAT_R16_TYPELESS;
        *view = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}

// Under g_mutex. Gets the geometry right when the depth buffer's size or format is new, retiring the old copies for a few
// frames (the menu or the motion step may still be reading them), and makes the preview's readback.
bool EnsureBackup(ID3D12Device* device, const D3D12_RESOURCE_DESC& source, DXGI_FORMAT typeless, DXGI_FORMAT view)
{
    // A multisampled buffer is resolved in the game's list into its own format family when the ini allows it, the
    // device can, the format family is one that resolves right (native::DepthResolveFormat: not D24S8) and the pattern
    // is quality 0 or the standard one (MIN/MAX does not depend on where the samples are). Any other is copied as it is
    // and resolved into R32 afterwards: that is then the format of the copies read and of the readback.
    const bool multisampled = source.SampleDesc.Count > 1;

    if (multisampled && device != g_resolveDevice)
    {
        g_resolveDevice = device;
        g_deviceResolves = native::DepthResolveSupported(device);
        LOG_INFO("Depth finder: the device {} resolve multisampled depth in the game's lists{}",
                 g_deviceResolves ? "may" : "may not",
                 g_deviceResolves ? "" : " (no programmable sample positions tier 1)");
    }

    const bool standardPattern =
        source.SampleDesc.Quality == 0 || source.SampleDesc.Quality == D3D12_STANDARD_MULTISAMPLE_PATTERN;
    const bool resolveInList = multisampled && standardPattern && g_deviceResolves &&
                               native::DepthResolveFormat(typeless) &&
                               Config::Instance()->DlssNrNativeDepthResolveInList.value_or_default();
    const bool resolveLater = multisampled && !resolveInList;
    const DXGI_FORMAT multisampledTypeless = resolveLater ? typeless : DXGI_FORMAT_UNKNOWN;
    const DXGI_FORMAT multisampledView = resolveLater ? view : DXGI_FORMAT_UNKNOWN;

    if (resolveLater)
    {
        typeless = DXGI_FORMAT_R32_TYPELESS;
        view = DXGI_FORMAT_R32_FLOAT;
    }

    if (g_backup.width == source.Width && g_backup.height == source.Height && g_backup.typeless == typeless &&
        g_backup.samples.Count == source.SampleDesc.Count && g_backup.samples.Quality == source.SampleDesc.Quality &&
        g_backup.multisampledTypeless == multisampledTypeless && g_backup.resolveInList == resolveInList &&
        g_backup.device == device && g_backup.readback != nullptr)
        return true;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = source.Width;
    desc.Height = source.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = typeless;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    // The preview is drawn on the CPU from a readback of the chosen copy: depth is mostly near zero, which no plain texture
    // view shows brightly, so it needs a curve.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

    ID3D12Resource* readback = nullptr;

    if (total != 0)
    {
        D3D12_HEAP_PROPERTIES readHeap {};
        readHeap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDesc {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = total;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        const HRESULT hr = device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));

        if (FAILED(hr))
        {
            readback = nullptr;
            SayOnce("readback", std::format("the preview's readback ({} bytes) could not be made (0x{:X}); the copies "
                                            "still go to the motion step",
                                            total, (unsigned) hr));
        }
    }
    else
    {
        SayOnce("footprint", std::format("no copyable footprint for a {}x{} buffer of format {}; the preview stays empty",
                                         source.Width, source.Height, (int) typeless));
    }

    for (auto& slot : g_slots)
    {
        if (slot.resource != nullptr)
            g_retired.emplace_back(slot.resource, g_core.Presents());

        if (slot.multisampled != nullptr)
            g_retired.emplace_back(slot.multisampled, g_core.Presents());

        slot = SnapshotSlot {};
    }

    if (g_backup.readback != nullptr)
        g_retired.emplace_back(g_backup.readback, g_core.Presents());

    g_slotsUsed = 0;
    g_best = GenericDepthDx12::Snapshot {};
    g_previewFrame = 0;
    g_resolvedFrame = ~0ull;
    g_backup = Backup { (uint32_t) source.Width, source.Height, typeless, view, readback, footprint, total };
    g_backup.samples = source.SampleDesc;
    g_backup.device = device;
    g_backup.resolveInList = resolveInList;
    g_backup.multisampledTypeless = multisampledTypeless;
    g_backup.multisampledView = multisampledView;
    return true;
}

constexpr D3D12_RESOURCE_STATES kSlotRest =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// Under g_mutex. A slot to copy into: made when it is first needed, in the state it rests in between frames. The depth is
// read by compute passes and, for the menu, pixel shaders, so it rests readable by both.
SnapshotSlot* TakeSlot(ID3D12Device* device, const D3D12_RESOURCE_DESC& source)
{
    if (g_slotsUsed >= kSnapshotSlots)
        return nullptr;

    SnapshotSlot& slot = g_slots[g_slotsUsed];

    if (slot.resource == nullptr)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = source.Width;
        desc.Height = source.Height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = g_backup.typeless;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // Resolved in the game's list: a plain single-sample slot, like a single-sample buffer's.
        const bool multisampled = g_backup.samples.Count > 1 && !g_backup.resolveInList;

        // The compute resolve writes a multisampled buffer's copy into it.
        if (multisampled)
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kSlotRest, nullptr,
                                                   IID_PPV_ARGS(&slot.resource))))
            return nullptr;

        // The copy as it is: the same format family and samples as the game's buffer, resting where the resolve reads
        // it.
        if (multisampled)
        {
            desc.Format = g_backup.multisampledTypeless;
            desc.SampleDesc = g_backup.samples;
            // A multisampled texture must be one a depth (or render) target can be made of, even if none ever is.
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                       IID_PPV_ARGS(&slot.multisampled))))
            {
                slot.resource->Release();
                slot.resource = nullptr;
                return nullptr;
            }
        }
    }

    ++g_slotsUsed;
    return &slot;
}

// Under g_mutex, from a hook, before the clear itself or at the point the list leaves the buffer or closes: the buffer
// is in the state its view says, so it goes to copy-source and back around one copy of its first subresource into a
// slot. A multisampled buffer with a standard pattern goes to resolve-source instead, resolved to its nearest sample
// straight into the slot; `reversed` (the pick's direction) says which sample is nearest.
void RecordSnapshot(ID3D12GraphicsCommandList* list, ID3D12Resource* source, bool readOnlyDepth, bool reversed,
                    const char* where = "", uint64_t stretchVertices = 0)
{
    // The first copies of a frame in a few frames, with what they were taken after.
    static uint64_t loggedFrames = 0, lastFrame = ~0ull;
    static int inFrame = 0;

    if (g_core.Presents() != lastFrame)
    {
        lastFrame = g_core.Presents();
        inFrame = 0;

        if (g_core.Presents() % 600 == 0)
            loggedFrames = g_core.Presents(); // a burst every 600 presents
    }

    // Debug level: this runs under g_mutex on the game's recording threads, and a log line is a synchronous file write
    if (g_core.Presents() - loggedFrames < 2 && inFrame++ < 8)
        LOG_DEBUG("Depth finder: frame {} copy at {} on list {:X}, stretch {} vertices (floor {})", g_core.Presents(),
                  where, (size_t) list, stretchVertices, g_core.SnapshotFloor());

    const auto depthState = readOnlyDepth ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE;

    const auto desc = source->GetDesc();

    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;

    if (!BackupFormats(desc.Format, &typeless, &view))
    {
        SayOnce("format", std::format("the picked buffer's resource format {} cannot be copied", (int) desc.Format));
        return;
    }

    ID3D12Device* device = nullptr;

    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
    {
        SayOnce("device", "a game command list gave no device; the copy is skipped");
        return;
    }

    if (!EnsureBackup(device, desc, typeless, view))
    {
        device->Release();
        return;
    }

    // Resolved in the list: only a direct list can, and every list that binds a depth buffer is one. Checked before a
    // slot is taken, so a skipped copy leaves no empty slot behind.
    if (g_backup.resolveInList)
    {
        ID3D12GraphicsCommandList1* list1 = nullptr;
        const bool canResolve = list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT &&
                                SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list1))) && list1 != nullptr;

        if (list1 != nullptr)
            list1->Release();

        if (!canResolve)
        {
            device->Release();
            SayOnce("resolveList", "a copy was asked on a list that cannot resolve (not a direct list, or no "
                                   "ID3D12GraphicsCommandList1); it is skipped");
            return;
        }
    }

    SnapshotSlot* slot = TakeSlot(device, desc);
    device->Release();

    if (slot == nullptr)
    {
        SayOnce("slot", g_slotsUsed >= kSnapshotSlots
                            ? std::format("more than {} copies in one frame; the rest are skipped", kSnapshotSlots)
                            : std::string("a copy target could not be made; the copy is skipped"));
        return;
    }

    SayOnce("copied",
            std::format("first copy of the picked buffer taken: {}x{}, resource format {}, copied as {}{}", desc.Width,
                        desc.Height, (int) desc.Format, (int) typeless,
                        desc.SampleDesc.Count > 1
                            ? std::format(" with its {} samples, resolved to the nearest {}", desc.SampleDesc.Count,
                                          g_backup.resolveInList ? "in the game's list" : "afterwards")
                            : std::string()));

    slot->vertices = stretchVertices;
    slot->reversed = reversed;

    // Standard pattern: one resolve in the game's list straight into the slot, no multisampled copy and no extra pass.
    if (g_backup.resolveInList)
    {
        if (const char* failed = native::RecordDepthResolve(list, source, depthState, slot->resource, kSlotRest,
                                                            g_backup.view, reversed))
        {
            // Checked above, so not expected. Nothing was recorded and the slot (the last one taken) holds no depth of
            // this frame: give it back so it is not read.
            --g_slotsUsed;
            SayOnce("resolveFailed", std::format("a copy could not be resolved in the game's list ({})", failed));
        }

        return;
    }

    auto barrier = [](ID3D12Resource* r, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = subresource;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };

    // A multisampled buffer goes into its multisampled slot as it is (a whole subresource, as such a copy must be); the
    // resolve runs later on a list of our own. Nothing but barriers and this copy is recorded into the game's list.
    ID3D12Resource* const target = slot->multisampled != nullptr ? slot->multisampled : slot->resource;
    const D3D12_RESOURCE_STATES targetRest =
        slot->multisampled != nullptr ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : kSlotRest;

    // Only the source's depth plane (subresource 0) is copied, so only it changes state: the stencil plane may rest in
    // another one (read-only depth with writable stencil). The target is ours and rests whole in targetRest.
    constexpr UINT all = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_RESOURCE_BARRIER in[2] = { barrier(source, 0, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                     barrier(target, all, targetRest, D3D12_RESOURCE_STATE_COPY_DEST) };
    list->ResourceBarrier(2, in);

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = target;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER out[2] = { barrier(source, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState),
                                      barrier(target, all, D3D12_RESOURCE_STATE_COPY_DEST, targetRest) };
    list->ResourceBarrier(2, out);
}

void STDMETHODCALLTYPE hkCreateDepthStencilView(ID3D12Device* This, ID3D12Resource* pResource,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    o_CreateDepthStencilView(This, pResource, pDesc, DestDescriptor);

    g_core.Counters().depthViewsCreated.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard lock(g_mutex);

    if (pResource == nullptr)
    {
        g_dsv.erase(DestDescriptor.ptr);
        return;
    }

    const auto desc = pResource->GetDesc();
    const bool readOnly = pDesc != nullptr && (pDesc->Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0;
    g_dsv[DestDescriptor.ptr] = DsvInfo { pResource, (uint32_t) desc.Width, desc.Height, desc.Format, readOnly };
}

void CopyDsv(SIZE_T dest, SIZE_T source)
{
    const auto found = g_dsv.find(source);

    if (found != g_dsv.end())
    {
        const auto info = found->second;
        g_dsv[dest] = info;
    }
    else
    {
        g_dsv.erase(dest);
    }
}

void STDMETHODCALLTYPE hkCopyDescriptorsSimple(ID3D12Device* This, UINT NumDescriptors,
                                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                               D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                               D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    o_CopyDescriptorsSimple(This, NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart,
                            DescriptorHeapsType);

    if (DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || g_dsvIncrement == 0)
        return;

    std::lock_guard lock(g_mutex);

    for (UINT i = 0; i < NumDescriptors; ++i)
        CopyDsv(DestDescriptorRangeStart.ptr + (SIZE_T) i * g_dsvIncrement,
                SrcDescriptorRangeStart.ptr + (SIZE_T) i * g_dsvIncrement);
}

void STDMETHODCALLTYPE hkCopyDescriptors(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                         const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                         const UINT* pSrcDescriptorRangeSizes,
                                         D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    o_CopyDescriptors(This, NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                      NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
                      DescriptorHeapsType);

    if (DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || g_dsvIncrement == 0 ||
        pDestDescriptorRangeStarts == nullptr || pSrcDescriptorRangeStarts == nullptr)
        return;

    std::lock_guard lock(g_mutex);

    // The copy walks the two lists of ranges in step, one descriptor at a time; a null size array means one each.
    UINT destRange = 0, destUsed = 0, srcRange = 0, srcUsed = 0;

    while (destRange < NumDestDescriptorRanges && srcRange < NumSrcDescriptorRanges)
    {
        const UINT destSize = pDestDescriptorRangeSizes != nullptr ? pDestDescriptorRangeSizes[destRange] : 1;
        const UINT srcSize = pSrcDescriptorRangeSizes != nullptr ? pSrcDescriptorRangeSizes[srcRange] : 1;

        CopyDsv(pDestDescriptorRangeStarts[destRange].ptr + (SIZE_T) destUsed * g_dsvIncrement,
                pSrcDescriptorRangeStarts[srcRange].ptr + (SIZE_T) srcUsed * g_dsvIncrement);

        if (++destUsed >= destSize)
        {
            ++destRange;
            destUsed = 0;
        }

        if (++srcUsed >= srcSize)
        {
            ++srcRange;
            srcUsed = 0;
        }
    }
}

// What each hook does, apart from calling on to the original: shared by the hooks on the runtime's own functions above and
// by the thunks that go into a game command list's private vtable below. The counting itself is the core's; what stays here is
// what only D3D12 can do: look a descriptor up, and record the copy the core asks for onto the list being recorded.
void TakeSnapshot(ID3D12GraphicsCommandList* list, const native::SnapshotRequest& request)
{
    if (!request.take)
        return;

    std::lock_guard lock(g_mutex);
    RecordSnapshot(list, (ID3D12Resource*) (size_t) request.id, request.readOnlyDepth, request.reversed, request.where,
                   request.stretchVertices);
}

void OnViewports(ID3D12GraphicsCommandList* This, UINT NumViewports, const D3D12_VIEWPORT* pViewports)
{
    if (NumViewports > 0 && pViewports != nullptr)
        g_core.OnViewport((uint64_t) (size_t) This, pViewports[0].Width);
}

void OnClear(ID3D12GraphicsCommandList* This, D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
             FLOAT Depth)
{
    if ((ClearFlags & D3D12_CLEAR_FLAG_DEPTH) == 0 || !g_core.Active())
        return;

    native::DepthBuffer buffer;

    {
        std::lock_guard lock(g_mutex);

        const auto found = g_dsv.find(DepthStencilView.ptr);

        if (found == g_dsv.end() || found->second.resource == nullptr)
            return;

        buffer.id = (uint64_t) (size_t) found->second.resource;
        buffer.width = found->second.width;
        buffer.height = found->second.height;
        buffer.format = (uint32_t) found->second.format;
        buffer.readOnlyDepth = found->second.readOnlyDepth;
    }

    TakeSnapshot(This, g_core.OnDepthClear((uint64_t) (size_t) This, buffer, Depth));
}

void OnIndirect(ID3D12GraphicsCommandList* This, UINT MaxCommandCount)
{
    g_core.OnIndirect((uint64_t) (size_t) This, MaxCommandCount);
}

// A list that ends while still bound to the picked buffer never unbinds it, so the stretch drawn since its last clear (Cyberpunk
// draws the world after the clear and ends the list there) is copied here, in the state the view says the buffer is in.
void OnClose(ID3D12GraphicsCommandList* This)
{
    TakeSnapshot(This, g_core.OnContextEnd((uint64_t) (size_t) This));
}

// ---- Game command lists with a vtable of their own ----------------------------------------------------------------------
// Witcher 3 (2026-10-04): its command lists each carry a copy of the vtable in heap memory whose DrawInstanced and
// DrawIndexedInstanced point into another module's code, while OMSetRenderTargets is the runtime's own. The hooks on the
// runtime's functions then never see a draw. So the first time a list is seen, an entry of its table that is not the
// runtime's own is replaced by a thunk that counts and calls what was there. A table whose entries are the runtime's own is
// left alone: the hooks above already cover it, and patching it as well would count every draw twice.
enum Slot
{
    kDraw = 0,
    kDrawIndexed,
    kViewports,
    kClear,
    kIndirect,
    kClose,
    kSlots
};
constexpr int kSlotIndex[kSlots] = { 12, 13, 21, 47, 59, 9 };
constexpr const char* kSlotName[kSlots] = { "DrawInstanced", "DrawIndexedInstanced", "RSSetViewports",
                                            "ClearDepthStencilView", "ExecuteIndirect", "Close" };

struct TablePatch
{
    PVOID previous[kSlots] {};
};

PVOID* g_installTable = nullptr; // the runtime's own table, read at install
std::unordered_map<PVOID*, TablePatch> g_patched;
std::unordered_map<PVOID*, bool> g_examined;
int g_patchLogs = 0;

PVOID PreviousOf(ID3D12GraphicsCommandList* list, int slot)
{
    PVOID* table = *(PVOID**) list;
    std::lock_guard lock(g_mutex);
    const auto found = g_patched.find(table);
    return found != g_patched.end() ? found->second.previous[slot] : nullptr;
}

void STDMETHODCALLTYPE hkTableDrawInstanced(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance,
                                            UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
    OnDraw(This, VertexCountPerInstance, InstanceCount);

    if (auto previous = (PFN_DrawInstanced) PreviousOf(This, kDraw))
        previous(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE hkTableDrawIndexedInstanced(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                                   UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
                                                   UINT StartInstanceLocation)
{
    OnDraw(This, IndexCountPerInstance, InstanceCount);

    if (auto previous = (PFN_DrawIndexedInstanced) PreviousOf(This, kDrawIndexed))
        previous(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                 StartInstanceLocation);
}

void STDMETHODCALLTYPE hkTableRSSetViewports(ID3D12GraphicsCommandList* This, UINT NumViewports,
                                             const D3D12_VIEWPORT* pViewports)
{
    OnViewports(This, NumViewports, pViewports);

    if (auto previous = (PFN_RSSetViewports) PreviousOf(This, kViewports))
        previous(This, NumViewports, pViewports);
}

void STDMETHODCALLTYPE hkTableClearDepthStencilView(ID3D12GraphicsCommandList* This,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
                                                    D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil,
                                                    UINT NumRects, const D3D12_RECT* pRects)
{
    if (g_core.Active())
        OnClear(This, DepthStencilView, ClearFlags, Depth);

    if (auto previous = (PFN_ClearDepthStencilView) PreviousOf(This, kClear))
        previous(This, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
}

void STDMETHODCALLTYPE hkTableExecuteIndirect(ID3D12GraphicsCommandList* This, ID3D12CommandSignature* pCommandSignature,
                                              UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer,
                                              UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer,
                                              UINT64 CountBufferOffset)
{
    if (g_core.Active())
        OnIndirect(This, MaxCommandCount);

    if (auto previous = (PFN_ExecuteIndirect) PreviousOf(This, kIndirect))
        previous(This, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer,
                 CountBufferOffset);
}

HRESULT STDMETHODCALLTYPE hkTableClose(ID3D12GraphicsCommandList* This)
{
    OnClose(This);

    if (auto previous = (PFN_Close) PreviousOf(This, kClose))
        return previous(This);

    return E_FAIL;
}

const PVOID kTableHooks[kSlots] = { (PVOID) hkTableDrawInstanced, (PVOID) hkTableDrawIndexedInstanced,
                                    (PVOID) hkTableRSSetViewports, (PVOID) hkTableClearDepthStencilView,
                                    (PVOID) hkTableExecuteIndirect, (PVOID) hkTableClose };

void PatchListTable(ID3D12GraphicsCommandList* list)
{
    PVOID* table = *(PVOID**) list;

    if (table == nullptr || table == g_installTable)
        return;

    {
        std::lock_guard lock(g_mutex);

        if (!g_examined.emplace(table, true).second)
            return;
    }

    TablePatch patch;
    bool any = false;

    for (int slot = 0; slot < kSlots; ++slot)
    {
        PVOID entry = table[kSlotIndex[slot]];

        if (g_installTable != nullptr && entry != g_installTable[kSlotIndex[slot]] && entry != kTableHooks[slot])
        {
            patch.previous[slot] = entry;
            any = true;
        }
    }

    if (!any)
        return;

    {
        std::lock_guard lock(g_mutex);
        g_patched[table] = patch; // before the entries change, so a thunk that runs at once finds what to call
    }

    int patched = 0;

    for (int slot = 0; slot < kSlots; ++slot)
    {
        if (patch.previous[slot] == nullptr)
            continue;

        PVOID* entry = &table[kSlotIndex[slot]];
        DWORD old = 0;

        if (VirtualProtect(entry, sizeof(PVOID), PAGE_READWRITE, &old))
        {
            InterlockedExchangePointer(entry, kTableHooks[slot]);
            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(PVOID), old, &ignored);
            ++patched;
        }
    }

    if (g_patchLogs++ < 6)
    {
        std::string which;

        for (int slot = 0; slot < kSlots; ++slot)
            if (patch.previous[slot] != nullptr)
                which += std::format(" {}={:X}", kSlotName[slot], (size_t) patch.previous[slot]);

        LOG_INFO("Depth finder: a game command list has a vtable of its own ({:X}); {} entries now count through us, they "
                 "pointed at:{}",
                 (size_t) table, patched, which);
    }
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                            BOOL RTsSingleHandleToDescriptorRange,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    static std::atomic<int> logged { 0 };

    if (logged.load(std::memory_order_relaxed) < 4 && logged.fetch_add(1) < 4)
    {
        PVOID* table = *(PVOID**) This;
        LOG_INFO("Depth finder: a game command list {:X} uses vtable {:X} (hooks were read from {:X}, {}); DrawInstanced {:X} "
                 "(read {:X}), DrawIndexedInstanced {:X} (read {:X}), OMSetRenderTargets {:X} (read {:X})",
                 (size_t) This, (size_t) table, (size_t) g_installTable, table == g_installTable ? "the same" : "a DIFFERENT one",
                 (size_t) table[12], g_installTable ? (size_t) g_installTable[12] : 0, (size_t) table[13],
                 g_installTable ? (size_t) g_installTable[13] : 0, (size_t) table[46],
                 g_installTable ? (size_t) g_installTable[46] : 0);
    }

    if (g_core.Active())
        PatchListTable(This);

    if (g_core.Active())
    {
        native::DepthBuffer buffer;
        const native::DepthBuffer* bound = nullptr;

        if (pDepthStencilDescriptor != nullptr)
        {
            std::lock_guard lock(g_mutex);

            const auto found = g_dsv.find(pDepthStencilDescriptor->ptr);

            if (found != g_dsv.end() && found->second.resource != nullptr)
            {
                buffer.id = (uint64_t) (size_t) found->second.resource;
                buffer.width = found->second.width;
                buffer.height = found->second.height;
                buffer.format = (uint32_t) found->second.format;
                buffer.readOnlyDepth = found->second.readOnlyDepth;
                bound = &buffer;
            }
        }

        // The core says whether leaving the buffer this list was bound to is the moment to copy it.
        TakeSnapshot(This, g_core.OnDepthBound((uint64_t) (size_t) This, pDepthStencilDescriptor != nullptr, bound));
    }

    o_OMSetRenderTargets(This, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange,
                         pDepthStencilDescriptor);
}

void STDMETHODCALLTYPE hkRSSetViewports(ID3D12GraphicsCommandList* This, UINT NumViewports,
                                        const D3D12_VIEWPORT* pViewports)
{
    OnViewports(This, NumViewports, pViewports);

    o_RSSetViewports(This, NumViewports, pViewports);
}

void STDMETHODCALLTYPE hkClearDepthStencilView(ID3D12GraphicsCommandList* This,
                                               D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
                                               D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil,
                                               UINT NumRects, const D3D12_RECT* pRects)
{
    OnClear(This, DepthStencilView, ClearFlags, Depth);

    o_ClearDepthStencilView(This, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
}

void STDMETHODCALLTYPE hkDrawInstanced(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance,
                                       UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
    OnDraw(This, VertexCountPerInstance, InstanceCount);
    o_DrawInstanced(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                              UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
                                              UINT StartInstanceLocation)
{
    OnDraw(This, IndexCountPerInstance, InstanceCount);
    o_DrawIndexedInstanced(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                           StartInstanceLocation);
}

// ExecuteIndirect carries draws, dispatches and mesh dispatches alike, and the command signature that says which is not
// followed here: with a depth buffer bound it is counted as indirect draws (up to MaxCommandCount), which is a guess for a
// dispatch issued while one happens to be bound. ReShade's add-on tells them apart through the API's own indirect type.
void STDMETHODCALLTYPE hkExecuteIndirect(ID3D12GraphicsCommandList* This, ID3D12CommandSignature* pCommandSignature,
                                         UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer,
                                         UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer,
                                         UINT64 CountBufferOffset)
{
    OnIndirect(This, MaxCommandCount);

    o_ExecuteIndirect(This, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer,
                      CountBufferOffset);
}

HRESULT STDMETHODCALLTYPE hkClose(ID3D12GraphicsCommandList* This)
{
    OnClose(This);

    return o_Close(This);
}

void STDMETHODCALLTYPE hkDispatch(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX, UINT ThreadGroupCountY,
                                    UINT ThreadGroupCountZ)
{
    if (g_core.Active())
        g_core.Counters().dispatches.fetch_add(1, std::memory_order_relaxed);

    o_Dispatch(This, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

void STDMETHODCALLTYPE hkExecuteBundle(ID3D12GraphicsCommandList* This, ID3D12GraphicsCommandList* pCommandList)
{
    if (g_core.Active())
        g_core.Counters().bundles.fetch_add(1, std::memory_order_relaxed);

    o_ExecuteBundle(This, pCommandList);
}

} // namespace

namespace GenericDepthDx12
{
bool Installed() { return g_installed; }
bool InstallFailed() { return g_installFailed; }

void Install(ID3D12Device* device)
{
    if (device == nullptr || g_installed || !Config::Instance()->DlssNrNativeDepthFinder.value_or_default())
        return;

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
    {
        LOG_WARN("Depth finder: could not make a command allocator, not installed");
        g_installFailed = true;
        return;
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))))
    {
        allocator->Release();
        LOG_WARN("Depth finder: could not make a command list, not installed");
        g_installFailed = true;
        return;
    }

    ID3D12GraphicsCommandList* realList = nullptr;

    if (!Util::CheckForRealObject(__FUNCTION__, list, (IUnknown**) &realList))
        realList = list;

    ID3D12Device* realDevice = nullptr;

    if (!Util::CheckForRealObject(__FUNCTION__, device, (IUnknown**) &realDevice))
        realDevice = device;

    PVOID* deviceTable = *(PVOID**) realDevice;
    PVOID* listTable = *(PVOID**) realList;

    o_CreateDepthStencilView = (PFN_CreateDepthStencilView) deviceTable[21];
    o_CopyDescriptors = (PFN_CopyDescriptors) deviceTable[23];
    o_CopyDescriptorsSimple = (PFN_CopyDescriptorsSimple) deviceTable[24];

    // ID3D12GraphicsCommandList vtable: DrawInstanced 12, DrawIndexedInstanced 13, RSSetViewports 21,
    // OMSetRenderTargets 46, ClearDepthStencilView 47, ExecuteIndirect 59 (checked against the SDK header with offsetof).
    o_DrawInstanced = (PFN_DrawInstanced) listTable[12];
    o_DrawIndexedInstanced = (PFN_DrawIndexedInstanced) listTable[13];
    o_RSSetViewports = (PFN_RSSetViewports) listTable[21];
    o_OMSetRenderTargets = (PFN_OMSetRenderTargets) listTable[46];
    o_ClearDepthStencilView = (PFN_ClearDepthStencilView) listTable[47];
    o_ExecuteIndirect = (PFN_ExecuteIndirect) listTable[59];
    o_ExecuteBundle = (PFN_ExecuteBundle) listTable[27];
    o_Dispatch = (PFN_Dispatch) listTable[14];
    o_Close = (PFN_Close) listTable[9];
    g_installTable = listTable;

    // Is a bundle's DrawInstanced the same code as the direct list's? If not, draws recorded in bundles are invisible to the
    // draw hooks (a bundle inherits the caller's depth buffer, so they would have to be counted at ExecuteBundle).
    {
        ID3D12CommandAllocator* bundleAllocator = nullptr;
        ID3D12GraphicsCommandList* bundle = nullptr;

        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundleAllocator))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundleAllocator, nullptr,
                                                IID_PPV_ARGS(&bundle))))
        {
            ID3D12GraphicsCommandList* realBundle = nullptr;

            if (!Util::CheckForRealObject(__FUNCTION__, bundle, (IUnknown**) &realBundle))
                realBundle = bundle;

            PVOID* bundleTable = *(PVOID**) realBundle;
            g_core.Counters().bundleSameDraw = (bundleTable[12] == listTable[12] && bundleTable[13] == listTable[13]) ? 1 : 0;
            bundle->Close();
        }

        if (bundle != nullptr)
            bundle->Release();
        if (bundleAllocator != nullptr)
            bundleAllocator->Release();
    }

    g_dsvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    DetourAttach(&(PVOID&) o_CreateDepthStencilView, hkCreateDepthStencilView);
    DetourAttach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);
    DetourAttach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);
    DetourAttach(&(PVOID&) o_DrawInstanced, hkDrawInstanced);
    DetourAttach(&(PVOID&) o_DrawIndexedInstanced, hkDrawIndexedInstanced);
    DetourAttach(&(PVOID&) o_RSSetViewports, hkRSSetViewports);
    DetourAttach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);
    DetourAttach(&(PVOID&) o_ClearDepthStencilView, hkClearDepthStencilView);
    DetourAttach(&(PVOID&) o_ExecuteIndirect, hkExecuteIndirect);
    DetourAttach(&(PVOID&) o_ExecuteBundle, hkExecuteBundle);
    DetourAttach(&(PVOID&) o_Dispatch, hkDispatch);
    DetourAttach(&(PVOID&) o_Close, hkClose);

    const auto result = DetourTransactionCommit();

    list->Close();
    list->Release();
    allocator->Release();

    if (result != NO_ERROR)
    {
        LOG_ERROR("Depth finder: hooking failed ({:X}), not installed", (UINT) result);
        o_CreateDepthStencilView = nullptr;
        o_CopyDescriptors = nullptr;
        o_CopyDescriptorsSimple = nullptr;
        o_DrawInstanced = nullptr;
        o_DrawIndexedInstanced = nullptr;
        o_RSSetViewports = nullptr;
        o_OMSetRenderTargets = nullptr;
        o_ClearDepthStencilView = nullptr;
        o_ExecuteIndirect = nullptr;
        o_ExecuteBundle = nullptr;
        o_Dispatch = nullptr;
        o_Close = nullptr;
        g_installFailed = true;
        return;
    }

    g_installFailed = false;
    g_overlayOn = Config::Instance()->DlssNrNativeDepthOverlay.value_or_default() &&
                  Config::Instance()->DlssNrNativeDebugView.value_or_default();
    g_core.Start([](const std::string& line) { LOG_INFO("{}", line); });
    g_installed = true;
    LOG_INFO("Depth finder: observing the game's depth buffers{}, after {} frames of warm-up; it stands down if the game "
             "makes an upscaler call",
             g_overlayOn.load() ? " (the overlay's copy is recorded into the game's command list)"
                                : " (nothing is changed)",
             Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default());
}

void OnPresent(IDXGISwapChain* swapChain)
{
    if (!g_installed || swapChain == nullptr)
        return;

    DXGI_SWAP_CHAIN_DESC desc {};

    if (FAILED(swapChain->GetDesc(&desc)))
        return;

    // Part one in the core: the frame's counts are closed and the floor for what is worth copying is found.
    const uint64_t presents = g_core.BeginPresent(desc.BufferDesc.Width, desc.BufferDesc.Height);
    const auto pick = g_core.CurrentPick();

    {
        std::lock_guard lock(g_mutex);

        // The frame's copy of the picked buffer: the stretch that drew the most, whatever order the lists ran in.
        g_best = GenericDepthDx12::Snapshot {};

        int chosen = -1;

        for (int i = 0; i < g_slotsUsed; ++i)
            if (chosen < 0 || g_slots[i].vertices > g_slots[chosen].vertices)
                chosen = i;

        // The slots were taken with the direction the pick had then; it changes only between frames, so a frame whose
        // slots disagree straddled that change and is left without depth rather than read the wrong way.
        bool sameDirection = true;

        for (int i = 0; i < g_slotsUsed; ++i)
            sameDirection = sameDirection && g_slots[i].reversed == g_slots[0].reversed;

        if (!sameDirection)
            SayOnce("direction",
                    "a frame's copies were taken with different depth directions; that frame has no depth");

        if (chosen >= 0 && g_slots[chosen].resource != nullptr && pick.valid && sameDirection)
        {
            g_best.valid = true;
            g_best.resource = g_slots[chosen].resource;
            g_best.viewFormat = g_backup.view;
            g_best.width = g_backup.width;
            g_best.height = g_backup.height;
            g_best.reversed = g_slots[chosen].reversed;
            g_best.frame = presents;

            for (int i = 0; i < g_slotsUsed && i < GenericDepthDx12::Snapshot::kMaxCopies; ++i)
            {
                if (g_slots[i].resource != nullptr)
                {
                    g_bestSlots[g_best.copyCount] = i;
                    g_best.copies[g_best.copyCount++] = g_slots[i].resource;
                }
            }
        }

        g_slotsUsed = 0;
        g_core.SetSnapshotsWanted(g_overlayOn.load() || Config::Instance()->DlssNrNativeMotion.value_or_default());
    }

    // Part two in the core: counts the present, stands down or wakes, and picks once the warm-up is over.
    const bool stoodDown =
        g_core.EndPresent(desc.BufferDesc.Width, desc.BufferDesc.Height, Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default());

    {
        std::lock_guard lock(g_mutex);

        // Copies replaced a few frames ago are no longer being read by the menu.
        for (auto it = g_retired.begin(); it != g_retired.end();)
        {
            if (g_core.Presents() > it->second + 4)
            {
                it->first->Release();
                it = g_retired.erase(it);
            }
            else
                ++it;
        }

        // Stood down: the copies were of a frame the finder no longer vouches for.
        if (stoodDown)
            g_best = GenericDepthDx12::Snapshot {};
    }
}

Snapshot BestSnapshot()
{
    std::lock_guard lock(g_mutex);
    return g_best;
}

Snapshot ResolvedSnapshot(ID3D12CommandQueue* queue)
{
    std::lock_guard lock(g_mutex);

    // Single-sample, or already resolved in the game's lists: the slots are what is read
    if (!g_best.valid || g_backup.samples.Count <= 1 || g_backup.resolveInList || g_resolvedFrame == g_best.frame)
        return g_best;

    native::DepthResolveDx12::Job jobs[native::DepthResolveDx12::kMaxJobs];
    int count = 0;

    for (int i = 0; i < g_best.copyCount && count < native::DepthResolveDx12::kMaxJobs; ++i)
    {
        const SnapshotSlot& slot = g_slots[g_bestSlots[i]];

        if (slot.multisampled != nullptr && slot.resource != nullptr)
            jobs[count++] = { slot.multisampled, g_backup.multisampledView, slot.resource };
    }

    const char* failed = count == g_best.copyCount ? Resolver().Run(queue, jobs, count, g_best.reversed, kSlotRest)
                                                   : "a multisampled copy has no slot to resolve into";

    if (failed != nullptr)
    {
        // Not resolved: the copies hold nothing this frame, so there is no depth rather than a wrong one.
        SayOnce("resolve",
                std::format("the multisampled copy could not be resolved ({}); no depth until it can", failed));
        return Snapshot {};
    }

    SayOnce("resolved", std::format("first resolve of a multisampled depth copy ({} samples, nearest, {})",
                                    g_backup.samples.Count, g_best.reversed ? "reversed-Z" : "standard Z"));
    g_resolvedFrame = g_best.frame;
    return g_best;
}

void RecordPreviewCopy(ID3D12GraphicsCommandList* list)
{
    if (list == nullptr || !g_overlayOn.load())
        return;

    std::lock_guard lock(g_mutex);

    // A frame now and then has no copy (a menu, a loading screen); a long run of them is what leaves the preview waiting,
    // so that is said once, with which condition held.
    static uint32_t missed = 0;

    // Only from the chosen copy of the frame just closed, and only when the menu is about to show it. A multisampled
    // buffer's copy holds something only once it has been resolved for this frame.
    const bool unresolved = g_backup.samples.Count > 1 && !g_backup.resolveInList && g_resolvedFrame != g_best.frame;

    if (!g_best.valid || unresolved || g_backup.readback == nullptr || g_best.width != g_backup.width ||
        g_best.height != g_backup.height)
    {
        if (g_core.CurrentPick().valid && ++missed == 600)
            SayOnce(
                "previewMissed",
                std::format("the menu's preview found no copy for 600 frames in a row (frame's copy {}{}, readback {}, "
                            "copy {}x{} vs readback {}x{}, pick {})",
                            g_best.valid ? "there" : "missing", unresolved ? " but not resolved" : "",
                            g_backup.readback != nullptr ? "there" : "missing", g_best.width, g_best.height,
                            g_backup.width, g_backup.height, g_core.CurrentPick().valid ? "valid" : "none"));
        return;
    }

    missed = 0;
    SayOnce("previewCopied", "the menu's preview recorded its first readback");

    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };

    D3D12_RESOURCE_BARRIER in = barrier(g_best.resource, kSlotRest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &in);

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_backup.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = g_backup.footprint;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_best.resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER out = barrier(g_best.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, kSlotRest);
    list->ResourceBarrier(1, &out);

    g_previewFrame = g_core.Presents();
}

GenericDepthSelect::Pick CurrentPick()
{
    return g_core.CurrentPick();
}

void NoteUpscalerCall()
{
    // Noted with no finder installed too: the menu and the frame source must know the game has an upscaler either way.
    native::NoteGameUpscalerCall();

    if (!g_installed)
        return;

    g_core.NoteUpscalerCall();
}

bool GameCallsUpscaler() { return native::GameUpscalerCalledRecently() || (g_installed && g_core.GameCallsUpscaler()); }

bool Armed() { return g_installed && g_core.Armed(); }

// Under g_mutex. The copy's depth as a grid of gray cells, nearer brighter on a log scale: a perspective depth is crowded
// near 0 (reversed-Z) or near 1 (normal), where a straight gray ramp is black. Reads the readback while the GPU may still be
// writing it, which for a picture to look at only shows as a torn frame.
static bool DrawDepthPreview(float boxWidth, float boxHeight)
{
    if (g_backup.readback == nullptr || g_backup.width == 0 || g_backup.height == 0)
        return false;

    // The buffer's own size, not RowPitch * height: the last row is not padded, so at a width whose row is not a multiple of
    // 256 bytes (1707 pixels of R32 in NBA 2K27) that range runs past the end and Map refuses it, every frame.
    D3D12_RANGE range { 0, (SIZE_T) g_backup.readbackBytes };
    uint8_t* data = nullptr;

    if (const HRESULT hr = g_backup.readback->Map(0, &range, (void**) &data); FAILED(hr) || data == nullptr)
    {
        SayOnce("previewMap", std::format("the preview's readback could not be mapped (0x{:X}, {} bytes)", (unsigned) hr,
                                          g_backup.readbackBytes));
        return false;
    }

    constexpr int kCols = 96, kRows = 54;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float cellW = boxWidth / kCols;
    const float cellH = boxHeight / kRows;
    const bool reversed = g_core.CurrentPick().reversed;
    auto* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(origin, ImVec2(origin.x + boxWidth, origin.y + boxHeight), IM_COL32(0, 0, 0, 255));

    // What the copy holds changes from frame to frame (the game's lists run in another order each frame, so the last copy
    // is sometimes the whole scene and sometimes a partial pass). The preview keeps the fullest picture of the last moments:
    // a new one replaces it when it covers nearly as much, and the kept one slowly gives way so a real change still shows.
    static uint8_t cells[kRows * kCols] = {};
    static float keptCoverage = 0.0f;
    uint8_t fresh[kRows * kCols];
    int covered = 0;

    for (int row = 0; row < kRows; ++row)
    {
        const uint32_t y = std::min<uint32_t>(g_backup.height - 1, (uint32_t) ((row + 0.5f) * g_backup.height / kRows));
        const uint8_t* line = data + (size_t) y * g_backup.footprint.Footprint.RowPitch;

        for (int col = 0; col < kCols; ++col)
        {
            const uint32_t x = std::min<uint32_t>(g_backup.width - 1, (uint32_t) ((col + 0.5f) * g_backup.width / kCols));
            float depth = 0.0f;

            switch (g_backup.typeless)
            {
            case DXGI_FORMAT_R16_TYPELESS:
                depth = ((const uint16_t*) line)[x] / 65535.0f;
                break;
            case DXGI_FORMAT_R24G8_TYPELESS:
                depth = (((const uint32_t*) line)[x] & 0xFFFFFFu) / 16777215.0f;
                break;
            default:
                depth = ((const float*) line)[x];
                break;
            }

            // How near it is, from 0 (far) to 1 (on the near plane).
            float near01 = reversed ? depth : 1.0f - depth;
            near01 = std::clamp(near01, 0.0f, 1.0f);
            const float shade = std::log1p(near01 * 5000.0f) / std::log1p(5000.0f);
            const int level = (int) (shade * 255.0f + 0.5f);

            fresh[row * kCols + col] = (uint8_t) level;

            if (near01 > 1e-5f)
                ++covered;
        }
    }

    keptCoverage *= 0.98f;

    if ((float) covered >= 0.85f * keptCoverage)
    {
        memcpy(cells, fresh, sizeof(cells));
        keptCoverage = (float) covered;
    }

    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < kCols; ++col)
        {
            const int level = cells[row * kCols + col];

            draw->AddRectFilled(ImVec2(origin.x + col * cellW, origin.y + row * cellH),
                                ImVec2(origin.x + (col + 1) * cellW, origin.y + (row + 1) * cellH),
                                IM_COL32(level, level, level, 255));
        }

    D3D12_RANGE none { 0, 0 };
    g_backup.readback->Unmap(0, &none);

    ImGui::Dummy(ImVec2(boxWidth, boxHeight));
    return true;
}

void DrawStatus()
{
    auto* config = Config::Instance();
    const bool wanted = config->DlssNrNativeDepthFinder.value_or_default();

    switch (DlssNrNativeMode::FinderFor(wanted, g_installed, g_installFailed))
    {
    case DlssNrNativeMode::Finder::Off:
        ImGui::TextDisabled("Depth: off; NR runs on motion only and does not stand aside for a game upscaler.");
        return;
    case DlssNrNativeMode::Finder::NeedsRestart:
        ImGui::TextDisabled("Depth: the finder needs a restart.");
        return;
    case DlssNrNativeMode::Finder::CouldNotStart:
        ImGui::TextDisabled("Depth: the finder could not start (see the log); NR runs on motion only.");
        return;
    case DlssNrNativeMode::Finder::Installed:
        break;
    }

    const auto pick = CurrentPick();
    const uint32_t warmup = config->DlssNrNativeDepthWarmupFrames.value_or_default();
    const char* untilRestart = wanted ? "" : " (finder off at next start)";

    // Every branch writes exactly one short line and the picture has a box of its own size, so nothing below moves when the
    // pick changes.
    if (g_core.GameCallsUpscaler())
        ImGui::TextDisabled("Depth: stood down, the game is calling an upscaler. Turn it off in the game.");
    else if (!g_core.Armed())
        ImGui::TextDisabled("Depth: watching (%llu of %u frames)...%s",
                            (unsigned long long) (g_core.Presents() - g_core.WarmupStart()), warmup, untilRestart);
    else if (!pick.valid)
        ImGui::TextDisabled("Depth: none found yet; NR runs on motion only%s.", untilRestart);
    else
        ImGui::Text("Depth: picked %ux%u%s%s", pick.width, pick.height, pick.reversed ? ", reversed-Z" : "",
                    untilRestart);

    if (ImGui::IsItemHovered() && pick.valid)
        ImGui::SetTooltip("Format %u. The log has the candidates (Depth finder lines).", pick.format);
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The log has the candidates (Depth finder lines).");
}

void DrawAdvancedUi()
{
    auto* config = Config::Instance();

    // Both switches are read when the game's device is created, so a change here is saved with the rest and applies at the
    // next start.
    bool finder = config->DlssNrNativeDepthFinder.value_or_default();

    if (ImGui::Checkbox("Use the game's depth (better quality; needs a restart)##depthfinder", &finder))
        config->DlssNrNativeDepthFinder = finder;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "%s", "Watches the game's depth buffers (DirectX 12) and picks the scene's, so NR and the\n"
                  "stabiliser get depth as well as motion. Optional: without it they run on motion only. It\n"
                  "only observes, and it stands down for good if the game makes an upscaler call of its own.\n"
                  "It is also what notices that call: without it, nothing here stands aside for the game's own\n"
                  "upscaler. The log lists the candidates. Choosing a mode above turns it on, Off turns it off.\n"
                  "Applies at the next start: save the settings and restart the game.");

    const bool debugView = config->DlssNrNativeDebugView.value_or_default();

    // Shown always on Optical F5Low's page; the copy is made only with the pictures on as well (both read at the
    // start).
    bool overlay = config->DlssNrNativeDepthOverlay.value_or_default();

    if (ImGui::Checkbox("Show the picked depth here##depthfinder", &overlay))
        config->DlssNrNativeDepthOverlay = overlay;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Debug. Copies the picked depth buffer at its busiest clear, recorded into the game's own\n"
                                "command list, and shows it below. Needs \"Show the motion and trust pictures\" on too.\n"
                                "Leave it off unless you are checking the pick. Applies at the next start.");

    if (finder != g_installed)
        ImGui::TextDisabled("Takes effect after saving and restarting the game.");

    if (!debugView || !g_installed)
        return;

    const auto pick = CurrentPick();

    if (!g_overlayOn.load())
    {
        ImGui::TextDisabled("Tick the depth checkbox above and restart to see the picked buffer.");
    }
    else
    {
        const float boxWidth = 360.0f;
        const float boxHeight = boxWidth * 9.0f / 16.0f;
        bool drawn = false;

        if (Armed() && pick.valid)
        {
            std::lock_guard lock(g_mutex);

            if (g_previewFrame != 0 && g_backup.readback != nullptr)
            {
                drawn = DrawDepthPreview(boxWidth, boxHeight);
            }
        }

        if (!drawn)
            ImGui::Dummy(ImVec2(boxWidth, boxHeight));

        ImGui::TextDisabled(drawn ? "Nearer is brighter (log scale)." : "Waiting for the picked buffer...");
    }
}
} // namespace GenericDepthDx12
