# Vulkan present host, stages 1 to 3 (wine tier)

Run from the repository root on Linux with Wine, a graphical session, working Vulkan, the native Khronos validation
layer, a host `g++` and the repository's msvc-wine toolchain, after `./build-local.sh Release`:

```sh
python3 tests/vulkan-present-host/run.py               # every case
python3 tests/vulkan-present-host/run.py --only marker # one case
python3 tests/vulkan-present-host/run.py --dll PATH    # another build (default x64/out/OptiScaler.dll)
```

Design: `docs/dlssnr/design/nr-vulkan-present-host.md`, "Stage 1 as built", "Stage 2 as built" and "Stage 3 as
built". The host-tier half is `tests/nr-vk-present-host`.

The stage-1 and stage-2 cases below run with `[DlssNr] Enabled` on and no model files beside the DLL, so on stage 3's
build every carried present is refused by the model (`model unavailable`, `nvngx.dll_dlssnr.dll was not found beside
OptiScaler or the game`) and the read-backs stay exact; the runner holds the window and destruction lines' NR part to
that, and the device lines to the model's extensions the host adds (`VK_KHR_format_feature_flags2` too, the harness's
devices being Vulkan 1.1 without format-less storage writes) and `bufferDeviceAddress enabled by the host`. Stage 3's
own cases are in "Cold NR" below.

## How it works

Unlike `tests/vulkan-overlay`, nothing is instrumented: the DLL under test is the production `OptiScaler.dll` that
`build-local.sh` produced, refused if it lacks the stage-2 host's line or is older than the host's sources. The runner
compiles `harness.cpp` (a Win32 Vulkan application, `/MD`, against `external/vulkan`) in a disposable copy of the
compiler prefix, `artifacts/build-prefix`, builds `hide_transfer_src.cpp` (a native Vulkan layer, below) with the
host's `g++`, and runs every case in its own directory under `artifacts/cases/` in a separate runtime prefix,
`artifacts/wineprefix`, with OptiScaler as `dxgi.dll` beside the application.

