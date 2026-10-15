#pragma once

#include <nvsdk_ngx_defs.h>

#include <dxgiformat.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace native::vparams
{
inline constexpr uint32_t kMinSide = 20;
inline constexpr uint32_t kMaxSide = 16384;

struct Shape
{
    uint32_t width = 0, height = 0;       // render
    uint32_t outWidth = 0, outHeight = 0; // output
    bool lowResMV = false;
    bool depthReversed = false;
    bool hdr = false;
    bool fixedExposure = false;
};

// MVLowRes is a creation flag; render-pixel motion uses MV_Scale=1, not the upscale ratio.
inline unsigned int CreateFlags(const Shape& s)
{
    unsigned int flags = s.fixedExposure ? 0u : NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    if (s.depthReversed)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

    if (s.hdr)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;

    if (s.lowResMV)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

    return flags;
}

template <class P> void SetCreate(P& p, const Shape& s)
{
    p.Set(NVSDK_NGX_Parameter_Width, s.width);
    p.Set(NVSDK_NGX_Parameter_Height, s.height);
    p.Set(NVSDK_NGX_Parameter_OutWidth, s.outWidth);
    p.Set(NVSDK_NGX_Parameter_OutHeight, s.outHeight);
    p.Set(NVSDK_NGX_Parameter_PerfQualityValue, 1);
    p.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, CreateFlags(s));
}

struct Subrect
{
    uint32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;

    bool operator==(const Subrect&) const = default;
};

template <class P> void SetFrame(P& p, bool reset)
{
    p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    p.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    p.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    p.Set(NVSDK_NGX_Parameter_Reset, reset ? 1u : 0u);
    p.Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    p.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
}

// The sub-pixel offset the scene of the frame being upscaled was drawn with, in render pixels (two floats; 0 = none)
inline constinit std::atomic<uint64_t> g_scaledJitter {};

template <class P> void SetScaledFrame(P& p, const Subrect& r, bool reset)
{
    SetFrame(p, reset);
    if (const uint64_t bits = g_scaledJitter.load(std::memory_order_relaxed))
    {
        float jitter[2];
        std::memcpy(jitter, &bits, sizeof(jitter));
        p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitter[0]);
        p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitter[1]);
    }
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, r.width);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, r.height);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, r.x);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, r.y);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, r.x);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, r.y);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, r.x);
    p.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, r.y);
}

struct ScaledInputs
{
    uint32_t colorWidth = 0, colorHeight = 0;   // frame.color's texture
    uint32_t motionWidth = 0, motionHeight = 0; // frame.motion's
    uint32_t depthWidth = 0, depthHeight = 0;   // frame.depth's; 0 when the frame has none
    uint32_t outputWidth = 0, outputHeight = 0;
    int32_t left = 0, top = 0, right = 0, bottom = 0; // the render subrect; all zero: the whole texture
};

inline const char* CheckScaled(const ScaledInputs& in, Subrect& rect)
{
    rect = {};

    if (in.colorWidth < kMinSide || in.colorHeight < kMinSide || in.colorWidth > kMaxSide || in.colorHeight > kMaxSide)
        return "the picture's size is out of range";

    if (in.outputWidth < kMinSide || in.outputHeight < kMinSide || in.outputWidth > kMaxSide ||
        in.outputHeight > kMaxSide)
        return "the output's size is out of range";

    if (in.colorWidth > in.outputWidth || in.colorHeight > in.outputHeight)
        return "the picture is larger than the output";

    if (in.left == 0 && in.top == 0 && in.right == 0 && in.bottom == 0)
    {
        rect.width = in.colorWidth;
        rect.height = in.colorHeight;
    }
    else
    {
        if (in.left < 0 || in.top < 0 || in.right <= in.left || in.bottom <= in.top)
            return "the render subrect is empty";

        if ((uint32_t) in.right > in.colorWidth || (uint32_t) in.bottom > in.colorHeight)
            return "the render subrect is outside the picture";

        rect.x = (uint32_t) in.left;
        rect.y = (uint32_t) in.top;
        rect.width = (uint32_t) (in.right - in.left);
        rect.height = (uint32_t) (in.bottom - in.top);
    }

    if (rect.width < kMinSide || rect.height < kMinSide)
        return "the render subrect is too small";

    if (in.motionWidth < rect.x + rect.width || in.motionHeight < rect.y + rect.height)
        return "the motion does not cover the render subrect";

    if (in.depthWidth != 0 && (in.depthWidth < rect.x + rect.width || in.depthHeight < rect.y + rect.height))
        return "the depth does not cover the render subrect";

    if (in.motionWidth > in.colorWidth || in.motionHeight > in.colorHeight)
        return "the motion is larger than the picture: it must be render-sized";

    if (in.depthWidth > in.colorWidth || in.depthHeight > in.colorHeight)
        return "the depth is larger than the picture: it must be render-sized";

    return nullptr;
}

inline bool CopiesPictureIn(bool formatDiffers, bool scaled, bool dlssNrOn)
{
    return formatDiffers || (scaled && dlssNrOn);
}

struct NrPlan
{
    bool copyIn = false; // the backend reads a typed copy of frame.color (CopiesPictureIn)
    bool before = false; // DlssNr::EvaluateBeforeUpscale is called
    bool after = false;  // DlssNr::EvaluateAfterUpscale is called, once the backend evaluated
};

inline NrPlan PlanNr(bool formatDiffers, bool scaled, bool dlssNrOn, bool allowNr)
{
    NrPlan plan;
    plan.copyIn = CopiesPictureIn(formatDiffers, scaled, dlssNrOn && allowNr);
    plan.before = allowNr && (!scaled || plan.copyIn);
    plan.after = allowNr;
    return plan;
}

inline NrPlan PlanNr(bool formatDiffers, bool scaled, bool dlssNrOn, bool allowNr, bool typedView)
{
    return PlanNr(formatDiffers && !(typedView && scaled && !allowNr), scaled, dlssNrOn, allowNr);
}

// UAVs cannot be sRGB; copies preserve the original bytes in the UNORM twin.
inline DXGI_FORMAT UavFormat(DXGI_FORMAT view)
{
    switch (view)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    default:
        return view;
    }
}

inline bool SdrFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8X8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_UNORM;
}

inline DXGI_FORMAT ScaledOutputFormat(DXGI_FORMAT asked, DXGI_FORMAT pictureFormat)
{
    return asked != DXGI_FORMAT_UNKNOWN ? UavFormat(asked) : pictureFormat;
}

inline const char* CheckFrameGeneration(bool xefg, bool upscalerInput, bool fgDisplaySizedMotion)
{
    if (xefg && upscalerInput && fgDisplaySizedMotion)
        return "XeFG's swapchain was made for display-sized motion ([XeFG] HighResMV): it must be made again for "
               "render-sized motion";

    return nullptr;
}

class OutputOwner
{
  public:
    void EnterRun() { _scaled = false; }
    void ScaledTextureReady() { _scaled = true; }
    bool Scaled() const { return _scaled; }

  private:
    bool _scaled = false;
};
} // namespace native::vparams
