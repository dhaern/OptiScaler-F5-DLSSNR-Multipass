#pragma once

// The game-side half of the native input producer that D3D12 and D3D11 share: one frame step on a native::IFrameSource,
// and the menu's status and tuning that go with it.
//
// A driver per API (native/NativeDriverDx12.cpp, native/NativeDriverDx11.cpp) owns an adapter (Dx12FrameSource,
// Dx11FrameSource) and one NativeDriver, and keeps only what is its API's: when the step runs (the frame generation
// hooks, the threads), the depth finder's frame close, the menu's pictures. Everything else lives here: the [DlssNr]
// gate, Acquire, the producer made again when its device changes, the virtual upscaler's life, the consumer (DLSS-NR or
// the virtual upscaler), the bookkeeping, the log lines and the status line.
//
// The producer and the virtual upscaler have two possible owners (native/NativeDriverRoles.h): the present path
// ([DlssNr] NativeMotion) and, in D3D12's driver, the in-list pass ([Upscalers] ReplaceGameFsr1), which needs no
// [DlssNr] key. Either keeps them; with both off the virtual upscaler is released.

#include "FrameContract.h"
#include "NativeDriverRoles.h"
#include "NativeProducer.h"
#include "VirtualUpscalerHold.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace native
{

class VirtualUpscalerDriver;

class NativeDriver
{
  public:
    enum class Status
    {
        Off,
        Waiting, // the game calls an upscaler
        Failed,
        Running
    };

    struct RunResult
    {
        bool ran = false;       // the producer was run (and the source's frame given back)
        bool submitted = false; // its work was recorded and sent: the source's picture is the result
    };

    // `tag` starts every log line ("Native motion", "Native motion (D3D11)"); `readyNote` is added to the line that
    // says the producer is ready. `inListApi`: this is D3D12's driver, the one the in-list pass uses.
    NativeDriver(const char* tag, const char* readyNote, bool inListApi = false)
        : _tag(tag), _readyNote(readyNote), _inListApi(inListApi)
    {
    }

    // Not copyable, and the virtual upscaler is never destroyed here (see _virtualUpscaler).
    NativeDriver(const NativeDriver&) = delete;
    NativeDriver& operator=(const NativeDriver&) = delete;

    // The first thing a present does: false while neither owner wants the driver, [DlssNr] NativeMotion and (D3D12)
    // [Upscalers] ReplaceGameFsr1 both off (the virtual upscaler is released and the status goes back to Off).
    bool Enabled();

    // Once the producer could not start nothing more is tried (the status line says why).
    bool Failed() const { return _status == Status::Failed; }

    // One frame: Acquire on `source` (its SetPresent is done), the producer, Return. `flowPreview` asks the producer
    // for the flow picture the menu shows. With the in-list pass alone (no present path), or while the in-list pass
    // records (`runPresentPath` false, native::inlist::BeginPresent): only EnsureProducer on source.Device() and the
    // virtual upscaler made; no Acquire, no Run, nothing given to DLSS-NR, `ran` stays false. `keepDevice`: work
    // recorded into a game's list may still run, so a producer made for another device is kept (the frame is left out).
    RunResult RunFrame(IFrameSource& source, bool flowPreview, bool runPresentPath = true, bool keepDevice = false);

    // Makes the producer for `device`, again when the device changed (which deletes the virtual upscaler too: its
    // backend belongs to the old device). On the present thread, never on a game's recording thread. False when the
    // producer could not start (the status is then Failed and says why).
    bool EnsureProducer(ID3D12Device* device);

    // ImGui: one line for the mode in effect (waiting/failed, NR or the stabiliser on this picture, or why not).
    void DrawStatus();

    // ImGui: the optical flow's tuning, with NativeDebugView on and a producer made. Applies at once. `live` false (the
    // session is busy this frame): the last values shown, disabled, nothing read or written.
    void DrawFlowTuning(bool live = true);

    // ImGui: which one of the trust mask's checks to look at (the mask picture shows it). `live` as above.
    void DrawTrustViewCombo(bool live = true);

    // NR ran on the finished picture in the last frame (not the virtual upscaler): the menu header says so.
    bool NrOnlyRunning() const;

    // For the API drivers' menu pictures and status text, and for the in-list pass under its API driver's session.
    NativeProducer* Producer() { return _producer.get(); }
    // Null until a present with an owner that keeps it (Roles::keepVirtualUpscaler). Released (its backend let go)
    // when no owner keeps it; deleted only by EnsureProducer on a device change.
    VirtualUpscalerDriver* VirtualUpscaler() { return _virtualUpscaler; }
    // The owners the configuration asks for now (read on every call; nothing is stored).
    roles::Roles DecideRoles() const;
    Status GetStatus() const { return _status; }
    uint64_t Frame() const { return _frame; }
    uint64_t Cuts() const { return _cuts; }
    // The mask ran within the last 30 frames. A frame can have no mask (no motion estimate yet); the picture then keeps
    // the last one rather than going black.
    bool TrustRecent() const { return _trustFrame != 0 && _frame - _trustFrame < 30; }

  private:
    void ReleaseVirtualUpscaler();
    void PauseVirtualUpscaler();
    void LogDiagnostics();

    const char* _tag;
    const char* _readyNote;
    const bool _inListApi;

    std::unique_ptr<NativeProducer> _producer;

    // Made on first use and never destroyed at exit: its destructor would tear down an upscaler backend under the
    // loader lock. Deleted only when the producer's device changes.
    VirtualUpscalerDriver* _virtualUpscaler = nullptr;
    hold::Takeover _takeover; // how long the game's own upscaler calls have run (VirtualUpscalerHold.h)

    uint64_t _frame = 0;      // frames the producer submitted
    bool _nativeRan = false;  // DLSS-NR or the virtual upscaler ran on native input in the last frame
    std::atomic<bool> _pictureHdr10 { false }; // the last picture handed to NR was HDR10 (read by the menu thread)
    uint64_t _trustFrame = 0; // the last frame the trust mask was recorded in
    uint64_t _cuts = 0;       // scene cuts the mask reported
    Status _status = Status::Off;
    std::string _failure;

    // A line every 300 presents says where the frames go, so a "waiting for the motion estimate" that never ends can be
    // told apart: no picture (Acquire not ready), flow never valid, trust mask not running, or the backend not
    // applying. Of the frames with a valid flow, how many were matched with depth (the depth finder settled on exactly
    // one depth buffer and depth matching is on): the edges-without-depth work only matters in the rest.
    uint64_t _diagPresents = 0, _diagReady = 0, _diagWaiting = 0, _diagFlowValid = 0, _diagDepth = 0, _diagTrust = 0,
             _diagNative = 0, _diagSubmitted = 0;
    uint64_t _stopped = 0; // frames the producer gave up on
    uint64_t _seen = 0,
             _ran = 0; // frames with a valid flow, and those the trust mask ran in: the log shows it every 600

    // What the menu showed last, for a frame it cannot read the producer (DrawFlowTuning, DrawTrustViewCombo)
    OpticalFlowDx12::Settings _shownTuning {};
    int _shownTrustView = 0;
    bool _tuningShown = false;
};

} // namespace native
