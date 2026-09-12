# Present-time NR — port the host, preserve this fork's contracts

Status: **design only, awaiting review; no implementation or deployment.** Written 2026-09-12 against
`393cd0b04e80d16923969c1a9d302983318c51e9` in `gabrielmaialva33/OptiScaler_DLSSNR`.
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
