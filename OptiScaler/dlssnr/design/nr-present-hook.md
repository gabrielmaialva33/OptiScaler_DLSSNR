# Present-time NR — port the host, preserve this fork's contracts

Update 2026-09-23: **implemented.** `3e1b3960` (the D3D12 present-time pass and temporal capture,
`HookMethod=2`) and `37db9f65` (the `DLSS_NEURAL_RENDERING` gate removed, without which LTCG dropped
the pass from every build). The text below is the design as written; the code is the authority.

Status: **design only, reviewed twice; no implementation or deployment.** Written 2026-09-12 against
`393cd0b04e80d16923969c1a9d302983318c51e9`, re-audited 2026-09-13 against `150b49d0` — see the
2026-09-13 update at the end, which changes the recommended first slice and settles the one hazard
that could have killed the approach.
This is step 1 of [NR without a game DLSS call](nr-without-game-dlss.md), under
[DEVELOPMENT.md](DEVELOPMENT.md). The recommendation is a **manual, opt-in port**, not a cherry-pick
of the sibling implementation. Its host is useful; several of its lifetime and state assumptions
are unacceptable here.

## The problem

The existing NR entry points depend on an intercepted upscale. The D3D12 and native Vulkan NVNGX
inputs call `EvaluateAfterUpscale` / `EvaluateAfterUpscaleVk`; the D3D12 bridges have equivalent
calls. This tree also has `ScopedPreUpscale`, RR routing, and FSR/XeSS inputs that reach the normalized
NGX path. Thus "no game DLSS" does not necessarily mean "no NR": the actual limitation is **no
intercepted evaluate driving the module**.

A DXGI present provides an independent dispatch boundary and a finished backbuffer. It can host NR
where the current seam is unavailable, and changes the source from an upscaler intermediate to the
displayed image. It does not, by itself, provide valid depth, motion, colour-space information or
proof that this is the game's main swapchain. Those are separate contracts.

The fork chain matters: optiscaler/OptiScaler → Dagherbou/OptiScaler_DLSSNR → this Linux/Proton fork.
`scottmudge/custom_dlssnr` is a sibling, not our upstream update target. At inspection it points to
`555ed6db7f8c4f7c562923468b9968b795329f2a`; the merge base is `973761621353b99bee3dc7d4bb27b117fef2644f`.
`git rev-list --left-right --count HEAD...scottmudge/custom_dlssnr` at the baseline reports **159
commits on our side, 33 on theirs**. Source comparison, not shared filenames, determines what survives.

## What the five commits actually contain

All five were read with `git show`, including their full patches. The branch contains intervening
merges; the list below is the requested port set, not an instruction to merge the branch.

| Commit | Behaviour and disposition |
|---|---|
| `bc223dae` — add the Present hook | 754 insertions / 8 deletions in ten files. Adds `RunPresentPass`, a private allocator/list/fence ring, backbuffer scratch, guide capture, Auto/Upscaled/Present selection, dummy guides, status and menu/config wiring. Calls from `FGHooks::FGPresent` before its original Present and from `LocalPresent` after the overlay. Port the host concept and call boundaries, adapting the contracts below. |
| `3d51cc82` — wait before the flip | Reports tearing/flicker and accumulating smear. Adds a per-submit CPU fence wait and `ResetPresentList` for partial construction cleanup and shutdown. Retain complete failure cleanup; **do not retain the per-flip wait** superseded by `555ed6db`. Its diagnosis is historical, not proof of the final queue contract. |
| `c1e2b710` — close a dropped list | Reports all four slots dying during startup skips, or one-in-four flicker later: an open dropped list prevented allocator reset. Closes dropped lists and logs reset failures / the first pass. Preserve that lifecycle fix, and strengthen handling of a failed Close. The first-pass log precedes submission, so it is not GPU completion evidence. |
| `555ed6db` — park temporals, stop stalling | Reports freeing captured depth/motion under in-flight present work and a present-thread stall inside FG synchronization that wedged/killed the device. Parks old guide references for 32 evaluates and removes the submit wait; allocator-reuse waiting remains. Keep the no-per-flip-wait intent, replace age-based parking with submission evidence for this new path. |
| `215019bc` — Fixes | Changes the dummy-clear handle casts to brace initialization; enables function-level linking in the DLL and forwarder and adds `LIBCMT.lib` to ignored default libraries in the DLL. It does **not** create valid UAV descriptors. Do not import unrelated build settings; our forwarder already has function-level linking enabled. |

Tearing/flicker, dropped-list exhaustion, unsafe temporal retirement and present-thread stalls are
**reported failures in the donor history**, not hypothetical concerns introduced by this review.
The two wait commits give opposing explanations of the flip. Their final source removes the wait;
we must measure ordering under Proton/FG rather than treating either commit message as validation.

## The cherry-pick experiment — every conflict

The main worktree was clean. Created `codex/nr-present-hook` at the baseline, then the disposable
`codex/nr-present-hook-scratch`, and actually ran:

```sh
git -c rerere.enabled=false cherry-pick bc223dae
# Record all unmerged files and diff3 regions.
# For this throwaway applicability probe ONLY, take the incoming side of each conflict region.
git -c rerere.enabled=false cherry-pick --continue
git -c rerere.enabled=false cherry-pick 3d51cc82
git -c rerere.enabled=false cherry-pick c1e2b710 555ed6db 215019bc
```

The provisional resolutions deliberately made **no claim to compile or preserve our semantics**.
They kept non-conflicting local text and selected the incoming side only within conflict markers,
so the later patches could be tried. They are not a port or an approved resolution. Rerere was
disabled so these choices would not train future merges. After recording the final conflict,
aborted the active sequence, switched back to `codex/nr-present-hook`, and deleted the scratch
branch. No trial C++ or project changes remain. The catalogue is inline so it survives the scratch
branch and temporary diagnostic files.

**`bc223dae`: five conflicted files, twelve diff3 regions.** Region numbers below are in file order;
symbols describe the location without depending on line numbers shifted by conflict markers.

