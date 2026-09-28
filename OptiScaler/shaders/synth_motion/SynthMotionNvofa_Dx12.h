#pragma once

// Synthesized motion from NVIDIA's Optical Flow Accelerator (NVOFA): the second motion source behind the
// same output contract as Estimator_Dx12 (SynthMotion_Dx12.h), selected by [DlssNr] SynthMotionSource.
// Design: dlssnr/design/synthesized-motion.md, "Motion sources: FidelityFX and NVOFA".
//
// Why a second source: the OFA is a fixed-function engine present on Turing and later, so the search
// runs off the shader cores that the NR model is already saturating on a mid-range card. The price is
// one frame of latency (below) and no scene-cut detection of its own.
//
// nvofapi64.dll is loaded at runtime and never linked: it ships with the NVIDIA driver on Windows and
// with dxvk-nvapi under Proton (D3D12 only, through VK_NV_optical_flow). When it is missing, refuses the
// device, or lacks the formats used here, the estimator says so once through Available() and the caller
// falls back to the FidelityFX estimator. The vendored NVIDIA headers are MIT (include/nvofa/).
//
// One frame late, by construction. The estimator records onto the caller's list, and the engine runs
// on its own queue, so it cannot run in the middle of that list: the frame's grayscale input is
// prepared on the caller's list, and the engine executes after the list has been submitted
// (ConfirmExecuted), fenced against the caller's queue. The field Record hands out therefore describes
// the motion between the two frames before this one -- for a steady camera the same thing, for an
// accelerating one a frame behind. Nothing waits on the CPU; the caller's queue waits on the engine's
// fence before the list that reads the result.

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace SynthMotion
{
class NvofaEstimator_Dx12
{
  public:
    NvofaEstimator_Dx12() = default;
    ~NvofaEstimator_Dx12();

    NvofaEstimator_Dx12(const NvofaEstimator_Dx12&) = delete;
    NvofaEstimator_Dx12& operator=(const NvofaEstimator_Dx12&) = delete;

    // Estimator_Dx12::Record's contract, plus queue: the queue cmdList will be executed on. The engine is
    // fenced against it -- a GPU wait before the list, a signal after it -- so it must be the queue that
    // actually runs the list. Returns false on a real failure, and when the engine is unavailable
    // (then Available() is false and stays false; the caller falls back).
    bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                D3D12_RESOURCE_STATES colourState, bool reset, ID3D12CommandQueue* queue);

    // Same format, extent, units, sign and rest state as Estimator_Dx12::Motion() (current -> previous,
    // colour pixels, +y down, NON_PIXEL_SHADER_RESOURCE). Valid when Ready().
    ID3D12Resource* Motion() const { return _motion; }

    // This frame's recording carries a field: false after a reset until two frames have been confirmed
    // (the engine needs a pair, and its result arrives a frame later).
    bool Ready() const { return _ready; }

    // Never raised: the engine has no scene-change output, and this backend does not measure one. The
    // caller's own resets (resize, staleness) still apply; a cut costs one frame of wrong motion.
    bool SceneCut() const { return false; }

    void ConfirmExecuted();  // the list that carried the last Record executed: fence it, run the engine
    void AbandonRecording(); // it was dropped: no engine run, history not advanced
    void Release();          // free everything; only after the caller's drain (waits on the engine too)

    bool Available() const { return _unavailable == nullptr; }
    const char* UnavailableReason() const { return _unavailable != nullptr ? _unavailable : ""; }

    static constexpr uint32_t MinWidth = 256; // the same floor as the FidelityFX backend, for one contract
    static constexpr uint32_t MinHeight = 128;
    static constexpr uint32_t MaxInputHeight = 540; // the engine's input is the colour scaled to this height
    static constexpr uint32_t LagFrames = 1;

  private:
    struct Session; // the engine's handle and the resources registered with it (one per extent)

    bool _EnsureApi();
    bool _EnsureDevice(ID3D12Device* device);
    bool _Allocate(ID3D12Device* device, uint32_t width, uint32_t height);
    void _Park();
    void _DestroySession(Session* session, bool engineIdle);
    bool _WaitEngineIdle(uint32_t timeoutMs);
    void _MarkUnavailable(const char* reason);
    D3D12_GPU_DESCRIPTOR_HANDLE _ColourSrv(ID3D12Resource* colour, DXGI_FORMAT format);
    D3D12_GPU_DESCRIPTOR_HANDLE _Descriptor(uint32_t slot) const;

    ID3D12Device* _device = nullptr;
    ID3D12RootSignature* _rootSignature = nullptr;
    ID3D12PipelineState* _prep = nullptr;
    ID3D12PipelineState* _expand = nullptr;
    ID3D12DescriptorHeap* _heap = nullptr;
    uint32_t _descriptorSize = 0;
    uint32_t _colourSlot = 0;

    Session* _session = nullptr;
    std::vector<Session*> _parked; // replaced without a Release in between; freed only once idle
    std::vector<IUnknown*> _parkedObjects;

    ID3D12Resource* _motion = nullptr;
    ID3D12Fence* _prepFence = nullptr;       // the caller's queue signals it after the list with the input
    ID3D12Fence* _completionFence = nullptr; // the engine signals it after each execute
    uint64_t _prepValue = 0;
    uint64_t _completionValue = 0; // last value an execute was asked to signal
    uint64_t _waitedValue = 0;     // last value the caller's queue was made to wait for
    ID3D12CommandQueue* _queue = nullptr;

    uint32_t _width = 0;
    uint32_t _height = 0;

    // History. Advanced only by ConfirmExecuted.
    uint32_t _current = 0;      // which input slot the next Record prepares
    bool _havePrevious = false; // the other slot holds the previous confirmed frame's input
    bool _flowValid = false;    // the last confirm ran the engine, so the flow texture has a result
    bool _needReset = true;
    bool _pending = false;
    bool _recordedReset = false;
    bool _temporalBroken = true; // tells the engine not to seed from its previous vectors

    bool _ready = false;
    uint32_t _executeFailures = 0;
    uint32_t _allocateFailures = 0;
    const char* _unavailable = nullptr;
    bool _reportedUnavailable = false;
    bool _reportedSession = false;
    bool _warnedSmall = false;
    bool _warnedFormat = false;
};
} // namespace SynthMotion
