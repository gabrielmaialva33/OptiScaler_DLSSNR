# NR before DLSS-G: the present pass at the game's Streamline real-frame boundary

Status: design for review, 2026-10-14. **Step 0 and the interim refusal are built** (see "Steps"); nothing run in a
game yet. Written against `68c490d2` on
`feat/neurotic-parity`; line numbers are into that tree. NeuRotic line numbers are into its clone at `7132a103`.
This is the task [present-real-guides.md](present-real-guides.md) (D5) left open: "running at DLSS-G's real-frame
boundary ... is a separate task with its own queue-ownership risk".

Read first: [nr-present-hook.md](nr-present-hook.md) (the present host, its two reviews, and the 2026-10-04
measurement this note starts from), [present-real-guides.md](present-real-guides.md) (the guide ring, its identity
layer, and the first look at NeuRotic), [hud-protection.md](hud-protection.md) (where a UI mask can come from), and
[DEVELOPMENT.md](DEVELOPMENT.md).

## Step 0 measured (2026-10-14)

Crimson Desert, build `c49b4b1c`, GE-Proton11-7, RTX 4090, driver 615.78.08, the game's DLSS-G with the frame count
forced to 3x (`[DLSSG] OverrideInterpolationCount=2`), `HookMethod=2`. 464 probe windows over 16 minutes; DLSS-G was
switched on and off three times, which split them cleanly:

| | DLSS-G active (196 windows) | DLSS-G idle (268 windows) |
|---|---|---|
| game presents at the interposer | 11,257 | 12,812 |
| presents at the wrapped entry | 36,830 (3.27 per game present) | 12,812 (1.00) |
| of those, nested in the game's `Present` on its thread | 68 | 12,812 |
| DLSS-G `FrameGeneration` evaluates | 25,856 (2.30 per game present) | 0 |
| list executed on the game's swapchain queue / the wrapped entry's / another | 0 / 0 / 25,856 | — |
| of those, inside the game's `Present` on its thread | 0 | — |
| `DLSSG.Backbuffer` is the game's current buffer | 18,987 of 25,856 (73%); never unreadable | — |
| game's creation queue and the wrapped entry's queue (as compared then: possibly proxy against native) | different, in every window | different |

What it settles:

- **Open question 2: DLSS-G presents on a thread of its own.** With it active, 99.8% of the wrapped entry's presents
  arrive outside the game's `Present`; with it idle, every one arrives nested inside it. So the flip-count stand-down is
  the right tool, and a thread-local guard alone would not be.
- **DLSS-G submits its evaluate from another thread, after the game's `Present` has returned**: none of its evaluate
  lists was submitted inside the game's `Present` on the game's thread. Two evaluates per game present at 3x: one per
  generated frame. **Which queue is provisional.** The lists went to neither queue the probe compared against, but the
  "game's queue" there was the pointer the game passed to the interposer, which may be the interposer's proxy, while
  `ExecuteCommandLists` sees native queues; "another queue" may be the game's own native queue. Step 0b compares native
  to native (Streamline answers a `QueryInterface` for `{ADEC44E2-...}` with the native object). NeuRotic's premise, a
  provider-owned queue (its `PreFg.h:40-43`), is neither confirmed nor refuted yet.
- **`DLSSG.Backbuffer` is the game's backbuffer**, read asynchronously: three in four evaluates name the buffer the game
  had current at its last `Present`, and the rest are consistent with the game having moved to the next buffer by the
  time DLSS-G evaluates.
- **S1 is not established by this.** A pass submitted on the game's queue before forwarding precedes DLSS-G's read only
  if DLSS-G's queue waits on something the game's queue signals after it. Streamline must make DLSS-G wait for the
  game's rendering of the buffer somehow; whether that is a fence the game's queue signals inside the forwarded
  `Present` is what step 0b measures: the probe now resolves the game's native queue, compares every queue native to
  native, and counts `Signal` and `Wait` on the game's queue (inside and outside its `Present`) and on the queue
  DLSS-G's evaluate lists execute on, including how many of DLSS-G's waits are on a fence the game's queue signals
  inside its `Present`.

