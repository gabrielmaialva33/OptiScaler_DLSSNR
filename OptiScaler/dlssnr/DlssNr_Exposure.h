#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNr::Exposure
{
// Match the live shader's valid sample interval. Invalid readings never become a held value.
inline bool ValidSample(float value)
{
    return std::isfinite(value) && value > 1e-6f && value < 1e6f;
}

inline float PreExposure(float value)
{
    return std::isfinite(value) && value > 1e-6f ? value : 1.0f;
}

inline float Trim(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.25f, 4.0f) : 1.0f;
}

inline bool MeterWanted(uint32_t source, bool usableTexture, bool holdingColor)
{
    return source == 1 && usableTexture && !holdingColor;
}

inline bool LiveWanted(uint32_t source, bool hdr, bool usableTexture, bool holdingColor)
{
    return hdr && MeterWanted(source, usableTexture, holdingColor);
}

inline bool InvalidateOnSourceChange(bool wasGameExposure, uint32_t source)
{
    return source == 1 && !wasGameExposure;
}

// Keep this fork's preExposure / exposure * trim convention and operation order.
// This deliberately does not import the different GameWhite/ExposureScale law from PR #14.
inline float WhitePoint(uint32_t source, bool hdr, float slider, float trim,
                        float heldExposure, float preExposure, float anchored)
{
    const float fallback = std::isfinite(slider) && slider > 0.0f ? slider : 1.0f;
    if (!hdr)
        return fallback;
    if (source == 2 && std::isfinite(anchored) && anchored > 0.0f)
        return anchored;
    if (source == 1 && ValidSample(heldExposure))
        return std::clamp(PreExposure(preExposure) / heldExposure * Trim(trim), 0.01f, 4096.0f);
    return fallback;
}
}
