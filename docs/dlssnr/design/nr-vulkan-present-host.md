# NR at Vulkan present — one Proton host below the game's rendering API

Status: **stage 1 (coverage and no-op control) built 2026-10-14, observe only**; stages 2-5 are design.
See [Stage 1 as built](#stage-1-as-built-2026-10-14), which also records how the tree drifted from the
references below. Audited at `c3310a15` (2026-09-12), after reading
[nr-present-hook.md](nr-present-hook.md), [nr-dx11-bridge-host.md](nr-dx11-bridge-host.md), both
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
