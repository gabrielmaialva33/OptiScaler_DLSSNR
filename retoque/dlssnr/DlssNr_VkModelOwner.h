#pragma once

// One owner of the Vulkan NR model at a time (docs/dlssnr/design/nr-vulkan-present-host.md, stage 3, "One owner").
//
// Two routes can run the model on Vulkan: the after-upscale route (EvaluateAfterUpscaleVk, inside the game's own
// upscaler call, with the game's depth and motion) and the present host (cold, on zero guides, at vkQueuePresentKHR).
// They must never both run it, on one frame or on one device, and the forwarder initialises NGX on one VkDevice at a
// time (a second device re-initialises it under the first's feature), so the rule is one owner in the process, not
// one per device. The after-upscale route wins: it has the game's guides and runs inside the game's own submission.
//
// - The after-upscale route asks at every evaluate it would run. Granted when nobody holds the model, or it already
//   does (its own device change is its own business: it abandons the old device's state, as before). Refused while
//   the present host holds it; the asking itself is what makes the present host stand down.
// - The present host asks before every frame it would run the model on. Refused while the after-upscale route holds
//   the model, while that route asked within the last kAfterUpscaleLiveMs (it wants it), and while the present host
//   holds it on another device.
// - Once refused for the after-upscale route, the present host stops recording NR, waits for every frame it already
//   recorded to be seen complete, frees its model and releases the token; the after-upscale route takes it at its
//   next evaluate. A frame whose completion is never seen keeps the token held: fail closed.
//
// Standard library only, so tests/nr-vk-present-host drives the state machine on the host.

#include <cstdint>

namespace DlssNr::VkModelOwner
{
enum class Who : uint8_t
{
    None,
    AfterUpscale,
    PresentHost,
};

// How recently the after-upscale route must have asked for the present host to keep standing down: the same half
// second the D3D12 present pass gives the boundary entry (BoundaryLive in DlssNr_Dx12.cpp).
inline constexpr uint64_t kAfterUpscaleLiveMs = 500;

// The reasons, one text each, so a log line can count them.
inline constexpr const char kHeldByPresentHost[] =
    "the Vulkan present host holds the model; it stands down for this route at its next present";
inline constexpr const char kAfterUpscaleOwns[] =
    "the after-upscale route (the game's own upscaler) owns the model in this process";
inline constexpr const char kAfterUpscaleWants[] =
    "the after-upscale route (the game's own upscaler) asked for the model within the last half second";
inline constexpr const char kOtherDevice[] = "the model is in use on another VkDevice of this process";

struct Verdict
{
    bool granted = false;
    const char* reason = nullptr; // one of the texts above, when refused
};

class Arbiter
{
  public:
    Verdict AfterUpscale(uint64_t device, uint64_t nowMs)
    {
        _asked = true;
        _askedMs = nowMs;

        if (_owner == Who::PresentHost)
            return { false, kHeldByPresentHost };

        _owner = Who::AfterUpscale;
        _device = device;
        return { true, nullptr };
    }

    Verdict PresentHost(uint64_t device, uint64_t nowMs) const
    {
        if (_owner == Who::AfterUpscale)
            return { false, kAfterUpscaleOwns };
        if (AfterUpscaleLive(nowMs))
            return { false, kAfterUpscaleWants };
        if (_owner == Who::PresentHost && _device != device)
            return { false, kOtherDevice };
        return { true, nullptr };
    }

    // The present host is about to record a frame on `device`: the token is taken. Only after PresentHost granted.
    void Take(uint64_t device)
    {
        _owner = Who::PresentHost;
        _device = device;
    }

    // The present host holds the model and another route wants it: stop recording, drain, release.
    bool PresentHostMustYield(uint64_t device, uint64_t nowMs) const
    {
        return _owner == Who::PresentHost && _device == device && AfterUpscaleLive(nowMs);
    }

    void Release(Who who, uint64_t device)
    {
        if (_owner == who && _device == device)
        {
            _owner = Who::None;
            _device = 0;
        }
    }

    Who Owner() const { return _owner; }
    uint64_t Device() const { return _device; }

  private:
    bool AfterUpscaleLive(uint64_t nowMs) const
    {
        return _asked && nowMs >= _askedMs && nowMs - _askedMs < kAfterUpscaleLiveMs;
    }

    Who _owner = Who::None;
    uint64_t _device = 0;
    bool _asked = false;
    uint64_t _askedMs = 0;
};
} // namespace DlssNr::VkModelOwner