Also seen, and not ours: Streamline's `setDynamicMFGParams failed with status 1` every frame under the forced count
(280 lines, the same as the previous day's run without this build).

## The problem, and how much of it was measured

The record is one item in nr-present-hook.md's 2026-10-04 update: in Crimson Desert, with the game's own DLSS-G at 2x
and the present route (`HookMethod=2`), "NR lands on the real frame's present and the generated frames reach the
screen without it. At 2x every other frame is unedited, which reads as flicker; with DLSS-G off it stopped (68
presents : 68 renders, NR on each)." That is all that was kept: an observation by eye, and one status-line ratio from
the DLSS-G-off control. No capture, no per-frame statistic and no present:render ratio from the DLSS-G-on session.

The advice there stands until something here is built and measured: under DLSS-G, use `HookMethod=1`. The
after-upscale pass edits the upscaler's output in place, upstream of the game's post-processing, its interface and
whatever it tags as HUD-less, so DLSS-G interpolates frames that already carry the edit. It is the Crimson Desert test
path in CLAUDE.md.

Who meets the flicker:

- `HookMethod=2` with the game's DLSS-G. Measured, as above.
- `HookMethod=0` (Auto), read from the code and not measured. Auto defers the after-upscale pass to present as soon as
  the present hook is live and a capture exists (`retoque/shaders/dlssnr/DlssNr_Dx12.cpp:7817-7824`, `:6962`), and
  under the game's Streamline the wrapped swapchain's hook is live on every present. So Auto lands on the same route.
- Not `HookMethod=1`, the default. Not OptiScaler's own frame generation, whose present hook runs the pass before FG
  (below).

## What the tree does today

### Three entries to the present pass

| Entry | Call site | Owns the pass when | Queue |
|---|---|---|---|
| Wrapped swapchain | `OptiScaler/wrapped/wrapped_swapchain.cpp:491-492`, before `MenuOverlayDx::Present` (`:495`) | every D3D12 present the FG hook did not take | the wrapper's own device queue `cq` |
| FG present hook | `OptiScaler/hooks/FG_Hooks.cpp:1437-1445`, before the original present (`:1485`) | OptiScaler's own FG output exists (FSR-FG, XeFG, the DLSS-G output, reprojection; chosen at `:263-281`) | the FG object's game queue (`:1437-1440`) |
| D3D11 bridge host | `retoque/dlssnr/DlssNr_PresentHost.cpp` | the D3D11-on-D3D12 bridge | the bridge's own |

DLSS-G is not a D3D11 feature, so the third row is out of this note. The wrapped entry stands down for 16 counted flips
after each FG-hook pass (`DlssNr_Dx12.cpp:6470`, `:6955`, `:6971`) and, for synthesized FG, from the first flip
(`:6941-6946`).

**Under the game's own DLSS-G only the wrapped entry exists, and it sits below DLSS-G.** The swapchain it wraps is the
one the Streamline interposer's proxy presents to, so DLSS-G's presents, real and generated, all reach it. The code
says so (`wrapped_swapchain.cpp:513-514`, "The game's DLSSG presents through here"), and the 2026-10-04 run is the
consequence: by the time the pass sees a backbuffer, DLSS-G has already taken it as interpolation input.

At that level nothing tells a real present from a generated one. `RunPresentPass` processes a present when a capture
newer than the last one it used exists, and skips otherwise (`DlssNr_Dx12.cpp:7164-7172`). So one present per cycle is
edited: the first to arrive after a new capture. The only frame-type signal at this level is
`InputCommon::dlssg_present` (`OptiScaler/low_latency/input/input_common.cpp:1191-1208`), which counts presents modulo
the detected multiplier for AntiLag 2 and calls the first of each batch real. That is a phase assumption, not an
identity.

### What the tree observes of the game's DLSS-G

With `FGInput` other than `dlssg` (the Crimson Desert configuration):

- **Observed.** The game's `slDLSSGSetOptions` and `slDLSSGGetState`, through the plugin's function table
  (`Streamline_Hooks.cpp:1759-1798`; the mode is recorded at `:1542`, and the state's
  `inputsProcessingCompletionFence` passed through at `:1585-1590`). DLSS-G's NGX evaluate (feature `FrameGeneration`)
  with its command list and parameter block (`OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp:1194-1201`). Every
  `ExecuteCommandLists`, through `Submission::Batch` (`OptiScaler/resource_tracking/ResTrack_dx12.cpp:732-737`).
- **Not observed.** Tags and constants: `slSetTag`, `slSetTagForFrame` and `slSetConstants` are attached only for
  `FGInput` `dlssg` or `nvngx` (`Streamline_Hooks.cpp:2300-2311`). The PCL present markers are attached only for
  `FGInput=dlssg` or the `FixSlSimulationMarkers` quirk (`:1997-2002`). The interposer's own swapchain: nothing is
  hooked on it.

`[DlssNr] PresentSync` (auto) makes the wrapped pass wait for its own write-back before it returns whenever frame
generation could be involved (`DlssNr_Dx12.cpp:7438-7468`, `retoque/dlssnr/DlssNr_PresentSync.h:24-30`). That is
the fix for the pass re-reading its own output under an interposer that paces the client queue (nr-present-hook.md,
2026-09-13). It orders the write-back against the flip below DLSS-G. It cannot move the edit above DLSS-G.

### Three statements the code does not support

1. **nr-present-hook.md, 2026-10-04: "NR lands on the real frame's present".** Nothing at the wrapped level
   identifies the real present, and the skip rule edits whichever present comes first after a capture. Interpolation
   shows the generated frame before the real frame it leads to, so the edited present may well be the generated one.
   Either way one present in two is unedited at 2x, so the flicker observation stands. Which frame carried the edit is
   unmeasured; step 0 below measures it.
2. **`FG_Hooks.cpp:1426-1430`:** the wrapped hook "stands down for the base frame and keeps enhancing the cycle's
   generated frames". It does not. The stand-down at `DlssNr_Dx12.cpp:6955` and `:6971` returns for every wrapped
   present within 16 counted flips of an FG-hook pass, and the wrapped entry does not advance the count while it stands
   down. Synthesized FG returns at `:6941-6946` before that. Generated frames are never enhanced, which is the intended
   design (present-real-guides.md, D5). The comment is stale and should be corrected with the code change.
3. **`OptiScaler.ini:1782-1785`:** "2 Present: on the finished, tone-mapped backbuffer at present, before frame
   generation makes its frames from it." True when OptiScaler owns FG. Under the game's own DLSS-G it runs after, as
   measured.

## Prior art: NeuRotic's pre-FG adapter

The clone reviewed for present-real-guides.md (tip `7132a103`, GPL-3.0 like this tree). The adapter arrived in
`623881ab` ("Run Present NR at the native Streamline real-frame boundary") and four follow-ups on the same day,
2026-09-12: `66e87622`, `21135ef9`, `68d7b76f` and `38b904d4`. It was read, not built and not run, and nothing is
copied: the rule is to port ideas, and its dense header style would not survive review here anyway. Its commit
subjects are one line each, so its intent is stated only in code comments. Below, **claim** marks what a comment says
and **read** marks what the code does. Neither is evidence that the result looks right in a game.

**Which Present.** *Read:* the game-facing swapchain of the game's Streamline interposer, not DLSS-G's. `Install`
detours the interposer's own exports `slSetTagForFrame`, `slSetTag`, `slSetConstants`, `slUpgradeInterface` and
`CreateDXGIFactory`/`1`/`2` (`OptiScaler/dlssnr/StreamlinePreFg.h:570-589`). From each factory those return, or that
`slUpgradeInterface` upgrades (`:525-542`), it detours `CreateSwapChain` and `CreateSwapChainForHwnd` (`:497-506`). From
each swapchain it detours `Present`, `Present1`, `ResizeBuffers` and `ResizeBuffers1` (`:455-481`). `Attach` refuses
any target whose code is not inside the interposer module (`:59-77`), so a non-Streamline swapchain is never hooked. The
swapchain is registered with its creation queue, which must be a DIRECT queue (`PreFg.h:809-818`). Below the
interposer, their copy of OptiScaler's wrapped swapchain stands its own present-time NR down for any swapchain with a
registered owner (`wrapped_swapchain.cpp:251`, `BypassLate` at `PreFg.h:819-824`). The owner record is DXGI private
data set on the proxy; the comment claims the interposer forwards it to the underlying swapchain (`PreFg.h:757-760`).

**Real frame versus generated.** *Read:* by construction. The game presents only real frames, and DLSS-G's generated
presents go below the hook. On top of that, with FG on, a frame is admitted only when:

- the Streamline frame token from the game's `ePresentStart` PCL marker on the presenting thread (`PreFg.h:393-407`)
  has tags and constants recorded on viewport 0 and has not been consumed (`Ledger`, `:292-391`; `Claim`, `:739-755`);
- exactly one swapchain is registered (`:748-752`).

Nested calls forward through a thread-local guard (`StreamlinePreFg.h:47`, `:151`, `:161`). A present arriving while
another thread holds the owner's present mutex is refused (`:153-160`).

**Making DLSS-G wait for NR.** *Claim* (`PreFg.h:40-43`): "Native DLSSG can evaluate asynchronously on a provider-owned
queue after the application Present call has entered Streamline." *Read:*

1. The copy-back runs on the application's creation queue, followed by a `Signal` on the pass's own fence
   (`DlssNr_Present.cpp:1793`, `:1804`).
2. That fence value is committed into a completion ledger keyed to the exact native backbuffer, the provider
   generation, the single native FG instance, the frame token and the sequence (`StreamlinePreFg.h:315-321`;
   `PreFg.h:108-279`).
3. In the NGX evaluate for `FrameGeneration`, it reads `DLSSG.Backbuffer`
   (`OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp:2489`), resolves its native identity, and claims the matching entry
   (`:2641`), including the MFG count and index.
4. `BindExternalWait` stores the dependency on DLSS-G's command list (`:2645-2648`). Their `ExecuteCommandLists` hook
   then inserts `queue->Wait(fence, value)` on whichever queue executes that list. It refuses on a list/queue type
   mismatch or a foreign device (`NrGpuSafety.cpp:290-360`).
5. The frame token is checked only when DLSS-G evaluates on the presenting thread inside the forwarded call: the
   thread-local forwarding frame (`PreFg.h:280-291`) is null otherwise, and a zero token is treated as unchecked
   (`:228`).
6. Before any image is changed, a readiness phase runs the model privately with no copy-back ("probes"). Output is
   allowed only once DLSS-G's consumer path has been observed binding and executing those waits
   (`StreamlinePreFg.h:266-273`; `DlssNr_Present.cpp:1415-1430`, `:1627-1645`).

**The HUD.** *Read:* processed with everything else. The trace line says so (`StreamlinePreFg.h:348-351`,
`policy=full-frame-hud-included`), as does the telemetry label "Present guided (game HUD included)"
(`DlssNr_Present.cpp:1854`). After the copy-back is recorded and closed, never before (`:1777-1779`),
`PrepareFullFrame` re-tags `kBufferTypeHUDLessColor`, `kBufferTypeUIColorAndAlpha` and `kBufferTypeUIAlpha` with null
resources for that frame token on viewport 0, which removes them (`StreamlinePreFg.h:122-141`). DLSS-G then
interpolates the edited full frame with no HUD-less or UI input. A refused frame keeps the game's tags (the subject of
`66e87622`).

