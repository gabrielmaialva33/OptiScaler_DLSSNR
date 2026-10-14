# Vulkan present host, stages 1 and 2 (host tier)

Run `python3 tests/nr-vk-present-host/run.py`. Pure Python source guards, then the production
`retoque/dlssnr/DlssNr_VkPresentRegistry.h` and `DlssNr_VkPresentTransport.h`, as they are, compiled with g++ C++20,
ASan/UBSan and `-Werror` against the real Vulkan headers in `external/vulkan`, and driven by `cases.cpp`.

Design: `docs/dlssnr/design/nr-vulkan-present-host.md`, sections "Stage 1 as built" and "Stage 2 as built".

## What this covers

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
- Two edits and no others: `imageUsage` of a copy of the swapchain create info, which goes down only when the plan
  (decided before the call) says the round trip runs and added something; and the waits of a copy of the present
  info, which goes down only when the round trip ran -- the pNext chain by pointer. Every other path hands the
  caller's own pointers down, once.
- The application's waits are consumed only by a submit that ran: recording precedes it, every refusal comes before
  it; the work queue's lock is taken before the transport's; nothing is logged or called down while the transport's
  lock is held. Retirement drains before it frees and destroys nothing of a lost device (DEVELOPMENT.md rule 8).
- What the host never does: Reflex, the frame limiter, `TickFrozenCheck`, upscaler timing, the overlay, spoofing, NR,
  NGX, a device-wide idle wait. Its own Vulkan calls are five read-only queries from vulkan-1.dll and the device
  functions it resolves per device; no prototype is called directly. Destroys retire the record before calling down,
  and the registry lock is never held across a call into Vulkan or the log.
- The present detour goes in at swapchain creation, not in `vkCreateDevice` (so it lands on top of the bundle's).
- `VulkanPresentHost`'s four config points and the ini comment's honest words; the project builds the files.

## What this does not cover

No Vulkan, no Detours, no Windows: the production glue in `DlssNr_VkPresentHost.cpp` is checked by source only here.
`tests/vulkan-present-host` (wine tier) runs the real DLL against real Vulkan under Wine, with the validation layer and
read-backs of the presented images.
