#pragma once

// The game's D3D Reflex as InputCommon's input while ReflexHooks' router still delivers it (input_reflex.cpp): which
// calls InputCommon hears of, and when it may run an output of its own. Pure, host tested
// (tests/routed_input_smoke.cpp).

namespace routedinput
{
// The router's call on this thread: whose it is (the game's or not) and how deep the router is nested. A Sleep for DLSS
// frame generation runs OptiScaler's Streamline inside the router, and its Reflex calls come back in on the same
// thread.
struct RoutedCall
{
    bool game = false;
    int depth = 0;
};

// Held around each call the router runs; the call outside it gets its own back afterwards
class RoutedCallScope
{
  public:
    RoutedCallScope(RoutedCall& call, bool game) : _call(call), _outer(call)
    {
        _call.game = game;
        ++_call.depth;
    }

    ~RoutedCallScope() { _call = _outer; }

    RoutedCallScope(const RoutedCallScope&) = delete;
    RoutedCallScope& operator=(const RoutedCallScope&) = delete;

  private:
    RoutedCall& _call;
    RoutedCall _outer;
};

// InputCommon hears of a call when it is the game's and the router is not already running one on this thread: a call
// made from inside the router (Streamline's, even counted as the game's when the game started Streamline) is part of
// the game's call around it
constexpr bool TellsInputCommon(bool gameCall, bool routerAlreadyRunning) { return gameCall && !routerAlreadyRunning; }

struct OutputStep
{
    bool stopOutput = false; // an output that runs stops
    bool mayStart = false;   // an output may start (or keep running) for this call
};

// One call into InputCommon. `activeInputRouted`: the active input is one the router delivers. `callerRouted`: the call
// is from such an input. `outputAskedFor`: the call names an output itself (set_low_latency_tech, for OptiScaler's
// XeFG; nothing makes that call yet, and who may make it while the router delivers is decided when XeFG moves over).
//
// While the active input is routed, the router is its output: one of InputCommon's would be a second sleeper, so a
// running one stops. A routed caller never starts one either, whatever input is active, as it would run on the game's
// device next to the router.
constexpr OutputStep ForCall(bool activeInputRouted, bool callerRouted, bool outputAskedFor)
{
    if (outputAskedFor)
        return { false, true };

    if (activeInputRouted)
        return { true, false };

    return { false, !callerRouted };
}
} // namespace routedinput
