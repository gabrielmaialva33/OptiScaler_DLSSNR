# DLSS-NR loopback harness

A deterministic D3D12 application that drives the production Neural Rendering path without a
game. It exists because every NR measurement so far came from launching a title and reading a
log, which gives no control over the scene, no ground truth for motion, and no way to compare
two runs pixel by pixel.

## Why this is not another unit test

`tests/nr-*` exercise decision logic with fakes: no NGX, no D3D12, no shader. This harness is
the opposite. It creates a real D3D12 device and swapchain, renders a real scene, and calls
**`NVSDK_NGX_D3D12_EvaluateFeature` exported by the production `OptiScaler.dll`** — one of the
four NR call sites, the same entry a game reaches. Nothing is mocked below that line.

## What it controls that a game cannot

- **Motion vectors are analytic.** The camera path and the geometry are ours, so the previous
  and current clip-space positions are both known and the vector is computed, not estimated.
  A game's vectors are an input we have to trust; here they are ground truth.
- **The scene is deterministic.** Frame N is identical across runs — same camera, same
  animation phase, driven by a frame counter and never by wall-clock time. Two runs are
  therefore comparable pixel by pixel, which is what an image-quality claim requires.
- **The whole matrix is reachable from one process.** Working scale, stage, resolution changes
  and flag combinations can be swept without a human opening a game.

## What it deliberately does NOT prove

The scene is synthetic. The NR model is a relighting network trained on real rendered content,
so this harness is evidence about **cost, lifetime, determinism and regression** — not about
whether the picture looks better in a real title. An image difference here means the pass
changed the frame, not that it improved it. Visual judgement stays in-game.

## Status

`python3 tests/dlssnr-loopback/run.py` builds the harness with the msvc-wine toolchain, sets up
a disposable runtime prefix with vkd3d-proton borrowed from the local Proton install, links the NR
kit (forwarder, model, `_nvngx.dll`) and a donor game's DLSS/Streamline DLLs beside the production
`OptiScaler.dll`, and runs it. It refuses to start while `build-local.sh` holds the compiler prefix.

What works: a real D3D12 device on the GPU, all `NVSDK_NGX_D3D12_*` exports resolved from the
production DLL, `Init` and `CreateFeature` (DLSS super-sampling) succeeding, and thousands of
`EvaluateFeature` calls across a scripted sweep of render extents. The NR module is reached — its
spatial contract, guides and exposure scan all log — and the post-stage settling gate fires once on
the cold start, as designed.

**The NR pass composes — under Proton.** `--runtime proton` (the default) runs the harness through
the local Proton install in a compatdata prefix of its own, and the model runs: 672 applied
evaluations, zero failures, on the first attempt. Under a bare Wine prefix (`--runtime wine`) the
same binary reaches NR but the model's `CreateFeature` returns `0xBAD00002` (`FAIL_PlatformError`).
Mirroring Proton by hand did not close that gap: `_nvngx.dll` matches the driver byte for byte,
`nvngx_dlss.dll` and the eleven Streamline DLLs change nothing, and adding dxvk-nvapi plus the
driver's `nvngx.dll` and the `NGXCore` registry key moved the failure to NGX core init and then to
a page fault. The model links NVAPI (45 `NvAPI_` references) and expects the environment Proton
assembles; nothing here reproduces that by hand. If NR does not compose, the runner **fails** with
`ZERO COVERAGE: the NR pass never composed a frame` rather than passing on the upscaler alone.

The harness writes `dlssnr-loopback.log` beside itself, flushed per line, because Proton does not
pass a child's stdout through and a crash discards buffered output. The runner prints it, and
`PROTON_LOG=1`, `SteamGameId=0` and explicit exception tracing land wine-side crash logs in
`artifacts/steam-0.log`. The neutral ID is required by Proton's log setup and selects no game profile.
Previously the missing ID prevented that file from being created, while `WINEDEBUG=-all` also
suppressed the exception trace.

The compiler watchdog matches MSVC paths case-insensitively (`Hostx64/x64/cl.exe` is the local
spelling), and monitors the wrapper if it has not spawned a compiler. An empty PID match must not
disable stall recovery indefinitely.

The generated INI selects `OverlayMenu=true`. This is the **swapchain overlay route**, not an
"enable all menus" switch. The harness has no swapchain, so this avoids the in-upscaler ImGui path.
`OverlayMenu=false` used to activate that unrelated path and could crash at
`Menu_Dx12::Render` with a null ImGui context during the sweep. Both the pre-optimization DLL
(`fb079c16`) and the persistent-mapping DLL (`61eabbce`) failed with that configuration. This runner
configuration isolates NR validation; it does not fix the production menu lifetime issue.

With the corrected harness, the persistent-mapping build completed all seven extent steps with
5 feature creations and 4268 upscale evaluations, and logged NR composition at both output sizes.
It then reproduced the known `Shutdown1` access violation inside `_nvngx.dll` (RVA `0x3af44`).
That is a **failed suite**, not a runtime acceptance or an FPS measurement.

What the first Proton run taught about the settling gate: it keys on the resource the pass composes
into, the upscaler's **output**. Sweeping only the render extent never trips it — the model takes
the guides as a subrect and was built once across five render sizes. The sweep therefore changes
the output extent, which is what a game's resolution change does.

Still to do, in order: the page fault the first Proton run ended in (the per-step log now names
where); the deterministic scene (`scene.hlsl` is written, not yet wired); frame dumps for
run-to-run comparison.

A stage that cannot reach its own instrumentation must fail loudly rather than pass quietly;
that is the `ZERO COVERAGE` rule the Vulkan harness established and this one inherits.
