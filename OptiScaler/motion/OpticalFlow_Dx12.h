#pragma once

// Dense optical flow on the GPU for the native input producer (DLSS-NR in a game that makes no upscaler call, so there are no
// motion vectors to take from one). Our own implementation of the standard method: a luma pyramid, block matching from the
// coarsest level down with a small search around the coarser level's answer, a few Lucas-Kanade gradient steps for the
// sub-pixel part, and a 3x3 median over the result. At each level the candidates are the coarser level's answer at the
// nearest cells, the last frame's flow at the same place, what the whole picture moved by last frame (when it moved as one)
// and no motion; the best of them is refined by a small search. The match compares windows with their mean brightness taken
// out (a fade or eye adaptation does not move anything), and with depth it counts only the window's samples on the pixel's
// own surface (a nearer thing's edge does not drag its motion onto what lies beside it: adaptive support weights). The
// result is smoothed where the matching is not sure (flat or grainy areas) without crossing the edges of moving things: each
// neighbour counts by how much picture structure its match had, how close its motion is and how close its brightness is.
// Nothing is taken from any shader of another project; some ideas are credited in docs/CREDITS.md.
//
// A hard cut is also found on the same frame, on the GPU, by the scene-cut detector (motion/SceneCut_Dx12.h) run on the
// first luma level: its flag is read by the match and the trust mask in that same frame, with no wait for the CPU.
//
// Self-contained: it needs only D3D12 and the HLSL compiler, so tests/nr_optical_flow_gpu.cpp drives it with synthetic images
// and known motion, with no game.
//
// Convention: for each pixel of the current frame the flow is the offset, in full-resolution pixels, to where the same
// content was in the previous frame (the usual game motion-vector direction, "from here to where it came from"). The flow
// texture is half the colour's resolution (R16G16B16A16_FLOAT, x and y in .xy, the match's confidence 0..1 in .z); a
// consumer samples it bilinearly.
//
// Dispatch() records onto a command list the caller owns and changes the descriptor heaps bound on it, so it belongs on a
// list of our own (the finished-picture seam), not on a game's list.

#include "SceneCut_Dx12.h"

#include <d3d12.h>
#include <dxgiformat.h>
#include <cstdint>
#include <string>

class OpticalFlowDx12
{
  public:
    static constexpr int kLevels = 6; // pyramid levels (1/2 .. 1/64); the coarsest reaches about 250 pixels of motion

    // What the picture's values mean, for the luma the match works on: gamma-encoded SDR (used as it is), linear scRGB
    // (1.0 = 80 nits) or PQ (HDR10).
    enum class Encoding
    {
        Srgb,
        ScRgb,
        Pq
    };

    OpticalFlowDx12() = default;
    ~OpticalFlowDx12();

    OpticalFlowDx12(const OpticalFlowDx12&) = delete;
    OpticalFlowDx12& operator=(const OpticalFlowDx12&) = delete;

    // Compiles the shaders and makes the pipelines. False with a reason in Error().
    bool Init(ID3D12Device* device);

