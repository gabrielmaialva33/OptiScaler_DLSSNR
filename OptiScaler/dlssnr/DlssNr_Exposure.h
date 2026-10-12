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
    OutOfRange,
    Swinging,
};

// Plausibility latch for WhitePointSource=1.
// Monitors the implied white point (PreExposure / exposure, without the user's trim and before the 0.01..4096
// clamp) of accepted readings. If a game publishes unusable exposure values (e.g. Control swinging from 0.00025 to 116
// within seconds, flashing between black and blown out), this latches unreliable for the session so DLSS-NR
// falls back to paper white rather than corrupting the image.
struct PlausibilityLatch
{
    // The plausible range of implied white point values (PreExposure / exposure).
    // In Control, the game's exposure moved from 0.00025 to 116 within seconds, implying white points
    // hitting the 0.01 floor and 4096 ceiling. A legitimate scene linear white point sits within [0.02, 512.0].
    static constexpr float kMinImpliedWhite = 0.02f;
    static constexpr float kMaxImpliedWhite = 512.0f;

    // Number of out-of-range readings required before latching. A single wild reading must never
    // latch it; 3 non-consecutive readings confirm that the game's exposure cannot be trusted.
    static constexpr uint32_t kOutOfRangeThreshold = 3;

    // Window size (~2 seconds at 60 fps) across which dynamic range swing is tracked.
    static constexpr size_t kWindowSize = 120;

    // Maximum plausible dynamic range swing in photographic stops (log2) over 120 readings.
    // 8 stops corresponds to a 256x luminance swing within ~2 s, which exceeds legitimate scene
    // transitions and indicates an oscillating or uncalibrated exposure buffer.
    static constexpr float kMaxStopsSpan = 8.0f;

    bool latched = false;
    LatchReason reason = LatchReason::None;
    uint32_t outOfRangeCount = 0;

    float window[kWindowSize] = {};
    size_t head = 0;
    size_t count = 0;

    // Diagnostics captured at the moment of latching for one-shot logging.
    float minSpan = 0.0f;
    float maxSpan = 0.0f;
    float stopsSpan = 0.0f;
    uint32_t readingsSpan = 0;
    float lastImpliedWhite = 0.0f;

    void Reset()
    {
        latched = false;
        reason = LatchReason::None;
        outOfRangeCount = 0;
        head = 0;
        count = 0;
        minSpan = 0.0f;
        maxSpan = 0.0f;
        stopsSpan = 0.0f;
        readingsSpan = 0;
        lastImpliedWhite = 0.0f;
        for (float& v : window)
            v = 0.0f;
    }

    // Feed an implied white point reading (PreExposure / exposure, before the trim and the 0.01..4096 clamp).
    // Returns true on the exact frame it latches so the caller can log once at WARN.
    bool Observe(float impliedWhite)
    {
        if (latched)
            return false;

        if (!std::isfinite(impliedWhite) || impliedWhite <= 0.0f)
            return false;

        lastImpliedWhite = impliedWhite;

        // Test 1: Out-of-range readings. Counted across the session; do not need to be consecutive.
        // A single wild reading must never latch it.
        if (impliedWhite < kMinImpliedWhite || impliedWhite > kMaxImpliedWhite)
        {
            ++outOfRangeCount;
            if (outOfRangeCount >= kOutOfRangeThreshold)
            {
                latched = true;
                reason = LatchReason::OutOfRange;
                return true;
            }
        }
        else
        {
            // Test 2: Swinging within the last 120 readings.
            window[head] = impliedWhite;
            head = (head + 1) % kWindowSize;
            if (count < kWindowSize)
                ++count;

            if (count > 1)
            {
                float minVal = window[0];
                float maxVal = window[0];
                for (size_t i = 1; i < count; ++i)
                {
                    minVal = std::min(minVal, window[i]);
                    maxVal = std::max(maxVal, window[i]);
                }

                if (minVal > 0.0f)
                {
                    const float stops = std::log2(maxVal) - std::log2(minVal);
                    if (stops > kMaxStopsSpan)
                    {
                        latched = true;
                        reason = LatchReason::Swinging;
                        minSpan = minVal;
                        maxSpan = maxVal;
                        stopsSpan = stops;
                        readingsSpan = static_cast<uint32_t>(count);
                        return true;
                    }
                }
            }
        }

        return false;
    }

    bool Latched() const { return latched; }
    LatchReason Reason() const { return reason; }
    uint32_t OutOfRangeCount() const { return outOfRangeCount; }
    float MinSpan() const { return minSpan; }
    float MaxSpan() const { return maxSpan; }
    float StopsSpan() const { return stopsSpan; }
    uint32_t ReadingsSpan() const { return readingsSpan; }
    float LastReading() const { return lastImpliedWhite; }
};
} // namespace DlssNr::Exposure