**Vulkan.** *Read:* a different mechanism, with no present hook. In the game's `slSetTag(ForFrame)` on Vulkan it
replaces the game's `kBufferTypeHUDLessColor` image with the private output of its after-upscale NR. It does so only
when that image is the very one NR processed, of the same format and extent, on the same recording command buffer. The
replacement is tagged `eOnlyValidNow` (`OptiScaler/dlssnr/VulkanNrStreamline.cpp:48-85`). So on Vulkan it is
after-upscale NR handed to DLSS-G as the HUD-less colour, HUD excluded. It requires fixed `eOn`, a blocked client queue
and no UI recomposition (`:57`).

**Failure and fallback.** *Read:*

- Any refusal at the boundary forwards the untouched image with the game's own tags.
- A copy-back that reached the GPU but whose completion could not be published makes the hook return `E_FAIL` to the
  game instead of presenting (`StreamlinePreFg.h:337`, `:358`).
- A refused claim or a failed wait binding at DLSS-G's evaluate returns `NVSDK_NGX_Result_FAIL_PlatformError`, which
  fails DLSS-G's evaluation of that frame (`NVNGX_DLSS_Dx12.cpp:2669`).
- A failed `queue->Wait` at execution withholds DLSS-G's list from the queue and latches their GPU-safety registry
  failed for the process (`NrGpuSafety.cpp:336-355`).
