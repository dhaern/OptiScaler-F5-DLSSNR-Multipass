#pragma once

#include "Luma_Hlsl.h"

// The optical flow's compute shaders (motion/OpticalFlow_Dx12.cpp). The scene-cut passes are in motion/SceneCut_Hlsl.h,
// and the luma both use is in motion/Luma_Hlsl.h.
// Compiled ahead of time into native/F5LowShaderBytecode.h by tests/nr_shader_bytecode_smoke.cpp (run it with --write
// after changing anything here; without, it checks the embedded bytecode is current).

namespace OpticalFlowHlsl
{
inline constexpr const char* kSource =
    R"HLSL(
cbuffer P : register(b0)
{
    uint2 size;      // the size of what is written
    uint2 aux;       // the size of what is read (luma, down) or unused
    int radius;
    uint hasPrediction;
    float lambda;
    float scale;
    uint hasHistory;
    float knee;
    uint coarseCells;
    uint depthMatching; // the window's samples count by how near their depth is to this pixel's (SceneDepth, t4)
    uint2 depthSize;
    uint reversed;
    uint hasGlobal; // Match: the frame-wide candidate (GlobalFlow) is there to be tried
    uint inverseRefinement; // Match: the sub-pixel steps use the current frame's gradients, found once
    uint sceneCutEnabled;   // Match: the scene-cut flag (CutFlag, t6) is there to be read
    float whiteNits;        // Luma: what an HDR picture's white is, in nits (scRGB 1.0 is 80 nits)
    float zeroMargin;       // Match, finest level: no motion wins when its cost is within this of the best (0 = off)
    uint zeroReach;         // Match: ... but only over a best offset no larger than this many pixels (0 = any)
    uint stillFrames;       // Match, finest level: keep the still age (0 = not); where the last age is this much or more the
                            // camera candidate must beat no motion by stillMargin
    uint hasAge;            // Match: the last frame's ages (LastAge, t7) are there to read
    float stillEpsilon;     // Match: the window did not change if its largest difference to the last frame is under this
    float stillMargin;      // Match: what the camera candidate must win by over no motion where the picture is still
    uint lookWeights;       // Match, finest level, no depth: the window's samples count by how near their luma is to the pixel's
    float lookRange;        // ... within this much luma
    float lookDistance;     // ... and a little less with their distance from it, in this level's pixels
    float2 jitter;          // Luma: the scene's offset in the colour's pixels (0 = none)
};

SamplerState Linear : register(s0);
Texture2D<float4> Color : register(t0);
Texture2D<float>  CurLuma : register(t0);
Texture2D<float>  PrevLuma : register(t1);
Texture2D<float4> Prediction : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float>  GuideLuma : register(t1);
Texture2D<float4> FlowIn : register(t0);
Texture2D<float>  SceneDepth : register(t4);
Texture2D<float4> GlobalFlow : register(t5);
Texture2D<uint>   CutFlag : register(t6);
Texture2D<uint>   LastAge : register(t7);
RWTexture2D<float>  OutLuma : register(u0);
RWTexture2D<float4> OutFlow : register(u0);
RWTexture2D<uint>   OutAge : register(u1);

)HLSL" F5LOW_LUMA_HLSL
    R"HLSL(// Cubic B-spline reconstruction, widened by the 2:1 luma reduction. Unlike a bilinear
// de-jitter followed by a box, it rejects detail above the luma Nyquist before decimation, and its
// bandwidth barely changes with sample phase. Positive adjacent taps combine into one bilinear read.
float CubicBSpline(float x)
{
    x = abs(x);
    if (x < 1.0)
        return (4.0 + x * x * (3.0 * x - 6.0)) / 6.0;
    float t = max(2.0 - x, 0.0);
    return t * t * t / 6.0;
}

void LumaTaps(float phase, out float4 weights, out float4 positions)
{
    [unroll] for (int i = 0; i < 4; ++i)
    {
        float at = 2.0 * i - 3.0;
        float a = CubicBSpline((at - phase) * 0.5);
        float b = CubicBSpline((at + 1.0 - phase) * 0.5);
        weights[i] = (a + b) * 0.5;
        positions[i] = at + b / (a + b);
    }
}

