# Vulkan present host, stages 1 to 3 (host tier)

Run `python3 tests/nr-vk-present-host/run.py`. Pure Python source guards, then the production
`retoque/dlssnr/DlssNr_VkPresentRegistry.h`, `DlssNr_VkPresentTransport.h`, `DlssNr_VkPresentNr.h` and
`DlssNr_VkModelOwner.h`, as they are, compiled with g++ C++20, ASan/UBSan and `-Werror` against the real Vulkan headers
in `external/vulkan`, and driven by `cases.cpp`.

Design: `docs/dlssnr/design/nr-vulkan-present-host.md`, sections "Stage 1 as built", "Stage 2 as built" and "Stage 3
as built".

## What this covers

The source guard scans every C/C++ file under `retoque/` for literal zero-count or null-array `QueueSubmit`,
`QueueSubmit2` and `QueueSubmit2KHR` calls (including dispatch-table names). Fence-only drains must use one empty,
non-null submit struct: DOOM Eternal's downstream wDx12 hook rejects a null array even when Vulkan permits it.
The guard covers the hand-back and bounded device-destroy drains; it does not perform pointer data-flow analysis
or change forwarded application submits. Its scanner is self-tested against each API and each bad argument alone.

The registry's state machine and the transport's decisions, with plain integers for handles as the headers use:

- The Vulkan constants the headers repeat so they compile alone, held to `vulkan_core.h` (queue flags, usages,
  sharing, the shared present modes, the protected swapchain flag, what the work queue needs).
- Caller classification by module (DXVK, vkd3d-proton, the loader, OptiScaler's own module even when it is named
  `dxgi.dll`, native).
- The usage plan at swapchain creation (`PlanUsage`): `0x12` on a surface offering `0x9F` becomes `0x13` with
  `TRANSFER_SRC added at creation`; `0x10` gets both; `0x13` is `as created`; and every refusal leaves the usage as
  asked and names only what is missing -- a surface without TRANSFER_SRC (or without both), a surface that could not
  be read (and that it does not matter when nothing needs asking), a device with no transport (OptiScaler's own), a
  protected swapchain, a shared present mode, more than one array layer, a format that does not allow the usages, a
  work image that cannot be made.
- The route at present (`ChooseRoute`): a graphics presenter does the round trip itself; a compute-only presenter
  (family 2, `0xE`) hands an EXCLUSIVE image to the lowest graphics queue the application retrieved (three submits);
  never to a queue created with flags, never to one the application did not retrieve; a CONCURRENT image listing a
  graphics family goes there without ownership transfers; a CONCURRENT image that lists neither, a present-only or
  sparse-only family, or an unknown family has no route, with the reason.
- The ring (`ChooseSlot`): grow up to four, reuse an idle slot, wait for the oldest pending one when full, never pick
  an abandoned slot.
- The counters and their words (`TransportStats`, `TransportText`): carried, refused by reason, waits taken over,
  submits, completions, ring depth, the longest CPU wait, routes.
- The registry: the window line, field for field, at a window's first present (now with the usage the application
  asked for beside the one it got, the round trip's state, and the transport's counters); at 10 and 100 presents,
  with the wait range of the presents since the previous line and the window starting over after each line; on a
  new queue, a new caller, a new swapchain (through `oldSwapchain`, the old one retired and still presentable); a
  present of an image never seen acquired. What the transport reads for a present (`ViewPresent`: the device, its
  families, its retrieved queues in order, the swapchain, the image, whether it was acquired since its last present,
  which a present then spends). The swapchain's creation line and its destruction line, with the completions observed
  at its retirement; the first no-route reason kept for the window.
- Stage 3's decisions (`DlssNr_VkPresentNr.h`): the colour plan (B8G8R8A8/R8G8B8A8, UNORM or sRGB, in sRGB
  non-linear; the sRGB formats through their UNORM twins; HDR10, scRGB, HLG, ten-bit, float and any other format
  refused with format and colour space named); the device-extension plan (the after-upscale route's four, the
  application's EXT buffer device address standing for the KHR one, nothing the device does not offer, nothing that
  needs properties2 on an instance without it, and `VK_KHR_format_feature_flags2` only where format-less storage writes
  are not already allowed); the bufferDeviceAddress feature (prepended only where no struct in the chain could
  conflict); the history-reset rule; the model's counters and their words (evaluated and refused only on carried
  presents, refusals by reason, history resets, pass GPU time last/mean/max, the first refusal's text); the NR part
  of the window and destruction lines; `ViewPresent`'s instance and retirement.
- The one-owner arbiter (`DlssNr_VkModelOwner.h`): the present host takes the model when nobody holds it, keeps it on
  its device and refuses another device; the after-upscale route asking is refused while the present host holds it
  and makes it yield; the present host stays down while the route asked within half a second and while it owns the
  model; releases only by the holder.