The application never asserts anything about the host's log. Before loading OptiScaler it copies the first 16 bytes of
seventeen vulkan-1.dll exports; it loads OptiScaler, loads `vulkan-1.dll` again (the loader hook's late path), and
then drives real Vulkan:

- window 1, device 1 with the graphics family through `vkGetDeviceQueue` and, when the surface takes presents from
  one, a compute-only family through `vkGetDeviceQueue2` (family 2, flags `0xE`, on the RTX 4090 here);
- swapchain `sc1` with usage `0x12` (COLOR_ATTACHMENT and TRANSFER_DST, as DXVK and vkd3d-proton ask), 12 presents
  with one wait each from the graphics queue: the host must add TRANSFER_SRC and carry them on the same queue;
- `sc2` through `oldSwapchain` with both transfer usages, presented from the compute-only queue: the round trip's
  cross-family route (release to the graphics family, copy there, release back, three submits); two presents with no
  wait (after the frame was seen complete), then two waits each, to the window's 100th present;
- `sc-held` through `oldSwapchain`, from the graphics queue again, with seven more images than the surface's minimum:
  about 1.2 s of buffer copies is submitted on the queue (sized by a probe of this GPU), then seven presents are queued
  behind it with nothing waited, so the host's ring of four fills; the fifth to seventh must wait 100 ms for the
  oldest slot and then go down without the round trip (`ring busy`); then frames read every image back;
- window 2 and device 2, alive beside device 1, three presents; one more on window 1; device 2 destroyed and device
  3 created (here it gets device 2's handle value back), a new swapchain, three presents; the window resized and its
  swapchain recreated through `oldSwapchain` at the new extent, six presents.

Every frame uploads a pattern of its own (a hash of a per-frame seed and the pixel's index, all four bytes) into the
image it presents. When the same image comes round again, before anything is written to it, the application copies
it back and compares it with the pattern it last presented there: equality, frame after frame, through the
same-queue route, the cross-family route, the ring under pressure and the resize, is the proof that the round trip is
lossless (and that the images it did not carry are untouched). An image is read back only with TRANSFER_SRC: on
swapchains that ask for it, and with `--host-adds-src` (the cases with the host on and the surface offering it) on
every swapchain, relying on the usage the host added; the validation layer sits below the host and sees the real
usage. Any difference fails the run.

It writes everything it did to `result.json`: handles, families and their flags, usages, image counts, every present
in order with its swapchain, device, queue, family and waits, which presents were in the held-back batch and whether
the GPU had drained the application's earlier work, the read-back counts, the held-back block's GPU time against the
time of the batch's last present, and whether each export's first bytes changed by the time the first swapchain
existed. The runner models, from that list, every present's outcome (carried by which route with how many waits and
submits, or refused for which reason), every window line (one at a window's first present, one when its swapchain,
device or queue changes, one at 10 and 100 presents) with its counters (carried, refused by reason, waits taken
over, submits, GPU completions observed, ring depth, CPU wait), every swapchain's creation line (usage asked,
usage given, round trip on or off and why) and destruction line (all its round trips seen complete, its objects
freed), and every device's destruction line, and holds `OptiScaler.log` against all of it field by field.

Validation is on (core and synchronization) and proved active per run by a deliberately invalid fence flag the layer
must reject (`tests/vulkan-overlay`'s negative-control technique); any other validation error fails the case. One
message is set aside and counted, `wine_injected_extension_messages`: Wine 11.19's winevulkan appends
`VK_EXT_external_memory_dma_buf` to the host-side device it creates, without `VK_KHR_external_memory_fd`, and the
native layer reports it (`VUID-vkCreateDevice-ppEnabledExtensionNames-01387`) at an index past the application's
one extension. It appears once per device whenever the bundle is skipped, with the host off as well; with the bundle
installed, its spoofing's extension additions make it go away. The host passes the device create info down unchanged.

### The surface that hides TRANSFER_SRC

`hide_transfer_src.cpp` is a native (Linux) Vulkan layer, `VK_LAYER_RETOQUE_hide_transfer_src`, built into
`artifacts/layer/` with its manifest and found through `VK_ADD_LAYER_PATH`. It clears TRANSFER_SRC from every
surface's `supportedUsageFlags` (`vkGetPhysicalDeviceSurfaceCapabilities(2)KHR`) and passes everything else through.
winevulkan answers both the application's and OptiScaler's surface queries from the host driver, so both see a
surface without TRANSFER_SRC. It is listed after the validation layer (nearer the driver), so the validation layer
sees the hidden usage too and would report a swapchain created with it.

## Cases

| Case | Marker | `[Vulkan] SkipHooks` | `Enabled` | `VulkanPresentHost` | `OverlayMenu` | Must hold |
|---|---|---|---|---|---|---|
| `marker` | yes | | true | true | true | every line right; 127 of 130 presents carried (88 cross-family), 3 refused as `ring busy` after a 100 ms wait; the 14 stage-1 exports and `vkQueueSubmit`/`vkQueueWaitIdle` patched, `vkCreateFence` not; every read-back equal |
| `skiphooks` | | true | true | true | true | the same, through the ini instead of the marker |
| `surface-no-src` | yes | | true | true | true | with TRANSFER_SRC hidden: every swapchain's usage left as asked and its round trip `off, the surface does not offer TRANSFER_SRC`; every present goes down unchanged (`observe only`) |
| `marker-key-off` | yes | | true | false | true | no host text in the log; all seventeen exports byte-identical; the read-backs of the swapchains that ask for TRANSFER_SRC equal (the control: images survive a present here without the host) |
| `marker-master-off` | yes | | false | true | true | the same |
| `menu-off` | | | true | true | false | every line right beside the bundle with no menu (`OverlayMenu=false`, which `vulkan-overlay` must see as zero coverage, is covered positively here) |
| `menu-on` | | | true | true | true | every line right beside the bundle and its menu, one window: the host's present runs first and the overlay's present then consumes the host's semaphore (application -> host -> overlay -> present) under validation; swapchain 1's usage shows the TRANSFER_SRC the overlay adds for its blur |
| `key-off` | | | true | false | true | no host text in the log, one window |

With the bundle installed (`menu-off`, `menu-on`, `key-off`), its instance-level hooks run before the host's on the
same exports, so the instance and device lines name `dxgi.dll (OptiScaler's own Vulkan hook, called down to this
one)` as creator; the runner expects exactly that. Its `vkGetDeviceProcAddr` also hands out its own `vkQueueSubmit`
hook, so there the export and `vkGetDeviceProcAddr`'s answer differ for the queue calls; the host detours the export,
which that hook calls down to.

With the legacy menu (`menu-on`, `key-off`) the overlay draws its splash into the presented images, host on or off, so
their read-backs are counted and not compared (`--overlay-draws`), and the held-back batch is not run: on a new
swapchain the overlay's present waits for the GPU, so presents never pile up behind the block of work.

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
application, so DXVK's `dxgi.dll` stays the real one. The marker is present, as deployed for such titles, except in
the two `dxvk-bundle` cases.

| Case | API | `VulkanPresentHost` | Validation | Must hold |
|---|---|---|---|---|
| `dxvk-key-off` | D3D11 | false | core + sync | no host text; the layer's own validation errors (the baseline) |
| `vkd3d-proton-key-off` | D3D12 | false | none | no host text |
| `dxvk` | D3D11 | true | core + sync | a device created by `d3d11.dll (DXVK)`; its swapchains `usage 0x12 -> 0x13`, round trip on; every present carried, none refused; every destroyed swapchain's round trips all seen complete and its objects freed; no validation error the baseline does not have |
| `vkd3d-proton` | D3D12 | true | none | the same with `d3d12core.dll (vkd3d-proton)`; OptiScaler's own throwaway D3D12 device (IdentifyGpu's vkd3d-proton check, under `ScopedSkipVulkanHooks`) is recorded as OptiScaler's, never reported or transported, and the title's device that gets its handle value back is a new record |
| `dxvk-bundle-key-off` | D3D11 | false | core + sync | no marker, the whole bundle and its menu, `LogLevel=0`: no host text (the baseline) |
| `dxvk-bundle` | D3D11 | true | core + sync | the same as `dxvk`, beside the bundle; and the bundle's own present hook, below the host's, runs for every present and takes each for a DXGI-backed one: its function trace for every present, the Vulkan overlay's `QueuePresent` never (the host hands the bundle the application's return address, `OriginalPresentCaller`) |