| File / region | Conflict | Classification and real resolution |
|---|---|---|
| `Config.md` / 1 | End-of-file insertion follows sibling-only Keyboard Input Fix and MFG Unlock documentation, absent here. | **Mechanical insertion, with scope filtering.** Append only accurate NR documentation. Taking their region wholesale also imports descriptions of features we do not have; its "Direct3D only" claim is wrong for this fork's existing native Vulkan NR. |
| `OptiScaler/Config.cpp` / 1 | `SaveIni` around Enabled, RR keys, ToggleKey and TransferStrength; different indentation and local RR additions. | **Mechanical.** Keep our block, including `ApplyAfterRR`, `RRPasses`, `RRWorkingScale`; insert new saves at the correct scope. The Reload additions merge cleanly. |
| `OptiScaler/dlssnr/DlssNr_Menu.cpp` / 1 | Opening of Colour: localized heading/text versus Present-source conditional. | **Semantic.** Gate by effective source/capability, including Auto, while retaining localization and the existing white-point state table. Checking only configured method 2 is insufficient. |
| same / 2 | Closing Colour / opening Compare: different brace structure and localized Compare heading. | **Mechanical brace placement after the semantic decision above.** Preserve our hierarchy and localized labels; do not transplant their extra enclosing braces. |
| `OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp` / 1 | Include area: their retained `<set>` plus new `<dxgi1_6.h>` versus our removed `<set>`. | **Mechanical.** Check whether our existing `<dxgi1_4.h>` already supplies the needed declaration before adding another DXGI include; do not restore unused `<set>`, and keep `pch.h` first. |
| same / 2 | `Dispatch` definition: their `void` plus `presentSource` versus our `bool`, coverage pointer and recording lease. | **Semantic.** Preserve success reporting, coverage and lease ownership; add source context without reverting the signature's existing guarantees. |
| same / 3 | Output-arrival selection: their `presentSource` override versus our `frame.OutputState`, timing scope and `RestoreOutput` RAII. | **Semantic.** Set the owned scratch's existing `OutputState` to UAV. Keep timing before the first barrier and restoration on every exit. Their region would delete both guards. |
| same / 4 | Scan Tick insertion collides with our `RestoreResourceState smallRead`. | **Semantic.** Preserve the small-resource restore and gate our existing `if (!holdingColor)` scan call by source too. A literal incoming resolution adds a second Tick before the existing call and removes restoration. |
| same / 5 | New `HookStatus` / `RunPresentPass` insertion collides with our `ReportSpatialContract` declaration after `RetryAfterFailure`. | **Mechanical location, semantic body.** Insert new functions beside, never instead of, the diagnostic helper; adapt their bodies as specified below. |
| same / 6 | Guide-capture/routing insertion collides with the end of `GatherFrame` and our expanded `EvaluateAfterUpscale`. | **Semantic.** Keep the separated gather helper, RR policy, per-evaluate pre-upscale decline, coverage, spatial diagnostics and resource checks. Capture and route at an explicit new boundary; do not paste into `GatherFrame` or erase before-upscale exclusion. |
| `OptiScaler/shaders/dlssnr/DlssNr_Dx12.h` / 1 | `Dispatch` documentation and bool return versus donor void return. | **Semantic.** Retain the contract that false does not promise a complete output and that timingQueue is only a hint. |
| same / 2 | Default arguments: coverage / lease versus `presentSource`. | **Semantic.** Match the preserved definition and every caller; no positional bool in a coverage slot. |

**Follow-ups, conditional on those provisional core resolutions:** `3d51cc82`, `c1e2b710` and
`555ed6db` apply with **zero textual conflicts**. `215019bc` adds **one region in
`OptiScaler/OptiScaler.vcxproj`**, where its Release compile settings surround FunctionLevelLinking
and our tree lacks that sibling optimization block. This is **semantic build policy**, not formatting:
keep our project settings. Its `IgnoreSpecificDefaultLibraries=LIBCMT.lib` addition auto-merges but
still must be excluded. The forwarder change is already satisfied and produces no net modification;
the C++ handle-initializer change merges cleanly but leaves the descriptor defect below.

Total observed: **six unique conflicted files, thirteen conflict regions** across the five attempts.
The core's other five files merge textually: `OptiScaler.ini`, `Config.h`, `DlssNrFeature_Dx12.h`,
`FG_Hooks.cpp`, and **`wrapped_swapchain.cpp`**. Clean application is not semantic acceptance. In
particular, the header adds status/entry-point declarations without integrating our ownership model,
and the hooks introduce a new caller of shared state.

## The Linux overlap that Git does not flag

At this baseline `dllmain.cpp` checks for `optiscaler_skip_vulkan_hooks` beside the loaded DLL, **on
Wine**, before calling `VulkanHooks::Hook`. When found it logs the skip and sets
`State::vulkanHooksSkipped = true`. Preserve the lookup location, Wine condition, flag and skip.
None of these five commits changes that file or `State.h`.

The current shortcut in `LocalPresent` is more specific than the older patch description:

```cpp
if (IdentifyGpu::getPrimaryGpu().usesDxvk && isD3D11 && !State::Instance().vulkanHooksSkipped)
```

It calls the underlying Present/Present1 and returns. **Keep all three predicates.** vkd3d-proton
also sets `usesDxvk`; the `isD3D11` restriction independently keeps D3D12 games on the DX overlay
route even without the marker. With the marker, bypassing this shortcut allows `MenuOverlayDx` to
draw through Direct3D. A true D3D11 swapchain still takes that overlay's D3D11 path; the marker does
not turn it into a D3D12 NR target.

The donor's two wrapped-file additions are an include and the new call inside `if (willPresent)`,
after `MenuOverlayDx::Present` and the FG reporting block, before `_frameCounter++` and original
Present/Present1. They auto-merge at this exact location and leave our predicate intact. Preserve
that predicate and existing menu execution when inserting the call. Do not restore the sibling
whole file, move the call into the DXVK early-return branch, gate the D3D12 path on `!usesDxvk`, or
enable Vulkan hooks to obtain the new dispatch. Neither `DXGI_PRESENT_TEST` nor a D3D11 swapchain
may run NR through this call.

There is a concrete state mismatch: `menu/menu_overlay_dx.cpp` transitions the D3D12 backbuffer
`PRESENT → RENDER_TARGET → PRESENT` and submits on `currentSCCommandQueue`. The donor then assumes
`RENDER_TARGET` as the arrival **and return** state. This is wrong at our wrapped boundary.
Use `PRESENT → COPY_SOURCE`, copy out, then `COPY_DEST` for copy-back and restore `PRESENT` before
the flip. Private scratch returns to UAV. The donor comment saying a wrong before-state can only
cause a harmless validation warning must not be ported: the API requires matching state transitions
and PRESENT at presentation. See Microsoft's
[ResourceBarrier contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resourcebarrier).

The FG hook is a separate contract: it uses global `currentCommandQueue`, while the wrapper obtains
`cq` from its swapchain device/queue object. Prove they identify the actual queue and resource
generation before enabling the FG site. `fg->Present()` / FSR callbacks already execute **before**
the donor insertion in our `FGPresent`; "before original Present" does not prove "before every
backend captures interpolation input". Unknown queue, state or ownership means skip with a reason,
not a guessed barrier or a CPU wait. Retain the existing Vulkan overlay's bounded waits, live-device
drain/abandon rules, non-graphics-queue bailouts and ImGui upload fixes untouched.

## The proposed port contract

### Scope and the no-DLSS prerequisite

First implement the **D3D12 DXGI host with captured, current engine guides**. Native Vulkan
presentation, D3D11 presentation and cross-device bridges are unsupported present sources in this
slice; their existing upscale paths must continue. A D3D12 application under vkd3d-proton is in
scope. Keep the forwarder and one-consumer detection; this is not a second NGX consumer or a new
ReShade dependency.