// Luma of the colour, reduced 2:1 into the first level.
[numthreads(8, 8, 1)]
void Luma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    if (any(jitter != 0))
    {
        float4 wx, wy, px, py;
        LumaTaps(0.5 + jitter.x, wx, px);
        LumaTaps(0.5 + jitter.y, wy, py);
        float3 color = 0.0;
        [unroll] for (int y = 0; y < 4; ++y)
            [unroll] for (int x = 0; x < 4; ++x)
                color += wx[x] * wy[y] * Color.SampleLevel(Linear,
                    (float2(id.xy) * 2.0 + float2(px[x], py[y]) + 0.5) / float2(aux), 0).rgb;
        OutLuma[id.xy] = LumaOf(color);
        return;
    }

    float sum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
        {
            int2 p = min(int2(id.xy) * 2 + int2(i, j), int2(aux) - 1);
            sum += LumaOf(Color.Load(int3(p, 0)).rgb);
        }

    OutLuma[id.xy] = sum * 0.25;
}

// The next level: a 2x2 average of the one above.
[numthreads(8, 8, 1)]
void Down(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float sum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
            sum += CurLuma.Load(int3(min(int2(id.xy) * 2 + int2(i, j), int2(aux) - 1), 0));

    OutLuma[id.xy] = sum * 0.25;
}

// The previous frame at a fractional pixel position, filtered.
float Sample(float2 position)
{
    return PrevLuma.SampleLevel(Linear, (position + 0.5) / float2(size), 0);
}

// How far the surface at uv (0..1 across the picture) is, larger is farther; only the ratio of two is used.
float Distance(float2 uv)
{
    int2 at = min(int2(uv * float2(depthSize)), int2(depthSize) - 1);
    float d = SceneDepth.Load(int3(at, 0));
    return 1.0 / max(reversed != 0 ? d : 1.0 - d, 1e-6);
}

// How much a sample at distance z counts beside one at zc: about 1 on the same surface, little across a depth edge.
float SameSurface(float z, float zc)
{
    float rel = abs(z - zc) / max(min(z, zc), 1e-6);
    return max(exp(-rel * rel / 0.01), 0.02);
}

// Sum of absolute differences between the current frame around p and the previous frame around p + d: sixteen samples, two
// pixels apart, over an 8x8 window, each counted by w. Each window's mean is taken out first, so a picture that got brighter
// or darker as a whole (eye adaptation, a fade, a flash) still matches where its content went. cur is the current frame's
// sixteen samples around p, which are the same for every offset, so the caller loads them once.
float Cost(int2 p, int2 d, float w[16], float cur[16])
{
    float diff[16];
    float mean = 0.0, total = 0.0;
    int2 hi = int2(size) - 1;

    [unroll] for (int j = 0; j < 4; ++j)
        [unroll] for (int i = 0; i < 4; ++i)
        {
            int2 q = int2(2 * i - 3, 2 * j - 3);
            float c = cur[j * 4 + i];
            float r = PrevLuma.Load(int3(clamp(p + q + d, 0, hi), 0));
            diff[j * 4 + i] = c - r;
            mean += w[j * 4 + i] * (c - r);
            total += w[j * 4 + i];
        }

    mean /= total;
    float s = 0.0;

    [unroll] for (int k = 0; k < 16; ++k)
        s += w[k] * abs(diff[k] - mean);

    return s * 16.0 / total;
}

// The biggest absolute difference to the last frame at the same place (no offset, the mean not taken out): the still age
// is told from it. Only the finest level asks. The window's sixteen samples lie at odd offsets, so the pixel itself and
// its even neighbours are checked too: a dot one pixel wide that moves by an even step would otherwise slip between the
// samples and be held still.
float LargestChange(int2 p, float cur[16])
{
    float largest = 0.0;
    int2 hi = int2(size) - 1;

    [unroll] for (int j = 0; j < 4; ++j)
        [unroll] for (int i = 0; i < 4; ++i)
            largest = max(largest, abs(cur[j * 4 + i] - PrevLuma.Load(int3(clamp(p + int2(2 * i - 3, 2 * j - 3), 0, hi), 0))));

    static const int2 kEven[5] = { int2(0, 0), int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2) };

    [unroll] for (int k = 0; k < 5; ++k)
    {
        int2 q = clamp(p + kEven[k], 0, hi);
        largest = max(largest, abs(CurLuma.Load(int3(q, 0)) - PrevLuma.Load(int3(q, 0))));
    }

    return largest;
}
)HLSL" // the compiler limits one string literal to 16 KB; the source goes on in a second one
    R"HLSL(
