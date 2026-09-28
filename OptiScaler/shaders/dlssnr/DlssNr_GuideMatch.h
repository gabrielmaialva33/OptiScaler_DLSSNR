#pragma once

#include <cstdint>

// The guides the model is handed below its frame size, and the motion-vector scale that goes with
// them. design/reduced-scale-guides.md.
//
// Both rules are jlrouzies-fr's, found and fixed in v0.8.92 of wilsjo2's
// OptiScaler-DLSSNR-PreSR-Multipass (GPL-3.0): this tree's reduced path had the same shape that
// fork's had, and so the same two faults.
//
//   1. Below native the colour shrinks to the working size and the guides did not. Nothing documents
//      that the model resamples a guide larger than its input, and the picture says it does not: each
//      pixel's depth and motion described some other place, history never lined up, and the frame
//      flickered and kept "settling" after the camera stopped.
//   2. The model reads its motion-vector scale in pixels of the motion texture it is handed. The game's
//      scale turns its vectors into pixels of the size they are measured in: the render size for
//      low-resolution vectors, the output size otherwise. Converting with working / frame shrank every
//      low-resolution vector below native, however the guides were handed over.
//
// Pure functions of sizes and flags, so the host suite can pin them without a device.
namespace DlssNr::GuideMatch
{
// The size the game's motion-vector scale converts its vectors into pixels of.
constexpr uint32_t MotionReference(bool lowResolutionVectors, uint32_t renderExtent, uint32_t outputExtent)
{
    return lowResolutionVectors ? renderExtent : outputExtent;
}

// The scale the model is given: the game's, re-expressed in pixels of the motion texture handed over.
constexpr float ModelMotionScale(float gameScale, uint32_t handedExtent, uint32_t referenceExtent)
{
    return referenceExtent != 0 ? gameScale * (float) handedExtent / (float) referenceExtent : gameScale;
}

// The conversion this tree used before, kept for the A/B key: working size over frame size, whatever
// texture the model was actually reading.
constexpr float LegacyMotionScale(float gameScale, uint32_t workExtent, uint32_t frameExtent)
{
    return frameExtent != 0 ? gameScale * (float) workExtent / (float) frameExtent : gameScale;
}

// Whether depth and motion are resampled to the working size before the model sees them: only when a
// guide is larger than the model's input along some axis. A guide smaller than the input is the
// upscaler's ordinary contract -- render resolution under display resolution -- which the model has
// always taken as a subrect, and a point resample up would only throw information away.
constexpr bool Wanted(bool enabled, bool available, uint32_t workWidth, uint32_t workHeight, uint32_t depthWidth,
                      uint32_t depthHeight, uint32_t motionWidth, uint32_t motionHeight)
{
    if (!enabled || !available || workWidth == 0 || workHeight == 0)
        return false;

    return depthWidth > workWidth || depthHeight > workHeight || motionWidth > workWidth || motionHeight > workHeight;
}

// The texel of a guide region a working-size texel reads: its centre mapped into the region, floored.
// The shader computes the same thing; this is the one the tests hold it to.
constexpr uint32_t PointSource(uint32_t destination, uint32_t destinationExtent, uint32_t regionBase,
                               uint32_t regionExtent)
{
    if (destinationExtent == 0 || regionExtent == 0)
        return regionBase;

    const uint64_t scaled = ((uint64_t) destination * 2 + 1) * regionExtent / ((uint64_t) destinationExtent * 2);
    return regionBase + (uint32_t) (scaled < regionExtent ? scaled : regionExtent - 1);
}
} // namespace DlssNr::GuideMatch
