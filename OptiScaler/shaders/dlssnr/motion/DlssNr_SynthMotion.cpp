// ===========================================================================================
// UNBUILT SKELETON. Design: OptiScaler/dlssnr/design/synthesized-motion.md.
//
// This file is NOT in OptiScaler.vcxproj and is compiled by nothing. Every method body is a
// TODO(skeleton) stub that fails closed -- the class never claims a field it did not produce, so
// wiring it in early is safe: the present host falls back to the zero field on every false answer.
// Do not add it to the build until the design note's implementation plan reaches it. The passes
// below are documented against precompile/synth_motion.hlsl, which is likewise unbuilt (no
// _Shader.h / _Shader_Vk.h generated yet).
//
// Intentionally free of "pch.h" and the Shader_Dx12 base while unbuilt, so it does not pretend to
// slot into the real translation-unit / precompiled-header rules before it is a real TU. When it is
// promoted to a build, it takes "pch.h" as its first line (CONTRIBUTING.md) and should derive its
// dispatch machinery from Shader_Dx12 the way FT_Dx12 / DlssNr_Dx12 do, rather than owning a
// bespoke root signature and descriptor ring.
// ===========================================================================================

#include "DlssNr_SynthMotion.h"

#include <algorithm>

namespace DlssNr
{

SynthMotion::~SynthMotion() { Release(); }

void SynthMotion::Release()
{
    // TODO(skeleton): release _motion, _lumaPing, _lumaPong, _sceneScore, any descriptor heaps and
    // PSOs, and zero the geometry/history flags. Callers hold these only for as long as they hold the
    // device that made them, and drain before release, exactly as ZeroGuides / PresentHost do.
    _motion = nullptr;
    _lumaPing = nullptr;
    _lumaPong = nullptr;
    _sceneScore = nullptr;
    _width = _height = _flowWidth = _flowHeight = 0;
    _hasPrevious = false;
    _producedThisFrame = false;
    _resetThisFrame = false;
    _resetOwed = true;
    _recorded = false;
}

bool SynthMotion::Ensure(ID3D12Device* device, uint32_t width, uint32_t height)
{
    (void) device;
    if (width == 0 || height == 0)
        return false;

    if (_motion != nullptr && _width == width && _height == height)
        return true;

    // A new extent shares no history with the old one.
    Release();
    return _Allocate(device, width, height);
}

bool SynthMotion::_Allocate(ID3D12Device* device, uint32_t width, uint32_t height)
{
    (void) device;

    // Estimation resolution: ~320 px wide, even, at least 64, height proportional. See guides.py:66-80
    // (the precedent NeuralScreen uses flow_width=320 and even-rounds both axes).
    constexpr uint32_t kFlowWidthTarget = 320;
    const double scale = width > kFlowWidthTarget ? (double) kFlowWidthTarget / (double) width : 1.0;
    _flowWidth = (uint32_t) std::max<uint32_t>(64, (uint32_t) ((width * scale) / 2.0 + 0.5) * 2);
    _flowHeight = (uint32_t) std::max<uint32_t>(64, (uint32_t) ((height * scale) / 2.0 + 0.5) * 2);

    // TODO(skeleton): create the working-resolution R16G16_FLOAT output (ALLOW_UNORDERED_ACCESS), the
    // two ping-pong luma textures at (_flowWidth x _flowHeight) with a 2-3 level mip chain, and the
    // small scene-score buffer. Create PSOs from precompile/synth_motion.hlsl. Set _resetOwed = true;
    // _hasPrevious = false. Return false and Release() on any failure (fail closed).
    _width = width;
    _height = height;
    _resetOwed = true;
    _hasPrevious = false;
    return false; // not built
}

bool SynthMotion::Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                         D3D12_RESOURCE_STATES sourceState, uint32_t width, uint32_t height, bool reset)
{
    (void) sourceState;
    _producedThisFrame = false;
    _resetThisFrame = false;
    _recorded = false;

    if (device == nullptr || cmdList == nullptr || source == nullptr)
        return false;

    if (!Ensure(device, width, height))
        return false;

    // The caller's own reset (first host frame, resize) OR anything owed from a dropped recording.
    const bool resetNow = reset || _resetOwed;

    // Sequence, all onto cmdList, all on the caller's queue (see synth_motion.hlsl):
    //   1. luma + pyramid  : source -> current luma pyramid (ping)
    //   2. scene score     : mean abs luma diff (ping vs previous pong) -> _sceneScore
    //                        > cut threshold  -> reset, emit zero
    //                        < static floor   -> emit zero, no search (cheapest path)
    //   3. block match     : coarse-to-fine SAD search, ping vs pong, sub-pixel parabola
    //   4. validate/upscale: noise floor, scale to working px, bilinear upscale -> _motion
    // On first frame / reset / static: write zero into _motion and set _producedThisFrame per policy
    // (a zero field with Reset is still a legitimate produced field; a first frame with no previous is
    // not -- caller falls back to the zero guide, which is identical). Leave _motion in the rest state
    // GuideRestState(true) names, so the pass transitions it from where it actually is.
    //
    // TODO(skeleton): record the four passes. Set _resetThisFrame = resetNow || detectedSceneCut.
    // Set _producedThisFrame only when real (or intentionally-zero-with-reset) motion reached _motion.
    (void) resetNow;

    _recorded = true;   // a real implementation sets this once it has put work on the list
    _recorded = false;  // skeleton: nothing was recorded
    return false;       // not built -> caller uses the zero field
}

void SynthMotion::ConfirmExecuted()
{
    if (!_recorded)
        return;

    // TODO(skeleton): swap _lumaPing <-> _lumaPong so this frame's luma is next frame's previous,
    // mark _hasPrevious, and clear _resetOwed. The swap happens here, not in Record, so a recording
    // that never executed does not become next frame's history.
    _hasPrevious = true;
    _resetOwed = false;
    _recorded = false;
    _producedThisFrame = false;
    _resetThisFrame = false;
}

void SynthMotion::AbandonRecording()
{
    if (!_recorded)
        return;

    // The recorded luma never landed, so what would have been next frame's previous does not exist.
    // Force a reset next frame rather than differencing against a frame the GPU never saw.
    _resetOwed = true;
    _hasPrevious = false;
    _recorded = false;
    _producedThisFrame = false;
    _resetThisFrame = false;
}

} // namespace DlssNr
