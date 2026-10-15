#pragma once

// When Optical F5Low's virtual upscaler (native/VirtualUpscalerDriver.h) is let go, paused or rebuilt. Pure, so a host
// test can run it (tests/nr_virtual_upscaler_hold_smoke.cpp).
//
// Letting it go destroys frame generation's context with it, and making both again costs hundreds of milliseconds on
// the present thread plus a gap in frame generation. A game that calls its own upscaler only for a moment (menus,
// cutscenes, a loading screen) therefore only pauses it; it is let go once the game's calls have run for kTakeoverMs.
// And the backend is rebuilt at once for a new size, format or backend, but for a flip of the depth direction or HDR
// only once the flip has held for kKeySettleFrames frames. The depth direction no longer flips: the guide depth is
// always reversed-Z (TrustMaskDx12::BuildGuides), however the depth finder's pick changes.

#include <cstdint>

namespace native::hold
{
inline constexpr double kTakeoverMs = 5000.0;
inline constexpr uint32_t kKeySettleFrames = 30;

// How long the game has been calling its own upscaler without a break (the frame source's WaitingForUpscaler, which
// already bridges gaps shorter than native::kGameUpscalerQuietMs).
class Takeover
{
  public:
    // True once the game's calls have run for kTakeoverMs. `gameCalling`: the frame source is waiting for the game's
    // upscaler this frame.
    bool Update(bool gameCalling, double nowMs)
    {
        if (!gameCalling)
        {
            _since = -1.0;
            return false;
        }

        if (_since < 0.0)
            _since = nowMs;

        return nowMs - _since >= kTakeoverMs;
    }

  private:
    double _since = -1.0;
};

// Whether a backend built for `built` should be rebuilt for `wanted` now. `flagsOnly`: the two differ only in the depth
// direction or HDR. `firstRealDepth`: the backend was built before any real depth came (its direction was a guess) and
// this frame has one: no reason to wait, the guess was simply wrong.
template <class Key> class RebuildSettle
{
  public:
    bool Now(const Key& built, const Key& wanted, bool flagsOnly, bool firstRealDepth = false)
    {
        if (wanted == built)
        {
            _frames = 0;
            return false;
        }

        if (!flagsOnly || firstRealDepth)
        {
            _frames = 0;
            return true;
        }

        if (_frames == 0 || !(wanted == _pending))
        {
            _pending = wanted;
            _frames = 1;
        }
        else
        {
            ++_frames;
        }

        if (_frames < kKeySettleFrames)
            return false;

        _frames = 0;
        return true;
    }

    // Predict without advancing the settle frame.
    bool WouldNow(const Key& built, const Key& wanted, bool flagsOnly, bool firstRealDepth = false) const
    {
        if (wanted == built)
            return false;
        if (!flagsOnly || firstRealDepth)
            return true;
        const uint32_t frames = _frames == 0 || !(wanted == _pending) ? 1 : _frames + 1;
        return frames >= kKeySettleFrames;
    }

  private:
    Key _pending {};
    uint32_t _frames = 0;
};
} // namespace native::hold
