#pragma once

// Peripheral compression's layout: from two settings, the frame and the working scale, to the packed extent
// the model works at and the constants the pack and unpack read. dlssnr/design/peripheral-compression.md.
//
// Two origins, both stated where they apply:
//   - Axis and BuildAxis are ported verbatim from PeripheralWarp ABI v2 at 64902dd6 (Copyright (c) 2026
//     Yuri Grib (BeliyG3), MIT; Licenses/PeripheralWarp_LICENSE.txt), through wilsjo2's SpatialWarp.h.
//   - EvenExtent and the validity rules in Build are ported from wilsjo2's
//     OptiScaler-DLSSNR-PreSR-Multipass v0.8.91, OptiScaler/shaders/dlssnr/DlssNr_Spatial.h (GPL-3.0).
//     v1 has one centre and one work value for both axes and no offset or shift, so its MaxCenterOffset,
//     MinimumWorkPercent and WorkShiftLimits are not ported: nothing here could reach them.
//
// Pure functions of sizes and settings, so tests/nr-periphery pins them without a device. The per-pixel
// mapping is precompile/dlssnr_periphery_warp.h, which the shader includes too.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "DlssNr_GuideMatch.h"

#ifndef PW_FN
#define PW_FN inline
#define PW_ABS(v) std::fabs(v)
#define PW_FLOOR(v) std::floor(v)
#define PW_CEIL(v) std::ceil(v)
#define PW_MINF(a, b) std::min<float>((a), (b))
#define PW_MAXF(a, b) std::max<float>((a), (b))
#define PW_MINI(a, b) std::min<int>((a), (b))
#define PW_MAXI(a, b) std::max<int>((a), (b))
#endif

#include "precompile/dlssnr_periphery_warp.h"

namespace DlssNr::Periphery
{
// What the settings may be. The centre band and the work extent are percent of each axis.
constexpr uint32_t kCenterMin = 10;
constexpr uint32_t kCenterMax = 96;
constexpr uint32_t kWorkMin = 55;
constexpr uint32_t kWorkMax = 99;

// Why the feature is not running, as the menu and the log say it. Literals, so a status can carry one and
// the menu can translate it.
constexpr const char* kReasonOff = "switched off";
constexpr const char* kReasonActive = "active";
constexpr const char* kReasonFrame = "the frame is too small to lay out";
constexpr const char* kReasonSupersampling =
    "supersampling (Model resolution above 100) is not supported with peripheral compression";
// "Centre band" and "packed extent" are the menu's names for PeripheryCenter and PeripheryWork.
constexpr const char* kReasonRange =
    "the centre band must be at least 10 and smaller than the packed extent, which must be below 100";
constexpr const char* kReasonCompression =
    "the edges would be squeezed below half their size, which aliases: widen the packed extent or narrow the "
    "centre band (packed extent x 2 must be at least 100 + centre band)";
constexpr const char* kReasonTooSmall =
    "the packed model would be under a quarter of the frame: raise Model resolution or the packed extent";
constexpr const char* kReasonOneTexel = "less than one packed pixel would be left on a side of the centre band";
// Refusals the pass itself makes, before or after the layout.
constexpr const char* kReasonBeforeUpscale = "not on the before-upscale stage";
constexpr const char* kReasonUseProxy = "not with the driver proxy (UseProxy)";
constexpr const char* kReasonNoShader = "this build has no pack/unpack shader";
constexpr const char* kReasonPassFailed = "the pack/unpack pass could not be built or run on this device";

struct Settings
{
    bool enabled = false;
    uint32_t center = 80; // full-density band, percent of each axis
    uint32_t work = 90;   // the packed input's extent before the working scale, percent of each axis

