// Host check of native/NativeDriverRoles.h: who owns the native driver's producer and virtual upscaler, and which hooks
// are installed, for every combination of [DlssNr] NativeMotion, NativeInput, NativeUpscaler and [Upscalers]
// ReplaceGameFsr1, on D3D12's driver and on the others. With ReplaceGameFsr1 off every answer is what be333e3f decided
// (NativeDriver.cpp: Enabled() = NativeMotion, the virtual upscaler kept and used = NativeUpscaler, applied =
// NativeUpscaler || NativeInput; the depth finder = NativeDepthFinder, its copies = overlay || NativeMotion). With it
// on, D3D12's driver keeps the producer and the virtual upscaler with no [DlssNr] key, and never gives the present's
// picture to DLSS-NR for it.
//
//   vcvars64, then from the repo root:
//   cl /nologo /std:c++20 /EHsc /W4 tests\nr_native_driver_roles_smoke.cpp
//   nr_native_driver_roles_smoke.exe
// Exit 0 when every check passes, 1 otherwise.
#include "../OptiScaler/native/NativeDriverRoles.h"

#include <cstdio>

namespace
{
int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what, unsigned combo)
{
    ++g_checks;

    if (!ok)
    {
        std::printf("FAIL: %s (combination %u)\n", what, combo);
        ++g_failures;
    }
}

using native::roles::Decide;
using native::roles::Keys;
using native::roles::Roles;

// be333e3f's NativeDriver, written out: the present path only.
Roles Legacy(const Keys& k)
{
    Roles r;
    r.presentPath = k.nativeMotion;
    r.enabled = k.nativeMotion;
    r.keepVirtualUpscaler = k.nativeUpscaler;
    r.presentUsesVirtualUpscaler = k.nativeUpscaler;
    r.presentApplies = k.nativeUpscaler || k.nativeInput;
    return r;
}

bool Same(const Roles& a, const Roles& b)
{
    return a.presentPath == b.presentPath && a.inList == b.inList && a.enabled == b.enabled &&
           a.keepVirtualUpscaler == b.keepVirtualUpscaler &&
           a.presentUsesVirtualUpscaler == b.presentUsesVirtualUpscaler && a.presentApplies == b.presentApplies &&
           a.inListOnly == b.inListOnly;
}

Keys KeysOf(unsigned combo)
{
    Keys k;
    k.nativeMotion = (combo & 1) != 0;
    k.nativeInput = (combo & 2) != 0;
    k.nativeUpscaler = (combo & 4) != 0;
    k.replaceGameFsr1 = (combo & 8) != 0;
    k.inListApi = (combo & 16) != 0;
    return k;
}

void Table()
{
    unsigned legacyRows = 0, inListRows = 0;

    for (unsigned combo = 0; combo < 32; ++combo)
    {
        const Keys k = KeysOf(combo);
        const Roles r = Decide(k);

        // Off, or not D3D12's driver: exactly as before.
        if (!k.replaceGameFsr1 || !k.inListApi)
        {
            Check(Same(r, Legacy(k)), "ReplaceGameFsr1 off (or not D3D12): the table is be333e3f's", combo);
            ++legacyRows;
            continue;
        }

        ++inListRows;

        Check(r.inList, "ReplaceGameFsr1 on D3D12: the in-list pass owns", combo);
        Check(r.enabled, "the driver is enabled without any [DlssNr] key", combo);
        Check(r.keepVirtualUpscaler, "the virtual upscaler is kept without NativeUpscaler", combo);
        Check(r.inListOnly == !k.nativeMotion, "without NativeMotion a present only makes the producer", combo);

        // The present path, when it runs, is the one be333e3f ran: the in-list pass changes none of its choices.
        const Roles legacy = Legacy(k);
        Check(r.presentPath == legacy.presentPath, "the present path runs exactly when NativeMotion is on", combo);
        Check(r.presentUsesVirtualUpscaler == legacy.presentUsesVirtualUpscaler,
              "the present's consumer is still NativeUpscaler's choice", combo);
        Check(r.presentApplies == legacy.presentApplies, "the present applies exactly as before", combo);
    }

    Check(legacyRows == 24 && inListRows == 8, "every combination seen", 0);
}

// The in-list pass alone (no [DlssNr] key at all): nothing reaches DLSS-NR from the present, nothing is applied.
void NoDlssNrKey()
{
    Keys k;
    k.replaceGameFsr1 = true;
    k.inListApi = true;
    const Roles r = Decide(k);

    Check(r.enabled && r.inList && r.inListOnly && r.keepVirtualUpscaler, "ReplaceGameFsr1 alone owns the driver", 0);
    Check(!r.presentPath && !r.presentApplies && !r.presentUsesVirtualUpscaler,
          "ReplaceGameFsr1 alone runs no present path (no DLSS-NR native input)", 0);

    k.inListApi = false;
    Check(!Decide(k).enabled, "D3D11 and Vulkan drivers ignore ReplaceGameFsr1", 0);
}

// The install gates and the depth copies.
void Gates()
{
    using native::roles::DepthFinderWanted;
    using native::roles::DepthSnapshotsWanted;
    for (unsigned combo = 0; combo < 8; ++combo)
    {
        const bool a = (combo & 1) != 0, b = (combo & 2) != 0, replace = (combo & 4) != 0;

        Check(DepthFinderWanted(a, replace) == (a || replace), "depth finder: NativeDepthFinder || ReplaceGameFsr1",
              combo);
        Check(DepthSnapshotsWanted(a, b, replace) == (a || b || replace),
              "depth copies: overlay || NativeMotion || ReplaceGameFsr1", combo);

        if (!replace)
        {
            Check(DepthFinderWanted(a, false) == a, "depth finder off: be333e3f's gate", combo);
            Check(DepthSnapshotsWanted(a, b, false) == (a || b), "depth copies off: be333e3f's choice", combo);
        }
    }
}

// The table is constexpr: usable where a constant is needed.
static_assert(!Decide(Keys {}).enabled, "all off: disabled");
static_assert(Decide(Keys { false, false, false, true, true }).inListOnly, "ReplaceGameFsr1 alone on D3D12");
static_assert(!Decide(Keys { false, false, false, true, false }).enabled, "not on another API");
} // namespace

int main()
{
    Table();
    NoDlssNrKey();
    Gates();

    if (g_failures != 0)
    {
        std::printf("nr_native_driver_roles_smoke: %d failure(s) of %d checks\n", g_failures, g_checks);
        return 1;
    }

    std::printf("nr_native_driver_roles_smoke: PASS (%d checks, 32 combinations)\n", g_checks);
    return 0;
}