- A resize invalidates everything pending. A `ResizeBuffers1` with per-buffer queues drops the queue and stops the
  route (`StreamlinePreFg.h:417-454`).

**What it admits.** *Read:*

- The game's DLSS-G options must be viewport 0, `eOn`, 1 to 5 generated frames, `eBlockPresentingClientQueue`, no UI
  recomposition and no `eShowOnlyInterpolatedFrame` (their `Streamline_Hooks.cpp:1718-1725`).
- OptiScaler's own FG output must be off (`StreamlinePreFg.h:274-282`).
- With FG off the same hook still runs NR, through the ordinary present admission (`:310-311`).

What none of this establishes: that DLSS-G really reads the backbuffer on a queue of its own (the premise of the whole
wait machinery), what DLSS-G makes of a frame with its HUD-less tag removed, and whether any of it runs under Proton.
present-real-guides.md already notes that their Streamline handling is a catalogue of failures for Dragon's Dogma 2
dialogue dropouts (`:117-119` there).

## Design options

### A. Hook the game-facing interposer Present (the boundary)

Run the existing D3D12 present pass, `RunPresentPass` with its guide ring, colour decisions, HDR conversion and
submission tracking, from a detour on the interposer swapchain's `Present`/`Present1`, before forwarding. Port the
idea, not the code:

- **Install.** `StreamlineHooks::hookInterposer` (`Streamline_Hooks.cpp:2207`) already receives the interposer's
  module. From it, detour `CreateDXGIFactory`/`1`/`2` and `slUpgradeInterface`. From the factories they return, detour
  `CreateSwapChain`/`CreateSwapChainForHwnd`. Detour a swapchain's functions only when their code lies inside that
  module. Record the creation queue: that is the queue the pass submits on, never `state.currentCommandQueue`, which
  the PCSX2 FFX run showed can hold someone else's queue (`FG_Hooks.cpp:1432-1436`).
- **Identity.** A call into the interposer's `Present` is the game's present, so it is the real frame. Generated frames
  never reach it. That needs no frame token, no tag observation and no PCL marker, as long as the HUD policy leaves the
  game's tags alone (H1 below).
- **Stand-down.** While the boundary is live, the wrapped entry stands down for every present, by the same flip-count
  rule the FG hook uses (`DlssNr_Dx12.cpp:6955`), so it does not depend on whether DLSS-G presents on the game's thread.
  It stands down even when the boundary refused this frame: a refused frame is shown as the game drew it, not half
  edited below.
- **FG on or off.** The boundary is correct either way. With DLSS-G off, or suspended by the game (out of focus, a
  menu), the interposer forwards one present per app present, and the pass still runs once. The route does not
  change, so there is no history reset when DLSS-G toggles. Own the boundary only while Streamline's DLSS-G plugin is
  loaded (the check `DlssNr_Dx12.cpp:7439-7442` already makes); otherwise the wrapped entry keeps the route it has.