Deliberately defer the donor's `RequireDlss=false` / dummy-guide branch to step 2 of
`nr-without-game-dlss.md`. The host can be reached with no evaluate, but in step 1 it must report
**waiting for guides**, not claim to have enhanced that frame. A DLSS-less title with no normalized
upscale call will not gain visible NR from this slice alone. That is a limitation of the proposed
scope, not evidence that the present hook failed.

The donor already tries the cheaper no-guide idea, but `EnsureDummyTemporal` clears depth to **1**
and motion to zero with zero GPU/CPU descriptor handles. `215019bc` only makes those handles compile.
Valid initialized UAV descriptors in the appropriate heaps, a bound shader-visible heap and proper
ordering are required by
[ClearUnorderedAccessViewFloat](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearunorderedaccessviewfloat).
Step 2 must define depth convention (the existing note proposes zero-filled depth), initialization,
barriers and completion; a clear merely recorded on a subsequently dropped list is not initialization.
Do not ship this broken fallback or expose its checkbox as if it worked.

### Placement, config and menu

Propose the donor-compatible new key `HookMethod`, default **1**, but interpret 1 as **the existing
upscaler seam selected by Stage**, including Before and its fallback. This preserves current users
with `Stage=1`; importing the donor label "Upscaled" literally would hide that distinction.
Use one localized **Run at** selector in place of two competing placement controls:

| Selection | Persisted values | Effective route |
|---|---|---|
| After the upscaler | HookMethod=1, Stage=0 | Existing after-upscale / eligible RR path. |
| Before the upscaler (experimental) | HookMethod=1, Stage=1 | Existing scoped pre path and same-evaluate fallback. |
| Present | HookMethod=2; preserve stored Stage | Capture guides only at the upscale seam; dispatch only at eligible present. Missing guides means no NR. |
| Auto (after or present) | HookMethod=0; preserve stored Stage | After-upscale until an eligible present route with fresh guides has been established, then present. Before-upscale is inactive in this explicitly labelled mode. |

`HookMethod=auto` in the INI means the `CustomOptional` default **1**; numeric **0** explicitly selects
Auto routing. Clamp/reject out-of-range values consistently on load/runtime/UI to effective 1.
Unsupported APIs use the existing Stage behaviour, with a read-only capability explanation instead
of dead present controls. Do not infer API support only from `IsRunningVk()` after the model starts.

Four-point round-trip for this slice: `CustomOptional<uint32_t> DlssNrHookMethod { 1 }` in `Config.h`;
`readUInt("DlssNr", "HookMethod")` in Reload; `SetValue` using `value_for_config()` in SaveIni; and
`HookMethod=auto` with the numeric meanings in the shipped `OptiScaler.ini`. Add accurate `Config.md`
documentation. Preserve all current Stage, RR, pass and timing saves.

**No RequireDlss key or checkbox in step 1.** When step 2 implements the fallback, add all four
points together: `CustomOptional<bool> DlssNrRequireDlss { true }`, `readBool`, `SetValue` via
`value_for_config()`, and `RequireDlss=auto` in the INI. Explain it as requiring engine depth/motion,
not the DLSS brand or the selected upscaler backend. Show it only on a supported explicit Present
route with that implemented fallback; Auto continues to require fresh guides.

For effective Present, hide inapplicable white-point/exposure/scan controls and explain the source;
do the same when Auto is actually using Present. Keep the existing Colour state table unchanged
on upscale sources. Present uses normal pass settings/WorkingScale; the separate after-RR controls
are inactive there, with that scope explicit. `AfterRayReconstruction` must not leak from a captured
guide frame into this distinct route. Preserve the existing ApplyAfterRR opt-in on upscale routes;
Present is an explicit choice to process the finished image, including any RR result already in it.

Keep effective-source status separate from requested settings. Report unsupported, waiting, skipped,
recorded, submitted and fence-confirmed states accurately; a non-null feature or a captured guide is
not "active" evidence. Retain current historical timing labels. Hold and Compare operate on the
present source in this mode; unlike before-HUD NR, that includes game UI. The donor's post-overlay
placement also includes OptiScaler's menu. Test it visibly; moving the wrapped call ahead of our
overlay is a possible later decision, not an unacknowledged change to the audited ordering.

### One owner, real submission, valid guides

1. **Choose ownership before either seam records NR.** Capture guides independently of whether the
   after-upscale path would return for Stage/RR reasons. For Present/Auto on a supported D3D12 path,
   prevent `ScopedPreUpscale` from editing the same frame. In Auto, reserve one route per base-frame
   identity; newly discovered eligibility takes effect on the next frame. If the selected present
   later fails, show the game's frame rather than running NR a second time. Loss of capability or
   fresh guides makes Auto fall back on a subsequent frame, with a history reset.
2. **Bind work to a swapchain/device/queue generation and actual frame identity.** The donor's global
   seq/seen token can suppress a different swapchain and does not identify base versus generated
   images. Associate nested FG/wrapped calls with the same image; distinguish attempted work from
   successful submission. Reject test presents, duplicate entry, secondary/unknown swapchains,
   incompatible formats and unproven queue relationships. Query `IDXGISwapChain3` support; do not
   trust the donor's unchecked cast.
3. **Start with one enhancement per real frame.** The donor also runs on generated flips, even though
   they may already interpolate enhanced bases and have no matching new guide capture. Do not enable
   that behaviour until a backend-specific trace and image test proves guide/history correctness.
   With FG, either prove the base-frame host and suppress its nested/generated calls, or report that
   combination unsupported. Do not advertise "before FG" across all backends from hook placement alone.
4. **Capture data, not just COM lifetime.** The donor AddRefs the game's resources; the game can still
   overwrite their contents after the evaluate. `capturedAt` is stored but never used to expire them,
   and a missing pair leaves the old valid flag set indefinitely. Capture into owned guide snapshots
   at the known-state evaluate boundary, record producer submission evidence, preserve render subrects,
   bases, MV scale/resolution, jitter and reset flags from `GatherFrame`, and associate the pair with
   its base frame. A never-submitted capture, wrong device/extent or stale pair must not be consumed.
   Restore the game's input states and parameter bindings. If a different queue produced the copy,
   require a proved GPU dependency; do not substitute an AddRef or a frame-count delay.
5. **Reuse this tree's dispatch guarantees.** Keep bool `Dispatch`, coverage, `RecordingLease`,
   `frame.OutputState`, extent settling, multi-pass admission and RAII restoration. Copy back only a
   complete successful composition, not merely `!g_nr.failed`. A false dispatch may have recorded
   feature initialization, scratch writes or timing: discarding the list must also invalidate that
   unsubmitted state and close/seal the recording. Alternatively submit explicitly defined setup-only
   work without copy-back and track it. Never claim it initialized GPU resources or advanced displayed
   history just because CPU bookkeeping changed. Audit every SetExtras/model branch, including chains.
