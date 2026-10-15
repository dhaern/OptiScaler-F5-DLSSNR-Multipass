#include "pch.h"

#include "NativeDriverDx12.h"

#include <native/Dx12FrameSource.h>
#include <native/NativeDriver.h>
#include "InListUpscale.h"

#include <Config.h>

#include <menu/menu_overlay_dx.h>
#include <resource_tracking/GenericDepth_Dx12.h>
#include <resource_tracking/SpatialPass_Dx12.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace
{

native::NativeDriver g_driver("Native motion", "", true);
native::Dx12FrameSource g_source;

// Under frame generation the menu's present hook sees the real swap chain after frame generation ran, on its presenter's
// thread and queue; FGPresent drives the step instead. The menu's hook stands aside while FGPresent ran within this many of
// its presents (frame generation presents several frames for each of the game's).
constexpr uint64_t kFgPresentGrace = 8;
std::atomic<uint64_t> g_menuPresents { 0 };
std::atomic<uint64_t> g_menuPresentsAtFg { 0 };
std::atomic<bool> g_fgDriven { false };

// The two hooks run on different threads while frame generation starts up. The menu's only tries it: its thread is frame
// generation's presenter, which must never wait on the game thread.
std::mutex g_runMutex;

// The session (NativeDriverDx12.h): after g_runMutex, before frame generation's Mutex and the depth finder's lock. An
// SRW lock: constant-initialised, and taken on a game's recording thread without thread-local storage or exceptions.
SRWLOCK g_sessionMutex = SRWLOCK_INIT;

// A hold on the session for a scope. Wait: blocks (the present, outside frame generation's present lock). Try: never
// waits. Unshared: the in-list pass's hooks are not installed, so only the present takes the session; nothing is
// locked, as before the pass existed (the menu).
class SessionHold
{
  public:
    enum class Mode
    {
        Wait,
        Try,
        Unshared
    };

    explicit SessionHold(Mode mode)
    {
        if (mode == Mode::Wait)
        {
            AcquireSRWLockExclusive(&g_sessionMutex);
            _locked = true;
        }
        else if (mode == Mode::Try)
        {
            _locked = TryAcquireSRWLockExclusive(&g_sessionMutex) != FALSE;
        }

        _owns = _locked || mode == Mode::Unshared;
    }

    ~SessionHold()
    {
        if (_locked)
            ReleaseSRWLockExclusive(&g_sessionMutex);
    }

    SessionHold(const SessionHold&) = delete;
    SessionHold& operator=(const SessionHold&) = delete;

    bool Owns() const { return _owns; }

  private:
    bool _locked = false;
    bool _owns = false;
};

// The menu's hold: a try once a recording thread can take the session, no lock before (see SessionHold).
SessionHold::Mode MenuMode()
{
    return SpatialPassDx12::Installed() ? SessionHold::Mode::Try : SessionHold::Mode::Unshared;
}

// Presents OnPresent left out because the session was busy (BusyPresents).
std::atomic<uint64_t> g_busyPresents { 0 };

bool g_previewWanted = false; // the menu node is open

// The menu's descriptors for the two preview pictures.
struct PreviewView
{
    bool allocated = false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu {};
    ID3D12Resource* resource = nullptr;
};

ID3D12DescriptorHeap* g_srvHeap = nullptr;
PreviewView g_flowView;
PreviewView g_maskView;

// Shows a texture in the menu: a descriptor in the menu's heap (made once, and again if the heap or the texture changes).
// `grayFromRed` shows a one-channel texture as gray.
bool ShowTexture(PreviewView& view, ID3D12Resource* texture, DXGI_FORMAT format, bool grayFromRed, float width,
                 float height)
{
    ID3D12DescriptorHeap* heap = MenuOverlayDx::SrvHeap();

    ID3D12Device* device = g_driver.Producer() != nullptr ? g_driver.Producer()->Device() : nullptr;

    if (heap == nullptr || texture == nullptr || device == nullptr)
        return false;

    if (heap != g_srvHeap)
    {
        g_srvHeap = heap;
        g_flowView = PreviewView {};
        g_maskView = PreviewView {};
    }

    if (!view.allocated && MenuOverlayDx::AllocSrv(&view.cpu, &view.gpu))
        view.allocated = true;

    if (!view.allocated)
        return false;

    if (texture != view.resource)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping =
            grayFromRed ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
                        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture, &srv, view.cpu);
        view.resource = texture;
    }

    ImGui::Image((ImTextureID) view.gpu.ptr, ImVec2(width, height));
    return true;
}

void RunFrame(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    // The in-list pass first, once its hooks are installed: its mode decides whether the present path runs, and work
    // recorded into a game's list keeps the producer's device
    const bool inList = SpatialPassDx12::Installed();
    native::inlist::PresentStep step;

    if (inList)
        step = native::inlist::BeginPresent(g_driver, device);

    if (g_driver.Enabled() && swapChain != nullptr && queue != nullptr && device != nullptr && !g_driver.Failed())
    {
        g_source.SetPresent(swapChain, queue, device);

        if (g_driver.RunFrame(g_source, g_previewWanted, step.runPresentPath, step.keepDevice).ran)
            g_previewWanted = false;
    }

    if (inList)
        native::inlist::EndPresent(g_driver, queue);
}

} // namespace

