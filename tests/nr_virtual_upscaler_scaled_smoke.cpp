#include "pch.h"
// cl /std:c++20 /EHsc /W4 /O2 /I tests\virtual_upscaler_stubs /I external\nvngx_dlss_sdk
// tests\nr_virtual_upscaler_scaled_smoke.cpp
#include "../OptiScaler/native/VirtualUpscalerParams.h"
#include "../OptiScaler/native/VirtualUpscalerHold.h"
#include "d3d12_harness.h"

using namespace native::vparams;
namespace
{
struct Entry
{
    std::string name;
    char type;
    double value;
    bool operator==(const Entry&) const = default;
};
struct Recorder
{
    std::vector<Entry> entries;
    void Set(const char* n, unsigned v) { entries.push_back({ n, 'u', double(v) }); }
    void Set(const char* n, int v) { entries.push_back({ n, 'i', double(v) }); }
    void Set(const char* n, float v) { entries.push_back({ n, 'f', double(v) }); }
};
struct Key
{
    unsigned size = 0;
    bool reversed = false;
    bool operator==(const Key&) const = default;
};
} // namespace
int main()
{
    d3d12_test::Checks c;
    Recorder p;
    Shape shape { 640, 360, 640, 360, false, true, false };
    SetCreate(p, shape);
    const std::vector<Entry> originalCreate { { NVSDK_NGX_Parameter_Width, 'u', 640 },
                                              { NVSDK_NGX_Parameter_Height, 'u', 360 },
                                              { NVSDK_NGX_Parameter_OutWidth, 'u', 640 },
                                              { NVSDK_NGX_Parameter_OutHeight, 'u', 360 },
                                              { NVSDK_NGX_Parameter_PerfQualityValue, 'i', 1 },
                                              { NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 'u',
                                                NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                                                    NVSDK_NGX_DLSS_Feature_Flags_DepthInverted } };
    c.Check(p.entries == originalCreate, "Run creation names, values, overloads and order unchanged");
    p.entries.clear();
    SetFrame(p, false);
    const std::vector<Entry> originalFrame { { NVSDK_NGX_Parameter_Jitter_Offset_X, 'f', 0 },
                                             { NVSDK_NGX_Parameter_Jitter_Offset_Y, 'f', 0 },
                                             { NVSDK_NGX_Parameter_MV_Scale_X, 'f', 1 },
                                             { NVSDK_NGX_Parameter_MV_Scale_Y, 'f', 1 },
                                             { NVSDK_NGX_Parameter_Reset, 'u', 0 },
                                             { NVSDK_NGX_Parameter_Sharpness, 'f', 0 },
                                             { NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 'f', 1 } };
    c.Check(p.entries == originalFrame, "Run frame names, values, overloads and order unchanged");
    shape.outWidth = 960;
    shape.outHeight = 540;
    shape.lowResMV = true;
    shape.hdr = true;
    p.entries.clear();
    SetCreate(p, shape);
    c.Check(p.entries[0].value == 640 && p.entries[2].value == 960 && p.entries[3].value == 540,
            "render and output dimensions are separate");
    const unsigned flags = CreateFlags(shape);
    c.Check((flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) && (flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) &&
                !(flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered),
            "MVLowRes at creation; unjittered render motion");
    p.entries.clear();
    SetScaledFrame(p, { 1, 2, 638, 358 }, true);
    c.Check(p.entries[2].value == 1 && p.entries[3].value == 1 && p.entries[4].value == 1,
            "render-pixel MV scale is 1 and reset is passed");
    c.Check(p.entries[7].value == 638 && p.entries[8].value == 358 && p.entries[9].value == 1 &&
                p.entries[10].value == 2 && p.entries[11].value == 1 && p.entries[12].value == 2 &&
                p.entries[13].value == 1 && p.entries[14].value == 2,
            "subrect dimensions and color/depth/motion bases agree");

    ScaledInputs in { 640, 360, 640, 360, 640, 360, 960, 540 };
    Subrect rect;
    c.Check(CheckScaled(in, rect) == nullptr && rect == Subrect { 0, 0, 640, 360 }, "zero rect means whole texture");
    in.right = 639;
    in.bottom = 358;
    c.Check(CheckScaled(in, rect) == nullptr && rect == Subrect { 0, 0, 639, 358 },
            "padded texture retains its real dimensions");
    in.left = 1;
    c.Check(CheckScaled(in, rect) == nullptr && rect.x == 1 && rect.width == 638, "nonzero base is represented");
    auto bad = in;
    bad.left = -1;
    c.Check(CheckScaled(bad, rect) != nullptr, "negative subrect refused");
    bad = in;
    bad.right = 641;
    c.Check(CheckScaled(bad, rect) != nullptr, "out-of-bounds subrect refused");
    bad = in;
    bad.right = bad.left;
    c.Check(CheckScaled(bad, rect) != nullptr, "empty subrect refused");
    bad = in;
    bad.right = 19;
    c.Check(CheckScaled(bad, rect) != nullptr, "too-small subrect refused");
    bad = in;
    bad.motionWidth = 637;
    c.Check(CheckScaled(bad, rect) != nullptr, "motion must cover subrect");
    bad = in;
    bad.depthHeight = 357;
    c.Check(CheckScaled(bad, rect) != nullptr, "depth must cover subrect");
    bad = in;
    bad.motionWidth = 960;
    c.Check(CheckScaled(bad, rect) != nullptr, "display-sized motion refused");
    bad = in;
    bad.depthWidth = 960;
    c.Check(CheckScaled(bad, rect) != nullptr, "display-sized depth refused");
    bad = in;
    bad.outputWidth = 600;
    c.Check(CheckScaled(bad, rect) != nullptr, "texture larger than output refused");
    bad = in;
    bad.colorWidth = kMaxSide + 1;
    c.Check(CheckScaled(bad, rect) != nullptr, "oversized texture refused");
    bad = in;
    bad.outputHeight = kMinSide - 1;
    c.Check(CheckScaled(bad, rect) != nullptr, "invalid output size refused");
    in.depthWidth = in.depthHeight = 0;
    c.Check(CheckScaled(in, rect) == nullptr, "absent depth uses neutral depth");

    const auto run = PlanNr(false, false, true, true);
    c.Check(!run.copyIn && run.before && run.after, "present-path NR calls unchanged");
    const auto off = PlanNr(false, true, true, false);
    c.Check(!off.copyIn && !off.before && !off.after, "in-list allowNr=false forbids both NR calls and forced copy");
    const auto typed = PlanNr(true, true, true, false);
    c.Check(typed.copyIn && !typed.before && !typed.after, "typed input copy does not enable NR");
    const auto nr = PlanNr(false, true, true, true);
    c.Check(nr.copyIn && nr.before && nr.after, "allowed scaled NR only reads a private copy");
    const auto noNr = PlanNr(false, true, false, true);
    c.Check(!noNr.copyIn && !noNr.before && noNr.after, "NR switched off never calls before on game color");
    c.Check(ScaledOutputFormat(DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                ScaledOutputFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM) ==
                    DXGI_FORMAT_B8G8R8A8_UNORM,
            "output format independent of input; sRGB becomes UAV-compatible UNORM");
    c.Check(CheckFrameGeneration(true, true, true) != nullptr && CheckFrameGeneration(true, true, false) == nullptr &&
                CheckFrameGeneration(false, true, true) == nullptr,
            "XeFG display-MV swapchain refused only for upscaler input");
    OutputOwner owner;
    owner.ScaledTextureReady();
    c.Check(owner.Scaled(), "scaled output ownership");
    owner.EnterRun();
    c.Check(!owner.Scaled(), "Run clears ownership even if it returns early");

    native::hold::RebuildSettle<Key> settle;
    Key old { 640, false }, changed { 640, true };
    bool prediction = true;
    for (unsigned i = 0; i < 30; ++i)
    {
        const bool expected = settle.WouldNow(old, changed, true, false);
        prediction &= expected == settle.Now(old, changed, true, false);
    }
    c.Check(prediction, "WouldRebuild settling prediction matches build decision");
    c.Check(settle.WouldNow(old, { 960, false }, false, false), "size change needs build immediately");
    c.Check(settle.WouldNow(old, changed, true, true), "first real depth corrects guessed direction immediately");
    return c.Summary();
}