6. **Track the entire new recording, including copy-out/back.** Register before the first resource use;
   verify private lists pass through the actual `Submission::Batch` queue hooks and Reset observation.
   Opt Present/Auto into tracking from the first NR recording, even at one pass; do not force a fake
   multi-pass setting to trigger `WantsTrackedRecording`. This also covers Auto's initial upscale
   fallback, otherwise its first legacy frame would make the subsequent live handover unsafe.
   Preserve single-execution timing certificates and name the new source in metadata/coverage.
   Existing dispatch timing excludes outer copies: retain that definition and measure total present
   cost separately, or explicitly introduce a new interval. Do not silently relabel one as the other.
7. **Retire by evidence.** Ring slots, scratch, snapshots, model features, descriptor heaps and mapped
   constants remain owned until all recorded uses are sealed and completed. Reusing a command-list
   allocator after timeout, failed event registration, failed Signal or device-loss `UINT64_MAX` is
   forbidden; the donor logs a timeout and then still attempts Reset. Prefer skipping a busy slot to
   waiting on the FG present thread. Close a dropped list, recreate a poisoned list safely if Close
   fails, and unwind every partial allocation. Shutdown/resize/device replacement use the same proof,
   retaining unknown work rather than freeing it. The default legacy path's existing 32-evaluate
   retirement is not a safety proof for the new private lists.
   A list ring alone does not isolate the shared `g_nr` surfaces or model history. Initially allow
   only one outstanding composition using them, skipping if busy. After completion, reset/seal the
   actual prior list before admitting another recording: our `Submission::Ready` requires both
   completion and protection against replay, and resetting a different backbuffer slot does neither
   for that prior recording. Measure this serialization cost before proposing per-slot model state.
8. **No unsafe live upgrade.** Our tracking is sticky and cannot reconstruct evidence for previously
   recorded legacy NR. If Present is selected after untracked work, persist the request and report
   restart required while retaining the old route/resources. Do not silently enable tracking halfway
   through their lifetime. Source changes in an already tracked session reset model history, held
   colour/guides, source-specific diagnostics and capture generation after safe retirement.
9. **Serialize without a new deadlock.** `Dispatch` already locks `g_nrMutex`; pre scopes / Shutdown
   take `g_preMutex` before it. The donor touches capture, list ring, seq/seen and `g_compose` outside
   that lock. Define one ownership protocol and keep the existing lock order; do not wrap a call to
   Dispatch in the same non-recursive mutex, hold it across original Present, or hold it while waiting
   for GPU progress. Concurrent evaluate/present/resize and menu status reads need explicit handling.

### Colour and passthrough

For the first supported SDR present contract, set `ColourIsLinearHdr=false`, clear ExposureTexture,
set PreExposure=1 and the owned scratch's OutputState=UAV. This selects existing `Passthrough=1`
for encode and resolve. Preserve the gates in `SoftKnee`, `CubeScaleResidual`, proxy/model decoding,
white-point normalization, matched-residual `fullProxy`, and reversible decode. "Passthrough" means
the colour bridge leaves the already encoded source alone; the intentional NR edit/composition can
still change the result. The donor's claim that the whole resolve becomes a copy is too broad.

Do not equate "swapchain" with "sRGB SDR": HDR10/PQ, scRGB, sRGB view conversion, channel formats
and UAV/copy compatibility need an explicit colour-space/format check. Initially skip unsupported
contracts with a reason rather than feed HDR values to an SDR passthrough. This slice needs no HLSL
or `DlssNrConstants` change; reusing the existing gates is preferable to inventing a second encode.

## Guards and the required review

These are proposed acceptance conditions, not claims that an implementation has passed them.

| DEVELOPMENT.md §1 | Application to this port |
|---|---|
| 1 — Default-identical | NR off or default HookMethod=1 executes the prior Stage path. No new present allocation, guide capture, tracking-hook activation, barriers, queue submits or waits beyond the existing path. Add the new early gate before touching resources; preserve normal Present/Present1 flags, menu path and timing. Test both fresh-disabled and disable-after-use; retained in-flight objects are safely retired, never synchronously torn down on toggle. |
| 2 — No dead control | Capability and effective-source state govern placement, Colour and RR controls, including Auto and native Vulkan before NR has run. Deferred dummy support gets no checkbox. Status must not claim a successful pass merely from the requested mode. |
| 3 — One quantity, one control | One Run at selector maps existing Stage plus new HookMethod. No competing Stage/HookMethod selectors, duplicate model-scale sliders, renamed exposure knob or separate Present colour tuning. Existing settings keep their meanings on their existing routes. |
| 4 — Four-point config | HookMethod declaration/read/save/shipped INI together; invalid/default/explicit values and SaveIni/reload tested. RequireDlss follows the same rule only with its later implementation. Config.md and localized menu describe effective defaults accurately. |
| 5 — Struct/cbuffer agreement | No constant-layout change planned. If implementation needs one, append the same ordered 4-byte scalars at the end of both definitions and check the entire layout. Source-context CPU metadata is not permission to alter the shader ABI accidentally. |
| 6 — Real shader rebuild | No rebuild for this note or an unchanged HLSL transport port. Any shader change requires both pinned DXIL and SPIR-V outputs/headers and freshness checks, per DEVELOPMENT.md; the fxc guidance in the older module README is stale. |
| 7 — gPassthrough | Reuse every existing gate listed above. Verify passthrough with changed white point, exposure, reversible and matched-residual settings; an encoded frame must not acquire a second colour transform. Do not promise no NR edit when the model is intentionally enabled. |
| 8 — Vulkan lifetime | No new native Vulkan resources/hooks. Preserve live-device drain-before-free and dead-device abandonment, and validate native Vulkan regression paths. Any future Vulkan present implementation triggers its own mandatory resource review. |
| 9 — Local until asked | Design and subsequent implementation stay local until explicitly authorized otherwise. This task commits only the note; no push or installation. |

Before deploying code, run §3's **menu review** against both the placement table here and every row
of §2's Colour table; **config review** over all four points; and **non-NR-path review** of both
hooks and their off-path traces. Apply the **shader review** if HLSL changes (including numerical
safety and both rebuilds), and **Vulkan resource review** if that scope changes. Independently walk
record/Close/drop/submit/Signal/Reset/retire/resize/shutdown and lock order adversarially. Passing a
build or accepting merge markers is not this review.

## What testing would prove it

The baseline registry was checked with `python3 tests/run_all.py --list`: **fourteen suites**, eleven
host and three wine. [tests/README.md](../../../tests/README.md) is the index and
[suites.toml](../../../tests/suites.toml) rejects missing/unregistered directories. The following is
future implementation validation; no host, Wine, game or GPU run is claimed for this docs-only work.