// How much a sample with luma l counts beside the centre pixel's lc: about 1 where the look is alike, little across an edge
// that changes it, and a little less with the distance from the centre (offset, in pixels of this level).
float LookWeight(float l, float lc, float2 offset)
{
    float dl = (l - lc) / lookRange;
    return max(exp(-dl * dl - 0.5 * dot(offset, offset) / (lookDistance * lookDistance)), 0.02);
}
)HLSL"
    R"HLSL(

// Block matching at one level: look around the coarser level's answer (doubled, it is in that level's pixels) for the offset
// into the previous frame with the smallest difference, then a few gradient steps for the part of a pixel.
[numthreads(8, 8, 1)]
void Match(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    // A hard cut found on this very frame: the previous frame shows another scene, so there is nothing to match. No motion and
    // no confidence at every level (the whole group leaves here or none does: the flag is one value for the dispatch).
    if (sceneCutEnabled != 0 && CutFlag.Load(int3(0, 0, 0)) != 0)
    {
        OutFlow[id.xy] = float4(0.0, 0.0, 0.0, 1.0);

        if (stillFrames != 0)
            OutAge[id.xy] = 0; // the picture is another one: nothing has been still yet

        return;
    }

    int2 p = int2(id.xy);

    // How much each sample of the window counts: all alike, or with depth-aware matching only those on this pixel's surface,
    // so the edge of something nearer does not decide the match of what lies beside it.
    float w[16];
    float zc = depthMatching != 0 ? Distance((float2(p) + 0.5) / float2(size)) : 0.0;

    [unroll] for (int wj = 0; wj < 4; ++wj)
        [unroll] for (int wi = 0; wi < 4; ++wi)
        {
            int2 q = clamp(p + int2(2 * wi - 3, 2 * wj - 3), 0, int2(size) - 1);
            w[wj * 4 + wi] = depthMatching != 0 ? SameSurface(Distance((float2(q) + 0.5) / float2(size)), zc) : 1.0;
        }

    // The current frame's window, loaded once for every candidate and search offset below.
    float cur[16];

    [unroll] for (int cj = 0; cj < 4; ++cj)
        [unroll] for (int ci = 0; ci < 4; ++ci)
            cur[cj * 4 + ci] = CurLuma.Load(int3(clamp(p + int2(2 * ci - 3, 2 * cj - 3), 0, int2(size) - 1), 0));

    // On the finest level the samples also count by how near their brightness is to this pixel's: without depth the depth
    // weights' counterpart, so the edge of a thing that looks different does not decide the match beside it, and with depth
    // on top of them, which keeps a misaligned depth from doing the same. The weights are then scaled back to the sum they
    // had: the cost and the sub-pixel step do not change with a common scale, but the confidence below is a sum over the
    // weights, and so its knee keeps the meaning it has with the depth weights alone.
    if (lookWeights != 0)
    {
        const float lc = CurLuma.Load(int3(p, 0));
        float before = 0.0, after = 0.0;

        [unroll] for (int lj = 0; lj < 4; ++lj)
            [unroll] for (int li = 0; li < 4; ++li)
            {
                before += w[lj * 4 + li];
                w[lj * 4 + li] *= LookWeight(cur[lj * 4 + li], lc, float2(2 * li - 3, 2 * lj - 3));
                after += w[lj * 4 + li];
            }

        [unroll] for (int ls = 0; ls < 16; ++ls)
            w[ls] *= before / after;
    }

    // The candidates for where to search: no motion, the coarser level's answer at the cells around this pixel (doubled, it is
    // in this level's pixels) and the last frame's flow here. The one that matches
    // best is where the search starts, so a steady pan carries over from frame to frame and an edge is not stuck with the
    // answer of a cell that lies across it.
    int2 centre = 0;
    float start = Cost(p, centre, w, cur);

    // How long the picture has been unchanged here (finest level only): the age last frame, and from this frame's
    // difference at no offset the age now, written at the end. Where it has been still long enough, the camera below has
    // to beat no motion by a margin instead of winning the tie.
    uint lastAge = 0;
    float largest = 0.0;

    if (stillFrames != 0)
    {
        largest = LargestChange(p, cur);
        lastAge = hasAge != 0 ? LastAge.Load(int3(p, 0)) : 0;
    }

    const bool pictureStill = stillFrames != 0 && lastAge >= stillFrames;

    // What the whole picture did last frame (the camera): where nothing in the window says otherwise it wins the tie with no
    // motion, so a flat wall moves with the picture. `scale` here is this level's pixels per full-resolution pixel.
    if (hasGlobal != 0)
    {
        float4 g = GlobalFlow.Load(int3(0, 0, 0));

        if (g.z > 0.5)
        {
            int2 d = int2(round(g.xy * scale));
            float c = Cost(p, d, w, cur);

            if (pictureStill ? c + stillMargin < start : c <= start)
            {
                start = c;
                centre = d;
            }
        }
    }

    if (hasPrediction != 0)
    {
        int2 coarseHi = int2(aux) - 1;
        int2 cp = min(p >> 1, coarseHi);
        int2 step = int2((p.x & 1) != 0 ? 1 : -1, (p.y & 1) != 0 ? 1 : -1);

        // Nearest first: the four a bilinear read would use, then the rest of the 3x3, the far side last.
        static const int2 kCells[9] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1), int2(-1, 0),
                                        int2(0, -1), int2(-1, 1), int2(1, -1), int2(-1, -1) };

        [loop] for (int k = 0; k < (int) coarseCells; ++k)
        {
            int2 cell = cp + kCells[k] * step;
            int2 d = int2(round(Prediction.Load(int3(clamp(cell, 0, coarseHi), 0)).xy * 2.0));
            float c = Cost(p, d, w, cur);

            if (c < start)
            {
                start = c;
                centre = d;
            }
        }
    }

    if (hasHistory != 0)
    {
        int2 d = int2(round(History.Load(int3(p, 0)).xy));
        float c = Cost(p, d, w, cur);

        if (c < start)
        {
            start = c;
            centre = d;
        }
    }

    float best = 1e30;
    float bestCost = 0.0;
    int2 bestD = centre;

    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
        {
            int2 d = centre + int2(dx, dy);
            float cost = Cost(p, d, w, cur);
            float c = cost + lambda * length(float2(dx, dy));

            if (c < best)
            {
                best = c;
                bestCost = cost;
                bestD = d;
            }
        }

    // Zero wins a near tie on the finest level: in flat or grainy ground the best match is a small offset picked on noise, and
    // the picture did not move there. Only an offset within zeroReach is overruled (a real pan is far from it).
    if (zeroMargin > 0.0 && any(bestD != 0) && (zeroReach == 0 || max(abs(bestD.x), abs(bestD.y)) <= (int) zeroReach))
        if (Cost(p, int2(0, 0), w, cur) <= bestCost + zeroMargin)
            bestD = int2(0, 0);

    // The part of a pixel: a few Lucas-Kanade steps. With the previous frame sampled at the matched offset, the remaining
    // difference is explained by the picture's gradient there; solve that for the shift. A match that is already exact has no
    // difference left and is not moved, which a fit through the costs either side cannot promise. The difference's mean over
    // the window is taken out first, so a brightness change does not read as a shift (the gradients are not centred).
    float2 sub = 0.0;
    float confidence = 0.0;

    if (inverseRefinement != 0)
    {
        // The same steps with the gradients taken from the current frame at the window's own pixels instead of from the
        // previous frame at the shifted position: they do not move with the shift, so they and everything built from them
        // are found once, and each step then needs a single filtered read per sample, not five. It stops once a step is
        // under a hundredth of a pixel.
        float a = 0.0, b = 0.0, c = 0.0, sx = 0.0, sy = 0.0, sw = 0.0;
        float gxs[16], gys[16];
        int2 hi = int2(size) - 1;

        [unroll] for (int j = 0; j < 4; ++j)
            [unroll] for (int i = 0; i < 4; ++i)
            {
                int2 q = clamp(p + int2(2 * i - 3, 2 * j - 3), 0, hi);
                float gx = 0.5 * (CurLuma.Load(int3(clamp(q + int2(1, 0), 0, hi), 0)) -
                                  CurLuma.Load(int3(clamp(q - int2(1, 0), 0, hi), 0)));
                float gy = 0.5 * (CurLuma.Load(int3(clamp(q + int2(0, 1), 0, hi), 0)) -
                                  CurLuma.Load(int3(clamp(q - int2(0, 1), 0, hi), 0)));
                float wk = w[j * 4 + i];

                gxs[j * 4 + i] = gx;
                gys[j * 4 + i] = gy;
                a += wk * gx * gx;
                b += wk * gx * gy;
                c += wk * gy * gy;
                sx += wk * gx;
                sy += wk * gy;
                sw += wk;
            }

        const float det = a * c - b * b;
        const float weak = 0.5 * ((a + c) - sqrt((a - c) * (a - c) + 4.0 * b * b));
        confidence = weak / (weak + knee);

        // A window with no structure across (flat, or one straight edge) gives no step: its reads are skipped. Written as the
        // negation so a NaN determinant runs the steps, as the break on `det <= 1e-9` it replaces did.
        [loop] for (int iteration = 0; iteration < 3 && !(det <= 1e-9); ++iteration)
        {
            float e = 0.0, f = 0.0, sr = 0.0;

            [unroll] for (int j = 0; j < 4; ++j)
                [unroll] for (int i = 0; i < 4; ++i)
                {
                    int2 q = clamp(p + int2(2 * i - 3, 2 * j - 3), 0, hi);
                    float r = cur[j * 4 + i] - Sample(float2(q + bestD) + sub);
                    float wk = w[j * 4 + i];

                    e += wk * gxs[j * 4 + i] * r;
                    f += wk * gys[j * 4 + i] * r;
                    sr += wk * r;
                }

            const float meanResidual = sr / sw;
            e -= sx * meanResidual;
            f -= sy * meanResidual;

            const float2 step = float2(c * e - b * f, a * f - b * e) / det;
            sub = clamp(sub + step, -1.5, 1.5);

            if (max(abs(step.x), abs(step.y)) < 0.01)
                break;
        }
    }
    else
    [loop] for (int iteration = 0; iteration < 3; ++iteration)
    {
        float a = 0.0, b = 0.0, c = 0.0, e = 0.0, f = 0.0;
        float sx = 0.0, sy = 0.0, sr = 0.0, sw = 0.0;

        [unroll] for (int j = -3; j <= 3; j += 2)
            [unroll] for (int i = -3; i <= 3; i += 2)
            {
                int2 q = clamp(p + int2(i, j), 0, int2(size) - 1);
                float2 at = float2(q + bestD) + sub;
                float centre = Sample(at);
                float gx = 0.5 * (Sample(at + float2(1, 0)) - Sample(at - float2(1, 0)));
                float gy = 0.5 * (Sample(at + float2(0, 1)) - Sample(at - float2(0, 1)));
                float r = CurLuma.Load(int3(q, 0)) - centre;
                float wk = w[((j + 3) / 2) * 4 + (i + 3) / 2];

                a += wk * gx * gx;
                b += wk * gx * gy;
                c += wk * gy * gy;
                e += wk * gx * r;
                f += wk * gy * r;
                sx += wk * gx;
                sy += wk * gy;
                sr += wk * r;
                sw += wk;
            }

        // Only the difference is taken about its mean: centring the gradients too fits one more unknown to sixteen samples,
        // which costs accuracy in grain.
        const float meanResidual = sr / sw;
        e -= sx * meanResidual;
        f -= sy * meanResidual;

        float det = a * c - b * b;

        if (det > 1e-9)
            sub = clamp(sub + float2(c * e - b * f, a * f - b * e) / det, -1.5, 1.5);

        // The weaker direction of the picture's structure in the window: a match along an edge, or in a flat area, is not sure
        // across it.
        float weak = 0.5 * ((a + c) - sqrt((a - c) * (a - c) + 4.0 * b * b));
        confidence = weak / (weak + knee);
    }

    OutFlow[id.xy] = float4(float2(bestD) + sub, confidence, 1.0);

    if (stillFrames != 0)
        OutAge[id.xy] = largest < stillEpsilon ? min(lastAge + 1, 255u) : 0u;
}

)HLSL" // ... and on in a third one
    R"HLSL(