- **Cost.** One pass per real frame, whatever the multiplier: the transport nr-present-hook.md costed (two full-size
  copies, 39.63 MB of payload per processed present at 3440x1440) plus the model, and no more at 4x than at 2x. The
  rejected alternative, processing every present, multiplies the model by the multiplier (nr-present-hook.md, "The 4x
  interaction").
- **Latency.** The pass now sits ahead of DLSS-G's input processing on the GPU. The real frame waits for the model, as
  it does after the upscaler on `HookMethod=1`, but at output size after tone mapping. Unmeasured.
- **Our menu.** `MenuOverlayDx` keeps drawing in the wrapped entry, below DLSS-G, on every displayed frame. The model
  never sees it and DLSS-G never interpolates it, which is the order nr-present-hook.md asked for ("Our ImGui should
  draw after the pass").

**Ordering against DLSS-G**, three ways, in order of preference:

- **S1, queue order alone.** The pass submits on the presenting queue before forwarding. DLSS-G has to make whatever
  queue reads the backbuffer wait for the game's own rendering of it, which is on that queue and precedes the present.
  If that dependency is inserted inside the forwarded `Present`, the pass is ordered before it with no wait at all.
  This is the hypothesis step 0 tests. Do not assume it.
- **S2, a bounded CPU wait before forwarding.** The existing `PresentSync` mechanism, moved above the forward. Safe
  against DLSS-G reading early. It costs throughput: in PCSX2 without FG the same wait took the pass from 127 frames a
  second to 106, one run each (`DlssNr_PresentSync.h:11-15`). **It can deadlock** if the presenting queue holds a
  wait on DLSS-G work that DLSS-G submits only inside the next forwarded `Present`: the pass's list sits behind that
  wait, and the wait for it can only time out. That is the shape of the present-thread stall the donor's `555ed6db`
  reported. If S2 is used, two consecutive timeouts must switch the route off for the session with a reason, rather
  than hitch every frame.
- **S3, a GPU wait bound to DLSS-G's own list.** NeuRotic's approach. Our `ExecuteCommandLists` hook already sees every
  list (`ResTrack_dx12.cpp:732-737`), so it is mechanically reachable. But it is the cross-queue GPU wait this tree
  has avoided twice: for DLSS-G deadlocks (nr-present-hook.md, 2026-10-04 header), and because under vkd3d-proton
  several D3D12 queues can map onto one `VkQueue` (present-real-guides.md, D4; `with_dx12/dx11_with_dx12.cpp:243`).
  Only with step 0's evidence of which queue runs DLSS-G's evaluate list, and a Proton run of the wait itself.

### B. OptiScaler owns frame generation (exists)

The FG present hook already runs the pass before FG (`FG_Hooks.cpp:1437-1445`, before `o_FGSCPresent` at `:1485`).
For a Streamline title that means switching the game's DLSS-G off in its menu and using OptiScaler's: an `FGInput` it
can feed from this title, plus an `FGOutput`. `FGOutput=dlssg` pairs only with the upscaler and synthesized inputs
(`DLSSG_Dx12.cpp:676`), not with `FGInput=dlssg`.

Read from the code, "before the original present" is before both consumers that matter:

- FSR-FG reads `presentColor` in its frame-generation callback, inside the proxy's present
  (`framegen/ffx/FSRFG_Dx12.cpp:513`, `:866-869`).
- The DLSS-G output's `fg->Present()` sets options, constants and tags before the pass (`FG_Hooks.cpp:1379`;
  `DLSSG_Dx12.cpp:863-978`). DLSS-G reads the image only inside `o_FGSCPresent`.
- One exception: with `FGDrawUIOverFG` the UI is drawn into the backbuffer in that `fg->Present()`, so the model sees
  it.

Measured for FSR-FG on PCSX2 (synthesized-frame-generation.md, step 5: 6,211 model evaluations for 12,407 presents,
picture confirmed), and for the DLSS-G output on the D3D11 bridge in Generation Zero. Not measured on a Streamline title
or on Crimson Desert.

Tradeoffs:

- **No new code**, and the ordering is ours.
- **The game's own integration is lost:** its HUD-less and UI tags, its Reflex markers and its pacing are replaced by
  OptiScaler's.
- **It is a user configuration, not a fix:** it does not make `HookMethod=2` correct under the game's DLSS-G.

### C. Stay on `HookMethod=1`

The baseline every measurement below is compared against, and the advice until A is measured. It edits before the
interface and before the HUD-less copy, so it has no HUD question. It does not give the present route's look (the
finished, tone-mapped picture). Titles with no upscaler evaluate cannot use it, but those titles have no DLSS-G either.

### Rejected: edit every present below DLSS-G

This is what the donor did on generated flips, and nr-present-hook.md (item 3) refused it:

- The model cost multiplies by the multiplier.
- A generated frame has no capture of its own.
- The model's history advances on interpolated content.

### The HUD at the boundary: three policies

The backbuffer at the interposer's `Present` has the interface in it. DLSS-G may also hold a HUD-less colour the game
tagged. If DLSS-G locates the interface by the difference between the two, as those inputs suggest, then any policy
that edits one and not the other changes what DLSS-G takes for interface. Neither NVIDIA's documentation as read here
nor NeuRotic's code says what DLSS-G does in that case.

- **H1, full frame, tags untouched.** The model edits the backbuffer with its HUD. The game's HUD-less tag stays,
  unedited.
  - Simplest: no tag observation, no frame token.
  - Risk: the whole edit may read as interface to DLSS-G, and be carried unwarped into generated frames.
  - `UiProtection` (hud-protection.md, the static-overlay mask) applies unchanged and keeps the model off the HUD.
