#pragma once

// LSFG study (branch study/lsfg-d3d12, never merged): Lossless Scaling's frame generation (LSFG 3.1) as the
// interpolator behind FSR-FG's own swapchain. tools/lsfg-study/OPTI_INTEGRATION.md, option (b): FFX keeps
// the swapchain, the pacing, the present thread and UI composition; LSFG replaces only the one call in
// FSRFG_Dx12::DispatchCallback that hands the frame to FFX's interpolation. x2 only, because FFX's swapchain
// asks for exactly one generated frame.
//
// The call sites in OptiScaler/framegen/ffx/FSRFG_Dx12.cpp are one-liners. With [LSFG] Enabled=false (the
// default) BeginFrame stores false and returns, TakesFrame returns false, nothing is searched, read or
// allocated, and FSR-FG runs exactly as it did before.

#include <cstdint>

struct ffxDispatchDescFrameGeneration;

namespace Lsfg
{

// FSRFG_Dx12::Dispatch, once per base frame, before FFX's prepare. Latches who interpolates frame `frameId` (slot
// `fIndex` = frameId % BUFFER_COUNT): true means LSFG does, so FFX's depth/velocity gate and its prepare are
// skipped for it (they would feed an interpolation that never runs). False while LSFG is off, refused, or still
// building, and FFX's own interpolation runs as before. Also frees parked contexts once their time is up (never
// inside the callback).
bool BeginFrame(int fIndex, uint64_t frameId);

// FSRFG_Dx12::DispatchCallback, at the hand-off to FFX. Returns the latch BeginFrame set for params->frameID, once.
// On a frame FFX interpolates, with LSFG enabled, it looks at the presented frame and starts LSFG's build for it
// on a worker.
bool TakesFrame(int fIndex, ffxDispatchDescFrameGeneration* params);

// In place of FfxApiProxy::D3D12_Dispatch on a frame LSFG took: records ingest, the pre-pass and the
// generated frame on FFX's interpolation list and sets numGeneratedFrames (0 while warming up, after a reset,
// or when the frame cannot be served; FFX then shows only the real frame). Always returns FFX_API_RETURN_OK,
// so FFX executes the list it handed over and the tracked resource states stay true.
uint32_t FfxDispatch(ffxDispatchDescFrameGeneration* params);

// Whether LSFG took the last base frame FSR-FG configured (the latest BeginFrame latch): false while LSFG is off,
// refused or still building. The synthesized input reads it to skip what only FFX's interpolation consumes; it
// is one base frame late there, which costs at most one frame of that work either way.
bool Interpolating();

// FSRFG_Dx12::ReleaseSwapchain, after FFX's swapchain context is destroyed. FFX may not have drained yet (its
// swapchain object can outlive the context), so the current context is parked, like any retired one.
void OnSwapchainReleased();

// ---------------------------------------------------------------------------------------------------------
// Multi-Frame Generation (x3, x4) Presenter Contract
//
// Records generated frames for the current base frame pair. Reuses per-pair pre-pass work across all K frames.
//
// Sequential mode:
//   Presenter calls RecordGenerated K times per base frame pair (index = 0 .. count - 1):
//   - index == 0: ingests presentColor, executes Pre-Pass + Main-Pass at timestamp 1/(count+1).
//   - index > 0: executes Main-Pass ONLY at timestamp (index+1)/(count+1), reusing the pinned feature pyramids.
//   - When index == count - 1, the history counter advances for the next base frame.
//
// Batch mode:
//   Presenter provides an array of K output resources in `outputs` to RecordBatch or FfxDispatch.
//
bool RecordGenerated(ID3D12GraphicsCommandList* list, ID3D12Resource* presentColor, D3D12_RESOURCE_STATES presentState,
                     uint32_t index, uint32_t count, ID3D12Resource* output, D3D12_RESOURCE_STATES outputState,
                     bool reset = false, uint64_t frameId = 0);

bool RecordBatch(ID3D12GraphicsCommandList* list, ID3D12Resource* presentColor, D3D12_RESOURCE_STATES presentState,
                 uint32_t count, ID3D12Resource* const* outputs, const D3D12_RESOURCE_STATES* outputStates,
                 bool reset = false, uint64_t frameId = 0);

struct Status
{
    bool enabled = false;
    bool active = false;
    uint32_t width = 0;
    uint32_t height = 0;
    float flow = 1.0f;
    bool performance = false;
    std::string fallbackReason;
    double gpuMs = 0.0;
};

Status GetStatus();

} // namespace Lsfg
