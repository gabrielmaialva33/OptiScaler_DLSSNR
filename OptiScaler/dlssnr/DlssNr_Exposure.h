#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNr::Exposure
{
// Match the live shader's valid sample interval. Invalid readings never become a held value.
inline bool ValidSample(float value) { return std::isfinite(value) && value > 1e-6f && value < 1e6f; }

inline float PreExposure(float value) { return std::isfinite(value) && value > 1e-6f ? value : 1.0f; }

inline float Trim(float value) { return std::isfinite(value) ? std::clamp(value, 0.01f, 4.0f) : 1.0f; }

inline bool MeterWanted(uint32_t source, bool usableTexture, bool holdingColor)
{
    return source == 1 && usableTexture && !holdingColor;
}

inline bool LiveWanted(uint32_t source, bool hdr, bool usableTexture, bool holdingColor, bool latched = false)
{
    return !latched && hdr && MeterWanted(source, usableTexture, holdingColor);
}

inline bool InvalidateOnSourceChange(bool wasGameExposure, uint32_t source) { return source == 1 && !wasGameExposure; }

// Keep this fork's preExposure / exposure * trim convention and operation order.
// This deliberately does not import the different GameWhite/ExposureScale law from PR #14.
inline float WhitePoint(uint32_t source, bool hdr, float slider, float trim, float heldExposure, float preExposure,
                        float anchored, bool latched = false)
{
    const float fallback = std::isfinite(slider) && slider > 0.0f ? slider : 1.0f;
    if (!hdr)
        return fallback;
    if (source == 2 && std::isfinite(anchored) && anchored > 0.0f)
        return anchored;
    if (source == 1 && !latched && ValidSample(heldExposure))
        return std::clamp(PreExposure(preExposure) / heldExposure * Trim(trim), 0.01f, 4096.0f);
    return fallback;
}

enum class LatchReason : uint8_t
{
    None = 0,
    BothEnds,
};

// Plausibility latch for WhitePointSource=1.
// Watches the implied white point (PreExposure / exposure, without the user's trim and before the 0.01..4096 clamp)
// of accepted readings. Control published an exposure that jumped between 0.00025 and 116 within seconds, which
// put the white point on the 0.01 floor one moment and the 4096 ceiling the next. Once a game's exposure has been
// implausible at both ends, the latch holds for the session and DLSS-NR uses paper white instead.
//
// A test of how far the white point moves was tried first (more than 8 stops within 120 readings, as Skynizz's
// fork does) and latched in Crimson Desert, whose eye adaptation took the white point from 0.28 to 303 within 120
// readings during a scene change. A real exposure moves a long way; it does not leave the plausible range on both
// sides. One end alone is not enough either: a loading screen or a black frame can push the exposure to an extreme
// for a moment without the value being meaningless.
struct PlausibilityLatch
{
    // Implied white points outside [0.02, 512] are not a scene a game exposes for. Crimson Desert, which
    // adapts hard, stays within 0.10..307.
    static constexpr float kMinImpliedWhite = 0.02f;
    static constexpr float kMaxImpliedWhite = 512.0f;

    // Readings needed beyond EACH end. Two, so neither one wild frame nor one excursion at each end latches it.
    static constexpr uint32_t kPerEnd = 2;

    bool latched = false;
    LatchReason reason = LatchReason::None;
    uint32_t lowCount = 0;
    uint32_t highCount = 0;
    float lowest = 0.0f;
    float highest = 0.0f;

    void Reset() { *this = PlausibilityLatch {}; }

    // Feed an implied white point reading (PreExposure / exposure, before the trim and the 0.01..4096 clamp).
    // Returns true on the reading that latches it, so the caller can log once.
    bool Observe(float impliedWhite)
    {
        if (latched || !std::isfinite(impliedWhite) || impliedWhite <= 0.0f)
            return false;

        if (impliedWhite < kMinImpliedWhite)
        {
            ++lowCount;
            lowest = lowest == 0.0f ? impliedWhite : std::min(lowest, impliedWhite);
        }
        else if (impliedWhite > kMaxImpliedWhite)
        {
            ++highCount;
            highest = std::max(highest, impliedWhite);
        }
        else
        {
            return false;
        }

        if (lowCount >= kPerEnd && highCount >= kPerEnd)
        {
            latched = true;
            reason = LatchReason::BothEnds;
            return true;
        }

        return false;
    }

    bool Latched() const { return latched; }
    LatchReason Reason() const { return reason; }
    uint32_t LowCount() const { return lowCount; }
    uint32_t HighCount() const { return highCount; }
    float Lowest() const { return lowest; }
    float Highest() const { return highest; }
};
} // namespace DlssNr::Exposure
