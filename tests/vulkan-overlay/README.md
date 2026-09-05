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
Logs are `artifacts/run.log`, `negative-control.log`, and `run/OptiScaler.log`.
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

Missing dependencies, unsupported surface capabilities, crashes, timeouts, missed
coverage, leaks, or validation errors are failures, never skips. A failed or interrupted
run cannot leave an aggregate PASS from an earlier execution.

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