// A 3x3 median of each component, which removes the odd wrong block, scaled to full-resolution pixels.
void Sort(inout float a, inout float b)
{
    float lo = min(a, b);
    b = max(a, b);
    a = lo;
}

float Median9(float v[9])
{
    // a sorting network for nine values, the middle one is the median
    Sort(v[0], v[1]); Sort(v[3], v[4]); Sort(v[6], v[7]);
    Sort(v[1], v[2]); Sort(v[4], v[5]); Sort(v[7], v[8]);
    Sort(v[0], v[1]); Sort(v[3], v[4]); Sort(v[6], v[7]);
    Sort(v[0], v[3]); Sort(v[3], v[6]); Sort(v[0], v[3]);
    Sort(v[1], v[4]); Sort(v[4], v[7]); Sort(v[1], v[4]);
    Sort(v[2], v[5]); Sort(v[5], v[8]); Sort(v[2], v[5]);
    Sort(v[2], v[4]); Sort(v[4], v[6]); Sort(v[2], v[4]);
    return v[4];
}


// What the whole frame did: the middle of the finished flow on a coarse grid, each answer counted by how sure its match was
// (each component on its own, so one moving thing does not drag it), skipping sky when there is depth. A flat picture's answers
// carry no weight, so a few things on it speak for the rest. Written to a 1x1 texture, z = 1 only when most of that weight
// agrees with the middle: then the picture moved as one (the camera); when it did not (one thing moving over a still
// scene), there is no whole-picture motion to offer. One group, a thread per grid sample: each finds its weight below its
// own value (ties by position), and the sample whose weight straddles half of the total is the middle.
groupshared float3 gSample[128]; // x, y, weight
groupshared float2 gMiddle;