GE-Proton11-7's vkd3d-proton aborts (exit 3) in its device initialisation under the Khronos layer 1.4.363, with core
validation alone and with the host off as well, so its cases run without the layer. Without a local Proton holding
both layers these six print SKIP and do not count as a failure.

What they showed on 2026-10-14 (GE-Proton11-7, RTX 4090): both layers present from the graphics family (0, flags
`0xF`) with one wait semaphore, on EXCLUSIVE swapchains of usage `0x12` whose surface offers `0x9F`, and both
recreate their VkSwapchain once early. Both chain `VkSwapchainPresentFenceInfoEXT` (1000275001) and
`VkSwapchainPresentModeInfoEXT` (1000275003) on every present; DXVK adds `VkPresentIdKHR` (1000294000), vkd3d-proton
1000479001 (`VK_KHR_present_id2`'s `VkPresentId2KHR`); the host carries the chain through by pointer. With the host on,
DXVK's 12 presents and vkd3d-proton's 12 were all carried on the same queue, ring depth 1.

An application killed by SIGKILL during Wine's process teardown, after it wrote a PASS `result.json` and printed its
PASS line, is accepted with a NOTE (seen once in about a dozen runs, in `marker-master-off`, where the host is not
installed): a kill at exit cannot make a check pass, since a log cut short only fails one. Any other exit fails.

A passing run exits 0, prints one PASS line per case and writes `artifacts/results.json` with `status: PASS`; logs
are `artifacts/<case>.log` (the application) and `artifacts/cases/<case>/OptiScaler.log`.

## Cold NR (stage 3)

`nr.cpp` is a third application, `vk-present-host-nr.exe`: one device asking for `VK_KHR_swapchain` alone (the model's
extensions are the host's to add), one window (an owned dialog, so a tiling window manager does not resize it under
the swapchain), and a picture -- gradients, a flat patch, fine stripes, a square that moves five pixels a frame, opaque
-- presented through a swapchain of every SDR format the surface offers in sRGB non-linear (here, under winevulkan on
the RTX 4090, B8G8R8A8_UNORM and B8G8R8A8_SRGB only), a four-byte non-SDR format when offered (none was), the same
format presented from the compute-only family (`--cross`, the cross-family route), and a swapchain recreated through
`oldSwapchain` at 517x389, not a multiple of eight. Every image is read back when it comes round again and compared
with what was presented: exactly, by mean and largest change of a colour channel, alpha, and whether it came back
black. It writes the facts and its validation reports (`validation.txt`) itself: Proton does not pass its output on.

The cases run under Proton (`proton run`; PROTON_DIR, else the newest GE-Proton, else Valve's), in a prefix of their own
(`artifacts/protonprefix-nr`), with the validation layer as in every other case. Under plain Wine the snippet comes up
but the driver's NGX core answers FAIL_PlatformError (0xBAD00002), even with the prefix set up by hand as Proton sets
it up (the core in system32, the NGXCore key, dxvk-nvapi): the D3D12 loopback found the same. Beside the DLL: the
forwarder built with it (`x64/out`), and NVIDIA's `nvngx_dlssnr.dll` from `~/.local/opt/dlssnr-kit` (`DLSSNR_KIT`),
symlinked, never copied; the driver's core comes from `/usr/lib/nvidia/wine` through Proton. Without the kit, the
driver's core or a Proton the cases print SKIP.

| Case | `ApplyModel` | `WorkingScale` | Cross-family | Forwarder | Model | Must hold |
|---|---|---|---|---|---|---|
| `nr-control` | false | auto | yes | built | yes | every carried SDR present evaluated, every read-back byte-exact (the conversion out, the pass and the conversion back are lossless when the model's edit is not applied), the model's lifecycle in the log (initialised with the core brought up, images and feature built, rebuilt at 517x389, freed at device destruction with NGX shut down), pass GPU times measured |
| `nr` | true | auto | yes | built | yes | the same, and every SDR read-back changed but close: at most half exact, mean change above 0 and at most 25 eight-bit steps per frame, alpha untouched, never black |
| `nr-working-scale` | true | 0.5 | no | built | yes | as `nr`, with the model's images at half size (`working 259x195` for 517x389) |
| `nr-no-extensions` | true | auto | no | built | yes | the test layer hides `VK_NVX_binary_import`: the device line says it is not possible, every present refused for `device extensions`, every read-back exact |
| `nr-old-forwarder` | true | auto | no | the kit's (2026-09-03, no `dlssnr_abi_version`) | yes | refused, `nvngx.dll_dlssnr.dll is from a different build`; every read-back exact |
| `nr-no-model` | true | auto | no | built | no | refused, `nvngx_dlssnr.dll was not found beside OptiScaler or the game`; every read-back exact |

`nr-route-quiet` runs `nr.cpp --route`: a game that turns its upscaler on and off, DOOM Eternal's shape (2026-10-14:
the game's DLSS turned off left the after-upscale route holding the model for the session). One B8G8R8A8 swapchain
presented throughout; NGX initialised through OptiScaler's own `NVSDK_NGX_VULKAN_*` exports, as a game calls them, with
`[Upscalers] VulkanUpscaler=fsr22` (OptiScaler's FSR 2.2 behind them, so no DLSS is needed: the evaluate reaches the
after-upscale route the way every OptiScaler upscaler does) on a device that enables what FSR 2.2's shaders declare
(`shaderInt16`, `shaderFloat16`). The phases, each with its read-backs counted apart: `cold` (1.5 s, the present host
runs the model on every frame), then twice `on` (a feature created, 30 evaluates at 640x360 -> 1280x720, the route
running the model on the upscaler's output), the feature released, and presents only until 9 s after the last
evaluate (`off-waiting`), 12 s (`off-handing-back`) and 14 s (`off-back`; it and `cold` go on until they have a dozen
frames, so a loaded machine thins them instead of emptying them); then `on-at-exit` (10 evaluates, the feature
released) and the device destroyed with the route holding the model. It must hold: every `cold` and `off-back`
read-back changed (the present host's model), every `on` and `off-waiting` one exact (the host stood down, and did not
take the model back before 10 s); in the log, in this order and twice, `this route waits`, `the model stood down and
was freed`, `this route has the model again (the present host stood down)`, `has not asked for the model for 10 s:
taking it back`, `the after-upscale route's model was freed for the hand-back (its feature at 1280x720`, `the model is
the present host's again`; then the stand-down once more, and at exit `the after-upscale route's objects on the device
freed before it goes` ahead of `NGX shut down on the device before it goes (returned 1)`; three feature builds of each,
nothing called off or abandoned. Measured 2026-10-14: drains of about 100 ms (a few presents at the harness's rate
under validation), about 80 read-backs exact before each hand-back.

`nr-route-quiet-wdx12` repeats those phases with the Vulkan bundle enabled (no skip marker, no `SkipHooks`),
the overlay disabled, and `OPTISCALER_DISABLE_VULKAN_WDX12_HOOKS` removed from the child environment. Before the
first frame, the harness resolves `vkQueueSubmit` through `vkGetDeviceProcAddr` and probes a zero-count/null-array
submit with no fence. Vulkan permits it, but the real `Vulkan_wDx12::hk_vkQueueSubmit` must return
`VK_ERROR_INITIALIZATION_FAILED`; the harness records `wdx12_probed=true`. The log must contain exactly one
`Invalid parameters to hk_vkQueueSubmit` (that probe), followed by two successful hand-backs and the bounded drain
at device destruction. This catches the DOOM failure the original `nr-route-quiet` missed: its skip marker prevents
`VulkanHooks::Hook` from installing the wDx12 bundle. The companion uses the production hook, not a mock or a
modified OptiScaler DLL. The fixed nine-second waiting windows require at least one read-back and every read-back
exact; they impose no frame-rate minimum (the enabled bundle once yielded four exact read-backs, falsely rejected
by the old minimum of five). The judged post-hand-back phases still run for at least twelve presents.
Neither harness reproduces DOOM's full rendering workload.

Verified on `da4988a1`: both route cases pass, including wDx12's two hand-backs and bounded destroy drain (94/96 ms
hand-back drains, zero host validation errors). As a negative control, the retained `80374044` DLL fails the new
case with two -3 hand-back rejections and a rejected destroy drain, followed by a live-child-object validation error.

What NVIDIA's model records into the host's command buffer is validated too. One report comes from it, once per
feature build: `VUID-vkCmdDraw-None-09600`, an image of its own named `nv.ngx.dlssnr.resource` used in GENERAL at its
first evaluate without ever leaving UNDEFINED. The application counts reports naming an `nv.ngx.*` object apart
(`model_messages`, `model_vuids`); the runner accepts that VUID alone, at most once per feature build, and nothing else
from anyone. Every other report fails the case.

What they showed on 2026-10-14 (GE-Proton11-7, RTX 4090, driver 615.78.08, model 310.8.SF from the kit): the pass at
632x446 costs about 2.4 ms on the GPU (first frames up to 9 ms), the model's edit moves a colour channel by about three
eight-bit steps on average on this picture, and the cross-family route carries it from the compute-only family like
any other frame.

## What this does not cover

- No real game, no Streamline or DLSS-G pacer, no FG. The translated cases are one minimal title each, with no
  menu opened, no upscaler and no Streamline; NR runs only in the cold-NR cases, on a synthetic picture, so nothing
  here judges image quality or motion (the guides are zeros: a moving camera smears).
- Cold NR: no RGBA8 or non-SDR swapchain (the surface offers none here; the colour plan is held on the host tier), no
  CONCURRENT image, no device lost mid-frame, no failed submit with a model frame in it, no live toggle of NR or the
  working scale, no D3D12 route beside the host, no translated title with the model. The after-upscale route runs
  beside it only in `nr-route-quiet`, behind OptiScaler's FSR 2.2 rather than DLSS, with one thread presenting and
  evaluating (a game's render thread recording while the hand-back drains is held on the host tier only).
- No concurrent presents from two threads, no application thread submitting to a borrowed graphics queue while the
  cross-family route runs (the queue lock exists for it; nothing here contends), no device loss, no failed submit in
  the middle of the cross-family chain, no late installation (the host is installed before the application creates
  anything), no CONCURRENT swapchain, no present-only family (none on this GPU).
- Windows' own Khronos loader: under Wine vulkan-1.dll's exports and `vkGetDeviceProcAddr`'s answers are the same
  winevulkan functions (the run checks it), so one detour covers both paths; on Windows they differ, and only the
  dispatch-level detour is shared.
- vkd3d-proton under the validation layer (above).

Never install the DLL from `artifacts/` into a game; the cases' markers live only under `artifacts/cases/`.