    // Records the flow from the previous Dispatch's frame to this one. `color` must be in a shader-readable state
    // (NON_PIXEL_SHADER_RESOURCE) and stays so. After it returns, Flow() is readable and FlowValid() says whether it
    // holds a flow (false on the first frame, after Reset() and after a size change). depth (optional, any size,
    // readable as depthFormat, NON_PIXEL_SHADER_RESOURCE) shows the match where one surface ends and another begins, so
    // the edge of a nearer thing does not carry its motion onto what lies beside it. encoding says what the colour's
    // values mean (see Settings::perceptualLuma).
    bool Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* color, DXGI_FORMAT colorFormat,
                  ID3D12Resource* depth = nullptr, DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN,
                  bool depthReversed = true, Encoding encoding = Encoding::Srgb, int finestLevel = 0,
                  int smoothRadius = -1, float jitterX = 0.f, float jitterY = 0.f);

    // Forget the previous frame (a scene cut, a resolution change): the next Dispatch only stores its picture.
    void Reset()
    {
        _havePrevious = false;
        _globalReady = false;
        _sceneCut.Reset();
        _ageValid = false;
    }

    // A picture of the flow for a menu: hue is the direction, brightness the speed up to maxSpeed pixels, black is still.
    // R8G8B8A8_UNORM at the flow's size, left in the PIXEL_SHADER_RESOURCE state. Call after Dispatch() when FlowValid().
    bool Visualise(ID3D12GraphicsCommandList* list, float maxSpeed);
    ID3D12Resource* Preview() const { return _preview.resource; }

    // The luma at the flow's size (R32_FLOAT, NON_PIXEL_SHADER_RESOURCE) of the frame last passed to Dispatch() and of
    // the one before it; the trust mask compares them. The previous one is only meaningful when FlowValid().
    ID3D12Resource* LumaOfLastFrame(int level = 0) const
    {
        return _pyramid[1 - _current][_finestLevel + level].resource;
    }
    ID3D12Resource* LumaOfFrameBefore() const { return _pyramid[_current][_finestLevel].resource; }

    // The flow, in the NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE state between Dispatches.
    ID3D12Resource* Flow() const { return _flow.resource; }
    bool FlowValid() const { return _flowValid; }
    bool UsedDepth() const { return _usedDepth; } // the last Dispatch matched with depth

    // The scene-cut flag of the last Dispatch: a 2x1 R32_UINT texture in the NON_PIXEL_SHADER_RESOURCE state, texel 0
    // is 1 on a frame found to be a hard cut, texel 1 the float bits of the divergence (0..1) it was judged by. Null
    // when the detector did not run; the trust mask takes it as its input, so it distrusts the cut frame whole.
    ID3D12Resource* SceneCutFlag() const { return _sceneCutRan ? _sceneCut.Flag() : nullptr; }
    // For the test: the texture even when the detector did not run.
    ID3D12Resource* SceneCutTexture() const { return _sceneCut.Flag(); }
    uint32_t FlowWidth() const { return _flow.width; }
    uint32_t FlowHeight() const { return _flow.height; }
    int FinestLevel() const { return _finestLevel; }

    // Search radius, in pixels of each level, around the coarser level's answer (coarsest level searches kCoarseRadius).
    // Penalty added per pixel of distance from that answer, which keeps flat areas from picking noise.
    struct Settings
    {
        int radius = 1;
        int coarseRadius = 4;
        float lambda = 0.03f;
        bool useHistory = true;         // last frame's flow as a candidate
        int coarseCells = 9;            // how many of the coarser level's nearest cells are candidates (1..9: the 3x3)
        int smoothRadius = 4;           // the edge-aware smoothing of the result, in flow pixels (0 = off)
        float confidenceKnee = 0.004f;  // how much picture structure counts as a trustworthy match
        bool depthMatching = true;      // with depth: the block match counts the window's samples on this pixel's surface
        bool globalCandidate = true;    // the last frame's whole-picture motion is a candidate everywhere
        bool inverseRefinement = false; // the sub-pixel steps use the current frame's gradients (found once, cheaper)

        // Find a hard cut on the frame it happens, on the GPU (see above); the divergence past which a frame is a cut
        // (0 alike .. 1 nothing alike).
        bool sceneCutDetector = true;
        float sceneCutThreshold = 0.45f;

        // The luma the match works on. Perceptual: a gamma-encoded SDR picture is used as it is (it already is
        // perceptual), a linear (scRGB) or PQ picture is turned into linear light, divided by hdrWhiteNits (the white
        // of the picture; scRGB 1.0 is 80 nits) and put through the CIE lightness curve, so a dark HDR scene is not
        // squeezed into the bottom of the range. Off: the Rec.601 weights on the values as they are, then l / (1 + l),
        // whatever they mean.
        bool perceptualLuma = true;
        float hdrWhiteNits = 203.0f; // the reference white of BT.2408 (HDR graphics white)

        // Zero preference on the finest level: where the best offset is within zeroReach pixels (0: any) of no motion
        // and no motion costs no more than zeroMargin above it (luma units; 0 = off), the flow is no motion, so flat
        // and grainy ground that matched on noise stays still.
        float zeroMargin = 0.0f;
        int zeroReach = 0;

        // Still age, on the finest level: how many frames in a row the picture has not changed around a pixel (the
        // largest difference to the last frame, at the same place, over the match window stays under stillEpsilon, in
        // luma units: about one 8-bit step). Where that is at least stillFrames (0 = off, at most 255), the
        // whole-picture camera candidate no longer wins a tie with no motion: it must beat it by stillMargin (luma
        // units, the match's cost). A HUD is unchanged for as long as it is shown; a flat patch of a panning wall stays
        // unchanged only until an edge or a line passes near it, so a still HUD stays still while the picture pans
        // behind it, and the camera candidate still carries a flat wall near its lines and edges, which a rule inside
        // one frame cannot tell apart. A plain patch farther than the window from any of them is held still too (the
        // thinPan tallies of the flow test). "Unchanged" is the luma at the window's sixteen samples, the pixel and its
        // four even neighbours, each within stillEpsilon of the last frame: such a patch looks the same either way, but
        // a change under stillEpsilon a frame (a slow, smooth gradient) or of colour alone is not seen.
        int stillFrames = 8;
        float stillEpsilon = 0.004f;
        float stillMargin = 0.02f;

        // Brightness weights, on the finest level: each sample of the window counts by how close its luma is to the
        // centre pixel's (lookRange, luma units) and a little by its distance (lookDistance, half-resolution pixels),
        // so the window does not take the motion of a thing that looks different across an edge. Without depth they are
        // the depth weights' counterpart; with depth they multiply them, so a misaligned or wrong depth costs no more
        // than having none. Measured against shifted windows, a small window where the match is unsure and a wider
        // search where it is poor (2026-10-07): these won on every edge tally and cost nothing measurable; the others
        // gained less, cost up to 0.9 ms or hurt grain.
        bool lookWeights = true;
        float lookRange = 0.06f;   // 0.04 is best on edges (0.759 vs 0.746) but costs dark HDR grain past its gate
        float lookDistance = 8.0f; // 8 keeps the grain tally nearly whole (4: -0.02), 2 costs 0.06
    };

    Settings& Tuning() { return _settings; }
    const std::string& Error() const { return _error; }

    // Timing, for the GPU test and [DlssNr] NativeStageTimings: with a timestamp query heap given, every pass records a
    // timestamp after itself, from entry `base` of the heap on. The owner resolves the TimestampCount() entries from
    // `base` (the one before the first pass included) and clears the count before the next Dispatch();
    // TimestampName(i) names the pass that ended at entry base + i (entry base + 0 is the start).
    void SetTimestampHeap(ID3D12QueryHeap* heap, uint32_t capacity, uint32_t base = 0)
    {
        _timeHeap = heap;
        _timeCapacity = capacity;
        _timeBase = base;
        _timeCount = 0;
    }
    uint32_t TimestampCount() const { return _timeCount; }
    void ClearTimestamps() { _timeCount = 0; }
    const char* TimestampName(uint32_t index) const { return index < _timeCount ? _timeNames[index] : ""; }

  private:
    struct Tex
    {
        ID3D12Resource* resource = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };

    struct Constants
    {
        uint32_t sizeX, sizeY, auxX, auxY;
        int32_t radius;
        uint32_t hasPrediction;
        float lambda;
        float scale;
        uint32_t hasHistory;
        float knee;
        uint32_t coarseCells, depthMatching;
        uint32_t depthX, depthY, reversed, hasGlobal;
        uint32_t inverseRefinement, sceneCutEnabled;
        float whiteNits;  // Luma: the white of an HDR picture, in nits
        float zeroMargin; // Match, finest level: no motion wins a near tie
        uint32_t zeroReach;
        uint32_t stillFrames; // Match, finest level: the still age is kept (0 = not); the camera must win by a margin
        uint32_t hasAge;      // Match: the last frame's ages are there to read
        float stillEpsilon;   // Match: under this the window did not change
        float stillMargin;    // Match: what the camera must win by where the age is at least stillFrames
        uint32_t lookWeights; // Match, finest level, no depth: the brightness weights (see Settings)
        float lookRange, lookDistance;
        float jitterX, jitterY;
        uint32_t padding[2];
    };

    bool CreateTexture(Tex& tex, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name);
    bool EnsureSize(uint32_t width, uint32_t height, int finestLevel);
    void ReleaseTextures();
    void Transition(ID3D12GraphicsCommandList* list, Tex& tex, D3D12_RESOURCE_STATES state);
    void StampBegin(ID3D12GraphicsCommandList* list);
    void StampEnd(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const char* name = nullptr);
    void Pass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* src0, DXGI_FORMAT format0,
              ID3D12Resource* src1, DXGI_FORMAT format1, ID3D12Resource* src2, DXGI_FORMAT format2, Tex& dst,
              DXGI_FORMAT dstFormat, const Constants& constants, ID3D12Resource* src3 = nullptr,
              DXGI_FORMAT format3 = DXGI_FORMAT_UNKNOWN, ID3D12Resource* src4 = nullptr,
              DXGI_FORMAT format4 = DXGI_FORMAT_UNKNOWN, ID3D12Resource* src5 = nullptr,
              DXGI_FORMAT format5 = DXGI_FORMAT_UNKNOWN, ID3D12Resource* src6 = nullptr,
              DXGI_FORMAT format6 = DXGI_FORMAT_UNKNOWN);

    // The finest match pass also reads the last frame's ages and writes this frame's; Pass binds these two for it (and
    // an unused view for every other pass).
    ID3D12Resource* _passAgeIn = nullptr;
    ID3D12Resource* _passAgeOut = nullptr;

    ID3D12Device* _device = nullptr;
    ID3D12RootSignature* _rootSignature = nullptr;
    ID3D12PipelineState* _luma = nullptr; // the old luma
    ID3D12PipelineState* _lumaSdr = nullptr;
    ID3D12PipelineState* _lumaScRgb = nullptr;
    ID3D12PipelineState* _lumaPq = nullptr;
    ID3D12PipelineState* _down = nullptr;
    ID3D12PipelineState* _match = nullptr;
    ID3D12PipelineState* _median = nullptr;
    ID3D12PipelineState* _smooth = nullptr;
    ID3D12PipelineState* _visualise = nullptr;
    ID3D12PipelineState* _global = nullptr;
    ID3D12DescriptorHeap* _heap = nullptr;
    UINT _descriptorSize = 0;
    UINT _heapCursor = 0;

    Tex _pyramid[2][kLevels]; // luma, 1/2 .. 1/64 of the colour; one set is the current frame, the other the previous
    Tex _levelFlow[2][kLevels]; // this frame's flow at each level, and the last frame's (a candidate for this one)
    Tex _flowMedian; // the median's result, which the smoothing reads
    Tex _globalFlow; // 1x1: the whole picture's motion in full-resolution pixels, z = 1 when there was enough to say
    Tex _flow;
    Tex _preview;
    Tex _age[2]; // R8_UINT, flow resolution: the still age (see Settings::stillFrames), this frame's and the last one's
    bool _ageValid = false; // the last frame's ages are there (not after a Reset or a size change)
    int _current = 0;
    bool _havePrevious = false;
    bool _flowValid = false;
    bool _usedDepth = false;
    bool _globalReady = false; // _globalFlow holds the last frame's whole-frame motion

    // The scene-cut detector, on the first luma level; the last Dispatch ran it, so its flag is this frame's.
    SceneCutDx12 _sceneCut;
    bool _sceneCutRan = false;
    int _finestLevel = 0;
    uint32_t _width = 0;
    uint32_t _height = 0;

    ID3D12QueryHeap* _timeHeap = nullptr;
    uint32_t _timeCapacity = 0;
    uint32_t _timeBase = 0;
    uint32_t _timeCount = 0;
    const char* _timeNames[64] = {};

    Settings _settings;
    std::string _error;
};