    bool operator==(const Settings&) const = default;
};

// -------------------------------------------------------------------------------------------------------
// Ported verbatim from PeripheralWarp (MIT), as adapted in wilsjo2's external/peripheral_warp/SpatialWarp.h.
// Copyright (c) 2026 Yuri Grib (BeliyG3).
// -------------------------------------------------------------------------------------------------------

struct Axis
{
    float nativeExtent = 0, rawExtent = 0, workExtent = 0;
    float bandCenter = 0, workCenter = 0, halfBand = 0, scale = 1;
    float halfSpan[2] = {}, periphery[2] = {}, allotted[2] = {};
    float center[2] = {}, work[2] = {}, compression[2] = {}, edgeSlope[2] = {};
};

// Split the peripheral budget between the two sides, respecting the native side extent.
inline Axis BuildAxis(uint32_t native, uint32_t rawWork, uint32_t model, float centerFraction, float offsetFraction,
                      float shiftFraction)
{
    Axis a {};
    a.nativeExtent = static_cast<float>(native);
    a.rawExtent = static_cast<float>(rawWork);
    a.workExtent = static_cast<float>(model);
    a.scale = a.workExtent / a.rawExtent;
    a.halfBand = centerFraction * a.nativeExtent * 0.5f;
    a.bandCenter = (0.5f + offsetFraction) * a.nativeExtent;
    a.halfSpan[0] = a.bandCenter;
    a.halfSpan[1] = a.nativeExtent - a.bandCenter;
    for (int i = 0; i != 2; ++i)
        a.periphery[i] = std::max(0.0f, a.halfSpan[i] - a.halfBand);
    const float budget = std::max(0.0f, a.rawExtent - 2.0f * a.halfBand);
    const int narrow = a.periphery[0] <= a.periphery[1] ? 0 : 1;
    const int wide = 1 - narrow;
    float base[2] = {};
    base[narrow] = std::min(0.5f * budget, a.periphery[narrow]);
    base[wide] = std::min(budget - base[narrow], a.periphery[wide]);
    const float minShift = -std::max(0.0f, std::min(base[1], a.periphery[0] - base[0]));
    const float maxShift = std::max(0.0f, std::min(base[0], a.periphery[1] - base[1]));
    const float shift = std::clamp(shiftFraction * a.nativeExtent, minShift, maxShift);
    a.allotted[0] = std::clamp(base[0] - shift, 0.0f, a.periphery[0]);
    a.allotted[1] = std::clamp(base[1] + shift, 0.0f, a.periphery[1]);
    if (rawWork == model)
    {
        // At 100% global scale the centre band should translate by whole texels.
        const float translation = a.halfBand + a.allotted[0] - a.bandCenter;
        const float candidates[3] = { std::round(translation), std::floor(translation), std::ceil(translation) };
        for (float candidate : candidates)
        {
            const float workCenter = a.bandCenter + candidate;
            const float left = workCenter - a.halfBand;
            const float right = a.rawExtent - workCenter - a.halfBand;
            if (left < -1e-3f || right < -1e-3f || left > a.periphery[0] + 1e-3f || right > a.periphery[1] + 1e-3f)
                continue;
            a.allotted[0] = std::clamp(left, 0.0f, a.periphery[0]);
            a.allotted[1] = std::clamp(right, 0.0f, a.periphery[1]);
            break;
        }
    }
    for (int i = 0; i != 2; ++i)
    {
        a.center[i] = a.halfBand / a.halfSpan[i];
        a.work[i] = (a.halfBand + a.allotted[i]) / a.halfSpan[i];
        a.compression[i] = a.periphery[i] > 0 ? a.allotted[i] / a.periphery[i] : 1.0f;
        a.edgeSlope[i] = a.compression[i] * a.compression[i];
    }
    a.workCenter = (a.halfBand + a.allotted[0]) * a.scale;
    return a;
}

// --- end of the MIT port ------------------------------------------------------------------------------

// The axis as the shader reads it.
inline PeripheryAxis ShaderAxis(const Axis& a)
{
    PeripheryAxis s {};
    s.bandCenter = a.bandCenter;
    s.workCenter = a.workCenter;
    s.scale = a.scale;
    s.halfSpanNeg = a.halfSpan[0];
    s.halfSpanPos = a.halfSpan[1];
    s.centerNeg = a.center[0];
    s.centerPos = a.center[1];
    s.workNeg = a.work[0];
    s.workPos = a.work[1];
    s.compressionNeg = a.compression[0];
    s.compressionPos = a.compression[1];
    s.edgeSlopeNeg = a.edgeSlope[0];
    s.edgeSlopePos = a.edgeSlope[1];
    return s;
}

inline float Pack(float nativePixel, const Axis& a) { return PeripheryPack(nativePixel, ShaderAxis(a)); }
inline float Unpack(float packedPixel, const Axis& a) { return PeripheryUnpack(packedPixel, ShaderAxis(a)); }

// -------------------------------------------------------------------------------------------------------
// Ported from wilsjo2's DlssNr_Spatial.h (GPL-3.0), v0.8.91.
// -------------------------------------------------------------------------------------------------------

// An extent for a fraction of the frame: rounded up to even (wilsjo2's rule; neither repository says why
// even, and it costs at most one texel), never above the frame's even floor below 100%, never below 2.
inline uint32_t EvenExtent(uint32_t native, double fraction)
{
    if (fraction == 1.0)
        return native;
    double value = static_cast<double>(native) * fraction;
    const double nearest = std::round(value);
    if (std::abs(value - nearest) <= 1e-6 * std::max(1.0, std::abs(value)))
        value = nearest;
    uint32_t result = static_cast<uint32_t>(std::ceil(value));
    if (result & 1u)
        ++result;
    if (fraction < 1.0)
        result = std::min(result, native & ~1u);
    return std::max(2u, result);
}

struct Layout
{
    Settings settings {};
    bool active = false;
    const char* reason = kReasonOff;
    uint32_t nativeW = 0, nativeH = 0;
    uint32_t rawW = 0, rawH = 0;
    uint32_t modelW = 0, modelH = 0;
    float globalScale = 1.0f;
    Axis x {}, y {};