- **H2, full frame, HUD-less and UI untagged.** NeuRotic's `policy=full-frame-hud-included`.
  - Consistent inputs: DLSS-G interpolates one edited frame.
  - DLSS-G loses the game's interface guidance, so the HUD may smear in generated frames.
  - Needs the frame token (the game's `ePresentStart` marker), `slGetNewFrameToken` and `slSetTagForFrame`, none of
    which this configuration hooks today. The tags may change only after the copy-back has been recorded and closed.
- **H3, HUD-less as the source, the difference kept.** Requires the game to tag a HUD-less colour that is readable at
  present (`eValidUntilPresent`), at the backbuffer's extent and in a colour space the pass can match.
  - The model runs on the HUD-less colour, so it never sees the interface and the guides describe what it sees.
  - Outside the interface (where backbuffer equals HUD-less), the backbuffer gets the model's result. Inside it, the
    backbuffer is kept.
  - The HUD-less tag is replaced by the edited copy. DLSS-G then sees exactly the difference the game drew, and
    hud-protection.md's mask (c), `abs(final - hudless)`, comes for free.
  - It rests on the game honouring Streamline's contract that the HUD-less colour is the final colour without the
    interface. An effect the game draws after its interface (grain, a vignette) breaks the equality everywhere, and the
    whole frame then reads as interface.
  - Costs one more full-size copy, a composition pass, a full-size texture, tag observation outside `FGInput=dlssg`,
    and the same frame-token machinery as H2 to replace the tag.
  - A tag's declared state can be wrong: see the Cyberpunk HUD-less state quirk (`Streamline_Hooks.cpp:553-561`). An
    `eOnlyValidNow` tag is copied by Streamline at tag time, so the game's resource cannot be trusted at present.

### Recommendation

Option A, in steps, with C as the advice until step 1 is measured.

- **Why not B.** B exists and is worth stating in the user docs, but it swaps the game's frame generation for ours
  instead of fixing the route where it is broken.
- **Why A.** A keeps the game's DLSS-G, makes the shipped ini text true, and makes NR's cost independent of the
  multiplier.
- **HUD.** Start with H1 or H2, chosen by comparing the two in Crimson Desert, as two builds rather than a shipped
  switch. H3 follows only if the HUD in generated frames is visibly worse and the title tags a usable HUD-less colour.
- **Ordering.** S1 if step 0 proves it, S2 otherwise. S3 only with evidence the other two cannot give.

Meanwhile, an interim correction: while the game's DLSS-G is active, the wrapped entry is the pass's host, and no
boundary is live, `HookMethod=2` declines with a reason and Auto stays on the upscaled route. A frame should be either
edited before DLSS-G or shown as the game drew it, never every other one. This is the "report that combination
unsupported" branch nr-present-hook.md (item 3) already allowed.

## Proton and vkd3d-proton

- **Proton is not a D3D12 validator.** vkd3d-proton accepted the guide resample's 48-byte constant-buffer view, and
  native Windows removed the device on the first frame (`13f10d60`; reduced-scale-guides.md, "Native Windows").
  - A and H1/H2 add no new D3D12 objects. The pass's own objects are unchanged.
  - H3's composition pass must follow the D3D12 rules, not what ran here: CBVs in multiples of 256 bytes
    (`nr-invariants` checks), UAV formats with typed UAV load support (nr-present-hook.md, first review, hazard 3), and
    the HUD-less colour's barriers from its declared state.
- **The backbuffer arrives in `PRESENT`** at the interposer's `Present`, as D3D12 requires of the game. The pass's
  `PRESENT -> COPY_SOURCE -> PRESENT` and `PRESENT -> COPY_DEST -> PRESENT` sequences (`DlssNr_Dx12.cpp:7288-7292`,
  `:7423-7425`) hold unchanged. The menu overlay's transitions happen later, below, on its own queue.
- **No GPU cross-queue wait** without a Proton run of it (S3, above).
- **Vulkan hooks stay skipped.** The boundary is DXGI-side. It must not turn on Vulkan hooks, and it leaves the
  `d8275824` interlock as it is: that interlock turns DLSS-G off only while the Vulkan overlay's menu is open
  (`Streamline_Hooks.cpp:2125-2140`). The D3D12 overlay already coexists with native FG.
- **Native Windows.** Rafael's RTX 3060 is the standing native check, and **DLSS-G does not run on the RTX 30
  series.** It can check the off path, the interim refusal, and H3's pass under the debug layer with FG off. It cannot
  check the boundary. A Windows run on an RTX 40 or 50 card is needed before this is called done (open question 8).
- **Vulkan is out of scope.** DLSS-G on a native Vulkan title goes through Streamline's Vulkan path, where our
  after-upscale `EvaluateAfterUpscaleVk` already edits upstream of the interface. The Vulkan present host
  (nr-vulkan-present-host.md) is a design with FG off as its first gate. NeuRotic's Vulkan idea (the HUD-less tag
  replaced by the after-upscale output) is an after-upscale route, not a present route, and needs no boundary.

## Lifetime, locking and the off path

- **Never free under the GPU.** The boundary allocates nothing of its own. It uses `g_bbCopy`, the present list ring
  and the guide ring, retired as today (`ParkNrResource`, submission evidence).
  - H3's texture follows the same path: allocated on its first frame, parked and rebuilt on a size or format change.
  - The interposer's `ResizeBuffers`/`ResizeBuffers1` detours drain the present lists, bounded, before forwarding
    (nr-present-hook.md, first review, hazard 1), and bump a generation so pending state cannot match the new buffers.
  - A `ResizeBuffers1` with per-buffer queues stops the route rather than guess a queue.
- **One lock.** `RunPresentPass` already takes `g_nrMutex` only around shared state, and waits outside it
  (`DlssNr_Dx12.cpp:7452-7468`). The boundary adds no lock, and no wait while the lock is held. The interposer's
  `Present` may be called on another thread than the evaluate, exactly like the wrapped one today.
- **Inert when off.** With `[DlssNr] Enabled` off, `RunPresentPass` returns before touching anything
  (`DlssNr_Dx12.cpp:6922-6927`), and the detour forwards. With `HookMethod=1` the detour forwards before any
  `GetBuffer`.
  - Install the detours only when `[DlssNr] Enabled=true` was read at start-up. Then a title that never uses NR has no
    detour at all.
  - The price: switching NR on later cannot reach a swapchain that already exists, so the menu says a restart is
    needed for the boundary. Until then the interim refusal applies.
- **Default-identical.** At `HookMethod=1` nothing changes: no detour work, no stand-down, no allocation. The interim
  refusal changes only the opt-in routes.
- **Model cadence** keeps standing down under frame generation (`FrameGenerationActive`, `DlssNr_Dx12.cpp:1139-1150`).
  At the boundary it would alternate the real frames' cost, and DLSS-G was measured unable to pace through that.

## Config and menu

**No new key.** `HookMethod=2` already promises "before frame generation makes its frames from it". The boundary makes
that true. Adding a placement key would make two controls for one quantity (DEVELOPMENT.md, rule 3). The HUD policy
(H1/H2) is decided by measurement, not by a user switch. If H3 lands it is automatic where the title supplies a usable
HUD-less colour, and `UiProtection` keeps its meaning (keep the model off the interface), gaining the HUD-less-diff
source where one exists.

What changes:

