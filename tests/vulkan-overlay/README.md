# Vulkan overlay lifetime harness

Run from the repository root on Linux with Wine, a graphical session, working Vulkan,
the native Khronos validation layer, populated submodules, and the repository's
msvc-wine toolchain:

```sh
python tests/vulkan-overlay/run.py --ref HEAD
```

`MSVC_BIN` defaults to `~/.local/opt/msvc/bin/x64`. `WINEPREFIX` identifies the
**compiler** prefix (default `~/.local/opt/msvc-wineprefix`); the runner copies it
before use. Both the compiler copy and the separate runtime prefix live under
`artifacts/`. `--build-only` and `--run-only` are available; the latter rejects
changed binaries or test sources. The production tree must match `--ref`.

The runner generates an instrumented copy of the selected revision's actual
`menu_overlay_vk.cpp`, builds a separate OptiScaler DLL, and loads it as `dxgi.dll`
beside a small Win32 Vulkan application. The generated copy adds counters and
one-shot fault wrappers; normal calls use real ImGui allocation and real Vulkan.
Creation and presentation enter through the production Vulkan hooks. Explicit
test teardown calls the public `DestroyVulkanObjects(false)` through a test-only
export. No production source, solution, input handler, or game install is edited.

A passing run exits **0** and writes **`artifacts/results.json` with `status: PASS`**.
Logs are `artifacts/run.log`, `negative-control.log`, `non-graphics-present.log`,
`non-graphics-present-exclusive.log`, and each process directory's `OptiScaler.log`.
It requires all of the following:

- Calls to `CreateSwapchain`, `DestroyVulkanObjects`, and `QueuePresent`, plus
  successful overlay rendering submissions in each normal recreation generation.
- 16 recreations, with actual surface extents and actual image counts changing.
  Counts are requested within the surface limits and the overlay's eight-image cap.
  A compositor ignoring resize requests or a driver returning one constant count
  cannot silently pass.
- Balanced `IM_ALLOC`/`IM_FREE` calls and zero remaining tracked bytes. Vulkan object
  counts cover the objects owned directly by the overlay translation unit;
  internal ImGui backend allocations are outside these counters. Command buffers
  still allocated from a pool when it is destroyed count as destroyed with it,
  which is how production releases its cross-family transfer buffers.
- An injected failed drain preserves ownership without frees or replacement,
  followed by successful real-drain cleanup. Failure of the second framebuffer
  creation cleans up the partially initialized generation. Rendering then recovers.
- Active validation and zero unexpected validation errors. A deliberately invalid
  fence flag must produce its exact VUID through the callback and be rejected with
  `VK_ERROR_VALIDATION_FAILED_EXT`. This one expected diagnostic proves validation
  works and is reported separately; all other errors fail the run.
- A second process with `OverlayMenu=false` must exit **1** with **`ZERO COVERAGE`**.
  This negative control uses another isolated directory and no marker file.
