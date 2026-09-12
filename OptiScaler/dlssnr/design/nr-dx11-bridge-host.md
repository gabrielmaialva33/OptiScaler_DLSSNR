# NR without an upscaler — use the existing D3D11-to-D3D12 swapchain bridge

Status: design for review, **not implemented**. Source audit against `f4b23a31` (2026-09-12),
including the independent review appended to [nr-present-hook.md](nr-present-hook.md) and the
updated step 2 in [nr-without-game-dlss.md](nr-without-game-dlss.md). Line references below are
against that baseline; paths are relative to `OptiScaler/` unless stated otherwise.

## The answer

**The three-piece hypothesis survives as an architecture, not as three small wiring changes.**
Build the existing bridge for an explicit NR reason, synthesize the zero-guide contract, and
originate evaluation from its `Present`. A native D3D12 presenter already exists as a fallback in
the bridge's factory paths; no FG object or pretend DLSS evaluate should be needed to use it.
This is a plausible shorter route to D3D11 games with no upscaler than adding a second interop layer.

**The claim that this already avoids four of the reviewer's five hazards is false.** The bridge
provides useful per-instance transport resources and a producer fence, but its drain can fail open,
the surrounding NR/device/menu state remains global, it preserves BGRA, and queue identity still
depends on global state. Zero guides remove the *fifth* hazard's game-guide producer entirely in
this first slice. None of that is a runtime proof of NR working under Proton without an upscaler.

## What is already here, and what actually blocks it

`with_dx12/dx11_with_dx12_sc.cpp` is substantial existing machinery:

- `_RequestSharedBackBuffer` copies the D3D11 backbuffer description, creates a shadow texture
  with `D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE` (859), exports a handle
  (877), and opens it on D3D12 (890). `_InitInteropObjects` shares a D3D11 fence into D3D12
  (770–791). `_WaitDx11ThenDx12` signals from D3D11, flushes, and queues a D3D12 wait (931).
- `WithDx12::PrepareD3D12ForD3D11` in `with_dx12/with_dx12.cpp:48` supplies the background device
  and queue. The factory makes a hidden, game-facing D3D11 swapchain and a visible D3D12 presenter
  for the original HWND. The game's `GetBuffer` still returns the real D3D11 backbuffer (361).
- `Present:293` copies that backbuffer through the shared shadow to the D3D12 presenter, draws
  Opti's overlay for a plain presenter, and presents. Despite the comment saying otherwise, it
  **also calls the hidden D3D11 chain's `Present(0, Flags)`** at 344 before the visible present.
  Both calls and their outcomes matter to buffer rotation and continuity.

There are two related bridges, not one identical code path. `upscalers/IFeature_Dx11wDx12.cpp`
uses the shared D3D12 foundation to run a D3D12 upscaler and calls `EvaluateAfterUpscale` at 471.
That is evidence of production interop and NR integration for FSR/XeSS-style upscaler paths.
`Dx11wDx12SC` is the separate swapchain/FG transport. Success in the first does not validate every
lifetime or presentation assumption in the second.

The enclosing factory predicate is more than `activeFgInput == FGInput::Upscaler`: it also
requires an enabled FG output and respects `_skipFGSwapChainCreation`. The four construction
sites are `hooks/DxgiFactory_Hooks.cpp:465,860` and
`hooks/DxgiFactory_WrappedCalls.cpp:309,701`; the latter uses
`ShouldCreateDx11wDx12Swapchain`. Cover both legacy `CreateSwapChain` and `CreateSwapChainForHwnd`,
through both hooked and wrapped factories. These states are config-driven; technically a game
without an upscaler could force the predicate through configuration. That would not create an
upscaler caller or make bogus FG state an appropriate NR activation mechanism.

The reported Divinity run is consistent with the ordinary D3D11 fallback: device capture, failure
to obtain a D3D12 queue from the D3D11 `pDevice`, then no upscaler/NR activity. The log is user-provided
evidence, not a run reproduced for this note. That message does not demonstrate that shared-handle
interop or feature 18 failed; neither was reached on the reported path.