[numthreads(128, 1, 1)]
void Global(uint3 id : SV_DispatchThreadID)
{
    // This thread's cell of a 16x8 grid, represented by its most structured answer of an 8x8 spread: a point sample would
    // mostly land on flat ground and miss the few things that say how the scene moved.
    uint t = id.x;
    float4 f = 0.0;
    float weight = -1.0;

    for (uint s = 0; s < 64; ++s)
    {
        float2 uv = float2((t % 16 + (s % 8 + 0.5) / 8.0) / 16.0, (t / 16 + (s / 8 + 0.5) / 8.0) / 8.0);
        float4 here = FlowIn.Load(int3(min(int2(uv * float2(aux)), int2(aux) - 1), 0));
        float w = here.z;

        if (depthMatching != 0)
        {
            float d = SceneDepth.Load(int3(min(int2(uv * float2(depthSize)), int2(depthSize) - 1), 0));

            if (reversed != 0 ? d <= 1e-6 : d >= 0.999999)
                w = 0.0;
        }

        if (w > weight)
        {
            weight = w;
            f = here;
        }
    }

    gSample[t] = float3(f.xy, weight);
    if (t == 0)
        gMiddle = 0.0; // in case rounding leaves no sample straddling the half
    GroupMemoryBarrierWithGroupSync();

    float total = 0.0;
    float2 below = 0.0;

    for (uint k = 0; k < 128; ++k)
    {
        float3 other = gSample[k];
        total += other.z;
        below.x += (other.x < f.x || (other.x == f.x && k < t)) ? other.z : 0.0;
        below.y += (other.y < f.y || (other.y == f.y && k < t)) ? other.z : 0.0;
    }

    float middle = 0.5 * total;

    if (below.x < middle && middle <= below.x + weight)
        gMiddle.x = f.x;

    if (below.y < middle && middle <= below.y + weight)
        gMiddle.y = f.y;

    GroupMemoryBarrierWithGroupSync();

    if (t == 0)
    {
        // The weight within a pixel and a half (and a tenth of the motion) of the middle.
        float agree = 0.0;
        float allowed = 1.5 + 0.1 * length(gMiddle);

        for (uint k = 0; k < 128; ++k)
            agree += length(gSample[k].xy - gMiddle) <= allowed ? gSample[k].z : 0.0;

        const bool camera = total >= 0.1 && agree >= 0.75 * total;
        OutFlow[uint2(0, 0)] = camera ? float4(gMiddle, 1.0, total) : float4(0.0, 0.0, 0.0, total);
    }
}

