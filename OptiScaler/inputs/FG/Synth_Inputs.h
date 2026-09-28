#pragma once
#include "SysUtils.h"

#include "Synth_Policy.h"

#include <d3d12.h>
#include <shaders/synth_motion/SynthMotion_Handoff.h>
#include <memory>
#include <vector>

class IFGFeature_Dx12;

namespace SynthMotion
{
class Estimator_Dx12;
}

// Frame generation inputs for a title that never calls an upscaler (FGInput::Synthesized).
//
// UpscalerInputsDx11wDx12 hands FG the depth and motion an upscaler was given. A title with no upscaler
// has neither, so this stands in with a display-size R16G16_FLOAT motion field and an R32_FLOAT depth,
// both cleared to zero once and never written again: no motion, and with inverted depth, every pixel
// at the far plane. FSR-FG still runs its own optical flow on the frames themselves, which step 1 of
// dlssnr/design/synthesized-frame-generation.md measured as enough on its own in Divinity.
//
// Two owners, one per transport, both calling Feed just before the presenter's Present dispatches FG:
// - The D3D11 bridge (one per bridge swapchain) records the clear onto its copy list, which the present
//   queue waits for, and confirms or abandons it with that list (RecordInit, ConfirmExecuted,
//   AbandonRecording).
// - A native D3D12 swapchain has no such list, so RecordInitOnQueue records the clear on a list of its
//   own and executes it on FG's queue, the one FSR-FG's prepare runs on, so the clear lands first.
// Either frees the pair only after a drain has proved the queues are done with it.
//
// With [FrameGen] SynthesizedMotion, Velocity is the synthesized motion field instead of the zero one:
// taken from DLSS-NR when it already estimated this base frame (SynthMotion::Handoff), otherwise from
// this object's own estimator, recorded on the same list or queue as the clear (RecordMotion,
// RecordMotionOnQueue). Samples of the field drive SynthFgPolicy: fast-motion response, the low-fps
// floor and the duplicate-present advice. Design: synthesized-frame-generation.md, "Motion into FG,
// and emulator behaviour".
class SynthInputs
{
  public:
    // Both out of line: the estimator is an incomplete type here, and a defaulted constructor would need
    // its destructor for the members after it.
    SynthInputs();
    ~SynthInputs();

    SynthInputs(const SynthInputs&) = delete;
    SynthInputs& operator=(const SynthInputs&) = delete;

    // Allocate for the presenter's backbuffer size, or keep what already matches, and record the
    // one-time clear onto cmdList if it is still owed. Safe to call every frame. Returns false only on
    // a real failure; nothing owed is success.
    bool RecordInit(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, UINT width, UINT height,
                    DXGI_FORMAT displayFormat);

    // A clear recorded onto a list nobody ran is not a clear. The bridge says which happened.
    void ConfirmExecuted();
    void AbandonRecording();

    // The same, for a caller with no list of its own: the clear goes on this object's list and is
    // executed on queue, then confirmed or abandoned here. Safe to call every frame; it only touches a
    // list when a clear is owed.
    bool RecordInitOnQueue(ID3D12CommandQueue* queue, UINT width, UINT height, DXGI_FORMAT displayFormat);

    // The synthesized field for this base frame, from colour -- the finished frame at display extent,
    // in colourState and left there. On the caller's list (the bridge's copy, confirmed or abandoned
    // with it through ConfirmExecuted / AbandonRecording), or on this object's own list executed on
    // queue. Nothing is recorded while [FrameGen] SynthesizedMotion is off. Call after RecordInit /
    // RecordInitOnQueue and before Feed.
    bool RecordMotion(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                      D3D12_RESOURCE_STATES colourState);
    bool RecordMotionOnQueue(ID3D12CommandQueue* queue, ID3D12Resource* colour, D3D12_RESOURCE_STATES colourState);

    // Per presented base frame: evaluate the FG state, start its frame and hand it the pair. Does
    // nothing until the pair exists and its zeros have executed.
    void Feed(IFGFeature_Dx12* fg, ID3D12Device* device);

    // Only after every queue that could still read the pair, or run the clear, has been drained.
    void Release();

