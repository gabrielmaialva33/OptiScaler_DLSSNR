# Frame generation for titles that give no motion

Status: **steps 1 and 2 built and measured** (`90990bba`, `d954692d`; see
[Step 1, measured](#step-1-measured) and [Step 2, measured](#step-2-measured)), merged in `54d900e7`.
**Step 5 (native D3D12) built and measured** in PCSX2; see [Step 5, built](#step-5-built) and
[Step 5, measured](#step-5-measured). Written 2026-09-27 against `944aff56`. The code this plans lives
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
- **The FG input:** `inputs/FG/Synth_Inputs.{h,cpp}` (first written as `Synth_Inputs_Dx11wDx12`),
  modelled on `Upscaler_Inputs_Dx11wDx12.cpp:150-288`, shared by the bridge and the D3D12 present path
  since step 5.
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
  `Config.md` say "D3D11 games only for now". Step 5 lifts this: see [Step 5, built](#step-5-built).
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
2. The D3D12 present path (step 5), for PCSX2. Built, below; not yet measured.
3. Rafael's 3060 (step 6).
4. The estimator (step 3): built and wired into NR's two no-guide present routes behind
   `[DlssNr] SynthMotion` (synthesized-motion.md, update 2026-09-27). FG integration deferred: FG still
   gets the zero field from `SynthInputs`, which step 1 measured as enough in Divinity. Wiring the same
   field into `SynthInputs` (display extent, no second estimator run when NR already recorded one this
   frame) is the next step, once the field is measured on NR.
5. The HUD mask (step 4), when a title needs it.

## Step 5, built

Why: PCSX2 runs on D3D12 (`Renderer=15`) on both machines. Switching it to D3D11 puts it on the bridge,
where steps 1 and 2 already work. A test on 2026-09-27 showed FG running that way. The user wants
D3D12 itself.

**One input, two owners.** `Synth_Inputs_Dx11wDx12` became `inputs/FG/Synth_Inputs.{h,cpp}`, class
`SynthInputs`.
- The bridge's use is unchanged. It records the one-time clear on its copy list, confirms or abandons
  it with that list, and feeds before `_fgSwapChain->Present`.
- For a caller with no list of its own, it adds `RecordInitOnQueue`. The class records the clear on
  its own allocator and list, closes it, executes it on the queue given, and signals its own fence.
  It confirms on a successful `Close` plus submission and abandons otherwise. The allocator is reset
  only after that fence shows the last clear finished (a bounded 2 s wait, reached only on a resize).

**The native D3D12 feed** is in `FGHooks::FGPresent`, right after the frame-time bookkeeping and before
`fg->Present()` runs Dispatch.
- *Condition:* `FGInput::Synthesized`, interop `None` (the bridge feeds its own), and an FG object.
- *Size and format:* from the FG swapchain's `GetDesc`.
- *Queue and device:* the queue is `fg->GetCommandQueue()`, which is FSR-FG's `_gameCommandQueue`.
  FSRFG_Dx12 executes its UI list and its prepare list there (`Present`, then `Dispatch`), so the
  clear, submitted on it first, is ordered ahead of the first read. The device is that queue's
  device, the one FFX's context is made on.
- *Owner:* a single file-static `SynthInputs` in `FG_Hooks.cpp`. FGHooks presents through one FG
  swapchain at a time (`State::currentFGSwapchain`), so one instance follows it. It is heap-allocated
  and never destroyed, because a static destructor would release D3D12 objects at process exit, after
  the device.
- *Release:* after the queue-idle wait in `hkResizeBuffers`/`hkResizeBuffers1` (only if the wait
  succeeded), and after the wait in the FG swapchain's final `hkFGRelease`. The next present
  reallocates at the new size. A resize whose wait failed leaves the old pair parked (`_retired`) until
  a proved release.

**Offering it on D3D12 again.**
- The `forDx11Bridge` refusal is gone. `FGHooks::CreateSwapChain*` have their original signatures,
  and the four bridge call sites are back to upstream's.
- The menu disables Synthesized only on Vulkan.
- The ini and `Config.md` say D3D11 and D3D12.

**NR once per base frame on D3D12.**
- On a native FG swapchain the game presents FFX's swapchain, whose `Present` is FGHooks' hook. That
  runs `RunPresentPass(..., true)` on the base frame, before `o_FGSCPresent`. FFX's inner real
  swapchain, made through the hooked factory, is wrapped, and it presents generated and real frames
  alike.
- The existing `g_lastFgFlip` rule already stood the wrapped entry down, but only once the FG hook
  had recorded a pass.
- The stand-down at the top of `RunPresentPass` now also covers native D3D12: `!fgHook`, Synthesized,
  and either the bridge's interop or `currentFGSwapchain != nullptr`. It returns before counting, so
  the status line counts base frames. Without an FG swapchain it does not fire, and the wrapped entry
  stays the route.

**Ordering, read from code.**
1. FGPresent feeds the zero pair, then `fg->Present()` executes FSR's UI and prepare lists on the game
   queue. Prepare reads only depth and motion.
2. `RunPresentPass(true)` then records NR on the FG object's game queue (`currentFG->GetCommandQueue()`),
   over FFX's replacement backbuffer, the buffer the game rendered into.
3. `o_FGSCPresent` hands that buffer to FFX, whose optical flow and interpolation read it after
   everything already submitted on the game queue, NR included.

This section first said `state.currentCommandQueue`, reasoning that `DxgiFactory_Hooks` stores the
game's queue there before `FGHooks::CreateSwapChain`. It does, but it stores again when FFX creates its
real swapchain, on FFX's own present queue (`presentInfo.presentQueue`,
`FrameInterpolationSwapchainDX12.cpp:1162`), through the same hooked factory. The pass then ran
unsynchronised with the game's rendering. See [Step 5, measured](#step-5-measured).

**To verify in-game** (PCSX2, `Renderer = 15`, `FGInput=synthesized`, `FGOutput=fsrfg`,
`[FrameGen] Enabled=true`, NR on with HookMethod=2):
- `synthesized FG input on a native D3D12 swapchain, fed from the FG present hook`, once;
- `FSRFG_Dx12::CreateContext D3D12_CreateContext result: 0`;
- `synthesized FG input feeding FSR-FG: WxH zero motion ...`, once;
- DispatchCallback lines with `numGeneratedFrames: 1` at LogLevel 1;
- `DLSS-NR status: present: active ... | N presents : 0 renders`, with N the base rate (about 60 per
  2 s window for a 30 fps game with `SkipDuplicateFrames`), not the doubled presented rate;
- `DLSS-NR present: first pass on the backbuffer (WxH, frame-generation hook)`, and never the
  `swapchain hook` variant: that one would mean the wrapped entry ran the model.

## Step 5, measured

PCSX2 2.9.23 on the 4090 under Proton Experimental, God Hand, `Renderer=15`, `FGInput=synthesized`,
`FGOutput=fsrfg`, `[DlssNr] Enabled=true HookMethod=2`, 2026-09-27. Built from the working tree on
`synth-fg-d3d12` (build id `0dfb0287`), deliberately uncommitted until measured.

**FG works on D3D12.** The log showed the native D3D12 feed and FSR-FG's context, and NR's first pass
came in through the frame-generation hook. The GPU timing counters read 6,211 model evaluations for
12,407 presents: one generated frame per base frame, and NR on base frames only. A window resize
(Hyprland retiling, 836 to 1608 wide) recreated the FG context and rebuilt NR cleanly.

**But the picture was wrong, and it took two fixes.**
- Seen: every frame, real and generated, showed only the model's difference over black, like an edge
  negative. With NR switched off (F7) the picture was right.
- **Cause 1, the main one: the queue.** NR ran on `state.currentCommandQueue`, which by then held
  FFX's present queue (see Ordering above), so it copied half-drawn frames.
  - The same build with `FGInput=auto` (no FFX swapchain, NR through the swapchain hook on the game's
    queue) was correct. With `[FrameGen] Enabled=false` but still `synthesized` it was wrong: the FFX
    swapchain alone was enough.
  - Fix: `FGPresent` passes `currentFG->GetCommandQueue()`, the queue FSR-FG's own lists run on.
- **Cause 2: a false exposure.** In every FG session the DLSS-NR exposure scan adopted `candidate 1 --
  buffer, 8 bytes`, a buffer FFX created during `D3D12_CreateContext`. Sessions without FG adopt
  nothing.
  - Fix: `ScopedInternalResourceCreation` (`State.h`) is set around `FfxApiProxy`'s
    `D3D12_CreateContext`, `Configure` and `Dispatch`, and `ExposureScan::NoteResource` and `NoteUav`
    skip anything created inside it.
  - Afterwards, zero adoptions, but the picture was still wrong until the queue fix.
- **After both fixes:** the title, loading and gameplay screens are correct, and the player confirmed
  the picture.

**Found on the D3D11 route too.** A D3D11 test first showed the model running on none of the frames
handed to it.
- The bridge's real D3D11 swapchain lives on a hidden 1x1 window, and PCSX2 calls
  `ResizeBuffers(0, 0)`, meaning "the window's size". It got 1x1.
- Fix: `Dx11wDx12::ResolveZeroExtent` resolves zeros against the game's window at both resize sites
  and all four creation sites.
  - Revised 2026-09-28: only the hidden swapchain gets the resolved size. The presenter sits in the
    game's window, so it gets the game's extent unchanged, zeros included, and is resized first. The
    hidden swapchain is then sized from the presenter's `GetDesc1`. Before, both got one client-rect
    sample, taken while a fullscreen emulation could still be moving the window from a helper thread.
  - Why it was revised: Generation Zero broke on Rafael's machine the day after this shipped. The
    extent change is hardening only; the cause is in
    [nr-dx11-bridge-host.md](nr-dx11-bridge-host.md#exclusive-fullscreen-on-the-plain-presenter-2026-09-28).
  - Tests: PCSX2's windowed 0x0 is still resolved (`tests/bridge-lifetime`, `fullscreenCases`).
- Separately, `PresentHost` no longer spends its single build and model attempt on a frame below 64
  px (`tests/nr-present-host`).
- The D3D11 route has not been re-run since these fixes.

## Motion into FG, and emulator behaviour: design

Written 2026-09-27, before the code. Everything here is opt-in; with the keys at their defaults the
FG input is byte-for-byte what step 5 shipped.

### The synthesized field as FSR-FG's motion vectors

`[FrameGen] SynthesizedMotion=true` hands FSR-FG the estimator's field
(`SynthMotion::Estimator_Dx12`, synthesized-motion.md) as Velocity instead of the zero field, on both
transports.
- **The convention needs no conversion.** FFX's `motionVectorScale` is documented as "(1.0, 1.0) if
  motion vectors are already in pixel space" (`ffx_framegeneration.h:137`). FSR's motion vectors
  encode "the motion from a pixel in the current frame to the position of that same pixel in the
  previous frame" (`super-resolution-temporal.md:147`, the convention FG shares). That is the
  estimator's contract: current to previous, display pixels, +y down. So `SetMVScale(1, 1)` and the
  `DisplayResolutionMVs` flag stay as they are.
- **The field is used only when it is real.** It is handed over only when the estimator is
  `Ready()`: not on a reset, not during its five warm-up frames, not on a cut it has seen.
  Otherwise FSR-FG gets the zero field as before. `SceneCut()` also sets FG's Reset.
- **No confidence mask.** FSR's interpolator already arbitrates per pixel between the vector field it
  is given and its own optical flow, keeping whichever reconstructs the colour better
  (`frame-interpolation.md`). A forward/backward consistency check would need a second estimate,
  twice the cost. Deferred until a title shows the field misleading FSR.

### Computing the field once when NR and FG both want it

`SynthMotion::Handoff` (`shaders/synth_motion/SynthMotion_Handoff.h`, header only, outside the NR
module) is one slot per process.
- **Tokens.** The transport advances a base-frame token before any consumer of that frame runs: the
  FG present hook on native D3D12, and the bridge's copy on D3D11.
- **Publishing.** The first consumer to record an estimate publishes its field with that token, its
  device and extent, and who it is.
- **Taking.** The second takes the field when the token, device and extent match and the publisher is
  not itself; it then records no estimate of its own.
- **Order per transport:**
  - *D3D12:* FG's feed runs before `fg->Present()`, and NR's pass after it. So FG publishes and NR
    takes. NR's list runs on the same game queue (the step 5 queue fix), after FG's.
  - *D3D11 bridge:* NR's host records in the copy, before the synthesized input, so NR publishes and
    FG takes. Both are on the copy list.
- **Each keeps its own estimator** for when it is alone. A consumer that took leaves its own
  estimator idle, and the frame that estimator last saw goes stale. NR's 250 ms staleness rule, and
  the same rule on the FG side, resets it the next time it records.
- **A warming publisher is waited for.** Until its estimator is `Ready()`, the first consumer
  publishes nothing and calls `AnnounceWarming` for the base frame instead.
  - The second consumer, if it has no estimator yet, sees `PeerWarming` and records nothing: zero
    motion, which is all a second estimator would give over the same warm-up frames.
  - The wait is bounded by `kMaxPeerWaitFrames` (16) fresh frames in a row: frames under 250 ms
    apart, which carry an estimator's history. A warm-up is five. A stale gap starts the count over,
    because it restarted the peer's warm-up too. A frame with no announcement ends the wait at once.
  - **Not in milliseconds.** The first version waited at most 1 s. In Divinity a loading screen
    presented a frame every 2 s, NR's estimator was reset by staleness on each, and FG gave up after
    "2008 ms (still warming)" and built its own. Over such a stretch FG's own estimator would be reset
    on each frame too, so waiting costs nothing there.
  - A consumer that already has an estimator keeps using it.
  - NR does not announce while it runs the optical-flow engine, whose fields are never published.
  - A released estimator withdraws its announcement (`WithdrawWarming`), for the case where the
    transport goes away and the base frame stops advancing.
  - **The bridge host now resets the estimator only after a rebuild** (`_motionResetOwed`). It used to
    pass the model's `_resetOwed`, which stays owed until the model runs. While the model was refused,
    or settling for 500 ms, the estimator was reset every frame and never warmed. FG would then have
    waited the full second for nothing.
- **Lifetime.** An owner withdraws its field from the slot when it releases the estimator, which
  happens only after its own drain. FSR-FG's prepare reads the vectors within the base frame, during
  `Dispatch` on the game queue, so nothing reads a taken field after its frame.
- **Without a transport** (NR on a plain swapchain, no FG), nobody advances the token. The
  publisher check stops a consumer from taking its own field back.
- **Measured on the D3D11 bridge**: Divinity: Original Sin 2, 2026-09-28, build `16dff6de`,
  3440x1440, `[DlssNr] SynthMotion=true` and `[FrameGen] SynthesizedMotion=true`.
  - The log reads `DLSS-NR synthesized motion: first field handed to the model on the D3D11 bridge
    route`.
  - Then `synthesized FG input: motion taken from DLSS-NR's estimate of the frame (3440x1440), no
    second estimate`.
  - The model ran on every base frame handed to it (148 of 148 per 2 s), and the user saw nothing
    wrong.
- **Known waste: both estimators allocate at start.** The log shows two `synthesized motion:
  3440x1440 allocated` lines, 140 ms apart, and FG's `runs its own motion estimate`, all before the
  first take 260 ms later.
  - The cause: NR publishes only once its estimator is `Ready()`, after the warm-up
    (`SynthMotionGuide::Record`). On those frames FG's `Take` fails, so it builds its own
    (`SynthInputs::_RecordMotionOn`).
  - FG's own estimator is still warming on those same frames, so it buys nothing. It then sits idle,
    about 34 MB at this size.
  - **Fixed** by the warm-up wait above (2026-09-28), and verified in Divinity on build `467446e9`.
    - FG logged `DLSS-NR's motion estimate is warming up ... waiting for it rather than running a
      second estimator`, and took NR's field 590 ms later.
    - It then went through a loading screen that presented a frame every 2 s, from 12:12:23 to
      12:12:42, and came out with synthesized motion on every frame of the next window (117 of 117).
    - The session had one `allocated` line, and no `stopped waiting` or `runs its own motion
      estimate`.

### Measured on native Windows (Rafael's RTX 3060, 2026-09-28)

This was the first native-Windows run of the estimator and of synthesized FG, on build `13f10d60`.
Divinity ran at 1920x1080 on the D3D11 bridge, with `WorkingScale=0.5`, `SynthMotion=true`,
`FGInput=synthesized` and `FGOutput=fsrfg`.

- **Twelve minutes, 0 `[E]`.** The model ran on 21217 of 21223 base frames, about 29 fps base, and
  every 2 s window reported synthesized motion.
- **Model time:** 19.5 ms median (p90 21.4) at 960x540. The estimator, guide resample and composition
  together took 0.67 ms outside it.
- **One estimator.** FG logged `waiting for it rather than running a second estimator`, then `motion
  taken from DLSS-NR's estimate ... no second estimate`.
- **The picture.** The user saw the edge shimmer of that title's previous setup gone: zero motion with
  `ZeroGuideReset` had dropped the model's history every frame.
- **FidelityFX DLLs.** FSR-FG needs `amd_fidelityfx_loader_dx12.dll` and
  `amd_fidelityfx_framegeneration_dx12.dll` beside the proxy or in its `OptiScaler\` subfolder. His install
  had neither, so FG did not engage at all until they were copied. Nothing in the log says why beyond the
  `Can't find amd_fidelityfx_*` warnings at start-up.

### What FSR 3.1 cannot do

The FSR 3.1.x swapchain generates exactly one frame (`numGeneratedFrames = 1`,
`FrameInterpolationSwapchainDX12.cpp:1927`). The `outputs[4]` array in the API belongs to the ML
frame generation of FSR 4, which is RDNA 4 only. So adaptive, fractional multipliers aimed at a target
rate (Lossless Scaling's AFG) are out of reach with this output. That would need our own interpolator,
the fallback this note keeps open.

### Fast-motion response

`[FrameGen] SynthesizedFastMotion=<px>` (0, the default, is off). When the median motion magnitude
exceeds that many display pixels per base frame, FG is fed with Reset.
- **What Reset does.** On a reset FSR's interpolation "copies the current back buffer and doesn't
  interpolate" (`ffx_frameinterpolation.h:174`). That is AMD AFMF's Fast Motion Response: repeat the
  frame instead of smearing it. FG stays active, so there is no pause and resume.
- **Hysteresis.** The response ends after two consecutive samples below 75% of the threshold.
- **The statistic.** 16 rows of the field are copied to a readback ring and read three confirmed
  frames later, the same rule as the estimator's `SceneCut()`. Motion is coherent over a few frames,
  so the lag costs the first two or three frames of a burst, not its body.

### Low-fps floor

`[FrameGen] SynthesizedMinFps=<fps>` (0, the default, is off).
- **Measurement.** The base rate comes from the interval between feeds, smoothed.
- **Below the floor, FG is not fed at all.** A sustained condition should not pay for generation it
  then throws away. FSR-FG pauses itself after three presents without new data
  (`FSRFG_Dx12::Present`), and when feeding resumes it waits ten frames
  (`IFGFeature::UpdateTarget`).
- **Hysteresis.** Feeding resumes above the floor plus 15%.

### Duplicate presents: detected, not collapsed

An emulator showing 30 fps content at 60 Hz presents every frame twice. Interpolating then produces
uneven cadence rather than smoothness.
- **Why not collapse.** Collapsing would mean skipping the duplicate present, and that must be decided
  before this frame's Present. The GPU knows whether the frame equals the previous one only after it
  has rendered it, so a same-frame decision needs a CPU wait on the GPU every frame. That serialises a
  present path emulators already bottleneck.
- **What is done instead: detect late and advise.**
  - The same row samples show whether a frame was identical to the previous one: all motion exactly
    zero.
  - An interleaved pattern of identical and moving frames logs one line naming the emulator's own
    option (PCSX2: Skip Presenting Duplicate Frames). The threshold is at least a quarter of the last
    60 samples identical, with at least 20 alternations.
  - A game that is merely paused, where every sample is zero, does not trigger it.

### Verification in PCSX2

- `[FrameGen] SynthesizedMotion=true` with `[DlssNr] SynthMotion=true`:
  - the log names FG's own estimator once;
  - NR's first-field line says it took the field from frame generation, and there is no second
    estimator allocation;
  - the motion into FSR is visible in FSR's debug view (`[FrameGen] DebugView`).
- `SynthesizedFastMotion=24`: whipping the camera shows repeated frames instead of smeared ones.
- A duplicate-presenting setup (PCSX2 with Skip Presenting Duplicate Frames off) logs the advice
  once.

## Open questions

- How FSR's interpolator behaves with flat depth: its disocclusion and inpainting assume real depth.
  Step 1 shows it.
- The units and sign FSR expects from our field, pinned by capture, not asserted. A flipped sign
  reads as "synthesis made it worse".
- Whether 8x8 blocks are fine enough for text-heavy scenes, or the HUD mask has to carry them.
- Latency. Interpolation holds one base frame. Divinity's turn-based pace tolerates that; a
  first-person game at 30 fps base would not, and the help text should say so.
