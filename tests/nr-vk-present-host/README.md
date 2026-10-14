# Vulkan present host, stage 1 (host tier)

Run `python3 tests/nr-vk-present-host/run.py`. Pure Python source guards, then the production
`retoque/dlssnr/DlssNr_VkPresentRegistry.h`, as it is, compiled with g++ C++20, ASan/UBSan and `-Werror` against
the real Vulkan headers in `external/vulkan`, and driven by `cases.cpp`.

Design: `docs/dlssnr/design/nr-vulkan-present-host.md`, section "Stage 1 as built".

## What this covers

The registry's state machine, with plain integers for handles as the header itself uses:

- The Vulkan constants the header repeats so it compiles alone, held to `vulkan_core.h`.
- Caller classification by module (DXVK, vkd3d-proton, the loader, OptiScaler's own module even when it is named
  `dxgi.dll`, native) and the round-trip verdict from a swapchain's usage and its surface's.
- The observe line, field for field, at a window's first present; at 10 and 100 presents, with the wait range of
  the presents since the previous line (0-2 when one had none and one had two) and the window starting over after
  each line; on a new queue, a new caller, a new swapchain (through `oldSwapchain`, the old one retired and still
  presentable); a present of an image never seen acquired.
- Two devices alive at once whose swapchains have the same handle value (non-dispatchable handles are only unique per
  device): no aliasing, each present on its own window and device record. A device destroyed takes its queues and
  swapchains; a new device with the old handle value, and a queue and swapchain with the old values too, are new
  records with new ids; a create that returns a handle still on record replaces it and is counted.
- A swapchain never seen created (one line per session, then counted), a queue never seen retrieved (the swapchain
  still names device and window), OptiScaler's own device (never reported), a surface never seen (`window unknown`),
  a multi-swapchain present with a pNext chain, out-of-range and unknown acquires, instance and device teardown, and
  the device line.

Source guards:

- One call site of `VkPresentHost::Install`, the first statement of `VulkanHooks::Hook`, ahead of its
  `vulkanHooksSkipped` check; nothing else of `Vulkan_Hooks.cpp` refers to the host. `Install` begins with the gate on
  `[DlssNr] Enabled` and `VulkanPresentHost`, before any lookup, lock, detour or record: off, it is one test and a
  return. The marker decides nothing in the host (it is read for the install line only).
- Observe only: the host never submits, creates, allocates, records, waits, copies a create or present info, or
  touches usage; never calls Reflex, the frame limiter, `TickFrozenCheck`, upscaler timing, the overlay, spoofing or
  the NR evaluate. Its own Vulkan calls are the three read-only queries. Every hook passes its own arguments down
  once, the present callback ends in `return next(queue, info)`, destroys retire the record before calling down, and
  the registry lock is never held across a call into Vulkan or the log.
- The present detour goes in at swapchain creation, not in `vkCreateDevice` (so it lands on top of the bundle's).
- `VulkanPresentHost`'s four config points and the ini comment's honest words; the project builds the files.

## What this does not cover

No Vulkan, no Detours, no Windows: the production glue in `DlssNr_VkPresentHost.cpp` is checked by source only here.
`tests/vulkan-present-host` (wine tier) runs the real DLL against real Vulkan under Wine.