  private:
    bool _Allocate(ID3D12Device* device, UINT width, UINT height);
    bool _EnsureOwnList(ID3D12Device* device);
    void _Retire();
    void _WarnOnce(const char* what);

    bool _RecordMotionOn(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                         D3D12_RESOURCE_STATES colourState);
    bool _EnsureMotionLists(ID3D12Device* device);
    bool _EnsureReadback(ID3D12Device* device, UINT width);
    void _RecordSample(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* field, UINT width, UINT height);
    void _ReadSamples();
    void _ConfirmMotion();
    void _AbandonMotion();
    void _ReleaseMotion();

    // The estimator, used when DLSS-NR did not publish a field for this base frame.
    std::unique_ptr<SynthMotion::Estimator_Dx12> _estimator;
    long long _lastEstimateMs = 0;
    // While DLSS-NR's estimator warms up and nothing of ours exists (SynthMotion::Handoff::WaitForPeer).
    SynthMotion::Handoff::PeerWait _peerWait {};
    bool _estimatorRecorded = false;

    // This base frame's field, owned by the estimator or by DLSS-NR; handed to FG instead of the zero
    // field. Cleared once fed.
    ID3D12Resource* _frameMotion = nullptr;
    bool _frameSceneCut = false;

    // RecordMotionOnQueue's lists: a ring, so a frame's recording never waits on the one before.
    static constexpr UINT kMotionLists = 3;
    ID3D12CommandAllocator* _motionAllocators[kMotionLists] {};
    ID3D12GraphicsCommandList* _motionLists[kMotionLists] {};
    UINT64 _motionFenceValues[kMotionLists] {};
    ID3D12Fence* _motionFence = nullptr;
    HANDLE _motionFenceEvent = nullptr;
    UINT64 _motionFenceValue = 0;
    UINT _motionSlot = 0;
    ID3D12Device* _motionDevice = nullptr;

    // Rows of the field read back for the policy, a few confirmed frames late, as the estimator reads
    // its scene-cut flag.
    static constexpr UINT kSampleRows = 16;
    static constexpr UINT kSampleSlots = 4;
    static constexpr UINT kSampleLatency = kSampleSlots - 1;
    ID3D12Resource* _readback = nullptr;
    const uint8_t* _readbackData = nullptr;
    UINT _sampleWidth = 0;
    UINT _sampleRowPitch = 0;
    UINT _sampleSlot = 0;
    bool _sampleRecorded = false;
    bool _slotValid[kSampleSlots] {};
    UINT64 _slotFrame[kSampleSlots] {};
    UINT64 _motionConfirmed = 0;
    std::vector<float> _magnitudes;

    SynthFgPolicy _policy;
    bool _reportedEstimator = false;
    bool _reportedTaken = false;
    bool _reportedPeerWait = false;
    bool _reportedMotionFailure = false;
    bool _reportedSkip = false;
    bool _reportedMotionFed = false;

    ID3D12Resource* _velocity = nullptr;
    ID3D12Resource* _depth = nullptr;
    ID3D12DescriptorHeap* _rtvHeap = nullptr;

    // A pair replaced without a drain in between. Parked, not freed, until the next Release.
    std::vector<ID3D12Resource*> _retired;

    // RecordInitOnQueue's own list, created on first use and kept until Release. The fence says when
    // its last clear finished, so the allocator is never reset under the GPU.
    ID3D12Device* _ownDevice = nullptr;
    ID3D12CommandAllocator* _ownAllocator = nullptr;
    ID3D12GraphicsCommandList* _ownList = nullptr;
    ID3D12Fence* _ownFence = nullptr;
    HANDLE _ownFenceEvent = nullptr;
    UINT64 _ownFenceValue = 0;

    UINT _width = 0;
    UINT _height = 0;
    DXGI_FORMAT _displayFormat = DXGI_FORMAT_UNKNOWN;

    bool _clearOwed = false;
    bool _clearRecorded = false;
    bool _resetOwed = true;
    bool _allocFailed = false;
    bool _fedReported = false;
    bool _warned = false;
};
