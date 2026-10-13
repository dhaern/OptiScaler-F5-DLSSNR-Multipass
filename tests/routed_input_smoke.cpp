// Host check of RoutedInputRule.h: the game's D3D Reflex as InputCommon's input while ReflexHooks' router delivers it.
// No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/routed_input_smoke.cpp
#include "../OptiScaler/low_latency/input/RoutedInputRule.h"

#include <cstdio>

using namespace routedinput;

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

// What InputReflex does for one call: decide before the router runs, then the router runs inside a scope
static bool TellsForCall(RoutedCall& call, bool game) { return TellsInputCommon(game, call.depth > 0); }

int main()
{
    // Output decisions
    {
        // The game's Reflex is the active input: a running output stops, none starts, from any caller
        auto s = ForCall(true, true, false);
        CHECK(s.stopOutput && !s.mayStart);
        s = ForCall(true, false, false);
        CHECK(s.stopOutput && !s.mayStart);

        // Another input is active (the UE mod): its own calls may run the output
        s = ForCall(false, false, false);
        CHECK(!s.stopOutput && s.mayStart);

        // ... but a call of the game's Reflex never starts one on the game's device
        s = ForCall(false, true, false);
        CHECK(!s.stopOutput && !s.mayStart);

        // An output asked for by name goes through untouched (nothing asks yet)
        s = ForCall(true, true, true);
        CHECK(!s.stopOutput && s.mayStart);
    }

    // Who InputCommon hears of
    CHECK(TellsInputCommon(true, false));
    CHECK(!TellsInputCommon(false, false));
    CHECK(!TellsInputCommon(true, true));
    CHECK(!TellsInputCommon(false, true));

    // The router's scope on one thread
    {
        RoutedCall call;
        CHECK(call.depth == 0 && !call.game);

        // The game's Sleep: told, then routed as the game's
        CHECK(TellsForCall(call, true));
        {
            RoutedCallScope outer(call, true);
            CHECK(call.depth == 1 && call.game);

            // OptiScaler's Streamline sleeps for DLSS frame generation inside it: not the game's, not told
            CHECK(!TellsForCall(call, false));
            {
                RoutedCallScope inner(call, false);
                CHECK(call.depth == 2 && !call.game);
            }

            // Back in the game's call: the game's again
            CHECK(call.depth == 1 && call.game);

            // A game that started Streamline itself: Streamline's call counts as the game's, still not told (the
            // game's Sleep around it was)
            CHECK(!TellsForCall(call, true));
            {
                RoutedCallScope inner(call, true);
                CHECK(call.depth == 2 && call.game);
            }

            CHECK(call.depth == 1 && call.game);
        }

        // After the call: nothing running
        CHECK(call.depth == 0 && !call.game);

        // OptiScaler's own Streamline outside any game call: routed as not the game's, not told
        CHECK(!TellsForCall(call, false));
        {
            RoutedCallScope scope(call, false);
            CHECK(call.depth == 1 && !call.game);
        }

        CHECK(call.depth == 0 && !call.game);
    }

    static_assert(ForCall(true, false, false).stopOutput);
    static_assert(!ForCall(false, true, false).mayStart);
    static_assert(!TellsInputCommon(true, true));

    if (fails == 0)
        printf("routed_input_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
