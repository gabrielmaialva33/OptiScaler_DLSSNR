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
//   the model, while that route asked within the last kAfterUpscaleLiveMs (it wants it), while it was refused for the
//   present host and has not taken the model since (unless it then went quiet for kAfterUpscaleIdleMs: a frame slower
//   than the live window must not let the host take the model back between the route's refusal and its next ask), and
//   while the present host holds it on another device. A host still holding the model after a refusal yields with no
//   time limit: it frees the model as soon as its frames are seen complete.
// - Once refused for the after-upscale route, the present host stops recording NR, waits for every frame it already
//   recorded to be seen complete, frees its model and releases the token; the after-upscale route takes it at its
//   next evaluate. A frame whose completion is never seen keeps the token held: fail closed.
// - The way back (found in DOOM Eternal's first stage-3 run, 2026-10-14: the game's DLSS turned off left the route
//   holding the model for the session, and the present host refusing every frame). The route has no moment of its own
//   to give the model up: a game that stops calling its upscaler simply stops calling. So once the route has not been
//   inside an evaluate for kAfterUpscaleIdleMs -- counted from the end of its last one (AfterUpscaleDone), since it
//   records after it asks -- the present host may take it back: it marks the hand-back begun on that device
//   (BeginHandBack, refused while the route records or if it asked in the meantime), drains every queue of the device
//   with a fence of its own, and when the fences have signalled frees the route's objects and the token together
//   (FinishHandBack). The route asking at any point after the hand-back began, on any device, cancels it: the route
//   keeps the model, nothing of its is freed, and the fences are only waited for. Asking again after the hand-back
//   finished is the ordinary stand-down, as many times as the game turns its upscaler on and off.
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

// How long the after-upscale route must not have asked before the present host takes the model back from it. Twenty
// times the live window: a game's upscaler pauses for a loading screen, a menu or a feature rebuild (DOOM Eternal made
// two features two seconds apart when DLSS came back on), and a hand-back in the middle of one costs two rebuilds of a
// 3440x1440 feature for nothing. Ten seconds is long against those and short against a player who turned the upscaler
// off and is waiting for the picture to change: the DOOM run sat 46 s with NR at present refused.
inline constexpr uint64_t kAfterUpscaleIdleMs = 10000;

// The reasons, one text each, so a log line can count them.
inline constexpr const char kHeldByPresentHost[] =
    "the Vulkan present host holds the model; it stands down for this route at its next present";
inline constexpr const char kAfterUpscaleOwns[] =
    "the after-upscale route (the game's own upscaler) owns the model in this process";
inline constexpr const char kAfterUpscaleWants[] =
    "the after-upscale route (the game's own upscaler) asked for the model within the last half second";
inline constexpr const char kAfterUpscaleWaiting[] =
    "the after-upscale route (the game's own upscaler) was refused the model and has not taken it yet";
inline constexpr const char kOtherDevice[] = "the model is in use on another VkDevice of this process";
inline constexpr const char kAfterUpscaleRetiring[] =
    "the after-upscale route went quiet; the model is being handed back to the present host";

