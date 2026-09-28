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
struct Slot
{
    std::mutex mutex;
    uint64_t baseFrame = 0;
    uint64_t publishedFrame = 0;
    bool published = false;
    ID3D12Device* device = nullptr;
    Field field {};
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
