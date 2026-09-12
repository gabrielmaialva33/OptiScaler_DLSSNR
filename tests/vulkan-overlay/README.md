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
  internal ImGui backend allocations are outside these counters.
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
  Exactly one real present must reach the completed non-graphics bailout exactly
  once, log the full matching family/flags and CONCURRENT warning exactly once, and submit **zero**
  overlay draws. Overlay creation, balanced allocation/object cleanup, successful
  presentation, the validation fence probe, and zero unexpected validation errors
  are required. Results appear under `non_graphics_present_control` in the aggregate
  JSON and in `artifacts/non-graphics-present/result.json`.
- A fourth process with `--non-graphics-present-exclusive` uses an **EXCLUSIVE**
  swapchain. All harness image work (both layout transitions and the clear) runs
  on the same non-graphics queue as presentation. The graphics queue is created
  for overlay initialization; the harness never submits image work to it. No
  harness ownership transfer is needed, so a future overlay writer on graphics
  must handle the ownership transfer itself. The selected present family must
  support compute or transfer commands to clear the image; other unsupported
  capabilities fail explicitly. This control has the same bailout, validation,
  and cleanup assertions as CONCURRENT, and additionally checks the **EXCLUSIVE**
  warning about a release barrier on the present queue and an acquire on graphics.
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

## Scope and packaging

This tests a serialized, real Vulkan overlay lifecycle under Wine. The application
drains its work before recreation and after each frame. The drain error and partial
initialization error are **simulated**, not actual GPU exhaustion or failure. This
does not prove safety under concurrent presents, device loss, every initialization
failure, DLSS-G/Streamline pacing, or any game's rendering path. No image comparison
or manual visual-quality assertion is made.

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
