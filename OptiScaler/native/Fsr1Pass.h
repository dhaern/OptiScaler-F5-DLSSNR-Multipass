#pragma once
#include "Fsr1Signature.h"
#include "DescriptorTrackerDx12.h"
#include <cmath>
#include <cstring>
#include <iterator>

namespace native::fsr1
{
using View = descriptors::View;
enum class Shape : uint8_t
{
    Draw,
    DrawIndexed,
    Dispatch
};
enum class Refusal : uint8_t
{
    None,
    NoTls,
    GameUpscaler,
    Settings,
    Indirect,
    Bundle,
    NotDirect,
    Predicated,
    Blend,
    WriteMask,
    RenderTargets,
    DepthWrite,
    InputUnresolved,
    InputState,
    EnhancedBarriers,
    AfterBundle,
    OutputUnresolved,
    Bindless,
    RectUnknown,
    Ratio,
    Offset,
    InputShape,
    OutputShape,
    ViewFormat,
    Srgb,
    Viewport,
    Scissor,
    UberRcas,
    Incomplete,
    DrawShape,
    DispatchGrid,
    Constants,
    Pipeline,
    Ambiguous,
    RenderPass,
    Count
};
inline const char* Name(Refusal r)
{
    constexpr const char* names[] { "none",
                                    "no-tls",
                                    "game-upscaler",
                                    "settings",
                                    "indirect",
                                    "bundle",
                                    "not-direct",
                                    "predicated",
                                    "blend",
                                    "write-mask",
                                    "render-targets",
                                    "depth-write",
                                    "input-unresolved",
                                    "input-state",
                                    "enhanced-barriers",
                                    "after-bundle",
                                    "output-unresolved",
                                    "bindless",
                                    "rect-unknown",
                                    "ratio",
                                    "offset",
                                    "input-shape",
                                    "output-shape",
                                    "view-format",
                                    "srgb",
                                    "viewport",
                                    "scissor",
                                    "uber-rcas",
                                    "incomplete",
                                    "draw-shape",
                                    "dispatch-grid",
                                    "constants",
                                    "pipeline",
                                    "bindings-ambiguous",
                                    "render pass" };
    static_assert(std::size(names) == size_t(Refusal::Count));
    return r < Refusal::Count ? names[size_t(r)] : "unknown";
}
struct Facts
{
    Shape shape = Shape::Draw;
    ShaderInfo shader {};
    bool noTls = false, gameUpscaler = false, overlayMenu = true, outputScaling = false, magnifier = false;
    bool indirect = false, tracked = true, afterBundle = false, predicated = false, blend = false;
    D3D12_COMMAND_LIST_TYPE listType = D3D12_COMMAND_LIST_TYPE_DIRECT;
    uint8_t writeMask = 0xF;
    UINT renderTargets = 1, boundTargets = 1;
    bool depthStencilWrite = false, bindless = false, backendReadsBase = false;
    bool inRenderPass = false;
    bool ambiguous = false; // bindings inferred without a layout matched more than one constant block or input
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
    View input {}, output {};
    D3D12_RESOURCE_DESC inputDesc {}, outputDesc {};
    bool inputStateKnown = false, enhanced = false, pendingTransition = false;
    D3D12_RESOURCE_STATES inputState = D3D12_RESOURCE_STATE_COMMON;
    UINT count = 0, instances = 1, first = 0, firstInstance = 0;
    INT baseVertex = 0;
    UINT groups[3] {};
    bool viewportValid = false, scissorValid = false;
    D3D12_VIEWPORT viewport {};
    D3D12_RECT scissor {};
    uint32_t constants[16] {};
    UINT constantCount = 0;
};
struct Plan
{
    Shape shape {};
    View input {}, output {};
    D3D12_RESOURCE_DESC inputDesc {}, outputDesc {};
    D3D12_RECT inputRect {};
    D3D12_BOX box {};
    D3D12_RESOURCE_STATES inputState = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
    float ratio = 0;
    bool constantsUsed = false;
};
inline bool IsSrgb(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}
inline DXGI_FORMAT FormatForOutput(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return format;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
inline DXGI_FORMAT Family(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
namespace pass_detail
{
inline bool Whole(float value, LONG* out)
{
    const float rounded = std::floor(value + 0.5f);
    if (!(std::fabs(value - rounded) <= 0.01f) || rounded < 0 || rounded > 65536)
        return false;
    *out = LONG(rounded);
    return true;
}
inline bool Texture(const View& v, const D3D12_RESOURCE_DESC& d)
{
    return v.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           d.MipLevels == 1 && d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 && v.mip == 0 && v.firstSlice == 0 &&
           v.sliceCount == 1 && v.plane == 0 && d.Width > 0 && d.Width <= 16384 && d.Height > 0 && d.Height <= 16384 &&
           !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
}
} // namespace pass_detail
inline Refusal Resolve(const Facts& f, Plan* out)
{
    using namespace pass_detail;
    const bool compute = f.shape == Shape::Dispatch;
    if (f.noTls)
        return Refusal::NoTls;
    if (f.gameUpscaler)
        return Refusal::GameUpscaler;
    if (!f.overlayMenu || f.outputScaling || f.magnifier)
        return Refusal::Settings;
    if (f.indirect)
        return Refusal::Indirect;
    if (f.afterBundle)
        return Refusal::AfterBundle;
    if (f.inRenderPass)
        return Refusal::RenderPass;
    if (!f.tracked)
        return Refusal::Incomplete;
    if (f.listType == D3D12_COMMAND_LIST_TYPE_BUNDLE)
        return Refusal::Bundle;
    if (f.listType != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return Refusal::NotDirect;
    if (f.predicated)
        return Refusal::Predicated;
    if (f.shader.pass != Pass::Easu || f.shader.stage != (compute ? Stage::Compute : Stage::Pixel))
        return Refusal::Pipeline;
    if (!compute)
    {
        if (f.blend)
            return Refusal::Blend;
        if (f.writeMask != 0xF)
            return Refusal::WriteMask;
        if (f.renderTargets != 1 || f.boundTargets != 1)
            return Refusal::RenderTargets;
        if (f.depthStencilWrite)
            return Refusal::DepthWrite;
        if (f.instances != 1 || f.first || f.firstInstance || f.baseVertex ||
            (f.shape == Shape::Draw ? f.count != 3 && f.count != 4 : f.count != 3 && f.count != 6))
            return Refusal::DrawShape;
    }
    if (f.bindless || f.shader.textures != 1 || (compute && f.shader.uavs != 1))
        return Refusal::Bindless;
    if (f.ambiguous)
        return Refusal::Ambiguous;
    if (!f.input.resource || f.input.kind != uint8_t(descriptors::Kind::Srv))
        return Refusal::InputUnresolved;
    if (!f.output.resource || f.output.kind != uint8_t(compute ? descriptors::Kind::Uav : descriptors::Kind::Rtv))
        return Refusal::OutputUnresolved;
    LONG outW = LONG(f.outputDesc.Width), outH = LONG(f.outputDesc.Height);
    const auto& vp = f.viewport;
    if (!compute && f.viewportValid && vp.TopLeftX == 0 && vp.TopLeftY == 0)
    {
        LONG w = 0, h = 0;
        if (Whole(vp.Width, &w) && Whole(vp.Height, &h) && w > 0 && h > 0 && w <= outW && h <= outH)
        {
            outW = w;
            outH = h;
        }
    }
    LONG left = 0, top = 0, width = LONG(f.inputDesc.Width), height = LONG(f.inputDesc.Height);
    bool used = false;
    EasuConstants con;
    if (!f.shader.uber && f.constantCount >= 16 && DecodeEasu(f.constants, UINT(outW), UINT(outH), &con))
    {
        used = true;
        if (con.inputWidth != f.inputDesc.Width || con.inputHeight != f.inputDesc.Height ||
            !Whole(con.offsetX, &left) || !Whole(con.offsetY, &top) || !Whole(con.viewportWidth, &width) ||
            !Whole(con.viewportHeight, &height) || width <= 0 || height <= 0 ||
            left + width > LONG(f.inputDesc.Width) || top + height > LONG(f.inputDesc.Height))
            return Refusal::Constants;
    }
    else if (f.shader.uber && f.constantCount >= 4)
    {
        float sizes[4];
        std::memcpy(sizes, f.constants, sizeof(sizes));
        LONG iw = 0, ih = 0, ow = 0, oh = 0;
        if (!Whole(sizes[0], &iw) || !Whole(sizes[1], &ih) || !Whole(sizes[2], &ow) || !Whole(sizes[3], &oh) ||
            iw != width || ih != height || ow != outW || oh != outH)
            return Refusal::Constants;
        used = true;
    }
    if (f.shader.uber && width == outW && height == outH)
        return Refusal::UberRcas;
    if (!used && (width <= 0 || height <= 0 || outW <= width || outH <= height ||
                  std::fabs(double(outW) * height / width - outH) > 1.0))
        return Refusal::RectUnknown;
    if (width <= 0 || height <= 0 || outW <= width || outH <= height || outW > 3 * width || outH > 3 * height)
        return Refusal::Ratio;
    if ((left || top) && !f.backendReadsBase)
        return Refusal::Offset;
    if (!Texture(f.input, f.inputDesc))
        return Refusal::InputShape;
    if (f.enhanced)
        return Refusal::EnhancedBarriers;
    const auto readable =
        compute ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto writes = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                            D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (!f.inputStateKnown || f.pendingTransition || !(f.inputState & readable) || (f.inputState & writes))
        return Refusal::InputState;
    if (!Texture(f.output, f.outputDesc) || outW <= 0 || outH <= 0 || outW > LONG(f.outputDesc.Width) ||
        outH > LONG(f.outputDesc.Height) ||
        !(f.outputDesc.Flags &
          (compute ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)))
        return Refusal::OutputShape;
    const auto format = FormatForOutput(f.output.format);
    if (format == DXGI_FORMAT_UNKNOWN || FormatForOutput(f.input.format) == DXGI_FORMAT_UNKNOWN ||
        Family(f.inputDesc.Format) != Family(f.input.format) ||
        Family(f.outputDesc.Format) != Family(f.output.format) || (!compute && f.output.format != f.rtvFormat))
        return Refusal::ViewFormat;
    if (IsSrgb(f.input.format) != IsSrgb(f.output.format))
        return Refusal::Srgb;
    if (!compute)
    {
        LONG w = 0, h = 0;
        if (!f.viewportValid || vp.TopLeftX != 0 || vp.TopLeftY != 0 || !Whole(vp.Width, &w) || !Whole(vp.Height, &h) ||
            w != outW || h != outH)
            return Refusal::Viewport;
        if (!f.scissorValid || f.scissor.left > 0 || f.scissor.top > 0 || f.scissor.right < outW ||
            f.scissor.bottom < outH)
            return Refusal::Scissor;
    }
    else
    {
        const auto* n = f.shader.numthreads;
        if (!n[0] || !n[1] || n[2] != 1 || f.groups[2] != 1 || !f.groups[0] || !f.groups[1] ||
            uint64_t(f.groups[0]) * n[0] < UINT(outW) || uint64_t(f.groups[1]) * n[1] < UINT(outH) ||
            uint64_t(f.groups[0] - 1) * n[0] >= UINT(outW) || uint64_t(f.groups[1] - 1) * n[1] >= UINT(outH))
            return Refusal::DispatchGrid;
    }
    if (out)
        *out = { f.shape,
                 f.input,
                 f.output,
                 f.inputDesc,
                 f.outputDesc,
                 { left, top, left + width, top + height },
                 { 0, 0, 0, UINT(outW), UINT(outH), 1 },
                 f.inputState,
                 format,
                 float(outW) / float(width),
                 used };
    return Refusal::None;
}
} // namespace native::fsr1
