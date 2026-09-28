#pragma once

// One synthesized motion field per base frame, whoever computes it. DLSS-NR's present hosts and
// synthesized frame generation both want the estimator's field for the same frame; this slot lets the
// first of them publish it and the second take it instead of running the estimator again. Design:
// dlssnr/design/synthesized-frame-generation.md, "Computing the field once when NR and FG both want it".
//
// Header only and free of any module: frame generation must not depend on DLSS-NR, and DLSS-NR must
// stay removable as one block. It stores pointers and compares them; it never touches the GPU.
//
// - The transport that owns a base frame (the FG present hook on native D3D12, the D3D11 bridge's copy)
//   calls BeginBaseFrame() once, before any consumer of that frame runs.
// - A consumer that recorded an estimate publishes its field, which must be real (the estimator's
//   Ready()) and must stay valid until the consumer's next Record on that estimator.
// - The other consumer takes it when the base frame, device and extent match. A consumer never takes
// its own field back, which also covers a route with no transport, where the frame never advances.
// - An owner withdraws its field before releasing the estimator behind it.
// - A consumer whose estimator recorded this base frame but is still warming up (no real field yet)
//   announces that instead. The other consumer, if it has no estimator of its own, waits for it --
//   zero motion meanwhile, which is also all a second estimator would give over the same frames --
//   rather than build one that would then sit idle once the first starts publishing. Bounded by
//   kMaxPeerWaitFrames.

#include <cstdint>
#include <mutex>

struct ID3D12Device;
struct ID3D12Resource;

namespace SynthMotion::Handoff
{
enum class Owner : uint8_t
{
    None,
    DlssNr,
    FrameGen,
};

struct Field
{
    ID3D12Resource* motion = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool sceneCut = false;
    Owner owner = Owner::None;
};

namespace detail
{
struct Warming
{
    bool announced = false;
    uint64_t frame = 0;
    Owner owner = Owner::None;
    ID3D12Device* device = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Slot
{
    std::mutex mutex;
    uint64_t baseFrame = 0;
    uint64_t publishedFrame = 0;
    bool published = false;
    ID3D12Device* device = nullptr;
    Field field {};
    Warming warming {};
};

inline Slot& Get()
{
    static Slot slot;
    return slot;
}
} // namespace detail

inline void BeginBaseFrame()
{
    auto& slot = detail::Get();
    std::lock_guard lock(slot.mutex);
    ++slot.baseFrame;
}

inline uint64_t CurrentBaseFrame()
{
    auto& slot = detail::Get();
    std::lock_guard lock(slot.mutex);
    return slot.baseFrame;
}

inline void Publish(Owner owner, ID3D12Device* device, ID3D12Resource* motion, uint32_t width, uint32_t height,
                    bool sceneCut)
{
    if (owner == Owner::None || device == nullptr || motion == nullptr || width == 0 || height == 0)
        return;

    auto& slot = detail::Get();
    std::lock_guard lock(slot.mutex);
    slot.published = true;
    slot.publishedFrame = slot.baseFrame;
    slot.device = device;
    slot.field = Field { motion, width, height, sceneCut, owner };
}

// This base frame's field, published by another consumer on the same device at the same extent.
inline bool Take(Owner taker, ID3D12Device* device, uint32_t width, uint32_t height, Field* out)
{
    auto& slot = detail::Get();
    std::lock_guard lock(slot.mutex);

    if (!slot.published || slot.publishedFrame != slot.baseFrame || slot.field.owner == taker ||
        slot.device != device || slot.field.width != width || slot.field.height != height ||
        slot.field.motion == nullptr)
    {
        return false;
    }

    if (out != nullptr)
        *out = slot.field;

    return true;
}

// This base frame, this consumer's estimator is warming up: it recorded, and will publish once it has a
// real field. Only a consumer that will publish may say so -- one whose fields never go into the slot
// (DLSS-NR's optical-flow engine, a frame late) would be waited for in vain.
inline void AnnounceWarming(Owner owner, ID3D12Device* device, uint32_t width, uint32_t height)
{
    if (owner == Owner::None || device == nullptr || width == 0 || height == 0)
        return;

    auto& slot = detail::Get();
    std::lock_guard lock(slot.mutex);
    slot.warming = detail::Warming { true, slot.baseFrame, owner, device, width, height };
}

// Whether another consumer announced a warm-up for this base frame, on this device, at this extent. Like
// Take, never the asker's own announcement, and never one from an earlier base frame: on a route with no
// transport the frame never advances, and there the asker is the only consumer anyway.
inline bool PeerWarming(Owner asker, ID3D12Device* device, uint32_t width, uint32_t height)
{
    auto& slot = detail::Get();
    std::lock_guard lock(slot.mutex);
    const auto& warming = slot.warming;

    return warming.announced && warming.frame == slot.baseFrame && warming.owner != asker && warming.device == device &&
           warming.width == width && warming.height == height;
}

// When the estimator behind an announcement is released. An announcement only outlives its base frame
// where the frame stops advancing (the transport gone), and nobody should wait there for an estimator
// that no longer exists.
inline void WithdrawWarming(Owner owner)
{
    auto& slot = detail::Get();
    std::lock_guard lock(slot.mutex);

    if (slot.warming.owner == owner)
        slot.warming = detail::Warming {};
}

// Frames closer together than this carry an estimator's history. After a longer gap both consumers'
// staleness rules reset theirs (kSynthMotionStaleMs in DLSS-NR, kMotionStaleMs in the FG input).
inline constexpr long long kPeerWaitFreshMs = 250;

// How many fresh frames in a row a consumer waits for a warming peer: a warm-up is five (FFX), and
// this leaves room for slow start-up frames. A peer still warming after that is stuck, and the consumer
// builds its own. A stale gap starts the count over, because it restarted the peer's warm-up too. Over
// a slow stretch -- a loading screen at a frame every second or two -- every estimator is reset each
// frame, the consumer's own included, so waiting there costs nothing and building would give nothing.
// A bound in milliseconds got exactly that wrong (Divinity, 2026-09-28).
inline constexpr uint32_t kMaxPeerWaitFrames = 16;

// One consumer's wait, its own state. Put back to {} when it takes a field or releases.
struct PeerWait
{
    long long sinceMs = -1; // when the wait began; -1 when not waiting
    long long lastMs = -1;
    uint32_t freshFrames = 0;
};

// A consumer that took nothing this base frame asks whether to record nothing and wait for a warming
// peer, rather than build an estimator of its own. A consumer that already has an estimator keeps using
// it, since it is warm or warming already.
inline bool WaitForPeer(PeerWait& wait, bool haveEstimator, bool peerWarming, long long nowMs)
{
    if (haveEstimator || !peerWarming)
        return false;

    if (wait.sinceMs < 0)
        wait.sinceMs = nowMs;
    else if (nowMs - wait.lastMs <= kPeerWaitFreshMs)
        ++wait.freshFrames;
    else
        wait.freshFrames = 0;

    wait.lastMs = nowMs;
    return wait.freshFrames <= kMaxPeerWaitFrames;
}

// Before the resource behind motion is released. Anything else published stays.
inline void Withdraw(ID3D12Resource* motion)
{
    if (motion == nullptr)
        return;

    auto& slot = detail::Get();
    std::lock_guard lock(slot.mutex);

    if (slot.field.motion == motion)
    {
        slot.published = false;
        slot.device = nullptr;
        slot.field = Field {};
    }
}
} // namespace SynthMotion::Handoff
