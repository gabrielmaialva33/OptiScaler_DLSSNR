#pragma once

// ===========================================================================================
// UNBUILT SKELETON. Design: OptiScaler/dlssnr/design/synthesized-motion.md.
//
// This file is NOT in OptiScaler.vcxproj, is compiled by nothing, and is referenced by no
// existing source. It exists so the integration worker has a concrete shape to wire in. Do
// not add it to a build until the design note's step 2 is reached, and do not let it claim a
// field it has not produced. Methods are declared; the .cpp beside it is stubs.
// ===========================================================================================

#include <d3d12.h>
#include <cstdint>

namespace DlssNr
{

// A motion field reconstructed from consecutive frames, for a caller that has no engine motion.
//
// Drop-in for DlssNr::ZeroGuides::Motion() on the no-capture branch of the present host: same
// R16G16_FLOAT format, same working-resolution extent, same rest-state discipline. The difference
// is the contents -- current-to-previous displacement in working-resolution pixels instead of zero --
// and that a caller using it sets MvScaleX = 1, MvScaleY = +/-1 (sign per the design note) rather
// than the zero field's degenerate units hack.
//
// It reads a colour the caller has already landed on D3D12 (the present host's working proxy),
// records its passes onto the caller's list, and executes on the caller's one queue. No private
// submission, no CPU wait, no second device. One instance per present host; nothing shared.
//
// The estimator is coarse-to-fine block matching over a small luma pyramid (~320 px wide), validated
// and bilinearly upscaled to the working-resolution output. See the design note for why block
// matching over DIS/LK, the cost budget, and the open questions (depth, the MvScaleY sign, whether
// the model inspects the texture at all).
class SynthMotion
{
  public:
    SynthMotion() = default;
    ~SynthMotion();

    SynthMotion(const SynthMotion&) = delete;
    SynthMotion& operator=(const SynthMotion&) = delete;

    // Allocate for this working size, or keep what is already the right size. Safe every frame. A
    // size change throws the previous-frame history away and raises a reset, so the caller must know
    // the GPU is done with the old resources (the present host drains before it gets here).
    bool Ensure(ID3D12Device* device, uint32_t width, uint32_t height);

    // Record the whole per-frame sequence onto the caller's list: luma + pyramid from source, the
    // scene-score reduction, the coarse-to-fine search (skipped on a static or reset frame), and the
    // validate/scale/upscale into the output field.
    //
    // source is the caller's working colour, read only; it arrives and is left in sourceState. reset
    // is the caller's own reset for this frame (first host frame, resize); this class ORs it with the
    // resets it detects itself (scene cut, abandoned history).
    //
    // False means no usable field this frame -- not built, static/first frame with nothing to
    // difference, or a failure -- and the caller falls back to the zero field. It is never an error
    // for the caller.
    bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                D3D12_RESOURCE_STATES sourceState, uint32_t width, uint32_t height, bool reset);

    // The reconstructed field, in R16G16_FLOAT at working resolution, left in the state RecordRest
    // was given. Null until a Record has produced one. Valid only for the recording it came from.
    ID3D12Resource* Motion() const { return _motion; }

    // Whether the field is allocated and the last Record actually produced motion into it (as opposed
    // to declining to a static/first/reset frame).
    bool Ready() const { return _motion != nullptr && _producedThisFrame; }

    // Whether the frame just recorded is one the model must treat as a history discontinuity (first
    // frame, resize, detected scene cut, or a previously abandoned recording). The caller ORs this
    // into DlssNrFrameInfo::Reset.
    bool ResetThisFrame() const { return _resetThisFrame; }

    // The recording ran: swap this frame's luma to become next frame's previous, and settle the
    // history. Mirrors ZeroGuides::ConfirmExecuted / PresentHost::ConfirmExecuted.
    void ConfirmExecuted();

    // The recording was dropped: do not advance the previous-frame history, and force a reset on the
    // next frame so nothing is differenced against a frame the GPU never saw.
    void AbandonRecording();

    // Throw everything away. The caller must already have proved the GPU is done with it.
    void Release();

  private:
    bool _Allocate(ID3D12Device* device, uint32_t width, uint32_t height);

    // Working-resolution output field, R16G16_FLOAT, ALLOW_UNORDERED_ACCESS.
    ID3D12Resource* _motion = nullptr;

    // Previous/current luma pyramids at estimation resolution (~320 px wide), ping-ponged on confirm.
    // R8_UNORM or R16_FLOAT (see design note, open question on bit depth). Mip chain for the pyramid.
    ID3D12Resource* _lumaPing = nullptr;
    ID3D12Resource* _lumaPong = nullptr;

    // A small readback/UAV for the scene-score reduction, so a static or cut frame skips the search.
    ID3D12Resource* _sceneScore = nullptr;

    uint32_t _width = 0; // working resolution
    uint32_t _height = 0;
    uint32_t _flowWidth = 0; // estimation resolution (~320 wide, even, >= 64)
    uint32_t _flowHeight = 0;

    bool _hasPrevious = false; // false on first frame and after a reset/abandon
    bool _producedThisFrame = false;
    bool _resetThisFrame = false;
    bool _resetOwed = true; // first frame, resize, or a dropped recording
    bool _recorded = false;
};

} // namespace DlssNr
