# LSFG 3.1 inside OptiScaler: integration design

Status: **option (b) is built** on this branch (2026-10-13, `retoque/lsfg/Lsfg.{h,cpp}`,
`LsfgFrameGen_Dx12.{h,cpp}`, `LsfgGraph.h`, `LsfgDxil.h`, `LsfgShaderSource.{h,cpp}`; host suite
`tests/lsfg-integration`) and ran in PCSX2 on native D3D12 under vkd3d-proton (x2, SDR, quality, flow 1.0, NR on).
What differs from the design below, for this step:

- Shaders are precompiled DXIL read from `[LSFG] ShaderDir` (`<rid>.cso`, made by `convert.py`) behind
  `IShaderSource`; the run-time converter (`LsfgShaderLibrary`) is meant to plug in behind it. No `DllPath` and no
  cache yet.
- Root signatures come from each module's PSV0 part (`LsfgDxil.h`): no dxcompiler.dll, no pinned register. PSV0 has
  no names, so `LsfgGraph.h` `CbuffersOf` says whether a lone `b0` is `cb` or the push constants, and a module whose
  CBV count disagrees is refused.
- Keys are `Enabled`, `FlowScale`, `PerformanceMode`, `ShaderDir`; no `HalfPrecision` (FP32 only) and no menu rows.
- HDR10 and scRGB are refused (FFX interpolates), so section 6 is not implemented.
- FFX keeps interpolating while LSFG builds on a worker thread (the decision is latched per base frame in
  `FSRFG_Dx12::Dispatch`), instead of showing only real frames.

**GPU cost, second step (2026-10-13).** PCSX2's ~1.0 ms per generated frame against the harness's ~0.5 ms was mostly
the GPU's clock, not the integration. At 1691x1312 PCSX2 keeps the RTX 4090 about 38% busy, and the driver holds it
in P3: graphics ~1650 MHz and memory 5-9 GHz (nvidia-smi every 250 ms), where the harness ran at 2730-2760 MHz and
10.5 GHz. In game the cost tracks the core clock: ms × MHz is about constant. What changed:

- **Barrier waves** (`LsfgGraph.h` `ScheduleWaves`): the harness's barrier around every dispatch (about 200
  `ResourceBarrier` calls for 100 dispatches) became one batch per wave of mutually independent passes. That makes
  10 waves for a frame that only ingests and 42 for one that also generates. The seven alpha levels run side by
  side, the delta chain runs alongside the gamma chain, gamma, delta and epsilon run together at levels 4-6, and
  the main pass's first levels overlap the pre-pass's beta chain. The gamma chain (7 levels × 5) is the floor.
  Each batch holds only the transitions and UAV barriers that wave needs. A per-image dirty flag carries an
  unordered UAV write across waves and across lists.
- **Direct output**: generate writes FFX's `outputs[0]` through a UAV written per frame into an 8-slot ring at the
  end of LSFG's heap. Nothing is cached by pointer, because FFX alternates two outputs and may recreate them.
  The copy-out stays only when D3D12 forbids the UAV: no `ALLOW_UNORDERED_ACCESS`, a size mismatch, or an `_SRGB`
  output (`DirectOutputFormat`). The log names which. FFX hands over UAV-capable RGBA8 in `UNORDERED_ACCESS`
  here, so PCSX2 takes the direct route.
- **The ingest copy stays.** The shaders sample the frame as one slice of a two-slice `Texture2DArray` (`0.hlsl`
  `Input`), and FFX's single backbuffer cannot be bound as a slice of it.
- **The synthesized input's HUD mask** (`[FrameGen] SynthesizedHudDepth`, on by default) is no longer computed
  while LSFG takes the frames. Its depth fed only FFX's prepare, which LSFG frames skip. The design note measured
  it at 0.080 ms at 1080p and 0.174 ms at 3440x1440, at full clock (synthesized-frame-generation.md). The UI layer,
  which FFX still composes, keeps it. The **motion estimate stays**: DLSS-NR takes it here (`[DlssNr]
  SynthMotion`), and its scene cuts reset LSFG as well as FFX.
- FFX's prepare and optical flow were already skipped on LSFG frames. The idle FFX context costs VRAM, not GPU time.

Measured on the RTX 4090 at 1691x1312, quality, FP32, flow 1.0, in ms per generated frame. The bench rows are
`tools/lsfg-study/ctxbench` at full clock, 1500 frames each. The game rows are PCSX2 under vkd3d-proton, NR on,
from the 10 s summary windows at 1691x1312 without a mode change in them, at ~1650-1700 MHz. The A/B rows alternate
the two schedules frame by frame, so both run at the same clocks.

| | p50 | p99 |
|---|---|---|
| bench, `0ba40b6a` (before) | 0.607 | 0.637 |
| bench, waves, copy-out forced (`--output-copy`) | 0.469 | 0.487 |
| bench, waves, direct output | 0.445 | 0.491 |
| PCSX2 before (steady windows) | 0.96-0.99 | 1.00-1.05 |
| PCSX2 `OPTISCALER_LSFG_AB=1`, same frames: harness's schedule | 0.89-0.99 | 0.99-1.04 |
| PCSX2 `OPTISCALER_LSFG_AB=1`, same frames: waves, direct | 0.67-0.76 | 0.76-0.81 |
| PCSX2 after (steady windows) | 0.68-0.74 | 0.78-0.81 |

- **Same images.** The bench hashes every generated frame of a 16-frame moving sequence (15 generated). The
  hashes are identical across the `0ba40b6a` code, waves with direct output, waves with the copy-out, the
  harness's schedule, and the two alternating frame by frame. BGRA8 is identical to `0ba40b6a` too. The reason is
  structural: every dispatch reads the same finished inputs through the same views, only in a different order.
- **Where the rest goes** (`ctxbench --wave-timing`, full clock): a wave of a few dozen thread groups costs ~4 µs
  however little it does, and 35 of the 42 waves sit on the gamma chain. The heavy items are the ingest copy
  15 µs, mipmaps 18, the alpha waves 63 together, the beta and level-0 waves 62, the level-6 waves ~80, and
  generate 31. Any further gain is inside the shaders (FP16, `FP16_NOTES.md`), not in the recording.
- **A lighter frame can cost more.** Two later runs never left PCSX2's half-width mode (841x1356). The driver then
  kept the GPU in P5 61% of the time, with memory at 810 MHz there: averages ~1270 MHz graphics and ~3.4 GHz memory.
  At half the pixels, on the same frames (A/B), LSFG took p50 0.81-1.19 ms with the waves against 1.17-1.46 ms with
  the old schedule.
- **Open problems.**
  - In game the GPU clock decides: 0.445 ms at full clock is ~0.7 ms at P3 clocks, and over 1 ms in P5. That is the
    driver's DVFS, which nothing in OptiScaler controls. Pinning clocks, or the driver's "prefer maximum
    performance" mode, would be a system setting, and neither was changed for these runs.
  - `OPTISCALER_LSFG_AB` is a study switch read from the environment, with no ini key.
  - Native Windows with the debug layer is still needed for the new barrier batches. vkd3d-proton does not
    validate them (CLAUDE.md, "Proton is not a D3D12 validator").

