# DLSS-NR loopback harness

A D3D12 application that drives the production Neural Rendering exports without a game.
It creates real input/output resources, submits evaluations, waits on queue fences and sweeps
the output extent to exercise recreation and the post-stage settling gate. Nothing below the
production `OptiScaler.dll` NGX entry points is mocked.

## Scope

The current harness has no swapchain and does not render the planned scene. `scene.hlsl`
exists but is not wired into execution; input contents and motion are not a deterministic
image fixture. The wall-clock sweep tests real initialization, NR composition, resolution
changes and shutdown. Evaluation counts include initialization and skipped NR work.

Passing this harness establishes neither image quality, deterministic pixels nor an FPS
improvement. Analytic motion, repeatable frame contents, readback and controlled A/B timing
remain future work. Visual acceptance also requires testing in a real title.

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
Those historical runs were failed suites, not runtime acceptance or FPS measurements.
The immediately preceding DLL (`fb079c16`) also completed this corrected harness (4487 evaluations)
and failed at the same `_nvngx.dll` RVA `0x3af44` during `Shutdown1`. These single-run evaluation
counts include skipped work and initialization and must not be interpreted as performance results.

What the first Proton run taught about the settling gate: it keys on the resource the pass composes
into, the upscaler's **output**. Sweeping only the render extent never trips it — the model takes
the guides as a subrect and was built once across five render sizes. The sweep therefore changes
the output extent, which is what a game's resolution change does.

The corrected core-shutdown ABI adapter now completes this same sweep and shutdown:
5 creates, 4371 upscale evaluations, NR composition at both output sizes, shutdown result
`0x00000001`, process exit 0. The raw core export needs writable output storage in its
second argument; using the public SDK's one-argument typedef left it unspecified.
See `OptiScaler/dlssnr/design/ngx-shutdown-order.md` for the binary evidence and scope.
The runner also verifies the shutdown result and prefers the forwarder built beside the
selected DLL. Logs and hashes are retained in `x64/dispatch-validation/core-abi/`.

The final build `20260905_224708` also passes with GPU timing off and on. The enabled run
used the same binary and INI with `GpuTiming=true` and `GpuTimingInterval=30`, then called
this runner's `run_under_proton()` directly to preserve those settings. It produced confirmed
GPU samples and three expected contract generations across the extent changes. This validates
the instrumented path, not comparable frame-time or image-quality measurements.

Still to do: the deterministic scene (`scene.hlsl` is written, not yet wired), analytic
motion and frame dumps for controlled run-to-run comparison.

A stage that cannot reach its own instrumentation must fail loudly rather than pass quietly;
that is the `ZERO COVERAGE` rule the Vulkan harness established and this one inherits.