| Existing coverage | Required use / extension for the port |
|---|---|
| `nr-before-upscale` | Prove HookMethod=1 preserves pre scope, RR decline and same-evaluate fallback; supported Present/Auto cannot record both pre and present work. Unsupported bridge/API routes keep their old behaviour. |
| `nr-dispatch` | Keep constant/descriptor isolation and partial-init checks. Exercise owned OutputState, bool success, skipped/partial dispatch with no copy-back, source-context gates and no duplicate scan Tick. |
| `nr-menu`, `nr-localization` | Walk the new placement/capability table and all Colour anchor sub-states. Check Auto transitions, no-model-yet native Vulkan, RR scale visibility, preserved labels/IDs and no dead dummy control. |
| `nr-pass-config`, `nr-gpu-timing-config` | Round-trip HookMethod absent/auto/0/1/2/invalid while preserving Stage, RR, pass settings and timing transactions. Test deferred RequireDlss only when implemented. |
| `nr-multipass`, `nr-submission` | Extend portable production logic coverage for ownership, busy/failed slot handling, capture submission, stale/missing guides, duplicate nested entry, two swapchains, Reset/drop/replay, queue changes, failed Signal and device loss. No free after 32 CPU frames while fences remain pending. Keep existing retirement bounds and recording lease tests. |
| `nr-gpu-timing`, `nr-timing-boundary` | Actual submission certificates, dropped/incomplete samples, explicit present source/frame generation metadata, immutable historical samples and correct boundaries for composition versus total present work. |
| `nr-shutdown` | Preserve the corrected NGX Shutdown1 ABI; add present-resource teardown coverage elsewhere rather than pretending this adapter test exercises GPU teardown. |
| `nr-gpu-timing-d3d12` | Retain real timestamp/submission validation. It never loads NGX, so it cannot prove the NR present host ran. |
| `dlssnr-loopback` | Preserve existing real NR initialization/extent/shutdown coverage. **It has no swapchain**, and `scene.hlsl` is not wired into a deterministic image fixture: unchanged, it cannot exercise either new hook or prove displayed output. |
| `vulkan-overlay` | Run unchanged as a real Wine/Vulkan regression guard, including negative coverage, partial-init/failed-drain, resize and both CONCURRENT/EXCLUSIVE non-graphics-present bailouts. It does not exercise D3D12 NR or Streamline pacing. |

The Vulkan runner now sets
`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` through Wine's native
loader, as well as enabling the Khronos layer. Keep synchronization validation enabled and require
zero unexpected errors; do not suppress `SYNC-HAZARD-*` to obtain green results. Its invalid-fence
negative control proves validation callbacks work, not every synchronization dependency. A capability
SKIP of a non-graphics control is recorded as such. The suite refuses the skip-hooks marker in its
test directories, so it **cannot prove the marker-enabled D3D12 menu route**.

Add a focused present suite (or a clearly separated swapchain mode to loopback), using production
entry points and a real flip-model D3D12 swapchain under Proton. Register any new directory and update
the index. It needs deterministic frame IDs/pixels, valid owned guide snapshots and readback. Require
nonzero successful NR submissions and fence-confirmed copy-back, not just successful API returns:

1. Baseline versus new build with NR off; then NR on with default HookMethod, Stage 0 and Stage 1.
   Compare unchanged image bytes and observable hook/queue/resource side effects. Repeat with
   explicit Present enabled then disabled; test Present and Present1, including TEST flags.
2. Explicit Present with missing guides for more than four flips, then valid guides: all slots
   recover. Capture-only, never-submitted and stale captures must not run the model. For the host-only
   no-evaluate case, assert the waiting reason and zero NR; it is not a positive no-DLSS quality test.
3. Delay GPU completion beyond ring reuse and beyond 32 CPU frames; inject each construction, Close,
   Reset, event and Signal failure. No unsafe reuse, leaked partial ring, copy-back of partial scratch
   or present-thread fence stall. Resize, change format, recreate the swapchain/device and shut down
   with pending work; ownership retention must be explicit and bounded.
4. Exercise nested FG/wrapped order, swapped callback order and multiple swapchains. Count by actual
   base/generated identity; the base is enhanced once, generated frames are not re-enhanced in the
   proposed first slice. Trace producer/copy/NR/copy-back/FG capture/flip ordering with tearing on/off
   and V-sync on/off. Wrong-image flicker or history smear fails acceptance even if no API fails.
5. Read back SDR passthrough/zero-edit controls, sub-native and full working scale, reversible and
   matched-residual modes. Unsupported HDR must fail closed with a visible reason. Record dropped
   work/history resets. Host fakes cannot establish colour correctness or displayed-frame identity.

For Linux acceptance, use isolated marker-present and marker-absent D3D12 runs and a D3D11 DXVK
control: confirm the exact skip log/flag, expected shortcut branch, D3D12 overlay on the correct
swapchain queue, menu open/closed, and no accidental Vulkan hook activation. Then validate in
Crimson Desert with its existing marker and native FG, and a title/path where the old NR dispatch
is not reached. In the latter, distinguish host entry from the deferred ability to run without guides.
Keep current game configuration/VRR policy; no installation or game changes belong to this note.

Run `python3 tests/run_all.py` and the wine tier serially with `build-local.sh`, never against the
compiler prefix concurrently. Build both Release DLLs; format changed production C++ with
`/usr/lib/llvm20/bin/clang-format` (20), not PATH's binary. Record commit/build stamp, both DLL hashes,
model hash, Proton/driver versions, swapchain colour space/extent, guide contract, FG policy and raw
logs. Compare CPU present duration and whole-frame median/p95 alongside confirmed GPU composition
and copy cost after equal warmup. Green host tests, an unchanged Vulkan suite, or a first-pass log
alone cannot establish this port's performance or safety.

## Is this a bad idea?

**The present host is worth pursuing. Shipping these patches verbatim is a bad idea.** The precise
barrier mismatch, invalid dummy clears, age-based retirement, stale global guide capture and missing
integration with our dispatch success/submission model are sufficient reasons to reject a straight
port even after resolving every conflict.

There is also a quality tradeoff that synchronization cannot fix: the model now sees post-processing,
HUD and possibly our menu, while engine guides describe an earlier scene. The prior path protected
UI by running before it, and this module's HUD detector was removed after it failed. Generated-frame
reprocessing compounds that mismatch. Two full-resolution copies, snapshot copies and present-side
model work can cost latency/bandwidth even with no CPU wait. No FPS or image-quality improvement is
claimed here.

Review should decide whether the restricted first slice — D3D12 SDR, fresh captured guides, one
enhancement per real frame, FG only where ordering is proved — earns those costs. If correct queue
ownership, bounded submission lifetime or usable images cannot be demonstrated, stop there. Keep
the existing upscale path as the default and leave dummy/synthesized contracts and guide generation
to the explicitly sequenced follow-up.

