# Full production DLSS-G provider on D3D12

```sh
python3 tests/fg-provider-d3d12/run.py --strict
python3 tests/fg-provider-d3d12/run.py --self-test  # host evidence parser and rejection controls only
```

Loads the complete existing `x64/out/OptiScaler.dll` and an actual user-supplied DLSS-G model through
an installed GE-Proton. No game is started, no production source is sliced into the harness, and no
backend is replaced. See [the design](../../docs/dlssnr/design/automated-fg-provider.md).

The synthetic game submits a deterministic moving checker plane, depth, pixel motion and perspective
camera through the public game-style Streamline API. OptiScaler intercepts that input and creates its
own real DLSS-G provider/swapchain. The actual output plugin's GetState is resolved from its loaded
PE exports and its address is checked against the plugin module; the synthetic game-facing GetState
is never accepted as evidence.

Four phases cover initial enable, game disable, re-enable and resize (1280x720 to 1600x900), with
336 application presents and completed GPU/input fences. Each active steady phase must have at least
16 more real-plugin presents than application presents; disabled steady state must report exactly one actual present per application present.
The first 32 frames of each phase allow production's 22-frame activation debounce and plugin warmup.
All remaining plugin statuses must be OK. A successful DLL load, resource tag, dispatch log or
application Present alone cannot pass this suite.

Prerequisites:

- A current complete production DLL, built separately with `./build-local.sh Release` (`--dll` overrides).
- MSVC Wine toolchain and installed GE-Proton (`MSVC_BIN`, `WINEPREFIX`, `--proton`/`RETOQUE_PROTON`).
- A graphical session, compatible NVIDIA hardware/driver and an existing local kit containing
  `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll`.
  Select it with `--kit`/`RETOQUE_FG_KIT`. The runner also discovers complete local test kits.
- An existing `nvngx_dlss.dll` (`--dlss`, or discovered in a local test kit): OptiScaler disables native
  NGX passthrough without the SR asset, including the real frame-generation calls. No SR feature is run.

All staged DLLs, compiler outputs, logs, the dedicated Proton prefix and `result.json` live under
ignored `artifacts/`. Assets are never downloaded or installed into games. Proton automatic upscaler
upgrades are explicitly disabled. The shared compiler-prefix lock and active-build/game guards run
before compiling/launching; no shared wineserver reset is performed.

Missing prerequisites print SKIP with `runtime_coverage=false`; `--strict` converts them to failure.
Once the process is launched, runtime errors always fail. `result.json` records per-phase counters,
staged asset/source hashes (checked unchanged across execution), DLL hash and repository state. The runtime log retains every frame. The host
self-test rejects missing oracle, no surplus, incomplete GPU completion, wrong enable state, zero disabled presents and
missing frame records. A host timeout regression also starts a wrapper with a SIGTERM-ignoring descendant and proves the owned
process group is killed even when its leader exits first. These checks provide no GPU evidence.

This exercises native x2. Higher ratios are explicitly untested; Ada's usual max of one generated
frame is accepted. Plugin actual-present counters are not display scanout capture or perceptual
quality measurements. Vulkan, D3D11, menu interaction, latency and native Windows validation remain
outside this fixture.
