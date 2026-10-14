# Vulkan present host, stage 1 (wine tier)

Run from the repository root on Linux with Wine, a graphical session, working Vulkan, the native Khronos validation
layer and the repository's msvc-wine toolchain, after `./build-local.sh Release`:

```sh
python3 tests/vulkan-present-host/run.py               # every case
python3 tests/vulkan-present-host/run.py --only marker # one case
python3 tests/vulkan-present-host/run.py --dll PATH    # another build (default x64/out/OptiScaler.dll)
```

Design: `docs/dlssnr/design/nr-vulkan-present-host.md`, "Stage 1 as built". The host-tier half is
`tests/nr-vk-present-host`.

## How it works

Unlike `tests/vulkan-overlay`, nothing is instrumented: the DLL under test is the production `OptiScaler.dll` that
`build-local.sh` produced, refused if it lacks the host's line or is older than the host's sources. The runner
compiles `harness.cpp` (a Win32 Vulkan application, `/MD`, against `external/vulkan`) in a disposable copy of the
compiler prefix, `artifacts/build-prefix`, and runs every case in its own directory under `artifacts/cases/` in a
separate runtime prefix, `artifacts/wineprefix`, with OptiScaler as `dxgi.dll` beside the application.

The application never asserts anything about the host. Before loading OptiScaler it copies the first 16 bytes of
sixteen vulkan-1.dll exports; it loads OptiScaler, loads `vulkan-1.dll` again (the loader hook's late path), and
then drives real Vulkan:

- window 1, device 1 with the graphics family through `vkGetDeviceQueue` and, when the surface takes presents from
  one, a compute-only family through `vkGetDeviceQueue2` (family 2, flags `0xE`, on the RTX 4090 here);
- swapchain 1 without TRANSFER_SRC, 12 presents with one wait each; swapchain 2 through `oldSwapchain` with
  TRANSFER_SRC and TRANSFER_DST, presented from the second queue: two presents with no wait (after the host saw the
  clear complete), then two waits each, to the window's 100th present;
- window 2 and device 2, alive beside device 1, three presents; one more on window 1; device 2 destroyed and device
  3 created (here it gets device 2's handle value back), a new swapchain, three presents;

and writes what it did to `result.json`: handles, families and their flags, usages, image counts, every present in
order with its swapchain, device, queue and waits, and whether each export's first bytes changed by the time the
first swapchain existed. The runner models the observe lines from that list -- one at a window's first present, one
when its swapchain, device or queue changes, one at 10 and 100 presents -- and holds every field of every line in
`OptiScaler.log` against it, plus the install line, the late-path line, the instance line and one device line per
device (families, requested queues, physical device, instance id, distinct record ids even for a reused handle).

Validation is on (core and synchronization) and proved active per run by a deliberately invalid fence flag the layer
must reject (`tests/vulkan-overlay`'s negative-control technique); any other validation error fails the case. One
message is set aside and counted, `wine_injected_extension_messages`: Wine 11.19's winevulkan appends
`VK_EXT_external_memory_dma_buf` to the host-side device it creates, without `VK_KHR_external_memory_fd`, and the
native layer reports it (`VUID-vkCreateDevice-ppEnabledExtensionNames-01387`) at an index past the application's
one extension. It appears once per device whenever the bundle is skipped, with the host off as well
(`marker-key-off` counts 3); with the bundle installed, its spoofing's extension additions make it go away. The
host passes the create info down unchanged.

## Cases

| Case | Marker | `[Vulkan] SkipHooks` | `Enabled` | `VulkanPresentHost` | `OverlayMenu` | Must hold |
|---|---|---|---|---|---|---|
| `marker` | yes | | true | true | true | every line right; caller and creators the application; the bundle skip line; the 14 host exports patched, `vkQueueSubmit`/`vkCreateFence` not |
| `skiphooks` | | true | true | true | true | the same, through the ini instead of the marker |
| `marker-key-off` | yes | | true | false | true | no host text in the log; all sixteen exports byte-identical |
| `marker-master-off` | yes | | false | true | true | the same |
| `menu-off` | | | true | true | false | every line right beside the bundle with no menu (`OverlayMenu=false`, which `vulkan-overlay` must see as zero coverage, is covered positively here) |
| `menu-on` | | | true | true | true | every line right beside the bundle and its menu: the bundle's present hook is installed and the host's caller is still the application, so the host's present detour runs first; swapchain 1's usage shows the TRANSFER_SRC the overlay adds for its blur |
| `key-off` | | | true | false | true | no host text in the log |

With the bundle installed (`menu-off`, `menu-on`, `key-off`), its instance-level hooks run before the host's on the same exports, so
the instance and device lines name `dxgi.dll (OptiScaler's own Vulkan hook, called down to this one)` as creator;
the runner expects exactly that.

`menu-on` and `key-off` run `--one-window` (window 1 only), and leave device 1 and the instance to process exit,
because of two defects of the legacy Vulkan overlay that show with the host off as well:

- creating window 2's swapchain on device 2 crashes the process: a null read at `0x90(rbx)` inside
  `MenuOverlayVk::CreateSwapchain` (backtrace `dxgi+0x19a9ee` <- `+0x19ae83` <- `+0x27dcc0` <- `+0x23b672` <- the
  application's `vkCreateSwapchainKHR`, on the build of 2026-10-14), right after it logs
  `MenuOverlayBase::Handle() != _hwnd`;
- destroying a device the overlay drew on reports `VUID-vkDestroyDevice-device-05137`: the overlay's objects are
  still on it, and it has no device-destroy hook (`tests/vulkan-overlay` tears it down first through a test-only
  export the production DLL does not have).

### A D3D title under Proton's layers

`translated.cpp` is a second application: `--d3d11` clears and presents twelve frames through D3D11, `--d3d12` through
D3D12 and a DXGI flip swapchain. Its cases run in their own prefix, `artifacts/wineprefix-translated`, where DXVK
(`dxgi`, `d3d11`, `d3d10core`) and vkd3d-proton (`d3d12`, `d3d12core`) from a local Proton (GE-Proton first, or
`PROTON_DIR`) are copied into `system32` and overridden native, as Proton does; OptiScaler is `winmm.dll` beside the
application, so DXVK's `dxgi.dll` stays the real one. The marker is present, as deployed for such titles. No validation
layer: these cases are about which hooks the layers reach, not their own Vulkan usage.

| Case | API | `VulkanPresentHost` | Must hold |
|---|---|---|---|
| `dxvk` | D3D11 | true | a device line created by `d3d11.dll (DXVK)`; observe lines for the application's window at the first present and at 10 presents, every one with caller `(DXVK)` and the presenting device one DXVK created |
| `vkd3d-proton` | D3D12 | true | the same with `d3d12core.dll (vkd3d-proton)`; OptiScaler's own throwaway D3D12 device (IdentifyGpu's vkd3d-proton check, under `ScopedSkipVulkanHooks`) is recorded as OptiScaler's and never reported, and the title's device that gets its handle value back is a new record |
| `dxvk-key-off` | D3D11 | false | no host text, DXVK presents all twelve frames |

Without a local Proton holding both layers these three print SKIP and do not count as a failure.

What they showed on 2026-10-14 (GE-Proton11-7, RTX 4090): both layers present from the graphics family (0, flags
`0xF`) with one wait semaphore, on EXCLUSIVE swapchains of usage `0x12` (COLOR_ATTACHMENT and TRANSFER_DST, no
TRANSFER_SRC) whose surface offers `0x9F`, and both recreate their VkSwapchain once early (DXVK at its 7th present,
vkd3d-proton at its 2nd). Both chain `VkSwapchainPresentFenceInfoEXT` (1000275001) and
`VkSwapchainPresentModeInfoEXT` (1000275003) on every present; DXVK adds `VkPresentIdKHR` (1000294000), vkd3d-proton
1000479001, which the vendored headers do not name (by its number, `VK_KHR_present_id2`'s `VkPresentId2KHR`).

An application killed by SIGKILL during Wine's process teardown, after it wrote a PASS `result.json` and printed its
PASS line, is accepted with a NOTE (seen once in about a dozen runs, in `marker-master-off`, where the host is not
installed): a kill at exit cannot make a check pass, since a log cut short only fails one. Any other exit fails.

A passing run exits 0, prints one PASS line per case and writes `artifacts/results.json` with `status: PASS`; logs
are `artifacts/<case>.log` (the application) and `artifacts/cases/<case>/OptiScaler.log`.

## What this does not cover

- The translated cases are one minimal title each, with no menu opened, no upscaler and no Streamline; a real D3D
  title's own threads, swapchain recreation and DXGI wrapping are not exercised beyond what these do.
- No real game, no Streamline or DLSS-G pacer, no FG, no concurrent presents from two threads, no device loss, no
  late installation (the host is installed before the application creates anything).
- Windows' own Khronos loader: under Wine vulkan-1.dll's exports and `vkGetDeviceProcAddr`'s answers are the same
  winevulkan functions (the run checks it), so one detour covers both paths; on Windows they differ, and only the
  dispatch-level detour is shared.
- Anything stage 2 adds: no submit, semaphore chain, image copy or NR. This suite proves interception, identity and
  that nothing changed, not that a pass inserted here would be safe.

Never install the DLL from `artifacts/` into a game; the cases' markers live only under `artifacts/cases/`.
