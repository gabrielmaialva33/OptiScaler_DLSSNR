# Synthesized motion (`SynthMotion::Estimator_Dx12`)

A per-pixel motion field reconstructed from consecutive finished frames. It is for passes that get no
motion vectors from the game: DLSS-NR's present hosts and synthesized frame generation. Designs:
[synthesized-motion.md](../../dlssnr/design/synthesized-motion.md) (output contract, §4) and
[synthesized-frame-generation.md](../../dlssnr/design/synthesized-frame-generation.md) (why this
kernel). It lives outside `dlssnr/` because frame generation uses it too, and the NR module has to stay
removable as one block.

Status: built and syntax-checked. Not yet run on a GPU; the loopback harness is where it gets measured.

## Output contract

`Motion()`, valid after `Record()` whenever `Ready()`:

| | |
|---|---|
| Format | `DXGI_FORMAT_R16G16_FLOAT` |
| Extent | the colour's |
| Contents | `.rg` = displacement from the current frame to the previous one, in colour pixels, +x right, +y down. `prev = cur + mv`, the DLSS/FSR convention |
| Zero | where no motion was found; on a reset; for FFX's five warm-up frames after it; on a scene cut; on a pixel the previous frame reproduces clearly better standing still than moved by its block's vector (a static overlay, see below) |
| State | `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE` |

**No conversion is applied.** FFX's flow already has exactly this meaning. The search looks for the
current frame's block in the previous frame's luma and stores where it found it relative to the block:
`newVector = currentVector + minSadCoord` in `ffx_opticalflow_compute_optical_flow_v5.h`, an offset into
the second image, which is the previous frame. Level 0 is the colour's own resolution. The expand pass
reconstructs bilinearly between 8x8 block centres, then makes one choice per pixel.

**The per-pixel choice (2026-09-28).** A thin static overlay, such as a crosshair arm or HUD text,
covers a small part of its 8x8 block, so the moving world decides the block's vector. The expand scores
two hypotheses on the level-0 luma pair, over the pixel's 3x3 weighted 1-2-1 by 1-2-1: the block's
vector `v` rounded, and zero. It writes zero only when `2 * S0 + 64 < Sv`, and `v` otherwise. A tie
keeps `v`. Nothing is tested where `round(v)` is zero or points outside the frame. The rule, its margin
and what it cannot fix: "Static overlays" in
[synthesized-motion.md](../../dlssnr/design/synthesized-motion.md).

## What callers must know

- **Colour.**
  - An RGBA UNORM or FLOAT format, 2D, one sample. TYPELESS is read as its UNORM/FLOAT sibling.
  - It can be in any state. If that state lacks `NON_PIXEL_SHADER_RESOURCE`, the estimator transitions
    the colour there and back around the one pass that reads it.
  - It is saturated before the luma is taken (transfer function 0), so HDR highlights above 1 clip.
    That costs texture in the highlights, not correctness.
- **Minimum extent: 256x128.** FFX's level-6 luma must be at least 4 pixels wide. A smaller frame
  records nothing, returns true, and leaves `Ready()` false. PCSX2's transient 1x1 swapchain is exactly
  this case.
- **Heaps.** `Record` binds its own shader-visible descriptor heap and its own root signature, so the
  caller must re-bind its own afterwards.
- **History discipline, the same as `ZeroGuides` and `PresentHost`.**
  - The previous frame's luma advances only on `ConfirmExecuted()`.
  - `AbandonRecording()` leaves everything where it was, and a reset that was asked for stays owed.
  - A `Record` with neither of those since the last one is treated as abandoned.
- **Resize.**
  - A new extent re-allocates everything, including a new descriptor heap.
  - Call `Release()` after your drain. If you don't, the old objects are parked, never freed under the
    GPU, until `Release()` or destruction.
- **`SceneCut()` is read back from the GPU, so it is `ReadbackSlots - 1` = 3 confirmed frames late.**
  - The field itself is zero on the GPU in the very frame FFX detects the cut, because its search then
    writes zero vectors for four frames.
  - OR `SceneCut()` into your model's Reset for the history, not for the field.