**x3 and x4 (option a′) are built too**: `tools/lsfg-study/MFG_PRESENTER.md`, `retoque/lsfg/presenter/`, `[LSFG]
Multiplier`. The "x2 only" and "no `Multiplier`" statements below describe FFX's swapchain, which `Multiplier=2` keeps.

The rest of this note is the design as written before any of it existed.

It is the third part of the study: the harness (`tools/lsfg-study/harness/`) runs LSFG's pass graph on D3D12, a
companion spec documents that graph with reference frames, and this note says how the harness becomes a frame
generator inside OptiScaler. Every number below names where it came from. Code citations are `file:line` against
this worktree. Two abbreviations:

- **FISC** = `external/FidelityFX-SDK-v2/Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12.{h,cpp}`
  (SDK 2.3.0, MIT per `docs/license.md:507`; checked out in the main tree, not in this worktree).
- **lsfg-vk** = lsfg-vk v2.0.0 (`2333707`), read at `~/.cache/aur-review/lsfg-vk-src`.

## Read this first: it stays a study

[synthesized-frame-generation.md, "What the research ruled out"](../../docs/dlssnr/design/synthesized-frame-generation.md#what-the-research-ruled-out)
already settled the legal side, and nothing here changes it:

- LSFG's shaders are Lossless Scaling's, and they are proprietary. `lsfg-vk.dll` ships them for lsfg-vk, not for us.
- A build anyone else runs needs the author's written permission.
- **No LS-derived bytes in git.** That covers SPIR-V, HLSL, DXIL, PNGs made by LSFG and `layout.json`. Generated files
  stay in `Tmp/lsfg-study/`, as `tools/lsfg-study/README.md` already does.
- **Nothing of this merges into `dlss-neural-rendering`.** That branch is mirrored publicly within minutes (CLAUDE.md).
- The pass graph the harness follows is read from lsfg-vk 2.0.0, which is **CC BY-NC-ND 4.0**
  (`lsfg-vk-pipeline/src/*.cpp` SPDX headers). Reading it for a private study is fine. Publishing code derived from it
  is not. lsfg-vk v1.0.0 (MIT) and the v2.0.0-dev snapshot (GPLv3) are the references that would not have that problem.

If this ever became code on a real branch, the note would move to `docs/` first, because DEVELOPMENT.md requires the
design document before the code.

## Summary

- **Recommendation: option (b).** LSFG becomes the interpolator behind FSR-FG's own swapchain, through the
  `frameGenerationCallback` that OptiScaler already installs (`OptiScaler/framegen/ffx/FSRFG_Dx12.cpp:507-519`). Today
  that callback hands every frame to FFX's interpolation in one line (`:1051`), and that line is where LSFG goes.
  - FFX keeps everything else: pacing, the present thread, UI composition, resize and fullscreen handling.
  - Both target routes (PCSX2 on native D3D12, Divinity on the D3D11 bridge) reach that callback without edits to the
    bridge or the hooks.
- **The cost of (b) is the multiplier.** FFX's swapchain asks for exactly one generated frame (`FISC.cpp:1927`), and its
  ABI-frozen pacing record holds one interpolated frame and one real one (`FISC.h:45-50`). So (b) is x2 only.
  - x3 and x4 need option (a′): a fork of that MIT swapchain, widened to N frames.
- **Inputs.** LSFG reads only the current presented frame. It keeps the previous one itself, in a two-slot array.
  - The current frame is the callback's `presentColor`, and NR has already edited it on both routes, so generated frames
    inherit the NR edit.
  - `FGInput::Synthesized` is the input this targets.
- **Shaders come from the user's own `lsfg-vk.dll`, at run time.** The pipeline is SPIR-V → HLSL (spirv-cross, linked
  in) → DXIL (dxcompiler.dll + dxil.dll), compiled on a worker thread and cached per DLL hash outside the game and the
  repo.
  - Precompiled DXIL is never shipped.
  - Lossless.dll's DXBC is a parked alternative.
- **HDR.** The mode follows FFX's `backbufferTransferFunction`: SRGB → (kind 0, support 0), PQ → (0, 0) as a proposal,
  scRGB → (2, 1). Only the mipmaps pass reads these values. Output precision is whatever the UAV view's format is.

## 1. Where LSFG plugs in

### 1.1 What FSR-FG already does with the callback

The hook point exists, and OptiScaler already uses it:

1. **Configure, every base frame.** `FSRFG_Dx12::Dispatch` (`FSRFG_Dx12.cpp:327`) configures the FG context with
   `ffxConfigureDescFrameGeneration`, which has `frameGenerationCallback` and `frameGenerationCallbackUserContext` at
   `ffx_framegeneration.h:109-110` (`external/FidelityFX-SDK/ffx-api/include/ffx_api/`, the header the build uses).
   - It always sets the callback to itself (`FSRFG_Dx12.cpp:507-519`).
   - It sets `presentCallback` only when reprojection is on (`:485-500`).
   - It then records FFX's prepare (`:535-646`). The prepare needs depth and velocity, and the function refuses to run
     without them (`:355-362`).
2. **Hand-off to the swapchain.** The FG context passes the callback to the swapchain (FFX provider,
   `framegeneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp:218-332` in SDK v2).
3. **Present.** At the game's present (`o_FGSCPresent`, `OptiScaler/hooks/FG_Hooks.cpp:1484-1487`), FFX's `Present`
   (`FISC.cpp:2150`) interpolates when `interpolationEnabled && callback set` (`:2167-2169`). `dispatchInterpolationCommands`
   (`:1890`) then builds an `ffxDispatchDescFrameGeneration` (`ffx_framegeneration.h:84-97`) with these fields:
   - `commandList`: FFX's interpolation list. It runs on the game queue, or on FFX's async compute queue with
     `allowAsyncWorkloads` (`FISC.cpp:1220`, `:1497-1510`). OptiScaler sets that flag from `[FrameGen] FGAsync`
     (`FSRFG_Dx12.cpp:458`).
   - `presentColor`: FFX's copy of the backbuffer, in `PRESENT` state (`FISC.cpp:1898`, `:1925`).
   - `outputs[0]`: FFX's interpolation output, in the backbuffer's format, in `UNORDERED_ACCESS` (`:1072-1083`,
     `:1655-1660`, `:1924`).
   - `numGeneratedFrames = 1` (`:1927`).
   - `reset`, `backbufferTransferFunction`, `minMaxLuminance`, `generationRect` and `frameID`.
4. **Run and present.** FFX calls the callback (`:1934`) and executes the list. If `numGeneratedFrames > 0`, the output is
   shown before the real frame (`:1946-1950`, `:1990-2010`). The game queue is signalled and the interpolation queue
   waits on it first (`:1959-1962`), so the list sees everything the game and NR submitted.
5. **OptiScaler's callback.** `FSRFG_Dx12::DispatchCallback` (`FSRFG_Dx12.cpp:843-1057`) handles the reset policy
   (`:889-892`) and cancellation (`:897-918`), and records its HUD helpers onto `params->commandList` (`:961-1049`).
   It then hands the descriptor to FFX: `FfxApiProxy::D3D12_Dispatch(&_fgContext, &params->header)` (`:1051`).

`FSRFG_Vk` has the same callback (`FSRFG_Vk.cpp:633-635`, `:712`). A later study could run `lsfg-vk.dll`'s SPIR-V there,
unconverted, for Vulkan titles.

