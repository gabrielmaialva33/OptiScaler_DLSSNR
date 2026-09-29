#pragma once

// Synthesized frame generation's HUD mask: DLSS-NR's static-overlay rule (precompile/static_overlay_rule.h,
// the header DLSS-NR's own mask pass includes) run at display size on the game's frame, for the two HUD
// fixes of dlssnr/design/synthesized-frame-generation.md, "The HUD: near depth and a UI layer":
// - Depth(): the depth FSR-FG is handed with [FrameGen] SynthesizedHudDepth. With inverted depth, 1.0 (the
//   near plane) where the mask holds and 0.0 (the far plane) elsewhere, so FSR's disocclusion sees the
//   interface in front of the scene, and the interface's own vectors win the collisions at the scatter.
// - Layer(): the UI resource FFX's swapchain composes over every frame it presents, with
//   [FrameGen] SynthesizedHudLayer: the presented frame, with the layer's own mask as alpha. That mask is a
//   superset of the depth's (precompile/static_overlay_layer.h): the strict mask, what grows from multi-frame
//   still seeds through still pixels, and a feathered band of [FrameGen] SynthesizedHudMargin px around it
//   ("The HUD layer's own mask: recall and a margin"). Computed only when a Record asks for it.
//
// Outside the DLSS-NR module on purpose, like the estimator beside it: frame generation must not depend on
// that module, and it has to stay removable as one block. Self-contained (its own root signature, heap and
// pipelines), so the GPU harness builds it with the estimator and nothing else.

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace SynthMotion
{
class Overlay_Dx12
{
  public:
    Overlay_Dx12() = default;
    ~Overlay_Dx12();

    Overlay_Dx12(const Overlay_Dx12&) = delete;
    Overlay_Dx12& operator=(const Overlay_Dx12&) = delete;

    // Records the rule on cmdList. colour: the game's frame at full extent, any RGBA UNORM/FLOAT format
    // readable through an SRV (TYPELESS is read as its UNORM/FLOAT sibling), in colourState and returned
    // there. reset: the caller knows history is broken (first frame, a stale gap); the mask is then empty
    // and builds up again over the rule's entry streak. layer: also compute the UI layer's own mask, which a
    // RecordLayer after this Record needs; without it the pass is the mask alone, and allocates nothing for the
    // layer. The mask and depth are the same either way. The layer's mask has a history of its own: a Record
    // without it breaks that history, and the next one with it starts over.
    // Allocates on first use and on an extent change. The old objects are parked, never freed under the GPU,
    // until Release(), which the caller makes only after its drain. Binds its own shader-visible heap and
    // root signature on cmdList. Returns false only on a real failure (logged once). A frame smaller than
    // MinWidth x MinHeight records nothing and returns true; Pending() then stays false.
    bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                D3D12_RESOURCE_STATES colourState, bool reset, bool layer = false);

    // Records the UI layer from presented, the frame as it will be presented, at the extent of the last
    // Record, which must have asked for the layer. Its alpha is that Record's layer core grown by a band of
    // margin px (clamped to MaxLayerMargin; 0 is the core alone). On the same list after Record, or on a list the
    // same queue executes after it. Allocates the layer on first use: RGBA8 UNORM for an 8-bit UNORM frame,
    // RGBA16F otherwise (see synth_overlay_layer.hlsl). Returns false when nothing was recorded.
    bool RecordLayer(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* presented,
                     D3D12_RESOURCE_STATES presentedState, uint32_t margin);

    // All rest in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, at Width() x Height(). Their contents are
    // defined once a recording that wrote them has executed: Depth() and Mask() after the first confirmed
    // Record at this extent, LayerCore() after one that asked for the layer, Layer() once LayerWritten().
    ID3D12Resource* Depth() const { return _depth; }    // R32_FLOAT, 1 where the mask holds, else 0
    ID3D12Resource* Mask() const { return _mask; }      // R8_UNORM, the mask itself
    ID3D12Resource* LayerCore() const { return _core; } // R8_UNORM, max(mask, grown): the band's input
    ID3D12Resource* Layer() const { return _layer; }    // RGBA8 UNORM or RGBA16F: rgb the frame, a the band

    uint32_t Width() const { return _width; }
    uint32_t Height() const { return _height; }

    // A Record is waiting for ConfirmExecuted or AbandonRecording.
    bool Pending() const { return _pending; }
    // A layer recording has executed since the layer was allocated.
    bool LayerWritten() const { return _layerWritten; }

    void ConfirmExecuted();  // the list(s) carrying what is pending executed: history advances
    void AbandonRecording(); // they were dropped: nothing advances, and the next Record redoes this frame
    void Release();          // free everything; only after the caller's drain

    // Anything smaller is a transient (PCSX2's 1x1 swapchain), not a picture with a HUD in it.
    static constexpr uint32_t MinWidth = 64;
    static constexpr uint32_t MinHeight = 64;

    // The widest band the layer pass supports (UL_MAX_MARGIN in precompile/static_overlay_layer.h, which sizes its
    // group-shared memory; tests/fg-synth-policy holds the two equal).
    static constexpr uint32_t MaxLayerMargin = 24;

  private:
    bool _EnsureDevice(ID3D12Device* device);
    bool _Allocate(ID3D12Device* device, uint32_t width, uint32_t height);
    bool _EnsureLayer(DXGI_FORMAT format);
    bool _EnsureLayerMask();
    void _Park();
    void _ReleaseParked();
    // The next slot of the descriptor ring: five SRVs, then seven UAVs; null views where srvs/uavs run out.
    uint32_t _WriteSlot(ID3D12Resource* const* srvs, const DXGI_FORMAT* srvFormats, uint32_t srvCount,
                        ID3D12Resource* const* uavs, uint32_t uavCount);
    D3D12_GPU_DESCRIPTOR_HANDLE _Gpu(uint32_t index) const;

    ID3D12Device* _device = nullptr;
    ID3D12RootSignature* _rootSignature = nullptr;
    ID3D12PipelineState* _detectPipeline = nullptr;      // the mask alone
    ID3D12PipelineState* _detectLayerPipeline = nullptr; // the mask and the layer's own
    ID3D12PipelineState* _layerPipeline = nullptr;
    ID3D12DescriptorHeap* _heap = nullptr;
    uint32_t _descriptorSize = 0;
    uint32_t _slot = 0;

    // Last frame's and this frame's, swapped when a Record is confirmed, so no pixel reads a neighbour
    // another thread is writing.
    ID3D12Resource* _luma[2] = {}; // R16_FLOAT
    ID3D12Resource* _acc[2] = {};  // R16G16_FLOAT: .x protection, .y consecutive candidate frames
    ID3D12Resource* _mask = nullptr;
    ID3D12Resource* _depth = nullptr;
    ID3D12Resource* _layer = nullptr;
    DXGI_FORMAT _layerFormat = DXGI_FORMAT_UNKNOWN;
    // The layer's own mask, allocated when a Record first asks for it at this extent. Pairs like the above, on the
    // same _current.
    ID3D12Resource* _layerState[2] = {}; // RGBA8 UNORM, /255: still frames, seed hold, grown distance
    ID3D12Resource* _tiles[2] = {};      // RGBA8 UNORM, per 8x8 tile, counts: moved, textured, orientation bits, pixels
    ID3D12Resource* _core = nullptr;     // R8_UNORM
    ID3D12Resource* _coreTiles = nullptr; // the tile map the latest Record wrote beside the core (not owned)
    uint32_t _current = 0;                // which of the pairs holds last frame's

    // Replaced without a Release in between: kept, never freed under the GPU, until Release.
    std::vector<IUnknown*> _parked;

    uint32_t _width = 0;
    uint32_t _height = 0;

    bool _valid = false;        // the previous pair holds a frame that executed
    bool _layerValid = false;   // and the previous layer pair holds that frame's layer state
    bool _pending = false;      // a Record awaits its confirmation
    bool _pendingLayer = false; // and it computed the layer's own mask
    bool _coreRecorded = false; // the latest Record computed the layer core that RecordLayer reads
    bool _layerPending = false; // a RecordLayer awaits its confirmation
    bool _layerWritten = false;
    bool _failed = false;
    bool _warnedFormat = false;
    bool _warnedLayer = false;
    bool _warnedLayerMask = false;
};
} // namespace SynthMotion