enum class HandBack : uint8_t
{
    Done,        // the route's objects may be freed: the token is released
    Cancelled,   // the route asked again after the hand-back began: it keeps the model
    NotRetiring, // no hand-back was begun
};

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

        // Recording again while its objects are being retired (on that device or, having moved, on another): the
        // hand-back is off, the route keeps what it has.
        if (_retiring)
            _retireCancelled = true;

        if (_owner == Who::PresentHost)
        {
            _waiting = true; // the host stands down for it, and stays down until it takes the model
            return { false, kHeldByPresentHost };
        }

        _owner = Who::AfterUpscale;
        _device = device;
        _recording = true;
        _waiting = false;
        return { true, nullptr };
    }

    // The route returned from an evaluate it was granted: what it recorded is in the game's command buffer, and the
    // idle window starts now, not at the ask (a cold feature build inside one evaluate can take a while).
    void AfterUpscaleDone(uint64_t nowMs)
    {
        if (!_recording)
            return;

        _recording = false;
        _askedMs = nowMs;
    }

    Verdict PresentHost(uint64_t device, uint64_t nowMs) const
    {
        if (_owner == Who::AfterUpscale)
            return { false, _retiring ? kAfterUpscaleRetiring : kAfterUpscaleOwns };
        if (AfterUpscaleLive(nowMs))
            return { false, kAfterUpscaleWants };
        if (AfterUpscaleWaiting(nowMs))
            return { false, kAfterUpscaleWaiting };
        if (_owner == Who::PresentHost && _device != device)
            return { false, kOtherDevice };
        return { true, nullptr };
    }

    // The present host is about to record a frame on `device`: the token is taken. Only after PresentHost granted.
    void Take(uint64_t device)
    {
        _owner = Who::PresentHost;
        _device = device;
        _waiting = false; // granted: a route still waiting had gone quiet for kAfterUpscaleIdleMs
    }

    // The present host holds the model and another route wants it: stop recording, drain, release. After a refusal it
    // yields with no time limit (the idle window bounds only the host's next claim): a host still holding the model
    // has frames not yet seen complete, and gives it up as soon as they are.
    bool PresentHostMustYield(uint64_t device, uint64_t nowMs) const
    {
        return _owner == Who::PresentHost && _device == device && (AfterUpscaleLive(nowMs) || _waiting);
    }

    // Why the present host must yield, for its log: the route asked within the live window, or was refused and waits.
    const char* YieldReason(uint64_t nowMs) const
    {
        return AfterUpscaleLive(nowMs) ? kAfterUpscaleWants : kAfterUpscaleWaiting;
    }

    void Release(Who who, uint64_t device)
    {
        if (_owner == who && _device == device)
        {
            _owner = Who::None;
            _device = 0;
        }
    }

    // The device whose after-upscale model may be handed back now: the route owns the model, no hand-back is under
    // way, and it has not asked for kAfterUpscaleIdleMs. 0 otherwise.
    uint64_t IdleAfterUpscaleDevice(uint64_t nowMs) const
    {
        if (_owner != Who::AfterUpscale || _retiring || _recording || !_asked || nowMs < _askedMs ||
            nowMs - _askedMs < kAfterUpscaleIdleMs)
            return 0;
        return _device;
    }

    // The present host is about to drain `device`'s queues to take the model back. False when it may not (the route
    // asked since, or holds the model on another device): nothing is to be drained then.
    bool BeginHandBack(uint64_t device, uint64_t nowMs)
    {
        if (IdleAfterUpscaleDevice(nowMs) == 0 || _device != device)
            return false;

        _retiring = true;
        _retireDevice = device;
        _retireCancelled = false;
        return true;
    }

    // The drain of `device` is over, with the route's lock held (so it is not recording): Done releases the token, and
    // the caller frees the route's objects; Cancelled leaves everything with the route.
    HandBack FinishHandBack(uint64_t device)
    {
        if (!_retiring || _retireDevice != device)
            return HandBack::NotRetiring;

        _retiring = false;
        _retireDevice = 0;

        // Not asked since the hand-back began, so the route still holds the model on this device.
        if (_retireCancelled || _owner != Who::AfterUpscale || _device != device)
        {
            _retireCancelled = false;
            return HandBack::Cancelled;
        }

        _owner = Who::None;
        _device = 0;
        return HandBack::Done;
    }

    // The drain of `device` could not finish (a failed drain submit, the device lost or destroyed under it): the route
    // keeps the model. Nothing for a hand-back of another device.
    void CancelHandBack(uint64_t device)
    {
        if (!_retiring || _retireDevice != device)
            return;

        _retiring = false;
        _retireDevice = 0;
        _retireCancelled = false;
    }

    // `device` is being destroyed: whoever held the model on it gives it back, and a hand-back from it is off. Without
    // this a route that held the model on a device the game replaced, and never asked again, would hold it for good:
    // the present host on the new device never sees the old one idle.
    void DeviceGone(uint64_t device)
    {
        // A hand-back draining it is over, whoever holds the model now (the route may have moved to another device).
        CancelHandBack(device);

        if (_owner == Who::None || _device != device)
            return;

        _owner = Who::None;
        _device = 0;
    }

    Who Owner() const { return _owner; }
    uint64_t Device() const { return _device; }
    bool Retiring() const { return _retiring; }
    bool Recording() const { return _recording; }

  private:
    bool AfterUpscaleLive(uint64_t nowMs) const
    {
        return _asked && nowMs >= _askedMs && nowMs - _askedMs < kAfterUpscaleLiveMs;
    }

    // Refused for the present host, not granted since, and not quiet for kAfterUpscaleIdleMs (a clock read out of order
    // counts as not quiet).
    bool AfterUpscaleWaiting(uint64_t nowMs) const
    {
        return _waiting && (nowMs < _askedMs || nowMs - _askedMs < kAfterUpscaleIdleMs);
    }

    Who _owner = Who::None;
    uint64_t _device = 0;
    bool _asked = false;
    uint64_t _askedMs = 0;
    bool _waiting = false;         // the route was refused for the present host and has not taken the model since
    bool _recording = false;       // the route is inside an evaluate it was granted
    bool _retiring = false;        // a hand-back from the route is under way
    uint64_t _retireDevice = 0;    // ... draining this device
    bool _retireCancelled = false; // ... and the route asked since it began
};
} // namespace DlssNr::VkModelOwner
