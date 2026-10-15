# Native D3D11 DLSS and Neural Rendering: getting the game's guides and image across the bridge

Status: steps 1 and 2 of the Recommendation implemented on 2026-10-13, and option (c) as step 3 on 2026-10-15 (see
"Implemented" below); the rest is design for review. Written against `93730d39`. Line numbers are into that tree.
Route (b) has run in a game (Sons Of The Forest, September; step 3 below); nothing built here has, and the cost figures
are arithmetic from stated formats, not measurements.

Read first: [nr-dx11-bridge-host.md](nr-dx11-bridge-host.md) (the swapchain bridge and its hazards),
[nr-without-game-dlss.md](nr-without-game-dlss.md), [nr-before-upscale.md](nr-before-upscale.md) (the
Stage 1 scope the bridge already carries), [present-real-guides.md](present-real-guides.md) (the
identity layer for the present-time ring, which option (c) below depends on), and
[DEVELOPMENT.md](DEVELOPMENT.md).

## The question, and a correction to its premise

A D3D11 game that calls DLSS itself (Baldur's Gate 3 in D3D11 mode and Satisfactory are the titles
NeuRotic's review names) should get Neural Rendering with the game's own depth and motion vectors.
The premise as it reached this note: today such a game gets NR only if the user picks one of
OptiScaler's "w/Dx12" upscalers, or the bridge present host with zero guides.

That is right, with one thing the premise leaves out: **the w/Dx12 option that matters here already
exists.** `Upscaler::DLSS_on12` (`dlss_12` in the ini) was added upstream by FakeMichau on 2026-10-01
(`435609d4`, "Add DLSS Dx11 w/Dx12 and DLSS Vk w/Dx12") and is in this tree. `OptiScaler.ini` lists it
as "dlss_12 (dx11on12, the only option that can run DLSS Neural Rendering as well)". It runs the game's
D3D11 DLSS evaluate on a D3D12 DLSS through the bridge, and the bridge calls the NR pass with the
game's D3D12-side copies of its inputs (below). So the question is not "is there a route", it is
"is that route good enough, or is a second one worth building".

Nothing in this tree's notes records a run of it. The README says a D3D11 title with an upscaler "gets
the pass when OptiScaler runs that upscaler through the bridge", which is a statement about the code,
and `IFeature_Dx11wDx12.cpp` logs "the D3D11 bridge reached the hand-off" once. No measurement note
exists for the upscaler-side bridge with NR, on Proton or on Windows. The swapchain-side bridge (the
no-upscaler host) was measured in Divinity on 2026-09-18 ([nr-dx11-bridge-host.md](nr-dx11-bridge-host.md),
"Measured"), and that is a different code path (see below).

## How a D3D11 DLSS call reaches OptiScaler today, and why NR does not run on it

The game calls `NVSDK_NGX_D3D11_CreateFeature` and `_EvaluateFeature`, which OptiScaler exports
(`inputs/NVNGX_DLSS_Dx11.cpp`).

1. **Which upscaler is created** is decided in `NVSDK_NGX_D3D11_CreateFeature` (`:592-600`): FSR 2.2
    by default; DLSS (native D3D11 pass-through) when the GPU is DLSS-capable and the original NGX
    loaded; and whatever `Dx11Upscaler` says if it is set. On a DLSS-capable machine with the key
    unset, a D3D11 game that calls DLSS therefore gets **native DLSS**, which is the case this note is
    about. (The ini text says "Default (auto) is fsr22"; the code is what is described here.)
    `FeatureProvider_Dx11::GetFeature` (`upscalers/FeatureProvider_Dx11.cpp:59-83`) builds
    `DLSSFeatureDx11` for `dlss` and `DLSSFeatureDx11On12` for `dlss_12`.
2. **Native DLSS is a straight pass-through.** `DLSSFeatureDx11::EvaluateInternal`
   (`upscalers/dlss/DLSSFeature_Dx11.cpp:73-103`) calls the original `D3D11_EvaluateFeature` on the
   game's context with the game's parameter block, and returns. It never forms a D3D12 resource, a
   command list or a queue, so nothing on this path can host the NR pass.
3. **No NR call exists on the D3D11 evaluate path.** The NR hooks outside the module are listed in
   `retoque/dlssnr/README.md`; for D3D11 there is exactly one, in `IFeature_Dx11wDx12::Evaluate`
   (`upscalers/IFeature_Dx11wDx12.cpp:495`), and `DLSSFeature_Dx11.cpp` and `NVNGX_DLSS_Dx11.cpp`
   carry no reference to `DlssNr` (checked with a grep of `inputs/` and `upscalers/dlss/`). I found no
   log line that says NR is not running in this configuration. **With NR enabled and native D3D11
   DLSS, the pass is silently inert**, which is the shape of the failure `DEVELOPMENT.md` §1.2 ("no
   dead control") is written against, and is what a user who enabled NR in BG3 would see.
4. **Streamline on D3D11 is not intercepted at all.** `hkslSetTag` and `hkslSetTagForFrame` log
   "only supports DX12" and call through for D3D11 and Vulkan
   (`hooks/Streamline_Hooks.cpp:498`, `:585`). A D3D11 game that uses Streamline for DLSS (rather than
   NGX directly) may never reach `NVSDK_NGX_D3D11_*` at the points above. Which of the review's two
   titles call NGX directly is not known to this note, and it decides whether this whole topic
   reaches them (open question 1).

### What `dlss_12` does, concretely

`IFeature_Dx11wDx12::Evaluate` (`:315`) for a D3D11 evaluate:

1. `ProcessDx11Textures` (`:441`) calls `Dx11WithDx12::PrepareUpscalerResources` for colour, motion,
   depth, output, and exposure and reactive when present. Each is a D3D11 texture copied into a
   shared texture that the bridge's D3D12 device has opened. The shared handles are **legacy** shared
   handles by default (`DontUseNTShared`, `Config.h:879`, default true), NT handles when false.
   Typeless depth in `R24G8` and `R32G8X24` goes through `DepthTransfer_Dx11` to a shareable buffer
   first (`with_dx12/dx11_with_dx12.cpp:504-560`).
2. A D3D11-to-D3D12 sync (`SyncDx11ToDx12`, `:212`): the D3D11 context signals a shared fence and the
   bridge's D3D12 queue waits on it, on the GPU.
3. The game's parameter block is rewritten to carry the D3D12 resources (`:454-`), the D3D12 DLSS is
   evaluated on a bridge list (`dx12Feature->Evaluate`, inside `ScopedPreUpscale` so Stage 1 applies,
   `:475`), and `DlssNr::EvaluateAfterUpscale(cmdList, InParameters, Dx12CommandQueue, ...)` runs on
   the same list (`:495`). Depth and motion come out of the same block as the game's own, so the NR
   pass sees the real guides, and the present-time ring captures them too when `HookMethod=2`.
4. The list is executed, a D3D12 fence signalled, and `SyncDx12ToDx11` (`:251`) waits for it. On Linux
   with a bridge swapchain that wait is on the CPU, because a GPU-side D3D11 wait on D3D12 can deadlock
   when vkd3d-proton maps several D3D12 queues onto one `VkQueue` (`:243-249`); otherwise it is a GPU
   wait on the D3D11 context. Then `CopyBackOutput` (`:558`, `CopyUpscalerOutputToDx11`, `:995`)
   copies the shared output into the game's D3D11 output texture.
5. The parameter block is restored (`:505-`), so the game's own pointers are back.

Two facts about this that bear on the options: the **D3D12 DLSS writes straight into the shared
output**, so the output crosses the bridge once (back to D3D11), not twice; and everything the D3D12
DLSS needs, colour included, crosses the bridge on the way in.

## What NeuRotic does, as prior art

NeuRotic (the clone reviewed on 2026-10-13, tip `7132a103`, GPL-3.0 like this tree; nothing was built
or run and nothing is copied) keeps the game's native D3D11 DLSS and runs NR beside it. Commits
`52eb7fef`, `6b6daaff`, `02f31c4e`, `ca08af64`, `0307f2af`, `d0adf436` (all 2026-09-12); files
`dlssnr/DlssNr_Dx11.cpp`, `DlssNr_Dx11Transport.h`, `NrNativeDx11OutputContract.h`. In outline:

- **Hook placement.** `DLSSFeatureDx11` calls `Created`, then `Prepare` immediately before the native
  `D3D11_EvaluateFeature` and `Complete` immediately after it (the `52eb7fef` diff against
  `DLSSFeature_Dx11.cpp`). "Native DLSS remains DX11" is in their log line.
- **Transport.** Private shared textures, NT-handle D3D11 textures opened on the bridge's D3D12 device,
  with a shared D3D11 fence (`ready`) that D3D11 signals and the D3D12 queue waits on, and a shared
  D3D12 fence (`completed`) in the other direction. Four slots. Guide transport capped at 1280 MiB,
  output transport at 512 MiB. A slot still in flight when the next evaluate comes is not waited for:
  the frame is refused ("private guide transport slots are still in flight") and NR is skipped for it.
- **Guide conversion on D3D11.** A small compute shader extracts the depth plane to `R32_FLOAT` and
  expands a two-component motion texture to four components, because their comment says two-component
  float formats are not NT-shareable. (Their claim; not verified here. It fits upstream's bridge
  defaulting to legacy shared handles, but under DXVK handle kinds are emulated and only a probe will say.)
- **Placement.** Post-SR: the native output is copied across, NR runs on it, and the result is copied
  back into the game's output. Pre-SR: the render-size colour is copied across, NR runs, and the edit
  is fed to the native DLSS as its input. Both exist; Pre-SR needs a de-jitter/re-jitter step on their
  side.
- **Refusals.** Non-zero output subrect base; output backing smaller than the declared output;
  unsupported output format or shape; a render subrect larger than the output or the colour; non-finite
  jitter or motion scale; `MVJittered`; more than one swapchain on the device; a different adapter or
  queue than the bridge's; a depth format outside a short list.
- **State isolation.** The D3D11 work runs under `SwapDeviceContextState` with a private state, so the
  game's pipeline state is not disturbed.

What is not taken: their runtime-compiled shaders (nothing in this tree compiles HLSL at runtime
without an embedded bytecode header, `CLAUDE.md`), their single-file one-line style, and their
Windows-only synchronisation assumptions. What is taken as a lesson: refuse rather than guess, never
wait on the other API on the GPU for a transport slot, bound the transport memory.

## Options

All four assume the same placement as `HookMethod=1`: NR runs on the DLSS output, before the game
draws its HUD. (c) differs and says so.

| | Route | What crosses | NR sees | New code | Status |
|---|---|---|---|---|---|
| (a) | Native D3D11 DLSS stays; after it, the output and the guides cross to the bridge's D3D12 device, NR runs, the output crosses back | output in and out, depth, motion | game's depth and motion, output | A new driver around `Dx11WithDx12`, one change to its resource mask | not built |
| (b) | `dlss_12`: the D3D11 DLSS evaluate becomes a D3D12 DLSS on the bridge | colour, depth, motion, exposure in; output out | game's depth and motion, output | none | **built, upstream, unmeasured here** |
| (c) | Native D3D11 DLSS stays; only depth and motion cross, into the present-time ring; NR runs at the bridge's present with `HookMethod=2` | depth and motion in | game's depth and motion, final backbuffer incl. HUD | an "adopt external guides" entry on the ring, plus the D3D11 half of (a) for guides only | not built |
| (d) | NR on the D3D11 device directly, through the model's D3D11 entry points | nothing | game's own resources | none, if it worked | **unknown**: the probe exists (`[DlssNr] ProbeD3D11`, `Config.h:422`; off by default and deliberately absent from the shipped ini, `tests/nr-invariants/run.py:58`) and no result is recorded |

### Cost arithmetic

Assumed: 3840x2160 output, 2560x1440 render (DLSS Quality), RGBA16F colour and output (8 B/px), R32
depth (4 B/px), RG16F motion (4 B/px). These are assumed formats, not read from a title.

| | Per frame, decimal MB | Both directions of DRAM traffic | At 60 fps | Share of 1008 GB/s | Share of 360 GB/s |
|---|---|---|---|---|---|
| (b) in: colour 29.5 + depth 14.7 + motion 14.7; out: 66.4 | 125.3 | 250.7 MB | 15.0 GB/s | 1.5% | 4.2% |
| (a) in: output 66.4 + depth 14.7 + motion 14.7; out: 66.4 | 162.2 | 324.4 MB | 19.5 GB/s | 1.9% | 5.4% |
| (c) in: depth 14.7 + motion 14.7 (the colour crossing is the bridge host's, which exists anyway) | 29.5 | 59.0 MB | 3.5 GB/s | 0.35% | 1.0% |

Two things to take from this. First, the intuition that (a) is cheaper than (b) because less crosses
is wrong at 4K: (a) sends the full-resolution output both ways, where (b) lets the D3D12 DLSS write it
once. (a) moves about 30% more than (b). Second, bandwidth is the small term. The cost that matters is
the two synchronisation points per frame and, on Proton, a CPU wait; neither is measured, and neither
differs between (a) and (b) in kind. The shared copies are also resident: for (b) about 280 MB at these
settings (one colour, four motion and four depth sets because `DX11_WITH_DX12_INPUT_FRAMES` is
`BUFFER_COUNT`, which is 4 for frame generation's sake, and two outputs), about 250 MB for (a) by the
same count.

### Why not (a) first

(a) is the route that mirrors the prior art, and it keeps the game's own DLSS call exactly as the game
made it. But it is a new GPU-synchronising path between two APIs, and on the numbers above it is not
cheaper than the route that already exists. The concrete reasons to want it would be a title where (b)
does not work (the D3D12 DLSS wrapper does not mirror something the D3D11 call does), or a user who
must keep the native D3D11 DLSS for a reason that is not performance. Neither is evidenced yet.

### Why (c) is a different product, not a cheaper (a)

At present the HUD is in the frame, and the guides describe the scene without it
([nr-present-hook.md](nr-present-hook.md), the HUD paragraph). (c) trades the quality of (a) and (b)
for the smallest transfer and for sharing the bridge host's existing colour path. It needs the
identity layer of [present-real-guides.md](present-real-guides.md) first, because the ring would be fed
from a second API with a producer fence instead of a command-list ticket, and the proof that a
capture belongs to the frame would have to cover that.

### Why not (d) now

The model's D3D11 entry points exist in the snippet and have never been called by this project
(`DlssNrFeature_Dx12.h:278-284`; the bridge exists because of an untested claim that the model refuses
D3D11). If the probe says the model works on a D3D11 device, (d) beats everything above on cost and
complexity, and none of this note's work is needed. The probe has never been recorded as run. Running
it is the cheapest experiment in this note and has no engineering cost; the caution in its comment
(initialising an NVIDIA subsystem on the game's live device) applies.

## Recommendation

In this order, each step cheap relative to the next, and each its own decision.

1. **Stop the silent inert state. No new route.** When NR is enabled and the D3D11 upscaler is native
   `dlss`, say so: one log line at the first native evaluate, and one read-only line in the NR tab,
   naming the reason and `dlss_12` as the route that carries NR. This is a decision made from facts
   that already exist at the first evaluate, which makes it a pure header (below) with a host suite.
2. **Choose `dlss_12` for the user when NR is on and `Dx11Upscaler` is unset.** In
   `NVSDK_NGX_D3D11_CreateFeature` (`:592-600`), where the default is DLSS when the GPU is capable,
   pick `DLSS_on12` instead when `DlssNrEnabled` is true. An explicit `Dx11Upscaler=dlss` is
   respected, and step 1's line reports it. At `DlssNrEnabled=false` this is byte-identical to today
   (invariant 1); the change is inside a branch NR already gates.
3. **Measure (b) before building anything.** Run `dlss_12` with NR in a D3D11 title that really calls
   NGX, on Proton and on a native Windows machine, and record what it costs against native `dlss`
   with NR off, and what it does to the image. The log already says whether the guides arrived. If it
   works and costs a few percent, stop: (a) has no case.
4. **Run the probe for (d).** Once, on a D3D11 title, `ProbeD3D11=true`, and record the answer in
   [nr-without-game-dlss.md](nr-without-game-dlss.md) either way.
5. **Build (a) only on evidence from steps 3 and 4:** a title where (b) fails or costs too much and
   (d) refuses. If it comes to that, the shape is below.
6. **(c) waits** for the present-guide identity work and for a decision on whether a present-time
   route for D3D11 DLSS titles is wanted at all. (Both came: the identity work is confirmed in Crimson Desert, and the
   roadmap's item 3 is this route. Built on 2026-10-15 as step 3; see "Implemented".)

### If (a) is ever built

A driver around the existing bridge, not a second bridge. The pieces are already in `Dx11WithDx12`:

- **Trigger.** In `DLSSFeatureDx11::EvaluateInternal`, after `D3D11_EvaluateFeature` returns success
  (`:87`), when NR is enabled: one call, in the style of the other NR call sites.
- **Resources.** `PrepareUpscalerResources` with a mask of output, motion and depth. Today the output
  is prepared without a copy-in because the D3D12 upscaler produces it; (a) needs the native output
  copied in. That is one new mask bit and one changed argument in an upstream-owned file
  (`dx11_with_dx12.cpp`, around `:970`), the only edit in a file that every upstream merge touches, and
  it should be written so a merge conflict there is a one-line one.
- **Sync.** `SyncDx11ToDx12` before the D3D12 side reads; `SyncDx12ToDx11` after, with the bridge's
  own CPU-wait-on-Linux rule. No new fence type. No GPU wait by D3D11 on a transport slot: a slot not
  free is a refused frame, not a stall, which is what NeuRotic does and what this tree's own
  Proton findings require.
- **List and queue.** The bridge's own list, allocator ring and queue, with the same completion wait
  `WaitForBridgeFence` gives the upscaler path (`IFeature_Dx11wDx12.cpp:165`).
- **The NR call.** The parameter block is rewritten with the D3D12 output, depth and motion exactly as
  `IFeature_Dx11wDx12::Evaluate` does (`:454-`, and restored at `:505-`), and
  `DlssNr::EvaluateAfterUpscale` is called unchanged. Nothing in the module changes: it takes the
  same inputs it takes from any bridge evaluate.
- **Contract refusals**, in a pure header with its own host suite: output subrect base other than zero,
  output backing smaller than the declared output, an output format outside the formats the pass can
  write (nr-present-hook.md hazard 3: BGRA has no UAV), a render subrect larger than the output,
  non-finite motion scale, and (open question 3) `MVJittered`.
- **Lifetime.** Shared textures are owned by the `Dx11WithDx12` cache and retired by its generation
  counter. Nothing is freed under the GPU: a cache reset waits for the bridge fence, as it does for
  the upscaler path.

## Implemented

**Step 1**, 2026-10-13, on `f3b42cca`. Not run in a game: the title the test plan asks for is still not in our set.

- `retoque/dlssnr/DlssNr_Dx11Route.h` holds the decision (`Decide`), pure and standard library only, as planned.
  It takes NR on/off, the route of the evaluate (native `dlss`, `dlss_12`, or anything else) and `HookMethod`, and
  answers one of two reports: at `HookMethod` 0, 1 or any other value the pass belongs after the upscaler and
  cannot get there; at 2 the game's depth and motion never reach the bridge's present host. GPU capability and NGX
  are not inputs to the report: a native `dlss` feature exists only when both held. The texts name the reason, the
  setting (`Dx11Upscaler dlss`), the menu entry (DLSS w/Dx12, under Upscalers) and `dlss_12`, and deliberately do
  not claim the pass is or is not running, because a present host built at HookMethod 2 keeps running after the
  key changes and the panel's own status line already says whether the pass ran.
- `retoque/dlssnr/DlssNr_Dx11Upscaler.h`, header-only, is what the call site and the panel use.
  `NoteDx11Evaluate(upscaler)` is one line in `NVSDK_NGX_D3D11_EvaluateFeature`, after the evaluate, with the
  feature's own `GetUpscalerType()` (so a backend switched in the menu is seen at its next evaluate). With NR off it
  returns before it stores or logs. Otherwise it keeps the route and its time for the panel and logs each report at
  warning level once per process (`LogOnce`), not once per frame and not again when the route comes back.
- The panel's line is DEVELOPMENT.md §2.2, from `Dx11RouteReport()`. It replaces "Waiting for the upscaler to
  run.", which on this route was waiting for something that was not coming, and in the after-upscaler case it also
  hides the hook status line under Source, whose "upscaled: active on the upscaler's output" said the opposite.
- `tests/nr-dx11-route` covers the decision, the once-only log, the panel's window, the production layer against a
  `Config.h` stub, the call site's position, and the pt-BR entries.

**Step 2**, 2026-10-13, as proposed. A separate commit, so it can be reverted alone if open question 2 is answered
the other way.

- `PreferBridge` in the same header: NR on, `Dx11Upscaler` unset, and the default upstream already chose is native
  DLSS. `Dx11DefaultUpscaler(upscalerChoice)` is one line in `NVSDK_NGX_D3D11_CreateFeature`, after upstream's own
  DLSS default and before the key is applied, so an explicit key still wins by upstream's own line, and GPU
  capability and NGX stay upstream's two conditions rather than inputs copied into the header. The suite mirrors
  those lines with and without the call, and a source guard holds the production order to the mirror.
- The choice is logged once, at info, saying what was chosen and that `Dx11Upscaler=dlss` keeps native DLSS.
  `OptiScaler.ini` (under `[DlssNr] Enabled`) and `Config.md` say it too.
- Found while implementing, not anticipated above. `FeatureProvider_Dx11::GetFeature` writes `Dx11Upscaler` with
  whatever it created, and the key is a `SoftDefault`: after the first feature the key is set for the session, and
  saving from the menu writes it. So NR switched on over a native feature keeps native DLSS until a restart or a
  switch in the Upscalers card (step 1 reports it); NR switched off keeps `dlss_12` until a restart or a switch; and
  a user who saved while NR chose `dlss_12` keeps
  `Dx11Upscaler=dlss_12` with NR off later, as today an auto `dlss` is written. Stopping that would take a second
  upstream line after `GetFeature`, and was not done.
- Unchanged, as the Risks section says: a `dlss_12` that fails to load or to run falls back to FSR 2.2, not to
  native DLSS, with upstream's toast; and since `GetFeature` then writes `fsr22` into the key, the rest of the
  session stays on FSR 2.2. The in-game check for step 2 is therefore that `dlss_12` is what the Upscalers card
  shows after the first frames, not merely what the log line says was chosen.

**Step 3**, 2026-10-15: option (c), the native D3D11 guides at present. Not run in a game. Built on
`feat/dx11-native-guides`, on top of [present-real-guides.md](present-real-guides.md)'s step 1.

Why (c), and why now, rather than the measurements the Recommendation put first:

- **Route (b) already works where it was tried.** Sons Of The Forest (Unity 2022.2, D3D11, `Dx11Upscaler=dlss_12`)
  ran it on 2026-09-06 and 2026-09-14 (`OptiScaler_35615710801.log`, `OptiScaler_48822690467.log`, builds `f034397a`
  and `4cbd0950`): "the D3D11 bridge reached the hand-off", then "DLSS-NR running after upscale: target 3440x1440,
  guides 2294x960", "depth inverted, motion vector scale -2294 x -960", and at the first session's last coverage line
  17,879 of 18,075 evaluates applied. That is the functional half of step 3; its cost against native DLSS is still
  unmeasured, and so is its picture (see the depth-stencil finding below).
- **The identity layer (c) waited for exists and is confirmed** (present-real-guides.md, step 1, Crimson Desert).
- **The roadmap's item 3 is the present route on native D3D11**, which is (c). (a) stays unbuilt for the reason
  above: (b) runs, and (a) moves more.
- Palworld (UE5, D3D11) shows why the identity layer matters on this route: its 2026-09-07 log (`f034397a`, NR on,
  `Dx11Upscaler` auto, so native DLSS before step 2) has DLSS features at 3440x1440, 1691x1356, 3396x1356 and
  836x1356 alive in one session, which NR then silently ignored. A present route fed by every evaluate would be
  handed other views of the game.

What it does:

- **Where.** `NoteDx11Evaluate` (step 1's call in `NVSDK_NGX_D3D11_EvaluateFeature`) now takes the evaluate's
  context, parameters and result, and hands them to `CarryNativeDx11Guides` (`retoque/dlssnr/DlssNr_Dx11Guides.cpp`)
  before it decides its report. The upstream line changed; there is still one call, and no new one.
- **When.** Only with NR on, `HookMethod=2`, the feature being native D3D11 DLSS (`Upscaler::DLSS`), and the D3D11
  bridge hosting the pass at present (`State::nrPresentHostActive`: set when the bridge is built for the pass, which
  needs NR on and `HookMethod=2` when the swapchain was created, or when an idle host kept for synthesized frame
  generation first runs the pass). Otherwise `Dx11Guides::Decide` answers `NotWanted` and the transport returns before
  it looks at the context, the parameters, the bridge or its own state. At the defaults nothing here runs. NR on,
  `HookMethod=2` and native DLSS with no host is said once (`Dx11Guides::NoHost`), with nothing else looked at either.
- **The decisions** are a pure header, `retoque/dlssnr/DlssNr_Dx11Guides.h`: the evaluate succeeded; an immediate
  context; depth and motion present, 2D, single-sampled, one slice; an output; and the output the size the bridge
  presents. That last one leaves out another view of the game, which the identity's rule 2 would refuse anyway and
  which, as the ring's newest capture, would cost the main view its own (the ring offers the newest). Before the
  bridge's first present the size is unknown, and rule 2 decides.
- **The transport** is upstream's, the one `dlss_12` uses, with one exception for depth. `Dx11WithDx12::
  PrepareUpscalerResources` copies the motion (and a depth that is not a depth-stencil, or one with a stencil plane,
  which its `DepthTransfer_Dx11` converts) on the game's immediate context into shared textures, legacy or NT handles
  as `DontUseNTShared` says, and syncs: the D3D11 context signals a shared fence and the bridge's D3D12 queue waits on
  it on the GPU, the direction the swapchain bridge already takes at every present. D3D11 never waits on D3D12.
- **A depth bound as a depth-stencil gets a copy of the transport's own** (`Dx11Guides::DepthCopy`): subresource 0
  into a shader-resource-only texture of the same family (typeless where the source is typed), shared and opened on
  D3D12 per set, recorded on the same context before upstream's signal so the same wait covers it. Upstream's copy
  path would share a texture that keeps the depth-stencil bind, and the wine case below found that D3D12 never sees
  D3D11's copy into such a texture, waited for or not. A depth-stencil with a stencil plane
  over NT handles has no safe copy here (upstream would share it as it is, and DepthTransfer runs only over legacy
  handles) and is refused (`depth-stencil`).
- **The game's D3D11 state.** The copies change no binding. The one step that runs a shader on the game's context is
  upstream's `DepthTransfer_Dx11` (`Dx11Guides::RunsShader`: R24G8 or R32G8X24 typeless over legacy handles), so only
  around it are the compute shader, shader resources and unordered-access views saved and put back, and, for a
  depth-stencil, the output merger unbound and restored whole (render targets, depth-stencil view and unordered-access
  views), since a depth still bound as the depth-stencil view could not be read. Append counters are kept. Constant
  buffers and samplers are not touched: nothing sets them, and a round trip would lose offsets bound with
  `CSSetConstantBuffers1`. The host suite holds `DepthTransfer_Dx11::Dispatch` to the calls this assumes.
- **Into the ring.** A list of the transport's own, on the bridge's device, copies the D3D12 openings of the two
  shared textures into the present guide ring through `CaptureBridgedGuides`: the same capture core as a D3D12
  evaluate (`CaptureTemporal` and it now share `CaptureGuides`), the same clones, tracking, stamp and commit. The
  stamp's output extent is the game's D3D11 output texture's, its base `DLSS.Output.Subrect.Base`; the guide
  description comes from `GatherGuideFrame`, the part of `GatherFrame` that reads values only (create flags, reset,
  subrects, motion scale), without its exposure reads, logs or counters. The list is executed on the bridge's queue
  right after that queue's wait for the D3D11 copies.
- **At present** nothing new: the bridge's present host asks `CapturedPresentGuides`, on the same queue, and the
  identity rules decide (same-queue admission; device, extent, base, presenter, ambiguous, consumed, age, subrect;
  rule 6 not enforced on the bridge). `CapturedPresentGuides` now also publishes the size it presents, which the
  transport reads; only presents that reach it publish, so one the host declines first (HDR it does not take, a
  feature that failed to build) leaves the last size in place.
- **Lifetime.** Four sets of shared textures, rotated per carried evaluate; they are upstream's four cache sets
  (a `static_assert` holds the count to `DX11_WITH_DX12_INPUT_FRAMES`). A set is written again only once the list
  that read it has completed, checked on the CPU against the transport's fence; a set still in flight refuses the
  evaluate (`slot in flight`) rather than wait. Upstream's prepare releases every set of a cache that a device change
  made stale (`Dx11WithDx12::Init` bumps the generation), whichever set it was asked for, so a stale cache is prepared
  only once every list the transport submitted has completed (`Dx11Guides::CacheFree`), and is `slot in flight`
  until then; a new D3D11 device also rebuilds the transport's own depth copies. A texture the game created shared
  already would be opened as it is and read while the game's next frame writes it, so it is refused (`already
  shared`). When the bridge's device or queue changes, the transport's old D3D12 objects are freed if their last list
  has completed and left alive otherwise: the game's thread does not wait for the GPU. A build that fails is not
  retried until the device or queue changes. `ShutdownNativeDx11Guides` drains (bounded) and releases them from
  `DlssNr::Shutdown`, before that takes `g_nrMutex`, since a capture takes the transport's lock and then
  `g_nrMutex`; nothing in the DLL calls `DlssNr::Shutdown` today, so in practice they live until the process exits,
  like the module's other D3D12 objects.
- **Step 1's report.** At `HookMethod=2` the native route's panel and log line (`NoGuides`) now appear only while the
  transport is not carrying (no host, or the transport refusing); its text says so and points at the transport's log
  line, which exists for both. Carried is what stands the report down: whether the present then uses a capture is
  the identity line's to say, and the host can still decline the frame (HDR it does not take, a feature that failed
  to build) or the identity refuse every capture. DEVELOPMENT.md section 2.2 and the pt-BR entry follow.
- **No key.** Where DLSS runs is still `Dx11Upscaler`, where NR runs still `HookMethod`; this only makes
  `Dx11Upscaler=dlss` with `HookMethod=2` do what the two say.

Checked against the D3D12 rules rather than against what Proton accepts (CLAUDE.md, "Proton is not a D3D12
validator"):

- No descriptor, view or constant buffer is created by this step; it records barriers and copies only.
- The shared textures are transitioned COMMON -> COPY_SOURCE -> COMMON explicitly, on a DIRECT list. A transition out
  of COMMON is valid for any resource, and a texture shared between two APIs is in COMMON at the boundary.
- The own depth copy is `CopySubresourceRegion` of subresource 0, whole, into a texture of the same typeless family
  without the depth-stencil bind, which D3D11 allows; it is shared like upstream's copies and opened on D3D12 with
  `OpenSharedHandle`.
- The ring's clones are made from the source's own description (`CreatePresentGuideClone`, as on the D3D12 route:
  `DENY_SHADER_RESOURCE` and `ALLOW_SIMULTANEOUS_ACCESS` cleared, mip 0 only), so the copy is between identical
  formats and covers the whole subresource, which depth-stencil formats require.
- The cross-API wait is `ID3D12CommandQueue::Wait` on a fence D3D11 signals; the transport's own fence is only read
  on the CPU. Opening a legacy D3D11 shared handle with `ID3D12Device::OpenSharedHandle` is upstream's default and
  what `dlss_12` already did in Sons Of The Forest under Proton.
- Not checked by anything here: the D3D12 debug layer on native Windows. Rafael's RTX 3060 runs native D3D11 DLSS
  (unlike DLSS-G), so it can check this path.

The log lines (counts and formats made up; no log of these titles records the guides' formats):

```
DLSS-NR native D3D11 guides: carrying the game's depth (format 39 2294x960) and motion (format 34 2294x960) from its native D3D11 DLSS to the present pass through the D3D12 bridge (legacy shared handles); output 3440x1440
DLSS-NR native D3D11 guides: 120 evaluates in 2.0 s | 120 carried to the present pass, 0 not | depth format 39 2294x960, motion format 34 2294x960, output 3440x1440
DLSS-NR native D3D11 guides: 60 evaluates in 2.0 s | 0 carried to the present pass, 60 not (transport 60) | last refused: depth format 39 2294x960, motion format 34 2294x960, output 3440x1440
DLSS-NR native D3D11 guides: not carried: no D3D11 bridge hosts the pass at present in this session. The bridge is built when the game creates its swapchain with [DlssNr] Enabled=true and HookMethod=2, and not beside frame generation fed by the upscaler; after changing either, restart the game.
```

The first is said once, at the first carry. The second is one per window, at least 2 s, held to 30 s while it looks
like the last one written; refusals are listed by reason (`evaluate failed`, `deferred context`, `no depth`,
`no motion`, `shape`, `no output`, `not the presented size`, `slot in flight`, `already shared`, `transport`, `d3d12`,
`ring`, `depth-stencil`; `ring` also covers NR switched off for the session after a fault inside the model). A window
with no carry ends with the last refused evaluate's guides instead (the third line). The fourth is said once, with
NR on, `HookMethod=2` and native DLSS but no bridge hosting the pass. Formats are `DXGI_FORMAT` numbers. Nothing is
logged on any other route. After it, present-real-guides.md's own line for the bridge says what the present did with
the captures:

```
DLSS-NR present guides, identity applied, D3D11 bridge: 1237 presents in 30.0 s, lag limit enforced on 0 | 1237 used the game's depth and motion, 0 refused, 0 skipped with no new capture, 0 had none | ...
```

`grep -a -c 'native D3D11 guides: carrying' OptiScaler.dll` is 1 when step 3 is linked in.

### Found while building: D3D12 never sees a D3D11 copy into a shared depth-stencil under Proton

`tests/dx11-guides-transport` (wine tier) runs the transport's sequence on real devices under GE-Proton11-7, DXVK
and vkd3d-proton as a game gets them, six frames per case, each read back byte for byte. Everything the transport
sends crosses: R32_TYPELESS and R16_TYPELESS depth-stencils through its own copy (legacy and NT handles), an
R32_FLOAT depth render target, and R16G16, R32G32 and R16G16B16A16 float motion through upstream's copy.

Upstream's own copy of an R32_TYPELESS depth-stencil (a shared texture that keeps `D3D11_BIND_DEPTH_STENCIL`) does
not cross, and the suite takes that case apart, one change at a time, logging every frame's first texel as D3D12 read
it against what D3D11 wrote (2026-10-15):

| D3D11 between its copy and the D3D12 read | Frames crossed | D3D12 read |
|---|---|---|
| shared fence only (the transport's order) | 0 of 6 | zero, every frame |
| fence, then a CPU wait for D3D11's work (event query), no access | 0 of 6 | zero, every frame |
| a D3D11 read of the shared texture before the signal, no wait | 6 of 6 | each frame's own value |
| fence, then a D3D11 read-back of the shared texture (access and CPU wait) | 6 of 6 | each frame's own value |

So it is not ordering, and not a lag: no frame ever read an earlier frame's value, and waiting for D3D11 on the CPU
changes nothing. D3D12 sees the copy only once D3D11 has accessed the shared image again after writing it, which
points at the state DXVK leaves a depth-stencil image in after a copy into it (its layout or compression at the
hand-off), not at the format or the fence. The own copy, a shader-resource-only texture, avoids it. That is the
evidence of one runtime and one GPU (GE-Proton11-7, RTX 4090), from a harness and not a game.

**This bears on route (b).** `dlss_12` shares the game's depth through the same upstream path. A D3D11 game whose
depth is a depth-stencil without a stencil plane (R32_TYPELESS, R16_TYPELESS) would, on this evidence, hand the D3D12
DLSS and the after-upscale pass a depth that never arrives (zero) under Proton. Formats with a stencil plane go
through `DepthTransfer_Dx11` and are not affected; Sons Of The Forest's depth format is not in its logs, so whether
its September runs met this is unknown. A fix belongs in upstream's `CopyTextureFrom11To12` (share depth-stencils
without the bind), and was not made here: it is upstream's file and `dlss_12`'s behaviour, not this step's.

### What a Sons Of The Forest or Palworld run should show

Set before the game starts (the bridge's present host is decided at swapchain creation): `[DlssNr] Enabled=true`,
`HookMethod=2`, and `[Upscalers] Dx11Upscaler=dlss` (Sons Of The Forest's ini has `dlss_12`; Palworld's is auto,
which step 2 turns into `dlss_12` with NR on, so both need the key set to reach native DLSS). The game's own DLSS on,
at Quality.

- At start-up: `bridge N hosts the neural pass: no upscaler in this title` (the swapchain bridge's existing line,
  written before any upscaler is called), then `NVSDK_NGX_D3D11_CreateFeature Creating new DLSS feature` (native,
  not "DLSS w/Dx12").
- At the first carried evaluate: `DLSS-NR native D3D11 guides: carrying the game's depth (...) and motion (...)` with
  the game's real formats and sizes. These are the numbers this note has assumed so far; record them.
- Every window after: `N carried to the present pass, 0 not`, the output 3440x1440. In Palworld, other views appear as
  `not the presented size`. In both, `slot in flight` stays near zero.
- The bridge's identity line: `N used the game's depth and motion, 0 refused`. At most one `ambiguous` at start-up.
- `DLSS-NR present host: using the upscaler's own depth and motion, not the zero guides`, once.
- If the bridge presented four or more frames on zero or synthesized guides before the first carry (a menu before the
  game's first DLSS evaluate does that): `DLSS-NR present guides, D3D11 bridge: the model's history restarts on the
  game's depth and motion, held for 4 frames (1 so far)`. It comes again at each later change of source (a loading
  screen with no evaluate, then the game), three times in all and every 100th after, a count shared with the D3D12
  present pass.
- In the NR panel: no route line under the status, and the guides line "Guides: the game's depth and motion, on
  every frame".
- No `WARN DLSS-NR: This game's DLSS runs natively on D3D11 (Dx11Upscaler dlss), and its depth and motion are not
  reaching the pass at present ...`, and no `not carried: no D3D11 bridge hosts the pass`. The warning is said once,
  at the first evaluate the transport did not carry, so one at start-up next to windows of `0 not` is a start-up
  refusal, not a running one; the transport's line says which.
- On screen: the present pass with motion that follows the camera. Without the transport (a build before this one,
  same settings) the present ran on zero guides and ghosted behind a moving camera.

Findings to look for: `depth-stencil` (a depth with a stencil plane and `DontUseNTShared=false`: set it back to
true), `already shared` (the game shares its depth or motion: this route cannot read it safely),
`transport` (a format DXVK will not share with vkd3d-proton: the line's `last refused` formats say which, when the
whole window was refused), `ambiguous` at the
present in Palworld (two evaluates of the presented size in one frame), and `extent` refusals at the present (the
evaluate's output is not the presented frame although it is the presented size).

An A/B worth one more session: the same scene with `Dx11Upscaler=dlss_12` (route (b) at present: the bridge's D3D12
DLSS hands its guides to the ring), the identity line then coming from the same bridge consumer, against native DLSS
with this step, for the frame time and the picture.

### Sons Of The Forest under Proton, 2026-10-15: native D3D11 DLSS is not offered, so step 3 cannot run here

Build `c49fd27c`, GE-Proton, RTX 4090, driver 615.78.08, `Dx11Upscaler=dlss` and `HookMethod=2` set for the run. The
game initialised NGX on D3D11 (`NVSDK_NGX_D3D11_Init_Ext`, AppId 4919, `GetCapabilityParameters` result 1) and never
created a feature: its graphics menu offered no DLSS. The driver's Wine NGX core logged, once:

```
NVSDK Feature 32765: [NGXGetPathUsingQAI:526] error: too many attempts to query adapter info
```

The same game, same machine, with `Dx11Upscaler=dlss_12` (2026-09-06 and 09-14 logs) created its DLSS feature on every
run, with no such error: there OptiScaler answers the capability query with its own D3D12 DLSS. So with `dlss` the
capabilities come from the Wine core's D3D11 path, which cannot query the adapter under DXVK here and reports DLSS
unavailable; the game hides the option. Native D3D11 DLSS is what route (c) exists for, so step 3 is **not testable
on this Proton setup**. Its validation moves to a machine where native D3D11 DLSS runs (native Windows: Rafael's PC).
Both test titles were put back as they were (`dlss_12` for Sons Of The Forest, `auto` for Palworld, no `HookMethod`
line). Under Proton, route (b) (`dlss_12`) remains the one that gives a D3D11 DLSS game NR on its real guides, with
the depth-stencil caveat found above.

## Config surface

**No new key.** Where DLSS runs for a D3D11 game is already one quantity, `Dx11Upscaler` (`dlss`
native or `dlss_12` bridged), and NR already has its own on switch. Steps 1 and 2 above add no key and
no menu control: step 1 adds one read-only line that appears only when NR is enabled and the route is
native (invariant 2: the line is not a control, and it exists only when there is something to say).
Step 3 adds no key either: it makes `Dx11Upscaler=dlss` with `HookMethod=2` carry the guides, and step 1's line at
`HookMethod=2` now appears only while they are not carried.
If (a) is ever built it would attach to `dlss` when NR is on, so `Dx11Upscaler=dlss` keeps meaning
"native D3D11 DLSS" and NR does what its switch says.

## Review against DEVELOPMENT.md

| Item | Anticipated answer |
|---|---|
| 1 Default-identical | Steps 1 and 2 only act when `DlssNrEnabled` is true. With NR off, `CreateFeature` and the evaluate path are unchanged. Step 3 acts only with NR on, `HookMethod=2`, native DLSS and the bridge's present host; otherwise it returns before touching anything. The D3D12 route's capture is the same code moved into a shared core, with the same states and output read. |
| 2 No dead control | Step 1 removes a dead state (NR on, nothing happening, nothing said). Step 3's report change is DEVELOPMENT.md section 2.2's HookMethod 2 row. |
| 3 One quantity, one control | No new key. Where DLSS runs stays `Dx11Upscaler`. |
| 4 Config round-trip | Not triggered. |
| 5, 6 Shader and cbuffer | No shader change. (a) would need none: it reuses `DepthTransfer_Dx11` and the NR pass as they are. |
| 7 Passthrough | The NR pass's passthrough gate is untouched. |
| 8 Vulkan lifetime | D3D11 and D3D12 only. |
| Non-NR-path touch | `DLSSFeature_Dx11.cpp` gets at most one inert call with NR off. Step 3 changes step 1's line in `NVNGX_DLSS_Dx11.cpp` (more arguments, same one call), inert with NR off. |
| Proton is not a D3D12 validator | Applies to every shared-resource and fence step. Native Windows with the D3D12 debug layer is required before any of this is called done. |

## Test plan

**Host, new: `tests/nr-dx11-route`** (host tier, registered in `tests/suites.toml`), on a pure
decision header, `retoque/dlssnr/DlssNr_Dx11Route.h`, standard library only. Inputs: `Dx11Upscaler`
(unset, `dlss`, `dlss_12`, others), `DlssNrEnabled`, `HookMethod`, GPU DLSS capability, whether
the original NGX loaded. Outputs: the route and a reason string. Cases: NR off leaves every
combination as it is today; NR on with `dlss` unset picks `dlss_12`; NR on with explicit `dlss` keeps
`dlss` and reports `inert`, with the text naming `dlss_12`; NR on with a non-DLSS-capable GPU does not
pick `dlss_12`; the report is made once, not per frame. If (a) is built, the same header gains the
output-contract refusals above, each tested alone.

**What the host tier will not show:** that a shared texture, fence or format works on a real
D3D11/D3D12 pair, that the image is right, or that nothing stalls.

**Host, step 3: `tests/nr-dx11-native-guides`**: the transport's decision header (every gate and refusal in order,
another view of the game left out, which copy carries the depth and when it runs a shader, the slot rotation against
a simulated GPU, a stale cache never released under a list, the missing host, the window and its line) and source
guards on the transport, its call site, the capture core it shares with the D3D12 route, the present host's half,
shutdown, and the upstream code it relies on (the stale-cache release in Prepare, the set index, what
`DepthTransfer_Dx11` sets on the context). `tests/nr-dx11-route` follows the report change,
`tests/nr-present-guide-identity` the shared capture core.

**Wine, step 3: `tests/dx11-guides-transport`**: the transport's API sequence on real devices under a local Proton,
above, with the depth-stencil hazard taken apart in four variants. It proves the API sequence and the formats under
DXVK and vkd3d-proton, not OptiScaler's code: not DepthTransfer, not `ScopedDepthTransferState`, and not sets
overlapping in flight (it waits for each frame on the CPU).

**Wine tier (conditional, only if (a) is built):** a round trip through the shared-resource path with
a known pattern, D3D11 writing, D3D12 transforming, D3D11 reading back, comparing pixels, in the
msvc-wine prefix. It would exercise wined3d's D3D11, **not DXVK**, so it proves the API sequence and
not the Proton behaviour; that has to be said in its README.

**In-game, with our titles and with one we do not have:**

| Title | What it checks |
|---|---|
| Divinity: Original Sin 2 (D3D11, no upscaler, `HookMethod=2`) | Unchanged. It calls no NGX; steps 1 and 2 never fire. |
| The Witcher 3 (DirectX 11 renderer, no upscaler) | Unchanged, for the same reason. |
| Crimson Desert, DOOM Eternal, PCSX2 | Unchanged; they are not D3D11 NGX callers. |
| **A D3D11 title that calls NGX directly** | **Required, and not in our set.** Steps 3 and 4 need it. Candidates are the two NeuRotic names (BG3 on D3D11, Satisfactory), neither verified by us to call NGX D3D11 rather than Streamline. Acquiring or confirming one is the first action item. |

For that title, record: the log lines from step 1, the "bridge reached the hand-off" line, the
`DLSS-NR present host` window line if `HookMethod=2`, the frame time with NR off and with `dlss_12`
and NR on against native `dlss`, and a Hold-frame pair for the image.

## Risks

- **The premise may not reach the titles.** If BG3 and Satisfactory reach DLSS through Streamline's
  D3D11 plugin, which this tree does not intercept for D3D11 (`Streamline_Hooks.cpp:498`), neither
  route sees their DLSS call, and the topic is a Streamline problem, not a bridge problem.
- **`dlss_12` is unmeasured and brand new** (2026-10-01, upstream). Its failure modes are unknown to
  this tree. The notification path for an upscaler that fails to run falls back to FSR 2.2
  (`NVNGX_DLSS_Dx11.cpp:873`), which would silently remove DLSS from the user's game if `dlss_12`
  failed; step 2 must therefore not make that more likely than it is, which is why an explicit
  `dlss` is respected.
- **Step 2 changes which DLSS runs** for a user who enabled NR and left the upscaler unset: from the
  driver's D3D11 DLSS to a D3D12 DLSS on the bridge's device. Reversible with `Dx11Upscaler=dlss`,
  and the log says what was chosen, but it is a behaviour change on an opt-in feature the user has to
  be told about.
- **Shared resources under DXVK.** What this tree exercises under Proton is the swapchain bridge's
  NT-shared textures and shared fences (Divinity, measured) and, by upstream's own users, the upscaler
  bridge's legacy shared handles. Whether DXVK supports every format and bind-flag combination the
  guides need is not known here; any (a) or (c) needs a probe on a real Proton install.
- **GPU waits across the bridge.** A D3D11 GPU wait on D3D12 can deadlock under vkd3d-proton's queue
  mapping (`dx11_with_dx12.cpp:243`). (a) must stay on the bridge's CPU-wait rule where it applies and
  must never wait on a transport slot.
- **Padded or sub-rect outputs.** The pass takes the output's extent from the resource's own
  descriptor, and its spatial-contract log says allocation size is not evidence of the rendered
  subrect (`DlssNr_Dx12.cpp:6747`). A game whose D3D11 output texture is larger than its declared
  output would have the padding edited and the guides mismatched, on (a) and on (b) alike. NeuRotic
  refuses an output smaller than declared and a non-zero base; this tree does neither, on any route.
- **Step 3 under DXVK.** The native guides cross through the same shared-texture transport `dlss_12` uses, but with
  the formats a game's own depth and motion have, which only Sons Of The Forest's have been seen to cross (through
  `dlss_12`). A format DXVK will not share with vkd3d-proton shows as `transport` on the transport's line, and the
  present then runs as before, on zero or synthesized guides.
- **Formats.** The pass writes the DLSS output in place through a typed UAV. A D3D11 output in a
  format without a D3D12 typed UAV store (BGRA) fails exactly as the D3D12 route does; not new, but
  more likely to be met on D3D11.
- **Prior art is not a specification.** NeuRotic's claims about NT-shareability and about which
  titles work are theirs; none was reproduced here.

## Open questions for the maintainer

1. Which D3D11 titles in the library call `NVSDK_NGX_D3D11_*` directly, and which use Streamline? That
   decides whether any of this reaches BG3 or Satisfactory, and which title tests steps 3 and 4.
2. Is step 2 (choose `dlss_12` when NR is on and the upscaler is unset) acceptable, or should the
   default stay native and only report? It changes which DLSS runs for NR users. (Implemented as proposed, in a
   commit of its own; reverting it leaves step 1's report in place.)
3. Should a bridge route refuse `MVJittered` motion as NeuRotic's does (their comment: "convention is
   not validated"), or keep parity with the D3D12 route, which ignores the flag?
4. Is a present-time route for D3D11 DLSS titles ((c)) wanted at all, given its HUD cost, or is
   after-upscale placement the only one worth the engineering? (Built as step 3, on the roadmap's word; whether the
   picture earns it is the Sons Of The Forest and Palworld runs' to say.)
5. Is running `ProbeD3D11` on a real title worth doing now, on the strength of it possibly making
   everything above unnecessary?
