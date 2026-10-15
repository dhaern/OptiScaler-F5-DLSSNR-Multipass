#include "pch.h"

#include "NativeDriver.h"
#include "VirtualUpscalerDriver.h"

#include <Config.h>
#include <dlssnr/DlssNrFeature_Dx12.h>

#include <imgui/imgui.h>

namespace native
{

namespace
{
constexpr float kPreviewMaxSpeed = 24.0f; // pixels per frame that show as full brightness
} // namespace

void NativeDriver::ReleaseVirtualUpscaler()
{
    if (_virtualUpscaler != nullptr)
        _virtualUpscaler->Release();
}

void NativeDriver::PauseVirtualUpscaler()
{
    if (_virtualUpscaler != nullptr)
        _virtualUpscaler->Pause();
}

roles::Roles NativeDriver::DecideRoles() const
{
    const auto& cfg = *Config::Instance();

    roles::Keys keys;
    keys.nativeMotion = cfg.DlssNrNativeMotion.value_or_default();
    keys.nativeInput = cfg.DlssNrNativeInput.value_or_default();
    keys.nativeUpscaler = cfg.DlssNrNativeUpscaler.value_or_default();
    keys.replaceGameFsr1 = cfg.ReplaceGameFsr1.value_or_default();
    keys.inListApi = _inListApi;
    return roles::Decide(keys);
}

bool NativeDriver::Enabled()
{
    if (!DecideRoles().enabled)
    {
        ReleaseVirtualUpscaler();
        _status = Status::Off;
        return false;
    }

    return true;
}

void NativeDriver::LogDiagnostics()
{
    if (_diagPresents % 300 != 0)
        return;

    LOG_INFO("{} frames: of {} presents, picture ready {}, waiting for the game's upscaler {}, "
             "submitted {}, flow valid {} (matched with depth {}), trust mask ran {}, native input applied {}, scene "
             "cuts {}, virtual upscaler {}",
             _tag, _diagPresents, _diagReady, _diagWaiting, _diagSubmitted, _diagFlowValid, _diagDepth, _diagTrust,
             _diagNative, _cuts, _virtualUpscaler != nullptr && _virtualUpscaler->Active() ? "active" : "not active");
}

bool NativeDriver::EnsureProducer(ID3D12Device* device)
{
    if (_producer != nullptr && _producer->Device() == device)
        return true;

    auto fresh = std::make_unique<NativeProducer>();

    const double initStart = Util::MillisecondsNow();

    if (!fresh->Init(device))
    {
        _failure = fresh->Error();
        _status = Status::Failed;
        LOG_ERROR("{}: {}", _tag, _failure);
        return false;
    }

    // The upscaler backend belongs to the old device too.
    if (_virtualUpscaler != nullptr)
    {
        delete _virtualUpscaler;
        _virtualUpscaler = nullptr;
    }

    _producer = std::move(fresh);
    LOG_INFO("{}: optical flow and trust mask ready{} (in {:.1f} ms, on the present thread)", _tag, _readyNote,
             Util::MillisecondsNow() - initStart);
    return true;
}

NativeDriver::RunResult NativeDriver::RunFrame(IFrameSource& source, bool flowPreview, bool runPresentPath,
                                               bool keepDevice)
{
    RunResult out;

    const roles::Roles roles = DecideRoles();

    // Work recorded into a game's list may still run on the producer and the upscaler of the last device
    const bool keep = keepDevice && _producer != nullptr && _producer->Device() != source.Device();

    // The in-list pass alone, or recording now: the producer for this device and the virtual upscaler are made here,
    // on the present thread, and nothing else is done (no picture is read, nothing runs on it, nothing reaches
    // DLSS-NR). The present path's status is Off, as when NativeMotion is switched off.
    if (roles.inListOnly || !runPresentPath)
    {
        _status = Status::Off;
        _nativeRan = false;

        if ((keep || EnsureProducer(source.Device())) && _virtualUpscaler == nullptr)
            _virtualUpscaler = new VirtualUpscalerDriver();

        return out;
    }

    FrameInput input;
    const auto acquired = source.Acquire(input);

    ++_diagPresents;
    _diagReady += acquired == AcquireStatus::Ready ? 1 : 0;
    _diagWaiting += acquired == AcquireStatus::WaitingForUpscaler ? 1 : 0;

    const bool gameTookOver = _takeover.Update(acquired == AcquireStatus::WaitingForUpscaler, Util::MillisecondsNow());

    if (acquired == AcquireStatus::WaitingForUpscaler)
    {
        // The game's own upscaler call: ours pauses, and goes (with frame generation's context, as a game's feature
        // would) only once the game's calls have run for a while. A menu or cutscene that calls it for a moment would
        // otherwise cost a rebuild and a gap in frame generation each time (VirtualUpscalerHold.h).
        if (gameTookOver)
            ReleaseVirtualUpscaler();
        else
            PauseVirtualUpscaler();

        _status = Status::Waiting;

        if (_producer)
            _producer->Reset();

        LogDiagnostics();
        return out;
    }

    if (acquired != AcquireStatus::Ready)
    {
        LogDiagnostics();
        return out;
    }

    if (keep || !EnsureProducer(source.Device()))
    {
        source.Return(input, FrameOutput {});
        return out;
    }

    // Presents the picture to an upscaler backend as a synthetic call instead of running DLSS-NR on it. Takes priority
    // when both are on. Under a D3D11 game's frame generation swap chain (Dx11wDx12SC) it runs from OnFGPresent instead
    // of the menu's present. The in-list pass keeps the virtual upscaler as well, but only NativeUpscaler gives it this
    // picture.
    const bool useVirtualUpscaler = roles.presentUsesVirtualUpscaler;

    if (roles.keepVirtualUpscaler && _virtualUpscaler == nullptr)
        _virtualUpscaler = new VirtualUpscalerDriver();
    else if (!roles.keepVirtualUpscaler)
        ReleaseVirtualUpscaler();

    NativeProducer::Options options;
    options.apply = roles.presentApplies;
    options.flowPreview = flowPreview;
    options.previewMaxSpeed = kPreviewMaxSpeed;
    options.stageTimings = Config::Instance()->DlssNrNativeStageTimings.value_or_default();

    const auto apply = [this, &source, useVirtualUpscaler](ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)
    {
        _pictureHdr10.store(frame.space == ColorSpace::Pq, std::memory_order_relaxed);

        const DXGI_COLOR_SPACE_TYPE type = frame.space == ColorSpace::ScRgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                           : frame.space == ColorSpace::Pq  ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                                            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        const uint64_t epoch =
            PresentEpoch(source.GetApi(), State::Instance().frameCount,
                         State::Instance().vulkanPresentCount.load(std::memory_order_relaxed));

        // The virtual upscaler runs NR on the picture itself while its backend only feeds frame generation, so it takes
        // what NR Only mode takes: the queue, the colour space and the epoch.
        if (useVirtualUpscaler)
            return _virtualUpscaler->Run(cmd, frame, source.Queue(), type, epoch);

        return DlssNr::ApplyNativeInput(source.Queue(), cmd, frame.color, frame.depth, frame.motion,
                                        frame.historyDistrust, frame.depthReversed, frame.reset, type, frame.pictureState,
                                        epoch);
    };

    FrameOutput output;
    const auto result = _producer->Run(source.Queue(), input, options, apply, output);
    source.Return(input, output);

    // Nothing ran on native input this frame (no NR mode, or the producer was not ready): NR's history must not reach across
    if (!result.nativeRan)
        DlssNr::NativeInputSkipped();
    if (!result.timings.empty())
        LOG_INFO("{}: Native timings (ms per frame, avg/max){}: {}", _tag,
                 _virtualUpscaler != nullptr && _virtualUpscaler->Active()
                     ? " with the " + _virtualUpscaler->BackendName() + " backend"
                     : std::string(),
                 result.timings);

    if (result.stoppedAt != nullptr)
    {
        ++_stopped;

        if (_stopped == 1 || _stopped == 100 || _stopped % 1000 == 0)
            LOG_WARN("{}: the producer stopped at \"{}\" (HRESULT 0x{:X}, device removed reason 0x{:X}), "
                     "{} frames so far",
                     _tag, result.stoppedAt, (unsigned) result.stoppedHr, (unsigned) result.deviceRemoved, _stopped);
    }

    _diagFlowValid += result.flowValid ? 1 : 0;
    _diagDepth += result.flowValid && _producer->Flow() != nullptr && _producer->Flow()->UsedDepth() ? 1 : 0;
    _diagTrust += result.trustRan ? 1 : 0;
    _diagNative += result.nativeRan ? 1 : 0;
    _diagSubmitted += result.submitted ? 1 : 0;

    LogDiagnostics();

    _nativeRan = result.nativeRan;

    if (result.trustRan)
        _trustFrame = _frame;

    if (result.flowValid)
    {
        // How often the depth finder has a copy for the mask: the log shows it every 600 frames.
        ++_seen;
        _ran += result.trustRan ? 1 : 0;

        if (_seen == 600)
        {
            LOG_INFO("{}: the trust mask ran in {} of {} frames", _tag, _ran, _seen);
            _seen = _ran = 0;
        }
    }

    if (result.sceneCut)
    {
        ++_cuts;
        LOG_INFO("{}: scene cut seen ({:.0f}% of the picture distrusted), histories reset", _tag,
                 result.distrustedShare * 100.0f);
    }

    if (result.submitted)
    {
        ++_frame;
        _status = Status::Running;
    }

    out.ran = true;
    out.submitted = result.submitted;
    return out;
}

bool NativeDriver::NrOnlyRunning() const
{
    return _status == Status::Running && _nativeRan && !Config::Instance()->DlssNrNativeUpscaler.value_or_default();
}

void NativeDriver::DrawStatus()
{
    switch (_status)
    {
    case Status::Off:
        ImGui::TextDisabled("Waiting for the first frame.");
        return;
    case Status::Waiting:
        ImGui::TextDisabled("Standing aside: the game is calling an upscaler.");
        return;
    case Status::Failed:
        ImGui::TextDisabled("Could not start: %s", _failure.c_str());
        return;
    case Status::Running:
        break;
    }

    if (Config::Instance()->DlssNrNativeUpscaler.value_or_default())
    {
        if (_nativeRan && _virtualUpscaler != nullptr && _virtualUpscaler->Active())
        {
            const char* backend = _virtualUpscaler->BackendName().c_str();

            if (_virtualUpscaler->NrInPlace())
                ImGui::TextDisabled("NR on the game's picture (no upscaler pass); %s gives frame generation its input.",
                                    backend);
            else if (_virtualUpscaler->BackendPass())
                ImGui::TextDisabled("%s is running as a stabiliser on this picture.", backend);
            else if (!Config::Instance()->DlssNrEnabled.value_or_default())
                ImGui::TextDisabled("NR is off; %s gives frame generation its input (no upscaler pass).", backend);
            else
            {
                // NR is on but did not run on this frame: say why, as NR Only mode does.
                const std::string reason = DlssNr::FinishedPictureStatus();
                ImGui::TextDisabled("NR is not running: %s", reason.empty() ? "waiting." : reason.c_str());
            }
        }
        else if (_virtualUpscaler != nullptr && !_virtualUpscaler->Error().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Upscaler for frame generation: %s",
                               _virtualUpscaler->Error().c_str());
        else
            ImGui::TextDisabled("Upscaler for frame generation: waiting for the motion estimate.");
    }
    else if (_nativeRan)
    {
        ImGui::TextDisabled(_pictureHdr10.load(std::memory_order_relaxed) ? "NR running on this picture (HDR10)."
                                                                          : "NR running on this picture.");
    }
    else
    {
        const std::string reason = DlssNr::FinishedPictureStatus();
        ImGui::TextDisabled("%s", reason.empty() ? "NR: waiting for the motion estimate." : reason.c_str());
    }
}

void NativeDriver::DrawFlowTuning(bool live)
{
    // Always shown and open on Optical F5Low's page; the settings live in the running motion estimate, so there is
    // nothing to tune until a mode runs.
    if (!ImGui::TreeNodeEx("Flow tuning (to compare, applies at once)##flowtuning", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (live ? !_producer : !_tuningShown)
    {
        ImGui::TextDisabled("Shown once a mode is running.");
        ImGui::TreePop();
        return;
    }

    // Busy this frame: the last values, disabled
    auto tune = live ? _producer->Flow()->Tuning() : _shownTuning;
    ImGui::BeginDisabled(!live);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Smoothing radius (0 = off)##flowsmooth", &tune.smoothRadius, 0, 4);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Search radius##flowsearch", &tune.radius, 1, 3);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Coarse cells as candidates##flowcells", &tune.coarseCells, 1, 9);
    ImGui::Checkbox("Last frame's flow as a candidate##flowhistory", &tune.useHistory);
    ImGui::Checkbox("Match within a surface (uses depth)##flowdepth", &tune.depthMatching);

    auto& guides = _producer->Trust()->Tuning();
    ImGui::Checkbox("Sharp motion at outlines (uses depth)##flowguidesurface", &guides.guideSurface);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Each pixel at the outline of a character or object takes its own surface's motion, "
                                "not a blend with what's behind it. Frame generation draws fewer see-through edges.");
    ImGui::Checkbox("Background motion behind moving things (uses depth)##flowuncover", &guides.uncoverBackground);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Background that comes out from behind a character (on a camera turn, or as it "
                                "walks) moves with the background, instead of a guess. Frame generation smears less "
                                "behind it.");
    ImGui::Checkbox("Match within what looks alike##flowlook", &tune.lookWeights);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Matches motion only against neighbours of similar brightness, so a moving edge "
                                "doesn't drag its motion onto what's beside it.");
    ImGui::Checkbox("Camera motion where the picture is flat##flowglobal", &tune.globalCandidate);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Gives flat areas (walls, sky) the whole picture's motion. Off: they get none.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Frames unchanged before it counts as still (0 = off)##flowstill", &tune.stillFrames, 0, 60);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "After this many unchanged frames, an area (a HUD panel) stays still while the "
                                "picture pans. Higher: waits longer. 0: off.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Still: largest change allowed##flowstilleps", &tune.stillEpsilon, 0.0005f, 0.05f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Still: what the camera must win by##flowstillmargin", &tune.stillMargin, 0.0f, 0.2f, "%.3f");
    ImGui::Checkbox("Cheaper sub-pixel refinement##flowinverse", &tune.inverseRefinement);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Faster motion refinement; less exact on thin lines and grain.");
    ImGui::Checkbox("Perceptual luma (HDR and SDR)##flowluma", &tune.perceptualLuma);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Matches motion on perceived lightness, so dark detail counts like bright (HDR and "
                                "SDR). Off: the older method.");
    bool preferStill = tune.zeroMargin > 0.0f;

    if (ImGui::Checkbox("Prefer no motion in flat ground (experimental)##flowzero", &preferStill))
        tune.zeroMargin = preferStill ? 0.001f : 0.0f;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Keeps flat areas still when no motion fits almost as well. Holds HUD panels still, "
                                "but plain walls may not follow the camera. Off by default.");
    ImGui::Checkbox("Find a hard cut on its own frame##flowscene", &tune.sceneCutDetector);

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Detects camera cuts on the frame they happen, so old motion isn't trusted across "
                                "them. Fades and exposure changes don't count.");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Confidence knee##flowknee", &tune.confidenceKnee, 0.0005f, 0.05f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);

    if (ImGui::Button("Close to the earlier build##flowold"))
    {
        tune.smoothRadius = 0;
        tune.radius = 2;
        tune.coarseCells = 1;
        tune.useHistory = false;
    }

    ImGui::SameLine();

    if (ImGui::Button("Defaults##flowdefaults"))
        tune = OpticalFlowDx12::Settings {};

    ImGui::EndDisabled();

    if (live)
    {
        _producer->Flow()->Tuning() = tune;
        _shownTuning = tune;
        _tuningShown = true;
    }

    ImGui::TreePop();
}

void NativeDriver::DrawTrustViewCombo(bool live)
{
    if (live ? !_producer : !_tuningShown)
        return;

    static const char* kViews[] = {
        "Final mask", "Depth check",        "Revealed-surface check", "Flow consistency check",
        "Luma check", "Outside the picture"
    };
    int view = live ? _producer->Trust()->Tuning().debugView : _shownTrustView;

    ImGui::SetNextItemWidth(220.0f);
    ImGui::BeginDisabled(!live);

    if (ImGui::Combo("Show##trustview", &view, kViews, IM_ARRAYSIZE(kViews)) && live)
        _producer->Trust()->Tuning().debugView = view;

    ImGui::EndDisabled();
    _shownTrustView = view;
}

} // namespace native