// Hue for the direction, brightness for the speed (scale is the speed that is full brightness).
[numthreads(8, 8, 1)]
void Visualise(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float2 f = FlowIn.Load(int3(id.xy, 0)).xy;
    float hue = atan2(f.y, f.x) / 6.2831853 + 0.5;
    float3 rgb = saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    float level = saturate(length(f) / scale);

    OutFlow[id.xy] = float4(rgb * level, 1.0);
}

[numthreads(8, 8, 1)]
void Median(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float xs[9], ys[9];
    int k = 0;
    int2 hi = int2(size) - 1;

    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
        {
            float2 f = FlowIn.Load(int3(clamp(int2(id.xy) + int2(i, j), 0, hi), 0)).xy;
            xs[k] = f.x;
            ys[k] = f.y;
            ++k;
        }

    OutFlow[id.xy] = float4(Median9(xs) * scale, Median9(ys) * scale, FlowIn.Load(int3(id.xy, 0)).z, 1.0);
}

// Edge-aware smoothing: the average of the neighbours' motion, each counted by how sure its match was, how close its motion
// is to this pixel's and how close its brightness is, so noise in a flat area is averaged out and the edge of something that
// moves differently is not. The group's 8x8 pixels and the radius (at most 4) around them are read once into a 16x16 tile that
// the group's threads share, with the picture's edge repeated outward.
groupshared float4 gTile[256]; // x, y and confidence of the flow, and the guide's luma

