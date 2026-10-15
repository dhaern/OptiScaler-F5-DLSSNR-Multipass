#include "pch.h"
// GPU test for OptiScaler/motion/OpticalFlow_Dx12.cpp, no game: a synthetic picture (multi-scale value noise), the same
// picture moved by a known amount, and the flow the pass reports for it. The picture is a continuous function, so a
// shift of half a pixel is exact.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_optical_flow_gpu.cpp OptiScaler\motion\OpticalFlow_Dx12.cpp
//   OptiScaler\motion\SceneCut_Dx12.cpp d3d12.lib dxgi.lib
//   d3dcompiler.lib
//
// Modes: no argument runs every check; "score" prints one summary line for the settings given as key=value; "perf"
// times a frame at 2560x1440 from the CPU; "passes" times each pass of a frame on the GPU (timestamps) at 2560x1440.
// "jitter" runs just the fine-ground still sequence and a textured slow pan, with and without scene jitter.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../OptiScaler/motion/OpticalFlow_Dx12.h"

using Microsoft::WRL::ComPtr;

namespace
{

uint32_t kWidth = 1280;
uint32_t kHeight = 720;

float Hash(int x, int y, int seed)
{
    uint32_t h = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u + (uint32_t) seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / 16777215.0f;
}

float ValueNoise(float x, float y, float scale, int seed)
{
    const float fx = x / scale, fy = y / scale;
    const int ix = (int) std::floor(fx), iy = (int) std::floor(fy);
    const float tx = fx - ix, ty = fy - iy;
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const float a = Hash(ix, iy, seed), b = Hash(ix + 1, iy, seed), c = Hash(ix, iy + 1, seed),
                d = Hash(ix + 1, iy + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

// The scene: continuous in x and y.
float Scene(float x, float y, int channel)
{
    return 0.45f * ValueNoise(x, y, 64, 11 + channel) + 0.25f * ValueNoise(x, y, 24, 23 + channel) +
           0.18f * ValueNoise(x, y, 9, 37 + channel) + 0.12f * ValueNoise(x, y, 4, 51 + channel);
}

// Far ground: a low-contrast broad pattern under fine, non-repeating detail. Frequencies are in output pixels:
// at ratio 3 the finest components approach render Nyquist, but none crosses it. Sampling the continuous scene,
// rather than shifting a saved bitmap, includes the detail changes caused by the game's moving sample positions.
float groundRatio = 1.0f;
float GroundScene(float x, float y)
{
    float value = 0.35f + 0.25f * Scene(x, y, 0);
    const float envelope = 0.5f + ValueNoise(x, y, 48.0f, 83);
    for (int k = 0; k < 12; ++k)
    {
        const float angle = 6.2831853f * Hash(k, 0, 89);
        const float frequency = groundRatio * (0.06f + 0.105f * Hash(k, 1, 89));
        value += 0.035f * envelope * std::sin(6.2831853f * frequency * (x * std::cos(angle) + y * std::sin(angle)) +
                                             6.2831853f * Hash(k, 2, 89));
    }
    return value;
}

// A mostly flat picture with a few textured blobs (a wall with a few things on it): across the flat part a block match
// has nothing to hold on to, and only what moves everything alike can say where it went. The blobs are placed by hash,
// away from the border so a pan of a few tens of pixels keeps them in the picture.
float SparseScene(float x, float y, int channel)
{
    float v = 0.42f;
    for (int i = 0; i < 12; ++i)
    {
        const float cx = 160.0f + Hash(i, 1, 71) * (1280.0f - 320.0f), cy = 120.0f + Hash(i, 2, 71) * (720.0f - 240.0f);
        const float radius = 26.0f + Hash(i, 3, 71) * 22.0f;
        const float d = std::hypot(x - cx, y - cy) / radius;
        if (d < 1.0f)
        {
            const float window = (1.0f - d * d) * (1.0f - d * d);
            v += (Scene(x - cx + 3000.0f, y - cy + 3000.0f, channel) - 0.5f) * 1.2f * window;
        }
    }
    return v;
}

// A flat wall with a few thin dark lines, two pixels wide (cables, window frames): the other hard case for a flow,
// since the pyramid loses the lines at its coarse levels and the wall between them has nothing to hold on to.
float LinesScene(float x, float y, int)
{
    float v = 0.42f;
    for (int i = 0; i < 6; ++i)
    {
        const float dy = y - (150.0f + 97.0f * i), dx = x - (140.0f + 173.0f * i);
        v -= 0.3f * std::exp(-dy * dy / 2.0f) + 0.3f * std::exp(-dx * dx / 2.0f);
    }
    return v;
}

// A picture with a layout, like a game's: a bright sky with a smooth gradient above a darker textured ground. Flipped,
// the ground is above and the sky below, so every part of the picture changes its brightness at once (a cut to another
// place).
float LayoutScene(float x, float y, int channel, bool flipped)
{
    const float split = (flipped ? 0.6f : 0.4f) * (float) kHeight;
    const bool sky = flipped ? y >= split : y < split;

    if (sky)
    {
        const float t = flipped ? (y - split) / ((float) kHeight - split) : y / split;
        return 0.8f - 0.2f * t + 0.04f * (Scene(x, y, channel) - 0.5f);
    }

    return 0.1f + 0.45f * Scene(x, y, channel);
}

// A HUD panel drawn over the picture and never moving: a flat fill inside a two-pixel border. Inside it a block match has
// nothing to hold on to, like a flat wall, but it stays still whatever the picture behind it does.
constexpr float kHudX = 500.0f, kHudY = 300.0f, kHudW = 300.0f, kHudH = 80.0f;

bool InHud(float x, float y) { return x >= kHudX && x < kHudX + kHudW && y >= kHudY && y < kHudY + kHudH; }
bool InHudFill(float x, float y) { return InHud(x - 2.0f, y - 2.0f) && InHud(x + 2.0f, y + 2.0f); }

// A HUD-heavy picture: a flat panel down the left, a strip along the bottom and a box at the top right, about 40% of
// the picture.
float HeavyHud(float x, float y)
{
    if (x < 0.22f * (float) kWidth)
        return 0.2f;
    if (y > 0.85f * (float) kHeight)
        return 0.12f;
    if (x > 0.78f * (float) kWidth && y < 0.12f * (float) kHeight)
        return 0.3f;
    return -1.0f;
}

// A float as a half, rounded to nearest (values in the range a picture holds; very small ones flush to zero).
uint16_t ToHalf(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000;
    const int exponent = (int) ((bits >> 23) & 0xFF) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFF;

    if (exponent <= 0)
        return (uint16_t) sign;
    if (exponent >= 31)
        return (uint16_t) (sign | 0x7BFF);

    uint32_t half = ((uint32_t) exponent << 10) | (mantissa >> 13);
    half += (mantissa >> 12) & 1; // round to nearest
    return (uint16_t) (sign | half);
}

// The PQ (SMPTE ST 2084) encoding of a luminance in nits.
float PqEncode(float nits)
{
    const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), 0.1593017578125f);
    return std::pow((0.8359375f + 18.8515625f * y) / (1.0f + 18.6875f * y), 78.84375f);
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;

    bool Init()
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            {
                wprintf(L"adapter: %s\n", desc.Description);
                break;
            }
        }

