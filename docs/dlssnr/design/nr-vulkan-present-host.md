# NR at Vulkan present — one Proton host below the game's rendering API

Status: **stage 1 (coverage and no-op control), stage 2 (submission-only control: the round trip) and stage 3 (cold
NR: the model inside the round trip, on zero guides) built 2026-10-14**; stages 4-5 are design. See [Stage 1 as
built](#stage-1-as-built-2026-10-14), which also records how the tree drifted from the references below, [Stage 2 as
built](#stage-2-as-built-2026-10-14) and [Stage 3 as built](#stage-3-as-built-2026-10-14). Audited at `c3310a15`
(2026-09-12), after reading [nr-present-hook.md](nr-present-hook.md), [nr-dx11-bridge-host.md](nr-dx11-bridge-host.md), both
independent reviews, and the corrected sequencing in [nr-without-game-dlss.md](nr-without-game-dlss.md).
Source paths below are relative to `OptiScaler/`; line references describe this baseline (the NR files have
since moved to `retoque/`).

## The answer

**This is the preferred route to test first for no-upscaler games under Proton. It is not yet a
proven replacement for the other hosts.** DXVK D3D11, vkd3d-proton D3D12 and Windows Vulkan games
converge on Vulkan presentation. OptiScaler already intercepts that boundary and already records
native Vulkan NR. Using the translated device avoids the D3D11 bridge's shared handles, second
swapchain and double presentation, without adding a helper process or IPC.

**“It draws no ImGui” does not establish that it avoids the marker's failure.** Some defects were
specific to ImGui, but the implicated queue submission, semaphore takeover, resource reuse and
Streamline pacer interaction apply to any pass inserted before present. The history contains a
DLSS-G failure even after the basic queue/semaphore fixes. Keep the marker's existing protection;
design a separately opted-in NR-only hook path and prove its behavior in stages, with FG disabled
first and the recorded FG failure case as an independent acceptance gate.

“Every game” needs a scope qualification. This DLL can reach games that load it early and route
their Vulkan calls through the intercepted Windows/Wine loader. It is not a native Linux implicit
layer: Linux `vkcube` does not load a Windows OptiScaler DLL. wined3d/OpenGL, bypassed loader paths,
late injection, protected content and unsupported queues are not covered merely because Proton
often uses Vulkan. Hook coverage on DXVK and vkd3d-proton must be measured, not inferred from the
existence of `hkvkQueuePresentKHR`.

## Evidence and the licensing boundary

The user's DOOM Eternal report — model initialization on the Vulkan device and 150 evaluations —
is evidence for the existing Vulkan model path, not a cold present-host test. It was not reproduced
for this note. The code also supports the premise: `DlssNrFeature_Vk.cpp:562` records NR into a
supplied Vulkan command buffer, and `shaders/dlssnr/DlssNr_Vk.cpp` dispatches the shared composition
shader's SPIR-V. There is no need to port a D3D12 neural renderer to Vulkan to begin this route.

The public [DLSS5VKLayer README](https://github.com/bmitch87/DLSS5VKLayer/blob/main/README.md)
documents an implicit Vulkan layer with a Wine-hosted NGX helper and synthetic motion support.
Its scope includes native Linux applications as well as Proton, explaining the process boundary
that this injected Windows DLL need not cross. Architecture and observed behavior are relevant
prior art; its implementation is not a donor for this project.

Read issue #13 through its [final instrumented correction](https://github.com/bmitch87/DLSS5VKLayer/issues/13#issuecomment-5645501048),
not just the opening report: the contributor reports inference from **2.59 ms at 480×270 to
6.52 ms at 2880×1620**, using `vkcube` on an RTX 5090/Linux setup. The report retracts both the
IPC-overhead explanation and the subsequent attribution to excessive waits: `flow_gpu` measured
optical flow, not NGX. The attempted submit-ring change recovered only 0.23 ms of the expected
3.6 ms. These are external measurements, not estimates for this workstation, end-to-end host cost,
or proof of acceptable game imagery. In-process execution removes a process boundary, not inference
time or the dependency of presentation on the processed image.

Respect the requested licensing separation: this fork and its intended upstream are GPL-3.0;
DLSS5VKLayer is AGPL-3.0. No implementation files or patches from that project were opened or
copied for this design. The external inputs are its README and issue discussion; implementation
decisions below derive from our tree and the Vulkan API contract. Do not translate or transplant
their source, shaders or algorithms into the eventual port. Published measurements do not require
bringing their implementation into our licensing boundary.

## What the marker actually does

The five requested commits were read with `git show`:

| Commit | Actual change and what it proves |
|---|---|
| `255ca14` | `dllmain.cpp` checks the adjacent marker on Wine and skips `VulkanHooks::Hook`. Its comment identifies present races with Streamline DLSS-G under vkd3d-proton. It is a coarse hook-bundle bypass, not a demonstrated ImGui-only diagnosis. |
| `a4f1bbb` | The bypass also sets `State::vulkanHooksSkipped`. |
| `33f2961` | Declares that new state field, independently of `vulkanSkipHooks`. |
| `7fe23f4` | The wrapped DXGI swapchain bypasses the DXVK direct-present shortcut when `vulkanHooksSkipped` is set, allowing the D3D overlay path. Current code additionally limits the shortcut to D3D11. **Retired 2026-10-12:** upstream `1cc9d5a1` deleted the shortcut, so every DXGI present reaches the D3D overlay. |
| `10c6760` | Replaces an infinite overlay fence wait with a one-second wait and skips drawing on failure. This bounds a symptom; it neither repairs synchronization nor proves the original cause. |

There is an important correction to the premise: **the marker does not set `vulkanSkipHooks`.**
`dllmain.cpp:947–964` sets `vulkanHooksSkipped` and does not call the hook installer. The distinct
`HookDevice:56` guard uses `ScopedSkipVulkanHooks::Active()`, the scoped suppression during internal
device work (a per-thread count since 2026-10-13; before that the process-wide `vulkanSkipHooks`). Do
not use it to bypass the marker: it is a different recursion/device-capture guard.

Nor is marker presence a complete static proof that no hooks can exist: the late Vulkan load path
in `hooks/LibraryLoad_Hooks.cpp:392–401` calls `VulkanHooks::Hook(module)` without checking the
marker, and `Hook` itself does not enforce it. *(Closed 2026-09-27: the policy is resolved once,
right after Wine detection, by `dllmain.cpp` `ResolveVulkanHookPolicy` — `[Vulkan] SkipHooks`, or the
marker as a deprecated fallback — and `VulkanHooks::Hook` returns early whenever
`State::vulkanHooksSkipped` is set, so the startup and the late-load paths are one gate. On the
installs checked that day the late path had never fired under the marker: Crimson Desert's log shows
no Vulkan hook installed after the skip line.)* The reported inventory of fourteen marked games
out of fifteen explains deployment intent, but should be paired with hook-install/callback logs.
This audit did not recount the installs or remove any markers. The policy must eventually cover
both startup and late loading; any fix to this preexisting gap outside the opt-in path should be
reviewed separately from the default-identical feature change.

### What conflicted, and what remains without ImGui

`menu_overlay_vk.cpp:852` explicitly describes simultaneous game-thread and Streamline-pacer
presents. It serializes ImGui and shared frame data; teardown takes the same locks. Other current
code and the explanatory commit `8e021bff` identify the following mechanisms:

- Command pools created for one queue family were submitted through another presentation path.
  The current overlay resolves the actual presenting queue, rebuilds pools for its family, and
  declines a non-graphics family (`QueuePresent:913` onward). NR is compute, so a compute-capable
  non-graphics family may work where ImGui cannot, but a pool-family mismatch is just as invalid.
- Present wait semaphores are consumed by the overlay submit, replaced with its completion
  semaphore (`1068–1089`). Stage masks must cover every wait; a counter-based completion semaphore
  ring could reuse a semaphore while presentation still waited on it. Both apply unchanged to NR.
- Frame fences must only be considered pending after successful submission, and resources must
  survive concurrent present/teardown. A CPU mutex does not establish GPU completion.
- ImGui's global context, competing D3D/Vulkan renderer backends and menu initialization/reentry
  are UI-specific. An NR-only host need not initialize, render, shut down or borrow any of them.

Most decisively, `8e021bff` records **18 minutes without a fault, followed by another session losing
the device 623 ms after the first menu toggle**, with Streamline's 500 ms GPU-fence timeout. Its
message says the fixes were necessary but insufficient and the marker remained required. That is
a recorded observation; its attribution to pacer/semaphore takeover is a diagnosis, not a complete
trace proving that every correctly synchronized Vulkan pass must fail.

`d8275824` subsequently routes vkd3d-proton's menu through D3D12, makes the Vulkan backend stand
down, and ties the DLSS-G/menu interlock to actual overlay ownership. Current
`DxOverlayOwnsBackend:192` and the reverse guard in `menu_overlay_dx.cpp` preserve that separation.
An NR pass that never opens ImGui also never automatically benefits from an interlock keyed to
menu visibility. It cannot assume FG is suspended merely because its own UI is absent.
Conversely, `QueuePresent` returns before GPU work when `RenderMenu` produces no frame. Opening
the menu therefore changes both ImGui activity and GPU submission: a failure on menu-open alone
does not isolate which caused it.

**Adjudication:** removing ImGui removes the backend/context hazards; it does not establish that
the pacing/submission hazard disappears. A correct NR-only semaphore chain may coexist, but this
tree does not yet contain the evidence to say it does. FG-off no-upscaler bring-up can proceed as
the first experiment without making an FG compatibility claim.

## A narrow NR-only hook path, not “remove all the markers”

Propose `[DlssNr] VulkanPresentHost=false` (`DlssNrVulkanPresentHost`), a startup opt-in under the
existing `Enabled` master. It selects this host even for translated D3D games; it is not a second
strength, scale or before/after control. The opt-in is an explicit exception allowing **only the
NR infrastructure** when the legacy hook bundle was suppressed. Keep all deployed markers intact.

| Startup state | Required behavior |
|---|---|
| Host key false, or NR master false | Today's paths and marker handling unchanged. No new host records, extension/usage edits, resources, submissions or pacing effects. Existing after-upscale NR remains as before when its master is on. |
| Both true, marker present | Install the minimal NR creation/lifetime/present interception independently of the Vulkan menu. Keep D3D overlay routing and the legacy bundle suppressed. Log both decisions distinctly; do not relabel the marker as “all Vulkan hooks inactive”. |
| Both true, marker absent | NR interception remains independent of `OverlayMenu`. Existing menu backend ownership is preserved; if the Vulkan menu runs, it follows NR in one defined wait chain. |
| Missing prerequisites or late interception | Report the exact inactive reason and preserve the application's present; do not create a replacement device/swapchain to rescue this host. Restart may be necessary because enabled extensions/features are fixed at creation. |

`hkvkCreateDevice:243` currently calls `NoteDeviceQueues` and `HookDevice` only when `OverlayMenu`
is enabled. Turning the menu off therefore is not a valid test of “NR hooks without overlay” yet.
Separate infrastructure capture from UI at creation, surface capture, swapchain creation and present.
`VulkanHooks::Hook` also invokes spoofing/extension/VRAM hook installers; the minimal route must
not restore the entire suppressed bundle merely to reach NR. Preserve internal-recursion guards
and use downstream dispatch functions, not calls that re-enter our own hooks.

Track instance → physical device → device → queues, surface → HWND, and swapchain → device/images
in owned records. Current hook globals `_device`, `_PD`, `_instance`, `_hwnd` and the first-device
`o_CreateSwapchainKHR != nullptr` sentinel are not that registry. Returned proc addresses and direct
exports must both reach the wrapper, with original dispatch appropriate to the actual device.
Record `vkGetDeviceQueue`/`vkGetDeviceQueue2` results and create flags; do not infer queue family from
an opaque handle or call for a queue the application never created. Add destroy-device/swapchain
and acquire lifecycle observation, currently absent from this hook file, for generation retirement
and presentation-semaphore reuse. Multiple devices and reused handle values must not alias records.

The NR-only present callback must also avoid inheriting unrelated behavior from
`hkvkQueuePresentKHR`: global-device upscale timing reads, `TickFrozenCheck`, Reflex updates and
`FrameLimit::sleep`. Translated DXGI presentation may already perform those actions. A new host
serial counts admitted source presents; it must not add duplicate limiter/reflex/frozen ticks.

## What after-upscale evaluation currently supplies for free

Factor a Vulkan frame-recording entry out of `EvaluateAfterUpscaleVk`, with an explicit frame
contract and a result distinguishing **skipped**, **setup recorded**, **composed**, and **failed**.
The existing API is `void`; its caller's game-filled parameter block and command buffer conceal
several responsibilities that a present host has to own:

| Existing assumption | Present-host replacement |
|---|---|
| A valid recording command buffer, instance, physical device and device are passed in. | Own the command pool/buffer for the proven queue family, resolve its device/instance from records, and end/submit/fence it. |
| Output, depth and motion are already `NVSDK_NGX_Resource_VK` wrappers; any absent one causes an early return (`562–668`). | Own full image/view/format/extent/subresource descriptions and the zero-guide resources. No fake upscaler object or DLSS evaluate. |
| Output is writable storage colour in GENERAL; the pass reads and resolves into it. | Never pass a PRESENT-layout swapchain image straight into that API. Transfer to an owned, compatible working image, process there, and transfer back with explicit barriers. |
| Game create flags provide HDR, inverted depth and MV resolution; the parameter block provides subrects, MV scale, exposure and history reset (`873` onward). | Supply SDR/passthrough intent, full source extents, source-pixel MV semantics, zero origins, inverted depth, no engine exposure and host history events explicitly. |
| NGX/parameter initialization occurs in a game already using the integration. | Prove a cold start, including writable data path, shim/model load and parameter allocation, without any application NGX calls. |
| Game submission/lifetime bounds the recorded work. | Track real submission completion for features, images, descriptors, constants, readbacks and timestamps; presentation completion separately protects present semaphores. |

Cold Vulkan bootstrap is **not the D3D12 bootstrap problem copied verbatim**. This path probes the
shim's four Vulkan exports, initializes the snippet directly with SDK version `0x15`, and calls
`NVSDK_NGX_VULKAN_AllocateParameters` (`DlssNrFeature_Vk.cpp:703–758`). Our allocation export
(`inputs/NVNGX_DLSS_Vk.cpp:665`) can fall back to `NVNGX_Parameters` without a loaded driver core.
Retain the ABI check, caller shim and float-slot discovery; give the host a valid data path instead
of relying on empty `State::NVNGX_ApplicationDataPath`. Verify the fallback block against the real
snippet. Do not force a DLSS SR initialization just to populate `State`.

At device creation the existing NR extension list includes `VK_NVX_binary_import`,
`VK_NVX_image_view_handle`, `VK_KHR_push_descriptor` and `VK_KHR_buffer_device_address`. The hook
appends supported names when NR is enabled; it is not a full feature/dependency negotiation.
Verify availability through the actual Wine/Proton entry points and enable required feature bits
in a copied, non-conflicting `pNext` chain. In particular, advertising buffer-device-address is not
the same as enabling `bufferDeviceAddress`
([Khronos feature guidance](https://docs.vulkan.org/guide/latest/buffer_device_address.html)).
Account for instance/core-version requirements too: `VkExt::kInstance` lists properties2, but the
present host must prove what was actually enabled. Never alter a device's capabilities after creation.

The Vulkan implementation also has gaps that 150 recorded frames cannot discharge:

- `VkState g_vk` and the forwarder's initialized flag are global. A different live device is not
  necessarily a dead old device, despite the current `ShutdownVk(false)` assumption on a device
  change. Start with one explicitly admitted NR owner across **all** hosts and after-upscale
  entry points, or make model/composition state per owner. A secondary swapchain must not switch
  or destroy it. The forwarder initialization flag must be tied to device generation; a second
  device must not receive success because the first initialized.
  *2026-09-29:* the forwarder now tracks the device it initialised NGX on and initialises a new
  one (janblade's `8be449d5`), so the host's re-init after `ShutdownVk(false)` is real. The same
  change fixed `NVSDK_NGX_VULKAN_Init_Ext`'s argument order: the forwarder had passed the feature
  info before the SDK version, so NGX got version 0 and a feature-info "pointer" of `0x15`. The
  same-handle case (a new device reusing the old handle value) and a `Shutdown1` before
  `vkDestroyDevice` are still open: the host has no device-destroy hook to hang them on.
- `DlssNr_Vk` rotates eighteen descriptor/constant slots (`6 × 3`) on each dispatch without a
  completion test. Query/meter rings similarly assume age implies completion. Add completion-based
  reuse; do not claim a frame ring is a fence. Initially allow one NR recording outstanding and
  skip a busy frame before touching any present wait, with enough slots for that recording.
- Image layout and dummy-initialized flags are updated during recording. Abandoning a command
  buffer must roll those assumptions back or discard the unsubmitted generation. Model creation
  can itself record initialization work; never release it while that work might execute.
- Resize/filter-change/shutdown call `vkDeviceWaitIdle` but do not consistently check the result
  before frees. Apply the fail-closed drain/abandon rule, including partial initialization.
  `FramesVk` increments before checking the evaluate result and before resolve; it is not a
  completed-composition counter. Report attempted, recorded, submitted and completed work distinctly.
- `Transition`/`TransitionForeign` use shader access masks and ignored queue-family indices;
  they are not general transfer/WSI/ownership barriers. Add the actual transfer access scopes
  and same-layout memory dependencies where needed rather than copying those helpers unchanged.

## Zero guides and colour, in Vulkan terms

The corrected step 2 is **bring-up only**. Zero guides prove cold initialization and transport;
consecutive-frame motion reconstruction is part of the first player-facing result, rather than a
possible optimization later. This note does not elevate the review's model-training explanation
into an independently verified vendor specification; the accepted requirement is to test motion
quality with synthesized motion before general use.

| Resource | Vulkan contract |
|---|---|
| Depth | Full source extent `VK_FORMAT_R32_SFLOAT`, zero; colour aspect, one mip/layer, sampled view, NGX image wrapper with `ReadWrite=false`. Bind as `DLSSNR.Depth`, inverted depth true. |
| Motion | Full source extent `VK_FORMAT_R16G16_SFLOAT`, same lifetime and zero initialization, `ReadWrite=false`; bind as `DLSSNR.MVec`. CurrentToPrevious, SourcePixels, RelativeInverse. Full-size guides, zero origins, unit source scale; preserve the existing source-to-working-size scaling exactly once. |
| Confidence | `VK_FORMAT_R8_UNORM` only if a local blending consumer requires it. Never an NGX binding; omit the unused image in the minimum. |
| Working colour | Owned `VK_FORMAT_R16G16B16A16_SFLOAT`, matching the existing Vulkan composition images; sampled/storage and required transfer usage. Explicit SDR metadata selects passthrough despite the float format. |

Unlike D3D's UAV clear, use `vkCmdClearColorImage` once on each new zero guide: UNDEFINED →
TRANSFER_DST_OPTIMAL, clear zero with TRANSFER_DST usage, then a transfer-write → compute/model-read
dependency into the verified NGX input layout (GENERAL for this contract). No depth attachment
image or depth/stencil aspect is involved. SAMPLED plus TRANSFER_DST is sufficient for immutable
guides; add STORAGE only if the chosen producer actually writes through a storage descriptor and
the format supports it. Zero contents survive history resets; only a new allocation needs clearing.
Do not confuse `DlssNr_Vk`'s one-pixel unused-binding dummy with a valid full-extent NGX guide.

Reuse the planned four-value `GuidanceMode`: 0 available, 1 force zero, 2 motion only/zero depth,
3 depth only/zero motion. During bring-up all resolve to zeros and status says so; there are no
four different functional UI choices yet. No confidence parameter, optical-flow queue or invented
jitter is needed to establish that first contract.

Reset on initialization, resize, scene change, interruption, device generation and long pause.
For the zero-guide diagnostic, conservative per-evaluate reset is acceptable if reported; it is
not a substitute for temporal reconstruction. The player-facing motion producer needs previous
unmodified source colour, a reviewed cut/pause policy, history reset when that history is invalid,
and explicit compute-write → NGX-read ordering. Produce motion on this same queue first; a future
optical-flow family reintroduces cross-queue synchronization and feature negotiation.

Swapchain images need not support STORAGE, and BGRA still needs format handling. During creation,
query surface/format support and, only under the opt-in, request supported TRANSFER_SRC/DST usage
for a transfer/blit route into and out of working colour. Preserve sharing mode, colour space,
`oldSwapchain`, allocator and the effective usage from any relevant `pNext` structure
([swapchain creation contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html)).
Do not request unsupported flags and then retry a failing swapchain creation blindly: retirement
of `oldSwapchain` has its own semantics. Decide whether this host is eligible before the real call.

Validate actual blit capabilities and conversion for RGBA/BGRA UNORM; `vkCmdCopyImage` is not a
channel/colour conversion. SDR sRGB image formats need an explicit transfer-function contract:
sampling/blitting can decode or encode sRGB, which must not silently change what NR is shown.
The initial accepted path may decline sRGB-typed images until that conversion is validated; report
the exact format, not just “SDR”. Reject HDR/PQ/scRGB in the minimum rather than inferring linear
HDR from a float format. Surface transform, alpha, array layers, protected images and shared-present
modes also need explicit support or rejection. Keep the original image untouched until a complete
successful composition can be transferred back.

## The queue and semaphore contract

Start on the **actual presenting queue**, if it supports every required compute/transfer command.
It need not support graphics merely because the existing overlay requires it. Query its family,
capabilities and timestamps; `timestampPeriod` alone does not prove `timestampValidBits` support.
Create the command pool for that family. Use the same device/queue for guide initialization, colour
transfer, NR and writeback; no “first graphics queue” fallback.

For EXCLUSIVE images, prove ownership before injected commands. Presentation on a family is not
a blanket proof that all required acquire/release barriers for *new rendering* are present. A
conservative first implementation accepts a single command-capable family equal to the present
family, or CONCURRENT images whose declared families include the execution family. Decline an
unproven multi-family EXCLUSIVE case. If a present-only queue cannot compute, using another queue
requires a separately reviewed semaphore handoff and, where applicable, matching ownership transfers.
CONCURRENT sharing does not make an unlisted family legal. Unknown ownership is an inactive reason,
not an invitation to emit `QUEUE_FAMILY_IGNORED` everywhere.

For one admitted swapchain and one ordinary image in a present call:

1. Validate the generation, image index, usage, queue, present metadata and free recording resources.
   If any prerequisite fails or the previous NR recording is still busy, forward the original
   present unchanged. Allocate/record before consuming any application wait semaphore.
2. Submit a command buffer that waits on **all** the original present's binary semaphores exactly
   once. Its wait stages must cover the first transfer/transition and compute reads (ALL_COMMANDS
   is an acceptable conservative initial mask), not the overlay's COLOR_ATTACHMENT_OUTPUT mask.
   Transition PRESENT_SRC_KHR → transfer source, initialize/convert/process the owned images with
   correct transfer/compute dependencies, write the answer back and restore PRESENT_SRC_KHR.
3. Signal a host-owned binary semaphore. Call the downstream present once with a local present-info
   copy waiting on that semaphore instead of the consumed originals. Keep `pResults`, image order
   and all supported `pNext` semantics. Even on the same queue, presentation requires the semaphore
   dependency; submission order alone is insufficient.
4. Associate the submit fence with command buffers, descriptors, model and images. Associate the
   downstream present with the signal semaphore's separate presentation lifetime. Update history
   and counters according to recorded/submitted/completed/presented outcomes, not merely hook calls.

These requirements follow the [Vulkan present contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html).
For a multi-swapchain present, the application's waits belong to the whole call, not one array entry.
Initially decline the entire call unchanged; do not consume each wait once per swapchain or process
only index zero using global resources. Incremental-present regions need expansion to cover the
full changed image or an explicit bailout; preserve present IDs, timing, fences and device-group
metadata where supported, and decline unsupported combinations before submission.

If the Vulkan overlay is active, chain application → NR → overlay → present, each consuming its
input waits once and producing the next dependency. Do not allow both consumers to wait on the
original binary semaphore. A translated game's D3D overlay is upstream of this boundary and may
already be in the image NR sees; “NR draws no UI” does not mean it cannot process UI. Opti's menu,
game HUD and third-party overlays need image-quality testing, even if the native Vulkan menu is
placed after NR.

Do not use a submit fence to recycle a semaphore still in a present wait. Use per-image completion
semaphores with proven reacquisition ordering, or enabled swapchain-maintenance presentation fences.
At retirement, `vkDeviceWaitIdle` is required for live-device NR resource cleanup by DEVELOPMENT,
but is not by itself a specification-level proof that presentation has consumed every semaphore.
Use present-completion evidence; otherwise retain those semaphore handles until safe retirement or
device destruction, with bounded retention and a disable reason rather than unbounded churn.
This distinction is documented in the [Khronos semaphore reuse guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).

All owned work referencing an old image must finish before its swapchain is destroyed. Record
`oldSwapchain` retirement without confusing it with immediate destruction: old acquired images
can still be presented. On failed drain retain resources; on a gone device abandon handles without
destructors calling Vulkan. A returned device-loss error is not proof of successful completion.
Honor live/dead ownership on model/shim teardown as well as images and command pools.

Fallback is easy **before** submission and conditional afterwards. Once a submit has accepted the
original waits, never forward those originals again. A successful setup-only or unmodified-copy
submission still needs its completion semaphore chained to present. Recording/submit failure must
establish whether waits were consumed; device loss cannot be turned into a successful pass-through.
Present errors also have different enqueue semantics: do not assume an out-of-date result leaves
waits untouched, or that every failed present consumed them. Retain uncertain semaphores/resources
and propagate the actual result instead of retrying with an invalid wait list.

Host locks must cover registry and recording lifetimes through the downstream call; the same
queue also requires external host synchronization. A lock taken only by our callback does not
serialize arbitrary application queue calls. Staying inside a valid application's synchronous
present call on that exact queue benefits from its queue discipline; an injected worker using a
different application queue does not. Streamline's concurrent paths are precisely why this must
be traced and tested, not “fixed” with a global ImGui mutex. There is no per-stage CPU wait or
per-frame `vkDeviceWaitIdle` in the proposed normal path.

## Relationship to the other hosts and FG

This is a third host sharing the NR contracts, settings, consumer arbitration and model/shader
implementation. It is the best candidate to **supersede the D3D11 bridge for Proton** if coverage,
cold initialization and pacing tests pass. The bridge remains a possible Windows/D3D11 host or
fallback where Vulkan interception cannot work. The D3D12 present port remains useful on Windows,
when Vulkan hooks must stay disabled, and when D3D12-level frame/HUD/FG context provides a better
placement. Native Vulkan and translated D3D12 paths must not both run NR on the same content.

Select one owner at startup and suppress other NR originators for it, including intercepted
upscale calls that appear later. Do not auto-enable this route because no upscaler has appeared
yet. Preserve the one-consumer guard against other neural add-ons; installing DLSS5VKLayer alongside
it is not a validation strategy for this host.

First acceptance is **FG off**. `vkQueuePresentKHR` can be called for generated as well as rendered
frames, on pacer threads; it does not carry a universal “real engine frame” bit. Applying NR to
every generated present can multiply inference cost and corrupt temporal continuity. Known FG
activity should cause an explicit unsupported state in the first slice, not an automatic FG toggle
or reliance on the menu interlock. Unknown third-party frame-generation paths remain unvalidated.
Extending this host to FG requires both a real/generated-frame policy and proof of pacer safety.

## Controls, guards and review

| DEVELOPMENT.md §1 | Contract |
|---|---|
| 1. Default-identical | New key false follows existing code, including marker and D3D overlay routing. No additional extension/feature/usage requests, capture records or submits. With an opted-in host, turning NR off live stops new NR work and safely retires resources; installed interception and immutable device/swapchain capabilities last until restart. No background processing while disabled. |
| 2. No dead control | Startup host selection says restart required. Show active owner or precise unsupported reason and actual composed count. Zero guides are diagnostic status, not four apparently different live options; no exposure/upscaler-stage controls whose inputs do not exist. Any menu edits must follow §2's Colour table. |
| 3. One quantity, one control | Reuse `Enabled`, working scale, strength, colour and pass controls where the Vulkan backend implements them. One shared `GuidanceMode` selector; no parallel motion/depth booleans or Vulkan strength. Existing `Stage`/proposed D3D12 `HookMethod` do not change meaning. Competing host selections produce an explicit conflict, never double dispatch. |
| 4. Four-point round-trip | `DlssNrVulkanPresentHost` bool false: `Config.h` default, `Config.cpp` reload, `Config.cpp` save, shipped `[DlssNr] VulkanPresentHost=false`. Reuse planned `DlssNrGuidanceMode` integer 0 if already introduced; otherwise all four edits for `GuidanceMode=0`, validating 0–3. Document startup/marker precedence in `Config.md`. |
| 5. Struct/cbuffer | Host identity and image metadata are CPU contracts. No new shared shader scalar is needed for a zero clear. Any actual constant addition remains append-only and identical in C++ and HLSL. |
| 6. Real shader rebuild | Any shared HLSL edit rebuilds both DXIL and SPIR-V and verifies artifact freshness. New conversion/motion shaders need their own reproducible generated artifacts. This docs-only commit changes none. |
| 7. Passthrough | Tone-mapped SDR stays on `gPassthrough`, including every reproduced encode/`fullProxy` path. Float working storage does not make the input scene-linear HDR. No fake exposure or double transfer-function application. |
| 8. Vulkan lifetime | Checked live-device drain before freeing; gone-device abandon, completion-based slot reuse and separate presentation-semaphore retirement. Resize, scale/filter change, partial create and teardown all follow the rule. |
| 9. Local until asked | No push or deployment in this task. |

DEVELOPMENT §3 applies to the eventual config, shader, menu, Vulkan-resource and non-NR-path
changes. Walk it adversarially, including every Colour-table row for menu changes. The Vulkan path
does not currently mirror all D3D12 chain/compare controls merely because it uses the shared shader:
inventory the actual readers and hide or implement unsupported controls instead of promising parity.

## What settles the marker question

The test is a controlled separation of hook effects, submission effects, NR and ImGui, rather than
one successful session with the marker deleted. Use isolated test deployments and keep a known-good
marked build for comparison; do not change the fourteen deployed markers as part of bring-up.

1. **Coverage and no-op control.** A Win32 Vulkan harness with no NGX/upscaler calls enters the
   production DLL through real creation/present hooks. Prove startup and late-load interception,
   with marker present/absent and `OverlayMenu=false/true`. New host off is identical; NR-only
   observation records exact instance/device/queue/family/swapchain/image identity but performs
   no submit, menu, Reflex or limiter work. Separately prove DXVK and vkd3d-proton reach that same
   hook set. A native Linux `vkcube` run is external prior art, not coverage of our DLL.
2. **Submission-only control.** Add the checked wait → unmodified image round-trip → signal →
   present chain with NR and Vulkan ImGui absent. Verify all waits, barrier/layout/ownership
   transitions and semaphore reuse under synchronization validation. Cover compute-only and
   graphics+compute presenters, explicit unsupported present-only and multi-family cases, multiple
   waits and zero waits, multiple devices/swapchains, delayed GPU work beyond ring depth, and
   concurrent present/resize/teardown. No inference is needed to expose the shared pacer hazard.
3. **Cold NR.** Add clear-once guides and real snippet create/evaluate/resolve, without application
   NGX init or parameters. Test RGBA/BGRA, supported SDR conversions, unaligned extents, working
   scale changes, missing extensions/features, shim ABI mismatch, missing model and fallback
   parameter allocation. Prove completion and output, not just `FramesVk` or module loading.
   Inject recording/submit/drain failures, out-of-date/surface-lost presents, device loss and old
   swapchain retirement. No frees on unknown completion and no retry of already consumed waits.
4. **The pacer experiment.** In the recorded vkd3d-proton/Streamline failure class, compare the
   marked baseline, NR-only observation, submission-only, and NR-only processing with the Vulkan
   overlay entirely absent, first FG off then explicitly FG on in the test build. Repeat menu
   open/close, resize, load and pacing transitions across several sessions; an 18-minute pass was
   already followed by a failure historically. Record Streamline's fence timeouts/pacer events,
   Vulkan validation, queue-call threads and identities, present IDs/images, waits/signals, GPU
   completion and frame-time tails. Add Vulkan ImGui only as a separate control where supported.
   A reproducible failure in submission-only or NR-only with no ImGui refutes the UI-only claim;
   a failure only with ImGui narrows it but does not prove arbitrary host scheduling safe.
5. **Player-facing proof.** After transport passes, implement/validate consecutive-frame motion
   and cut/reset handling before asking players to judge this route. Compare camera/object motion,
   disocclusion, cuts, HUD/text, menus and pause/resume against transport-only and NR-off images.
   Measure conversion, guidance, NGX and resolve separately, plus present CPU waits, GPU utilization
   and latency. The final issue-13 correction is why optical-flow timing must not stand in for
   inference timing. Keep unrelated display/VRR settings fixed.

The existing [test index](../../../tests/README.md) has fourteen suites guarded by
[suites.toml](../../../tests/suites.toml). Keep all eleven host suites green; add production-slice
tests for policy, parameter construction and submission/retirement state machines, rather than
source-grep claims of GPU correctness. Register new suites and their README entries.
`nr-gpu-timing-d3d12` and `dlssnr-loopback` are D3D12 evidence, not Vulkan NR coverage.

`vulkan-overlay` is valuable real-hook/lifetime scaffolding and enables synchronization validation
in `run.py:263`. It covers graphics presentation and non-graphics bailouts in CONCURRENT and
EXCLUSIVE cases, but its application drains after frames and it explicitly does **not** prove
concurrent presents, actual device loss, Streamline pacing or NGX. Its `OverlayMenu=false` negative
control expects zero overlay coverage; a new independent NR harness must positively cover that
state without weakening the old assertion. Prove validation is active with its negative-control
technique. Run Wine suites serially and outside build windows.

## Stage 1 as built (2026-10-14)

Item 1 above, and only item 1: interception, identity and proof that nothing changed. Branch
`feat/vulkan-present-host`, commits `61bc250c` (the host), `476e9f4e` (a fix the translated tests found) and
`e460595c` (tests). No submit, resource, semaphore, NR or menu work exists in it.

### What, and where

- **The key.** `[DlssNr] VulkanPresentHost` (`DlssNrVulkanPresentHost`, bool, default false), the four points:
  `Config.h` (DlssNr block, after the HookMethod notes), `Config.cpp` read and save, the shipped ini with a comment
  that says it is an experiment that only watches. Read once, when vulkan-1.dll is hooked; a change needs a restart.
  It works only under `[DlssNr] Enabled`.
- **The call site.** `DlssNr::VkPresentHost::Install(vulkan1)` is the first statement of `VulkanHooks::Hook`,
  ahead of its `vulkanHooksSkipped` return, so the startup path (`CheckWorkingMode`, which on Wine always loads
  vulkan-1.dll) and the late path (`LibraryLoad_Hooks.cpp`'s vulkan-1 branch) both reach it. Off, `Install`
  returns at its first statement; that and "one call site" are source guards (`tests/nr-vk-present-host`).
  `ResolveVulkanHookPolicy`'s line and the bundle's "Skipping vulkan-1.dll hooks" line are unchanged; the host's
  install line says the bundle stays off and this host is the one exception, so the two decisions log separately.
- **The records.** `retoque/dlssnr/DlssNr_VkPresentRegistry.h`, standard library only: instance, physical device
  (from `vkEnumeratePhysicalDevices`), device (with the physical device's queue families and the queue create
  infos), queue (what `vkGetDeviceQueue`/`vkGetDeviceQueue2` returned, with family, index and create flags),
  surface to HWND, swapchain to device, surface, images, format, extent, usage, sharing, retirement by
  `oldSwapchain`, per-image acquires. Every record takes an id from one counter, so a reused handle value is a new
  record; swapchains and surfaces are keyed by their parent, since non-dispatchable handles are only unique per
  parent. Destroys retire the record before the call goes down, while the value cannot be handed out again.
  Nothing in the bundle's globals (`_device`, `_PD`, `_instance`, `_hwnd`) was touched: the registry is additive.
- **The hooks.** `retoque/dlssnr/DlssNr_VkPresentHost.{h,cpp}`, Detours, each hook calling the trampoline of the
  target it was attached to with its own arguments, once:
  - vulkan-1.dll's `vkCreateInstance`, `vkDestroyInstance`, `vkEnumeratePhysicalDevices`, `vkCreateDevice`,
    `vkCreateWin32SurfaceKHR`, `vkDestroySurfaceKHR`, at install. Installed before the bundle's own detours on the
    same exports, so the `hkvkCreateDevice` the bundle's `vkGetInstanceProcAddr` hands out still calls down
    through the host. (Consequence: with the bundle on, the host's instance and device lines name
    `OptiScaler's own Vulkan hook, called down to this one` as creator.)
  - per device, right after `vkCreateDevice` returns, what `vkGetDeviceProcAddr` gives for `vkDestroyDevice`,
    `vkGetDeviceQueue`, `vkGetDeviceQueue2`, `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`,
    `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, `vkAcquireNextImage2KHR`. One template instantiation per
    distinct address (four slots, one per ICD or first layer), so each hook knows its own trampoline without a
    lookup; a fifth address is logged and left alone. Under the bundle, its `vkCreateSwapchainKHR` hook runs
    first, so the host records the usage the driver actually gets.
  - `vkQueuePresentKHR` at a device's first swapchain creation, after the bundle's `HookDevice` (which runs in its
    `vkCreateDevice`): the host's detour sits on top and runs first. That is already the order stage 2 needs,
    application -> NR -> overlay -> present.
- **What counts as OptiScaler's own.** `ScopedSkipVulkanHooks` only. The first build also read
  `State::creatingD3DDevice`; the D3D hooks set it around the game's own `D3D12CreateDevice`, `CreateDXGIFactory`
  and D3D11 device creation, inside which vkd3d-proton and DXVK make their instance and device, so every translated
  title's device was filed as internal and never reported. The vkd3d-proton case caught it (`476e9f4e`).
  `IdentifyGpu`'s throwaway D3D12 device, made under `ScopedSkipVulkanHooks`, is still recorded as OptiScaler's,
  and the title's device that gets its handle value back is a separate record.
- **What it never does** (source-guarded): submit, create, allocate, record, wait, copy or edit a create or present
  info, touch usage; call Reflex, `FrameLimit::sleep`, `TickFrozenCheck`, upscaler timing, the overlay, spoofing or
  the NR evaluate. Its own Vulkan calls are three read-only queries: `vkGetDeviceProcAddr`,
  `vkGetPhysicalDeviceQueueFamilyProperties` (at device creation) and `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`
  (at swapchain creation, to say what the surface would allow). The registry lock is never held across a call into
  Vulkan or the log. Per present: one `GetModuleHandleExW` (as `IsDxgiBackedCaller` does), a few hash lookups, and
  a line only when one is due.

### The log

At install, once: `DLSS-NR Vulkan present host (observe only; stage 1 of nr-vulkan-present-host.md): installed on
vulkan-1.dll, while the rest of OptiScaler's Vulkan hooks stay off ([Vulkan] SkipHooks or the marker); this host is
the one exception` (or `beside OptiScaler's own Vulkan hooks`). A later load through the loader hook adds, once,
`vulkan-1.dll reached the hook again (the same module); already installed`. One line per instance and device:

```
DLSS-NR Vulkan present host: device 0x7FFFF1464CB0 #3 created by vk-present-host-harness.exe (native Vulkan); physical 0x7FFFF147B6F8 instance #1; queue families 0:0xFx16 1:0xCx2 2:0xEx8 3:0x2Cx1 4:0x4Cx2 5:0x10Cx1; requested family 0 x1 family 2 x1
```

Per window, at its first present, whenever its swapchain, device, presenting queue or calling module changes, and
at 10, 100, 1000... presents (so a session writes a handful), with the wait range of the presents since the
window's previous line:

```
DLSS-NR Vulkan present host (observe only): window 0x10058 (new swapchain, new queue); presents 13, swapchains 2, devices 1; swapchain 0x55558BE923D0 #5 632x446 format 44 colour-space 0 images 4 usage 0x13 (transfer-src yes, transfer-dst yes, storage no) surface-usage 0x9F sharing EXCLUSIVE; round-trip legal as created; device 0x7FFFF1464CB0 #3 physical 0x7FFFF147B6F8 instance #1; queue 0x7FFFF1464D08 family 2 index 0 flags 0xE (graphics no, compute yes, transfer yes); waits 0-1; acquires 1; caller vk-present-host-harness.exe (native Vulkan)
DLSS-NR Vulkan present host (observe only): window 0x10058 (first present); presents 1, swapchains 1, devices 1; swapchain 0x555561902160 #4 632x446 format 44 colour-space 0 images 5 usage 0x12 (transfer-src no, transfer-dst yes, storage no) surface-usage 0x9F sharing EXCLUSIVE; round-trip needs TRANSFER_SRC at creation, the surface offers it; device 0x7FFFF1464610 #2 physical 0x7FFFF1484E78 instance #1; queue 0x7FFFF1464658 family 0 index 0 flags 0xF (graphics yes, compute yes, transfer yes); waits 1-1; acquires 1; present pNext 1000275001 1000275003 1000294000; caller d3d11.dll (DXVK)
```

`round-trip` answers stage 2's legality question for that swapchain: `legal as created` (both transfer usages),
`needs X at creation, the surface offers it`, or `not possible, the surface does not offer X`. Only when non-zero:
`presented without an observed acquire N`, `multi-swapchain presents N`, `present pNext <sTypes>`. A present of a
swapchain the host never saw created writes one line per session and is counted after that; OptiScaler's own
devices are never reported. The caller is classified by module: `dxgi`/`d3d11`/`d3d10core`/`d3d9`/`d3d8.dll` DXVK,
`d3d12`/`d3d12core.dll` vkd3d-proton, `vulkan-1`/`winevulkan.dll` the loader (the real caller is not visible past
it), OptiScaler's own module (another of its hooks ran first), anything else native.

### Validated under Wine (11.19, RTX 4090, 2026-10-14)

- `tests/vulkan-present-host` (new, wine tier), the production DLL uninstrumented, ten cases, all PASS. Native
  Vulkan application: under the marker and under `SkipHooks` with the host on, every field of all six lines over
  two windows, three devices (the third got the destroyed second's handle value back and is a new record), four
  swapchains (one through `oldSwapchain`), presents from the graphics family and from compute-only family 2
  (`0xE`), 0, 1 and 2 waits, `vkGetDeviceQueue2`; the 14 hooked exports patched, `vkQueueSubmit` and
  `vkCreateFence` not; the late path's line. Key off and master off under the marker: no host text and all
  sixteen watched exports byte-identical to before OptiScaler loaded. Beside the bundle with `OverlayMenu=false`
  (positive coverage of the state `vulkan-overlay` must see as zero) and `true` (the overlay's present hook is in,
  and the host's caller is still the application, so its present detour runs first). D3D titles through
  GE-Proton11-7's DXVK and vkd3d-proton under the marker: their devices are created by `d3d11.dll (DXVK)` and
  `d3d12core.dll (vkd3d-proton)` and their presents reach the host with that caller. Validation (core and
  synchronization) on for the native cases, proved active by a rejected fence flag, zero errors.
- `tests/nr-vk-present-host` (new, host tier): the registry's state machine and the source guards.
- `tests/vulkan-overlay` PASS at `e460595c` (both non-graphics controls and the zero-coverage control);
  `python3 tests/run_all.py` 48/48; `nr-invariants` 73 keys round-trip.

What the translated cases showed: both layers present from the graphics family (0) with one wait, on EXCLUSIVE
swapchains of usage `0x12` (no TRANSFER_SRC) whose surface offers `0x9F`; both recreate their VkSwapchain once
early; both chain `VkSwapchainPresentFenceInfoEXT` (1000275001) and `VkSwapchainPresentModeInfoEXT` (1000275003)
on every present, DXVK adds `VkPresentIdKHR` (1000294000), vkd3d-proton 1000479001 (not in the vendored headers;
by its number, `VK_KHR_present_id2`'s `VkPresentId2KHR`).

Found along the way, none of it caused or changed by the host:

- The legacy Vulkan overlay (`OverlayMenu=true`, no marker) crashes the process when a second window's swapchain
  is created on a second device: a null read at `0x90(rbx)` inside `MenuOverlayVk::CreateSwapchain`, right after
  `MenuOverlayBase::Handle() != _hwnd`. With the host off too (`key-off`). The bundle cases run one window.
- That overlay leaves its objects on a device the application destroys (`VUID-vkDestroyDevice-device-05137`): it
  has no device-destroy hook, which `vulkan-overlay` hides by calling a test-only teardown export first.
- Wine 11.19's winevulkan appends `VK_EXT_external_memory_dma_buf` without `VK_KHR_external_memory_fd` to its
  host-side device, and the native validation layer reports it once per device whenever the bundle is skipped
  (the bundle's own extension additions make it go away). The suite counts it separately.

### Not validated

Any game; native Windows (the Khronos loader's trampolines, where an export and `vkGetDeviceProcAddr`'s answer
differ and only the dispatch-level detour is shared; Install inside DllMain there); concurrent presents from two
threads (Streamline's pacer); device loss; late installation over devices that already exist (their objects would
be unknown records); OptiScaler's Vulkan FSR-FG (its swapchain handles are not the driver's: expect the unknown-
swapchain line); `vkAcquireNextImage2KHR` and queue create flags beyond the host test; multi-GPU (more than one
target address per function); the per-present cost, which was not measured.

### Drift against today's tree (re-checked 2026-10-14 at `508d4d2d`)

| Cited (2026-09-12) | Today |
|---|---|
| marker check `dllmain.cpp:947–964` | `ResolveVulkanHookPolicy` at `dllmain.cpp:369–389`, called right after Wine detection (1643); `CheckWorkingMode`'s Vulkan block at 966–981; `VulkanHooks::Hook` enforces the skip itself (already noted above) |
| late load `hooks/LibraryLoad_Hooks.cpp:392–401` | 420–432, through the same `VulkanHooks::Hook` gate |
| `HookDevice:56` | `hooks/Vulkan_Hooks.cpp:169`; the guard is the per-thread `ScopedSkipVulkanHooks` since 2026-10-13 |
| `hkvkCreateDevice:243`, `NoteDeviceQueues` and `HookDevice` only under `OverlayMenu` | 310–502; `NoteDeviceQueues` is now upstream's `MenuOverlayVk::DeviceCreated` (437), still under `OverlayMenu` (425); the FG path also calls `HookDevice` (419–423) |
| `hkvkQueuePresentKHR` reads upscaler timing, ticks frozen, runs Reflex and the limiter | still all four (505 on), but since upstream `1cc9d5a1` (taken 2026-10-12) a present from dxgi/d3d11/d3d12 (`IsDxgiBackedCaller`) skips the Vulkan overlay, `swapchainApi` and `TickFrozenCheck`. **Without the marker, DXVK and vkd3d-proton presents no longer reach the Vulkan overlay at all**, which narrows the marker's original motivation (the overlay against Streamline's pacer under vkd3d-proton) to the rest of the bundle: Reflex, the limiter, spoofing, Vulkan_wDx12 |
| `menu_overlay_vk.cpp:852` (simultaneous presents) | 1049–1056 in `QueuePresent` (1036) |
| `QueuePresent:913` declines a non-graphics family | **changed in substance**: since `fc08aa32` (2026-09-19) a non-graphics present family is drawn on the graphics family with a queue-family transfer around it (three submits, semaphore chain), not declined |
| waits replaced `1068–1089` | present-queue path 1283–1328, cross-family chain 1362–1426 |
| `DxOverlayOwnsBackend:192` | 339 |
| `DlssNrFeature_Vk.cpp:562` (record into a supplied command buffer), `562–668` early returns, `873` parameter reads, `703–758` bootstrap | now `retoque/dlssnr/DlssNrFeature_Vk.cpp`: `EvaluateAfterUpscaleVk` 608, the missing-resource return 740, parameter reads around 1015, `Init_Ext` with `0x15` 846 and `AllocateParameters` 862 |
| `shaders/dlssnr/DlssNr_Vk.cpp` | `retoque/shaders/dlssnr/DlssNr_Vk.cpp` (moved 2026-10-13); the ring is `kSlotsPerFrame 6 x kFramesInFlight 3` in `DlssNr_Vk.h:33–35`, unchanged |
| `inputs/NVNGX_DLSS_Vk.cpp:665` | 661 |
| `vulkan-overlay/run.py:263` (sync validation) | 447 |
| "fourteen suites", "all eleven host suites" | 51 registered before this stage (47 host, 4 wine); 53 now (48, 5) |
| clang-format `/usr/lib/llvm20/bin/clang-format` | clang-format 22 through `uvx` (CLAUDE.md, "Formatting") |

### What stage 2 needs

- **Usage, decided before the real call.** No swapchain a translation layer made here allows the copy round trip:
  DXVK and vkd3d-proton both ask for `0x12`, without TRANSFER_SRC, on a surface that offers it (`0x9F`, STORAGE
  included). So stage 2 starts by editing `VkSwapchainCreateInfoKHR` under the opt-in (TRANSFER_SRC added when the
  surface offers it, eligibility decided before the call, never a retried creation), which stage 1 deliberately
  does not do. The overlay already does exactly that for its blur when the bundle runs with its menu, and the host
  sees the result (`menu-on`: `0x13`). STORAGE on the swapchain image is offered but would need a format-feature
  check on BGRA8; the copy route is the one to build.
- **In the harness**, the round trip is legal on swapchain 2 (`0x13`) as created, and swapchain 1 (`0x12`) is the
  "needs a usage edit" case; the compute-only present family 2 (`0xE`: compute and transfer, no graphics) is the
  presenting queue the design wants NR to run on without graphics. Sync validation and its negative control are
  already in place for the submission-only control.
- **Present chain.** Both layers chain swapchain-maintenance1 present fences and present-mode info, and present
  ids, on every present. Stage 2 must carry the chain through its local present-info copy, and the present fences
  are the presentation-completion evidence the semaphore-retirement section asks for, available on exactly these
  paths. One wait per present on both layers; the harness covers 0, 1 and 2.
- **Retirement.** Both layers recreate their swapchain early, so generation retirement is exercised from the first
  seconds; the device-destroy and swapchain-destroy hooks exist now (records only) for resources to hang on.
- **Order with the overlay.** The host's present hook already runs ahead of the overlay's; stage 2 still has to
  build the defined chain (NR consumes the waits once, the overlay waits on NR's signal), and the bundle-with-menu
  tests stay single-window until the overlay's second-window crash is fixed.
- **Game runs** before any of that: the lines from DOOM Eternal (native Vulkan, compute-family present) and from a
  marked vkd3d-proton title (with Streamline), to confirm in a game what the harness showed.

## Stage 2 as built (2026-10-14)

Item 2 of "What settles the marker question", the submission-only control: the full transport, with no NR and no
ImGui. Branch `feat/vulkan-present-host`, on top of stage 1: commits `b559d059` (the host) and `18983a3c` (tests).
Same keys: with `[DlssNr] Enabled` and `VulkanPresentHost` both true the host now carries every eligible present
through a round trip; with either off, `Install` still returns at its first statement and nothing exists (the off
cases below hold the exports byte-identical).

### What, and where

- **The decisions** are a new header, `retoque/dlssnr/DlssNr_VkPresentTransport.h`, standard library only:
  `PlanUsage` (what a swapchain is given at creation), `ChooseRoute` (which queue does the round trip), `ChooseSlot`
  (which ring slot), the refusal reasons (`Skip`), one present's `Outcome` and the counters (`TransportStats`). The
  constants: a ring of 4 slots per swapchain, at most 16 application waits taken over, 100 ms the longest a present
  waits for the oldest slot, 50 ms the longest it waits to borrow another queue, 2 s the longest a retirement drains,
  4096 present semaphores retained per device. `DlssNr_VkPresentHost.cpp` makes the Vulkan calls and acts on nothing
  it did not get from there; the registry carries each present's outcome into the lines.
- **Usage, at creation, before the real call.** The surface's `supportedUsageFlags` and
  `vkGetPhysicalDeviceImageFormatProperties` for the swapchain's implied image (format, the usage with both transfer
  bits, the image flags its swapchain flags imply) and for the work image are read first. If the surface offers what
  is missing and the format allows it, the driver gets a copy of the create info whose only change is `imageUsage`
  (`TRANSFER_SRC`, and `TRANSFER_DST` when missing); its pNext chain and `oldSwapchain` go down as they came. Never a
  retried creation (a creation with `oldSwapchain` retires it even when it fails). Otherwise the swapchain is
  observe-only for its life, the application's usage untouched, the reason on its creation line: the surface does not
  offer the bit (named), the surface could not be read, the format does not allow it, a protected swapchain, a shared
  present mode, more than one array layer, OptiScaler's own device.
- **The route.** The presenting queue does the round trip itself when its family has graphics and compute
  (`kWorkQueueNeeds`). That is narrower than "The queue and semaphore contract" above, which asked only for what the
  copy needs: the transport runs on the queue stage 3's model will run on, and the model has only ever run here on a
  graphics queue (the after-upscale route records into the game's graphics command buffer). A presenting family
  without graphics (id Tech 7's compute present, family 2 `0xE` in the harness) takes the overlay's pattern: an
  EXCLUSIVE image is released on the presenting queue to the lowest graphics family the application retrieved a queue
  of (layout PRESENT_SRC -> TRANSFER_SRC carried by the release/acquire pair), copied there, released back (TRANSFER_DST
  -> PRESENT_SRC) and acquired on the presenting queue: three submits chained by two semaphores, the fence on the last.
  A CONCURRENT image that lists a graphics family is copied there with no ownership transfer, the present waiting
  across queues. Never a queue the application did not retrieve, never one created with flags; a present-only family,
  a CONCURRENT list without the family, an unknown family: no route, with the reason. Relaxing the route to a
  compute-only presenter is that one constant, once NGX is proven on a compute queue.
- **The round trip** (`RecordWork`): one barrier pair (the image PRESENT_SRC -> TRANSFER_SRC, the work image
  UNDEFINED -> TRANSFER_DST, source stage ALL_COMMANDS so it is ordered after the waits), a copy into the work image,
  a second pair, the copy back, the image back to PRESENT_SRC. The work image is the swapchain's format and extent,
  device-local, one per swapchain; every slot's recording orders its first write after the previous recording's reads
  on the same queue, and a round trip on another queue first waits for the ring to drain. Stage 3 runs the model
  between the two copies (the comment marks the place).
- **The waits.** One submit (or the first of three) waits on all of the present's semaphores, at ALL_COMMANDS, exactly
  once, and signals the host's semaphore of that swapchain image. The present goes down as a copy of the caller's
  `VkPresentInfoKHR` whose only change is the wait list -- one semaphore, the host's -- its pNext chain (DXVK's and
  vkd3d-proton's present fences, present modes and present ids) carried by pointer. Zero waits are fine: the
  application ordered its work by the host then. A present semaphore is reused only by a present of the same image,
  which needs that image acquired again since -- the validation layer's own advice for exactly this case -- and a
  present the host did not see acquired is refused. A present that returns anything other than success, suboptimal,
  out-of-date, surface-lost or exclusive-fullscreen-lost (after which the wait may not have run) has its semaphore
  retained and replaced.
- **The ring.** Per swapchain, up to four slots (a fence, the work command buffer, and the two hand-over command
  buffers and chain semaphores for the cross-family route). Each present polls the pending slots' fences; an idle slot
  is reused, the ring grows to four, and when all four are pending the present waits up to 100 ms for the oldest and
  otherwise goes down unchanged (`ring busy`). A slot is never reused or freed before its fence was seen signalled.
- **Queue locks.** A borrowed graphics queue is not inside the application's present: another of its threads may be
  submitting to it, or presenting on it. So the host now also detours `vkQueueSubmit`, `vkQueueSubmit2`(`KHR`),
  `vkQueueBindSparse` and `vkQueueWaitIdle` -- `vkGetDeviceProcAddr`'s answer below OptiScaler's hooks and the export,
  never an OptiScaler function -- and every queue the application retrieves gets two recursive locks: a *submit*
  lock those hooks take around their call down, and a *present* lock the present hook takes for the whole of a
  present on that queue, its call down included. A borrower try-locks the borrowed queue's present lock, then its
  submit lock (50 ms each, `queue busy` otherwise), only around its submits, after the ring and the recording. Order:
  the presenting queue's present lock, the transport's lock, a borrowed queue's two, the registry's (never held across
  a Vulkan call or a log line). The two kinds are apart because the legacy overlay below the present hook submits to
  the graphics queue, through the submit hook, while holding a lock another thread's present on that queue waits for:
  one lock per queue held across that present would close the cycle.
- **Below OptiScaler's own hooks.** The round trip's device functions are resolved through
  `VulkanHooks::GetDeviceProcAddr` (the bundle's trampoline, or the plain export without the bundle), and its submits
  go through the host's own trampoline of the device's `vkQueueSubmit`. This was initially described as below every
  hook; the stage-3 DOOM finding below shows wDx12 can still sit underneath it. The independent review
  found why that matters: resolved through the bundle's `vkGetDeviceProcAddr`, the host's submit became Vulkan_wDx12's
  hook, which takes a queue lock of its own before calling down into the host's -- the reverse of an application
  submit's order, a deadlock in exactly DOOM Eternal's configuration. All of these are resolved before the host's
  install lock is taken: without the bundle the resolution goes through OptiScaler's delay-loaded import of
  vulkan-1.dll, whose first use runs the loader hook and, through `VulkanHooks::Hook`, `Install` again (the
  vkd3d-proton case caught it: `resource deadlock would occur`).
- **The bundle's own caller check.** With the host's present detour above the bundle's, the bundle's
  `IsDxgiBackedCaller(_ReturnAddress())` saw OptiScaler as the caller and took a DXVK or vkd3d-proton present for a
  native one (the Vulkan overlay, `TickFrozenCheck`, `swapchainApi = Vulkan`) -- since stage 1, whenever the bundle and
  the host ran together on a translated title. `hooks/Vulkan_Hooks.cpp` now asks
  `DlssNr::VkPresentHost::OriginalPresentCaller(_ReturnAddress())`, which is the application's return address inside
  a present that went through the host and the address it was given otherwise (the host off: unchanged). One line of
  upstream's file; `dxvk-bundle` holds it.
- **Everything that can refuse is decided before a wait is consumed**: eligibility, image, acquire, wait count,
  route, queue lock, ring, resources (made lazily: the work image, the pools per family, the slot's objects; a failure
  turns the round trip off for that swapchain) and recording -- a recovery command buffer included. A first submit
  that fails consumed nothing (Vulkan's rule for `VK_ERROR_OUT_OF_*`), so the present goes down as the application
  made it. If the cross-family chain's work-queue submit fails after the release ran, the recovery takes the image
  back on the presenting family with its contents discarded (UNDEFINED -> PRESENT_SRC, no ownership transfer, which an
  EXCLUSIVE image allows when its contents are not needed): that frame shows garbage, the chain stays valid. A failed
  last submit is retried once; failing again, the present waits on the last semaphore the chain signalled, the slot is
  abandoned (its fence never signals) and its pool never freed. Either way the round trip stops for that swapchain. A
  `VK_ERROR_DEVICE_LOST` anywhere stops the device's transport.
- **Retirement.** At `vkDestroySwapchainKHR`, before the call down: the pending slots' fences are waited on, bounded at
  2 s in all, on the host's own fences (no `vkDeviceWaitIdle`, which would need every queue); drained, the slots and
  the work image are freed; not drained, they are kept and tried again when the device goes. Present semaphores are
  freed when a reacquire of their image proved their wait ran, otherwise retained until the device goes (more than
  4096 retained turns the device's transport off, a bound instead of churn). At `vkDestroyDevice`: whatever the
  application left, the kept leftovers after another bounded drain, the retained semaphores, the pools. On a lost
  device every one of these is abandoned, never destroyed (DEVELOPMENT.md rule 8).
- **What it still never does**: Reflex, the frame limiter, `TickFrozenCheck`, upscaler timing, the overlay, spoofing,
  NR, NGX, a per-frame CPU wait or a device-wide idle wait (source-guarded in `tests/nr-vk-present-host`).

### The log

The install line now says `(round trip, no NR; stage 2 of nr-vulkan-present-host.md)`. Once per swapchain, at
creation:

```
DLSS-NR Vulkan present host: swapchain 0x55555AD95DE0 #4 on device 0x7FFFF1465390 #3, window 0x10058, 632x446 format 44: usage 0x12 -> 0x13 (surface offers 0x9F); round trip on, TRANSFER_SRC added at creation
```

(`round trip on as created` when the application asked for both, `round trip off, the surface does not offer
TRANSFER_SRC` and the like when it stays observe-only). The window line keeps stage 1's fields and rules (first
present, a change of swapchain, device, queue or caller, 10, 100, 1000... presents), shows the usage the application
asked for when the host changed it, and ends with the transport's counters since the window's first present:

```
DLSS-NR Vulkan present host (round trip, no NR): window 0x10058 (100 presents); presents 100, swapchains 2, devices 1; swapchain 0x55555AF19520 #5 632x446 format 44 colour-space 0 images 4 usage 0x13 (transfer-src yes, transfer-dst yes, storage no) surface-usage 0x9F sharing EXCLUSIVE; round-trip on as created; device 0x7FFFF1465390 #3 physical 0x7FFFF1483598 instance #1; queue 0x7FFFF14653E8 family 2 index 0 flags 0xE (graphics no, compute yes, transfer yes); waits 0-2; acquires 88; caller vk-present-host-harness.exe (native Vulkan); transport: transported 100, skipped 0, waits taken over 184, submits 276, completions 99, ring 1, max CPU wait 0.000 ms, routes same queue 12, cross-family 88
```

Refusals are named with their counts (`skipped 3 (ring busy 3)`), and the first no-route reason is appended when there
is one. Once per swapchain, when it is destroyed, its own totals and what became of the host's objects:

```
DLSS-NR Vulkan present host: swapchain 0x55555AF2EE80 #6 destroyed after 18 presents; transported 15, skipped 3 (ring busy 3), waits taken over 15, submits 15, completions 15, ring 4, max CPU wait 100.031 ms, routes same queue 15; freed after a drain of 1.024 ms; present semaphores 0 freed, 5 kept until the device goes (no reacquire proved their wait ran)
DLSS-NR Vulkan present host: device 0x7FFFF1465390 #3 destroyed; freed: 0 swapchain(s) the application left, 13 retained present semaphore(s), 0 slot(s) kept from earlier swapchains
```

Warnings, only when something fails: `the round trip's submit returned N`, `cross-family submits returned A and B`,
`... could not be made; round trip off for it`.

### Validated under Wine (11.19, RTX 4090, 2026-10-14)

`python3 tests/vulkan-present-host/run.py`, the production DLL uninstrumented, 14 cases, all PASS. Core and
synchronization validation on in every native case and in the DXVK ones, proved active by a rejected fence flag,
**0 validation errors** in each (Wine's own `dma_buf` extension report counted aside, as in stage 1). Every presented
image the harness reads back equals the pattern it made, through the host or not.

- `marker` and `skiphooks` (the bundle off): 130 presents over two windows and three devices (the third with the
  second's handle value), 127 carried and 3 refused. The `0x12` swapchain got TRANSFER_SRC and 12 presents on the
  same queue; the compute-only presenter (family 2) 88 presents across families, three submits each, with 0 and 2
  waits; 7 presents queued behind 1.2 s of GPU work on an 8-image swapchain: four carried, the ring full, the next
  three each waited 100 ms for the oldest slot and went down unchanged (`ring busy`), and every one of the seven read
  back unchanged afterwards; a resize recreated window 2's swapchain at 392x266 through `oldSwapchain`. 104 read-backs
  equal; every destroyed swapchain's round trips all seen complete and its objects freed after a drain of about 1 ms;
  every device's teardown clean. `vkQueueSubmit` and `vkQueueWaitIdle` are now detoured with the host on.
- `surface-no-src`: a native test layer below the validation layer hides TRANSFER_SRC; every swapchain stays as asked,
  `round trip off, the surface does not offer TRANSFER_SRC`, all 130 presents go down unchanged.
- `marker-key-off`, `marker-master-off`: no host text, all seventeen watched exports byte-identical, the read-backs of
  the swapchains that ask for TRANSFER_SRC equal (the control that images survive a present here without the host).
- `menu-off` (the bundle beside the host, no menu): as `marker`. `menu-on` (one window, the legacy overlay drawing its
  splash): 110 presents, all carried, the overlay's present consuming the host's semaphore -- application -> host ->
  overlay -> present -- with no validation error; its read-backs are counted, not compared (the splash is in them),
  and the held-back batch does not run there (on a new swapchain the overlay's present waits for the GPU). `key-off`:
  no host text.
- DXVK (GE-Proton11-7, D3D11): both swapchains `usage 0x12 -> 0x13`, all 12 presents carried on the same queue with
  their present fence, present mode and present id chained through, ring 1, both swapchains drained and freed; 0
  validation errors, as in the host-off baseline. vkd3d-proton (D3D12): the same, 12 of 12, OptiScaler's own
  throwaway device never transported; run without the validation layer, under which GE-Proton11-7's vkd3d-proton
  aborts at device creation with the host off as well. `dxvk-bundle` (no marker: the whole bundle and its menu beside
  the host, `LogLevel=0`): 12 of 12 carried, 0 validation errors, and the bundle's present hook traced for every
  present while the Vulkan overlay's `QueuePresent` never ran -- the bundle took each for a DXGI-backed one, as in its
  host-off baseline.
- `tests/nr-vk-present-host` (host tier): the decisions and the lines, and the source guards rewritten for stage 2
  (the two edits, the lock order, the trampoline submit, nothing resolved under the install lock, drains before frees).
  `python3 tests/run_all.py`: 48/48. `tests/vulkan-overlay --ref HEAD`: PASS at `18983a3c` (both non-graphics controls
  and the zero-coverage control). clang-format 22 clean on every C++ file touched.
- DEVELOPMENT.md section 3's review for a Vulkan resource change was done as an independent read-only review of the
  diff. It found the lock inversion through the bundle's `vkQueueSubmit`, the invalid acquire after a failed
  work-queue submit, the borrowed queue's unguarded present, waits held under the borrowed queue's lock, a protected-
  capable family wrongly excluded from the route, two unbounded retentions, and stage 1's caller check; all are fixed
  above, and every case was rerun after the fixes.

### Not validated

- **Streamline pacing (stage 4).** No Streamline, no DLSS-G, no frame generation, no pacer thread presenting beside the
  game's: the recorded failure class the marker exists for has not been touched by this stage.
- **Any game.** DOOM Eternal (compute-family present, cross-family route, the bundle and its overlay on) and a marked
  vkd3d-proton title have not run this build.
- Concurrency: no two threads presenting at once, no application thread submitting to the borrowed graphics queue
  while the cross-family route runs (the lock exists; nothing here contends), no concurrent present and destroy.
- Failure paths: a failed submit, a mid-chain failure, device loss, a drain that times out, retention past 4096, a
  present error after which the semaphore is uncertain -- reasoned, source-guarded, never executed.
- CONCURRENT swapchains, present-only families, protected or shared-present swapchains (decided on the host tier only).
- Cost: the per-present CPU time, the queue lock on every application submit, two copies per frame of a 4K image,
  and one extra swapchain-sized image per swapchain were not measured. Nor is it known whether TRANSFER_SRC on swapchain
  images changes a driver's presentation path (compression, direct scan-out) on some hardware.
- Native Windows (the loader's trampolines; there the export and `vkGetDeviceProcAddr`'s answer differ, and both are
  detoured for the queue calls).

### First game run (2026-10-14, DOOM Eternal)

Build `3144c8dc`, DOOM Eternal (native Vulkan, `winmm.dll`, Vulkan hooks not skipped, the Vulkan overlay menu on),
GE-Proton11-7, RTX 4090, `[DlssNr] VulkanPresentHost=true`. About three and a half minutes, menu opened and an Alt-Tab:

- 49,143 presents, every one transported, none skipped; 132,313 submits, 49,142 completions observed.
- Ring depth 2, max CPU wait 0.000 ms: the round trip never held the present thread.
- Routes: 41,585 cross-family (id Tech 7 presents from compute family 2; the copy ran on graphics family 0 with the
  ownership transfers and the three-submit chain), 7,558 on the presenting queue itself.
- The Vulkan overlay's menu drew as before ("drawing the menu on graphics family 0 with a queue-family transfer
  around it") beside the host: the bundle-lock-then-host-lock order the review reasoned held in a game.
- No new warning or error in the log. By eye (the user): no stutter and no frame-rate drop, "all normal".

### What stage 3 needs

- **The model where the comment says**, between the two copies, on the work queue: the work image becomes NR's input
  and output at working scale, so it needs STORAGE/SAMPLED usage and the R16G16B16A16 working format, which means a
  conversion (blit or compute) from the swapchain format and back instead of `vkCmdCopyImage` -- with the sRGB and BGRA
  questions of "Zero guides and colour" decided, HDR swapchains refused.
- **A cold NGX on the transport's device**: the snippet's create/evaluate/resolve recorded into the slot's command
  buffer, the clear-once zero guides, no application NGX; the shim and model on that device, `DlssNr_Vk`'s global state
  made per owner or admitted once (its eighteen descriptor/constant slots rotate without a completion test, which the
  ring's fences can now give it).
- **One owner**: the after-upscale Vulkan path and this host must not both run NR on a frame.
- **The device's NR extensions and features at `vkCreateDevice`** (`VK_NVX_binary_import` and the rest, the feature
  structs), which this stage never adds; until then stage 3 can only run where the application enabled them.
- **The present semaphore retention**: a game that recreates its swapchain hundreds of times (window drags) reaches
  4096 and loses the transport; swapchain-maintenance1 present fences, on exactly the devices DXVK and vkd3d-proton
  make, would prove the waits ran instead.
- **The pacer experiment** (stage 4) is still the gate for any claim with frame generation.

### Risks for a first real run

- **DOOM Eternal** (native Vulkan, the bundle and the overlay on, compute-family present): every frame crosses to the
  graphics family and back, three submits; the game's render thread submits to that graphics queue, so each of its
  submits now takes the host's submit lock, and a present that finds it held more than 50 ms (a `vkQueueWaitIdle` of
  the game's at a load, say) goes down unchanged. The bundle's Vulkan_wDx12 `vkQueueSubmit` hook sits on the same
  path with its own lock; the host's own submits now go below it, but the order "bundle lock, then host lock" on the
  game's thread was reasoned, not run. With the overlay drawing, the image is then handed across once more by the
  overlay (its own three submits). The overlay's present on a new swapchain waits for the GPU, which the harness saw;
  the menu-open path with the host's semaphore in front of it was never exercised. Watch for `queue busy` and
  `ring busy` counts and frame-time spikes.
- **A marked vkd3d-proton title**: Streamline's pacer presents from its own thread on the game's queue; the host's
  round trip now sits in front of every present, with its queue lock taken and released inside each present, and its
  own submit on the presenting queue inside the present call -- the very insertion the marker's history blames
  (`8e021bff`: 18 minutes clean, then a device lost at the first menu toggle). Run it first with frame generation off,
  then on, and treat a Streamline fence timeout as the stage-4 answer, not as noise.
- **Memory**: one swapchain-sized work image per swapchain (kept for retired swapchains until they are destroyed), and
  present semaphores retained per recreation (four or five each, never freed before the device goes in the runs here,
  since no image is reacquired after its last present).

## Stage 3 as built (2026-10-14)

Item 3 of "What settles the marker question", cold NR: the model inside the round trip's own command buffer, on the
work queue, with no application NGX and no game parameters. Branch `feat/vulkan-present-host`, on top of stage 2:
`ea506f5d` (the host and the model), `aa122612` (tests), `e3c71460` (a chain with bufferDeviceAddress off), `1894b1ae`
(what the independent review found, below), `d325cf39` and `e2951c81` (tests); after the first DOOM Eternal run,
`b19d88c7` (the way back from a quiet after-upscale route) and `d9523592` (tests), then what that fix's review found
(below, "The review" under the way back). Same keys: with `[DlssNr] Enabled` and `VulkanPresentHost`
both true the model now runs on every eligible frame; with either off, `Install` still returns at its first statement,
and the after-upscale route is what it was (its one new call, the owner claim below, is never refused when the host
is off).

### What, and where

- **The frame.** `RecordWork` (`DlssNr_VkPresentHost.cpp`) keeps stage 2's shape and puts the model between the copies:
  the presented image copied into the work image, which is now in the *raw* format -- the swapchain's own, or its UNORM
  twin when it is sRGB, so the bytes are never decoded and the model is shown the sRGB-encoded frame, as the D3D12
  present pass shows it its copy of the backbuffer -- blitted into a working colour (R16G16B16A16_SFLOAT, one per
  swapchain, sampled and stored; a blit converts number by number and swaps BGRA by channel), the model's pass on that,
  blitted back, copied out. `NEAREST` both ways, same extent, so a frame the model leaves alone comes back to the bit:
  the `nr-control` case reads back every frame exact. The barriers: transfer write -> compute read/write into GENERAL
  before the pass, compute write -> transfer read after it; every earlier frame's use of the working colour is in the
  source scope of its UNDEFINED transition.
- **What the model may be shown** (`PlanColour`, `DlssNr_VkPresentNr.h`): RGBA8/BGRA8, UNORM or sRGB, in the sRGB
  non-linear colour space -- SDR only, as the D3D12 present pass without its HDR conversion. HDR10, scRGB, HLG, ten-bit,
  float and any other format are refused with format and colour space named (`an HDR10 swapchain (PQ, Rec.2020)
  (A2B10G10R10_UNORM_PACK32, colour space 1000104008): the present host's NR takes SDR only; the frame is shown as the
  game rendered it`), on the creation line and as the window's first refusal, and the round trip carries them unchanged.
- **The model** (`VkPresentModel`, `DlssNr_VkPresentModel.h`, implemented in `DlssNrFeature_Vk.cpp` beside the
  after-upscale route it reuses): per device, its own parameter block, feature, composition pass (`DlssNr_Vk`), proxy,
  keep and output images, two zero guides and a timestamp pool. `Admit` does what needs no command buffer: the
  forwarder (now loaded once for the process, `g_fwd`, shared with the after-upscale route: one DLL, one fault record,
  one float slot), the snippet's NGX init on the device (`dlssnr_vk_init`, SDK `0x15`, the data path State's or
  OptiScaler's own folder), **the driver's NGX core brought up on the device** (`NVNGXProxy::InitVulkan`) for its
  capability block -- the D3D12 route does the same, and without it the feature was refused at creation: OptiScaler's
  own block (what `NVSDK_NGX_VULKAN_AllocateParameters` hands back when no DLSS is loaded) carries none of the snippet's
  callbacks -- the pass, and the images for this size. `Record` does the rest into the slot's work buffer: the guides'
  clear when owed, the feature's creation when there is none (its initialisation is recorded there too), encode
  (`Passthrough = 1`; the downsample and resolve are copies of those constants, so `gPassthrough` holds everywhere the
  encode is reproduced), the downsample below native, the model, the resolve into the working colour, a timestamp pair.
- **Zero guides**: `R32_SFLOAT` depth and `R16G16_SFLOAT` motion, made at the model's working size (a resample of a
  constant is the constant: the source-to-working scaling done once, exactly), cleared once per allocation with
  `vkCmdClearColorImage` and left in GENERAL, inverted depth, zero origins, unit motion scale. The working scale never
  goes above native (the model refuses supersampling on zero guides, as the D3D12 hosts found). History starts over on
  the model's first frame, on a frame the host knows is a cut (a swapchain the model takes over, a frame after one it
  did not see), and on every frame with `[DlssNr] ZeroGuideReset`.
- **Lifetime**, from the host's fences: each recording carries a ticket; the host reports it `Submitted` (only when the
  buffer carrying it was: the same-queue submit, or the cross-family chain's work-queue submit), `Discarded` (a refusal
  after recording, a failed submit; a feature built in it is released again, the guides' clear is owed again, and the
  pass is rebuilt if no recording of it ever reached the GPU) or `Completed` (its slot's fence seen signalled, in `Poll`
  or `WaitSlot`: the GPU time is read then, never waited for). A resize or a working-scale change rebuilds the feature
  and images; the old set waits in a list for the tickets that were in flight and is freed when they complete. At most
  four of the model's frames are in flight, and a frame is admitted only while the oldest one in flight is less than
  a ring behind it (`model busy` otherwise): the pass hands out eighteen descriptor/constant slots in order with no
  completion test of its own, a frame takes at most three, and a discarded recording advanced the ring too, so it is
  counted. The model is freed only with nothing in flight (`Release`), and abandoned -- every handle forgotten, nothing
  destroyed -- on a lost device, after a submit that lost the device, or when a frame never completed. A latched
  failure stays when its objects are freed, so a model that cannot run is not brought up again every frame.
- **NGX goes with the device**, not with the model (`ShutdownPresentNgx`, at `vkDestroyDevice` before the call down,
  whatever became of the model -- stood down earlier, never made there): the driver's core is shut down
  (`NVSDK_NGX_VULKAN_Shutdown1`) when the host brought it up on that device, and NGX through the forwarder's new
  `dlssnr_vk_shutdown`, which answers only for the device it initialised (so a device the after-upscale route
  initialised goes the same way). A later device with the same handle value is then initialised again: stage 2's open
  "same-handle case". On a lost device, or with frames never seen complete, nothing is called into; both only forget
  the device (`dlssnr_vk_forget`). The shutdowns are called with the out count NGX's device-specific entry takes (the
  D3D12 one does too, `ShutdownDx12Device`): called without it, the core wrote through a null pointer inside
  `_nvngx.dll` at device destruction -- the first cold run's crash.
- **One swapchain at a time** per device: the model's size and history are one picture's. A live swapchain keeps it
  (`another swapchain`); a retired or destroyed one hands it over, and the frame that takes it starts the history over.
  A swapchain takes it only once the model was admitted for it, so one too small for the model never holds it.
- **One owner of the model in the process** (`DlssNr_VkModelOwner.h`): the forwarder initialises NGX on one device at
  a time, so it is one owner, not one per device. The after-upscale route asks (`ClaimModelForAfterUpscale`, in
  `EvaluateAfterUpscaleVk` where it commits to a frame) and wins: refused while the present host holds the model, its
  asking makes the host stand down at its next present -- stop recording, wait for its frames to be seen complete, free
  the model, release -- and the route takes it at its next evaluate. The host stays down while the route asked within
  half a second (the D3D12 boundary's `BoundaryLive` window), from a refusal until the route takes the model (unless
  the route goes quiet for ten seconds first: `the after-upscale route ... was refused the model and has not taken it
  yet`), and while it owns the model -- until the route goes quiet, when the host takes it back (below, "The way back";
  the first build held it for good). The host also stands
  down while the D3D12 route has a model (`DlssNr::IsRunning()`: a translated D3D12 title's own NR, a Vulkan title
  through the D3D12 bridge, the D3D11 bridge's host), and refuses on a second device while it holds the first. The
  stand-down check runs at every present of the device, before the refusals that return without reaching the model's
  decision, so a model that once ran (an SDR menu, then an HDR swapchain) cannot keep the token from the game's own
  upscaler. A model that cannot run (`model unavailable`), with nothing in flight, holds no token; a lost device's
  model gives it back. `[DlssNr] Enabled` turned off live stands the model down the same way; it comes back cold when
  NR is turned on again.
- **At `vkCreateDevice`**, on a device the host will carry presents on (never OptiScaler's own): the after-upscale
  route's four extensions the device offers (`VK_NVX_binary_import`, `VK_NVX_image_view_handle`,
  `VK_KHR_push_descriptor`, `VK_KHR_buffer_device_address`; the application's EXT buffer device address stands for the
  KHR one; nothing at all on a device without the two NVX extensions, so another vendor's device keeps its create info;
  the KHR buffer device address only on a Vulkan 1.1 instance, since below it it needs `VK_KHR_device_group`, which is
  not added; push descriptors not beside a `VkPhysicalDeviceVulkan14Features` with `pushDescriptor` off), `VK_KHR_format_feature_flags2` where the composition pass's format-less storage writes are not already
  allowed (no `shaderStorageImageWriteWithoutFormat`, below Vulkan 1.3: VUID-VkShaderModuleCreateInfo-pCode-08740, the
  first report validation made), and the **bufferDeviceAddress feature** the model's allocations need
  (VUID-VkMemoryAllocateInfo-flags-03331, the second): one `VkPhysicalDeviceBufferDeviceAddressFeatures` (or its EXT
  struct) prepended to a copy of the create info, the application's chain behind it unchanged, only where nothing in
  the chain could conflict. A chain that has the feature off (in its own struct or `VkPhysicalDeviceVulkan12Features`)
  is not edited -- that would mean copying structs of types the host cannot know -- and the model runs with the
  extensions alone, as the after-upscale route always has; the device line says which. Behind the bundle (no marker)
  its own device hook has added the four first, and the host sees them as already enabled. Decided before the call,
  never retried; nothing that needs `VK_KHR_get_physical_device_properties2` is asked of an instance without it.
- **Present semaphores** of a destroyed swapchain are destroyed once `vkDestroySwapchainKHR` has returned: a valid
  destroy comes after every use of its images has completed (VUID-vkDestroySwapchainKHR-swapchain-01282), the
  presentation engine's included, and a present's wait precedes that use. Only a semaphore whose present failed in a
  way that may not have run its wait is still kept until the device goes. Stage 2 kept them all (no reacquire follows
  a swapchain's last presents), so a game recreating its swapchain hundreds of times walked toward the 4096 bound and
  lost its transport; the harness's devices now go with `0 retained present semaphore(s)`. This replaces the rule in
  "The queue and semaphore contract" (retain until present-completion evidence): the application's valid destroy is
  that evidence. The semaphores are destroyed after the call down returned, in case an implementation finishes its
  pending presents inside it.

### The review

DEVELOPMENT.md section 3's review for a Vulkan resource change and a non-NR-path touch was done as an independent
read-only review of `ea506f5d`. It found the barriers, the ticket accounting, the lock order, the off paths and stage
2's transport sound, and three defects in ownership and teardown, all fixed in `1894b1ae`: the token held for the
session when the device's presents stopped reaching the model's decision; the token leaked on a lost device; NGX and
the core never shut down when the model had stood down before the device went (the forwarder left answering
"initialised" for a destroyed device, the core marked down while initialised). And risks, fixed with them: a discard
after a device-lost submit released what the recording built; the pass's slot ring could alias with discards between
frames in flight; a too-small swapchain could hold the model; extensions added to another vendor's device; the KHR
buffer device address on a 1.0 instance without `VK_KHR_device_group`; push descriptors beside a 1.4 features struct.
Left as risks: below.

### The log

The install line says `(cold NR on zero guides; stage 3 of nr-vulkan-present-host.md)`. The device line ends with
what the host did for the model:

```
DLSS-NR Vulkan present host: device 0x14565F0 #4 created by vk-present-host-nr.exe (native Vulkan); physical 0x1470F08 instance #1; queue families 0:0xFx16 1:0xCx2 2:0xEx8 3:0x2Cx1 4:0x4Cx2 5:0x10Cx1; requested family 0 x1 family 2 x1; NR extensions: added VK_NVX_binary_import, VK_NVX_image_view_handle, VK_KHR_push_descriptor, VK_KHR_buffer_device_address, VK_KHR_format_feature_flags2; already enabled none; not possible none; bufferDeviceAddress enabled by the host
```

The swapchain's creation line adds `; the model may run on it (B8G8R8A8_UNORM, sRGB non-linear)` or `; the model will
not run on it: <the refusal>`. The model's own lines, once each, logged after the host's lock is released:

```
DLSS-NR Vulkan present host: the model initialised on device 0x14565F0 (cold: no application NGX initialisation; the driver's NGX core brought up on it)
DLSS-NR Vulkan present host: the model's images built for 632x446 (working 632x446), zero guides (constant depth, no motion) at the working size
DLSS-NR Vulkan present host: the model's feature is up at 632x446 (frame 632x446)
DLSS-NR Vulkan present host: the model runs at present on 632x446 (working 632x446), on zero guides
```

and, when it ends: `the model is off on this device: <why>` (latched: no forwarder, an old forwarder, no model, NGX
init or the core refused, a feature refused with its NGX result, a fault inside the model), `the model stood down and
was freed`, `NGX shut down on the device before it goes (returned 1)`. The window line (`(cold NR)` now) and the
swapchain's destruction line end with the model's counters since the window's first present -- evaluated and refused
count only presents the round trip carried; the pass's GPU time is the timestamp pair around encode -> model ->
resolve, read when the frame is seen complete:

```
DLSS-NR Vulkan present host (cold NR): window 0x1008A (10 presents); presents 10, swapchains 1, devices 1; swapchain 0x55558F4D0370 #5 632x446 format 44 colour-space 0 images 4 usage 0x13 (transfer-src yes, transfer-dst yes, storage no) surface-usage 0x9F sharing EXCLUSIVE; round-trip on as created; device 0x14565F0 #4 physical 0x1470F08 instance #1; queue 0x1456638 family 0 index 0 flags 0xF (graphics yes, compute yes, transfer yes); waits 1-1; acquires 10; caller vk-present-host-nr.exe (native Vulkan); transport: transported 10, skipped 0, waits taken over 10, submits 10, completions 9, ring 1, max CPU wait 0.000 ms, routes same queue 10; NR: evaluated 10, refused 0, history resets 1, pass GPU last 2.400 ms, mean 3.265 ms, max 9.545 ms
```

Refusals are named with their counts and the first one's text is kept: `NR: evaluated 0, refused 36 (model unavailable
36), history resets 0, pass GPU not measured; first refusal: nvngx.dll_dlssnr.dll is from a different build`. The
reasons: `NR off`, `colour`, `device extensions`, `format features`, `too small`, `after-upscale route`, `D3D12
route`, `another device`, `another swapchain`, `model unavailable`, `model busy`, `record failed`.

### Validated (2026-10-14, RTX 4090, driver 615.78.08, model 310.8.SF from the local kit)

- `tests/vulkan-present-host`, 20 cases, all PASS, before the review's fixes and again after them (`1894b1ae`): the
  whole suite runs in about a minute. The 14 of stages 1 and 2 hold the new lines; they run with no model
  beside the DLL, so every carried present is refused (`model unavailable`) and every read-back stays exact, and their
  devices now go with no present semaphore retained. Six new cases (`nr.cpp`) under **Proton** (GE-Proton11-7), the
  validation layer with synchronization on in every one:
  - `nr-control` (`ApplyModel=false`): the model evaluated on all 48 SDR presents -- B8G8R8A8_UNORM and _SRGB (all the
    surface offers under winevulkan here; no RGBA8, no HDR), the same from the compute-only family 2 across families
    (36 submits for 12 presents), and a recreation at 517x389 (not a multiple of eight, the model rebuilt there) -- and
    every read-back **byte-exact**: the conversion out, the pass and the conversion back are lossless when the edit is
    not applied. NGX shut down on the device at its destruction, `the model freed`.
  - `nr` (`ApplyModel=true`): the same 48 evaluated; every SDR read-back changed, by a mean of 3.0 eight-bit steps per
    channel (largest frame mean 3.2, largest single change 40), alpha untouched, never black. The pass cost about
    2.4 ms on the GPU at 632x446 (the first frames up to 9.5 ms).
  - `nr-working-scale` (`WorkingScale=0.5`): evaluated on all 36, the images built at `working 259x195` for 517x389.
  - Refusals, every read-back exact: `nr-no-extensions` (the test layer hides `VK_NVX_binary_import`; the device line
    says it is not possible, every present refused for `device extensions`), `nr-old-forwarder` (the kit's forwarder
    of 2026-09-03, which predates `dlssnr_abi_version`: `is from a different build`), `nr-no-model`.
  - Validation: **no report of the host's**. NVIDIA's model makes one per feature build, in what it records into the
    host's buffer: `VUID-vkCmdDraw-None-09600`, its own image `nv.ngx.dlssnr.resource` used in GENERAL at its first
    evaluate without ever leaving UNDEFINED. The application keeps reports naming an `nv.ngx.*` object apart by VUID;
    the runner accepts that one, at most once per build, and nothing else.
  - Plain Wine is not enough: the snippet comes up, but the driver's core answers FAIL_PlatformError (0xBAD00002) even
    with the prefix set up by hand as Proton sets it up (the core in system32, the NGXCore key, dxvk-nvapi), as the
    D3D12 loopback found. The cases run under `proton run` for that.
- `tests/nr-vk-present-host` (host tier): the colour plan, the extension plan, bufferDeviceAddress, the counters and the
  lines, the arbiter; source guards for the ordering above. `python3 tests/run_all.py` 49/49; `tests/vulkan-overlay
  --ref HEAD` PASS at `aa122612` and again with the review's fixes in; clang-format 22 clean on every C++ file touched.

### Not validated

- **Any game.** What a DOOM Eternal run should show is below.
- Image quality: the picture is synthetic and the guides are zeros, so anything that moves smears (with `ZeroGuideReset`
  it does not ghost, and loses the history that smooths); synthesized motion (`SynthMotion`) is D3D12 only. Stage 5.
- Cost at game resolutions (the pass at 632x446 is 2.4 ms; at 3440x1440 it is several times that), and the CPU time of
  the first frame, which brings the core and the model up inside a present.
- RGBA8 swapchains and every HDR refusal under a real surface (none offered here: host tier only); CONCURRENT images;
  a device lost with model frames in flight; a failed submit carrying one; the live toggles (NR off and on, the working
  scale) in a run; the after-upscale route, or the D3D12 route, running beside the host (the arbiter is host tier only);
  the bufferDeviceAddress-off-in-the-chain path; a translated title with the model; Streamline and FG (stage 4).
- Native Windows.
- Risks the review left open: the first frame brings the core and the model up inside the game's present, holding
  that queue's present lock and the transport's lock (every device's presents wait); the host brings
  `NVNGXProxy`'s process-wide core up, which upstream's DLSS path then finds up (a game initialising NGX after the
  host started shares that initialisation); a queue NGX retrieves with `vkGetDeviceQueue` is recorded as one the
  application retrieved; the zero guides are cleared for one work family per device (NVIDIA has one graphics
  family); `DlssNr::IsRunning()` is read without its lock.

### The first DOOM Eternal run, and the way back (2026-10-14)

The stage-3 build, merged into `feat/neurotic-parity` as `d10abe21`, ran in DOOM Eternal (GE-Proton11-7, 3440x1440,
the bundle on). What it showed, from `OptiScaler.log`:

- **Cold NR at present works in a real game.** At 19:45:24 the model came up on the game's start window (836x1356),
  `the model runs at present`, 294 frames evaluated, pass GPU mean 3.06 ms (last 2.85, the first frame 10.1).
- **The stand-down works.** The game created its DLSS feature at 19:45:25.45 (OptiScaler's NGX, handle 1000000); the
  route's first evaluate, 19:45:26.497, was refused (`this route waits`), 12 ms later the host's next present stood the
  model down and freed it, and the route took it: `feature up at 836x1356`, then 3440x1440 when the game rebuilt its
  feature (handle 1000001) at 19:45:31-32.
- **The way back did not exist.** At 19:46:30.87 the player turned DLSS off (`NVSDK_NGX_VULKAN_ReleaseFeature` of
  1000001). For the next 46 s there was not one `EvaluateAfterUpscaleVk`, and the present host never brought the model
  up again: the 10,000-present window line at 19:46:35 reads `NR: evaluated 294, refused 9706 (NR off 105, after-upscale
  route 9601)` (the 105 are F7). DLSS came back at 19:47:16 (handles 1000002, then 1000003 at a 2293x960 render) and the
  route resumed. The cause: the arbiter refused the host with `the after-upscale route ... owns the model` for as long
  as the route held the token, and nothing ever released it -- a game that stops calling its upscaler simply stops
  calling; the route has no moment of its own to give the model up.
- **The transport kept every present**, cross-family from the compute queue as in stage 2: every window and swapchain
  line in the log reads `skipped 0` and `max CPU wait 0.000 ms` (1 + 1,186 presents of the start swapchains, then a
  window of 10,000: 1,916 on the same queue, 8,084 across families).
- **No exit lines are normal for this title.** DOOM Eternal never destroys its device or its last swapchain at exit:
  no `destroyed` line for either, in all three runs, stage 2's included. So the `NGX shut down on the device before it
  goes` / `the model freed` lines a harness shows will not appear here, and their absence is not a fault.

**The fix: the way back** (`b19d88c7`; arbiter `DlssNr_VkModelOwner.h`, host `Housekeep` in `DlssNr_VkPresentHost.cpp`,
the route's side in `DlssNrFeature_Vk.cpp`):

- Once the route has been out of an evaluate for `kAfterUpscaleIdleMs` -- **10 s**, counted from the end of its last
  one (a guard in `EvaluateAfterUpscaleVk` stamps it on the way out: the route records after it asks, and a cold feature
  build inside one evaluate can take a while; never while it records) -- the host may take the model back on the
  route's device. Ten seconds is twenty times the live window: long against what pauses a game's upscaler without the
  player turning it off (a loading screen, a menu, a feature rebuild: DOOM made two features two seconds apart when DLSS
  came back), where a hand-back costs two rebuilds of a 3440x1440 feature for nothing; short against a player who
  turned DLSS off and waits for the picture to change (the run sat 46 s). A pause shorter than the window never loses
  the model, however often it happens.
- It is begun only by a present of the route's device on which the host has no model of its own, NR is on, the model's
  extensions are there and a carried swapchain is one the model may see (SDR, its working colour possible): a hand-back
  the host could not use would only free the route's objects for the route to build again.
- **The drain.** Every queue of the device the application retrieved is try-locked (its present lock, then its submit
  lock, as a borrower takes them; one another thread holds is tried again at the next present: the host never waits
  here), a fence is made per queue, the hand-back is marked begun in the arbiter (`BeginHandBack`, refused if the route
  asked in the meantime), and each fence is submitted on an empty batch (`vkQueueSubmit` with count 1, a non-null
  `VkSubmitInfo` with `sType` set and all other fields zero, and the fence), so it signals once everything submitted to its queue before it has completed -- the command
  buffers the route last recorded into among them. The fences are polled at later presents, never waited for.
- **Finish.** When every fence was seen signalled, the route's lock is try-locked (held means the route is evaluating:
  next present) and the arbiter finishes the hand-back: **Done** releases the token and the route's objects are freed
  -- its feature, images, pass, meter readback, parameter block and timestamp pool, with no device-wide wait (the drain
  proved them idle; `ShutdownVk(true)` still waits first) -- or it is **Cancelled**: the route asked at any point after
  the hand-back began, recording into a buffer that may be submitted after the fences, and it keeps the model and
  everything of its. NGX stays initialised on the device through the forwarder (it is the device's): the host's model,
  made again at its next frame, finds it up (`dlssnr_vk_init` answers 1 for the device it initialised), finds the
  forwarder's float slot again for its own parameter block, and builds its own images and feature. Nothing of the old
  present model is left: it was freed when it stood down. The hand-back is the drained device's: finishing or calling
  off another device's does nothing, and a route that moved to another device while a drain was under way has called
  it off (its asking anywhere does).
- **Again and again.** DLSS on after a hand-back is the ordinary stand-down; the route, taking the token from nobody,
  finds the forwarder's float slot again for its block (the two blocks may keep floats in different slots). Its `this
  route waits` line is said at each change now, with `this route has the model again` when it takes it.
- **Failure paths.** A drain submit that fails calls the hand-back off (the fences already submitted are only waited
  for; on a lost device nothing is destroyed) and is not tried again for another ten seconds. A fence that never
  signals keeps the hand-back open and frees nothing (fail closed), said once after the 2 s drain bound; a device with
  no queue on record (the host came in late) is never handed back, said once. At `vkDestroyDevice` a hand-back under
  way ends within the drain's bound, and the route's objects on the device go before the call down and before NGX is
  shut down on it (`AfterUpscaleDeviceGone`): freed once the host has drained every queue of the device behind fences
  of its own, waited for within the 2 s bound (`DrainForDestroy`; the application destroying the device uses none of
  its queues), or -- the device lost, work of the host's on it never seen complete, the drain not finished -- only
  forgotten, and NGX on the device then only forgotten too; and its token is given back. A route that held the model on a device the game replaced, and never asked again, no longer holds it for
  good; a later device with the same handle value no longer finds the route's stale handles; NGX is no longer shut down
  under the route's live feature.
- **The assumption it rests on**: a game submits what it recorded within ten seconds of recording it. A game that
  re-submitted a command buffer recorded once, long ago, with the route's pass in it, would see its objects freed under
  it. No engine known here does that with its upscaler's buffer; a game that does would show a device lost after a
  hand-back.

**The review.** An independent read-only review of `b19d88c7` found the fence semantics, the queue try-locks, the
cancel's atomicity (every recording after the hand-back began is preceded by an ask under the owner's lock, and the
free holds the route's lock from the token's release to its end), the lock order with the route's lock taken inside
the transport's, the free on the present thread, the forwarder's one-device state and the off/on/off cycle sound. It
found two bugs, both fixed in the commit after it: the idle clock started at the route's ask, so one evaluate longer
than ten seconds (a cold NGX bring-up and feature build) could have been handed back under its own recording -- now it
counts from the evaluate's end, and never while the route records; and a drain under way on one device when the route
moved to another, the first then destroyed or lost, left the hand-back open for the session (its cancel matched the
route's new device) -- the arbiter now keys the hand-back on the drained device. And the destroy path, which broke the
forwarder's "every feature released" precondition before `Shutdown1` (as stage 3 already did): its first fix, a
`vkDeviceWaitIdle` before the free, was itself reviewed again and replaced -- unbounded under the transport's lock on a
hung GPU, its result ignored on a lost one -- by the bounded fence drain above, whose failure forgets both the route's
objects and NGX on the device. Left as risks: the ten seconds, not the fences, is what covers a command buffer recorded before the hand-back began but
submitted after it (a game holding one across a stall, or re-submitting one recorded long ago); a settings menu kept
open past ten seconds -- where players toggle DLSS -- costs a needless hand-back and a rebuild when the game's upscaler
comes back. The game releasing its own feature (`NVSDK_NGX_VULKAN_ReleaseFeature`, which OptiScaler intercepts) would
be a stronger signal that its buffers are done, but DOOM releases and recreates its feature within a second on a
resolution change with DLSS still on, so it can only shorten the wait, never replace it; not done. The model's and the
route's frees log under the transport's lock, as a stand-down's release always has.

**The stand-down under slow frames.** The first full run of `nr-route-quiet` after those fixes ran with the host tier
in parallel, and the harness's frames slowed to about a second each. The stand-down then flapped: refused at the
route's evaluate, the host stood down at its next present, but the route's next ask came more than half a second
later, so the host took the model again, stood down again, and only then did the route get it (`stood down and was
freed` twice in a row, a fourth feature build of the host's). Any game whose upscaler evaluates less than twice a
second -- a loading screen still evaluating, a loaded machine -- would flap the same way, two feature rebuilds a time.
The arbiter now remembers a refusal: from the route's refusal until it takes the model, the host is refused
(`kAfterUpscaleWaiting`) and, if it still holds the model, must yield, however long the route's frames take; a route
refused once that never asks again lapses after the same ten seconds as the hand-back, and the host then takes the
model back. The harness's judged phases also go on until they have a dozen frames, so a slow machine thins them
instead of emptying them.

The lines, in the order a DLSS off/on cycle makes them (`nr-route-quiet`'s run; device and sizes vary):

```
DLSS-NR Vulkan: this route waits: the Vulkan present host holds the model; it stands down for this route at its next present
DLSS-NR Vulkan present host: device 0x14572D0 #4: the model stood down and was freed
DLSS-NR Vulkan: this route has the model again (the present host stood down)
DLSS-NR Vulkan present host: device 0x14572D0 #4: the after-upscale route has not asked for the model for 10 s: taking it back, once the device's 2 queue(s) drained
DLSS-NR Vulkan: the after-upscale route's model was freed for the hand-back (its feature at 1280x720, its images, pass and parameter block); NGX stays initialised on the device
DLSS-NR Vulkan present host: device 0x14572D0 #4: the model is the present host's again (the drain took 95 ms)
DLSS-NR Vulkan present host: the model initialised on device 0x14572D0 (cold: ...)
```

Frames refused while the queues drain are counted as `after-upscale route` like the ten seconds before them; when one is
a window's first refusal its text reads `the after-upscale route has not asked for the model for ten seconds; it is
being handed back to the present host`. A hand-back the route called off says `the hand-back is called off: the after-upscale route asked for the model again
while the device drained; it keeps it`.

Validated: `tests/nr-vk-present-host` (the arbiter through three DLSS on/off cycles, a long evaluate never handed back
under its own recording, a hand-back called off, a failed drain, twenty pauses just short of the window, the device
going, the review's device move during a drain; source guards for the order of the drain and the free, the recording
guard, the bounded drain at destroy), and **`nr-route-quiet`** in `tests/vulkan-present-host`, a 21st case under Proton
with synchronization validation: a game initialising NGX through OptiScaler's exports with OptiScaler's FSR 2.2 behind
them, 30 evaluates (the route running the model at 1280x720), the feature released, then presents only, twice; then on
once more and the device destroyed with the route holding the model. Every read-back exact from the stand-down until
9 s after the last evaluate (about 80 each time), every one edited again from 12 s; the hand-back's lines in order
twice, drains of about 100 ms at the harness's rate under validation, six feature builds (three of each), nothing called
off or abandoned, the route's objects freed at exit before NGX was shut down (returned 1), no validation report of
anyone's but the model's known one. Not validated: a game (the next DOOM run, below), a render thread recording while the hand-back
drains (one thread does both in the harness: host tier only), a fence that never signals.

### DOOM Eternal: the downstream wDx12 submit hook rejected the drain

The game run on build `cec50409` reached the ten-second hand-back, but every attempt was called off. The reported
log lines were (device handle omitted):

```text
[E] Invalid parameters to hk_vkQueueSubmit
DLSS-NR Vulkan present host: device ...: a hand-back drain submit returned -3; the hand-back is called off, the after-upscale route keeps the model
```

The pair repeated every ten seconds; with DLSS off the route kept the model and NR never ran at present. The
cancel path safely destroyed the fences that were never submitted. The failure was not a lost device or a leak.
`dev.fn.queueSubmit` can reach `Vulkan_wDx12::hk_vkQueueSubmit` below the host; the earlier claim that the trampoline
is below every hook was too strong. That hook rejects `pSubmits == nullptr` with
`VK_ERROR_INITIALIZATION_FAILED` (-3), even though
[Vulkan permits a zero-count/null-array submit](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html).

Both fork-owned fence-only submits now pass count 1 and an empty `VkSubmitInfo` with its `sType` set:
`StartHandBack`, and `DrainForDestroy` (the bounded destroy drain introduced by `559a54a7`). There are no command
buffers, waits or signals in the batch; the fence still follows prior queue work. The after-upscale implementation
in `DlssNrFeature_Vk.cpp` records commands but has no queue-submit call; the audit found no other zero-count/null-array
submit or Submit2/KHR equivalent under `retoque/`. No upstream hook is changed.

The host-tier guard now scans all fork C/C++ sources for that pattern, including Submit2/KHR and dispatch-table
spellings; it failed on both drain sites before the fix. The existing `nr-route-quiet` passed because it creates
`optiscaler_skip_vulkan_hooks`: `VulkanHooks::Hook` returns before installing wDx12. The new
`nr-route-quiet-wdx12` companion leaves the bundle enabled, disables only the overlay, and clears the diagnostic
wDx12-disable environment override. A deliberate zero-count/null-array probe through the application's device
proc address must return -3 and produce exactly one rejection line, proving the actual production hook is active;
no further rejection is permitted during two hand-backs and the bounded device-destroy drain. It uses the same
read-back, ownership-order and synchronization-validation checks as the original case. A real DOOM retest remains
separate; this change does not deploy a DLL into the running game.

The companion's negative control on the retained pre-fix build `80374044` reproduced both -3 hand-back failures.
Its bounded destroy drain also failed, so the route's objects were abandoned and validation reported
`VUID-vkDestroyDevice-device-05137` (live child objects at destroy). This is distinct from the safe cancellation of
never-submitted hand-back fences. On the fixed build, the first companion run completed the two hand-backs and
clean teardown with zero host validation errors, but the checker falsely called four exact read-backs in its first
nine-second window an early takeover because it required five. The bounded waiting windows now require a nonempty
sample and reject every changed frame; unlike the post-hand-back phases, their length cannot be extended for a slow
machine without crossing the hand-back deadline. An empty sample and a sample with one changed frame are rejected.

Final validation on build `da4988a1`: all 49 host suites passed; `nr-route-quiet` and
`nr-route-quiet-wdx12` both passed under Proton with synchronization validation. The wDx12 case saw exactly the
one deliberate probe rejection, drained in 94 and 96 ms, read back 79 exact frames in each early waiting window,
then edited every judged post-hand-back frame, and freed the route's objects before NGX shutdown at exit. Both
cases had zero host validation errors and six occurrences each of the model's already-allowed
`VUID-vkCmdDraw-None-09600`. This proves the hook-chain regression in the harness, not a new DOOM game run.

### Second DOOM cycle: hand-back diagnostics and a window heartbeat

The 2026-10-14 run on `c8d9df7f` (the parity branch's integration of `da4988a1`) proves the first hand-back:
at 22:23:10.473 the host drained four queues in 5 ms and resumed NR at 3440x1440. At 22:24:43 it stood down again.
The last exposure-change message is at 22:24:59.768; `ReleaseFeature(1000000)` is logged at 22:25:12.212, then
nothing through the reported continued rendering and quit. There is no second new-queue line after 22:24:43.

This does not identify a second-cycle runtime defect. Exposure is logged only after a change above its threshold,
not at every evaluate. F7 turning NR off returns before the route asks for the model and also prevents the host
from taking it back; continued evaluates at stable exposure can instead keep the route non-idle. The release line
precedes an unbounded `vkDeviceWaitIdle` and two 500 ms sleeps in upstream; it is not proof that release returned.
Release does not relinquish the NR owner token. The existing real-hook harness already releases its NGX feature
after the last evaluate on each of two cycles and successfully hands the model back.

The diagnostic change preserves ownership and the ten-second quiet window. Once a hand-back is due, it names each
blocking reason once per due episode: NR off, a model still present, retry backoff, unavailable device/model/instance,
a yielding route, or no eligible swapchain (with individual reasons). It also names no recorded queues, fence-create
failure and a refused begin. A busy queue names its handle, family, index and present/submit lock, after one second
of failed attempts; changing reasons cannot produce one line per frame. Success or no longer being due resets the
episode. Re-enabling NR incurs no new quiet window. Messages queued by Housekeep are flushed before early present
refusals, too; previously such a message could stay queued until a transport path reached the later flush.

Queue locking stays try-only. Present already holds its own present lock and the transport mutex; another present
may hold another queue's present lock while waiting for that transport mutex. Waiting for that queue here would
close a cycle. Acquiring each queue's present then submit lock with try-lock, retaining them only for a successful
attempt, gives them all back on any refusal and retries at the next present. It adds no timed wait to a present and
makes progress once the locks are free, but cannot promise fairness against continuous contention. No observed
lock failure in this DOOM log justifies changing that policy or submitting without the required synchronization.

Each window additionally emits its usual cumulative transport/NR counters on the first present at least 60 seconds
after its previous line, with reason `60 s heartbeat`. Event/decade lines reset that deadline. No presents means no
heartbeat; a long gap emits one current line, not catch-up lines. The clock is monotonic and tests inject its value.

Validation on build `bfe1b517` (`20261014_224714`): all 49 host suites passed in the memory-capped scope.
The host suite covers F7-off readiness while already quiet, immediate admission on re-enable, real present/submit
mutex contention with one delayed diagnostic and progress after release, and fake-clock heartbeat boundaries.
`nr-route-quiet` and `nr-route-quiet-wdx12` passed both NGX-release cycles. The new `nr-route-toggle-wdx12`
passed both release/F7-off/F7-on cycles through the real wDx12 hook: exactly one NR-off blocker per cycle,
hand-back starting 96.698/100.320 ms after re-enable and draining in 96/98 ms, exact disabled/route read-backs,
and changed post-hand-back frames. All three have zero host validation errors; the six already classified
`nv.ngx.*` model reports per case remain separately counted.

The toggle case enables the legacy overlay to exercise its actual F7 handler, using window-local messages.
Its first run exposed the overlay's missing device-exit cleanup (54 live objects), unrelated to hand-back.
The harness now creates/destroys a temporary Win32 surface after all measured phases to invoke the overlay's
existing cleanup path; validation suppresses no leak report. Startup splash and F7-toast frames are kept outside
the byte-exact route/disabled checks. This tests real input and ownership transitions, not ordinary overlay exit,
concurrent game queues or DOOM's exact call sequence. No second-cycle stall was reproduced and no game was deployed.

### What the next DOOM Eternal run should show

Deployed with `nvngx.dll_dlssnr.dll` from the same build, `[DlssNr] Enabled=true`, `VulkanPresentHost=true`, as for the
first run:

- At start, as before: cold NR at present on the start window, then the game's DLSS feature and one `this route waits`,
  `the model stood down and was freed`, `this route has the model again (the present host stood down)`, the route's
  `feature up at 3440x1440`.
- **DLSS turned off in the game**: about ten seconds of frames refused as `after-upscale route` (the picture unedited:
  the route no longer runs and the host waits), then `has not asked for the model for 10 s: taking it back, once the
  device's N queue(s) drained`, `the after-upscale route's model was freed for the hand-back (its feature at 3440x1440,
  ...)`, `the model is the present host's again (the drain took M ms)` -- a few frames' worth at the game's rate --
  and the host's `the model initialised on device`, `images built for 3440x1440`, `feature is up at 3440x1440`, `the
  model runs at present`. The next window line's `NR: evaluated` climbs again; `pass GPU` is the cost at 3440x1440.
- **DLSS on again**: `this route waits`, `stood down and was freed`, `this route has the model again`, the route's
  `feature up at` (its first frames rebuild the feature: a hitch, as at any DLSS toggle). **Off again**: the hand-back
  again, ten seconds after. Any number of times; never `called off` unless DLSS comes back within the drain's few
  frames.
- No exit lines (above). A `device lost` or a crash after a hand-back would point at the assumption above; the log's
  last hand-back line would be the place to start.

### What a DOOM Eternal run should show (DLSS off, so the after-upscale route stays idle; written before the first run)

With `[DlssNr] Enabled=true`, `VulkanPresentHost=true`, and both `nvngx.dll_dlssnr.dll` **from this build** (an older
one has no `dlssnr_vk_shutdown`) and the model beside `winmm.dll`; the bundle on, as before:

- The install line ending `beside OptiScaler's own Vulkan hooks`. The bundle's own `DLSS-NR Vulkan: device offers ...
  added here: [VK_NVX_binary_import, ...]`, then the host's device line `created by winmm.dll (OptiScaler's own Vulkan
  hook, called down to this one)` with `NR extensions: added none` (or `VK_KHR_format_feature_flags2`) `; already enabled
  VK_NVX_binary_import, VK_NVX_image_view_handle, VK_KHR_push_descriptor, VK_KHR_buffer_device_address; not possible
  none` and one of `bufferDeviceAddress already enabled`, `enabled by the host`, or `off in the application's create
  info, not edited`.
- Its swapchain's creation line `; the model may run on it (B8G8R8A8_UNORM, sRGB non-linear)` -- or, with the game's
  HDR on, `; the model will not run on it: an HDR10 swapchain ...`, and nothing else changes.
- At the first present: `the model initialised on device ... (cold ...)`, `the model's images built for 3440x1440
  (working 3440x1440)`, `the model's feature is up at 3440x1440`, `the model runs at present on ...` (the first present
  is slow: NGX and the model come up inside it).
- Window lines from the compute family 2 (`graphics no, compute yes`), `routes cross-family` as in stage 2's run, and
  `NR: evaluated` close to `transported`, few or no refusals (`model busy` if more than four of its frames queue up),
  `history resets` at the start and after a gap, and a `pass GPU` time per frame to judge the cost by (expect several
  milliseconds at 3440x1440; `[DlssNr] WorkingScale` lowers it).
- On screen: the game's picture edited by the model, smearing under camera motion (zero guides); `ZeroGuideReset=true`
  trades the smear for no history. The Vulkan menu drawn after NR (application -> NR -> overlay -> present).
- At exit: the device's destruction line ending `; the model freed`, after `NGX shut down on the device before it goes
  (returned 1)`.
- Turning DLSS on in the game makes the after-upscale route ask for the model: one `the model stood down and was freed`,
  then refusals counted as `after-upscale route`, and the route's own `DLSS-NR Vulkan:` lines take over.

## Decision and stop conditions

Prefer this experiment over implementing the D3D11 dual-swapchain host first. It removes an entire
transport layer and can cover both translated APIs, while retaining this fork's actual Vulkan
neural implementation. It does not remove the need for owned state, a full present semaphore chain,
safe retirement, format conversion or synthetic motion, and it may process UI already drawn above it.

The universal-Proton claim fails if the required translated present/device calls cannot be reached
early enough, the translated device lacks enabled NGX capabilities, or common swapchains cannot
be legally read/written on a supported queue without much broader ownership interception. The
FG-compatible claim fails if an NR-only correctly traced submit reproduces pacer timeouts or if
rendered/generated frames cannot be distinguished with acceptable cost. Unacceptable latency or
motion/HUD quality can defeat general use even when all API calls succeed. Any of these findings
keeps the D3D12 host relevant; none justifies silently enabling the legacy Vulkan overlay everywhere.

This note's evidence is a manual source/history audit and the cited public documentation/report,
not a new GPU run. No implementation, marker edit, installation or game launch is part of this task.
Local documentation links, whitespace and the fourteen-suite registry listing were checked; the
suites themselves were not run for this documentation change.
Eventual C++ uses the pinned `/usr/lib/llvm20/bin/clang-format`; this documentation needs no build. (2026-10-14:
the pin is clang-format 22 through `uvx` now, and stage 1's C++ is clean under it.)
