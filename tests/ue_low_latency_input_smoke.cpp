// Host check of UeInputRule.h: the UE Low Latency mod as a low latency input. No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/ue_low_latency_input_smoke.cpp
#include "../OptiScaler/low_latency/input/UeInputRule.h"

#include <cstdio>

using namespace ueinput;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

static const int deviceA = 1;
static const int deviceB = 2;
static const void* const A = &deviceA;
static const void* const B = &deviceB;

int main()
{
    // Who else lowers latency: the game first, then ours, then frame generation
    CHECK(OtherOwner(false, false, false) == Owner::None);
    CHECK(OtherOwner(true, true, true) == Owner::Game);
    CHECK(OtherOwner(false, true, true) == Owner::OwnLowLatency);
    CHECK(OtherOwner(false, false, true) == Owner::FrameGeneration);

    // Sleep mode: sent once, again only on a change
    CHECK(SleepModeToSend(false, std::nullopt) == std::optional<bool>(true));
    CHECK(SleepModeToSend(true, std::nullopt) == std::optional<bool>(false));
    CHECK(!SleepModeToSend(false, true).has_value());
    CHECK(SleepModeToSend(true, true) == std::optional<bool>(false));

    {
        Memory m;

        // No swap chain device yet (or Vulkan): nothing
        auto s = Next(m, nullptr, Owner::None, false, false);
        CHECK(!s.drive && !s.stopOutput && !s.dropOutput && !s.sleepMode);

        // First frame: drives, low latency on
        s = Next(m, A, Owner::None, false, false);
        CHECK(s.drive && !s.stopOutput && !s.dropOutput && s.sleepMode == std::optional<bool>(true));

        // Steady: drives, nothing to send
        s = Next(m, A, Owner::None, true, false);
        CHECK(s.drive && !s.stopOutput && !s.sleepMode);

        // Our own low latency starts while the output is Reflex: stand aside, Reflex stays (stopping it would send
        // low latency off over our own mode)
        s = Next(m, A, Owner::OwnLowLatency, true, false);
        CHECK(!s.drive && !s.stopOutput && !s.dropOutput);
        s = Next(m, A, Owner::OwnLowLatency, true, false);
        CHECK(!s.drive && !s.stopOutput);

        // It stops and hands back (it left the driver with low latency off): the output starts afresh and the mode
        // is sent again
        s = Next(m, A, Owner::None, true, false);
        CHECK(s.drive && s.stopOutput && s.sleepMode == std::optional<bool>(true));

        // Frame generation that runs its own low latency, output Reflex: stand aside, Reflex stays
        s = Next(m, A, Owner::FrameGeneration, true, false);
        CHECK(!s.drive && !s.stopOutput);
        s = Next(m, A, Owner::None, true, false);
        CHECK(s.drive && s.stopOutput && s.sleepMode == std::optional<bool>(true));

        // Same with Anti-Lag 2 or LatencyFlex as the output: it stops (no second context next to theirs)
        s = Next(m, A, Owner::FrameGeneration, false, false);
        CHECK(!s.drive && s.stopOutput);

        // The game's own Reflex: stand aside and stop the output, whatever it is
        s = Next(m, A, Owner::Game, true, false);
        CHECK(!s.drive && s.stopOutput);
    }

    {
        Memory m;
        Next(m, A, Owner::None, true, false);

        // A new device: the old output is let go of without a call to the gone device, the mode is sent again
        auto s = Next(m, B, Owner::None, true, false);
        CHECK(s.drive && s.dropOutput && !s.stopOutput && s.sleepMode == std::optional<bool>(true));

        // The device goes away: let go, nothing driven
        s = Next(m, nullptr, Owner::None, true, false);
        CHECK(!s.drive && s.dropOutput && !s.stopOutput);

        // A new device while someone else runs: let go of the old one, still aside
        Next(m, A, Owner::None, true, false);
        s = Next(m, B, Owner::OwnLowLatency, true, false);
        CHECK(!s.drive && s.dropOutput && !s.stopOutput);
    }

    {
        Memory m;
        Next(m, A, Owner::None, true, false);

        // Input Off: low latency off once, Sleep and markers keep flowing
        auto s = Next(m, A, Owner::None, true, true);
        CHECK(s.drive && s.sleepMode == std::optional<bool>(false));
        s = Next(m, A, Owner::None, true, true);
        CHECK(s.drive && !s.sleepMode);

        // Back on
        s = Next(m, A, Owner::None, true, false);
        CHECK(s.drive && s.sleepMode == std::optional<bool>(true));
    }

    static_assert(OtherOwner(false, false, true) == Owner::FrameGeneration);

    if (fails == 0)
        printf("ue_low_latency_input_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
