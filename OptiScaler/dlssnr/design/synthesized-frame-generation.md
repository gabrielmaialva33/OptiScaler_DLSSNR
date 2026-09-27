# Frame generation for titles that give no motion

Status: **steps 1 and 2 built and measured on branch `synth-fg`** (`90990bba`, `d954692d`; see
[Step 1, measured](#step-1-measured) and [Step 2, measured](#step-2-measured)). Written 2026-09-27 against `944aff56`. The code this plans lives
outside the NR module (see [Where the code goes](#where-the-code-goes)); the note sits here because
it builds on the present host and on [synthesized-motion.md](synthesized-motion.md).

## The problem

Every frame-generation path OptiScaler has needs the game's depth and motion vectors, and a game
only hands those over when it calls an upscaler:

- FSR-FG and XeFG refuse to dispatch without both: `Depth or Velocity is not ready, skipping`
  (`framegen/ffx/FSRFG_Dx12.cpp:347-353`, `framegen/xefg/XeFG_Dx12.cpp:681-686`).
- OptiFG (`FGInput=Upscaler`) is fed from the upscaler's evaluate
  (`inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp`, `Upscaler_Inputs_Dx12.cpp`).
- Upstream's new Reprojection output warps the last frame by raw mouse input and needs depth and a
  hudless buffer (`framegen/reprojection/Reprojection_Dx12.cpp:236-263`). It is extrapolation for a
  first-person camera, and does not fit an isometric one or an emulator.

The titles this is for call no upscaler at all: Divinity: Original Sin 2 and The Witcher 3 DX11 on
the D3D11 bridge ([nr-dx11-bridge-host.md](nr-dx11-bridge-host.md)), and emulators on the D3D12
present pass (PCSX2, [nr-present-hook.md](nr-present-hook.md)). They are also the titles where the
NR model is most expensive relative to the frame: 18 ms at 960x540 on an RTX 3060.

The `80004002` in Divinity's log is not this problem. With FG off, the bridge probes
`FGHooks::CreateSwapChain*` first, gets `E_NOINTERFACE` from `CheckForFGStatus()`
(`hooks/FG_Hooks.cpp:82`, `:133-136`), and falls back to a plain DX12 presenter, as designed.

## What the research ruled out

Researched 2026-09-27 from public sources only.

- **Lossless Scaling's frame generation cannot be embedded.** Its shaders and weights are
  proprietary; the Steam Subscriber Agreement (§2.G) forbids reverse engineering and derivative
  works; TERRAHEART files DMCA notices. lsfg-vk ships none of it: it reads the shaders out of the
  user's own `Lossless.dll` at runtime, with the author's blessing, and LS 3.2.2 even ships a set
  meant for it. That blessing names lsfg-vk, not us, so a "bring your own `Lossless.dll`" backend
  would need written permission first. Current lsfg-vk is CC BY-NC-ND; only its `v1.0.0` (MIT) and
  `v2.0.0-dev` (GPLv3) snapshots could be read for host-side ideas.
- **NVIDIA FRUC** (`NvFRUC.dll`) is closed and takes D3D11 or CUDA surfaces; dxvk-nvapi returns "not
  available" for both, so it cannot run under Proton. 4.4 ms at 1080p on a 4090.
- **ReShade optical-flow shaders** (DRME, Marty McFly's, Zenteon, vort) are CC BY-NC, proprietary or
  all-rights-reserved. None is compatible with GPL-3.0.
- **Learned interpolators** (RIFE, IFRNet) cost about 14 ms at 1080p on a 3090 with TensorRT, so
  30-40 ms on a 3060, and depend on CUDA.
- **Upstream will not take it.** On injecting LS frame generation: "No interest for anything like
  this" (optiscaler/OptiScaler#1153). For content without motion vectors they point to LS or AFMF
  (#354). This stays in the fork.

What *is* public about LSFG, and ours to reimplement: interpolation from two finished frames only;
a luma pyramid at a reduced "flow scale"; coarse-to-fine motion estimation; per-frame features
cached so 3x and 4x only redo the part that depends on t; a UI-detection mask with a threshold; and
turning off below 10 fps base.

## What we can build on

- **AMD's frame interpolation and optical flow are MIT, and already in the tree.**
  `external/FidelityFX-SDK-v2` (`60f4ea8`, SDK 2.3.0) carries the full sources and shaders under
  `Kits/FidelityFX/framegeneration/fsr3/`. `docs/license.md` lists every one of them, including
  `internal/shaders/ffx_opticalflow_*_pass.hlsl` and `ffx_frameinterpolation_*_pass.hlsl`, under the
  MIT licence; the rest of that SDK is binary-only. The optical flow takes colour only and outputs
  one vector per 8x8 block and a scene-cut flag, using SAD over a 7-level luma pyramid, a median
  filter, and up to 512 px of travel.
- **FSR3 frame interpolation already runs that optical flow itself.** It builds a game-vector field
  and an optical-flow field and, per pixel, keeps whichever reconstructs the colour better. So zero
  game vectors should fall back to optical flow alone, minus depth-based disocclusion. That is
  inferred from AMD's documentation; nobody has published a test of it.
- **The FSR-FG output, its pacing and its UI composition already work here.** `FSRFG_Dx12` owns the
  FFX swapchain, pacing and inpainting. `FGDrawUIOverFG` plus `RUI_Dx12` composite a `UIColor`
  resource over every presented frame (`FSRFG_Dx12.cpp:1659-1690`).
- **The D3D11 bridge already makes an FG-capable presenter.** `Dx11wDx12SC` creates the FG swapchain
  through `FGHooks` on the bridge's D3D12 queue when FG is wanted (`hooks/DxgiFactory_Hooks.cpp:432`).
  DX11 titles that do call an upscaler use this today.
- **Precedent for the whole shape:** afmf-linux (MIT, a Vulkan layer) uses the FidelityFX optical
  flow plus its own interpolation, colour only, at 0.56-0.61 ms at 3440x1440 on an RX 9070 XT. On a
  GTX 1050 Ti it was reported smoother, with smearing on text and UI.
- **NVIDIA's optical-flow engine is reachable on both platforms.** `nvofapi64` works through D3D12
  natively, and under Proton through dxvk-nvapi ≥ 0.8.0, which maps it to `VK_NV_optical_flow`
  (vkd3d-proton 2.14+; D3D11 and CUDA entry points are refused). It runs on a dedicated engine, not
  the shader cores. That matters on the 3060, where the NR model already fills the shader cores.

## The route

**A new FG input, `Synthesized`, that feeds the existing FSR-FG output from the presented frame
alone.** Per base frame it supplies:

- **Velocity:** a colour-only motion field. At first it is the zero field. Later it comes from a
  shared estimator, described next.
- **Depth:** a constant field, `InvertedDepth` set, until a reason for real depth appears.
- **Constants:** camera values from config (the bridge input's 60° fallback), jitter 0, MV scale
  1,1 with `DisplayResolutionMVs`, and an interpolation rect equal to the display.

Why feed FSR-FG instead of writing our own interpolator, as afmf-linux did:

- **Most of the hard parts ship already:** swapchain, pacing, inpainting and UI composition. Owning
  pacing ourselves, on a D3D11 bridge and with VRR off on the target display, is the part most
  likely to go wrong.
- **FSR's interpolator arbitrates between the fields per pixel.** Our field only has to be good
  where it disagrees with FSR's own optical flow, not everywhere.
- **The fallback stays open.** If flat depth breaks FSR's disocclusion heuristics badly, a
  standalone interpolator built on the same MIT shaders can reuse this design's input, estimator
  and HUD mask.

**The estimator is a port of the FidelityFX optical flow,** emitting the output contract of
[synthesized-motion.md §4](synthesized-motion.md#4-exact-output-contract): `R16G16_FLOAT`,
current-to-previous, in pixels. One producer then serves both consumers:
- FG gets the field at display resolution.
- NR gets it at working resolution, replacing the zero guides that make Generation Zero ghost.

That note's hand-written block matching has the same skeleton: pyramid, SAD, coarse to fine.
The FidelityFX kernel is already tested in shipped games, handles scene cuts, and is in the tree.
Its 8x8 blocks are finer than the 320-wide grid that note planned (240 blocks across at 1080p,
430 at 3440).

Two alternative motion sources can sit behind the same contract. Neither is first:
- **NVIDIA OFA** through `nvofapi64` on D3D12, which moves the estimator off the shader cores on the
  3060. It is NVIDIA-only and needs `nvofapi64` present in the Proton prefix.
- **Zero,** which step 1 below tests.

### The HUD

Without a hudless frame, generated frames warp the interface. Divinity is mostly interface, so this
decides whether the result is usable there.

- **The mask.** A pixel that is identical in both frames while its neighbourhood moves is treated as
  a static overlay. This is afmf-linux's rule, and it is how LS's "UI detection" is described.
- **The composite.** The mask becomes the alpha of a `UIColor` texture cut from the current frame,
  and the existing `FGDrawUIOverFG` path composites it over every generated frame. The interface is
  then never warped, only held still, which is what it is doing anyway.
- **Not planned:** a real hudless capture on D3D11, by tracking the draws the interface is made of.
  That is upstream's DX11 Hudfix (`12ec9011`), which this fork declined in `512e168d`.

### With NR on

The order falls out right on both paths: FG interpolates whatever reaches the presenter, and NR has
already written it.

- **Bridge:** `_nrHost->Record` runs inside `_CopyDx11SharedToDx12FGBackBuffer` before
  `_fgSwapChain->Present` (`with_dx12/dx11_with_dx12_sc.cpp:1485-1521`, `:605`).
- **Native D3D12:** `RunPresentPass(..., true)` runs before `o_FGSCPresent`, where FFX interpolates
  (`hooks/FG_Hooks.cpp:1269-1276`).

Two things block running NR and FG together today:

1. **The bridge is built for FG or for NR, never both.** `Dx11wDx12::Wanted()` admits NR only when
   FG is not wanted (`dx11_with_dx12_sc.cpp:47-53`). This was deliberate
   ([nr-dx11-bridge-host.md](nr-dx11-bridge-host.md): do not set `activeFgInput` for NR), so lifting
   it needs its own review.
2. **NR would run a second time on generated frames** (read from the code, not run). FFX's inner
   swapchain is wrapped
   (`DxgiFactory_Hooks.cpp:548-552`). The wrapped entry stands down only after the FG hook has run
   (`g_lastFgFlip`, `shaders/dlssnr/DlssNr_Dx12.cpp:4198`, set at `:4387`). The FG hook is gated
   off whenever there is interop (`FG_Hooks.cpp:1274`), so on the bridge
   `wrapped/wrapped_swapchain.cpp:508` would run NR again on every presented frame, generated ones
   included, on top of the bridge host's own pass. On native D3D12 that re-enhancement is intended for
   DLSS-G. For this input, the model should run **once per base frame**, because doubling 18 ms on a
   3060 is the opposite of the point, and generated frames would enter the model's history.

### Emulators

PCSX2's `SkipDuplicateFrames` (on in both our installs) presents at the game's rate, so a 30 fps
game becomes 60 with FG at 2x. If an emulator does present duplicates, the estimator sees zero
motion, and interpolating between identical frames produces judder, not smoothness. The scene score
the estimator computes anyway can spot an identical pair, so we skip generation for it rather than
interpolate. On D3D12, the FFX swapchain is created on the game's queue by `FGHooks` when the input
is admitted, and the input is recorded at `FG_Hooks.cpp:1215`, before `fg->Present()`.

## Where the code goes

- **The shared estimator:** `OptiScaler/shaders/synth_motion/`, not `shaders/dlssnr/motion/`. FG
  must not depend on the NR module, which stays removable as one block. The unbuilt skeleton in
  `shaders/dlssnr/motion/` is superseded, and [synthesized-motion.md](synthesized-motion.md) points
  here.
- **The FG input:** `inputs/FG/Synth_Inputs_Dx11wDx12.{h,cpp}`, modelled on
  `Upscaler_Inputs_Dx11wDx12.cpp:150-288`, with a D3D12 twin for the present path.
- **Wiring:**
  - `FGInput::Synthesized` in `State.h`, before `ForceXeLL`, because the menu indexes by value.
  - The four-point config round trip for its string.
  - Admitting the new input in `Dx11wDx12::Wanted()` and in FSR-FG's auto-activation
    (`FSRFG_Dx12.cpp:1270`).
- **Copied FidelityFX shaders** keep their MIT headers. `Licenses/` gets the notice, the same way
  RenoDX's does.
- **Branch.** All of it goes on a branch and merges to `dlss-neural-rendering` only once measured
  (CLAUDE.md: the default branch is mirrored publicly within minutes).

## Steps, each falsifiable before the next

1. **Transport with the zero field.** New input, the bridge call site, zero Velocity and constant
   Depth at display size, NR off.
   - *Where:* Divinity on the 4090.
   - *Look at:* the log (context created, frames dispatched, no "not ready"), MangoHud's rate
     doubling, and a slow pan and a fast pan, captured.
   - *Settles:* whether FSR's own optical flow is enough on its own. If it is, the estimator is
     needed only for NR, and FG gets it later as refinement.
2. **NR and FG together.** Lift the `Wanted()` exclusion and gate the second NR run. A
   before/after capture must show the model ran once per base frame and the generated frames
   carry its output.
3. **The estimator.**
   - Port the FidelityFX optical flow into `shaders/synth_motion/`.
   - Score endpoint error against a synthetic known shift in the loopback harness before any game.
   - Pin the field's units and y sign with a capture.
   - Feed FG, and compare against step 1 on the same pan.
   - Hand the field to NR's present host, which is where Generation Zero's ghosts go away.
4. **The HUD mask**, into `UIColor` through `FGDrawUIOverFG`. Judged on Divinity's hotbar and
   tooltips during a pan.
5. **The D3D12 present path:** PCSX2, then a check that `SkipDuplicateFrames` and the identical-pair
   skip hold up at 30 and 60 fps.
6. **Rafael's 3060.**
   - Measure FG and estimator cost next to the model; FSR-FG is 0.7-2.1 ms at 1080p by AMD's table.
   - If the shader cores are the limit, try NVIDIA OFA as the motion source.
   - Measure pacing on a fixed-refresh display: VRR stays off on this workstation by decision, and is
     not to be changed for this.

## Step 1, measured

Divinity: Original Sin 2, 2026-09-27. Setup:
- **Hardware and build:** RTX 4090 at 3440x1440, GE-Proton11-7, build `90990bba` (20260927_180727).
- **Ini:** `FGInput=synthesized`, `FGOutput=fsrfg`, NR off at launch and switched on with F7 part of
  the way through.

**Log:**
- FSR-FG created its context.
- The bridge fed a 3440x1440 zero motion field (`R16G16_FLOAT`) and a constant depth (`R32_FLOAT`,
  inverted); the backbuffer was `R10G10B10A2`.
- About 5,950 dispatches, every one with `numGeneratedFrames: 1`.
- Not one `Depth or Velocity is not ready`, and no errors.

**Rate:**
- The game ran 60 base frames per second throughout; that is its own limiter.
- With FG, 120 frames per second were presented.
- Switching FG off in the menu (18:13:22 to 18:13:25) dropped it to 60 at once, and back to 120 on.
- The base rate stayed at 60 either way, so the limiter hides FG's cost. **Cost is not measured
  here.**

**Image:** judged by the player, comparing FG on and off in the menu, on slow and fast pans:
nothing looked abnormal, with no doubled edges and no smeared interface. No capture was taken, so
this is one person's eye at a 60 fps base, not a measurement of error.

**What it settles.**
- For Divinity at 60 base, FSR's own optical flow, with zero game vectors and flat depth, is enough.
- So in this title FG does not need our estimator. The estimator's first job goes back to NR, for
  Generation Zero's ghosts, and to faster-camera titles.
- The HUD mask (step 4) waits until a title actually shows the interface smearing.

**What it does not settle:**
- a fast first-person camera;
- a low base rate, such as the 3060 with NR on;
- cost;
- pacing beyond counts per second.

**Seen on the way: NR and FG already ran together, by the unplanned route.**
- The player pressed F7 mid-session, and the present pass came up through the wrapped swapchain
  (`first pass on the backbuffer (3440x1440, swapchain hook)`).
- It ran at 3440x1440, scale 1.00. The ini's `WorkingScale=2.0` is clamped to native for a
  zero-guide caller.
- It counted presented frames, generated ones included: about 200 per 2 s window against 120 base
  frames. This is the double run "With NR on" predicted from the code.
- The status line's `0 renders` does not mean the model never ran. That counter counts the game's
  own render frames (`CaptureTemporal`, upscaler evaluates), and a title with no upscaler has none.
  The line only takes its `active` form when passes were recorded.
- The player judged NR plus FG good too. On the 4090 the doubled model cost fit under the 60 fps
  cap. On a 3060, where the model costs 18 ms, it cannot.
- So step 2 does not make NR and FG work together; they already do. What it does is put the model
  where it runs once per base frame (the bridge host, before FG), instead of on every presented
  frame after it.

## Step 2, measured

Same setup, build `d954692d` (20260927_182832), NR on at launch (`HookMethod=2`).

- **Once per base frame.** The bridge logged that it runs the pass once per base frame, before
  synthesized FG. The new host line read `the model ran on 120 of the 120 base frames … 2.0 s`
  window after window: 60 model runs per second at the 60 fps cap, against about 100 in step 1.
- **The duplicate route is closed.** Not one `swapchain hook` pass and no process-wide `DLSS-NR
  status` line all session: nothing ran on FFX's swapchain.
- **FG ran as in step 1.** MangoHud showed 120, and nothing reported `not ready`.
- **Size.** The model tried 2x (6880x2880) from the ini's `WorkingScale=2.0`, was refused on zero
  guides as before, and ran at native.
- **F7** switched the pass off (`the pass is disabled`) and back on mid-session.
- **Startup noise.** `Frame count jumped too much` warnings appear over the first ten frames only,
  while the FSR-FG context comes up.

## Review after the merge

An adversarial review of steps 1 and 2 (after merge `54d900e7`) found no blockers. Fixed on top:

- **Native D3D12.** The input was selectable there, and it got an FFX swapchain nothing ever feeds.
  `FGHooks::CreateSwapChain*` now refuse `Synthesized` unless the bridge is the caller
  (`forDx11Bridge`). The menu offers it only when the swapchain is DX11 or bridged, and the ini and
  `Config.md` say "D3D11 games only for now". Step 5 lifts this.
- **Present queue.** Step 2 keyed the bridge's fallback to its own queue on "no FG object". A
  synthesized-FG bridge whose FFX context failed was then left with no queue, and so was an NR-only
  bridge with a stray FG object; both had worked before. The fallback is back to "a host exists".
- **Wasted bridges.** `Synthesized` with an output other than FSR FG no longer asks for a bridge,
  since that FG is refused anyway.
- **Menu.** An idle host sets `nrPresentHostActive` from the first frame it actually runs, so the
  panel stops pointing at an upscaler the game does not have.

Left as found, because it predates this work: a built host switched off, then resized, reallocates
and spends its one model-creation attempt while off. The model is then created on the frame's list
when NR comes back.

**Order from here:**
1. ~~Step 2: move NR to once per base frame, before FG.~~ Done, above.
2. The D3D12 present path (step 5), for PCSX2.
3. Rafael's 3060 (step 6).
4. The estimator (step 3) and the HUD mask (step 4), when a title needs them.

## Open questions

- How FSR's interpolator behaves with flat depth: its disocclusion and inpainting assume real depth.
  Step 1 shows it.
- The units and sign FSR expects from our field, pinned by capture, not asserted. A flipped sign
  reads as "synthesis made it worse".
- Whether 8x8 blocks are fine enough for text-heavy scenes, or the HUD mask has to carry them.
- Latency. Interpolation holds one base frame. Divinity's turn-based pace tolerates that; a
  first-person game at 30 fps base would not, and the help text should say so.