    // The share of the uniform grid's pixels the model works on, for the log and the menu.
    double PixelShare(uint32_t gridW, uint32_t gridH) const
    {
        return gridW != 0 && gridH != 0 ? (double) modelW * modelH / ((double) gridW * gridH) : 1.0;
    }
};

// The layout for a frame at a working scale, or an inactive one that says why not.
inline Layout Build(const Settings& s, uint32_t nativeW, uint32_t nativeH, float globalScale)
{
    Layout result {};
    result.settings = s;
    result.nativeW = nativeW;
    result.nativeH = nativeH;
    result.globalScale = globalScale;

    if (!s.enabled)
        return result;

    if (nativeW < 2 || nativeH < 2 || !std::isfinite(globalScale) || globalScale < 0.25f)
    {
        result.reason = kReasonFrame;
        return result;
    }

    // The supersample legs are uniform resamplers; nothing packs or unpacks around them.
    if (globalScale > 1.0f)
    {
        result.reason = kReasonSupersampling;
        return result;
    }

    if (s.center < kCenterMin || s.center >= s.work || s.work > kWorkMax)
    {
        result.reason = kReasonRange;
        return result;
    }

    // k = (work - centre) / (100 - centre) >= 0.5. BeliyG3's own line ("compression < 0.5 may visibly
    // alias"), held as a floor in v1; it also bounds the colour pack's footprint (PERIPHERY_MAX_SPAN).
    if (2 * s.work < 100 + s.center)
    {
        result.reason = kReasonCompression;
        return result;
    }

    const double work = s.work * 0.01;
    const uint32_t rawW = EvenExtent(nativeW, work);
    const uint32_t rawH = EvenExtent(nativeH, work);
    const uint32_t modelW = EvenExtent(nativeW, static_cast<double>(globalScale) * work);
    const uint32_t modelH = EvenExtent(nativeH, static_cast<double>(globalScale) * work);

    if (modelW < nativeW * 0.25f || modelH < nativeH * 0.25f ||
        s.center * 0.01f >= static_cast<float>(rawW) / nativeW ||
        s.center * 0.01f >= static_cast<float>(rawH) / nativeH)
    {
        result.reason = kReasonTooSmall;
        return result;
    }

    result.x = BuildAxis(nativeW, rawW, modelW, s.center * 0.01f, 0.0f, 0.0f);
    result.y = BuildAxis(nativeH, rawH, modelH, s.center * 0.01f, 0.0f, 0.0f);

    // A peripheral side needs at least one raw work texel. At a shift endpoint the reference split may
    // assign zero; snapping can leave a fraction of one.
    const auto invertible = [](const Axis& axis)
    {
        return (axis.periphery[0] <= 0 || axis.allotted[0] >= 1.0f) &&
               (axis.periphery[1] <= 0 || axis.allotted[1] >= 1.0f);
    };

    if (!invertible(result.x) || !invertible(result.y))
    {
        result.reason = kReasonOneTexel;
        return result;
    }

    result.rawW = rawW;
    result.rawH = rawH;
    result.modelW = modelW;
    result.modelH = modelH;
    result.active = true;
    result.reason = kReasonActive;
    return result;
}

// --- end of the GPL-3.0 port --------------------------------------------------------------------------

// The game's motion-vector scale re-expressed in FRAME pixels, which is what the mapping moves the vector's
// ends through: measured against the size the vectors come in (the render size for low-resolution vectors,
// GuideMatch::MotionReference), converted to the frame's. wilsjo2's v0.8.91 handed the raw game scale here,
// which leaves low-resolution vectors render-pixel long -- about 0.67x too short at DLSS Quality.
constexpr float FrameMotionScale(float gameScale, uint32_t frameExtent, uint32_t referenceExtent)
{
    return GuideMatch::ModelMotionScale(gameScale, frameExtent, referenceExtent);
}

// The cbuffer of precompile/dlssnr_periphery.hlsl, in order: sixteen scalars, then the two axes at whole
// rows. alignas(256) because a constant-buffer view must be a multiple of 256 bytes, and the upload buffer
// behind it with it (native D3D12 removes the device otherwise; reduced-scale-guides.md, "Native Windows").
struct alignas(256) PeripheryConstants
{
    uint32_t OutWidth; // the dispatch: the packed extent for the packs, the uniform grid for the unpack
    uint32_t OutHeight;
    uint32_t NativeWidth; // the frame
    uint32_t NativeHeight;
    uint32_t ModelWidth; // the packed extent
    uint32_t ModelHeight;
    uint32_t DepthBaseX; // the guides' regions, as the model would otherwise have been handed them
    uint32_t DepthBaseY;
    uint32_t DepthWidth;
    uint32_t DepthHeight;
    uint32_t MotionBaseX;
    uint32_t MotionBaseY;
    uint32_t MotionWidth;
    uint32_t MotionHeight;
    float MotionScaleX; // the game's vectors -> frame pixels (FrameMotionScale)
    float MotionScaleY;
    PeripheryAxis AxisX;
    PeripheryAxis AxisY;
};

static_assert(sizeof(PeripheryAxis) == 64, "PeripheryAxis must be four whole cbuffer rows");
static_assert(offsetof(PeripheryConstants, MotionScaleY) == 60, "PeripheryConstants must match cbuffer Params");
static_assert(offsetof(PeripheryConstants, AxisX) == 64, "PeripheryConstants must match cbuffer Params");
static_assert(offsetof(PeripheryConstants, AxisY) == 128, "PeripheryConstants must match cbuffer Params");
static_assert(sizeof(PeripheryConstants) == 256, "a constant-buffer view must be a multiple of 256 bytes");

// The layout's half of the constants. The pass fills the dispatch size, the regions and the motion scale.
inline PeripheryConstants MakeConstants(const Layout& layout)
{
    PeripheryConstants c {};
    c.NativeWidth = layout.nativeW;
    c.NativeHeight = layout.nativeH;
    c.ModelWidth = layout.modelW;
    c.ModelHeight = layout.modelH;
    c.AxisX = ShaderAxis(layout.x);
    c.AxisY = ShaderAxis(layout.y);
    return c;
}
} // namespace DlssNr::Periphery