- **`PresentSync` auto.** `PresentSyncInputs` gains the boundary as an entry (`DlssNr_PresentSync.h:16-30`). Auto
  waits there under S2 and not under S1. Explicit `true`/`false` keep winning. The menu's box shows what is in effect.
- **Status and menu.** One read-only line under the present controls, shown only when a present route is active:
  - "present: before the game's DLSS-G";
  - "present: the game's DLSS-G interpolates before this pass; HookMethod=1 edits the frames it interpolates" (the
    interim refusal, amber);
  - "present: restart to run before the game's DLSS-G" (NR switched on after start-up).

  The status line counts app presents at the boundary, so present:render reads 1:1 at any multiplier.
- **Text.** `OptiScaler.ini`'s `HookMethod` comment (`:1782-1791`) and `Config.md` say which FG the boundary covers.
- **Round-trip.** If a key is ever needed after all (say, a strictness switch for H3), it gets all four points of
  DEVELOPMENT.md rule 4 at once. None is planned.

## Review against DEVELOPMENT.md

| Item | Anticipated answer |
|---|---|
| 1 Default-identical | `HookMethod=1`: no change. NR not enabled at start-up: no detour. The interim refusal changes only `HookMethod=0` and `2` under the game's DLSS-G. |
| 2 No dead control | No control added. The status lines appear only with a present route active. |
| 3 One quantity, one control | No key. The placement stays `HookMethod`. |
| 4 Config round-trip | Not triggered. |
| 5 Struct equals cbuffer | No change to `DlssNrConstants` or `Params`. H3's composition pass has its own constants, checked like the HDR pair in `nr-invariants`. |
| 6 Shader rebuild | None for A. H3 adds a shader: both precompiled targets it needs, and freshness in `nr-invariants`. |
| 7 Passthrough | The pass's encode is unchanged. H3 composes after the resolve and must not re-encode. |
| 8 Vulkan lifetime | No Vulkan change. |
| §3 Non-NR path | The interposer detours are a Streamline-path change. They are reviewed for inertness with NR off and at `HookMethod=1`, and against the menu overlay and Reflex markers. |

## Steps

**Step 0, log only, plus the interim refusal.** Install the boundary detours in observation mode: they forward, and
record. One line per window, in the style of present-real-guides.md's step 0:

- app presents at the boundary, and presents reaching the wrapped entry (the measured multiplier);
- whether DLSS-G's presents nest on the presenting thread inside the forwarded call or arrive on another thread;
- the boundary's creation queue against the wrapped `cq`, and against the queue that executes DLSS-G's
  `FrameGeneration` evaluate list (matched in `ResTrack_Dx12::hkExecuteCommandLists` by the list pointer NGX was
  handed);
- whether `DLSSG.Backbuffer` at that evaluate is the resource the boundary's `GetBuffer` returned for the preceding app
  present (with proxies unwrapped);
- `Signal`/`Wait` calls DLSS-G issues on the presenting queue during the forwarded call (a log-only hook on those two
  queue methods);
- **which present in the cycle the wrapped pass edits today**: its position after the app present. That answers
  statement 1 above.

The interim refusal ships with step 0, since it needs none of the boundary.

**Built (2026-10-14), not yet run in a game.**

- **The interim refusal** is `retoque/dlssnr/DlssNr_PresentBoundary.h`, which `RunPresentPass` and the upscaled route
  decide through (`tests/nr-present-boundary`). "The game's DLSS-G" is a mode other than off passed to Streamline's
  DLSS-G, or a frame count at DLSS-G's NGX evaluate, with OptiScaler's own frame generation not active. Auto latches
  the upscaled route for the session the first time it is seen.
- **The probe** is `retoque/dlssnr/DlssNr_SlBoundaryProbe.{h,cpp}`, installed only with `[DlssNr] Enabled=true` and
  `HookMethod=2` when `sl.interposer.dll` is hooked. It detours the interposer's `CreateDXGIFactory`/`1`/`2` and
  `slUpgradeInterface`, then `CreateSwapChain`/`CreateSwapChainForHwnd` on the factories they return and
  `Present`/`Present1` on the swapchains, each only when its code lies inside the interposer, and forwards every call.
  One line per two seconds, `DLSS-NR boundary probe: ...`, gives: game presents (and `DXGI_PRESENT_TEST` ones), presents
  at the wrapped entry per game present, how many of those were nested inside the game's `Present` on its thread; for
  DLSS-G's `FrameGeneration` evaluates, whether the list NGX was handed was executed on the queue the game created its
  swapchain on, on the wrapped entry's queue or on another, and how many inside the game's `Present`; and how often
  `DLSSG.Backbuffer` is the buffer the game's swapchain reported current at its last `Present`.
- **Step 0b** adds `ID3D12CommandQueue::Signal`/`Wait` detours (d3d12.dll's, taken from the game's native queue's
  vtable, so every native queue shares them; they count only the two queues above) and native queue identity.
- **Not measured by it:** which present the wrapped pass edits today: with the interim refusal the wrapped entry no
  longer edits under the game's DLSS-G, so statement 1 above stops mattering and is left as it stands.
- **Caveat on identities:** queues and the backbuffer are compared as raw pointers. The game's queue is what it passed
  to the interposer, the wrapped entry's is what the wrapped swapchain holds, and the executing queue is the real one
  `ResTrack_Dx12` sees. If any of them is a proxy of another, "another queue" is the honest reading, not proof.

**Step 1, the boundary.** Option A, the ordering step 0 allows (S1, else S2), and H1 and H2 built twice and compared on
the same scenes. Correct the `FG_Hooks.cpp` comment and the ini text.