Opening the gate alone still fails. `Present` requires `_fgSwapChain`, even if it is actually a
plain presenter. `_WaitForInteropCopyOnPresentQueue:1087` returns false when `_fg == nullptr`, and
`Present` turns that into device removed. The plain factory fallback is therefore not a complete
no-FG mode today. Also, the normal wrapper updates `State::frameCount`, while this wrapper's
`Present` does not. NR's chain scheduler reads that global (`DlssNr_Dx12.cpp:2052`); simply calling
it here can suppress subsequent chain evaluations when the observed frame never advances.

## The smallest useful host

Add an explicit **NR-only mode** to the existing swapchain bridge, using its plain D3D12 presenter
path directly. Keep the existing FG eligibility expression and behavior intact. Conceptually the
extra reason is `DlssNrEnabled && DlssNrDx11BridgeHost && eligible D3D11 swapchain && no FG`, under
the existing factory recursion guards. Do not set `activeFgInput`, invent `currentFG`, load an FG
backend to obtain a queue, or call DLSS SR as a bootstrap side effect.

Eligibility for the first slice is a normal Win32 HWND, single-sample SDR colour, a shareable
D3D11 device/context with the required interfaces, a compatible D3D12 DIRECT queue, and successful
preflight of the format/transport. Support the ordinary legacy and flip routes used by older games;
do not make a modern flip-only harness the definition of success. MSAA, HDR, CoreWindow and
special FG/DLSSG routes need explicit rejection before publishing the bridge. The existing factory
helpers already reject MSAA and promote the visible presenter to flip presentation with at least
two buffers; that does not change the hidden source's buffer count or swap effect.

Use one admitted NR presentation owner initially. Keep its resources, source and destination
indices, frame serial, generations, fences, and retained COM device/queue references on the wrapper.
Other swapchains keep their ordinary path and report why they were not admitted. Hold a common
NR ownership token across all NR entry points, including the existing after-upscale path and the
future D3D12 present host. A game can create an upscaler *later*; absence of `currentFeature` during
factory creation is not proof that it never will. Do not let that later callback evaluate the same
global NR context a second time or replace its device behind the bridge.

This bounded ownership policy avoids a full multi-device NR refactor in the first slice. It must
also prevent a secondary window from replacing the active menu/device state or clearing the owner's
interop state on release. Without that isolation, “only one owner” is just a label. If those guards
cannot be made local to the opt-in mode, per-owner NR and overlay contexts become prerequisite work.
The cached hidden HWND in `misc/HiddenWindow.h` is not wrapper-owned either: give the NR-only
candidate an explicit window lifetime, including partial construction cleanup, rather than destroying
a cached window another bridge may still use. Serialize owner admission, resize and teardown;
per-instance vectors alone do not make concurrent presents safe.

Build the topology at swapchain creation, not with a live menu toggle that replaces a game's
swapchain in flight. An NR failure after publication may leave the healthy transport running with
an ordinary scene copy; it cannot restore visibility by presenting only the hidden D3D11 chain.
Pre-publication failure must clean up the partial candidate and use the existing ordinary visible
swapchain path. Device/transport failure after publication returns the appropriate failure and
retains unsafe-to-free resources; it is not a successful neural or fallback frame.

## The five hazards, tested against this bridge

| Reviewer hazard | What the existing code establishes | What this port still has to establish |
|---|---|---|
| Resize drain | Per-instance copy fence and `_WaitForCopyQueueIdle` exist. | `ResizeBuffers:399` and `ResizeBuffers1:607` warn and continue on a failed wait. `_ReleaseInteropObjects:1165` ignores its result; the destructor cleans overlay targets before that wait. Saved copy fence values do not cover later overlay work. Drain all relevant work before cleanup or resize; failed drain must not free it. **Not avoided.** |
| Globals versus multiple swapchains | Shared shadows, opened resources, allocators and fence values are wrapper members. | `WithDx12` device/queue, `State::current*`, FG presenter markers, overlay state, and NR's `g_nr`/`g_compose` remain shared. `Release:228` can clear interop state and affect global FG state. A new shader object alone does not isolate its file statics. Enforce the admitted-owner contract or refactor them. **Only transport ownership is already local.** |
| BGRA plus UAV | The shared shadow keeps the source format and bind flags; the presenter copy is a same-format `CopyResource` (1051). | Nothing converts BGRA into an NR-compatible UAV format. `DlssNr_Dx12.cpp:1331` still hardcodes UAV scratch allocation using the target format. Introduce validated RGBA working colour and an actual format conversion. **Not avoided.** |
| Queue identity | Bridge copies use `_dx12CommandQueue`, also passed to `MenuOverlayDx::Present`. This is a much better starting point. | `PrepareD3D12ForD3D11` can reuse a global device/queue before checking the source adapter. The wrapper borrows those pointers. Current present synchronization obtains a different queue through `_fg`. Bind and retain one compatible DIRECT queue for the plain presenter, NR, conversion and menu; check canonical identity. **Simplified, not already guaranteed.** |
| Captured guides from another queue | The bridge has a D3D11 producer signal/flush and a D3D12 consumer wait for colour. | With immutable zero guides initialized on the owned D3D12 queue there is no game-guide capture queue to synchronize. Still order guide initialization before NR and fence colour transport. Real guides later reintroduce this requirement. **Eliminated for the zero-guide slice only.** |