- A third process with `--non-graphics-present` creates both a graphics queue for
  overlay initialization/clearing and a non-graphics queue for presentation. It
  queries `vkGetPhysicalDeviceSurfaceSupportKHR` for the actual Win32 surface,
  preferring a compute-capable non-graphics family (such as DOOM Eternal's family
  2, flags `0xE`) over other non-graphics families. The swapchain uses concurrent
  sharing between these families, with a semaphore connecting clear and present.
  It presents **twice as many frames as the swapchain has images**, so images come
  round again: each present after an image's first must complete a wait on the
  fence the chain's last submit armed (`fence_waits == frames - images_drawn`,
  counted where production clears the pending flag after a successful wait) and
  reuses that image's transfer command buffers and semaphores. **Every**
  present must take the cross-family path: one menu submit on the graphics queue
  (`cross_family_draws`, counted at the middle submit of the chain) and one
  completed chain back to the present queue (`overlay_submits`, counted where the
  image's fence is armed), both equal to `frames` and `present_calls`. `frames`
  counts only presents that returned success and drained. The production log must
  hold `present happens on queue family <present> (flags <X>), which cannot run a
  render pass; drawing the menu on graphics family <graphics> with a queue-family
  transfer around it (CONCURRENT swapchain)` exactly once (it is one-shot), and
  none of the chain's failure exits: a fence-wait timeout (`vkWaitForFences
  returned`), a failed release/menu/acquire submit, failed transfer recording or
  cross-family object creation, `menu disabled`, or the graphics-family pool move.
  Overlay creation, balanced allocation/object cleanup, the validation fence probe,
  and zero unexpected validation errors (synchronization validation included) are
  required. Results appear under `non_graphics_present_control` in the aggregate
  JSON and in `artifacts/non-graphics-present/result.json`.
- A fourth process with `--non-graphics-present-exclusive` uses an **EXCLUSIVE**
  swapchain. All harness image work (both layout transitions and the clear) runs
  on the same non-graphics queue as presentation. The graphics queue is created
  for overlay initialization; the harness never submits image work to it, so the
  only ownership transfers are the overlay's own release/acquire pairs, and the
  harness's next clear on the present family validates that ownership came back.
  The selected present family must support compute or transfer commands to clear
  the image; other unsupported capabilities fail explicitly. This control has the
  same cross-family, validation, and cleanup assertions as CONCURRENT, with the
  log line ending `(EXCLUSIVE swapchain)`.
  Both controls verify production's recorded sharing mode and report/assert the
  clear queue family (graphics for CONCURRENT, present for EXCLUSIVE). Results are
  under `non_graphics_present_exclusive_control` and in
  `artifacts/non-graphics-present-exclusive/result.json`.

Missing dependencies, unsupported surface capabilities, crashes, timeouts, missed
coverage, leaks, or validation errors are failures. The sole capability exception is
each non-graphics process: if no enumerated device has a non-graphics queue supporting
the surface, it exits **77**, prints **SKIP** with the reason, and records **`status:
SKIP`** for that control. The runner prints that reason loudly; the graphics tests
still must pass, and their aggregate PASS does not claim non-graphics coverage.
A failed or interrupted run cannot leave an aggregate PASS from an earlier execution.

## History of the non-graphics controls

Until `fc08aa32` (2026-09-19, "menu: draw Vulkan overlay on non-graphics present
queues via cross-family transfer") a present from a family without graphics could
not run the menu's render pass, and the overlay bailed out: the controls asserted
exactly one completed bailout, **zero** overlay submits, and a one-shot "the Vulkan
overlay is not possible on this swapchain" warning naming what a cross-family draw
would need. `fc08aa32` built exactly that for id Tech 7 (DOOM Eternal presents from
compute family 2): release on the present queue, acquire + menu + release on the
graphics queue, acquire back on the present queue, three submits chained by
semaphores with the image's fence on the last. The bailout, its counter
(`nonGraphicsBailouts`) and its warning are gone; the suite stayed red on its
retired anchors until the controls were rewritten to assert the draw instead
(probe ABI 3, counter `crossFamilyDraws`).

## Scope and packaging

This tests a serialized, real Vulkan overlay lifecycle under Wine. The application
drains its work before recreation and after each frame. The drain error and partial
initialization error are **simulated**, not actual GPU exhaustion or failure. This
does not prove safety under concurrent presents, device loss, every initialization
failure, DLSS-G/Streamline pacing, or any game's rendering path. No image comparison
or manual visual-quality assertion is made: the non-graphics controls prove the
cross-family chain submits, completes and validates, not that the menu's pixels
reached the presented image.

All generated sources, DLLs, executables, logs, and prefixes are in ignored
`tests/vulkan-overlay/artifacts/`. No test is added to `OptiScaler.sln` or its projects.
`package_release.ps1` only copies its explicit file/folder allow-list from
`x64/Release/a` (plus the production forwarder), so this directory is not packaged.
The test DLL must never be installed into a game. The runner does not alter any
`optiscaler_skip_vulkan_hooks` marker and refuses one in its test directories.

## Recorded baseline (2026-09-05)

Against `f2eb9a18` (overlay SHA-256
`aed48c12bde6a760926ac76720eb2b4470fc4872868358af899cb7ccec16687a`),
Wine 11.17 / RTX 4090 / Khronos validation 1.4.357:
17 creates, 3 public destroys, 18 internal teardown entries, 54 presents and 54
overlay submissions; 16 recreations, 12 actual count changes (3 ↔ 4), and 12 actual
extent changes (652×486 ↔ 812×586). The 32 array allocations matched 32 releases,
with zero live bytes (224-byte peak); 343 tracked Vulkan objects were created and
destroyed. Both injected failures were reached once, the zero-coverage control
failed as required, and validation reported no unexpected errors.

The baseline hash above predates the authorized commit-message amendment; the
production source is unchanged by that amendment.
