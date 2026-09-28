#pragma once
#include "SysUtils.h"

#include "Synth_Hud.h"
#include "Synth_Policy.h"

#include <d3d12.h>
#include <shaders/synth_motion/SynthMotion_Handoff.h>
#include <memory>
#include <vector>

class IFGFeature_Dx12;

namespace SynthMotion
{
class Estimator_Dx12;
class Overlay_Dx12;
} // namespace SynthMotion

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
// RecordFrameOnQueue). Samples of the field drive SynthFgPolicy: fast-motion response, the low-fps
// floor and the duplicate-present advice. Design: synthesized-frame-generation.md, "Motion into FG,
// and emulator behaviour".
//
// The HUD ([FrameGen] SynthesizedHudDepth, SynthesizedHudLayer; Synth_Hud.h says which runs): a static-overlay
// mask (SynthMotion::Overlay_Dx12) from the game's frame, recorded with the motion. With the first key, FG gets
// the mask's depth instead of the constant one: the interface near, the scene far. With the second, FG gets a
// UI layer, the presented frame with the mask as alpha, which FFX composes over every frame it presents; on
// native D3D12 it is recorded late, after DLSS-NR's pass (RecordLayerOnQueue). Design:
// synthesized-frame-generation.md, "The HUD: near depth and a UI layer".
//
// The output is FSR-FG or DLSS-G, fed through the same IFGFeature_Dx12 calls. DLSS-G takes everything but the UI
// layer, which only FFX composes (Synth_Hud.h). Design: synthesized-frame-generation.md, "DLSS-G output".
class SynthInputs
{
  public:
    // Both out of line: the estimator and the mask are incomplete types here, and a defaulted constructor
    // would need their destructors for the members after them.
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

    // Whether anything is recorded from the finished frame this base frame: the motion field or the HUD mask.
    // A caller that has to fetch the frame first asks this before fetching it.
    static bool FrameWanted();

    // The synthesized field for this base frame, from colour -- the finished frame at display extent,
    // in colourState and left there. On the caller's list (the bridge's copy, confirmed or abandoned
    // with it through ConfirmExecuted / AbandonRecording). Nothing is recorded while [FrameGen]
    // SynthesizedMotion is off. Call after RecordInit and before Feed.
    bool RecordMotion(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                      D3D12_RESOURCE_STATES colourState);

    // The bridge's HUD: the mask from colour, the game's frame, and with SynthesizedHudLayer the UI layer from
    // presented, the frame that goes on screen (the neural host's output when it ran, colour otherwise), at the
    // same extent. Both in their states and left there, on the caller's list, confirmed or abandoned with it.
    // Nothing is recorded unless a HUD key asks for it. Call after RecordMotion and before Feed.
    bool RecordOverlay(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                       D3D12_RESOURCE_STATES colourState, ID3D12Resource* presented,
                       D3D12_RESOURCE_STATES presentedState);

    // Native D3D12, where there is no list of the caller's: the motion field and the HUD mask from colour, on
    // one list of this object's own, executed on queue (FG's, ahead of FSR-FG's prepare) and settled here.
    // Call after RecordInitOnQueue and before Feed; nothing is recorded unless FrameWanted().
    bool RecordFrameOnQueue(ID3D12CommandQueue* queue, ID3D12Resource* colour, D3D12_RESOURCE_STATES colourState);

    // Native D3D12, after DLSS-NR's present pass and before FFX's Present: the UI layer from presented, the
    // backbuffer as it will be shown (the pass edits it in place). Records only when this base frame's mask
    // asked for a layer (LayerOwed), on a list of this object's own executed on queue.
    bool RecordLayerOnQueue(ID3D12CommandQueue* queue, ID3D12Resource* presented, D3D12_RESOURCE_STATES presentedState);
    bool LayerOwed() const { return _layerOwed; }

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
    bool _RecordOverlayOn(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                          D3D12_RESOURCE_STATES colourState, ID3D12Resource* presented,
                          D3D12_RESOURCE_STATES presentedState);
    void _ConfirmOverlay();
    void _AbandonOverlay();
    void _ReleaseOverlay();
    // Records onto the next list of the frame ring, closes it, executes it on queue and settles everything
    // recorded on it (confirmed when it executed, abandoned otherwise). record(device, list) says whether
    // anything worth executing was recorded.
    template <typename Record> bool _RunOnQueue(ID3D12CommandQueue* queue, Record&& record);
    bool _EnsureFrameLists(ID3D12Device* device);
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

    // The HUD mask, created when a HUD key first asks for it; see Synth_Hud.h.
    std::unique_ptr<SynthMotion::Overlay_Dx12> _overlay;
    long long _lastOverlayMs = 0;
    bool _overlayRecorded = false; // the mask is on the current list, not yet settled
    bool _layerRecorded = false;   // the layer is on the current list, not yet settled
    bool _overlayWantsLayer = false;
    // This base frame's mask executed at the extent it was recorded at. Cleared once fed.
    bool _frameOverlay = false;
    // Native D3D12: this base frame's mask executed and a layer is wanted; RecordLayerOnQueue records it.
    bool _layerOwed = false;
    bool _reportedOverlayFailure = false;
    bool _reportedLayerFailure = false;
    bool _reportedMaskDepth = false;
    bool _reportedLayer = false;
    bool _reportedLayerRefused = false;

    // RecordFrameOnQueue's and RecordLayerOnQueue's lists: a ring, so a frame's recording never waits on the
    // one before. Up to two per base frame (the frame, then the layer), so six covers three frames.
    static constexpr UINT kFrameLists = 6;
    ID3D12CommandAllocator* _frameAllocators[kFrameLists] {};
    ID3D12GraphicsCommandList* _frameLists[kFrameLists] {};
    UINT64 _frameFenceValues[kFrameLists] {};
    ID3D12Fence* _frameFence = nullptr;
    HANDLE _frameFenceEvent = nullptr;
    UINT64 _frameFenceValue = 0;
    UINT _frameSlot = 0;
    ID3D12Device* _frameDevice = nullptr;

    // Rows of the field read back for the policy, a few confirmed frames late, as the estimator reads
    // its scene-cut flag.
    static constexpr UINT kSampleRows = SynthMotionStats::kRows;
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
    SynthMotionStats::Scratch _sampleScratch;

    SynthFgPolicy _policy;
    bool _reportedEstimator = false;
    bool _reportedTaken = false;
    bool _reportedPeerWait = false;
    bool _reportedMotionFailure = false;
    bool _reportedSkip = false;
    bool _reportedMotionFed = false;

    // Fast-motion repeats in the current 10 s summary window (Feed), and the motion samples it saw.
    long long _fastWindowStartMs = 0;
    uint32_t _fastWindowFrames = 0;
    uint32_t _fastRepeats = 0;
    SynthMotionStats::Window _fastWindowMotion;

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