**Step 2, if earned.** H3, and with it hud-protection.md's mask (c).

## Test plan

**Host tier.**

- **New: `tests/nr-present-boundary`.** Registered in `tests/suites.toml` and indexed in `tests/README.md`. It builds a
  standard-library decision header, `retoque/dlssnr/DlssNr_PresentBoundary.h`, on the pattern of
  `DlssNr_PresentSync.h`, which `RunPresentPass` and both detours call. Cases:
  - cycles of one app present and one to five generated presents, nested on the presenting thread and on another
    thread: exactly one pass per app present, none on a generated present, the wrapped entry never running while the
    boundary is live, including on a frame the boundary refused;
  - DLSS-G toggled off and on mid-session (the route does not move, no reset);
  - an MFG count change, a resize generation, a `DXGI_PRESENT_TEST` flag, re-entry, and two interposer swapchains
    (refused);
  - detours installed with NR off (forward, zero work);
  - detours absent with the game's DLSS-G active (the interim refusal for 2, upscaled for Auto);
  - a source guard that the wrapped call site still consults the decision.
- **Extend `nr-present-sync`:** the boundary entry under auto, S1 and S2, and explicit values winning.
- **Extend `nr-present-guide-identity`:** the boundary as a consumer. Generated frames no longer offer themselves, so
  `consumed` refusals under DLSS-G should vanish. The interposer proxy and the wrapped swapchain must register as one
  presenter, not trip rule 3 as two.
- **Extend `nr-invariants`:** under H2, no tag call before the copy-back's `Close`. Under H3, the composition pass's
  constant layout and bytecode freshness.
- **Unchanged as regressions:** `nr-present-host`, `nr-present-guides`, `bridge-lifetime`, `fg-synth-policy`,
  `sl-log-repeat`.
- **What these cannot show:** DLSS-G's ordering, the picture, pacing.

**Wine tier.** Nothing in it reaches Streamline or DLSS-G, and `dlssnr-loopback` has no swapchain. No change.

**In-game**, one session per row, with the build stamp, both DLL hashes, Proton and driver versions, multiplier and
raw logs kept:

| Title | What it checks |
|---|---|
| Crimson Desert, game's DLSS-G 2x, `HookMethod=2` (primary) | Step 0: the log above, a menu, a cutscene, Alt-Tab (DLSS-G disables itself unfocused), a resolution change. Step 1: no flicker, judged by eye and by a capture of the displayed frames at the display rate with a per-frame statistic of the edit in a fixed region (alternation is flicker); one pass per app present; no `887A0001` or device removal; wait times; frame-time median and p95 against `HookMethod=1` and NR off; the HUD in generated frames under H1 and H2. |
| Crimson Desert, MFG 3x and 4x ([mfg-count-override.md](mfg-count-override.md)) | The pass count and the model's GPU time do not rise with the multiplier. |
| Crimson Desert, game's DLSS-G, `HookMethod=0` | Interim: Auto stays upscaled and says so. Step 1: Auto reaches the boundary. |
| Crimson Desert, game's DLSS-G, `HookMethod=1` | Unchanged: the standard test path, no detour work. |
| Crimson Desert, DLSS-G off, `HookMethod=2` | 68:68 holds at the boundary, no reset churn. |
| Divinity (D3D11 bridge, synthesized FG) | Unchanged: no Streamline. |
| DOOM Eternal (Vulkan) | Unchanged: nothing new runs. |
| PCSX2 (`~/Games/pcsx2-nr-test/run.sh`, D3D12 present, FSR-FG synthesized) | Unchanged: no interposer, no detour. The FG hook still runs the pass once per base frame. |

## Open questions

1. **Where does DLSS-G read the backbuffer, and what orders it?** On the presenting queue, or on its own queue, and
   does it make that queue wait for the presenting queue inside the forwarded `Present`? S1 against S2 or S3 turns on
   this. NeuRotic's premise is a provider-owned queue, stated in a comment, not shown.
2. **Does DLSS-G present on the game's thread inside the forwarded call, or on a thread of its own?** The flip-count
   stand-down does not depend on it. A thread-local guard, and NeuRotic's token check, do.
3. **H1 or H2.** What does DLSS-G do with an edited backbuffer against an unedited HUD-less colour, and with none at
   all? Only a capture of generated frames in a game answers it.
4. **Does Crimson Desert tag a HUD-less colour, with which lifecycle, format and colour space?** This decides whether
   H3 is possible there. Answering it needs tag observation outside `FGInput=dlssg`.
5. **Which present does today's route edit under DLSS-G?** Real or generated; step 0 answers it, and
   nr-present-hook.md's sentence is then corrected either way.
6. **Is the interim refusal acceptable?** It trades today's half-edited picture for the game's own under
   `HookMethod=2`, and keeps Auto upscaled under DLSS-G.
7. **Install policy.** Detours only when NR is enabled at start-up, with a restart to reach the boundary later, or
   always installed and forwarding?
8. **Native Windows on an RTX 40 or 50.** Who can run the boundary there, since the RTX 3060 cannot run DLSS-G?
9. **Dynamic MFG (`eDynamic`)** changes the count frame to frame. The boundary does not care. NeuRotic refuses
   anything but `eOn`. Is there a reason for that which this note has missed?
10. **Reflex and pacing.** The pass now runs inside the game's `Present`, between its `ePresentStart` marker and
    DLSS-G. What that does to Reflex's latency accounting and DLSS-G's pacing is unmeasured.