FFX has a second hook, the swapchain queries `..._INTERPOLATIONCOMMANDLIST_DX12` and `..._INTERPOLATIONTEXTURE_DX12`
(`ffx_api_dx12.h:77-89`, consumed at `FISC.cpp:1906-1915`). It is no better here:
- interpolation still has to be enabled through the FG context;
- it records before Present, not at it.

### 1.2 Option (a): `FGOutput::LSFG` with its own presenter

A new `LSFG_Dx12 : IFGFeature_Dx12` would own a DXGI swapchain wrapper.

**The precedent does not help.** `Reprojection_Dx12` is the precedent for an output that owns its wrapper. But
`ReprojectionDXGISwapChain::Present` just calls the real swapchain (`framegen/reprojection/swapchain/reprojectionSwapchain.cpp:356-371`):
no replacement buffers, no pacing.

**Everything else would be new.** FFX's swapchain does all of this, in 2,623 lines plus helpers:
- replacement backbuffers;
- a presenter thread and a pacer thread;
- the frame-latency waitable object;
- UI composition;
- fullscreen and resize handling;
- HDR metadata.

**It touches about 20 sites in eight upstream files:**
- `State.h`: the enum and its mapping, appended after `Reprojection`, because the menu indexes by value.
- `FG_Hooks.cpp`: `CheckForFGStatus` (`:177-244`), both factory paths (`:263-282`, `:373-392`), and the per-output
  branches in `FGPresent` (`:1250`, `:1353`, `:1468`).
- `wrapped/wrapped_swapchain.cpp`: `:290`, `:498`, `:517`, `:965`, `:1360`, `:1402`, `:1582`.
- `with_dx12/dx11_with_dx12_sc.cpp`: `:35`, which admits Synthesized for FSR-FG and DLSS-G only.
- `dllmain.cpp:1616-1633`, `Config.cpp:161-186`, `menu/menu_common.cpp:3686-3746`, and the fork-owned
  `retoque/inputs/FG/Synth_Inputs.cpp:53`.

**Pacing would be ours to write.** There are two models:
- **lsfg-vk's "vsync" mode** forces FIFO and presents the generated frames back to back before the real one
  (`lsfg-vk-layer/src/modes/vsync.cpp:160-247`). That works only when refresh = N × base.
  - This workstation's display is 3440x1440 at 165 Hz with VRR off. That is the profile's policy, so pacing cannot
    lean on VRR.
  - At 165 Hz, a 60 fps emulator at x2 would judder; an uncapped game would fill the FIFO and add latency.
- **A QPC pacer like FFX's.** Its interpolation thread waits for the generation fence. It then schedules the generated
  frame ½ × the 10-frame mean frame time, minus the variance and a safety margin, ahead of the real one
  (`FISC.cpp:836-886`; the 0.5 is at `:883`).

**Option (a′) is the variant worth keeping.**
1. Fork FISC (MIT) into `retoque/lsfg/swapchain/`.
2. Widen `FrameType` and `PacingData` from `{Interpolated_1, Real}` to N interpolated frames. The "no modifications ...
   ABI stability" comments (`FISC.h:52`, `:68`) protect the contract with FFX's provider DLL, and a private copy has no
   provider talking to it.
3. Schedule each generated frame at `avg/N` instead of `avg × 0.5`.
4. Call LSFG directly.

This is the only route to x3 and x4. It reuses FFX's tested pacing instead of writing a new one.

### 1.3 Option (b): LSFG as FSR-FG's interpolator

`FGOutput=fsrfg` stays. `[LSFG] Enabled=true` makes three changes inside `FSRFG_Dx12`, and changes nothing else:

- **No depth/velocity gate** (`:355-362`). LSFG does not read them.
- **No FFX prepare** (`:535-646`). It would cost GPU time for an interpolation that never runs.
- **The callback calls LSFG** instead of `D3D12_Dispatch` (`:1051`).

The FG context is still created (`CreateContext`, `:1254-1407`), because the callback reaches the swapchain only through
that context's configure. The synthesized input keeps feeding the zero pair. It drives FSR-FG's frame clock
(`EvaluateState`, `StartNewFrame`, `SetReset` and `SetResource` at `Synth_Inputs.cpp:1097`, `:1126`, `:1135`,
`:1185-1186`). LSFG simply ignores the pair.

### 1.4 Comparison

| | (a) own presenter (a′ = FFX fork) | **(b) FSR-FG callback** |
|---|---|---|
| Pacing | Ours to write. FIFO pacing is poor with VRR off; (a′) inherits FFX's pacer | FFX's QPC pacer, as it runs today, with OptiScaler's `FGFramePacingTuning` keys (`FSRFG_Dx12.cpp:260-296`) |
| Latency | FIFO: at least one queued base frame. (a′): same as (b) | Same as FSR-FG: the real frame is held about ½ base frame plus generation time, because the presenter waits for the interpolation fence (`FISC.cpp:836-858`). Expected below Lossless Scaling's own overlay, which also pays capture and composition |
| Multiplier | (a′): any N, LSFG caches the pre-pass per base frame | **x2 only**: `numGeneratedFrames = 1` (`FISC.cpp:1927`) and two output buffers (`FISC.h:43`). `outputs[4]` in the header is unused by this swapchain |
| HUD / UI | Ours. (a′) inherits FFX's UI composition | FFX composes the registered UI layer over every presented frame, including LSFG's. `SynthesizedHudLayer` keeps working (`FSRFG_Dx12.cpp:411-426`) |
| NR ordering | Same hook, if the wrapper registers as `currentFGSwapchain` | Unchanged: NR runs before `o_FGSCPresent` (§2) |
| D3D11 bridge | Needs `dx11_with_dx12_sc.cpp:35` and `FG_Hooks.cpp:188-199` | Already admitted for Synthesized + FSR-FG |
| Upstream surface | About 20 sites in 8 files, plus 2.6k lines for (a′) | 4 one-line hooks in `FSRFG_Dx12.cpp`, one config block, one menu call |
| VRAM | LSFG only | LSFG plus an idle FFX FG context (§8) |
| Effort | Weeks: presenter, pacing, fullscreen and resize, overlay interplay | The harness core as a class, about 300 lines of adapter, the shader provisioning, config and menu |

### 1.5 Recommendation

**Build (b).** Pacing is the part most likely to go wrong, on a fixed-refresh display and on the D3D11 bridge, and
synthesized-frame-generation.md chose to feed FSR-FG for exactly that reason. (b) answers the study's real question,
whether LSFG's interpolation beats FFX's on titles without motion vectors, with nothing else changed. Pacing, latency
and the UI layer stay identical, so an A/B measures the interpolator alone.

**x3 and x4 come later, as (a′).** Write the adapter so it already loops `k = 1..numGeneratedFrames` into `outputs[k-1]`.
A future FFX swapchain that asks for more frames, or (a′) itself, then needs no change in the LSFG code.

### 1.6 The (b) call sites

