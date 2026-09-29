# Frame generation for titles that give no motion

Status: **steps 1 and 2 built and measured** (`90990bba`, `d954692d`; see
[Step 1, measured](#step-1-measured) and [Step 2, measured](#step-2-measured)), merged in `54d900e7`.
**Step 5 (native D3D12) built and measured** in PCSX2; see [Step 5, built](#step-5-built) and
[Step 5, measured](#step-5-measured). Written 2026-09-27 against `944aff56`. The code this plans lives
outside the NR module (see [Where the code goes](#where-the-code-goes)); the note sits here because
it builds on the present host and on [synthesized-motion.md](synthesized-motion.md).
**The HUD (step 4)** is designed in [The HUD: near depth and a UI layer](#the-hud-near-depth-and-a-ui-layer)
(2026-09-28, branch `fg-hud-depth-ui`), which also corrects what this note first planned for it.
**The UI layer's own mask** (recall of outlines, thin elements and panels, and a feathered margin for the band)
is in [The HUD layer's own mask: recall and a margin](#the-hud-layers-own-mask-recall-and-a-margin) (2026-09-29,
branch `synth-hud-layer-recall`).
**The DLSS-G output** (`FGOutput=dlssg` beside FSR-FG) is in [DLSS-G output](#dlss-g-output) (2026-09-28,
branch `synth-fg-dlssg`): designed and written from code, not yet measured in a game.

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
- **The FSR-FG output and its pacing already work here.** `FSRFG_Dx12` owns the FFX swapchain, pacing
  and inpainting. This bullet first said that `FGDrawUIOverFG` plus `RUI_Dx12` composite a `UIColor`
  resource over every presented frame. They do not: they draw it into the backbuffer before FFX's
  Present, into the frame FFX then interpolates. FFX's own UI composition was never used here. See
  [Correction: FGDrawUIOverFG is not a UI layer](#correction-fgdrawuioverfg-is-not-a-ui-layer).
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
- **The composite.** The mask becomes the alpha of a texture cut from the current frame, composited
  over every generated frame. The interface is then never warped, only held still, which is what it
  is doing anyway. As first written, this bullet sent it through `FGDrawUIOverFG`; that path cannot do
  it, and the design that replaced it is [The HUD: near depth and a UI layer](#the-hud-near-depth-and-a-ui-layer).
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
4. **The HUD mask.** Judged on Divinity's hotbar and tooltips during a pan. First planned into
   `UIColor` through `FGDrawUIOverFG`; built instead as near depth and an FFX UI layer, see
   [The HUD: near depth and a UI layer](#the-hud-near-depth-and-a-ui-layer).
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
exceeds that many display pixels per base frame, FG is fed with Reset. Since 2026-09-28 this is the
hard cap beside the coherence rule ("Coherence, not speed" below).
- **What Reset does.** On a reset FSR's interpolation "copies the current back buffer and doesn't
  interpolate" (`ffx_frameinterpolation.h:174`). That is AMD AFMF's Fast Motion Response: repeat the
  frame instead of smearing it. FG stays active, so there is no pause and resume.
- **Hysteresis.** The response ends after two consecutive samples below 75% of the threshold.
- **The statistic.** 16 rows of the field are copied to a readback ring and read three confirmed
  frames later, the same rule as the estimator's `SceneCut()`. Motion is coherent over a few frames,
  so the lag costs the first two or three frames of a burst, not its body.

#### Coherence, not speed (2026-09-28, before the code)

**Measured.** Generation Zero, first person, about 27 fps base, `SynthesizedFastMotion=12`. It repeated
20-81% of base frames while the camera moved (`fast motion repeated N of the last M base frames`), so
interpolation was mostly off.

**What a Reset costs**, read in `external/FidelityFX-SDK-v2/Kits/FidelityFX/framegeneration/fsr3/`.
The cost goes well beyond the repeated frame:
- The frame is copied, not interpolated (`include/gpu/frameinterpolation/ffx_frameinterpolation.h:174`).
- The preparation passes are skipped (`internal/ffx_frameinterpolation.cpp:1155`, `:1211`).
- FSR's own optical flow restarts at frame 0 (`internal/ffx_opticalflow.cpp:665`). It treats frames 0-5
  as a scene change and stores zero vectors for them (`ffx_opticalflow_callbacks_hlsl.h:529-537`,
  `ffx_opticalflow_compute_optical_flow_v5.h:216`).
- For 10 frames after it, the interpolator takes the game vectors outright, with no arbitration
  against its optical flow (`ffx_frameinterpolation.h:162-163`).

So a burst of repeats keeps FSR in its degraded start-up state for as long as the burst lasts.

**Why speed is the wrong trigger.** A uniform pan with accurate vectors interpolates correctly at any
speed: every pixel moves by the same vector, and only the frame border is disoccluded. Interpolation
fails where the vectors disagree locally: parallax, disocclusion, a thin foreground over a far
background. The median magnitude cannot tell a fast pan from a fast strafe past trees.

**The rule.**
- **Statistics.** From the same 16 rows, sampled every 4 px, a pure function (`Synth_MotionStats.h`)
  computes:
  - the median and p90 of |v|;
  - the per-component median vector;
  - the median as a fraction of the width;
  - the **incoherent share**. This is the share of horizontally neighbouring samples whose vectors
    differ by more than max(2 px, 25% of the larger of the two).
- **Repeat on incoherent motion.** When the incoherent share exceeds `[FrameGen]
  SynthesizedIncoherence` and the median exceeds 1% of the width, the frame repeats.
- **`SynthesizedFastMotion` becomes a hard cap on the median, whatever the coherence.** 0 means no cap.
  The number means what it always meant, so an existing ini behaves exactly as before.
- **`SynthesizedIncoherence` is a new key**, a fraction from 0 to 1. The default is 0, which is off.
  With it at 0 the response is the old one to the byte (DEVELOPMENT.md invariant 1). That is why this
  is a key and not a constant: always on, it would change every `SynthesizedMotion=true` setup and could
  not be switched off. It also gives the threshold a live menu slider for tuning, because it has not
  been measured yet. A suggested start is 0.25.
- **Hysteresis, as before.** Each trigger holds until its levels fall below 75%: the incoherent share
  and the 1% gate for one, the cap for the other. Two calm samples in a row end the response. A
  threshold change clears the state, and the next sample decides afresh.

**Why these numbers.**
- **2 px floor.** A smaller disagreement moves content by at most 1 px at the midpoint frame. Below it
  is estimator noise.
- **25% relative.** The field is bilinear between 8x8 block centres. A smooth field (zoom, walking
  forward) changes by a few percent over 4 px. A depth edge between two blocks appears as two steps,
  each half the difference. For a foreground at 40 px over a background at 10 px, that is a step of
  15 px against a 10 px threshold. Static HUD pixels are zero from the expand's per-pixel choice, and
  only their edges count.
- **1% of the width.** At the midpoint a wrong vector misplaces content by at most half the local
  disagreement, which is bounded by the motion. Below 1% (19 px at 1920, 34 px at 3440) that halo is
  small next to the ten degraded frames a Reset costs. It is a constant, to be revisited from the
  logged p50.
- **Horizontal pairs only.** The sampled rows are H/16 apart, 90 px at 1440. That is not local. Over
  that distance a smooth field changes by more than 25% (the ground plane when walking forward) and
  still interpolates fine. A depth edge crosses the sampled rows unless it runs exactly horizontal.
- **0.25, the suggested start.** A coherent pan with a HUD scores a few percent (the overlay edges).
  A strafe past tree trunks adds a few pairs per trunk per row. The share passes a quarter when the
  field breaks up broadly: near foliage, or the estimator losing the match. This is an estimate, not
  a measurement.

**Logged.** The 10 s summary adds the window's mean and maximum of p50, p90 and the incoherent share.
It is written while the response is configured and the window repeated a frame or saw a median above
1% of the width.

**Built** the same day. One change from the plan: the median vector is taken over every fourth vector
(16 px apart). Its two selections over every vector cost more than the rest of a readback together.
The CPU time per readback at 3440 wide, measured on the dev box with g++ `-O2`:
- the old loop, 67 µs;
- with a full median vector, 261 µs;
- as built, 148 µs.

That is the extra cost of a base frame on the present thread.

**Not done.** A Reset is whole-frame in FSR, so there is no regional repeat. The thresholds have not
been measured in a game yet. Tests: `tests/fg-synth-policy`.

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

## The HUD: near depth and a UI layer

Written 2026-09-28, before the code, on branch `fg-hud-depth-ui`; built the same day, see
[Built and measured](#built-and-measured-2026-09-28). Two keys under `[FrameGen]`, read every base frame:
- `SynthesizedHudDepth` (Fix A) defaults on: the GPU harness showed the mask marks no scenery. It acts
  only with `SynthesizedMotion`, which is itself off by default.
- `SynthesizedHudLayer` (Fix B) defaults off until it has been seen in a game.

With both off, or without synthesized motion and with the layer off, the input is byte-for-byte what it
was.

Sources below are FSR 3.1.6's frame interpolation in the tree,
`external/FidelityFX-SDK-v2/Kits/FidelityFX/framegeneration/fsr3/`. `fi/` stands for its
`include/gpu/frameinterpolation/`, and `dx12/` for its swapchain.

### What goes wrong at the HUD, read from FSR's code

Our depth is a constant: `SynthInputs` clears an R32_FLOAT to 0 once and sets `InvertedDepth`. The HUD's
own vectors are already zero wherever the expand's per-pixel choice finds them
([synthesized-motion.md](synthesized-motion.md), "Static overlays"). With synthesized motion on, two
things still go wrong around the interface, and both come from the flat depth.

- **Every pixel is a 50/50 blend of its two samples.**
  - FSR marks a sample occluded only where the interpolated pixel's depth lies behind the depth found
    at that sample by more than a separation margin (`fi/ffx_frameinterpolation_disocclusion_mask.h:65-116`).
    Flat depth never gives that, so the mask is (1, 1) everywhere.
  - `computeInterpolatedColor` then blends the previous-frame and current-frame samples at t = 0.5
    (`fi/ffx_frameinterpolation.h:100-123`).
  - A background pixel beside a HUD element samples the previous frame at `q + v/2` and the current one
    at `q - v/2`, with `v` its current-to-previous vector. When one of those lands on the element, which
    sits in the same place in both frames, the element's colour comes in at half weight.
  - So each side of an element gets a band half as wide as the frame-to-frame motion: the ~24 px band
    beside the HUD at 48 px of motion per base frame.
- **HUD pixels lose vector collisions.**
  - Every current pixel scatters its half vector to where it lands at t = 0.5. Each destination keeps the
    maximum of a packed key: 10 bits of depth priority, then 5 bits of colour agreement, then the raw
    float16 bits of the vector. X and Y are separate atomics (`fi/ffx_frameinterpolation_common.h:218-229`,
    `:266-272`; `fi/ffx_frameinterpolation_callbacks_hlsl.h:667-671`;
    `fi/ffx_frameinterpolation_game_motion_vector_field.h:26-76`).
  - Flat depth ties the first 10 bits everywhere. A HUD pixel and a background pixel with a correct
    vector both score full colour agreement. So the vector bits decide, and any non-zero vector
    scattered onto a HUD pixel beats the HUD's own zero.
  - The HUD pixel is then interpolated along the camera's vector: the smear the per-pixel zero was
    meant to stop, brought back at the scatter.

### Fix A: the HUD is near

`[FrameGen] SynthesizedHudDepth`. The depth handed to FSR is written every base frame: 1.0 (the near
plane, inverted) where the static-overlay mask is at least 0.5, and 0.0 everywhere else.

- **Collisions.** Priority comes from view-space depth (`getPriorityFactorFromViewSpaceDepth`,
  `fi/ffx_frameinterpolation_game_motion_vector_field.h:26-33`). With the config's near 0.1 and far
  100000, depth 1 scores about 700 of 1023 and depth 0 about 22. A HUD pixel then keeps its own zero
  vector whatever else lands on it.
- **Disocclusion.** Take a band pixel at depth 0 whose sample lands on the HUD, at depth 1, in one of
  the frames. Its depth is far behind that sample, so that side is marked occluded.
  - The pixel is then taken from the other frame only (t = 0 or 1, `fi/ffx_frameinterpolation.h:118-122`).
  - A disoccluded pixel also prefers the game vectors over FSR's own optical flow: `fDisoccludedFactor`
    lifts `fGame_Sim` to 1 (`:123`, `:159`).
- **Camera values do not matter.** Near and far only turn device depth into view depth
  (`internal/ffx_frameinterpolation.cpp:815-850`, which also ignores their order). A step from 0 to 1
  spans the whole range whatever they are, and FOV only scales the separation margin, which this step
  exceeds by orders of magnitude.
- **It needs synthesized motion.** With zero game vectors, both samples sit on the pixel itself, so no
  side is ever occluded, and every vector ties at zero anyway. So the key does nothing, and costs nothing,
  without `SynthesizedMotion`.
- FSR's own optical-flow field does not read depth (`fi/ffx_frameinterpolation_optical_flow_vector_field.h`).

### Fix B: a UI layer FFX composes

`[FrameGen] SynthesizedHudLayer`.

- **What FFX composes.** FFX's swapchain composites a registered UI resource over every frame it
  presents, generated and real: `lerp(backbuffer, ui.rgb, ui.a)`
  (`internal/shaders/FrameInterpolationSwapchainUiComposition.hlsl:31-40`). That is the default present
  callback, which is the one in use whenever OptiScaler installs none; it installs one only for
  reprojection, which needs a hudless frame this input never has.
- **What we register.** An RGBA16F layer: rgb is the frame as it will be presented, alpha is the mask.
- **In generated frames,** the masked pixels are exactly the newer base frame's, whatever the
  interpolator did there. The HUD is held, never warped or ghosted.
- **In real frames,** `lerp(x, x, a) = x`. The frame is unchanged: RGBA16F holds an 8- or 10-bit UNORM
  value to within a quarter of a step, which rounds back to the same value, and scRGB exactly.
- **What it does not fix: the band.** Band pixels are background, not masked, so the layer leaves them
  to the interpolator. The band is Fix A's job. B makes the interface itself exact; A keeps the
  interpolation around it honest.
- **Double buffering.** `FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING`
  makes the swapchain copy the layer on the game queue inside its own Present (`dx12/FrameInterpolationSwapchainDX12.cpp:2179-2181`,
  `copyUiResource`). The presenter thread composes from that copy, so the next base frame can
  overwrite ours.
  - The copy consumes the registration (`currentUiSurface.resource = nullptr`), so it is renewed at
    every dispatch.
  - A present with none drops the internal copy (`verifyUiDuplicateResource`). That makes switching the
    key off clean.
- **Format.** The composition reads the layer through a view of its own format (`convertFormatSrv`), so
  it need not match the swapchain.
- **Which frame the colour comes from.**
  - *Bridge:* the frame copied into the presenter, which is DLSS-NR's output when the host ran. It is
    recorded on the copy list.
  - *Native D3D12:* DLSS-NR's present pass edits the backbuffer in place after `fg->Present()`. So the
    layer is recorded after that pass, just before `o_FGSCPresent`, on FG's queue. The mask was recorded
    earlier, before FSR's prepare. Taking the colour early instead would put the pre-NR pixels into
    real frames too.
- **Wiring.** The layer goes in as `UIColor` through `SetResource`, as with every FG input.
  - `FSRFG_Dx12::Dispatch` registers it as FFX's UI resource only for `FGInput::Synthesized`. Every
    other input keeps today's empty registration.
  - `FGDrawUIOverFG` skips it for this input.
  - `[FrameGen] DisableUI` refuses it like any UI.

### Correction: FGDrawUIOverFG is not a UI layer

This note said twice that `FGDrawUIOverFG` and `RUI_Dx12` composite a `UIColor` over every presented
frame, and planned step 4 through them.
- **What they do.** `FSRFG_Dx12::Present` draws `UIColor` into the backbuffer, on the swapchain list,
  before FFX's Present. That is the frame FFX interpolates from.
- **Who that is for.** A title that hands a hudless frame and a separate UI texture: FFX interpolates
  the hudless and puts the interface back from the difference.
- **Why it does nothing here.** With no hudless frame, drawing the interface into the backbuffer changes
  nothing about how it is interpolated.
- **What does work.** FFX's own UI registration (`uiDesc` in `FSRFG_Dx12::Dispatch`). Until this
  change it was always registered empty.

### The mask

- **Which detector.** DLSS-NR's static-overlay rule ([hud-protection.md](hud-protection.md)), not the
  expand's per-pixel zero choice.
  - The rule: a still 5x5 core; motion on all four axis sides at 6, 14 or 24 px; contrast at least 0.15;
    an 8-frame entry streak; a 1.5 s decay; dropped on its own change above 0.2; exported only with at
    least 3 protected pixels in the 5x5; grown by 1 px.
  - Why not the expand's choice: it fires on any pixel the previous frame reproduces better standing
    still. That includes band pixels (uncovered background that happens to match in place) and blocks of
    mixed motion ([synthesized-motion.md](synthesized-motion.md), "What it cannot fix"). Marking those
    near would pin moving scenery.
- **Where it lives.**
  - The per-pixel rule, `shaders/dlssnr/precompile/dlssnr_uimask_rule.h`, moves to
    `shaders/synth_motion/precompile/static_overlay_rule.h`, with the same text.
  - NR's shader includes it from there. Its bytecode is recompiled and must be byte-identical to the
    committed one, which makes the move checked, not assumed.
  - FG gets its own pass, `SynthMotion::Overlay_Dx12` (`shaders/synth_motion/SynthOverlay_Dx12.*`), with
    NR's thresholds. It runs at display size on the game's frame, on the list the synthesized feed
    already records on: the bridge's copy list, or on native D3D12 the motion list executed on FG's
    queue. Either way it lands before FSR's prepare reads depth.
  - *Added while building it:* FG's pass uses a **3x3 still core** where NR's uses 5x5. The core radius
    became a knob of the shared rule (`UM_CORE_RADIUS`, default 2, so NR's bytecode is unchanged). A 5x5
    core marks nothing of a 2 px crosshair arm or a 3 px stroke with a 1 px outline, because the moving
    scene lies within 2 px of every pixel of them, and those are exactly what FG smears. The fixtures that
    made the core necessary still mark nothing at 3x3: sand beside a swaying coat, and a concave gap.
    `tests/fg-synth-policy` reruns them at FG's radius.
  - *Found while building it:* the macros `dlssnr_uimask.hlsl` hands the rule substitute the swizzle
    (`UM_PROT(x, y) ... .x`), so NR's committed mask is 1 on nearly every pixel. FG's shader names the
    parameters `px, py`. NR's is left as it was, and the defect is written up in
    [hud-protection.md](hud-protection.md#the-mask-macros-every-pixel-protected-found-2026-09-28-not-fixed).
  - So FG depends on nothing in the NR module, and the NR module stays removable as one block.
- **Not shared with NR at run time.** NR's mask is computed at the working size, on the model's proxy
  input, and only with `UiProtection` on. FG needs it at display size, on the game's frame, every frame.
  A handoff like `SynthMotion::Handoff` would only pay when both run at scale 1 with `UiProtection` on.
  Left for later.
- **History.** Like the estimator's, it advances only when the list that carried it executed. A gap of
  more than 250 ms between recordings starts the history over.

### Risks

- **A false positive cuts a hole.**
  - A pixel wrongly marked is near (A), and with the layer it is shown from the base frame (B).
  - An object that crosses it in a generated frame loses the collision there (A), or is cut to its
    base-frame position (B).
  - The rule's gates exist to make false positives rare: the still core, all four sides moving, and the
    streak. That is what took the sand rim away in PCSX2 (hud-protection.md).
  - A marked pixel whose own value jumps by more than 0.2 is released on that very frame. An object close
    in luma to what it covers is not, until the 1.5 s decay.
- **Recall is partial.**
  - Large solid HUD shapes are marked only at their outlined inner edges (hud-protection.md, "What it
    costs in recall"). Their outer edge sees motion on one side only.
  - So the band beside the outer edge of a large solid panel stays. Crosshairs, glyphs and thin lines
    are what this covers.
  - Interface over a still scene is never marked, and then has nothing to protect it from.
- **Old FFX runtimes.** The double-buffering flag is in the API header this tree builds against
  (`external/FidelityFX-SDK/ffx-api/include/ffx_api/ffx_framegeneration.h:58`). A runtime that ignores it
  would compose from our layer at present time, while the next base frame may be writing it. That gives
  a torn HUD in one frame, not a fault. Which version Rafael copied in is not recorded ("Measured on
  native Windows", above); the log line `FfxApi Dx12 FG version` says, and it goes into his test notes.
- **Proton is not a D3D12 validator** (CLAUDE.md). So, by the D3D12 rules rather than by what ran here:
  - UAV stores only to R32_FLOAT, R8_UNORM, R16_FLOAT, R16G16_FLOAT and R16G16B16A16_FLOAT, all of which
    every D3D12 device supports.
  - The previous frame's state is read through SRVs, never through typed UAV loads; beyond the R32
    formats those are optional.
  - No constant buffer: root constants.
  - Every resource is created in the state it rests in, and returned to it.

### Tests

- `nr-uimask-rule` runs unchanged against the moved header.
- `fg-synth-policy` covers which key does what: A only with motion, B refused by `DisableUI`, and which
  depth FSR is handed.
- `synth-motion-d3d12` runs the FG pass on the harness's own frames, on a real device.
  - On `overlay_*`: depth 1 on overlay pixels and 0 on background. Precision and recall are reported,
    per element.
  - On the pans, the moving object and the static sequence: no pixel marked.
  - The layer's alpha equals the mask, and its rgb equals the frame.
  - None of this runs FSR, so none of it says what FSR does with the result.

### Built and measured (2026-09-28)

**Code.**
- `SynthMotion::Overlay_Dx12`, with shaders `synth_overlay_detect.hlsl` and `synth_overlay_layer.hlsl`.
- `Synth_Hud.h`: which fix runs and what is fed.
- `SynthInputs::RecordOverlay` on the bridge.
- `RecordFrameOnQueue` and `RecordLayerOnQueue` on native D3D12. The first is the old
  `RecordMotionOnQueue`, which now carries the mask too; the second is called from `FGHooks::FGPresent`
  after DLSS-NR's pass.
- The UI registration in `FSRFG_Dx12::Dispatch`.
- Two menu checkboxes under the synthesized input ("HUD depth", "HUD layer").

**GPU harness** (`tests/synth-motion-d3d12`, RTX 4090 under vkd3d-proton, clocks unlocked). It runs the
pass on every frame of every sequence and reads back its depth, mask and layer.
- **No scenery marked.** Not one pixel on the eight pans, the moving object, the static sequence, the
  cut or the abandon, on any frame.
- **Precision** on the overlay sequences: 100.00% of the marked pixels are overlay pixels. None is even
  the 1 px growth.
- **Recall** over the last ten of 40 frames:

  | pan per frame | crosshair | floating text | panel |
  |---|---|---|---|
  | 8 px | 32.0% | 33.5% | 0% |
  | (3, 2) px | 18.1% | 26.9% | 0% |

  - About half of each element's pixels are its 1 px dark outline. The rule never marks those, because
    its 3x3 core reaches the scene.
  - The rest builds up as the scene moves past: 6% of the crosshair at t=9, 16% at t=14, 34% at t=39
    (8 px a frame).
  - The panel's interior never sees motion on four sides.
- **Exactness.** The depth is 0 or 1 everywhere and agrees with the mask. The layer is the frame's own rgb
  with the mask as alpha, byte for byte, on every frame.
- **Cost of the mask plus the layer:** 0.084 ms at 1920x1080 and 0.19 ms at 3440x1440. The estimator is
  0.29 and 0.55 ms in the same run.
  - A second run, with the card clocked down, read 0.68 and 1.74 ms against the estimator's 1.43 and
    3.66 ms. The clocks were not lockable, so only the ratio holds: about a third to a half of the
    estimator's time.
  - An RTX 3060 has about a third of this card's shader throughput, so expect about three times as much
    there (*estimate*).
- **Memory at 3440x1440:** about 85 MB for the mask's state, depth and mask. The layer adds 20 MB (RGBA8)
  or 40 MB (RGBA16F), and FFX keeps a copy of it of the same size.

**Not measured:** anything FSR does with it. The harness does not run FSR.

### What needs a game

Rafael's RTX 3060, native Windows, with `[FrameGen] DebugView`. FFX's debug grid shows the
game-vector field's depth priority (top middle) and the disocclusion mask (bottom left)
(`fi/ffx_frameinterpolation_debug_view.h:160-167`).
- With A on and synthesized motion, the crosshair and HUD text should read as high priority.
- The strip beside them should read as disoccluded while the camera pans.
- Then, with A and B off and on, on the same pan: the crosshair, HUD text and the band beside them.
- Also the cost of the pass next to the model.
- **The log, once each:**
  - `synthesized FG HUD mask: WxH allocated`;
  - `synthesized FG input: FSR-FG now gets the HUD mask as depth`, with A;
  - `synthesized FG HUD mask: UI layer WxH, format N` and `the HUD layer ... is FFX's UI resource`, with B;
  - `FfxApi Dx12 FG version`, for which FFX runtime composed it.
- **Also to watch:** a crosshair over a still scene is never marked, and a camera that stops lets the
  marks fade over about 1.5 s. Neither is a fault.
- **Configuration:** Generation Zero on the D3D11 bridge (`FGInput=synthesized`, `SynthesizedMotion=true`)
  is the title that showed the smear. Also PCSX2 on D3D12, for the late layer after DLSS-NR's pass.

## The HUD layer's own mask: recall and a margin

Written 2026-09-29, before the code, on branch `synth-hud-layer-recall`, against `0914ca4f`; built and measured
the same day, see [Built and measured](#built-and-measured-2026-09-29), which also says what the build changed.
The text below is the design as built.

**What was seen.** Generation Zero, FSR-FG, `SynthesizedHudDepth=true` and `SynthesizedHudLayer=true`: the HUD
still smears while the camera turns.

**Why, from [Built and measured](#built-and-measured-2026-09-28).** The layer's alpha is the strict mask, and
the strict mask marks little of what smears:
- about a third of the crosshair and of the floating text, and none of the panel;
- never a 1 px outline or drop shadow. The 3x3 still core of such a pixel always reaches the moving scene;
- nothing of an element with no stroke 3 px thick. A thin crosshair or a compass tick with a 1 px shadow has no
  pixel whose 3x3 is still, so it gets no mark at all. Generation Zero's HUD is mostly such elements.

The layer also leaves the band beside each element to the interpolator, by design ("What it does not fix: the
band", above). So the unmarked outlines and the band are exactly what FSR drags.

**What the research converged on.** Lossless Scaling's UI detection, Qualcomm's US11587208B2 and the CVPR 2023
D-map work do the same three things:
1. find static pixels with a test over several frames;
2. grow and feather that mask so that it covers the smear band around each element;
3. show the real frame's pixels there.

### Two masks, one per fix

- **Depth (Fix A) keeps the strict mask, unchanged.** A false positive there is a pixel FSR treats as near: it
  wins every vector collision and occludes its neighbours, so moving scenery gets pinned. Precision is what
  matters for it, and it has it (100.00% within 1 px on every overlay sequence).
- **The layer (Fix B) gets a mask of its own, a superset of the strict one.** A false positive there shows the
  base frame's pixel in generated frames. For a static pixel that is what interpolation would show anyway. The
  cost is local judder, not a pinned object, so the layer can take much more.

### The layer's mask, in three steps

1. **Seeds**, per pixel. A pixel is a seed while all of these hold:
   - it has been still for at least K = 8 frames in a row: within 0.012 of its anchor, the luma it had when the
     still run began. That is the "|I_t - I_{t-k}| small for k = 1, 2, 4" test for every k up to the run's
     length, any two frames of the run within twice 0.012 of each other, at two 8-bit channels of state (the
     count and the anchor) instead of three frames of luma history. *Changed while building it:* the plan
     compared each frame with the one before, which lets a slow drift through;
   - it has contrast: 0.15 against a 4-neighbour, the strict rule's figure;
   - **the scene moves on at least three of the four axis sides within R.** R is about a sixth of the frame's
     height (120 px at 720p, 240 px at 1440p), read from a tile map (below). That is the strict rule's own reach
     of 24 px, made large enough to span a panel. *Changed while building it:* the plan said all four, which
     seeds only the middle of a panel wider than R;
   - **two perpendicular orientations of static structure** within the surrounding 5x5 tiles, about 20 px:
     horizontal and vertical edges, or both diagonals. One orientation per pixel, the direction it changes least
     along, and only against neighbours that are steady too: static structure is still on both sides of its
     edge. *Both added while building it;*
   - **the camera moves**: at least half of the textured tiles are moving, from 256 tiles sampled over the frame.

   A seed stays one for 90 frames (1.5 s, the strict decay) while it stays still, and drops at once when it
   changes. **There is no still core.** The core is what kept outlines and thin elements out.
2. **Growth through still pixels.** A geodesic distance from the nearest seed, carried in the state and
   advanced 2 px a frame through pixels still for K frames, up to G = 48 px. It reaches the pixels no seed test
   can take:
   - 1 px outlines and drop shadows;
   - stroke interiors and flat panel fills;
   - the long straight edges of frames and bars, from their seeded corners.

   It stops at anything that moves: the scene, and a translucent panel's fill. The layer's core is then the
   maximum of the strict mask and the grown set, so the layer covers everything the depth marks.
3. **The band.** The layer's alpha is the core dilated and feathered. It is 1 within M/2 px of the core, then
   falls linearly to 0 at M + 1 px.
   - M is `[FrameGen] SynthesizedHudMargin`, in pixels, 0 to 24, and is only used with `SynthesizedHudLayer`.
   - FSR's band is half the motion wide on each side ("What goes wrong at the HUD", above). A margin of M
     covers the band fully up to M px of motion per base frame, and partly up to 2M.
   - The default is chosen from the harness below. The in-game speed that matters has not been measured.

**Why the test is not "one side moving is enough".** It was the first proposal, and it seeds:
- the static background beside every moving object, as in the harness's `object` sequence, where the
  camera is still and one patch moves;
- every straight scenery edge that runs along the motion. That is the aperture problem: strafe along a wall,
  and its skirting line stays still on screen while the wall around it moves.

Each seed then holds a ring of moving scenery. Three sides, two orientations and a moving camera answer both:
- the patch has motion on one side only, and with a still camera the camera gate is closed anyway;
- the skirting line has one orientation only;
- HUD elements have both: crosshair arms, glyph strokes, the corners of frames and panels;
- rain or water around a still object in a still scene moves on every side, but the camera gate is shut.

### Where it runs

- **The detect pass gets a second permutation.** `SynthOverlay_DetectLayer` is compiled from the same source
  with `SYNTH_OVERLAY_LAYER=1`, and runs only with the layer on. With the layer off the pass is today's bytecode
  and nothing new is allocated, so the depth-only default costs and produces exactly what it did.
- **A tile map.** Each 8x8 thread group writes one RGBA8 texel per frame, all counts, exact in 8 bits:
  - its pixels that moved (luma change above 0.02, the strict rule's figure); the tile moves when half do;
  - its pixels that are textured (4-neighbour contrast above 0.04); the tile is textured when a quarter are;
  - the orientations of its static structure, and whether it holds some of the layer's core;
  - its pixels inside the image.

  The next frame reads it, so the side scan, the orientation test and the camera gate are a frame late. The
  camera turns continuously, and a seed already holds for 1.5 s, so the lag costs one frame when motion starts.
- **The per-pixel state** is one RGBA8 texel: still frames, hold, grown distance, anchor. It is a ping-pong pair
  beside the strict rule's, advanced by the same confirm and abandon.
- **The rule is a header**, `precompile/static_overlay_layer.h`, as `static_overlay_rule.h` is. The shader and a
  host test run the same decisions. `static_overlay_rule.h` and DLSS-NR's pass are not touched, so NR's bytecode
  stays byte-identical by construction.
- **The layer pass does the band.** It keeps its place (after DLSS-NR's pass on native D3D12, on the copy list
  on the bridge). It reads the core with an apron into group-shared memory and runs a separable max filter:
  16x16 groups, about 20 KB of group-shared memory at the 24 px cap, which is under D3D12's 32 KB. A group whose
  apron reaches no tile with core in it, by the tile map the same Record wrote, loads nothing more.
- **Memory**, with the layer on only, at 3440x1440: about 40 MB for the state pair, 5 MB for the core, and
  under 1 MB for the tiles.

### Risks

- **The judder ring.** In a generated frame the band shows the base frame's background. Around each element,
  a ring up to M px wide moves at the base rate instead of smearing, and the feather blends two positions of
  the background. That is Lossless Scaling's trade. It is also why M is a key and 0 turns the band off.
- **A false seed holds a ring of scenery.** It needs static structure with two orientations, moving scenery on
  four sides within R, and a moving camera. Under translation a far object between near moving ones can have
  all of that. So can a third-person character that the camera orbits, but the strict rule already marks that
  as near today.
- **Growth leaks into flat regions** touching the HUD, such as sky, up to G px. Holding a flat region changes
  nothing; the band around the leak holds whatever moves beside it.
- **Translucent panels.** Their fill is never still, so it is never in the core. Only the opaque border and
  text are, plus the band. A uniformly tinted interior interpolates correctly with the scene's vectors; its
  edge is what the band is for.
- **A fast small object crossing the layer** vanishes there in generated frames, as with every held pixel.
- **Onset.** A HUD element that just appeared or changed needs K frames still before it seeds or grows. Its
  neighbours' band covers part of it meanwhile.
- **Proton is not a D3D12 validator.** So, by the rules rather than by what ran here:
  - the UAV stores are to R8_UNORM and R8G8B8A8_UNORM, which every D3D12 device supports;
  - the previous state and tiles are read through SRVs;
  - there are no constant-buffer views, only root constants;
  - group-shared memory stays under 32 KB;
  - the root signature grows to 24 root constants, 5 SRVs and 7 UAVs. The old detect bytecode binds a subset
    of that, which is legal.

### Tests

- **`tests/synth-motion-d3d12`.**
  - The `hud_*` sequences gain two elements: a 1 px hollow frame with a 1 px drop shadow, and a translucent
    panel with an opaque 1 px border and outlined text.
  - Every overlay pixel is classed as stroke, outline or fill.
  - Reported per element and part: the strict mask's recall, which is also the old layer's alpha, next to the
    new core's and the new alpha's.
  - The band: mean alpha in rings 1-2, 3-4, 5-8, 9-16 and 17-24 px from the nearest element.
  - The smear set under the half-vector model: background pixels whose sample at q + v/2 or q - v/2 lands on the
    overlay. Its coverage, and the held background outside it, per frame.
  - A sweep of M from the read-back core, on the CPU. The GPU alpha must equal the CPU dilation at the default
    margin.
  - Judged: not one core or alpha pixel on the pans, the object, static, cut and abandon; the layer's rgb is the
    frame; its alpha is at least the strict mask; the core's precision; recall floors; smear-set coverage.
  - GPU time with the layer off and on.
- **`tests/fg-synth-policy`** runs the header on the CPU over fixtures the harness has no room for:
  - a glyph and a 1 px frame over a pan, seeded and grown;
  - sand beside a swaying coat, and a concave gap between legs;
  - a straight edge along the motion;
  - a still scene with rain;
  - a moving patch over a still scene.

### Built and measured (2026-09-29)

**Code.**
- `precompile/static_overlay_layer.h`: the rule, shared by the shader and the host test.
- `synth_overlay_detect.hlsl`: the `SYNTH_OVERLAY_LAYER` permutation, `SynthOverlay_DetectLayer`. The plain
  permutation compiles to the committed `SynthOverlay_Detect.cso`, byte for byte, checked by recompiling.
- `synth_overlay_layer.hlsl`: the band.
- `SynthMotion::Overlay_Dx12`: `Record(..., layer)`, `RecordLayer(..., margin)`, `LayerCore()`.
- `SynthInputs`: the layer's mask only when a layer is planned; the margin read every base frame.
- `[FrameGen] SynthesizedHudMargin`, 0 to 24, default 16, with a menu slider under "HUD layer".
- `static_overlay_rule.h` and every DLSS-NR file are untouched, so NR's bytecode is the same by construction.

**What the build changed, and why.** Each change came from a measurement.
- **One orientation per pixel.** Each diagonal tested on its own gave a straight stripe running along a pan both
  diagonals, because beside a straight edge the diagonal neighbours lie in whatever texture borders it. That made
  the stripe "perpendicular" structure. Found by the host fixture before the first GPU run.
- **Still against an anchor, not the frame before.** The first GPU run seeded scenery on the harness's 1 px pans,
  26,000 to 63,000 core pixels a frame, and alpha over 87% of the frame. Its brick mortar runs along the pan and
  changes by less than 0.012 a frame, so a frame-to-frame count let it through, and growth then carried the core
  through the rest of the slowly drifting texture.
- **Orientation only against steady neighbours.** The cut's darker content, panning 4 px a frame, still seeded.
  Smooth dark brick stays within 0.012 of its anchor while a mortar joint slides up beside it, and the edge
  between them read as vertical structure. The first fix counted a moving neighbour as the pixel's own value,
  which gave that direction a spurious "along"; a moving neighbour now gives no evidence either way.
- **Three sides of four.** With four, the panel was 60% covered on the GPU. A 208 px panel is wider than the
  120 px reach at 720p, so only its middle tiles saw motion both left and right. The host emulation had shown
  88%, because a tile with exactly 32 of 64 pixels moving rounded to "moving" on the CPU and not on the GPU.
- **Counts in the tile map.** The same knife edge, fixed at its source: counts are exact in 8 bits, and a share
  is compared as count >= share * pixels.
- **A tile moves when half its pixels do, not a quarter.** Sparse rain changes about a fifth of a still scene's
  pixels.
- **Cost.** Every load comes before any store; the orientation runs only for steady pixels; the layer pass skips
  groups by the core bit. With the final masks the first timing read 0.20 ms (1080p) and 0.47 ms (3440x1440);
  now 0.18 and 0.37.

**Host fixtures** (`tests/fg-synth-policy`, `layer.cpp`), the shader's decisions on the CPU:
- **Over a 3 px pan:** a glyph and a 1 px frame with a drop shadow are taken whole, 502 of 502 pixels, from t=9,
  and nothing of the scene.
- **Nothing is seeded in:**
  - sand beside a swaying coat, a concave gap, a moving patch (still camera);
  - a stripe running along a pan;
  - sparse rain;
  - the harness's texture panning in five directions and speeds, and darkened;
  - 40 more pans on the CPU at 1280x720, not in the suite: ten directions and speeds up to 48 px a frame, each at
    four brightnesses.
- **The fixtures are live:** the coat, the gap, the patch, the stripe and the darkened pan each seed with their
  gate switched off (one side enough and no camera gate; no orientation test; orientation against moving
  neighbours), so they test something. The rain and the other pans are regression guards only.

**GPU harness** (`tests/synth-motion-d3d12`, RTX 4090 under vkd3d-proton, clocks locked at 2100/10501 MHz). The
`hud_*` sequences now carry a 1 px hollow frame with a drop shadow and a translucent panel ("glass": the scene at
45%, an opaque 1 px border, outlined text) besides the crosshair, text and panel. Recall is the mean over t=30-39.

| element, part | strict mask = old layer, `+8x` / `(3, 2)` | new core, both |
|---|---|---|
| crosshair stroke / outline | 35.6 / 20.6%, 29.4 / 16.4% | 100%, 100% |
| text stroke / outline | 41.4 / 34.5%, 22.8 / 16.8% | 100%, 100% |
| panel border and glyphs / fill | 0%, 0% | 100%, 100% |
| 1 px frame line / shadow | 0%, 0% | 100%, 100% |
| glass border and text / outlines | 0.7 / 0%, 0.5 / 0% | 87%, 100% |
| glass translucent fill | 0% | 0.1% (the band covers 64% of it) |

- **No scenery in the layer:** not one core or alpha pixel on the eight pans, the object, static, cut and abandon.
  The core's precision on `hud_*` is 100.00% within 1 px of the overlay, with 23 scenery pixels over 30 frames of
  `+8x`.
- **The strict mask and depth are unchanged:** the same recall to the decimal as before on `hud_*` and
  `overlay_*`, and still 100.00% precise.
- **Exact:** the layer's rgb is the frame's, and its alpha is the CPU dilation of the read-back core at margin 16.
  Off by 0 on every pixel of every frame.
- **The smear set** (background whose half-pan sample lands on the overlay), old layer against new at margin 16:

  | | old | new | beside crosshair, text, frame, panel | beside the glass |
  |---|---|---|---|---|
  | `+8x` (4 px each side) | 0% | 100% | 100% | 100% |
  | `(3, 2)` (1-2 px) | 0% | 91.6% | 100% | 67.1% |

  The glass's 1 px border has no still neighbour to contrast with: outside is the scene and inside its own fill,
  and both move. So it seeds only within the orientation test's reach of the glass's text, and grows 48 px along
  itself from there. Its bare bottom edge stays out, and the diagonal pan's smear lands on it.
- **The band and its cost, by margin** (`+8x`, from the read-back core; "held" is alpha on background outside the
  smear set, per 1280x720 frame, of 921,600 pixels):

  | margin | smear covered | held px | rings 1-2 / 3-4 / 5-8 / 9-16 px |
  |---|---|---|---|
  | 0 | 0% | 0 | 0 / 0 / 0 / 0% |
  | 4 | 78.0% | 4,224 | 88 / 43 / 0 / 0% |
  | 8 | 100% | 9,766 | 88 / 87 / 43 / 0% |
  | 16 | 100% | 23,035 | 89 / 88 / 88 / 43% |
  | 24 | 100% | 37,630 | 91 / 89 / 89 / 81% |

  The smear set is covered fully once the margin reaches the motion per base frame (8 px here), as designed.
  Beyond that a margin only adds held background, about 1,400 px per px of margin for this HUD.
  **The default, 16,** covers the smear fully up to 16 px of motion per base frame and partly to 32. That is a
  slow-to-moderate turn: a 90 degree per second turn at 3440 wide and 50 fps base moves about 60 px a frame. The
  speed that matters in Generation Zero has not been measured. The rings stop at about 90% because the glass's
  bare border has no core to grow a band from.
- **GPU time**, medians:

  | | 1920x1080 | 3440x1440 |
  |---|---|---|
  | before: mask + old layer (always both in the harness) | 0.097 ms | 0.219 ms |
  | mask alone (`SynthesizedHudLayer` off, the default) | 0.080 ms | 0.174 ms |
  | mask with the layer's own + layer pass | 0.147 + 0.030 = 0.176 ms | 0.313 + 0.056 = 0.369 ms |

  So the layer now costs 0.10 ms more at 1080p, and 0.19 ms more at 3440x1440, than the mask alone.
  - The mask alone is the same bytecode as before.
  - The estimator is 0.33 and 0.64 ms in the same run.
  - On an RTX 3060, about a third of this card's shader throughput, expect about 0.5 and 1.1 ms for the mask with
    the layer (*estimate*).
- **Memory,** only with the layer, at 3440x1440: the state pair 40 MB, the core 5 MB, the tiles 0.6 MB.

**Not measured:** anything FSR does with it, and any game.

### What needs a game (the layer's own mask)

Generation Zero on the lead's PC, FSR-FG, `SynthesizedMotion=true`, `SynthesizedHudDepth=true`,
`SynthesizedHudLayer=true`, then Rafael's RTX 3060 on native Windows.
- **Camera turns:** the HUD itself (crosshair, compass, text, bars, outlines and shadows) should no longer smear,
  and the band beside it should no longer carry ghosts of it.
- **The judder ring.** Margins 0, 8, 16 and 24 from the menu, on the same turn: how wide a ring moves at the base
  rate around each element, against how much ghost is left. That picks the default.
- **False holds.** Look for scenery that judders while the camera moves and nothing is near the HUD. The design's
  candidates: a far object between near moving ones while walking, and a weapon or cockpit static on screen.
- **Still camera:** nothing should change; the camera gate keeps the layer to the strict mask.
- **Log, once each:**
  - `synthesized FG HUD mask: the UI layer's own mask allocated, WxH (TWxTH tiles)`;
  - `UI layer WxH, format N`;
  - `the HUD layer ... is FFX's UI resource`.
- **Cost:** the pass next to the model, on the 3060.
- **Native Windows:** the new passes use group-shared memory, group-shared atomics and RGBA8 UAV stores. Proton
  accepted them; the 3060 run is what says the D3D12 rules were read right.

## DLSS-G output

Written 2026-09-28, before the code, on branch `synth-fg-dlssg`, against `02c5d0f6`. Until now the input fed
FSR-FG alone:
- `CheckForFGStatus` refused every other output for it and switched FG off (`hooks/FG_Hooks.cpp:184-195`).
- The bridge was not built for another output (`Dx11wDx12::WantedForFrameGeneration`,
  `with_dx12/dx11_with_dx12_sc.cpp:31-34`).

This lets `FGOutput=dlssg` take the same input on both transports. XeFG and Reprojection stay refused: nobody
has read what XeFG does with this input, and Reprojection needs a hudless frame this input never has.

Why bother, when FSR-FG already works: on an RTX 40 or newer, DLSS-G's interpolation is NVIDIA's own model,
with multi-frame generation on an RTX 50. Whether it does better than FSR-FG from zero or synthesized vectors
and flat depth is exactly what nobody knows yet; this makes the comparison possible on one install.

Sources: `framegen/dlssg/DLSSG_Dx12.cpp` as it stands, and the Streamline headers the tree builds against,
`external/streamline/`.

### What carries over

The input calls the same `IFGFeature_Dx12` methods whichever output is behind them (`SynthInputs::Feed`), so
the question is only what DLSS-G does with each.

- **The motion field needs no conversion.**
  - `SetMVScale(1, 1)` becomes Streamline's `mvecScale` of (1/W, 1/H), with W and H the field's size
    (`DLSSG_Dx12.cpp:535-536`). That is the scale that normalises pixel vectors to the [-1, 1] range
    Streamline expects (`sl_consts.h:197`).
  - No sign change. DLSS's vectors point from the current frame to the previous one, as FSR's do. The
    upscaler input hands DLSS-G the same vectors it hands FSR-FG, through the same line, and that route is
    upstream's, in use.
  - So the estimator's contract holds as it is: R16G16_FLOAT, display pixels, current to previous. With
    `SynthesizedMotion` off the field is zero, and none of this matters.
- **Depth.** The constant zero, or with Fix A the HUD mask's 0 and 1. `InvertedDepth` becomes
  `depthInverted = eTrue` (`:557`), so zero is still the far plane.
- **Camera.** `SetCameraValues` gets the config's near and far, swapped for inverted depth, and the 60°
  fallback.
  - With no camera position, `Dispatch` builds a projection from FOV and aspect, and sets `clipToPrevClip`
    and `prevClipToClip` to identity (`:474-515`), with `cameraMotionIncluded = eTrue`.
  - That is what upstream's OptiFG hands DLSS-G for a game that is not Streamline's. All of the motion then
    rests on the vectors, which is this input's premise anyway.
- **Tags.** The pair goes in as `UntilPresent` with no command list.
  - `SetResource` tags it `eValidUntilPresent` with that null list (`:1127-1151`). Streamline allows a null
    list when every tag has that lifecycle (`sl_core_api.h:145`).
  - The contract is that the resource does not change from the tag until the frame is presented
    (`sl_core_types.h:376`). It holds, read from code. The pair is written only on lists that run before the
    present: the bridge's copy list, or on native D3D12 the per-frame list executed on FG's queue before
    `fg->Present()`. The next base frame's writes are recorded after this present returns, on the same queue.
- **Reset** becomes `sl::Constants::reset` (`:552-555`), unless `[FrameGen] SkipReset`.
- **Activation.** `DLSSG_Dx12::EvaluateState` activates only for `FGInput::Upscaler` (`:640`). It gets the
  same `Synthesized` case FSR-FG's has (`framegen/ffx/FSRFG_Dx12.cpp:1559-1561`).
- **NR once per base frame, unchanged.** The stand-down at the top of `RunPresentPass` keys on the input and
  on the bridge or an FG swapchain existing, not on the output (`shaders/dlssnr/DlssNr_Dx12.cpp:4478-4483`).
  If DLSS-G's inner swapchain reaches the wrapped entry, as FFX's does, it stands down the same way.
- **Pacing and latency** are DLSS-G's own, as for OptiFG on a game that is not Streamline's. When the game
  sends no Reflex markers, `FGPresent` sets the PCL present markers and calls Reflex's sleep itself
  (`hooks/FG_Hooks.cpp:1349-1362`, `:1470-1479`). Multi-frame generation follows `[DLSSG]
  InterpolationCount`: the count is the output's, not the input's. Not tried with this input.

### What does not

- **The HUD layer (Fix B).**
  - FFX composes a registered UI resource over every frame it presents. DLSS-G tags a UI resource as
    `kBufferTypeUIColorAndAlpha` (`DLSSG_Dx12.cpp:1115-1116`), but it recomposes the interface only with
    `DLSSGOptions::enableUserInterfaceRecomposition` (`sl_dlss_g.h:114-116`), which OptiScaler never sets.
  - Even then it interpolates a HUD-less colour and the UI separately. This input has no HUD-less colour.
  - So with DLSS-G the layer is neither planned nor recorded: `PlanSynthHud` gets a `layerComposed`
    argument, true only for FSR-FG. The menu disables the checkbox under any other output.
  - Fix A still goes in, because it is only the depth DLSS-G is given. What DLSS-G makes of it is not
    measured.
- **`FGDrawUIOverFG`** draws `UIColor` into the backbuffer before DLSS-G's present (`DLSSG_Dx12::Present`,
  `:832`). It is skipped for this input, as FSR-FG skips it (`FSRFG_Dx12.cpp:1960-1962`). With no layer fed
  it would only warn `UI resource is nullptr` on every frame.
- **`SetInterpolationRect`** is ignored: DLSS-G interpolates the whole swapchain.
- **Reset, unmeasured.**
  - On a reset FSR-FG copies the back buffer instead of interpolating, and the fast-motion response and the
    coherence rule are built on that.
  - Streamline documents `reset` only as "previous frame has no connection to the current one"
    (`sl_consts.h:228`). What DLSS-G shows on such a frame has not been seen.
  - So under DLSS-G, `SynthesizedFastMotion` and `SynthesizedIncoherence` send it, but whether they repeat
    frames there is unverified. So is the first frame of a size and a scene cut.
- **The low-fps floor.**
  - Below `SynthesizedMinFps` the input stops feeding. After three presents `DLSSG_Dx12::Present`
    deactivates (`:929-935`), which sends `eOff` (`:246-251`); feeding again sends `eOn`.
  - Without `DLSSGFlags::eRetainResourcesWhenOff` (`sl_dlss_g.h:48`), DLSS-G may drop its resources on every
    such transition and rebuild them. That is a stall, not a fault. See the follow-ups below.

### Requirements

- **Hardware.** Real DLSS-G needs an RTX 40 or newer. This workstation's 4090 has it. Rafael's RTX 3060
  does not, and there the attempt ends as the Refusal bullet says.
- **Files.** A `streamline` folder in OptiScaler's own folder: `<game>/OptiScaler/streamline/`, or
  `<game>/streamline/` without that subfolder (`Config::MainDllPath`, `proxies/Streamline_Proxy.h:82-83`).
  - It holds `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll` and
    `nvngx_dlssg.dll`.
  - We ship none of them: NVIDIA's licence. The user supplies them, as for the upscaler input with DLSS-G.
  - Without the folder, `CheckForFGStatus` switches FG off with a toast (`FG_Hooks.cpp:220-230`).
- **`FGNvngxReplacement=None`.**
  - The key defaults to Nukem's (`Config.h:881`), and `auto` keeps that for the DLSS-G output
    (`dllmain.cpp:1913-1919`).
  - Nukem's replaces `nvngx_dlssg` with FSR3 behind the same Streamline calls. This input through that
    replacement is neither designed nor tested here: on a card without DLSS-G, `FGOutput=fsrfg` is the
    route.
- **Refusal.** If Streamline says DLSS-G is not supported while the replacement is `None`,
  `InitWithD3D12` does not fail quietly (`Streamline_Proxy.h:494-507`):
  - it writes `FGNvngxReplacement=Nukems` into the ini;
  - it shows a "No DLSSG Support" message box;
  - and it exits the game with `exit(1)`.

  That is upstream's behaviour for the upscaler input too. For this input it means one aborted launch on an
  RTX 30, and an ini changed behind the user's back. Worth knowing before handing Rafael a build.

### On the D3D11 bridge

- **The Witcher 3's quirk matches the DX11 executable.**
  - `CreateSLOnThe2ndDevice` is keyed on `witcher3.exe` (`misc/Quirks.h:398`). The DX12 executable in
    `bin/x64_dx12/` and the DX11 one in `bin/x64/` share that name.
  - Under the quirk, `InitWithD3D12` marks Streamline ready without setting its device
    (`Streamline_Proxy.h:521-526`). The device is set when a second D3D12 device is created
    (`hooks/D3D12_Hooks.cpp:1544-1558`); that is how the DX12 executable skips its throwaway first device.
  - On the bridge the bridge's device is the only one. So Streamline never gets a device.
  - Since `85bef821` the DLSS-G entry points are bound only when the device is set (`SetD3DDeviceAndBind`).
    They stay null, and `CreateSwapchainInternal` calls `DLSSGGetState` through null at once (`:109`, `:196`).
  - Worse, any later D3D12 device would be handed to Streamline in place of the bridge's.
- **The fix:**
  - Neither site applies the quirk when the game's own device is D3D11. The signal is
    `State::currentD3D11Device`: every bridge creation site records it before `WithDx12` creates the D3D12
    device (`hooks/DxgiFactory_Hooks.cpp:560`, `:962`; `DxgiFactory_WrappedCalls.cpp:234`, `:627`).
  - Only a swapchain made on a D3D11 device sets it, so the DX12 executable, which presents with D3D12, keeps
    the quirk.
  - Independently, the DLSS-G swapchain creation refuses when Streamline bound no DLSS-G entry point, before
    a swapchain exists. A failed bind then leaves the bridge on its plain presenter, or a D3D12 game on its
    own swapchain, instead of faulting. On the quirk's own title that path already faulted; it now refuses.
- **Divinity's prefix has no `nvapi64.dll`** (CLAUDE.md, "Titles with no upscaler"). Streamline's DLSS-G and
  Reflex go through NVAPI, and fakenvapi is what OptiScaler substitutes. `slInit` or the support check may
  then fail, and a failed support check takes the Refusal path above. The same `[Libraries] NvapiPath` fix
  NR needs there is the first thing to set. Read from code, not tried.

### Follow-ups, not done here

- `DLSSGFlags::eRetainResourcesWhenOff` while the low-fps floor holds FG off, so resuming does not rebuild
  DLSS-G's resources.
- `motionVectorsDilated = eFalse` for this input. `Dispatch` calls any display-size field dilated
  (`:562`), which the synthesized field is not: it is a block field, bilinear between 8x8 block centres.
- A HUD-less synthetic frame, which would give DLSS-G's UI recomposition something to work with.
- XeFG as an output for this input.

### To verify in-game

On this workstation's 4090, `FGInput=synthesized`, `FGOutput=dlssg`, `FGNvngxReplacement=None`, the
`streamline` folder in place.
- The log, once each:
  - `synthesized FG input feeding DLSSG: WxH ...`;
  - `Max supported interpolations: N` from the swapchain creation;
  - no `bound no DLSS-G entry points`, and no `Depth or Velocity is not ready` after the first ten frames;
  - `synthesized FG input: DLSSG now gets the synthesized motion field ...` with `SynthesizedMotion`.
- MangoHud's rate doubling, as in step 1, and a pan compared with `FGOutput=fsrfg` on the same scene.
- `SynthesizedFastMotion` set low, whipping the camera: whether DLSS-G repeats frames on a reset.
- Divinity on the bridge (with `NvapiPath` set), then The Witcher 3 DX11 for the quirk. The start-up list
  still names the quirk (`Quirk: Create SL on the 2nd device`); what must follow it is `Streamline features
  bound through the active interposer`, or its fallback line, before `Max supported interpolations`. Under
  the quirk that line would not appear until a second D3D12 device, which the bridge never creates.
- PCSX2 on D3D12, for NR's once-per-base-frame count under DLSS-G.

### Built (2026-09-28)

On `synth-fg-dlssg`, not yet compiled or run by the author of this section; host tests only.
- **Admission.** `CheckForFGStatus` lets `FGOutput::DLSSG` through for this input, and its toast and log name
  both outputs. `Dx11wDx12::WantedForFrameGeneration` builds the bridge for it.
- **The output.** `DLSSG_Dx12::EvaluateState` activates for `Synthesized`, and `Present` skips the
  `FGDrawUIOverFG` draw for it, both as in FSR-FG.
- **Failing closed.** Both `CreateSwapchain*Internal` refuse when `DLSSGGetState` or `DLSSGSetOptions` is
  null, right after Streamline's initialisation and before the swapchain is made. The check sits there, not
  at the `DLSSGGetState` call itself: skipping only that call would create the swapchain and then fault on
  `DLSSGSetOptions` in the first `Dispatch`.
- **The HUD plan.** `PlanSynthHud(..., layerComposed)`, fed `activeFgOutput == FSRFG` by `HudPlan()`. Under
  DLSS-G the layer is never planned, recorded or fed, and a layer asked for alone records no mask. The menu
  disables "HUD layer" under any output but FSR FG. The log lines that named FSR-FG name `fg->Name()`.
- **The quirk.** Both `CreateSLOnThe2ndDevice` sites also require `currentD3D11Device == nullptr`, one
  condition each. A DX12 title that made a D3D11 swapchain before its first D3D12 device would lose the quirk
  too; The Witcher 3's DX12 executable is not known to, and nobody has checked.
- **Tests.** `bridge-lifetime`: the synthesized input with DLSSG builds the FG bridge, with the pass when NR is
  at present and an idle host when it is off there, and Reprojection is refused. Restoring the old predicate
  fails them. `fg-synth-policy`: the layer only with a composing output, near depth the same under either, over
  all 32 combinations.
- **Text.** The menu's input tooltip and three help texts, their pt-BR entries, the `[FrameGen]` comments in
  `OptiScaler.ini`, and `Config.md`.

### First run (2026-09-29): a resize froze the picture

Generation Zero on this workstation's 4090, build `8009f920`, `FGInput=synthesized`, `FGOutput=dlssg`, NR at
present, Streamline 2.14.1 from `<game>/OptiScaler/streamline/`, `LogLevel=1`. The log is kept at
`~/.local/state/dlss5/deployments/local-gz-logs-20260929/gz-8009f920-dlssg-0749.log`.

**It worked until the second resize.**
- `07:49:29`: DLSS-G's swapchain is created (`buffers 2`, `Max supported interpolations: 1`). The game's
  `SetFullscreenState(TRUE)` is emulated.
- `07:49:30`: `DLSS-G interpolation state changed from disabled to enabled`. About 125 fps base by the user's
  reading, Velocity and Depth tags `eOk`, `Dispatch Ok` on every frame.
- `07:49:39`: focus lost. `DLSS-G disabled: window not focused`, the game leaves fullscreen and resizes to
  3440x1418. `fg 0, real 0`: this resize succeeded.
- `07:49:47.012`: focus back, and interpolation is enabled again on the next present. At `.013` the game asks
  for fullscreen, and at `.020` it resizes to 3440x1440.
- `07:49:47.094`: `WrappedIDXGISwapChain4::ResizeBuffers ... result: 887A0001`, inside Streamline's resize, so
  `fg 887A0001, real 887A0001`. Streamline adds `PFunResizeBuffersBefore failed Invalid call`.
- From `07:49:47.347`, on every frame: `slHookGetBuffer: proxyBuffer is NULL!`, and the bridge logs
  `Present frame 595` 979 times. No `LocalPresent` line appears after `07:49:47.037`: Streamline's present
  returned S_OK and never presented again. The picture was frozen until the game exited at `07:50:00`.

**Cause.** A reference to the real swapchain's buffers was still held when DLSS-G resized it. vkd3d-proton refuses
`ResizeBuffers` while any buffer holds a public reference (`dxgi_vk_swap_chain_ChangeProperties`, "Public
ref-counts must be 0"). The holder was the D3D12 overlay's render targets. The bridge's resize cleaned the
overlay from the game thread, while Streamline's present thread was drawing it:
- `Dx11wDx12SC::ResizeBuffers` called `MenuOverlayDx::CleanupRenderTarget(true, ...)` after its drain and
  before the presenter's `ResizeBuffers` (`with_dx12/dx11_with_dx12_sc.cpp:1107` at `8009f920`; the same in
  `ResizeBuffers1`, `:1390`, and `_ResizePresenterToMatch`, `:969`).
- With DLSS-G interpolating, Streamline presents from a thread of its own. Each present goes through the
  wrapped real swapchain's `LocalPresent`, and so through `MenuOverlayDx::Present`
  (`wrapped/wrapped_swapchain.cpp:515`). Nothing in the menu overlay serialises the two threads.
- The log places the overlap exactly. Frames 593 and 594 were presented at `07:49:47.037`, between the bridge
  resize at `.020` and FG's resize hook at `.041`. Frame 593 drew with the render targets in place. Frame 594
  found them gone, and logged `CreateRenderTargetDx12 done!` at `.037925`, which takes a reference on both
  buffers again.
- Frame 594 also found the overlay still initialised. There is no `D3D12CommandQueue captured` and no
  `BackendRendererUserData == nullptr` before it, although a full cleanup is always followed by both, as at
  `07:49:39.418`. So the cleanup had released the render targets and had not yet cleared `_isInited` or shut
  ImGui down: it was inside `ImGui_ImplDX12_Shutdown`.
- The cleanup then cleared `_isInited` over the new references. The later cleanup in
  `WrappedIDXGISwapChain4::ResizeBuffers` (`wrapped_swapchain.cpp:917`) is the one that would have released
  them. It returned first because of that same state (`menu/menu_overlay_dx.cpp:117`, the old guard). So the
  real resize was refused.
- DLSS-G's resize hook had already dropped its proxy buffers, and rebuilds them only on a resize that
  succeeds. Its Present then does nothing and still returns S_OK, so the bridge's Present recovery, which reacts
  to 887A0001 from Present, never fired.

**Why DLSS-G and not FSR-FG, and not the first resize.** At `07:49:39` DLSS-G had turned itself off for the lost
focus. Streamline then presents on the calling thread, and nothing ran concurrently with the cleanup. At
`07:49:47` interpolation had been re-enabled 8 ms before the resize, and two queued presents landed inside it.
FSR-FG's pacing thread can race the same cleanup in principle, and nothing here proves it cannot. That it never
has may be timing: the bridge's drain waits on the present queue that DLSS-G's present thread also waits on,
which may line the two up. That last point is inferred, not measured.

**Fix** (branch `synth-fg-dlssg-resize`):
1. **The bridge no longer cleans the overlay of a frame generation presenter** (`_ReleaseOverlayForResize`).
   - That overlay belongs to the real swapchain inside the backend. `WrappedIDXGISwapChain4::ResizeBuffers`
     releases it inside the backend's resize, after Streamline's `flushAll`, which is where upstream's native
     D3D12 path releases it.
   - Frame generation is still paused there, as the old cleanup did.
   - The plain presenter's overlay is still cleaned by the bridge, because the bridge draws it on its own
     thread.
2. **`CleanupRenderTargetDx12` releases the buffers whatever the init state says** (`menu_overlay_dx.cpp`,
   upstream-owned; the guard moved below the release loop). A race that re-creates them can no longer turn into
   a permanent reference. The same move also stops a handle change from keeping render targets of the old
   swapchain.
3. **A refused resize is retried, and remembered** (`_ResizePresenter`, `_RetryRefusedPresenterResize`).
   - A presenter `ResizeBuffers` refused with 887A0001 is tried once more at once: the backend's own cleanup
     inside the first attempt clears a holder that only lost a race.
   - Any failure leaves the resize owed, so the game's next resize reaches the presenter even at the size it
     already has.
   - Any failure also arms three resizes of the presenter alone at its own size, one per present. The hidden
     swapchain still matches that size, because a refused presenter leaves it untouched.
   - One error line is logged when the attempts are spent.
   - Falling back to a plain presenter when every attempt fails was not built. It would mean replacing a
     swapchain the game holds in flight.

Tests are in `bridge-lifetime`: `refusedResizeCases`, and the overlay unit that replays this interleaving
against the production `CreateRenderTargetDx12` and `CleanupRenderTargetDx12`. Eight mutations each fail a case;
see that suite's README.

**The `RSYNC` errors.** Every frame logs `rsync.cpp:660 setDynamicMFGParams failed with status 1`: 370 of them in
this log. `NvAPI_D3D_SetReflexSync ... failed error -3` appears twice, at the first present and at a flush. That
error is `NVAPI_NO_IMPLEMENTATION`: dxvk-nvapi does not implement `SetReflexSync`. Streamline's pacing calls it
by itself, and there is no `DLSSGOptions` field or flag to turn it off (`external/streamline/sl_dlss_g.h`).
Interpolation and pacing worked with it failing, so it is log noise. OptiScaler cannot avoid triggering it
without changing what it asks of DLSS-G on Windows, where the call succeeds. The only lever is cosmetic: demote
or rate-limit those two lines in the Streamline log callback when running on Wine
(`proxies/Streamline_Proxy.h:684-692`). That was not done here.

**To verify in-game**, same setup:
- Alt+Tab out and back while DLSS-G is interpolating, several times. Then toggle fullscreen from the game's
  options.
- Wanted in the log: every `Dx11wDx12SC ResizeBuffers results: fg 0, real 0`; no `proxyBuffer is NULL`; and
  `LocalPresent` continuing after each resize.
- `the presenter refused ResizeBuffers ... Trying once more` may appear. It is acceptable only when it is
  followed by `fg 0`.
- `refused ... more resizes at its own size` means the retries were spent. Keep that log.
- The menu (Home) must still draw after each resize. On a frame generation presenter its render targets are now
  released only inside the backend's resize.

## Open questions

- How FSR's interpolator behaves with flat depth: its disocclusion and inpainting assume real depth.
  Step 1 shows it. Read from its code on 2026-09-28: flat depth disables disocclusion, see
  [The HUD: near depth and a UI layer](#the-hud-near-depth-and-a-ui-layer).
- The units and sign FSR expects from our field, pinned by capture, not asserted. A flipped sign
  reads as "synthesis made it worse".
- Whether 8x8 blocks are fine enough for text-heavy scenes, or the HUD mask has to carry them.
- Latency. Interpolation holds one base frame. Divinity's turn-based pace tolerates that; a
  first-person game at 30 fps base would not, and the help text should say so.
