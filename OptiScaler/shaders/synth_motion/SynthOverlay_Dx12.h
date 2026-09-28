#pragma once

// Synthesized frame generation's HUD mask: DLSS-NR's static-overlay rule (precompile/static_overlay_rule.h,
// the header DLSS-NR's own mask pass includes) run at display size on the game's frame, for the two HUD
// fixes of dlssnr/design/synthesized-frame-generation.md, "The HUD: near depth and a UI layer":
// - Depth(): the depth FSR-FG is handed with [FrameGen] SynthesizedHudDepth. With inverted depth, 1.0 (the
//   near plane) where the mask holds and 0.0 (the far plane) elsewhere, so FSR's disocclusion sees the
//   interface in front of the scene, and the interface's own vectors win the collisions at the scatter.
// - Layer(): the UI resource FFX's swapchain composes over every frame it presents, with
//   [FrameGen] SynthesizedHudLayer: the presented frame, with the mask as alpha.
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
    // and builds up again over the rule's entry streak.
    // Allocates on first use and on an extent change. The old objects are parked, never freed under the GPU,
    // until Release(), which the caller makes only after its drain. Binds its own shader-visible heap and
    // root signature on cmdList. Returns false only on a real failure (logged once). A frame smaller than
    // MinWidth x MinHeight records nothing and returns true; Pending() then stays false.
    bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                D3D12_RESOURCE_STATES colourState, bool reset);

    // Records the UI layer from presented, the frame as it will be presented, at the extent of the last
    // Record, with that Record's mask as alpha. On the same list after Record, or on a list the same queue
    // executes after it. Allocates the layer on first use: RGBA8 UNORM for an 8-bit UNORM frame, RGBA16F
    // otherwise (see synth_overlay_layer.hlsl). Returns false when nothing was recorded.
    bool RecordLayer(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* presented,
                     D3D12_RESOURCE_STATES presentedState);

    // All three rest in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, at Width() x Height(). Their contents
    // are defined once a recording that wrote them has executed: Depth() and Mask() after the first confirmed
    // Record at this extent, Layer() once LayerWritten().
    ID3D12Resource* Depth() const { return _depth; } // R32_FLOAT, 1 where the mask holds, else 0
    ID3D12Resource* Mask() const { return _mask; }   // R8_UNORM, the mask itself
    ID3D12Resource* Layer() const { return _layer; } // RGBA8 UNORM or RGBA16F: rgb the frame, a the mask

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

  private:
    bool _EnsureDevice(ID3D12Device* device);
    bool _Allocate(ID3D12Device* device, uint32_t width, uint32_t height);
    bool _EnsureLayer(DXGI_FORMAT format);
    void _Park();
    void _ReleaseParked();
    // The next slot of the descriptor ring: three SRVs, then four UAVs; null views where srvs/uavs run out.
    uint32_t _WriteSlot(ID3D12Resource* const* srvs, const DXGI_FORMAT* srvFormats, uint32_t srvCount,
                        ID3D12Resource* const* uavs, uint32_t uavCount);
    D3D12_GPU_DESCRIPTOR_HANDLE _Gpu(uint32_t index) const;

    ID3D12Device* _device = nullptr;
    ID3D12RootSignature* _rootSignature = nullptr;
    ID3D12PipelineState* _detectPipeline = nullptr;
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
    uint32_t _current = 0; // which of the pairs holds last frame's

    // Replaced without a Release in between: kept, never freed under the GPU, until Release.
    std::vector<IUnknown*> _parked;

    uint32_t _width = 0;
    uint32_t _height = 0;

    bool _valid = false;        // the previous pair holds a frame that executed
    bool _pending = false;      // a Record awaits its confirmation
    bool _layerPending = false; // a RecordLayer awaits its confirmation
    bool _layerWritten = false;
    bool _failed = false;
    bool _warnedFormat = false;
    bool _warnedLayer = false;
};
} // namespace SynthMotion