**In upstream files** (one-liners, as the NR module's call sites are):

1. `FSRFG_Dx12.cpp:355`: skip the depth/velocity gate when `Lsfg::Interpolates(fIndex)`.
2. `FSRFG_Dx12.cpp:535`: skip the prepare block under the same condition, and set `dispatchResult = true`.
3. `FSRFG_Dx12.cpp:1051`:
   `auto dispatchResult = Lsfg::Interpolates(fIndex) ? Lsfg::FfxDispatch(params) : FfxApiProxy::D3D12_Dispatch(...)`.
4. `FSRFG_Dx12.cpp:1235-1242` (`ReleaseSwapchain`): after FFX's swapchain context is destroyed, call
   `Lsfg::OnSwapchainReleased()`. FFX has drained its queues by then (§4.6).
5. `Config.h/.cpp` and `OptiScaler.ini`: the `[LSFG]` block, marked "removable as one block" like `[DlssNr]` (§5).
6. `menu_common.cpp`: one `Lsfg::RenderMenuRows(ctx)` inside the FSR FG card (`:4376`), plus guards on the rows that
   only feed FFX's interpolation (§5).

**In fork-owned files:**

- New: `retoque/lsfg/`. It resolves through the existing `..\retoque\` include root, as `<lsfg/Lsfg.h>`.
- `Synth_Inputs.cpp`: `MotionWanted()` is false under LSFG. Nobody reads the field, unless NR wants it through the
  handoff.

**The decision is latched per base frame.** `Interpolates()` is read at `Dispatch` for frame N and reused by the callback
for the same `frameID`. The two must agree: once FFX's prepare is skipped, FFX's interpolation cannot run that frame.

- If LSFG is not ready when the callback arrives (shaders still compiling, context rebuilding), the callback sets
  `numGeneratedFrames = 0`. FFX then presents only the real frame (`FISC.cpp:1946`).
- Switching interpolator in the menu sets `state.fgChanged`, which pauses about 10 frames (`UpdateTarget`, as the card's
  Async toggle does). FFX's interpolation therefore never sees a frame whose prepare was skipped.

## 2. Inputs

### 2.1 What LSFG consumes

- **Source.** A two-slot source array. `cb_SourceIndex % 2` selects the current slot and the other slot is the previous
  frame (mipmaps `Tmp/lsfg-study/hlsl/0.hlsl:41`, generate `2.hlsl:57-58`).
  - Each base frame the host writes the new frame into slot `iteration % 2`. lsfg-vk does this with a blit from the
    swapchain image (`lsfg-vk-layer/src/wrapper.cpp:286`, `:296`).
  - No depth, motion, camera or jitter is read.
- **Ingest.** In (b) the current frame is `params->presentColor`, and ingest is one `CopyTextureRegion` into the slot.
  - The array is only ever sampled (mipmaps and generate read it, nothing writes it as a UAV), so it is created in
    `presentColor`'s own format: `B8G8R8A8`, `R8G8B8A8`, `R10G10B10A2` or `R16G16B16A16_FLOAT`. No conversion pass.
  - A BGRA view samples as RGBA, just as lsfg-vk's blit swizzles.
  - Flip-model swapchains cannot be `_SRGB`, so the blit's sRGB-decode case does not arise on D3D12.

### 2.2 Native D3D12 (PCSX2)

The order in `FGHooks::FGPresent`:

1. `FeedSynthD3D12` (`FG_Hooks.cpp:1282-1283`): the frame clock and the zero pair.
2. `fg->Present()` → `Dispatch` (`:1379`): configure only, under LSFG.
3. `DlssNr::RunPresentPass` (`:1444`): NR edits the backbuffer in place.
4. `FinishSynthD3D12` (`:1458-1459`): the optional UI layer, cut after NR.
5. `o_FGSCPresent` (`:1484-1487`): FFX copies the backbuffer, then the callback runs LSFG on it.

### 2.3 D3D11 bridge (Divinity)

1. `_CopyDx11SharedToDx12FGBackBuffer` (`with_dx12/dx11_with_dx12_sc.cpp:2156`) copies the game's frame, or the NR host's
   output (`_nrHost->Record`, `:2220`), into the FG presenter's backbuffer (`:2249`).
2. `_synthInputs->Feed` (`:1008`), then `_fgSwapChain->Present` (`:1010`).
3. That present enters `FGHooks::FGPresent`, and from there it is the same path as §2.2.

The bridge already builds an FSR-FG presenter for Synthesized (`:34-35`), so (b) needs no bridge edit. The callback's
command list belongs to the bridge's D3D12 device. The adapter takes the device from that list
(`cmdList->GetDevice`), as FSR-FG's own helpers do ("do not blindly trust the cached `_device`",
`FSRFG_Dx12.cpp:983-997`).

### 2.4 After NR

- **Synthesized.** Generated frames are blends of two NR-edited frames, so they carry the edit for free. NR still runs
  once per base frame: for `Synthesized`, the wrapped-swapchain entry stands down whenever an FG swapchain exists
  (`retoque/shaders/dlssnr/DlssNr_Dx12.cpp:6941-6946`). That rule depends on the FFX swapchain being
  `currentFGSwapchain`, which (b) keeps.
- **Upscaler.** NR edits the upscaled frame, the game draws its HUD over it, and LSFG interpolates the result.

### 2.5 Which `FGInput` values make sense

| Input | With LSFG through (b) |
|---|---|
| `Synthesized` | **The target.** No upscaler, no motion; works on native D3D12 and on the bridge |
| `Upscaler` (OptiFG) | Works: LSFG ignores the depth and motion it is handed. FSR-FG with real vectors is usually the better choice there. A later step could feed LSFG the hudless frame and let FFX compose the UI |
| `DLSSG`, `FSRFG`, `FSRFG30`, `XeFG` | Reach the same callback and work mechanically; the game's own FG data goes unused. Not a target |
| `NvngxFG`, `NoFG` | Never reach an `FGOutput` (`FG_Hooks.cpp:183-184`) |

## 3. Shader loading

### 3.1 Finding `lsfg-vk.dll`

This mirrors lsfg-vk's `findDll`/`fixDllPath` (`lsfg-vk-config/src/config.cpp:451-488`, `:584-595`). The first hit wins:

1. **`[LSFG] DllPath`**, as `[Libraries] NvapiPath` does. It accepts the Lossless Scaling folder, `Lossless.dll`, or
   `lsfg-vk.dll`, and the first two resolve to `lsfg-vk.dll` beside them, as `fixDllPath` does.
2. **Windows.** Steam's root comes from `HKCU\Software\Valve\Steam\SteamPath`, falling back to `HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath`.
   Then every library listed in `steamapps\libraryfolders.vdf` is tried, as
   `<library>\steamapps\common\Lossless Scaling\lsfg-vk.dll`. `appmanifest_993090.acf` (Lossless Scaling's app id)
   names the library that holds it.
3. **Proton**, detected with `State::isRunningOnLinux` via `wine_get_version`.
   - Unix environment variables are visible in the Wine process.
   - Start from `STEAM_COMPAT_CLIENT_INSTALL_PATH`, which Steam sets for compatibility tools.
   - Fall back to lsfg-vk's list relative to `$XDG_DATA_HOME` and then `$HOME`: `.local/share/Steam`, `.steam/steam`,
     `.steam/debian-installation`, and the Flatpak and Snap paths (`config.cpp:452-458`).
   - Read `libraryfolders.vdf` the same way. Every Unix path goes through kernel32's `wine_get_dos_file_name` to become
     a `Z:` path, rather than string surgery.
   - pressure-vessel shares the home directory and the Steam libraries with the container. Confirm it from the log on
     the first run.
4. **Not found.** No LSFG. The menu says why and the log lists what was searched (§5).

On this workstation the install is the default library's `steamapps/common/Lossless Scaling/`, which holds both
`lsfg-vk.dll` and `Lossless.dll` (the `lsfg-vk` beta branch).

### 3.2 Extracting the modules

`lsfg-vk.dll` keeps the SPIR-V as `RT_RCDATA` resources. The id table is in `lsfg-vk-pipeline/src/library.cpp`:

- Base shaders: mipmaps 0, `generate_8bit` 1, `generate_16bit` 2.
- 24 core shaders at 3..26. Performance variants are +24, FP16 variants +48.
- **Mipmaps is a named entry, not an id.** lsfg-vk looks it up as `2147488584` (`0x80001348`). Its high bit marks a
  name-string offset, so `FindResourceW(MAKEINTRESOURCE(id))` cannot reach it. The two extraction options:
  - a small PE walker over the file bytes, which behaves the same on Windows and under Wine;
  - `EnumResourceNamesW`.

**The walker.** Header-only and free of `<windows.h>`. It sits beside `retoque/dlssnr/DlssNr_PeScan.h`, which is the
precedent: byte scans tested on the host tier against synthetic images. It must be written fresh, because lsfg-vk's
`library/dll.cpp` is ND-licensed.

**Validate before use.** Refuse the DLL, with a log line, on any mismatch:
- exactly the 99 expected modules;
- the SPIR-V magic `0x07230203`;
- the binding layout matching what the graph expects (56 distinct bindings across all modules, at most 8 per shader,
  per `Tmp/lsfg-study/layout.json`).

A Lossless Scaling update can change any of this.

### 3.3 SPIR-V to something D3D12 runs

| Route | For | Against | Verdict |
|---|---|---|---|
| **Ship precompiled DXIL** in OptiScaler.dll, as `precompile/` headers | Simplest. No compiler at run time | Redistributes LS-derived bytecode, and a public repository would hold it. Breaks the moment LS updates its shaders | **Ruled out** |
| **Convert at run time in-process**: spirv-cross (Apache-2.0, linked) → HLSL SM 6.0 → `dxcompiler.dll` + `dxil.dll` → DXIL | Exactly what the harness validated (`convert.py`, 99/99). Works under Wine, since `dxc.exe` runs there today. Follows the user's DLL automatically | Ship about 20 MB of compiler DLLs. First-run compile time, unmeasured. Native Windows needs DXIL signed by the validator: `dxil.dll` beside `dxcompiler.dll` signs it, which is why `OptiScaler/shaders/shader_tools/` carries both. vkd3d-proton does not check | **Recommended** |
| **Lossless.dll's 202 DXBC blobs**, used as-is (D3D12 accepts SM 5.x DXBC) | No conversion, no compiler. What LS itself runs on Windows | A different graph from `lsfg-vk.dll`'s: D3D11 register slots, unknown constant layouts, ids that move with LS updates. The harness, the companion spec and the `lsfg-vk-cli` reference frames all describe the SPIR-V graph. lsfg-vk v1.0.0 (MIT) mapped this graph once and would be the reference | **Parked**: a second study if the SPIR-V route hits a wall |
| **spirv-cross → SM 5.1 → `D3DCompile`** (`d3dcompiler_47`, already delay-loaded: `OptiScaler.vcxproj:206`, `shaders/Shader_Common.cpp:14`) | No new DLL. The HLSL needs nothing above SM 5.1: no wave operations or 16-bit types (grep over `Tmp/lsfg-study/hlsl/`) | Under Proton this is Wine's builtin `d3dcompiler_47`, a reimplementation we cannot count on for 99 machine-generated shaders up to about 70 KB of SPIR-V each. fxc is also slow on large unrolled shaders | Rejected for Proton; a possible Windows-only fallback |

### 3.4 Recommendation and cache

**When.** Convert on a worker thread, the first time LSFG is switched on, and never at DLL load.

- PSO creation is free-threaded, so it runs on the same worker.
- Until it is done, the callback reports `numGeneratedFrames = 0`, so the real frames keep flowing.
- The menu shows "Preparing LSFG shaders (n/99)".

**Where.** The cache is `%LOCALAPPDATA%\OptiScaler\lsfg\<sha256(lsfg-vk.dll)>\<converter version>\`, holding one `.dxil`
per resource id plus the binding table.

- Under Proton that sits inside each game's prefix. That is acceptable: one compile per prefix.
- It is a derived copy on the user's own disk, like lsfg-vk's own pipeline cache. It is never written to the game
  folder or the repo.

**Converter changes relative to `convert.py`:**

- **Pin the push-constant cbuffer register.** spirv-cross leaves `type_PushConstant_PC` unregistered (`3.hlsl:11`), and
  the harness finds its register through reflection. A fixed register, for example `b0, space100`, means root
  signatures need no runtime reflection.
- **Emit the binding table**, the equivalent of `layout.json`, into the cache.
- **For scRGB only, declare the generate output as `RWTexture2D<float4>`**, not `<unorm float4>` (§6).

## 4. Resources and lifetime on D3D12

### 4.1 The graph and its images

The graph has 27 shaders and runs 100 dispatches: 34 in the pre-pass, once per base frame, and 66 in the main pass, once
per generated frame (harness README; `v3_1.cpp`, split at `:233`). Images, from `lsfg-vk-pipeline/src/pipelines/v3_1.cpp`:

| Image | Format | Size | Count | Lifetime |
|---|---|---|---|---|
| Source | swapchain format (§2.1); lsfg-vk uses RGBA8 or RGBA16F (`:23-30`) | display | 2 slots | pinned, ingest target |
| Mipmap pyramid | R8 | flow-scaled | 7 (`Texture2D[7]`) | per base frame |
| alpha 0-2 ping-pong | RGBA8 | per level | 1-2 × mul | transient |
| alpha3 result | RGBA8 | per level | (2 × mul) × 3 per level | **pinned**: features of the last 3 base frames (harness README: "rotates modulo 3") |
| beta 0-3 / beta4 | RGBA8 / R8 mips | coarsest level | 2 / 6 | per base frame |
| gamma, delta, epsilon ping-pong | RGBA8 | per level | 3 or 2 × mul | transient, per generated frame |
| gamma/delta/epsilon results | RGBA16F | per level | 1 per level | per generated frame |
| Output | RGBA8, or 16-bit for HDR (`:539-550`) | display | 1 | copied out each generated frame |

`mul` is 2 in quality mode and 1 in performance mode. The harness can print the summed `GetResourceAllocationInfo` per
resolution: record it at 3440x1440, 2560x1440 and 1920x1080. Placed-resource aliasing of the transient images is a later
optimisation; the harness does none, deliberately.

### 4.2 Root signatures, root constants, descriptors

- **One root signature per shader (27)**, built from the cached binding table:
  - **`cb` (b0, space0) as six 32-bit root constants**: `Timestamp`, `SourceIndex`, `AdvancedColorKind`, `HdrSupport`,
    `ResolutionInvScale`, `UiThreshold` (`2.hlsl:1-8`; lsfg-vk `pipeline.hpp` `UniformBuffer`). This departs from the
    harness's 256-byte CBVs, on purpose: root constants are recorded into the list, so no CPU-written GPU memory exists.
    That means no ring and no fence. A fence matters here because the list runs on FFX's queue, where we cannot signal.
    lsfg-vk itself updates the timestamp on the GPU timeline (`vkCmdUpdateBuffer`, `pipeline.cpp:868-878`).
  - **Push constants** (`specialFlag`, `subiteration`; `pipeline.cpp:760-773`) as two more root constants.
  - **One descriptor table per SRV or UAV binding**, which is one per register space, as the converter laid them out.
  - **Three static samplers**, mirroring lsfg-vk's fixed three (`pipeline.cpp:437-457`). No sampler heap.
  - At most about 16 DWORDs, well under 64.
- **One shader-visible CBV/SRV/UAV heap per context.** It holds per-dispatch immutable tables, following the harness's
  contract:
  - full-length tables with typed null views for unused elements;
  - no image bound as an SRV and a UAV in the same dispatch, which D3D12's state rules forbid.

  The heap is written once, when the context is created. Each frame only root constants change. `SetDescriptorHeaps` on
  FFX's list has a precedent: FSR-FG's HUD helpers already record their own heaps on `params->commandList` before FFX's
  dispatch (`FSRFG_Dx12.cpp:999-1045`).

### 4.3 What the callback records

`Lsfg::FfxDispatch(params)`:

1. **Pick a context.** `ctx = Contexts.For(device of params->commandList, presentColor size/format, flow, perf, fp16, hdrVariant)`.
2. **If it is not ready**, or this is the first frame after a reset: `params->numGeneratedFrames = 0`, then return OK.
3. **Derive** `kind` and `support` from `params->backbufferTransferFunction` (§6).
4. **Record** on `params->commandList`, which is a DIRECT list, or a COMPUTE list when FGAsync is on:
   - `SetDescriptorHeaps(ctx.heap)`;
   - `presentColor` PRESENT → COPY_SOURCE, copy into `source[iteration % 2]`, then back to PRESENT;
   - the pre-pass, 34 dispatches, with root constants `{0, iteration, kind, support, 1/flow, 0.5}`;
   - for each `k` in `1..numGeneratedFrames`:
     - the main pass, 66 dispatches, with `Timestamp = k / (numGeneratedFrames + 1)` (`pipeline.cpp:869`);
     - `outputs[k-1]` UAV → COPY_DEST, copy from `ctx.output`, then back to UAV.
5. **Advance.** `iteration++`, then return OK.

Notes on that sequence:

- **Restore FFX's states.** `presentColor` and `outputs[]` must end in the states their `FfxApiResource` descriptors name.
  On a COMPUTE list only compute-legal states are used: COPY_*, NON_PIXEL_SHADER_RESOURCE, UNORDERED_ACCESS, COMMON.
- **The final copy could go.** Writing generate's output straight into `outputs[0]` would save one display-size copy,
  about 20 MB of traffic at 3440x1440, but it needs a per-frame UAV descriptor for FFX's two alternating buffers. It is a
  later optimisation, measured first.
- **Ignored for now:** `generationRect` (letterboxing) and `minMaxLuminance`.

### 4.4 History and reset

- **Warm-up.** History starts at zero, and frame 0 is warm-up (harness README). A new or rebuilt context produces nothing
  until two base frames have been ingested.
- **Reset.** A `params->reset` is set from `_reset[fIndex]` by the synthesized input's policy: scene cut, fast-motion
  response, low-fps floor (`FSRFG_Dx12.cpp:889-892`, `Synth_Inputs.cpp:1135`). On a reset, ingest still runs, generation
  is skipped (`numGeneratedFrames = 0`), and the context returns to warm-up. LS's own "off below 10 fps base" maps to
  the existing `[FrameGen] SynthesizedMinFps`, with no new key.
- **Duplicate presents.** These are emulator frames shown twice. The synthesized input already detects and reports them
  (`Synth_Inputs.cpp:1021-1027`), and interpolating between identical frames judders under any interpolator.

### 4.5 Recreation

**What triggers a rebuild.** Size or format of `presentColor`, the device, `FlowScale`, `Performance`, `HalfPrecision`, and
the HDR variant all key the context.

- **PSOs** depend only on the shader variant (perf, fp16, scRGB output), so one set per variant is cached.
- **Resources and heap** depend on size and format.
- The callback never builds on the present path. It schedules the build on the worker and generates nothing until the
  build is done.
- A resize is detected from `presentColor`'s description, so `hkResizeBuffers` needs no hook. A swapchain recreated
  under the same window (the bridge's takeover, `25c448e1`) shows up the same way, as a new `presentColor` or device,
  and the old context is parked.

### 4.6 Never free under the GPU, one lock

`retoque/dlssnr/README.md`, "Design notes worth knowing", paid for these rules with device hangs. They carry over:

- **Park, then free on proof.** A retired context is parked with the FG present count. It is freed when FFX has
  provably drained:
  - after `ReleaseSwapchain` destroys FFX's swapchain context (`FSRFG_Dx12.cpp:1235-1242`);
  - after FFX's own resize. Its `waitForPresents` waits on the interpolation fence before FFX frees its own buffers
    (`FISC.cpp:1553`, `:1643-1651`).

  Otherwise it is freed 32 FG presents later, NR's count. We hold no fence on FFX's queue, so the count is the fallback.
  FFX keeps at most two interpolation outputs in flight (`FISC.h:43`), far inside 32.
- **One lock.** The callback runs on the game's present thread, inside FFX's `Present`. The menu, the worker's hand-over
  and the release hook take the same mutex. Nothing in the callback blocks on the worker.
- **No resource of ours** is `Release()`d inside the callback.
- **Each context is bound to one device.** A device change parks the old context, as a resize does.

## 5. Config and menu

The block is new, sits in its own `[LSFG]` ini section, and is marked "removable as one block" like `[DlssNr]`. Every
key takes the four edits: declared with its default in `Config.h`, read in `Config.cpp`, saved in `Config.cpp`, and
present in the shipped `OptiScaler.ini`. `nr-invariants` round-trips only `DlssNr` keys, so a host-tier test of its own
covers these (§7, step 2).

| Key | Type, default | Meaning |
|---|---|---|
| `Enabled` | bool, `false` | With `FGOutput=fsrfg`, LSFG replaces FFX's interpolation. FFX keeps the swapchain, pacing and UI composition |
| `DllPath` | wstring, unset | `lsfg-vk.dll`, `Lossless.dll` or the Lossless Scaling folder; unset searches (§3.1) |
| `FlowScale` | float, `1.0`, clamped 0.25..1.0 | LSFG's flow scale (lsfg-vk default `1.0`, `config.hpp:54`) |
| `Performance` | bool, `false` | Performance variants (lsfg-vk default `false`, `config.hpp:55`) |
| `HalfPrecision` | bool, `false` | FP16 variants. lsfg-vk defaults to `true` (`config.hpp:34`), but on D3D12 the converted shaders say `min16float`, which drivers may ignore. Off until it is measured faster (harness README) |

**Not added:**

- **`Multiplier`.** FSR-FG cannot go past x2, so the control would act on nothing (DEVELOPMENT.md invariant 2). It
  arrives with (a′).
- **An HDR override.** The mode follows the swapchain's colour space (§6), with one control for one quantity.

**Menu.** In the FSR FG card (`menu_common.cpp:4376`), an **Interpolator** combo: `FFX (FSR x.y)` / `LSFG 3.1`. The
card's state table, in the format of DEVELOPMENT.md §2:

| State | Interpolator combo | Rows shown | Line under it |
|---|---|---|---|
| `[LSFG] Enabled` off, DLL found | FFX selected, LSFG selectable | the card as today | — |
| DLL not found | LSFG shown **disabled** | the card as today | "lsfg-vk.dll not found: install Lossless Scaling's lsfg-vk beta, or set [LSFG] DllPath" |
| DLL refused (validation, §3.2) | LSFG disabled | as today | the reason, amber |
| LSFG selected, preparing | LSFG | Flow scale, Performance, Half precision | "Preparing LSFG shaders (n/99)" |
| LSFG running | LSFG | the three above; FFX FG Version, Debug View, Debug Tear Lines and Debug Reset Lines **hidden**, because they act on FFX's interpolation dispatch; Debug Pacing Lines, Allow Async, Frame pacing tuning and FG Only Generated kept, because FFX's swapchain still does those | "LSFG 3.1 from <path> (sha 1a2b3c4d), 3440x1440 flow 1.00 quality, HDR10" |

**The synthesized-input rows hide too.** Under LSFG, `SynthesizedMotion`, the fast-motion cap, the incoherence rule and
`SynthesizedHudDepth` only feed FFX's interpolation, so they are hidden. `SynthesizedMinFps`, `SynthesizedHudLayer` and its
margin stay, because FFX still composes that layer.

**The frame-gen ratio slider** stays at max 1 (`RenderFGModeSlider(ctx, "Frame Gen##fsrfg", 1, nullptr)`, `:4407`).

**Default-identical when off** (invariant 1): with `Enabled=false`:
- `Lsfg::Interpolates()` is false;
- `FSRFG_Dx12` takes byte-for-byte today's path;
- no DLL is searched or opened, no file is read, nothing is allocated.

The menu checks whether the DLL exists only while the FSR FG card is drawn: one stat, no parsing.

**Log lines:**

- `LSFG: lsfg-vk.dll at <path> (<bytes>, sha256 <8 hex>)` / `LSFG: lsfg-vk.dll not found; searched: ...`
- `LSFG: 99 shaders converted in <s>, cached at <dir>` / `LSFG: shader cache hit (<dir>)`
- `LSFG: context <W>x<H> <format> flow <f> <quality|performance> <fp32|fp16>, transfer <SRGB|PQ|scRGB> -> kind <k>
  support <s>, <n> descriptors, <MiB> MiB`
- every 10 s while running, like the synthesized input's summary: `LSFG: <n> generated, <m> skipped (reset, not ready),
  GPU p50 <ms> p99 <ms>`. The timestamps are recorded in the callback list and resolved a few frames later.

## 6. HDR

**What the constants do.** `cb_AdvancedColorKind` and `cb_HdrSupport` are declared in every shader's `cb`, but only the
mipmaps pass reads them (grep over `Tmp/lsfg-study/hlsl/`). There they shape the luma pyramid that motion estimation
works on (`0.hlsl:41-60`):

- kind == 1 → `pow(c, 1/2.2)`: linear values in [0,1] are gamma-encoded.
- else, support != 0 → `pow(c × 0.13333, 1/2.2)`: scRGB, where 1.0 = 80 nits, so 7.5 = 600 nits maps to 1, then gamma.
- else → `c` as is: an already-encoded signal.

The kind values follow Windows' `AdvancedColorKind` (0 SDR, 1 WCG, 2 HDR). lsfg-vk sets kind 2 and support 1 for HDR,
and 0 and 0 otherwise (`pipeline.cpp:410-411`).

**Generate never applies a transfer function.** It writes a normalised weighted blend of four source samples
(`2.hlsl:63`). The output stays in the input's encoding, bounded by the input's range. So wrong constants cost motion
quality, never colour.

| Swapchain | Colour space (FFX's `backbufferTransferFunction`, `FISC.cpp:2498-2516`) | kind / support | Source | Output |
|---|---|---|---|---|
| SDR, 8-bit or 10-bit | G22 P709 → `SRGB` | 0 / 0 | swapchain format | swapchain format |
| HDR10 | G2084 P2020 → `PQ` | **0 / 0 (proposal)**: PQ code values are perceptual, like gamma-encoded SDR, so the pyramid sees a sensible signal. Kind 2 / support 1 would apply scRGB scaling to non-linear data | `R10G10B10A2` | `R10G10B10A2` (10-bit; never through 8-bit) |
| scRGB | G10 P709 → `SCRGB` | **2 / 1** (lsfg-vk's HDR setting) | `R16G16B16A16_FLOAT` | `R16G16B16A16_FLOAT`, with the generate output declared `float4` |

**Output precision is the UAV view's format, not the shader's.** `generate_8bit` and `generate_16bit` differ only in the
SPIR-V image format of their output, `Rgba8` against `Rgba16`, and spirv-cross turns both into byte-identical HLSL
(`1.hlsl` and `2.hlsl` share a SHA-1; `spirv-dis` diff). In D3D12 the 8/16-bit split therefore disappears: one DXIL,
with precision set by the resource bound.

**The one catch is scRGB.** The declaration is `RWTexture2D<unorm float4>` (`2.hlsl:17`), and over a FLOAT view a UNORM
component type may clamp to [0,1], which would destroy highlights above 80 nits and negative values. Hence the converter
patch in §3.4.

**Where the colour space comes from.** In (b), `params->backbufferTransferFunction` carries it. FFX derives it in
`SetColorSpace1`. In (a′), or as a cross-check, use `retoque/dlssnr/DlssNr_SwapchainColour.h`, the per-window record
NR already keeps, since DXGI has no `GetColorSpace1`. That would make FG depend on the NR module, so it stays a
cross-check only.

**Unknown, and no reference exists.** lsfg-vk 2.0.0 never runs its HDR variant: `Context` hard-codes `hdr = false`
(`lsfgvk.cpp:137-138`). Its HDR format, `R16G16B16A16_SFLOAT` (`signature/image.hpp:22`), disagrees with the shader's
`Rgba16` UNORM anyway. So `lsfg-vk-cli` has no HDR output to compare against. HDR is validated by self-consistency only:
output within the input range, no clamp, no banding against SDR.

## 7. From "harness matches the reference" to Divinity and PCSX2

The order goes from simplest to hardest and reuses the test paths CLAUDE.md lists. Nothing builds while another job uses
the Wine compiler prefix.

0. **Reference gate, harness only.** Run the same frame sequences through the harness and through `lsfg-vk-cli debug`,
   and compare with the companion `compare.py`.
   - Order: FP32 quality, flow 1.0, x2 and x3; then performance, flow 0.5, FP16.
   - Record per configuration: max absolute difference, the share of interior pixels within 1/255 after warm-up, and
     PSNR.
   - The harness already reaches byte-exact interiors on a constant sequence (README); the real sequences decide.
   - GPU time at 3440x1440 on the 4090, against `lsfg-vk-cli benchmark`'s 0.75 ms per generated frame.
1. **Extract `LsfgContext_Dx12`** into `retoque/lsfg/`, with the API `Create`, `RecordIngest`, `RecordPrepass`,
   `RecordGenerate` and `Retire` on a caller-provided list, and root constants per §4.2.
   - The harness keeps compiling against the same source, as a thin driver.
   - Gate: step 0's PNGs reproduce byte for byte.
2. **Shader provisioning**: discovery, the PE walker, conversion, the cache. Host-tier suites, registered in
   `tests/suites.toml` because the drift guard requires it:
   - PE walker on synthetic images, including the named mipmaps entry;
   - path resolution given environment and paths;
   - timestamp and iteration schedule;
   - transfer function → kind/support;
   - `[LSFG]` round trip.

   Gate: the in-process DXIL matches `Tmp/lsfg-study/dxil/` for the same dxc, or failing that, the harness PNGs match.
3. **PCSX2 on native D3D12** (`~/Games/pcsx2-nr-test/run.sh`), with `FGInput=synthesized`, `FGOutput=fsrfg`,
   `[LSFG] Enabled=true`, NR off, x2.
   - Check: the §5 log lines, the menu state table walked row by row (DEVELOPMENT.md §3, menu review), FFX's pacing
     lines on screen.
   - Then measure (below).
4. **Divinity on the D3D11 bridge** (`DefEd/bin/`, the `winmm.dll` proxy), with the same keys.
   - First NR off.
   - Then NR on with `HookMethod=2`, so the bridge hosts NR. That needs `[Libraries] NvapiPath` in Divinity's prefix.
   - Check that NR still runs once per base frame: the status line counts base presents.
   - Check that generated frames carry the NR look: capture below.
5. **HDR plumbing**, with `[HDR] ForceHDR` (scRGB) and `UseHDR10` on PCSX2. This checks the plumbing (kind/support,
   formats, no clamp), not HDR quality.
6. **Native Windows on Rafael's RTX 3060**, with the D3D12 debug layer and GPU-based validation. The Wine prefix has no
   debug layer (harness README, `0x887a002d`), and Proton is not a D3D12 validator (CLAUDE.md, the `13f10d60` CBV
   incident). This step also confirms that the DXIL signing path works and measures 1080p cost on a smaller GPU.

**Measurements, LSFG against FSR-FG synthesized.** Same title, same scene, same base cap, `SynthesizedMotion` both off and
on for the FSR side.

- **Display.** VRR stays off, as policy, and is not to be changed. With vsync, choose the mode so that refresh = 2 ×
  base: for PCSX2 at 60 fps, the monitor's 120 Hz mode rather than 165. Measure once with vsync and once with tearing
  allowed.
- **Deterministic content.** PCSX2's input recording replays the same frames on every run. In Divinity, a slow keyboard
  camera rotation over a fixed scene.

| What | How | Record |
|---|---|---|
| GPU cost per generated frame | Timestamps around the interpolator in the callback list, for LSFG and around FFX's `D3D12_Dispatch` alike; the 10 s summary | p50 / p99, plus FFX's prepare skipped under LSFG |
| Pacing | MangoHud frame-time log under Proton (it sees vkd3d-proton's presents); PresentMon `MsBetweenDisplayChange` on Windows; FFX's Debug Pacing Lines | Standard deviation, p1/p99 of display intervals, share of intervals over 1.5× target |
| Latency | A 240 fps camera on a mouse flick or button LED, 20 trials each for FG off, FSR-FG and LSFG. Proxy: the real frame's hold, which grows with generation time because FFX's presenter waits on the interpolation fence | median / spread |
| Artifacts | A debug capture of `outputs[0]` after either interpolator wrote it (a copy in the same callback list, read back later, PNG through WIC), on the deterministic content. Offline: the held-out-frame test in the harness (drop every other frame of a 120 fps capture, interpolate t=0.5, compare with the dropped frame) | PSNR/SSIM offline. In game, a written checklist: HUD and text warping, disocclusion halos, fast-pan break-up, scene cut → repeat, duplicate-frame judder |
| VRAM | `FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE` (`ffx_framegeneration.h:155-160`) for the idle FFX context, plus LSFG's allocations | MiB at each resolution |
| CPU | QPC around the callback's recording, which issues 100 dispatches per generated frame (bundles do not help when the list is COMPUTE) | µs per frame |

## 8. Risks and unknowns

1. **Licence and permission** (top of this note). They decide whether this can ever leave the machine, whatever the
   measurements say.
2. **x2 ceiling on (b).** FFX's swapchain hard-codes one generated frame (`FISC.cpp:1927`, `FISC.h:45-50`). x3 and x4
   mean (a′), a 2.6k-line fork to own and to keep in step with FFX fixes.
3. **The idle FFX FG context.** It exists only to carry the callback to the swapchain, yet it allocates FFX's
   interpolation and optical-flow resources.
   - Measure it with the memory query.
   - Prefer FFX's non-ML version (`[FSR] FGIndex 0`) under LSFG, if it is the smaller one.
4. **Signed FFX DLL against SDK source.** OptiScaler loads AMD's signed `amd_fidelityfx_framegeneration_dx12.dll`. The
   callback contract is public API and OptiScaler already depends on it. But `presentColor`'s state, `outputs[0]`'s
   format and state, and `numGeneratedFrames` are read here from SDK 2.3.0 source. Log them on the first callback and
   check.
5. **The scRGB clamp** (§6): the generate output is declared UNORM. The converter patch is the fix; verify under the
   debug layer on Windows.
6. **HDR10 constants are a proposal.** Lossless Scaling's own choice is not known, and there is no HDR reference (§6).
7. **Native Windows DXIL signing.** It needs `dxil.dll` alongside `dxcompiler.dll`. Two more DLLs to ship. Under Wine
   nothing checks the signature, so only Windows can prove it.
8. **Lossless Scaling updates.** The beta can change resource ids, shader count or bindings. The cache key (DLL
   SHA-256) and the validation (§3.2) refuse cleanly. Nothing guesses.
9. **FP16 may buy nothing on D3D12.** `min16float` is a hint the driver is free to ignore (harness README). Real
   half-precision would need the converter to emit `float16_t` at SM 6.2 with `-enable-16bit-types`. That is a separate
   change, worth it only if measured.
10. **Async compute lists.** With FGAsync, the callback's list is COMPUTE. That restricts barrier states and rules out
    bundles. It is covered by design (§4.3) but needs testing in both modes.
11. **The NR stand-down rule depends on the FFX swapchain being `currentFGSwapchain`**
    (`DlssNr_Dx12.cpp:6941-6946`). (b) keeps that; (a) would have to keep it deliberately.
12. **Two LSFGs are not the same thing.** The `lsfg-vk.dll` graph (lsfg-vk's SPIR-V) and Lossless Scaling's Windows DXBC
    graph may differ in version or tuning. This study's reference is `lsfg-vk-cli`, not LS on Windows, so "matches
    LSFG" means "matches lsfg-vk".
13. **Cost on smaller GPUs is unknown.** 0.75 ms is a 4090 at 3440x1440. On the 3060 the NR model already fills the
    shader cores (synthesized-frame-generation.md), so LSFG and NR together may not fit a frame there. Step 6 decides.
14. **First-run compile time.** It has not been measured. If it is long, it is a one-time cost per prefix and per
    Lossless Scaling update. The menu must say what is happening (§5) instead of looking stuck.