- **Ready().** False on a reset frame, for frame indices 0 to 5 after it (FFX's `FrameIndex() <= 5` is
  a scene change), and while `SceneCut()` is set.

## Passes

Everything runs on the caller's list, in this order. All internal textures stay in `UNORDERED_ACCESS`,
and a global UAV barrier follows each dispatch.

| # | Pass | Reads | Writes | Dispatch (groups) |
|---|---|---|---|---|
| 0 | clear x4 (reset only, ours) | | scene-change histogram, previous histogram, temp, output | (w/64, 1) |
| 1 | prepare luma (FFX) | colour (t0) | current luma L0, R8_UINT | ((W/2)/16, (H/2)/16) |
| 2 | luminance pyramid (FFX SPD) | current luma L0 | current luma L1..L6 | one per 64x64 tile |
| 3 | scene-change histogram (FFX) | current luma L0 | histogram (2304x1 R32_UINT) | ((W/12)/32, 16, 9) |
| 4 | scene-change divergence (FFX) | histograms | previous histogram, temp, output (3x1) | (9, 3) |
| 5 | search, per level 6..0 (FFX v5) | current and previous luma at L; the prediction in flow A[L] | flow A[L] | (ceil(W_L/16), ceil(H_L/16)) |
| 6 | filter, per level (FFX v5, 3x3 median) | flow A[L] | flow B[L], or the final flow at L0 | (fw/16, fh/4) |
| 7 | scale, levels 6..1 (FFX v5) | luma at L, flow B[L] | prediction in flow B[L-1] | (fw_{L-1}/4, fh_{L-1}/4) |
| 8 | expand (ours) | final flow L0 (R16G16_SINT, one vector per 8x8); current and previous luma L0 | `Motion()` | (W/8, H/8) |
| 9 | copy | scene-change output | readback slot | |

Which luma pyramid is current flips with each confirmed frame. Which flow pyramid is A or B flips with
each level, exactly as `dispatch()` in `ffx_opticalflow.cpp` alternates them. So the scale output at
level L is the search's prediction at L-1.

**Resources, per extent.** 2 luma pyramids x 7 levels (R8_UINT, W>>L x H>>L), 2 flow pyramids x
7 levels (R16G16_SINT, ceil(W/8) halving with round-up), the final L0 flow, the four scene-change
textures, `Motion()` (R16G16_FLOAT, W x H), and a 2 KB readback buffer (4 slots of 512 bytes). At
3440x1440 that is about 13 MB of luma, 20 MB of motion, and under 1 MB for the rest.

## Root signature and descriptors

One root signature for every pass:

| Param | Contents |
|---|---|
| 0 | descriptor table, UAV u0..u7 (volatile descriptors and data) |
| 1 | descriptor table, SRV t0: the colour |
| 2 | 8 root constants at b0: FFX's `cbOF`, or the expand/clear constants |
| 3 | 8 root constants at b1: FFX's `cbOF_SPD` (pyramid only) |

No samplers: every pass loads.

One shader-visible heap per allocation, 520 descriptors:
- 2 parities x 32 dispatch slots x 8 UAVs. Unused entries hold null views. Each table is written once,
  on first use after the allocation.
- A ring of 8 colour SRVs, because the colour resource can change from frame to frame.

The heap is created under `ScopedSkipHeapCapture`, and every resource and view under
`ScopedInternalResourceCreation`. So the heap tracking and the DLSS-NR exposure scan never see them;
the 3x1 scene-change textures would otherwise look like exposure candidates.

## Cost

Estimated, not measured: roughly 0.3-0.6 ms at 3440x1440 on an RTX 4090. That is 30 small compute
dispatches, most of them on the 8x8-block grid or smaller. The two full-resolution passes are the luma
preparation and the expand, each one read and one write per pixel. AMD's published FSR3
frame-generation cost (0.7-2.1 ms at 1080p across GPUs) includes this optical flow and more. The
loopback harness will replace this estimate with a measurement.