There is another concrete synchronization problem: the current `Present` queues the D3D11 write
to a shadow **before** `_CopyDx11SharedToDx12FGBackBuffer` waits for that slot's previous D3D12
copy allocator (1015). The forward D3D11-to-D3D12 fence does not stop D3D11 overwriting a shadow
the previous D3D12 read still uses. Acquire a completed slot before the next D3D11 `CopyResource`,
or add a reverse GPU dependency. A CPU allocator wait later in the function is too late.

The fence helpers need failure semantics as well as happy-path waits. After Execute, a failed
Signal leaves submitted work outside the recorded fence values. A missing fence/event is currently
treated as idle, and `GetCompletedValue() == UINT64_MAX` would pass the ordinary comparison even
though it means device removal ([D3D12 fence contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue)).
Quarantine an unprovably complete generation; do not release it on timeout, signal failure, or
device loss as if the fence completed. A resize drain must include source writes and all D3D12
users, not only the last successfully signalled copy list.

Resize also needs correct indices and partial-failure handling. `_RefreshCachedSwapchainDesc:1195`
keeps `_currentFakeIndex` unchanged for any nonzero count: shrinking from three buffers with index
two to two buffers can index past the new vectors. Separate legacy source index rules (including
DISCARD's buffer zero), transport ring slots, and the visible presenter's current index. Drain and
retire the old generation, resize both chains, reset indices, then publish the new generation only
when both agree. If the hidden resize succeeds and visible resize fails, do not continue with stale
NR extents or claim the operation was transactional.

## Zero guidance, colour and history

Use the step-2 contract, not a fabricated engine frame. The resources are owned by the host's
device/extent generation and shared across its sequential NR evaluations:

| Resource / field | Contract in this host |
|---|---|
| Colour and output | Full source extent, SDR, explicit typed colour conversion to validated `R8G8B8A8_UNORM` working storage; compose in place there, then write to the visible presenter. No game exposure texture. |
| Depth | `R32_FLOAT`, full source extent, permanently zero; bind as `DLSSNR.Depth`, with `DepthInverted = true`. |
| Motion | `R16G16_FLOAT`, full source extent, permanently zero; bind as `DLSSNR.MVec`. Semantics are CurrentToPrevious, SourcePixels, RelativeInverse; unit scale, no low-resolution guides, zero subrect origins. |
| Confidence | `R8_UNORM` zero if a local blending consumer needs it. Never pass it to NGX. This host has no such consumer, so omit the unused allocation. |
| Colour metadata | `ColourIsLinearHdr = false`, no RR, no engine exposure, pre-exposure one, explicit extents and resource states. Do not inherit the struct's linear-HDR default. |

The donor/reference uses D3D11 SRV/UAV resources, but these new guides have no D3D11 producer.
Allocate them directly on D3D12 with UAV/SRV access; sharing zero textures across APIs buys nothing.
Clear each newly allocated guide **once**, then keep it read-only until retired. Record valid
CPU/GPU UAV descriptors, bind the descriptor heap, and explicitly order clear-to-read transitions;
`ClearUnorderedAccessViewFloat` is not an implicit synchronization shortcut
([D3D12 clear contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearunorderedaccessviewfloat)).
An abandoned, unsubmitted initialization must not mark the guide initialized. Resetting model
history does not require re-clearing an immutable zero guide.

`GuidanceMode` has exactly the step-2 values: 0 available, 1 force zero, 2 motion only with zero
depth, 3 depth only with zero motion. With no real guide providers all four resolve to zeros;
status must say so. There is no confidence NGX parameter, invented depth heuristic, optical flow,
or synthetic jitter in this slice. The current `DlssNrFrameInfo` and forwarder evaluate contract
do not supply a jitter field; importing a reference project's Halton sequence without a matching
colour sampling contract would be wrong.

History reset must cover initialize, resize, scene change, capture interruption, device recreation
and long pause, as step 2 requires. Initialize/resize/device generations and skipped/occluded/failed
presents are observable here; an engine scene-cut notification is not. **Proposed minimum for review:
reset every evaluated frame in the first zero-guide slice.** That conservatively covers unobservable
scene changes without a new image-analysis pass, but gives up temporal accumulation and is a quality
limitation, not a claim of parity with a host that detects cuts. Persistent history needs a reviewed
content-discontinuity detector and pause threshold before it is enabled; silently resetting only on
resize would not satisfy step 2. A disabled/unsupported/interrupted host must not reuse old temporal
state as a continuous frame stream when it resumes. Keep guide generation separate from model-history
reset so this conservative policy does not allocate or clear every frame.

Do not request UAV access on the imported BGRA backbuffer, write through its read-only shared handle,
or rely on an RGBA “cast”. `CopyResource` only supports compatible format groups; it does not do a
BGRA-to-RGBA channel conversion ([copy contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copyresource)).
Validate format support before NGX/scratch creation. Use a typed shader read into RGBA working
colour (a readable intermediate if the source bind flags require it), and an RTV blit back into the
original-format visible presenter. Preserve channel order, alpha and SDR transfer without accidental
extra sRGB decoding. `shaders/format_transfer/FT_Dx12` is useful precedent, not a drop-in solution:
it writes a UAV and its shader forces alpha to one. Merely deferring BGRA would exclude a common
format in the very D3D11 games this host is for.

## Who originates the evaluate, and on what timeline

The NR-only `Dx11wDx12SC::Present` is the caller the game never supplied. Factor the D3D12 NR
composition entry so this caller supplies colour/output, zero guides, resource states, host frame
serial and submission ownership directly. Do not manufacture an upscaler object or enter an
`EvaluateAfterUpscale` path that expects its state. Keep a monotonic serial per admitted owner;
thread it into scheduling and timing rather than silently assigning the process-wide frame counter.

For an ordinary accepted present, the required sequence is:

1. Exclude `DXGI_PRESENT_TEST`; acquire a safe transport slot **before** D3D11 writes it. The
   previous D3D12 read must have completed. Check adapter LUID, device and DIRECT queue ownership
   when constructing the generation; retain references instead of borrowing mutable globals.
2. Copy the game's D3D11 colour to its shared shadow, signal the shared D3D11 fence and flush.
   Queue the wait on the one owned D3D12 queue before reading that shadow.
3. Record the colour transfer, any first-generation guide clear, NR composition and output
   conversion. Imported resources enter/leave their known interop state (currently COMMON);
   the visible backbuffer enters and leaves PRESENT. Working colour supplies its explicit state
   to `Dispatch`; it is not a fabricated RENDER_TARGET backbuffer assumption.
4. Close and execute on that same queue. Retain this fork's bool dispatch result, recording lease,
   submission tracking and deferred resource parking. A skipped NR pass still needs an ordinary
   colour transfer. A false result can follow setup commands: submit and fence valid initialization
   or explicitly abandon its CPU bookkeeping, never publish unexecuted initialization as complete.
5. Draw Opti's menu after NR on that queue. Signal completion covering NR, source reads, conversion
   and overlay; use appropriate queue-tail coverage again when draining for resize/teardown.
   Then preserve the hidden-source and visible-presenter presentation sequence, tracking their
   outcomes separately. Do not double-enter the ordinary wrapper or FG present hook.

Same-queue submission orders the visible work before the flip without a per-frame CPU wait for
NR completion. It does not make slot reuse or resize free. Use completion checks/backpressure at
reuse and a checked drain at resize. If NR's global composition context is busy, skip NR rather
than reset in-flight resources; transport still needs a safe slot to display the original frame.
The initial single-owner implementation can keep only one NR composition recording outstanding.
It must opt into recording tracking even for a single pass, and seal/reset the completed old
recording before reusing shared composition state; rotating to another list alone does not seal it.
Do not enable multi-pass artificially to activate that safety mechanism.

This is where the donor's known failures remain relevant: waiting for every neural pass stalls the
present thread; failing to order it before presentation allows a flip to overtake it; a dropped list
needs a legal close/reset/abandon path; old temporals must survive until their actual consumer is
complete. The bridge does not repeal any of those obligations.

There is a further cold-start prerequisite. `DlssNr_Dx12.cpp:711` obtains core capability parameters
through `NVNGXProxy::InitDx12`, which uses `State` metadata. With no game NGX init, the defaults are
application ID 1337, an empty data path and a zero-initialized SDK version (`State.h:219`). Source
inspection cannot establish that this is a valid standalone bootstrap. Give the host an explicitly
owned initialization path with valid parameters and matching teardown, using the existing forwarder
ABI and consumer guard. Do not overwrite another consumer's NGX state or shut its core down.
The feature-18 snippet initialization in `forwarder/dlssnr_forwarder.cpp:838` is distinct from core
capability initialization; a loaded DLL or a successful outer evaluate is not proof the model ran.
The first real harness must start with **no game NGX metadata or calls** and prove create/evaluate.

## Linux routing and the other present host

Preserve `dllmain.cpp`'s adjacent `optiscaler_skip_vulkan_hooks` marker and `State::vulkanHooksSkipped`,
and the `wrapped_swapchain.cpp:455` bypass of the DXVK direct-present shortcut when hooks were
skipped. Neither file is permission to change the marker, re-enable Vulkan hooks, or alter the
ordinary off path. This bridge presents through D3D12 under vkd3d-proton even though the game renders
through D3D11/DXVK; it must select the D3D12 menu backend for that topology.

That selection is not automatic today. `menu_overlay_vk.cpp:192` tests `swapchainApi == DX12` to
defer to the D3D overlay. The bridge sets `swapchainInteropApi`, not the normal wrapper's
`swapchainApi`; `menu_overlay_dx.cpp` can yield when Vulkan already owns the menu. Establish overlay
ownership before translated Vulkan swapchain/present activity can claim it, for both hidden and
visible chains. Keep the game's rendering API truthful and make ownership specific to the admitted
presenter. Verify the menu uses the canonical queue passed by the bridge, and that secondary-window
cleanup cannot free its targets. Test with the Vulkan-hook marker both present and absent.

The D3D12 no-upscaler host remains the job of [nr-present-hook.md](nr-present-hook.md) and the
`bc223dae` port. The two hosts should share the zero-guide builder, cold NGX bootstrap, NR ownership
and dispatch contract, colour policy, config semantics, coverage and timing. Their transport differs:
the D3D12 host uses the game's present queue/backbuffer; this host first imports a D3D11 frame into
an owned D3D12 presenter. Neither should call the other or dispatch twice on that presenter.
Both need the five-hazard review. Sharing a contract does not require first landing the entire
donor host: the D3D11 bridge can be the first implementation of the no-upscaler caller.

This qualifies the older sequencing note's statement that the reference interop “does not” carry
over: a suitable interop foundation already exists here on a background D3D12 device. It does not
make this a native Vulkan NR implementation, nor require reconstructing guides before either host
can run. Feeding NR on a hidden-output/shared-return D3D11 path instead is possible, but would add
a writable output share and reverse synchronization; it is not the smallest reuse of this existing
visible D3D12 presenter architecture.

## Controls and invariants

Propose `[DlssNr] Dx11BridgeHost=false` (`DlssNrDx11BridgeHost`) as a startup transport opt-in,
in addition to the existing NR `Enabled` master. `GuidanceMode=0` (`DlssNrGuidanceMode`, values
0–3 above) is shared by both future no-upscaler hosts; reuse it if step 2 lands first. It does not
enable either host. `Dx11BridgeHost` is not another before/after setting: existing `Stage` selects
an upscaler boundary, which does not exist here, and the proposed D3D12 `HookMethod` must not
become a competing D3D11 bridge selector. Show the effective host as “D3D11 bridge, at present”.

| DEVELOPMENT.md §1 | Requirement for this port |
|---|---|
| 1. Default-identical | With the new key false, or NR disabled at startup, take exactly today's factory branches: no new device, shadow, guide, hook effect or NR bootstrap. Old FG bridge behavior remains available unchanged. After an explicit bridge opt-in, turning NR off live stops evaluations; the published transport remains until recreation/restart. Document this distinction, rather than promising an impossible live unwrapping. |
| 2. No dead control | A startup option says “requires restart”; runtime status distinguishes inactive, unsupported, warming, ran and skipped with a reason. No live bridge toggle. Hide upscaler-stage and unavailable guide-provider controls in this host. With no providers, report “zero depth and motion”; do not show four apparently different live guide choices that all do nothing. Preserve the four-value persisted contract for future providers. |
| 3. One quantity, one control | Reuse `Enabled`, strength, working scale, pass settings and compare controls; no separate bridge strength/scale or depth/motion booleans. Do not repurpose `Stage` or mutate its persisted value to force dispatch. |
| 4. Four-point round-trip | For **each** of `Dx11BridgeHost` (bool, false) and `GuidanceMode` (integer, 0), add the `Config.h` default, `Config.cpp` reload reader, `Config.cpp` save writer and shipped `OptiScaler.ini` entry. Validate out-of-range modes to 0; document startup semantics and values in `Config.md`. |
| 5. Struct/cbuffer | No new shared composition constants are required for zero resources or host identity. If implementation changes them, append identical ordered 4-byte scalars to C++ and HLSL. Keep conversion-specific constants separate. |
| 6. Shader rebuild | Any `dlssnr.hlsl` change rebuilds both DXIL and SPIR-V and checks freshness. New D3D12 conversion shaders need their real generated artifact/build integration too; an existing format-transfer shader is not proof of colour correctness. |
| 7. Passthrough | SDR is already tone-mapped. Supply the metadata that selects `gPassthrough` and preserve the gate in encode, decode and every reproduced encode including `fullProxy`/matched residual. Passthrough bypasses HDR proxy transforms, not the requested neural edit. Skip HDR exposure scans for this host; no fake game exposure or double tonemap. |
| 8. Vulkan lifetime | No new native Vulkan NR resources. Preserve overlay live-device drain/dead-device abandon behavior. D3D resources translated by Proton still need the checked queue ownership/drains above; translation is not a lifetime exemption. |
| 9. Local until asked | Design and eventual implementation remain local; no push in this task. |

Apply DEVELOPMENT §3 by hand or independent review before implementation is called ready: config
four-point audit, full Colour/White-point-source state-table walk for any menu touch, shader
layout/passthrough/numerical checks for any shader touch, overlay lifetime review, and non-NR-path
inertness at all four factories. Existing controls whose inputs do not exist must explain the
effective fallback or be hidden according to that table; do not silently reinterpret them.

## What would prove it

This task is a static design audit, not a build, GPU experiment or game validation. The existing
fourteen suites are indexed in [tests/README.md](../../../tests/README.md) and guarded by
[tests/suites.toml](../../../tests/suites.toml): eleven host suites and three serial Wine suites.
None currently proves this cold, no-upscaler D3D11 swapchain path.

1. **First falsification test: cold bridge plus real NR.** Add a small D3D11 HWND harness that never
   imports/calls NGX or any upscaler SDK and has no FG object. Through Opti's production factory and
   present paths, prove the background device, shared texture and fence, standalone capability init,
   feature-18 creation, successful neural evaluate and visible output. Use the real forwarder;
   log host/queue identity, source format, guide contract, submitted/completed evaluations and skips.
   Loading the snippet or counting `Present` calls does not pass. Establish whether Proton supports
   the exact D3D11 Device5/Context4 shared-fence/NT-handle contract on the target adapter. Exercise
   clean host-owned shutdown and restart; reject a preexisting queue on a different adapter/device
   or of the wrong type before publishing the bridge.
2. **Transport before image judgment.** With a deterministic pattern, exercise all four factory
   routes, RGBA and BGRA with distinguishable channels/alpha, legacy DISCARD/SEQUENTIAL and flip,
   unequal hidden/visible buffer counts and non-aligned extents. Check off-default equivalence,
   unsupported-mode pre-publication fallback, TEST without work, and plain copy after an NR skip.
   `Present1` currently discards its dirty rectangles/scroll parameters (500); implement their
   correct semantics or explicitly decline that capability, not silently claim full IDXGISwapChain4
   behavior. Include fullscreen/Alt+Enter, source-size changes and window occlusion.
3. **Break ownership and reuse.** Delay GPU completion until the ring wraps; verify D3D11 cannot
   overwrite a shadow before D3D12's final read. Resize while NR/menu work is pending, including
   three-to-two buffers with old index two. Inject reset/close/signal/wait/open-handle failures,
   setup-only recordings, partial two-chain resize and device removal. Assert no free or allocator
   reset on an unproved drain, no dropped open list, and no stale-generation resource use. Interleave
   a second swapchain, release it first, and attempt a later upscaler evaluate: the admitted owner
   must stay intact and evaluate at most once per its own present serial.
4. **Contract and regression tests.** Extend `nr-dispatch`, `nr-submission`, `nr-multipass`,
   `nr-pass-config`, `nr-menu` and timing/boundary tests for host state, four-value resolution,
   exact zero-guide formats, one clear per allocation, no confidence binding, reset policy,
   mandatory single-pass tracking and skip reporting. Keep `nr-before-upscale`, localization,
   timing config and `nr-shutdown` coverage green. Add new production-slice host tests where needed;
   fakes can establish ordering/decisions, not GPU validity. Register every new suite in both the
   README and `suites.toml`; run `python3 tests/run_all.py --list` to catch registry drift.
5. **Real GPU and Linux regression.** Run the serial Wine tier after builds finish:
   `nr-gpu-timing-d3d12` verifies real timestamp plumbing but never loads NGX;
   `dlssnr-loopback` covers real NGX/composition but supplies its own evaluate and is not this host;
   `vulkan-overlay` has synchronization validation enabled and must remain clean. Extend real
   coverage to the new D3D11 harness, with D3D12 debug/validation where available and translated
   Vulkan synchronization validation actually confirmed enabled. The existing overlay suite alone
   cannot catch commands it never exercises. Test Vulkan hooks skipped and enabled, with exactly
   one visible menu and no cleanup race on either translated chain.
6. **Game acceptance and cost.** Start with the reported Divinity case, then another independently
   shaped D3D11 game from the target class (Metro Redux, BioShock Remastered or Splinter Cell
   Blacklist). Record the selected renderer/API and swapchain contract rather than assuming them
   from the title. Compare default-off, transport-only, and NR-on runs using the same scene;
   include moving camera, cuts, HUD/text, pause/resume and resize. Measure present-thread waits,
   frame-time tails and GPU copy/conversion/NR time. Successful evaluations alone do not establish
   acceptable latency or image quality. Leave VRR and unrelated workstation settings unchanged.

For eventual production C++ use `/usr/lib/llvm20/bin/clang-format` (pinned 20), then the normal build
and relevant tests. This docs-only change needs neither a DLL build nor shader regeneration.
For this note, the review was a manual source walk of creation, presentation, resize, teardown,
NR dispatch and overlay ownership. The test-registry listing was checked; it is not a suite run.

## Scope and reasons to stop

Implement in reviewable slices: NR-only plain presenter and safe ownership/transport first;
cold bootstrap plus clear-once guides and colour conversion second; the actual present originator
and common NR tracking third. The transport slice is an internal test stage, not a claim that NR
works. Each slice stays behind the off-default gate. No depth selection, reconstructed motion,
optical flow, HDR, native Vulkan NR or simultaneous multi-owner NR belongs in this first delivery.

The route is worth pursuing because the hard API boundary has an existing implementation and a
natural owner. It becomes a bad shortcut if “already shipped” is used to skip its failed-drain,
shadow-reuse, global ownership or format work. A failure of cold feature-18 initialization or the
shared-fence contract under Proton would invalidate the proposed minimum until resolved. Broad
dual-swapchain API incompatibility would justify revisiting a D3D11-output interop host instead.

Finally, the model sees the game's final HUD and text; putting Opti's menu afterwards protects
only Opti's menu. Zero guidance and conservative history resets can alter motion quality as well
as style. The extra shared copy, colour conversions and second presentation path add cost and
failure modes even without NR. Those are reasons to require image and latency evidence before
general use, not reasons to postpone a small, honest no-upscaler experiment behind an explicit gate.