- The way back (DOOM Eternal, 2026-10-14): the route quiet for `kAfterUpscaleIdleMs` (10 s), counted from the end of
  its last recording (`AfterUpscaleDone`), is handed back, on its own device only, through `BeginHandBack` /
  `FinishHandBack(device)`; never while it records, however long one evaluate takes; refused for the present host with
  its own reason while it drains; three cycles of DLSS on (stand down), a minute of evaluates (never idle), DLSS off
  (idle only at 10 s, then Done and the present host takes it again); a hand-back called off by the route asking while
  it drains (twice, once going quiet again) and one cancelled for a failed drain, another device's cancel or finish
  changing nothing; twenty pauses a millisecond short of the window that never lose the model; a clock read out of
  order; the token following the route's own device change; `DeviceGone` for both owners and during a hand-back; and
  the review's case, a drain on d1 when the route moves to d2 and d1 then goes (destroyed, lost or drained): the
  hand-back ends and d2 can be handed back later. And frames slower than the live window: a route refused at T keeps
  the host down (`kAfterUpscaleWaiting`, and yielding if it still holds the model) until it takes the model 1.6 s
  later; a route refused once that never asks again lapses after the idle window, a clock read out of order not
  counting as quiet, and the host's `Take` ends the reservation.
- Two devices alive at once whose swapchains have the same handle value: no aliasing. A device destroyed takes its
  queues and swapchains; a new device with the old handle value, and a queue and swapchain with the old values too,
  are new records with new ids; a create that returns a handle still on record replaces it and is counted.
- A swapchain never seen created (one line per session, then counted), a queue never seen retrieved, OptiScaler's own
  device (never reported), a surface never seen (`window unknown`), a multi-swapchain present with a pNext chain,
  out-of-range and unknown acquires, instance and device teardown, and the device line.

Source guards:

- One call site of `VkPresentHost::Install`, the first statement of `VulkanHooks::Hook`, ahead of its
  `vulkanHooksSkipped` check; nothing else of `Vulkan_Hooks.cpp` refers to the host. `Install` begins with the gate on
  `[DlssNr] Enabled` and `VulkanPresentHost`, before any lookup, lock, detour or record: off, it is one test and a
  return. The marker decides nothing in the host (it is read for the install line only).
- Three edits and no others: `imageUsage` of a copy of the swapchain create info, which goes down only when the plan
  (decided before the call) says the round trip runs and added something; the waits of a copy of the present info,
  which goes down only when the round trip ran -- the pNext chain by pointer; and (stage 3) the extension list of a
  copy of a non-internal device's create info, with one bufferDeviceAddress features struct prepended to the
  application's chain when the plan says so, decided before the call. Every other path hands the caller's own
  pointers down, once.
- Stage 3: the model is decided before anything is recorded and before any wait is spent; its pass sits in the work
  buffer between the blit in and the blit out; a model frame counts only once the buffer carrying it was submitted
  (exactly the two submits that carry it, each on success), anything else discards it, and a pass that failed while
  recording is replaced by the plain round trip; it is seen complete only where a slot's fence was; it is freed only
  with nothing in flight and abandoned on a lost device or when frames never completed, the owner token returned
  either way; the claim comes after every cheap refusal; present semaphores of a destroyed swapchain are freed after
  the destroy returned. In `DlssNrFeature_Vk.cpp`: the after-upscale route asks the arbiter where it commits to a frame;
  the present model touches nothing of the route's state; its encode is passthrough and the downsample and resolve are
  copies of it; the working scale never goes above native; NGX's device shutdowns are called with their out count.
- The way back: begun only with no model of the host's on the device, NR on, a swapchain the model may see and the
  route quiet there; every queue of the device try-locked (present, then submit; never waited for), the fences made
  before and submitted after `BeginAfterUpscaleHandBack`, each on an empty batch; a failed drain submit calls it off
  and a lost device destroys nothing; the route freed only after every fence was seen signalled, its lock try-locked
  and the token released first, never after a failed drain submit, without a device-wide wait; the route records
  nothing before it asks, and a guard right after its claim ends its recording window when the evaluate returns; it
  finds the forwarder's float slot again when it takes the token from nobody; the device going ends a hand-back before
  the route's objects go and before NGX is shut down -- freed once the host drained every queue of the device behind
  fences of its own within a bound (`DrainForDestroy`; no device-wide wait anywhere), only forgotten, and NGX only
  forgotten too, when the device is lost, work on it was never seen complete or the drain did not finish -- and its
  token with them.
- The application's waits are consumed only by a submit that ran: recording precedes it, every refusal comes before
  it; the work queue's lock is taken before the transport's; nothing of the host's own is logged or called down while
  the transport's lock is held (what the model's or the route's frees log underneath it, as a stand-down's release
  always has, is theirs; the guard reads the host's functions only). Retirement drains before it frees and destroys nothing of a lost device (DEVELOPMENT.md rule 8).
- What the host never does: Reflex, the frame limiter, `TickFrozenCheck`, upscaler timing, the overlay, spoofing, NGX
  of its own (the model is `DlssNrFeature_Vk.cpp`'s), the after-upscale route's state, a device-wide idle wait. Its
  own Vulkan calls are eight read-only queries from vulkan-1.dll and the device functions it resolves per device; no
  prototype is called directly. Destroys retire the record before calling down,
  and the registry lock is never held across a call into Vulkan or the log.
- The present detour goes in at swapchain creation, not in `vkCreateDevice` (so it lands on top of the bundle's).
- `VulkanPresentHost`'s four config points and the ini comment's honest words; the project builds the files.

## What this does not cover

No Vulkan, no Detours, no Windows: the production glue in `DlssNr_VkPresentHost.cpp` is checked by source only here.
`tests/vulkan-present-host` (wine tier) runs the real DLL against real Vulkan under Wine, with the validation layer and
read-backs of the presented images.