The expand's per-pixel choice (2026-09-28) was measured in `tests/synth-motion-d3d12`, with clocks
locked on the RTX 4090. The whole estimator went from 0.310-0.316 to 0.328 ms at 1080p, and from
0.591-0.612 to 0.629-0.631 ms at 3440x1440, on uniform pans, where every pixel is tested.

## Provenance and licence

Ported from the AMD FidelityFX SDK 2.3.0 (`external/FidelityFX-SDK-v2`, commit `60f4ea8`):

- **Copied unchanged into `precompile/ffx/`:** `api/internal/gpu/ffx_core.h`, `ffx_common_types.h`,
  `ffx_core_hlsl.h`, `ffx_core_gpu_common.h`, `ffx_core_gpu_common_half.h`, `ffx_core_portability.h`;
  `framegeneration/fsr3/include/gpu/spd/ffx_spd.h`; and
  `framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_` + `common`, `prepare_luma`,
  `compute_luminance_pyramid`, `generate_scd_histogram`, `compute_scd_divergence`,
  `compute_optical_flow_v5`, `filter_optical_flow_v5`, `scale_optical_flow_advanced_v5` `.h`.
- **Derived (bindings changed, pass bodies unchanged):** `precompile/synth_motion_callbacks.h` from
  `ffx_opticalflow_callbacks_hlsl.h`, and the seven `synth_motion_*.hlsl` passes from
  `internal/shaders/ffx_opticalflow_*_pass(_v5).hlsl`. Each carries FFX's licence and says what changed.
- **Ours:** `synth_motion_expand.hlsl`, `synth_motion_clear.hlsl`, `SynthMotion_Dx12.{h,cpp}`. The host
  code follows `internal/ffx_opticalflow.cpp` for sizes, ping-pong and dispatch dimensions.

Every copied or derived file was checked against the SDK's `docs/license.md`. Each is listed under
MIT, whereas the rest of that SDK is binary-only. The MIT notice ships as
`Licenses/FidelityFX_OpticalFlow_ATTRIBUTION.txt`.

## The second source: NVIDIA Optical Flow (`NvofaEstimator_Dx12`)

`SynthMotionNvofa_Dx12.{h,cpp}` meets the same output contract, but the search runs on NVIDIA's
fixed-function optical-flow engine through `nvofapi64.dll`. That DLL is loaded at runtime from the
driver on Windows, or from dxvk-nvapi under Proton. `[DlssNr] SynthMotionSource=nvofa` selects it,
and `DlssNr::SynthMotionGuide` falls back to the estimator above whenever it is unavailable. Design:
"Motion sources" in [synthesized-motion.md](../../dlssnr/design/synthesized-motion.md).

Where it differs from the table above:

- **`Record` takes the queue the list will execute on.** The engine is fenced to it: a GPU wait is
  queued before the list, and `ConfirmExecuted` signals after it and submits the engine.
- **The field is one frame late.** `Ready()` is false after a reset until two frames are confirmed.
- **`SceneCut()` is always false.** The engine has no scene-change output.
- **Passes.** Two, both ours:
  - `synth_motion_nvofa_prep.hlsl`: colour to `R8_UNORM` luma at up to 540 lines;
  - `synth_motion_nvofa_expand.hlsl`: the S10.5 `R16G16_SINT` grid to per-pixel `R16G16_FLOAT` in
    colour pixels.
- **Headers.** The NVIDIA headers are MIT, vendored in `OptiScaler/include/nvofa/`, with the notice
  in `Licenses/NVIDIA_OpticalFlow_ATTRIBUTION.txt`.
- **Until `build.sh` generates `SynthMotion_NvofaPrep_Shader.h` and `SynthMotion_NvofaExpand_Shader.h`,**
  the source compiles to "unavailable" (`__has_include`).

## Rebuilding the shaders

`precompile/build.sh` runs dxc through the msvc-wine prefix and regenerates the `*_Shader.h` headers. It
uses AMD's flags for the base permutation: `cs_6_2`, `FFX_HLSL_SM=62`, `FFX_HALF=0`, wave32, no msad4.
Never run it while `build-local.sh` holds the same prefix.

No SPIR-V is built: there is no Vulkan consumer yet. A Vulkan port would add a `-spirv` target of the
same sources.
