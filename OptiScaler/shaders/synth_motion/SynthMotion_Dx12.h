#pragma once

// Synthesized motion: a per-pixel motion field reconstructed from consecutive finished frames, for the
// passes that get no motion vectors from the game (DLSS-NR's present hosts, synthesized frame
// generation). It is a port of the FidelityFX SDK's optical flow (MIT; see README.md beside this file
// for provenance, passes and costs), followed by one pass of ours that turns its 8x8-block vectors into
// the output contract below. Designs: dlssnr/design/synthesized-motion.md (contract, §4) and
// dlssnr/design/synthesized-frame-generation.md (why this kernel).
//
// Outside the DLSS-NR module on purpose: frame generation uses it too, and the module has to stay
// removable as one block.

#include <d3d12.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace SynthMotion
{
class Estimator_Dx12
{
  public:
    Estimator_Dx12() = default;
    ~Estimator_Dx12();

    Estimator_Dx12(const Estimator_Dx12&) = delete;
    Estimator_Dx12& operator=(const Estimator_Dx12&) = delete;

    // Records the whole estimate onto cmdList. colour: the finished frame at full extent, any RGBA
    // UNORM/FLOAT format readable through an SRV (a TYPELESS resource is viewed as its UNORM/FLOAT
    // sibling), currently in colourState and returned to colourState. reset: the caller knows history is
    // broken (first frame, resize, its own reset). Allocates on first use and on extent/format change
    // (the caller guarantees the GPU is done with the old resources when the extent changes -- it calls
    // Release() after its drain; if not released, old resources are parked, never freed under the GPU).
    // Binds its own shader-visible descriptor heap on cmdList: the caller must re-bind its own heaps
    // afterwards. Returns false only on a real failure (logged once).
    //
    // A colour smaller than MinWidth x MinHeight records nothing and returns true; Ready() is false.
    bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                D3D12_RESOURCE_STATES colourState, bool reset);

    // R16G16_FLOAT at the colour's extent: .rg = displacement from the current frame to the previous one,
    // in colour pixels, +x right, +y down (the DLSS/FSR convention: prev = cur + mv). Zero where no
    // motion was found and on the first frame after a reset. Left in
    // D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE. Null until a large enough frame was recorded.
    ID3D12Resource* Motion() const { return _motion; }

    // A real field was produced this frame (false on a reset, during FFX's five warm-up frames after it,
    // and on a scene cut this object has seen).
    bool Ready() const { return _ready; }

    // The estimator detected a scene change. Read back from the GPU, so it arrives a few frames late
    // (ReadbackSlots - 1 confirmed frames); the field itself is already zero on the GPU in the frame
    // the cut is detected, because FFX's own search writes zero vectors then. Callers OR it into Reset.
    bool SceneCut() const { return _sceneCut; }

    void ConfirmExecuted();  // the list that carried the last Record executed
    void AbandonRecording(); // it was dropped: history must not advance
    void Release();          // free everything; only after the caller's drain

    static constexpr uint32_t MinWidth = 256;  // FFX's level-6 luma must be at least 4 pixels wide
    static constexpr uint32_t MinHeight = 128; // and the scene-change strata non-empty
    static constexpr uint32_t Levels = 7;      // FFX's OpticalFlowMaxPyramidLevels, all of them used
    static constexpr uint32_t ReadbackSlots = 4;

  private:
    bool _EnsureDevice(ID3D12Device* device);
    bool _Allocate(ID3D12Device* device, uint32_t width, uint32_t height);
    void _Park();
    void _ReleaseParked();
    D3D12_GPU_DESCRIPTOR_HANDLE _Table(uint32_t parity, uint32_t slot, std::initializer_list<ID3D12Resource*> uavs);
    D3D12_GPU_DESCRIPTOR_HANDLE _ColourSrv(ID3D12Resource* colour, DXGI_FORMAT format);
    void _Dispatch(ID3D12GraphicsCommandList* cmdList, uint32_t pass, D3D12_GPU_DESCRIPTOR_HANDLE table,
                   const void* constants, uint32_t x, uint32_t y, uint32_t z, const void* spdConstants = nullptr);

    ID3D12Device* _device = nullptr;
    ID3D12RootSignature* _rootSignature = nullptr;
    std::vector<ID3D12PipelineState*> _pipelines;
    ID3D12DescriptorHeap* _heap = nullptr;
    uint32_t _descriptorSize = 0;
    bool _tableWritten[2][32] {};
    uint32_t _colourSlot = 0;

    // Two luma pyramids, the current frame's and the previous one's, swapped on ConfirmExecuted.
    ID3D12Resource* _luma[2][Levels] {};
    // FFX's two flow pyramids, which the levels alternate between within one frame.
    ID3D12Resource* _flow[2][Levels] {};
    ID3D12Resource* _flowFinal = nullptr; // level 0, filtered: what the expand pass reads
    ID3D12Resource* _scdHistogram = nullptr;
    ID3D12Resource* _scdPreviousHistogram = nullptr;
    ID3D12Resource* _scdTemp = nullptr;
    ID3D12Resource* _scdOutput = nullptr;
    ID3D12Resource* _motion = nullptr;
    ID3D12Resource* _readback = nullptr;
    const uint32_t* _readbackData = nullptr;

    // Replaced without a Release in between: kept, never freed under the GPU, until Release.
    std::vector<IUnknown*> _parked;

    uint32_t _width = 0;
    uint32_t _height = 0;
    uint32_t _lumaWidth[Levels] {};
    uint32_t _lumaHeight[Levels] {};
    uint32_t _flowWidth[Levels] {};
    uint32_t _flowHeight[Levels] {};

    D3D12_RESOURCE_STATES _motionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES _motionStateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // History. Advanced only by ConfirmExecuted, so a dropped list leaves the estimator where it was.
    uint32_t _nextParity = 0;
    int64_t _confirmedFrameIndex = -1;
    uint32_t _recordedFrameIndex = 0;
    bool _recordedReset = false;
    bool _pending = false;
    bool _needReset = true;
    uint64_t _confirmedCount = 0;
    uint64_t _confirmedSinceReset = 0;

    bool _ready = false;
    bool _sceneCut = false;
    bool _failed = false;
    bool _warnedSmall = false;
    bool _warnedFormat = false;
};
} // namespace SynthMotion