        if (!device)
            return false;

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;

        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, event);
        WaitForSingleObject(event, INFINITE);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }

    // An RGBA8 texture holding the scene moved by (dx, dy), left in the non-pixel shader resource state.
    // gain darkens it, noise (in 1/255 steps, peak) adds a different grain to every picture (noiseSeed).
    // With squareSize > 0 a square of another texture sits on top with its top-left corner at (squareX, squareY), its
    // texture moving with it. With scene 1 the scene is SparseScene instead, with 2 LinesScene, with 3 and 4
    // LayoutScene (not flipped, flipped). hud 1 draws the panel, 2 the heavy HUD. The square's texture is scaled by
    // squareGain and raised by squareOffset (in 0..1 picture units), so it can also differ from the background in
    // brightness and contrast, as an object in a game usually does.
    ComPtr<ID3D12Resource> Picture(float dx, float dy, float gain = 1.0f, float noise = 0.0f, int noiseSeed = 0,
                                   float squareX = 0.0f, float squareY = 0.0f, float squareSize = 0.0f, int scene = 0,
                                   int hud = 0, float squareGain = 1.0f, float squareOffset = 0.0f)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> tex;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&tex));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        uint8_t* data = nullptr;
        upload->Map(0, nullptr, (void**) &data);

        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                uint8_t* px = data + (size_t) y * fp.Footprint.RowPitch + x * 4;
                const float ground = scene == 5 ? GroundScene(x + 0.5f - dx, y + 0.5f - dy) : 0.0f;
                for (int c = 0; c < 3; ++c)
                {
                    const bool inSquare = squareSize > 0.0f && x >= squareX && x < squareX + squareSize && y >= squareY &&
                                          y < squareY + squareSize;
                    const float base = inSquare     ? Scene(x - squareX + 3000.0f, y - squareY + 3000.0f, c)
                                       : scene == 1 ? SparseScene(x - dx, y - dy, c)
                                       : scene == 2 ? LinesScene(x - dx, y - dy, c)
                                       : scene == 3 ? LayoutScene(x - dx, y - dy, c, false)
                                       : scene == 4 ? LayoutScene(x - dx, y - dy, c, true)
                                       : scene == 5 ? ground
                                                    : Scene(x - dx, y - dy, c);
                    float v = (inSquare ? squareOffset + squareGain * base : base) * 255.0f * gain;
                    if (hud == 1 && InHud(x, y))
                        v = (InHudFill(x, y) ? 0.55f : 0.15f) * 255.0f;
                    if (hud == 2 && HeavyHud(x, y) >= 0.0f)
                        v = HeavyHud(x, y) * 255.0f;
                    if (noise > 0.0f)
                        v += (Hash((int) x * 3 + c, (int) y, 1000 + noiseSeed) * 2.0f - 1.0f) * noise;
                    px[c] = (uint8_t) std::clamp(v + 0.5f, 0.0f, 255.0f);
                }
                px[3] = 255;
            }

        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        Submit();
        return tex;
    }

    // An R16G16B16A16_FLOAT picture of a dark HDR scene moved by (dx, dy), left in the non-pixel shader resource state:
    // the scene's value s (0..1) becomes the linear light 0.01 * 20^s (0.01 .. 0.2, in scRGB units, 1.0 = 80 nits),
    // with grain (shot-noise like: its size goes with the square root of the light, grain is the factor, a new one
    // every picture). With pq the same light is stored as PQ.
    ComPtr<ID3D12Resource> HdrPicture(float dx, float dy, float grain, int noiseSeed, bool pq)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> tex;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&tex));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        uint8_t* data = nullptr;
        upload->Map(0, nullptr, (void**) &data);

        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                uint16_t* px = (uint16_t*) (data + (size_t) y * fp.Footprint.RowPitch + x * 8);
                for (int c = 0; c < 3; ++c)
                {
                    float v = 0.01f * std::pow(20.0f, Scene(x - dx, y - dy, c));
                    if (grain > 0.0f)
                        v = std::max(v + (Hash((int) x * 3 + c, (int) y, 2000 + noiseSeed) * 2.0f - 1.0f) * grain *
                                             std::sqrt(v),
                                     0.0f);
                    px[c] = ToHalf(pq ? PqEncode(v * 80.0f) : v);
                }
                px[3] = ToHalf(1.0f);
            }

        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        Submit();
        return tex;
    }

    // An R32_FLOAT reversed-Z depth map for Picture's square: the square at distance 5, the rest at 20. scale < 1 makes a
    // smaller map (a depth buffer at a lower render resolution) covering the same picture.
    ComPtr<ID3D12Resource> DepthMap(float squareX, float squareY, float squareSize, float scale = 1.0f)
    {
        const uint32_t width = (uint32_t) (kWidth * scale), height = (uint32_t) (kHeight * scale);
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> tex;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&tex));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        uint8_t* data = nullptr;
        upload->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const float px = (x + 0.5f) / scale, py = (y + 0.5f) / scale; // in picture pixels
                const bool in = px >= squareX && px < squareX + squareSize && py >= squareY && py < squareY + squareSize;
                const float d = 0.1f / (in ? 5.0f : 20.0f);
                memcpy(data + (size_t) y * fp.Footprint.RowPitch + x * 4, &d, 4);
            }
        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        Submit();
        return tex;
    }
};

uint16_t* g_unused = nullptr;

float Half(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
    float v;
    if (exp == 0)
        v = std::ldexp((float) mant, -24);
    else if (exp == 31)
        v = INFINITY;
    else
        v = std::ldexp((float) (mant + 1024), (int) exp - 25);
    return sign ? -v : v;
}

// The flow (RGBA16F, in its resting read state) as x, y pairs, row by row.
std::vector<float> ReadFlow(Gpu& gpu, ID3D12Resource* out)
{
    const auto desc = out->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto readback = gpu.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    gpu.list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = out;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    gpu.list->ResourceBarrier(1, &b);
    gpu.Submit();

    std::vector<float> flow((size_t) desc.Width * desc.Height * 2);
    uint8_t* data = nullptr;
    readback->Map(0, nullptr, (void**) &data);
    for (uint32_t y = 0; y < desc.Height; ++y)
        for (uint32_t x = 0; x < desc.Width; ++x)
        {
            const uint16_t* px = (const uint16_t*) (data + (size_t) y * fp.Footprint.RowPitch + x * 8);
            flow[((size_t) y * desc.Width + x) * 2] = Half(px[0]);
            flow[((size_t) y * desc.Width + x) * 2 + 1] = Half(px[1]);
        }
    readback->Unmap(0, nullptr);
    return flow;
}

// The scene-cut flag the detector left (a 2x1 R32_UINT texture, in its resting read state): the flag and the
// divergence.
struct CutRead
{
    bool flag = false;
    float value = 0.0f;
};

CutRead ReadCut(Gpu& gpu, ID3D12Resource* cut)
{
    const auto desc = cut->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto readback = gpu.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = cut;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    gpu.list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = cut;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    gpu.list->ResourceBarrier(1, &b);
    gpu.Submit();

    uint32_t* data = nullptr;
    readback->Map(0, nullptr, (void**) &data);
    CutRead out;
    out.flag = data[0] != 0;
    memcpy(&out.value, data + 1, 4);
    readback->Unmap(0, nullptr);
    return out;
}