---

## Independent review (2026-09-12)

This note was reviewed by a second agent on a different model, read-only, against the same tree and
the same five donor commits. Its brief was adversarial: find what is wrong or missing, not what is
good. Full text at the time of writing: `agy-review-present-hook.md` in the session scratchpad.

**What it confirmed.** Every code claim above was audited against the source and none was wrong — the
merge base, the 159/33 rev counts, `bc223dae` at 10 files and 754 insertions, the marker check at
`dllmain.cpp:947-960`, the exact bypass predicate at `wrapped_swapchain.cpp:455`, the `Dispatch`
signature, the internal symbols, the suite counts. The mechanical/semantic split across the 13 diff3
regions was judged sound, with nothing filed as mechanical that is secretly semantic. All nine
DEVELOPMENT.md §1 invariants were judged satisfied by the proposed contract.

**What it found missing.** Five structural hazards the conclusion above understates. They do not
change the verdict — the host is still worth pursuing and the patches still must not ship verbatim —
but each is a thing the port has to answer, not a thing to discover in a game.

1. **Swapchain resize is an unguarded drain.** D3D12 requires every in-flight command referencing a
   backbuffer to have completed before `ResizeBuffers`. The donor's private list ring submits work
   that references backbuffers through `CopyResource`, and neither `WrappedIDXGISwapChain4::ResizeBuffers`
   (`wrapped_swapchain.cpp:665`) nor `FGHooks::hkFGRelease` (`FG_Hooks.cpp:241`) waits on
   `g_presentList.fence`. Alt+Enter or a resolution change during a pass is `DXGI_ERROR_INVALID_CALL`
   and device loss. A mandatory fence drain inside `ResizeBuffers` and `CleanupRenderTarget` is part
   of the port, not an optimisation.

2. **The present host's state is global, and swapchains are not.** `g_presentList`, `g_bbCopy`,
   `g_temporal` and `g_compose` are file statics. A game with a second swapchain — a launcher, a tool
   window, a secondary viewport — interleaves calls from both into one four-slot ring: mismatched
   copies, index collisions, crashes. The state has to hang off the swapchain instance.

3. **`ALLOW_UNORDERED_ACCESS` is not free on a backbuffer format.** `CreateScratch`
   (`DlssNr_Dx12.cpp:1331`) hardcodes the flag. Swapchains are commonly `B8G8R8A8_UNORM`, and on
   drivers without typed UAV load for it `CreateCommittedResource` returns `E_INVALIDARG`, which
   latches `g_nr.failed = true` permanently. The port needs format validation or an `R8G8B8A8_UNORM`
   cast, decided before the first allocation.

4. **Queue identity is assumed, not established.** `menu_overlay_dx.cpp:490` submits the overlay's
   list on `currentSCCommandQueue`; the donor's present hook uses the `cq` it gets from the device.
   Where those differ, the menu's backbuffer transitions and the NR transitions are on two queues with
   no fence between them.

5. **Guides may be captured on another queue.** `EvaluateAfterUpscale` snapshots depth and motion on
   the game's upscale list, which can be async compute; `RunPresentPass` reads them on the present
   queue. Without a cross-queue barrier and fence that is a read-after-write hazard — and the
   `vulkan-overlay` suite now runs with synchronization validation on, so this class of mistake is
   catchable rather than mysterious.

**And one thing synchronization cannot fix**, which sharpens the tradeoff already named above: without
a HUD-less backbuffer the model will denoise text, crosshairs and damage numbers. That is not a bug to
be fixed later; it is a property of running at present time, and either a HUD mask exists or the mode
is only honest for content without a HUD.

**One correction to the placement discussion.** Moving `RunPresentPass` ahead of `MenuOverlayDx::Present`
is not only about whether the model sees our menu — it changes the arrival state. Before the overlay,
the backbuffer arrives in `PRESENT`, is processed, and is restored to `PRESENT`, after which the
overlay does its own clean `PRESENT -> RENDER_TARGET -> PRESENT`. After the overlay, the donor's
arrival assumption does not hold against this tree's overlay, which leaves the backbuffer in `PRESENT`.


---

# Update, 2026-09-13: the feedback loop is solved upstream, and the first slice has to change

Re-audited against `150b49d0`, fifteen commits after this note was written, with a second external
survey. Three things changed: a hazard that could have ended the approach was closed by the donor, a
new use case moved the target, and the pass finally has a measured cost.

## The colour feedback loop has a fix upstream, and it is not a barrier

Between this note and now, the donor spent four commits on a symptom that would have sunk the host:
DLSS-NR at present time "compounding then snapping", the enhancement accumulating frame over frame
until it blew out. `4bb376e5` and `276a2e13` are instrumentation, `b24389ca` adds a functional guard by `renderSeq` as
well as logging, and **`afad5195` is the fix**, dated 2026-09-13.

The diagnosis is worth reading because it exonerates the obvious suspect. With a probe withholding
the write-back to the backbuffer the symptom vanished completely, and the fingerprinting was clean —
fresh render each run, alternating physical buffers, stateless model, one evaluate per base frame.
So it was never the model's temporal accumulator. In the donor's words:

> the pass submits its GPU work on the presenting queue but never waits, so under a
> frame-generation interposer that paces the client queue the write can retire after the flip
> consumed the buffer and land on a later frame's content, where the next pass then re-reads it.

The fix is `DlssNr.PresentSync`, default on, 41 lines across three files: after `SubmitPresentList`
the present thread waits on the slot fence before the hook returns, so the flip reads a fully written
buffer. Two qualifications, because the first draft of this section overstated both: it does **not**
block every frame — it enters the wait only when the fence still reads pending — and the CPU time it
costs is not "the price of NR", since it can absorb earlier work already queued. The timeout bounds
that one wait; it does not prove the absence of stalls.

