#pragma once

// The UE Low Latency mod as a low latency input (input_uell.cpp): each frame, whether its frames drive the output, what
// happens to the output when someone else takes over or hands back, and which sleep mode it sends. Pure, host tested
// (tests/ue_low_latency_input_smoke.cpp).

#include <cstdint>
#include <optional>

namespace ueinput
{
// Who else lowers latency this frame. The mod stands aside for each, one sleeper per frame.
enum class Owner : uint8_t
{
    None,            // nobody: the mod's frames drive the output
    Game,            // the game's own Reflex
    OwnLowLatency,   // OptiScaler's own low latency (Optical F5Low, frame generation)
    FrameGeneration, // frame generation that runs its low latency itself (DLSS FG output, XeFG with its XeLL)
};

constexpr Owner OtherOwner(bool gameCalledReflex, bool ownLowLatencyRuns, bool frameGenerationOwns)
{
    if (gameCalledReflex)
        return Owner::Game;

    if (ownLowLatencyRuns)
        return Owner::OwnLowLatency;

    if (frameGenerationOwns)
        return Owner::FrameGeneration;

    return Owner::None;
}

// The sleep mode to send this frame (low latency on or off), nothing while the last one sent still holds. The Framerate
// page's Input Off sends low latency off; Sleep and the markers keep flowing.
constexpr std::optional<bool> SleepModeToSend(bool inputOff, std::optional<bool> lastSent)
{
    const bool lowLatency = !inputOff;

    if (lastSent == lowLatency)
        return std::nullopt;

    return lowLatency;
}

// What the mod knows between frames
struct Memory
{
    const void* device = nullptr; // the swap chain's device the output runs on
    Owner owner = Owner::None;
    std::optional<bool> sentLowLatency; // the sleep mode last sent
};

struct Step
{
    bool drive = false;            // the frame goes to the output (Sleep, markers)
    bool dropOutput = false;       // let go of the output without a call to its device: that device is gone
    bool stopOutput = false;       // stop the output (Reflex leaves the driver with low latency off)
    std::optional<bool> sleepMode; // the sleep mode to send first
};

// One frame. `device`: the swap chain's D3D device now, null without one (Vulkan, before the swap chain).
// `outputIsReflex`: the output running now is Reflex.
//
// Someone taking over: the output stops when it is the game (a game that only sends markers would keep our low latency
// on), and for anyone when it is not Reflex (a second Anti-Lag 2 context next to theirs). Reflex stays for OptiScaler's
// own low latency and frame generation: stopping it would send low latency off over the mode they set.
// Handing back: the output starts afresh and the sleep mode is sent again. The one before may have left the driver with
// low latency off (our own low latency does when it stops), which an output that thinks its mode holds would never
// undo.
constexpr Step Next(Memory& memory, const void* device, Owner owner, bool outputIsReflex, bool inputOff)
{
    Step step;

    if (device != memory.device)
    {
        step.dropOutput = memory.device != nullptr;
        memory.device = device;
        memory.sentLowLatency.reset();
    }

    if (owner != memory.owner)
    {
        step.stopOutput = !step.dropOutput && (owner == Owner::None || owner == Owner::Game || !outputIsReflex);

        if (owner == Owner::None)
            memory.sentLowLatency.reset();

        memory.owner = owner;
    }

    if (device == nullptr || owner != Owner::None)
        return step;

    step.drive = true;
    step.sleepMode = SleepModeToSend(inputOff, memory.sentLowLatency);

    if (step.sleepMode.has_value())
        memory.sentLowLatency = step.sleepMode;

    return step;
}
} // namespace ueinput