// Two full Halton cycles include the phase wrap. A slow pan of the existing textured scene also checks
// that jitter cancellation does not suppress real motion. All statistics are in render pixels.
bool CheckJitter(Gpu& gpu, OpticalFlowDx12& flow)
{
    const uint32_t savedWidth = kWidth, savedHeight = kHeight;
    kWidth = 512;
    kHeight = 320;
    auto halton = [](int index, int base)
    {
        float value = 0.0f, weight = 1.0f;
        for (; index; index /= base)
        {
            weight /= (float) base;
            value += weight * (index % base);
        }
        return value - 0.5f;
    };
    bool ok = true;
    for (const float ratio : { 1.36f, 3.0f })
        for (const bool moving : { false, true })
            for (const bool jittered : { false, true })
            {
                groundRatio = ratio;
                flow.Reset();
                const int phases = (int) (8.0f * ratio * ratio), frames = 2 * phases + 1;
                const float vx = moving ? 0.75f : 0.0f, vy = moving ? -0.5f : 0.0f;
                std::vector<float> errors;
                uint64_t over = 0;
                for (int frame = 0; frame < frames; ++frame)
                {
                    const int phase = frame % phases + 1;
                    const float jx = jittered ? halton(phase, 2) : 0.0f;
                    const float jy = jittered ? halton(phase, 3) : 0.0f;
                    auto picture = gpu.Picture(frame * vx + jx, frame * vy + jy, 1.0f, 0.0f, 0, 0, 0, 0,
                                               moving ? 0 : 5);
                    if (!flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, nullptr,
                                       DXGI_FORMAT_UNKNOWN, true, OpticalFlowDx12::Encoding::Srgb, 1, 2, jx, jy))
                        return false;
                    gpu.Submit();
                    if (frame == 0)
                        continue;
                    if (!flow.FlowValid())
                        return false;
                    const auto field = ReadFlow(gpu, flow.Flow());
                    for (uint32_t y = 12; y + 12 < flow.FlowHeight(); ++y)
                        for (uint32_t x = 12; x + 12 < flow.FlowWidth(); ++x)
                        {
                            const size_t at = ((size_t) y * flow.FlowWidth() + x) * 2;
                            const float error = std::hypot(field[at] + vx, field[at + 1] + vy);
                            if (!std::isfinite(error))
                                return false;
                            errors.push_back(error);
                            over += error > 0.25f;
                        }
                }
                std::sort(errors.begin(), errors.end());
                const float p50 = errors[errors.size() / 2], p95 = errors[errors.size() * 95 / 100];
                const double share = (double) over / errors.size();
                const bool pass = moving ? p95 <= 0.5f : errors.back() <= 0.25f;
                printf("scene jitter ratio %.2f, %s, jitter %s, %d frames: error p50 %.4f p95 %.4f max %.4f render px, "
                       ">0.25 px %.3f%%: %s\n", ratio,
                       moving ? "textured pan (0.75,-0.5)" : "fine ground still", jittered ? "on" : "off", frames,
                       p50, p95, errors.back(), 100.0 * share, pass ? "PASS" : "FAIL");
                ok = ok && pass;
            }
    kWidth = savedWidth;
    kHeight = savedHeight;
    flow.Reset();
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    const bool perf = argc > 1 && std::string(argv[1]) == "perf";
    const bool passes = argc > 1 && std::string(argv[1]) == "passes";
    bool score = false; // "score": one summary line for the settings given as key=value (radius=2 cells=4 ...)
    const bool noSmoothing = (argc > 1 && std::string(argv[1]) == "nosmooth") || (argc > 2 && std::string(argv[2]) == "nosmooth");

    if (perf || passes)
    {
        kWidth = 2560;
        kHeight = 1440;
    }

    Gpu gpu;

    if (!gpu.Init())
    {
        printf("no D3D12 device\n");
        return 2;
    }

    OpticalFlowDx12 flow;

    if (!flow.Init(gpu.device.Get()))
    {
        printf("init failed: %s\n", flow.Error().c_str());
        return 1;
    }

    // Settings to compare, from the command line (they apply to every case below).
    OpticalFlowDx12::Settings tuning;
    if (noSmoothing)
        tuning.smoothRadius = 0;
    for (int a = 1; a < argc; ++a)
    {
        const std::string arg = argv[a];
        const size_t eq = arg.find('=');
        const std::string key = arg.substr(0, eq);
        const float value = eq == std::string::npos ? 0.0f : std::stof(arg.substr(eq + 1));

        if (arg == "nodmatch")
            tuning.depthMatching = false;
        else if (arg == "noglobal")
            tuning.globalCandidate = false;
        else if (arg == "score")
            score = true;
        else if (key == "radius")
            tuning.radius = (int) value;
        else if (key == "coarse")
            tuning.coarseRadius = (int) value;
        else if (key == "lambda")
            tuning.lambda = value;
        else if (key == "history")
            tuning.useHistory = value != 0.0f;
        else if (key == "cells")
            tuning.coarseCells = (int) value;
        else if (key == "smooth")
            tuning.smoothRadius = (int) value;
        else if (key == "knee")
            tuning.confidenceKnee = value;
        else if (key == "dmatch")
            tuning.depthMatching = value != 0.0f;
        else if (key == "global")
            tuning.globalCandidate = value != 0.0f;
        else if (key == "inverse")
            tuning.inverseRefinement = value != 0.0f;
        else if (key == "scene")
            tuning.sceneCutDetector = value != 0.0f;
        else if (key == "scenethr")
            tuning.sceneCutThreshold = value;
        else if (key == "zero")
            tuning.zeroMargin = value;
        else if (key == "zeroreach")
            tuning.zeroReach = (int) value;
        else if (key == "still")
            tuning.stillFrames = (int) value;
        else if (key == "stilleps")
            tuning.stillEpsilon = value;
        else if (key == "stillmargin")
            tuning.stillMargin = value;
        else if (key == "look")
            tuning.lookWeights = value != 0.0f;
        else if (key == "lookrange")
            tuning.lookRange = value;
        else if (key == "lookdist")
            tuning.lookDistance = value;
        else if (key == "lumaperc")
            tuning.perceptualLuma = value != 0.0f;
        else if (key == "white")
            tuning.hdrWhiteNits = value;
    }
    flow.Tuning() = tuning;

    if (argc > 1 && std::string(argv[1]) == "jitter")
        return CheckJitter(gpu, flow) ? 0 : 1;

    if (argc > 1 && std::string(argv[1]) == "inlist")
    {
        D3D12_QUERY_HEAP_DESC query { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 64, 0 };
        ComPtr<ID3D12QueryHeap> heap;
        if (FAILED(gpu.device->CreateQueryHeap(&query, IID_PPV_ARGS(&heap))))
            return 1;
        auto before = gpu.Picture(0, 0), after = gpu.Picture(12, -8);
        flow.SetTimestampHeap(heap.Get(), 64);
        flow.Dispatch(gpu.list.Get(), before.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, DXGI_FORMAT_UNKNOWN, true,
                      OpticalFlowDx12::Encoding::Srgb, 1, 2);
        gpu.Submit();
        flow.ClearTimestamps();
        bool ok = flow.Dispatch(gpu.list.Get(), after.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, DXGI_FORMAT_UNKNOWN,
                                true, OpticalFlowDx12::Encoding::Srgb, 1, 2) &&
                  flow.FlowValid();
        UINT w = (kWidth + 3) / 4, h = (kHeight + 3) / 4, match = 0, down = 0;
        ok = ok && flow.FlowWidth() == w && flow.FlowHeight() == h;
        for (int level = 0; level < 5; ++level, w = (w + 1) / 2, h = (h + 1) / 2)
        {
            const auto desc = flow.LumaOfLastFrame(level)->GetDesc();
            ok = ok && desc.Width == w && desc.Height == h;
        }
        for (UINT i = 0; i < flow.TimestampCount(); ++i)
        {
            match += std::string(flow.TimestampName(i)) == "match";
            down += std::string(flow.TimestampName(i)) == "down";
        }
        ok = ok && match == 5 && down == 5 && flow.TimestampCount() == 16;
        const auto field = ReadFlow(gpu, flow.Flow());
        UINT samples = 0, within = 0;
        for (UINT y = 24; y + 24 < flow.FlowHeight(); ++y)
            for (UINT x = 24; x + 24 < flow.FlowWidth(); ++x, ++samples)
                within += std::hypot(field[(y * flow.FlowWidth() + x) * 2] + 12,
                                     field[(y * flow.FlowWidth() + x) * 2 + 1] - 8) <= 1.0f;
        ok = ok && samples > 0 && double(within) / samples >= 0.95;
        printf("in-list: flow %ux%u, 5 pyramid levels checked, %u match/%u down passes, shift (12,-8) within 1 render "
               "px %.1f%%: %s\n",
               flow.FlowWidth(), flow.FlowHeight(), match, down, 100.0 * within / samples, ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    // Score mode: the measures under exactly these settings, gathered per kind of scene.
    struct Tally
    {
        double sum = 0, worst = 1;
        int n = 0;
        void Add(double v)
        {
            sum += v;
            worst = std::min(worst, v);
            ++n;
        }
        double Mean() const { return n ? sum / n : 0; }
    };
    Tally panHalf, panError, grainOne, brightHalf, sparseOne, thinOne, thinAliasError, edgeDepthOne, edgeNoDepthOne,
        smallDepthOne, smallNoDepthOne, hudStill, wallStill, hudStillLong, thinSlowBand, thinSlowRest, thinPanBand,
        thinPanRest, edgeLookNoDepthOne;

    if (perf)
    {
        // Frames of a pan, in batches that fit the descriptor ring, timed from the CPU around a wait on the GPU.
        std::vector<ComPtr<ID3D12Resource>> frames;
        for (int i = 0; i < 6; ++i)
            frames.push_back(gpu.Picture(i * 7.0f, i * -3.0f));

        // With a depth-aware setting on, a depth map of a square to read.
        const bool withDepth = tuning.depthMatching;
        auto depthMap = withDepth ? gpu.DepthMap(800.0f, 400.0f, 500.0f) : ComPtr<ID3D12Resource>();
        auto dispatch = [&](ID3D12Resource* frame)
        {
            flow.Dispatch(gpu.list.Get(), frame, DXGI_FORMAT_R8G8B8A8_UNORM, depthMap.Get(),
                          withDepth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN, true);
        };

        flow.Reset();
        for (int warm = 0; warm < 3; ++warm)
        {
            for (auto& frame : frames)
                dispatch(frame.Get());
            gpu.Submit();
        }

        LARGE_INTEGER freq, a, b;
        QueryPerformanceFrequency(&freq);
        const int batches = 20;
        QueryPerformanceCounter(&a);
        for (int batch = 0; batch < batches; ++batch)
        {
            for (auto& frame : frames)
                dispatch(frame.Get());
            gpu.Submit();
        }
        QueryPerformanceCounter(&b);
        printf("%ux%u: %.3f ms per flow (wall, %d frames)\n", kWidth, kHeight,
               1000.0 * (double) (b.QuadPart - a.QuadPart) / freq.QuadPart / (batches * 6), batches * 6);
        return 0;
    }

    if (passes)
    {
        // The GPU time of each pass, averaged over many frames of a pan with depth: one frame per submit, a timestamp
        // after every pass, the match summed over its levels.
        std::vector<ComPtr<ID3D12Resource>> frames;
        for (int i = 0; i < 4; ++i)
            frames.push_back(gpu.Picture(i * 7.0f, i * -3.0f));

        auto depthMap = gpu.DepthMap(800.0f, 400.0f, 500.0f);

        D3D12_QUERY_HEAP_DESC heapDesc {};
        heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heapDesc.Count = 64;
        ComPtr<ID3D12QueryHeap> heap;
        gpu.device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap));
        auto readback = gpu.Buffer(64 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        UINT64 frequency = 0;
        gpu.queue->GetTimestampFrequency(&frequency);

        flow.SetTimestampHeap(heap.Get(), 64);
        flow.Reset();

        std::vector<std::string> names;
        std::vector<double> sums;
        double total = 0;
        const int warm = 8, measured = 120;

        for (int n = 0; n < warm + measured; ++n)
        {
            flow.ClearTimestamps();
            flow.Dispatch(gpu.list.Get(), frames[n % 4].Get(), DXGI_FORMAT_R8G8B8A8_UNORM, depthMap.Get(),
                          flow.Tuning().depthMatching ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN, true);
            const uint32_t count = flow.TimestampCount();
            gpu.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, count, readback.Get(), 0);
            gpu.Submit();

            if (n < warm || count < 2)
                continue;

            UINT64* ticks = nullptr;
            readback->Map(0, nullptr, (void**) &ticks);
            for (uint32_t i = 1; i < count; ++i)
            {
                const double ms = 1000.0 * (double) (ticks[i] - ticks[i - 1]) / (double) frequency;
                const std::string name = flow.TimestampName(i);
                size_t slot = 0;
                while (slot < names.size() && names[slot] != name)
                    ++slot;
                if (slot == names.size())
                {
                    names.push_back(name);
                    sums.push_back(0);
                }
                sums[slot] += ms;
                total += ms;
            }
            readback->Unmap(0, nullptr);
        }

        for (size_t i = 0; i < names.size(); ++i)
            printf("pass %-8s %.4f ms per frame\n", names[i].c_str(), sums[i] / measured);
        printf("%ux%u: %.4f ms per frame in all (GPU timestamps, %d frames)\n", kWidth, kHeight, total / measured,
               measured);
        return 0;
    }

    struct Case
    {
        float dx, dy;
        double minWithinHalf; // the share of flow samples that must be within half a pixel (0: only reported)
        float gain = 1.0f;    // picture brightness
        float noise = 0.0f;   // grain peak, in 1/255 steps, different on every picture
        int frames = 2;       // pictures in the pan (constant velocity); the last pair is measured
        const char* what = "";
        float fade = 1.0f;    // brightness of each picture relative to the one before (eye adaptation, a fade)
    };

    const Case cases[] = {
        { 0, 0, 0.95 },
        { 3, -2, 0.90 },
        { 2.5f, 1.5f, 0.85 },
        { 11, 7, 0.90 },
        { 21, -14, 0.90 },
        { -37, 29, 0.85 },
        { 60, 0, 0.80 },
        // fast pans, which the pyramid must reach, and the same pan a few pictures in a row (last frame's flow is known)
        { 100, -60, 0.95, 1.0f, 0.0f, 2, "fast pan" },
        { -160, 40, 0.95, 1.0f, 0.0f, 2, "fast pan" },
        { 220, 0, 0.95, 1.0f, 0.0f, 2, "fast pan" },
        // the same pan a few pictures in a row: last frame's flow is known
        { 30, -18, 0.95, 1.0f, 0.0f, 4, "steady pan, 4 pictures" },
        { 90, 20, 0.95, 1.0f, 0.0f, 4, "steady fast pan, 4 pictures" },
        // dark and grainy: flat areas where block matching has little to hold on to
        { 3, -2, 0, 0.08f, 3.0f, 2, "dark and grainy" },
        { 12, 5, 0, 0.08f, 3.0f, 4, "dark and grainy, 4 pictures" },
        // the brightness changes between the pictures
        { 3, -2, 0.95, 1.0f, 0.0f, 2, "darker by 15%", 0.85f },
        { 21, -14, 0.95, 1.0f, 0.0f, 2, "darker by 15%", 0.85f },
        { 11, 7, 0.95, 0.9f, 0.0f, 2, "brighter by 10%", 1.1f },
        { 30, -18, 0.95, 1.0f, 0.0f, 4, "steady pan darkening 10% a picture", 0.9f },
    };
    bool ok = true;

    for (const Case& c : cases)
    {
        flow.Reset();

        // pictures 0..frames-1, picture k shifted by k times the velocity; every one but the last is submitted
        std::vector<ComPtr<ID3D12Resource>> pictures;
        for (int k = 0; k < c.frames; ++k)
            pictures.push_back(gpu.Picture(c.dx * k, c.dy * k, c.gain * std::pow(c.fade, (float) k), c.noise, k));

        for (int k = 0; k + 1 < c.frames; ++k)
        {
            flow.Dispatch(gpu.list.Get(), pictures[k].Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            gpu.Submit();
        }

        flow.Dispatch(gpu.list.Get(), pictures[c.frames - 1].Get(), DXGI_FORMAT_R8G8B8A8_UNORM);

        if (!flow.FlowValid())
        {
            printf("flow not valid after two frames\n");
            return 1;
        }

        const auto desc = flow.Flow()->GetDesc();
        const std::vector<float> field = ReadFlow(gpu, flow.Flow());

        // Content that moved by +d was at p - d before: the flow is -d. Away from the border, where the shift brings in
        // content that was not in the previous picture.
        const int margin = (int) std::ceil(std::max(std::fabs(c.dx), std::fabs(c.dy)) / 2.0f) + 24;
        uint64_t n = 0, half = 0, one = 0, wild = 0;
        double sumErr = 0;

        for (uint32_t y = margin; y + margin < desc.Height; ++y)
            for (uint32_t x = margin; x + margin < desc.Width; ++x)
            {
                const size_t i = ((size_t) y * desc.Width + x) * 2;
                const float ex = field[i] - (-c.dx), ey = field[i + 1] - (-c.dy);
                const float err = std::sqrt(ex * ex + ey * ey);
                sumErr += err;
                ++n;
                half += err <= 0.5f;
                one += err <= 1.0f;
                wild += err > 3.0f;
            }

        const double shareHalf = (double) half / n;
        if (c.noise > 0.0f)
            grainOne.Add((double) one / n);
        else if (c.fade != 1.0f)
            brightHalf.Add(shareHalf);
        else
        {
            panHalf.Add(shareHalf);
            panError.Add(sumErr / n);
        }
        const bool pass = shareHalf >= c.minWithinHalf;
        ok = ok && (pass || c.minWithinHalf <= 0);
        printf("shift (%6.1f, %6.1f): mean error %7.3f px, within 0.5 px %5.1f%%, within 1 px %5.1f%%, off by over 3 px %5.1f%%   %s  %s\n",
               c.dx, c.dy, sumErr / n, 100.0 * shareHalf, 100.0 * one / n, 100.0 * wild / n,
               c.minWithinHalf > 0 ? (pass ? "ok" : "FAIL") : "(reported)", c.what);
    }

    // A pan over a mostly flat picture with a few textured blobs: the share of the whole picture (flat part included)
    // that gets the pan's flow, without and with the whole-picture candidate, and with it given depth (one flat
    // surface). A flat part is a tie between every offset, so only what the picture as a whole did can say where it
    // went. The candidate must lift the share by at least kSparseGain, and to at least kSparseNeed.
    struct SparseCase
    {
        float dx, dy;
        int frames;
    };

    const SparseCase sparseCases[] = { { 6, -4, 3 }, { 30, -18, 3 }, { 12, 5, 4 }, { 90, 20, 4 } };
    const double kSparseGain = 0.5, kSparseNeed = 0.9;

    for (const SparseCase& c : sparseCases)
    {
        double share[3] = {};
        const char* names[3] = { "without", "with", "with, and depth" };

        for (int run = 0; run < 3; ++run)
        {
            flow.Reset();
            flow.Tuning() = tuning;
            flow.Tuning().globalCandidate = run != 0;

            for (int k = 0; k < c.frames; ++k)
            {
                auto picture = gpu.Picture(c.dx * k, c.dy * k, 1.0f, 0.0f, k, 0.0f, 0.0f, 0.0f, 1);
                auto depth = run == 2 ? gpu.DepthMap(0.0f, 0.0f, 0.0f) : ComPtr<ID3D12Resource>();
                flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, depth.Get(),
                              run == 2 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN, true);
                gpu.Submit();
            }

            const auto desc = flow.Flow()->GetDesc();
            const std::vector<float> field = ReadFlow(gpu, flow.Flow());
            const int margin = (int) std::ceil(std::max(std::fabs(c.dx), std::fabs(c.dy)) / 2.0f) + 24;
            uint64_t n = 0, one = 0;
            double sum = 0;

            for (uint32_t y = margin; y + margin < desc.Height; ++y)
                for (uint32_t x = margin; x + margin < desc.Width; ++x)
                {
                    const size_t i = ((size_t) y * desc.Width + x) * 2;
                    const float err = std::hypot(field[i] + c.dx, field[i + 1] + c.dy);
                    sum += err;
                    one += err <= 1.0f;
                    ++n;
                }

            share[run] = (double) one / n;
            if (run == (tuning.globalCandidate ? 2 : 0))
                sparseOne.Add(share[run]);
            printf("sparse blobs on a flat picture, shift (%5.1f, %5.1f), %d pictures, %-15s candidate: within 1 px "
                   "%5.1f%%, mean error %6.3f px\n",
                   c.dx, c.dy, c.frames, names[run], 100.0 * share[run], sum / n);
        }

        flow.Tuning() = tuning;
        const bool pass = share[1] >= share[0] + kSparseGain && share[1] >= kSparseNeed && share[2] >= kSparseNeed;
        ok = ok && pass;
        printf("  %s\n", pass ? "ok" : "FAIL");
    }

    // Where the whole-picture candidate must not win: the flat inside of a still HUD panel while the picture behind it pans,
    // and a still flat wall while one textured thing moves across it. Without and with the candidate; the share of those
    // pixels whose flow is within 1 px of no motion.
    struct StillCase
    {
        float dx, dy;  // the background's pan (HUD) or the moving square's speed (wall)
        bool hud;      // true: textured picture panning under a still HUD; false: still flat wall, a square moving
        const char* what;
    };

    const StillCase stillCases[] = {
        { 12, 0, true, "still HUD panel, picture panning" },
        { 30, -10, true, "still HUD panel, picture panning" },
        { 4, 3, true, "still HUD panel, picture panning" },
        { 20, 8, false, "still flat wall, a 160 px square moving" },
        { -30, 12, false, "still flat wall, a 160 px square moving" },
    };

    for (const StillCase& c : stillCases)
    {
        double share[2] = {};

        for (int run = 0; run < 2; ++run)
        {
            flow.Reset();
            flow.Tuning() = tuning;
            flow.Tuning().globalCandidate = run != 0;
            const int frames = 3;
            const float size = 160.0f, x0 = 500.0f, y0 = 250.0f;

            for (int k = 0; k < frames; ++k)
            {
                auto picture = c.hud ? gpu.Picture(c.dx * k, c.dy * k, 1.0f, 0.0f, k, 0.0f, 0.0f, 0.0f, 0, true)
                                     : gpu.Picture(0.0f, 0.0f, 1.0f, 0.0f, k, x0 + c.dx * k, y0 + c.dy * k, size, 1);
                flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
                gpu.Submit();
            }

            const auto desc = flow.Flow()->GetDesc();
            const std::vector<float> field = ReadFlow(gpu, flow.Flow());
            const float sx = x0 + c.dx * (frames - 1), sy = y0 + c.dy * (frames - 1);
            auto nearSquare = [&](float x, float y, float ax, float ay)
            { return x >= ax - 8.0f && x < ax + size + 8.0f && y >= ay - 8.0f && y < ay + size + 8.0f; };
            uint64_t n = 0, still = 0;

            for (uint32_t y = 12; y + 12 < desc.Height; ++y)
                for (uint32_t x = 12; x + 12 < desc.Width; ++x)
                {
                    const float fx = 2.0f * x + 1.0f, fy = 2.0f * y + 1.0f;
                    const bool counted = c.hud ? InHudFill(fx, fy)
                                               : !nearSquare(fx, fy, sx, sy) && !nearSquare(fx, fy, sx - c.dx, sy - c.dy);
                    if (!counted)
                        continue;

                    const size_t i = ((size_t) y * desc.Width + x) * 2;
                    still += std::hypot(field[i], field[i + 1]) <= 1.0f;
                    ++n;
                }

            share[run] = (double) still / n;
        }

        flow.Tuning() = tuning;
        (c.hud ? hudStill : wallStill).Add(share[1]);

        // The still wall must stay still: one thing moving is not the camera. The HUD's flat inside is reported only: it
        // looks exactly like a flat wall that pans with the picture, which is what the candidate is for, so the flow takes
        // the camera's motion there (its border and anything drawn on it still match as still).
        const bool pass = c.hud || share[1] >= 0.95;
        ok = ok && pass;
        printf("%s (%5.1f, %5.1f): still within 1 px, without candidate %5.1f%%, with %5.1f%%   %s\n", c.what, c.dx, c.dy,
               100.0 * share[0], 100.0 * share[1], c.hud ? "(reported)" : (pass ? "ok" : "FAIL"));
    }

    // The same still HUD panel over a panning picture, but for a long time: sixteen pictures, scored on the last. A HUD
    // is unchanged frame after frame for as long as it is shown, which three pictures cannot show. Reported only (the
    // number for a build that tells a still HUD from a panning flat wall by time); the three-picture share above is
    // hudStill.
    for (const StillCase& c : stillCases)
    {
        if (!c.hud)
            continue;

        flow.Reset();
        flow.Tuning() = tuning;
        const int frames = 16;

        for (int k = 0; k < frames; ++k)
        {
            auto picture = gpu.Picture(c.dx * k, c.dy * k, 1.0f, 0.0f, k, 0.0f, 0.0f, 0.0f, 0, true);
            flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            gpu.Submit();
        }

        const auto desc = flow.Flow()->GetDesc();
        const std::vector<float> field = ReadFlow(gpu, flow.Flow());
        uint64_t n = 0, still = 0;

        for (uint32_t y = 12; y + 12 < desc.Height; ++y)
            for (uint32_t x = 12; x + 12 < desc.Width; ++x)
            {
                if (!InHudFill(2.0f * x + 1.0f, 2.0f * y + 1.0f))
                    continue;

                const size_t i = ((size_t) y * desc.Width + x) * 2;
                still += std::hypot(field[i], field[i + 1]) <= 1.0f;
                ++n;
            }

        hudStillLong.Add((double) still / n);
        printf("%s (%5.1f, %5.1f), %d pictures: still within 1 px %5.1f%%   (reported)\n", c.what, c.dx, c.dy, frames,
               100.0 * still / n);
    }

    // The still ages are cleared by a Reset: after sixteen pictures of the HUD case and a Reset, the next pictures
    // start from no age, so the flat inside of the HUD is the camera's again (as in the three-picture case above).
    if (tuning.stillFrames > 0)
    {
        flow.Reset();
        flow.Tuning() = tuning;

        for (int k = 0; k < 16; ++k)
        {
            auto picture = gpu.Picture(12.0f * k, 0.0f, 1.0f, 0.0f, k, 0.0f, 0.0f, 0.0f, 0, true);
            flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            gpu.Submit();
        }

        flow.Reset();

        for (int k = 16; k < 19; ++k)
        {
            auto picture = gpu.Picture(12.0f * k, 0.0f, 1.0f, 0.0f, k, 0.0f, 0.0f, 0.0f, 0, true);
            flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            gpu.Submit();
        }

        const auto desc = flow.Flow()->GetDesc();
        const std::vector<float> field = ReadFlow(gpu, flow.Flow());
        uint64_t n = 0, still = 0;

        for (uint32_t y = 12; y + 12 < desc.Height; ++y)
            for (uint32_t x = 12; x + 12 < desc.Width; ++x)
            {
                if (!InHudFill(2.0f * x + 1.0f, 2.0f * y + 1.0f))
                    continue;

                const size_t i = ((size_t) y * desc.Width + x) * 2;
                still += std::hypot(field[i], field[i + 1]) <= 1.0f;
                ++n;
            }

        const bool pass = (double) still / n <= 0.5;
        ok = ok && pass;
        printf("still HUD panel, a Reset after sixteen pictures, then three: still within 1 px %5.1f%%   %s\n",
               100.0 * still / n, pass ? "ok" : "FAIL");
    }

    // Headroom, reported only: a pan over a flat wall with thin lines, as the flow is now (depth matching and the
    // whole-picture candidate on), with and without the candidate. What is still wrong here is what a camera model
    // could win.
    for (const SparseCase& c : { SparseCase { 6, -4, 4 }, SparseCase { 30, -18, 4 }, SparseCase { 90, 20, 4 } })
        for (int run = 0; run < 2; ++run)
        {
            flow.Reset();
            flow.Tuning() = tuning;
            flow.Tuning().globalCandidate = run != 0;

            for (int k = 0; k < c.frames; ++k)
            {
                auto picture = gpu.Picture(c.dx * k, c.dy * k, 1.0f, 0.0f, k, 0.0f, 0.0f, 0.0f, 2);
                auto depth = gpu.DepthMap(0.0f, 0.0f, 0.0f);
                flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, depth.Get(),
                              DXGI_FORMAT_R32_FLOAT, true);
                gpu.Submit();
            }

            const auto desc = flow.Flow()->GetDesc();
            const std::vector<float> field = ReadFlow(gpu, flow.Flow());
            const int margin = (int) std::ceil(std::max(std::fabs(c.dx), std::fabs(c.dy)) / 2.0f) + 24;
            uint64_t n = 0, one = 0, wild = 0;
            double sum = 0;

            for (uint32_t y = margin; y + margin < desc.Height; ++y)
                for (uint32_t x = margin; x + margin < desc.Width; ++x)
                {
                    const size_t i = ((size_t) y * desc.Width + x) * 2;
                    const float err = std::hypot(field[i] + c.dx, field[i + 1] + c.dy);
                    sum += err;
                    one += err <= 1.0f;
                    wild += err > 3.0f;
                    ++n;
                }

            if (run == (tuning.globalCandidate ? 1 : 0))
            {
                if (c.dx == 90.0f)
                    thinAliasError.Add(sum / n);
                else
                    thinOne.Add((double) one / n);
            }
            printf("thin lines on a flat wall, shift (%5.1f, %5.1f), %d pictures, %-7s candidate: within 1 px %5.1f%%, "
                   "off by over 3 px %5.1f%%, mean error %6.3f px (reported)\n",
                   c.dx, c.dy, c.frames, run ? "with" : "without", 100.0 * one / n, 100.0 * wild / n, sum / n);
        }

    // A slow pan over thin lines on a flat wall, a pixel a picture for sixteen pictures (a camera creeping along a wall
    // with cables on it): the share of flow samples within 1 px of the pan, in a band of 12 pixels around the lines
    // (where the picture says how it moved) and on the flat rest (where only the camera's motion can say), reported;
    // the lines themselves are checked from two pixels a picture. At a pixel a picture the camera's motion is half a
    // pixel of the finest level, which rounds to no motion, so holding the wall still and following the camera look the
    // same there; at two pixels a picture (thinPan) they differ: that is where a flat patch held still by the still age
    // would show.
    struct SlowCase
    {
        SparseCase c;
        Tally& band;
        Tally& rest;
    };

    for (const SlowCase& s :
         { SlowCase { { 1, 0, 16 }, thinSlowBand, thinSlowRest }, SlowCase { { 1, 1, 16 }, thinSlowBand, thinSlowRest },
           SlowCase { { 2, 0, 16 }, thinPanBand, thinPanRest }, SlowCase { { 2, -2, 16 }, thinPanBand, thinPanRest },
           SlowCase { { 4, 0, 16 }, thinPanBand, thinPanRest }, SlowCase { { 4, -4, 16 }, thinPanBand, thinPanRest } })
    {
        const SparseCase& c = s.c;
        flow.Reset();
        flow.Tuning() = tuning;

        for (int k = 0; k < c.frames; ++k)
        {
            auto picture = gpu.Picture(c.dx * k, c.dy * k, 1.0f, 0.0f, k, 0.0f, 0.0f, 0.0f, 2);
            flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            gpu.Submit();
        }

        const auto desc = flow.Flow()->GetDesc();
        const std::vector<float> field = ReadFlow(gpu, flow.Flow());
        const float lastX = c.dx * (c.frames - 1), lastY = c.dy * (c.frames - 1);
        const int margin = (int) std::ceil(std::max(std::fabs(lastX), std::fabs(lastY)) / 2.0f) + 24;
        uint64_t n[2] = {}, one[2] = {}, lineN = 0, lineOne = 0;

        for (uint32_t y = margin; y + margin < desc.Height; ++y)
            for (uint32_t x = margin; x + margin < desc.Width; ++x)
            {
                // The lines of LinesScene, where the last picture has them.
                const float fx = 2.0f * x + 1.0f, fy = 2.0f * y + 1.0f;
                bool band = false;
                for (int i = 0; i < 6; ++i)
                    band = band || std::fabs(fy - (150.0f + 97.0f * i + lastY)) <= 12.0f ||
                           std::fabs(fx - (140.0f + 173.0f * i + lastX)) <= 12.0f;

                // On a line that the pan moves across itself (a row line when the pan has a vertical part, a column
                // line when it has a sideways one): there the picture changes, so the still age must never hold it
                // still.
                bool line = false;
                for (int i = 0; i < 6; ++i)
                    line = line || (c.dy != 0.0f && std::fabs(fy - (150.0f + 97.0f * i + lastY)) <= 2.0f) ||
                           (c.dx != 0.0f && std::fabs(fx - (140.0f + 173.0f * i + lastX)) <= 2.0f);

                const size_t i = ((size_t) y * desc.Width + x) * 2;
                const bool right = std::hypot(field[i] + c.dx, field[i + 1] + c.dy) <= 1.0f;
                one[band] += right;
                ++n[band];
                lineOne += line && right;
                lineN += line;
            }

        s.band.Add((double) one[1] / n[1]);
        s.rest.Add((double) one[0] / n[0]);
        // From two pixels a picture the camera's motion is a whole pixel of the finest level: the lines themselves must
        // follow it (at one pixel a picture it rounds to no motion, so they are reported only).
        const double lineShare = lineN ? (double) lineOne / lineN : 1.0;
        const bool linePass = std::fabs(c.dx) < 2.0f || lineShare >= 0.95;
        ok = ok && linePass;
        printf("thin lines on a flat wall, slow pan (%3.1f, %3.1f), %d pictures: within 1 px, band of 12 px round the "
               "lines %5.1f%%, flat rest %5.1f%%, on the lines that cross %5.1f%%   %s\n",
               c.dx, c.dy, c.frames, 100.0 * one[1] / n[1], 100.0 * one[0] / n[0], 100.0 * lineShare,
               std::fabs(c.dx) < 2.0f ? "(reported)" : (linePass ? "ok" : "FAIL"));
    }

    flow.Tuning() = tuning;

    // A square moving over a background that moves differently: the flow near its edges, for each way of making it. Background
    // content the square uncovered this frame is left out (it was not in the previous picture). Every run is given a depth map
    // of the square; only the depth-aware settings read it.
    struct EdgeCase
    {
        float squareDx, squareDy, backgroundDx, backgroundDy;
        const char* what;
        float size = 256.0f; // smaller ones are scored only (score mode): a thing smaller than the smoothing's reach
    };

    const EdgeCase edgeCases[] = {
        { 24, 10, 0, 0, "square over a still background" },
        { -30, 0, 8, -4, "square against a panning background" },
        { 60, 30, 0, 0, "fast square over a still background" },
        { 8, 0, 0, 0, "slow square over a still background" },
        { 20, 8, 0, 0, "small square (24 px) over a still background", 24.0f },
        { 20, 8, 0, 0, "small square (12 px) over a still background", 12.0f },
    };

    struct Variant
    {
        const char* what;
        int cells;
        bool matching;
        float depthOffset = 0.0f; // the depth map's square this many pixels right of the picture's (a misaligned depth)
        float depthScale = 1.0f;  // the depth map's size relative to the picture's
        bool wrongDepth = false;  // a depth map whose edges are not the picture's (the square 60 px off)
        bool unsmoothed = false;  // the settings as given, without the smoothing
        bool look = false;        // the square also differs from the background in brightness and contrast
    };

    const Variant variants[] = {
        { "4 cells, no depth matching", 4, false },
        { "9 cells, no depth matching", 9, false },
        { "depth matching (default)", 9, true },
        { "depth matching, depth 2 px off", 9, true, 2.0f },
        { "depth matching, depth 5 px off", 9, true, 5.0f },
        { "depth matching, half-size depth", 9, true, 0.0f, 0.5f },
        { "depth matching, wrong depth", 9, true, 0.0f, 1.0f, true },
        { "as set", 0, true }, // cells 0: the settings' own; depth matching only if the settings have it
        { "as set, no depth", 0, false },
        { "as set, unsmoothed", 0, true, 0.0f, 1.0f, false, true },
        { "as set, no depth, unsmoothed", 0, false, 0.0f, 1.0f, false, true },
        { "as set, no depth, brighter square", 0, false, 0.0f, 1.0f, false, false, true },
    };
    constexpr int kVariants = (int) (sizeof(variants) / sizeof(variants[0]));

    for (const EdgeCase& e : edgeCases)
    {
        double share[kVariants] = {}, mean[kVariants] = {};
        printf("edges, %s\n", e.what);

        // A small square runs only on the settings as given: the variants compare ways to make a flow at a big one's edges.
        const bool smallSquare = e.size < 256.0f;
        for (int run = (score || smallSquare) ? 7 : 0; run < kVariants; ++run)
        {
            // Which runs: a big square 0..8 (7 and 8 in score mode) and the brighter square (11, reported); a small one
            // 7..10 (7 and 8 in score mode).
            const bool wanted = run <= 6    ? !smallSquare && !score
                                : run <= 8  ? true
                                : run <= 10 ? smallSquare && !score
                                            : !smallSquare;
            if (!wanted)
                continue;

            flow.Reset();
            flow.Tuning() = tuning;
            flow.Tuning().coarseCells = variants[run].cells != 0 ? variants[run].cells : tuning.coarseCells;
            flow.Tuning().depthMatching = variants[run].cells != 0 ? variants[run].matching
                                                                   : variants[run].matching && tuning.depthMatching;
            // The fixed variants without depth are plain matching (what depth matching is measured against), so they
            // match without the brightness weights; the depth variants and the "as set" ones match as the settings do.
            if (variants[run].cells != 0 && !variants[run].matching)
                flow.Tuning().lookWeights = false;
            if (variants[run].unsmoothed)
                flow.Tuning().smoothRadius = 0;
            const int frames = 3;
            const float size = e.size, x0 = 400.0f, y0 = 200.0f;

            for (int k = 0; k < frames; ++k)
            {
                const float qx = x0 + e.squareDx * k, qy = y0 + e.squareDy * k;
                const Variant& v = variants[run];
                auto picture = gpu.Picture(e.backgroundDx * k, e.backgroundDy * k, 1.0f, 0.0f, k, qx, qy, size, 0, 0,
                                           v.look ? 0.4f : 1.0f, v.look ? 0.55f : 0.0f);
                auto depth = v.wrongDepth ? gpu.DepthMap(qx + 60.0f, qy + 60.0f, size)
                                          : gpu.DepthMap(qx + v.depthOffset, qy, size, v.depthScale);
                flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, depth.Get(), DXGI_FORMAT_R32_FLOAT,
                              true);
                gpu.Submit();
            }

            const auto desc = flow.Flow()->GetDesc();
            const std::vector<float> field = ReadFlow(gpu, flow.Flow());
            const float sx = x0 + e.squareDx * (frames - 1), sy = y0 + e.squareDy * (frames - 1);
            const float px = sx - e.squareDx, py = sy - e.squareDy;
            auto inside = [&](float x, float y, float ax, float ay)
            { return x >= ax && x < ax + size && y >= ay && y < ay + size; };

            uint64_t n = 0, within = 0;
            double sum = 0;

            for (uint32_t y = 0; y < desc.Height; ++y)
                for (uint32_t x = 0; x < desc.Width; ++x)
                {
                    const float fx = 2.0f * x + 1.0f, fy = 2.0f * y + 1.0f; // the flow pixel's centre, in picture pixels
                    const float edge = std::min(std::min(std::fabs(fx - sx), std::fabs(fx - (sx + size))),
                                                std::min(std::fabs(fy - sy), std::fabs(fy - (sy + size))));
                    const bool nearEdge = edge <= 8.0f && fx > sx - 8.0f && fx < sx + size + 8.0f && fy > sy - 8.0f &&
                                          fy < sy + size + 8.0f;
                    const bool now = inside(fx, fy, sx, sy);

                    if (!nearEdge || (!now && inside(fx, fy, px, py)))
                        continue;

                    const float tx = now ? -e.squareDx : -e.backgroundDx, ty = now ? -e.squareDy : -e.backgroundDy;
                    const size_t i = ((size_t) y * desc.Width + x) * 2;
                    const float err = std::hypot(field[i] - tx, field[i + 1] - ty);
                    sum += err;
                    within += err <= 1.0f;
                    ++n;
                }

            share[run] = (double) within / n;
            mean[run] = sum / n;
            printf("  %-32s within 1 px %5.1f%%, mean %5.2f px\n", variants[run].what, 100.0 * share[run], mean[run]);
            if (run == 7)
                (e.size < 256.0f ? smallDepthOne : edgeDepthOne).Add(share[run]);
            else if (run == 8)
                (e.size < 256.0f ? smallNoDepthOne : edgeNoDepthOne).Add(share[run]);
            else if (run == 11)
                edgeLookNoDepthOne.Add(share[run]);
        }

        if (score)
            continue;

        // Smaller than the smoothing's reach: the smoothing must not take its motion away (no worse than without it).
        if (smallSquare)
        {
            const bool pass = share[7] >= share[9] - 0.02 && share[8] >= share[10] - 0.02;
            ok = ok && pass;
            printf("  %s\n", pass ? "ok" : "FAIL");
            continue;
        }

        flow.Tuning() = tuning;
        // 9 cells no worse than 4; depth matching well above none, and no worse than none with misaligned or wrong depth.
        const bool pass = share[1] >= share[0] - 0.01 && share[2] >= share[1] + 0.25 && share[2] >= 0.85 &&
                          share[3] >= share[1] && share[4] >= share[1] && share[5] >= share[2] - 0.01 &&
                          share[6] >= share[1] - 0.03;
        // Misaligned or wrong depth against what the flow does with no depth at all as set (with the brightness
        // weights, which plain matching above has not): a bad depth must cost no more than ignoring it.
        const bool badPass = share[3] >= share[8] && share[4] >= share[8] && share[6] >= share[8] - 0.03;
        ok = ok && pass && badPass;
        printf("  %s\n", pass ? "ok" : "FAIL");
        printf("  bad depth against no depth as set (%5.1f%%): 2 px off %+5.1f, 5 px off %+5.1f, wrong %+5.1f   %s\n",
               100.0 * share[8], 100.0 * (share[3] - share[8]), 100.0 * (share[4] - share[8]),
               100.0 * (share[6] - share[8]), badPass ? "ok" : "FAIL");
    }

    // Dark HDR pictures (scRGB, and the same light as PQ; the scene between 0.01 and 0.2 of 80 nits): the pans and the
    // grain of the cases above, with the luma the flow matches on as the settings give it. The perceptual luma (a
    // lightness curve after dividing by a white point) spreads the dark end that a tone-mapped linear luma squeezes, so
    // it must not do worse here.
    struct HdrScore
    {
        double panErr = 0, panHalf = 1, grainOne = 0, grainErr = 0;
    };

    auto hdrScore = [&](bool pq, bool perceptual) -> HdrScore
    {
        struct HdrCase
        {
            float dx, dy;
            int frames;
            float grain;
        };

        const HdrCase hdrCases[] = { { 3, -2, 2, 0 }, { 11, 7, 2, 0 },     { 21, -14, 2, 0 },
                                     { 60, 0, 2, 0 }, { 3, -2, 2, 0.06f }, { 12, 5, 4, 0.06f } };
        Tally err, half, grainShare, grainError;

        for (const HdrCase& c : hdrCases)
        {
            flow.Tuning() = tuning;
            flow.Tuning().perceptualLuma = perceptual;
            flow.Reset();

            for (int k = 0; k < c.frames; ++k)
            {
                auto picture = gpu.HdrPicture(c.dx * k, c.dy * k, c.grain, k, pq);
                flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, nullptr,
                              DXGI_FORMAT_UNKNOWN, true,
                              pq ? OpticalFlowDx12::Encoding::Pq : OpticalFlowDx12::Encoding::ScRgb);
                gpu.Submit();
            }

            const auto desc = flow.Flow()->GetDesc();
            const std::vector<float> field = ReadFlow(gpu, flow.Flow());
            const int margin = (int) std::ceil(std::max(std::fabs(c.dx), std::fabs(c.dy)) / 2.0f) + 24;
            uint64_t n = 0, within = 0, one = 0;
            double sum = 0;

            for (uint32_t y = margin; y + margin < desc.Height; ++y)
                for (uint32_t x = margin; x + margin < desc.Width; ++x)
                {
                    const size_t i = ((size_t) y * desc.Width + x) * 2;
                    const float e = std::hypot(field[i] + c.dx, field[i + 1] + c.dy);
                    sum += e;
                    within += e <= 0.5f;
                    one += e <= 1.0f;
                    ++n;
                }

            if (c.grain > 0.0f)
            {
                grainShare.Add((double) one / n);
                grainError.Add(sum / n);
            }
            else
            {
                err.Add(sum / n);
                half.Add((double) within / n);
            }
        }

        flow.Tuning() = tuning;
        HdrScore score;
        score.panErr = err.Mean();
        score.panHalf = half.worst;
        score.grainOne = grainShare.Mean();
        score.grainErr = grainError.Mean();
        return score;
    };

    HdrScore hdrNow;

    {
        printf("dark HDR pictures, the match's luma: legacy (tone-mapped) and perceptual (white %.0f nits)\n",
               tuning.hdrWhiteNits);

        for (int pq = 0; pq < 2; ++pq)
        {
            const HdrScore legacy = hdrScore(pq != 0, false);
            const HdrScore perceptual = hdrScore(pq != 0, true);
            // The pans must be no worse (they are well better) and the grain no worse by more than a hundredth of the
            // share within a pixel or five percent of the error: the grain here is flat dark noise, which neither luma
            // helps.
            const bool improves = perceptual.panErr <= legacy.panErr && perceptual.grainOne >= legacy.grainOne - 0.01 &&
                                  perceptual.grainErr <= legacy.grainErr * 1.05;
            printf(
                "  %-6s legacy: pan error %.4f px, worst within 0.5 px %5.1f%%, grain within 1 px %5.1f%%, grain error "
                "%.3f px\n"
                "         perceptual: pan error %.4f px, worst within 0.5 px %5.1f%%, grain within 1 px %5.1f%%, grain "
                "error %.3f px   %s\n",
                pq ? "PQ" : "scRGB", legacy.panErr, 100.0 * legacy.panHalf, 100.0 * legacy.grainOne, legacy.grainErr,
                perceptual.panErr, 100.0 * perceptual.panHalf, 100.0 * perceptual.grainOne, perceptual.grainErr,
                improves ? "ok" : "FAIL");
            ok = ok && (improves || !tuning.perceptualLuma);

            if (!pq)
                hdrNow = tuning.perceptualLuma ? perceptual : legacy;
        }
    }

    // The same-frame scene-cut detector. Each sequence is a few pictures; the flag the detector left after each frame
    // is read back. A real cut must be flagged on its own frame and on none before it (and the flow must be zero on
    // it); a fade, an exposure step, a fast pan and a HUD-heavy picture must be flagged on no frame. `value` is the
    // divergence the flag was judged by, to see how far each is from the threshold.
    struct CutSequence
    {
        const char* what;
        int frames;
        bool cutAtEnd; // the last frame is a hard cut from the one before; else no frame is
        bool reported; // only reported (a case no histogram can tell apart)
    };

    int cutHit = 0, cutTotal = 0, cutFalse = 0, quietTotal = 0;
    double worstQuiet = 0.0, weakestCut = 1.0;

    if (tuning.sceneCutDetector)
    {
        // a picture of sequence `id` at frame k
        auto make = [&](int id, int k) -> ComPtr<ID3D12Resource>
        {
            switch (id)
            {
            case 0: // hard cut: sky above -> sky below
                return k < 4 ? gpu.Picture(3.0f * k, 0, 1.0f, 0.0f, k, 0, 0, 0, 3)
                             : gpu.Picture(0, 0, 1.0f, 0.0f, k, 0, 0, 0, 4);
            case 1: // hard cut: a textured scene -> sky and ground
                return k < 4 ? gpu.Picture(3.0f * k, -2.0f * k, 1.0f, 0.0f, k)
                             : gpu.Picture(0, 0, 1.0f, 0.0f, k, 0, 0, 0, 3);
            case 2: // a cut to a picture of the same make (no histogram tells them apart): reported only
                return k < 4 ? gpu.Picture(3.0f * k, -2.0f * k, 1.0f, 0.0f, k)
                             : gpu.Picture(2000.0f, 900.0f, 1.0f, 0.0f, k);
            case 3: // a fade, 5% a picture
                return gpu.Picture(3.0f * k, 0, std::pow(0.95f, (float) k), 0.0f, k, 0, 0, 0, 3);
            case 4:
                return gpu.Picture(3.0f * k, -2.0f * k, 0.95f * std::pow(0.95f, (float) k), 0.0f, k);
            case 5: // an exposure step to twice the brightness
                return gpu.Picture(3.0f * k, 0, k < 4 ? 0.4f : 0.8f, 0.0f, k, 0, 0, 0, 3);
            case 6: // and back down to half
                return gpu.Picture(3.0f * k, 0, k < 4 ? 0.8f : 0.4f, 0.0f, k, 0, 0, 0, 3);
            case 7: // a fast pan
                return gpu.Picture(160.0f * k, -40.0f * k, 1.0f, 0.0f, k);
            case 8:
                return gpu.Picture(160.0f * k, 0, 1.0f, 0.0f, k, 0, 0, 0, 3);
            case 9: // a HUD-heavy picture over a panning one
                return gpu.Picture(12.0f * k, 4.0f * k, 1.0f, 0.0f, k, 0, 0, 0, 0, 2);
            case 10:
                return gpu.Picture(12.0f * k, 0, 1.0f, 0.0f, k, 0, 0, 0, 3, 2);
            case 11: // dark and grainy
                return gpu.Picture(3.0f * k, -2.0f * k, 0.08f, 3.0f, k);
            default: // a fade, 10% a picture
                return gpu.Picture(3.0f * k, 0, std::pow(0.9f, (float) k), 0.0f, k, 0, 0, 0, 3);
            }
        };

        const CutSequence sequences[] = {
            { "hard cut, sky above -> sky below", 5, true, false },
            { "hard cut, textured scene -> sky and ground", 5, true, false },
            { "hard cut to a picture of the same make", 5, true, true },
            { "fade, 5% a picture (sky and ground)", 9, false, false },
            { "fade, 5% a picture (textured)", 9, false, false },
            { "exposure step to twice the brightness", 8, false, false },
            { "exposure step to half the brightness", 8, false, false },
            { "fast pan, 160 px a picture (textured)", 5, false, false },
            { "fast pan, 160 px a picture (sky and ground)", 5, false, false },
            { "HUD-heavy picture, textured scene panning", 5, false, false },
            { "HUD-heavy picture, sky and ground panning", 5, false, false },
            { "dark and grainy", 5, false, false },
            { "fade, 10% a picture (sky and ground)", 9, false, false },
        };

        printf("scene-cut detector, threshold %.2f\n", tuning.sceneCutThreshold);

        for (int id = 0; id < (int) (sizeof(sequences) / sizeof(sequences[0])); ++id)
        {
            const CutSequence& seq = sequences[id];
            flow.Reset();
            std::vector<CutRead> reads;
            float flowMagnitude = -1.0f;

            for (int k = 0; k < seq.frames; ++k)
            {
                auto picture = make(id, k);
                flow.Dispatch(gpu.list.Get(), picture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
                reads.push_back(ReadCut(gpu, flow.SceneCutTexture()));

                if (seq.cutAtEnd && k == seq.frames - 1)
                {
                    const auto desc = flow.Flow()->GetDesc();
                    const std::vector<float> field = ReadFlow(gpu, flow.Flow());
                    double sum = 0;
                    for (size_t i = 0; i < field.size(); i += 2)
                        sum += std::hypot(field[i], field[i + 1]);
                    flowMagnitude = (float) (sum / ((double) desc.Width * desc.Height));
                }
            }

            // the frames that have a previous one to compare with: from the second on
            bool flaggedEarlier = false;
            float largestBefore = 0.0f;
            for (int k = 1; k + (seq.cutAtEnd ? 1 : 0) < seq.frames; ++k)
            {
                flaggedEarlier = flaggedEarlier || reads[k].flag;
                largestBefore = std::max(largestBefore, reads[k].value);
            }

            const CutRead& last = reads.back();
            bool pass;

            if (seq.cutAtEnd)
            {
                pass = last.flag && !flaggedEarlier && flowMagnitude >= 0.0f && flowMagnitude < 0.01f;
                if (!seq.reported)
                {
                    ++cutTotal;
                    cutHit += pass ? 1 : 0;
                    weakestCut = std::min(weakestCut, (double) last.value);
                }
                printf(
                    "  %-52s divergence on the cut %.3f, before it at most %.3f, flow on the cut frame %.3f px   %s\n",
                    seq.what, last.value, largestBefore, flowMagnitude,
                    seq.reported
                        ? (last.flag ? "(reported: flagged)" : "(reported: not flagged; the readback finds it later)")
                        : (pass ? "ok" : "FAIL"));
                ok = ok && (pass || seq.reported);
            }
            else
            {
                const float largest = std::max(largestBefore, last.value);
                pass = !flaggedEarlier && !last.flag;
                ++quietTotal;
                cutFalse += pass ? 0 : 1;
                worstQuiet = std::max(worstQuiet, (double) largest);
                printf("  %-52s largest divergence %.3f, flagged: %s   %s\n", seq.what, largest, pass ? "no" : "YES",
                       pass ? "ok" : "FAIL");
                ok = ok && pass;
            }
        }

        // The flag must be left out of the trust mask and the match when the detector is off.
        {
            flow.Tuning() = tuning;
            flow.Tuning().sceneCutDetector = false;
            flow.Reset();
            auto a = make(0, 0);
            flow.Dispatch(gpu.list.Get(), a.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            gpu.Submit();
            const bool none = flow.SceneCutFlag() == nullptr;
            printf("  %-52s %s\n", "detector off: no flag is offered", none ? "ok" : "FAIL");
            ok = ok && none;
            flow.Tuning() = tuning;
        }
    }

    if (score)
    {
        printf("SCORE radius=%d coarse=%d lambda=%g history=%d cells=%d smooth=%d knee=%g dmatch=%d global=%d "
               "inverse=%d still=%d stilleps=%g stillmargin=%g look=%d lookrange=%g lookdist=%g | "
               "pan0.5 %.4f panErr %.4f bright0.5 %.4f grain1 %.4f sparse1 %.4f thin1 %.4f aliasErr %.1f "
               "edgeDepth1 %.4f edgeNoDepth1 %.4f smallDepth1 %.4f smallNoDepth1 %.4f hudStill %.4f wallStill %.4f "
               "hudStillLong %.4f thinSlowBand %.4f thinSlowRest %.4f thinPanBand %.4f thinPanRest %.4f "
               "edgeLookNoDepth1 %.4f "
               "cutHit %d/%d cutFalse %d/%d weakestCut %.3f worstQuiet %.3f hdrPanErr %.4f hdrGrain1 %.4f\n",
               tuning.radius, tuning.coarseRadius, tuning.lambda, tuning.useHistory ? 1 : 0, tuning.coarseCells,
               std::clamp(tuning.smoothRadius, 0, 4), tuning.confidenceKnee, tuning.depthMatching ? 1 : 0,
               tuning.globalCandidate ? 1 : 0, tuning.inverseRefinement ? 1 : 0, tuning.stillFrames,
               tuning.stillEpsilon, tuning.stillMargin, tuning.lookWeights ? 1 : 0, tuning.lookRange,
               tuning.lookDistance, panHalf.worst, panError.Mean(), brightHalf.worst, grainOne.Mean(), sparseOne.Mean(),
               thinOne.Mean(), thinAliasError.Mean(), edgeDepthOne.Mean(), edgeNoDepthOne.Mean(), smallDepthOne.Mean(),
               smallNoDepthOne.Mean(), hudStill.Mean(), wallStill.Mean(), hudStillLong.Mean(), thinSlowBand.Mean(),
               thinSlowRest.Mean(), thinPanBand.Mean(), thinPanRest.Mean(), edgeLookNoDepthOne.Mean(), cutHit, cutTotal,
               cutFalse, quietTotal, weakestCut, worstQuiet, hdrNow.panErr, hdrNow.grainOne);
        return 0;
    }

    flow.Tuning() = tuning;
    ok = CheckJitter(gpu, flow) && ok;
    printf(ok ? "all passed\n" : "FAILED\n");
    return ok ? 0 : 1;
}