It is the same kind of synchronisation, in nearly the same position, that `555ed6db` ("stop stalling
the present thread") had removed, and it supersedes that no-wait policy while enabled — the donor's
tree currently carries both, with `SubmitPresentList` still commenting that the thread must not
block while `RunPresentPass` blocks it. That is a real question for **integration**, and it is not a
reason to require the wait in step 1 below, which has no frame generation for the write to race.

Two agents disagreed about this and one was working from a stale fetch. Verified directly:
`git fetch scottmudge` brought `276a2e13..afad5195`, and the commit is real. Worth recording because
the disagreement was the load-bearing question, and the resolution was a fetch, not an argument.

## The target moved: emulators, and the first slice does not reach them

The question that prompted this re-audit was whether the neural pass can run in emulators — PCSX2,
RPCS3, Dolphin, xenia. It cannot today, and the reason is structural: every dispatch originates in
`inputs/NVNGX_DLSS_*.cpp` or the `IFeature_*wDx12.cpp` bridges, so **something must call an upscaler**.
Emulators call none. The few with FSR have FSR 1, which is spatial and never reaches this path.

This note's first slice — "D3D12 SDR, fresh captured guides, one enhancement per real frame" —
**does not cover that case**, and the re-audit was explicit that it does not cover it by accident
either: the slice deliberately waits for guides rather than falling back to zeroes, so implemented
literally it would still produce nothing in an emulator. The slice has to split:

1. **Technical proof.** Explicit D3D12 SDR host, no FG, no Auto, one selected swapchain, independent
   NGX initialisation, and **its own zeroed guides** — depth `R32_FLOAT` and motion `R16G16_FLOAT`,
   cleared once with valid descriptors and actually submitted. Confidence is not allocated and not
   passed to NGX. This proves initialisation, transport, completion and what the image looks like.
2. **First playable mode.** Reconstructed motion from consecutive frames, plus temporal and HUD
   quality criteria. This is the sequence already corrected in
   [nr-without-game-dlss.md](nr-without-game-dlss.md) — zero guides are bring-up, not a mode to put
   in front of a player.

Step 1 removes this note's queue-ownership conflict over guide capture entirely: no engine-guide
capture, no per-frame snapshots, no dependency between the queue producing guides and the queue
consuming them. That is a specific simplification and should not be read as the slice being cheaper
overall — it brings its own work in cold NGX initialisation, a synthesized contract, and descriptors
and clears that have to be proven submitted.

### Two prerequisites the original slice did not need

**Cold NGX initialisation is unproven.** `EnsureCapabilityParams` calls `NVNGXProxy::InitDx12`
(`shaders/dlssnr/DlssNr_Dx12.cpp:711`, `proxies/NVNGX_Proxy.h:777`), which uses metadata normally
filled in by the game's own NGX init. In an emulator there is no such init. Nothing demonstrates the
defaults suffice; find out before building on it.

**A Present is not evidence of a new frame.** Under pause, frame limiting or a repeated display the
hook *may* fire on content the pass already modified — none of that has been observed here, and an
emulator may equally stop presenting or redraw a clean image each time. Presentation identity alone
does not settle it either: a new presentation number does not prove the content was rewritten. For
step 1 a controlled source that writes a known clean image before each evaluation is enough; a
general contract for when it is safe to reprocess an image belongs to integration.

### What a DXGI D3D12 host actually reaches

Less than "emulators" suggests, and the note should not have implied otherwise:

| emulator / backend | reach |
|---|---|
| PCSX2, Dolphin, xenia on D3D12 | candidates for this host |
| RPCS3 on Vulkan/OpenGL | **outside it** — needs a different presentation point |
| any **native Linux** build | **never loads this Windows DLL at all**, whatever it presents with |

Most modern emulators prefer Vulkan or OpenGL. On this workstation the practical question is whether
the *Windows* build runs acceptably under Proton, which is a separate investigation.

## The target exists: measured in Xenia, 2026-09-13

The re-audit above says a DXGI D3D12 host reaches PCSX2, Dolphin and xenia, and that xenia is the
strongest case. That was research; this is a run on this workstation.

xenia-canary (Windows build, `canary_experimental@44cb87328`) under Proton Experimental, with this
fork's `OptiScaler.dll` installed as `dxgi.dll` and the full NR kit beside it. Its own log:

```
CheckWorkingMode OptiScaler working as dxgi.dll, system dll loaded
VulkanSpoofing::hkvkCreateInstance Skipping because DXVK/VKD3D is creating a D3D device
WrappedIDXGISwapChain4::WrappedIDXGISwapChain4 1 created, real: 10A8210, refCount: 1
DlssNr::ExposureScan::NoteResource DLSS-NR scan near-miss #1: UAV dim 1 10485760x1x1
```

Window up at 3396x1356, **zero errors**. So: we load, xenia really is on D3D12 through vkd3d-proton,
**our swapchain wrapper is installed on its swapchain** — the exact object a present host would work
on — and the resource tracker is already observing its resources. The pass does not run, because
nothing calls an upscaler, which is the whole reason this note exists.

Two corrections to the research this replaces:

- **A native Linux build does exist.** `xenia_canary_linux.AppImage` ships in the same release, dated
  the same day. The claim that 100% of Linux users run the Windows build does not hold. The weaker
  form survives — xenia's D3D12 backend is the maintained one and its Vulkan backend is experimental,
  so someone wanting the good backend still runs the Windows build — but that is now an inference,
  not a fact, and nobody has checked what the AppImage actually uses.
- **The overlay took the Vulkan path** (`CreateVulkanObjects swapchain image sharing mode EXCLUSIVE`),
  because the scratch directory has no `optiscaler_skip_vulkan_hooks` marker. Under vkd3d that marker
  is this fork's own Linux patch and every deployed game carries it. Any xenia work needs it too.

Environment kept at `~/Games/xenia-nr-test` — emulator, NR kit, its own Proton prefix, minimal ini.
It is a scratch target, not a deployment.

## Cost, now that the pass has been measured

[model-cost-vs-working-scale.md](model-cost-vs-working-scale.md) measured the inference at **2.90 ms
at 0.50x and 6.36 ms at 1.00x** (the fit `1.93 + 0.886/Mpx` describes the trend, it does not produce
those two numbers exactly). The model's own share of the pass falls with scale: ~96.9% at 1.00x,
93.4% at 0.50x, ~90.2% at 0.25x.

Moving that inference to present time does not make it cheaper. It adds transport, and the
re-audit quantified what this note described only as "two full-resolution copies":

| operation | size | queue |
|---|---|---|
| backbuffer -> `g_bbCopy` | 1 `CopyResource`, full resolution and format | private DIRECT list, on the queue `RunPresentPass` receives |
| encode, reduction, model, composition | NR pipeline, model at working scale | same list |
| `g_bbCopy` -> backbuffer | 1 `CopyResource`, full resolution | same list |
| typeless guide clones | up to two more copies, at guide dimensions | inside `Dispatch`, same list |

At 3440x1440 and 4 bytes per pixel one image is 19.81 MB, so the two outer copies move **39.63 MB of
payload per processed present** — 79.26 MB of logical accesses counting read and write, which is not
a measurement of physical VRAM traffic. **Lowering the working scale does not shrink them**: they are
full resolution whatever the model runs at. That is a fixed *volume* for a given resolution and
format, not a floor in milliseconds — nothing here measured its cost — and it is not counted in the
`outside_model_ms` measured on the after-upscale route.

Note also that the donor's capture takes **no** snapshot copies — it AddRefs and records metadata
(`555ed6db:shaders/dlssnr/DlssNr_Dx12.cpp:1702`). The safe snapshot copies this note proposes are
therefore additional work the port introduces, not something already implemented.

### The 4x interaction, which is new since this note

MFG at 4x is now measured working (see [mfg-count-override.md](mfg-count-override.md)). If the host
processed every presentation of a 4x cycle that is **four inferences per real frame** — 11.60 ms at
0.50x, 25.44 ms at 1.00x, before anything else. Those are conditional sums of inference measurements
taken elsewhere, not frame latency measured at present time. It has not been verified that all four
presentations traverse the hook, and traversing it would not be enough: they would also have to pass
the guards. The donor already refuses a captured render sequence it has processed
(`afad5195:shaders/dlssnr/DlssNr_Dx12.cpp:3181`), though that guard is global, depends on
`g_temporal.valid`, and provides no identity for the no-guides case. The arithmetic is a risk of
getting integration wrong, not a description of the donor as it runs today.

### A config trap worth catching in the menu

This note correctly says to clear `AfterRayReconstruction` in the new host. That also changes which
key selects the working scale — from `DlssNrRRWorkingScale`, default **0.5**, to `DlssNrWorkingScale`,
default **1.0** (`shaders/dlssnr/DlssNr_Dx12.cpp:2344`). Migrating a user from the after-RR route to
the present host would silently quadruple the model's pixel area — roughly doubling its work, taking
the after-RR measurements as the reference, since nothing has been measured on the new host. The menu
has to make that visible.

**This is not a hypothetical about a future host: it happened, without one.** A Cyberpunk session on
2026-09-13 ran with `RRWorkingScale=0.50` in the ini and produced 209 timing samples at
`working_scale=1`, because the game was no longer running Ray Reconstruction and the route had moved
to `after`, where `WorkingScale` governs. Nothing on screen said so; the stage had to be read out of
the log. The menu now greys the RR controls and names the governing control when the route is not
theirs, mirroring what the after-upscale slider already did in the opposite case. Whatever the present
host does about route selection, it inherits this: **two similar-looking controls, one of which is
inert, and the route decides which** — not the checkbox next to them.

## Two answers from outside that this note wanted

**HUD protection is still unanswered — a claim that it was already solved did not survive checking.**
An external survey reported that `DLSSNR.UseAutoMask = 1` is the model's own HUD classifier,
"independently confirmed" in three projects. It is not supported. This tree presents that key as
**"Auto skin mask"** (`dlssnr/DlssNr_Menu.cpp:254,262`), declared beside `DlssNrSkinStructure`
(`Config.h:277-278`), and Magpie and the Resolve filter both expose *Automatic Mask* and *UI
correction* as separate controls. Passing the parameter is not the same as knowing what it does. The
HUD concern raised in "Is this a bad idea?" above stands, unanswered — but see
[no-upscaler-titles.md](no-upscaler-titles.md), which found a *candidate* for it.
`DLSSNR.UICorrection` is a separate boolean from the skin mask, this tree already passes it through
every forwarder create path, and the D3D12 path hard-codes it to 1 under a comment explaining that
there is no UI in the frame on the after-upscale route. That comment stops being true here.

**Measured 2026-09-14, and the candidate failed.** `76feb2cb` put hard-edged bitmap text, a
translucent menu, thin minimap lines and a moving counter into a presented frame and compared
independently created feature-18 instances at `UICorrection` 0 and 1 over identical source and
history sequences, with repeats and runtime switches and verified parameter-block readbacks. **All
192 presented RGB8 outputs were identical between the two flag values.** The probable reason is
structural — but not the structure assumed at the time. **Corrected 2026-09-15:** `UICorrection` is a
**gate**, not an effect. A third party's probing of the contract
(`kibblerz/DLSS5-Reshade-AIO`, `lab/PRIVATE-CONTRACT-FINDINGS.md:112-127`) reports that it makes
`DLSSNR.UI`, `UIAlpha` and `Backbuffer` live, and that with it on, `UI`'s alpha is a protected-pixel
mask — alpha one bypasses NR, `Backbuffer`'s RGB supplies what is restored there, and their test
recovered the original pixels inside a rectangle while keeping NR outside it. This tree clears all
three inputs (`76feb2cb`'s own message: *"Explicitly clear the absent separate UI/alpha/backbuffer
inputs"*), so the flag had nothing to gate. The measurement was sound and the inference from it was
not.

Unverified here, and worth reproducing cheaply in the loopback harness. And even when it
reproduces it is a **mechanism, not a mask**: a present host still has to work out *where* the HUD
is. What it removes is the need to invent a protection path — one already exists and is documented.

**Our ImGui should draw after the pass**, so the model is not handed our own menu's glyphs to
denoise. That is a visual-preservation decision, not a synchronisation requirement — either order
can be made correct as long as each stage enters and leaves the backbuffer in `PRESENT`, which the
barrier discussion above is really about. The first draft of this section conflated the two.

**And the quality question has a partial answer.** Magpie's zero provider — null depth, zeroed motion —
is reported clean on static and low-motion content, with a temporary softness on fast camera motion
rather than geometric artifacts, and NVIDIA Optical Flow available as the upgrade. That is a
plausibility argument for step 1 above, not evidence about our route.

**Step 1 ran on 2026-09-14 (`dbe6894c`) and the plausibility argument held, with one caveat worth
more than the confirmation.** A real owned D3D12 swapchain carried 48/48 feature-18 evaluations over
a presented frame with zeroed guides, deterministically, with geometry intact — no smear, no
ghosting, no hallucination. Zero guides on static content degrade *safely*.

The caveat: the model's colour edit **changes direction with resolution**. On the same fixture at
1280x720 mean saturation falls 1.48%; at 960x540 it rises 1.25% (`5461a6f0`). That was found while
falsifying a different hypothesis — that our composition's OkLab calls were distorting the frame,
which `gReversibleMode=2` disproved by leaving the fading in place to within one RGB8 level. So the
edit is the model's own, and it is not a fixed transform. Whatever the present host does about
working scale, it should not assume the model's colour behaviour is stable across the scales the
user can select. Nothing here measured that on moving content, which is where it matters most.

One further scoping result from the same session, kept because it is easy to get backwards: the
concern that our composition feeds encoded sRGB into linear-light OkLab is **real in structure and
confined to `gPassthrough != 0`** — the production after-upscale route decodes to linear first and is
unaffected. Present-host work inherits that question; nothing in production does.

Magpie is GPL-3.0 and can be read as a reference. DLSS5VKLayer is AGPL-3.0 and cannot.

## Verdict, unchanged in direction

The host is still worth pursuing and the patches still must not ship verbatim. What changed is that
the worst unknown closed in the port's favour, the first slice is now cheaper and aimed at a case
that is unreachable today, and the transport cost has a number. The five hazards from the first
review stand; none was fixed incidentally by the fifteen intervening commits.

One thing this update should not be read as saying: that the approach is cleared. A second review of
this very section removed a "green light", a guarantee about HUD protection and a "measured floor",
all three of which were certainty this evidence does not support. The donor has *a* fix for the
feedback loop, running on his tree, not verified on ours. Everything downstream of that is still
design.