[numthreads(8, 8, 1)]
void Smooth(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    // Off: only the centre counts, so the tile is not filled (the radius is the same for the whole group, so every thread
    // leaves here or none does).
    if (radius == 0)
    {
        if (id.x < size.x && id.y < size.y)
        {
            // Rounded as the full pass rounds it (precise keeps the compiler from folding the weight away).
            float3 only = FlowIn.Load(int3(id.xy, 0)).xyz;
            precise float weight = only.z + 0.05;
            precise float2 weighted = only.xy * weight;
            OutFlow[id.xy] = float4(weighted / weight, only.z, 1.0);
        }

        return;
    }

    int2 hi = int2(size) - 1;
    int2 origin = int2(group.xy) * 8 - 4;
    uint t = local.y * 8 + local.x;

    // Every thread loads four of the tile's entries, including those outside the picture, which must still reach the barrier.
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        uint e = t + k * 64;
        int2 tile = int2(e % 16, e / 16);
        int2 q = clamp(origin + tile, 0, hi);
        gTile[e] = float4(FlowIn.Load(int3(q, 0)).xyz, GuideLuma.Load(int3(q, 0)));
    }

    GroupMemoryBarrierWithGroupSync();

    if (id.x >= size.x || id.y >= size.y)
        return;

    int2 at = int2(local.xy) + 4;
    float4 here = gTile[at.y * 16 + at.x];
    float3 centre = here.xyz;
    float lumaCentre = here.w;

    float2 sum = centre.xy * (centre.z + 0.05);
    float total = centre.z + 0.05;
    float motionRange = 1.0 + 0.02 * dot(centre.xy, centre.xy);
    float lumaRange = 0.15 * (lumaCentre + 0.05);

    [unroll] for (int j = -4; j <= 4; ++j)
        [unroll] for (int i = -4; i <= 4; ++i)
        {
            if ((i == 0 && j == 0) || abs(i) > radius || abs(j) > radius)
                continue;

            int2 q = at + int2(i, j);
            float4 other = gTile[q.y * 16 + q.x];
            float2 df = other.xy - centre.xy;
            float dl = (other.w - lumaCentre) / lumaRange;
            float w = (other.z + 0.05) * exp(-dot(df, df) / motionRange - dl * dl);
            sum += other.xy * w;
            total += w;
        }

    OutFlow[id.xy] = float4(sum / total, centre.z, 1.0);
}
)HLSL";
} // namespace OpticalFlowHlsl