namespace NativeMotionDx12
{

void OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    // A D3D11 game's frame generation swap chain is the interop's D3D12 one: its picture is the game's, already handled by
    // NativeMotionDx11 from Dx11wDx12SC::Present, and this present runs inside frame generation's own present, under the
    // lock the virtual upscaler's UpscaleStart (-> EvaluateState) takes again: that deadlocked.
    if (State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
        return;

    const uint64_t presents = ++g_menuPresents;

    if (g_fgDriven.load() && presents - g_menuPresentsAtFg.load() <= kFgPresentGrace)
        return;

    std::unique_lock lock(g_runMutex, std::try_to_lock);

    if (!lock.owns_lock())
        return;

    // As the menu's (MenuMode): no lock, and frame generation not looked at, before the in-list pass's hooks are
    // installed. Then, while there is frame generation at all (active or not: it can turn active meanwhile), this
    // thread can be its presenter, which must never wait on a game thread: one holding the session can be waiting
    // for frame generation's present. Only tried then; a present left out is counted.
    const SessionHold session(!SpatialPassDx12::Installed()            ? SessionHold::Mode::Unshared
                              : State::Instance().currentFG != nullptr ? SessionHold::Mode::Try
                                                                       : SessionHold::Mode::Wait);

    if (!session.Owns())
    {
        g_busyPresents.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    g_fgDriven = false;
    GenericDepthDx12::OnPresent(swapChain);
    RunFrame(swapChain, queue, device);
}

void OnFGPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    g_menuPresentsAtFg = g_menuPresents.load();
    g_fgDriven = true;

    std::lock_guard lock(g_runMutex);
    // Outside frame generation's present lock: waits, once the in-list pass's hooks are installed (no lock before, as
    // the menu's MenuMode).
    const SessionHold session(SpatialPassDx12::Installed() ? SessionHold::Mode::Wait : SessionHold::Mode::Unshared);
    GenericDepthDx12::OnPresent(swapChain);
    RunFrame(swapChain, queue, device);
}

Session::~Session()
{
    if (_owned)
        ReleaseSRWLockExclusive(&g_sessionMutex);
}

native::NativeDriver* Session::Driver() const { return _owned ? &g_driver : nullptr; }

Session TrySession() { return Session(TryAcquireSRWLockExclusive(&g_sessionMutex) != FALSE); }

uint64_t BusyPresents() { return g_busyPresents.load(std::memory_order_relaxed); }

void DrawStatus()
{
    // Busy this frame: the line is kept, empty.
    const SessionHold session(MenuMode());

    if (!session.Owns())
    {
        ImGui::NewLine();
        return;
    }

    g_driver.DrawStatus();
}

bool NrOnlyRunning() { return g_driver.NrOnlyRunning(); }

void DrawAdvancedUi()
{
    // The producer is read and its tuning written only with the session. Busy this frame: the tuning shows its last
    // values, disabled, and the pictures are left out (their boxes kept).
    const SessionHold session(MenuMode());
    const bool owned = session.Owns();

    const bool debugView = Config::Instance()->DlssNrNativeDebugView.value_or_default();

    g_driver.DrawFlowTuning(owned);

    // The pictures and their controls are debugging aids: off by default, nothing is recorded for them while they are hidden.
    if (!debugView)
        return;

    const float boxWidth = 360.0f;
    const float boxHeight = boxWidth * 9.0f / 16.0f;
    bool drawn = false;

    ImGui::TextDisabled("Motion: hue is the direction, brightness the speed.");

    if (owned && g_driver.GetStatus() == native::NativeDriver::Status::Running)
    {
        g_previewWanted = true;

        if (g_driver.Producer() && g_driver.Producer()->PreviewReady())
            drawn = ShowTexture(g_flowView, g_driver.Producer()->Flow()->Preview(), DXGI_FORMAT_R8G8B8A8_UNORM, false, boxWidth, boxHeight);
    }

    if (!drawn)
        ImGui::Dummy(ImVec2(boxWidth, boxHeight));

    // A frame can have no mask (no motion estimate yet); the picture then keeps the last one rather than going black.
    const bool trustRecent = g_driver.TrustRecent();

    if (g_driver.GetStatus() == native::NativeDriver::Status::Running)
    {
        if (trustRecent)
            ImGui::TextDisabled("Trust: white is where the last frame cannot be trusted (%llu cuts seen).",
                                (unsigned long long) g_driver.Cuts());
        else
            ImGui::TextDisabled("Trust: waiting for the motion estimate.");
    }
    else
        ImGui::TextDisabled("Trust: -");

    g_driver.DrawTrustViewCombo(owned);

    bool maskDrawn = false;

    if (owned && g_driver.GetStatus() == native::NativeDriver::Status::Running && trustRecent && g_driver.Producer())
        maskDrawn = ShowTexture(g_maskView, g_driver.Producer()->Trust()->Mask(), DXGI_FORMAT_R8_UNORM, true, boxWidth, boxHeight);

    if (!maskDrawn)
        ImGui::Dummy(ImVec2(boxWidth, boxHeight));
}

} // namespace NativeMotionDx12
