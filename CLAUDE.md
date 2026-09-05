# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A fork chain, three deep. Upstream **optiscaler/OptiScaler** (a Windows DLL that sits between a game and
its upscaler / frame-gen SDKs and swaps the backend) → **Dagherbou/OptiScaler_DLSSNR** (adds the DLSS 5
Neural Rendering module under `OptiScaler/dlssnr/`) → **gabrielmaialva33/OptiScaler_DLSSNR** (this
repo), which adds Linux/Proton overlay patches on top.

- Working and default branch: `dlss-neural-rendering` = Dagherbou's `v0.2.0-dlssnr` (commit `9737616`)
  plus our Linux patches. `up/master` is plain upstream OptiScaler, ~150 commits behind.
- Remotes: `origin` (SSH, push target) and `up` (Dagherbou, read-only).
- Update flow: `git fetch up && git merge up/dlss-neural-rendering`. `fetch.recurseSubmodules=no` is
  already set in the repo config; keep it.
- Commit subjects use prefixes seen in history: `DLSS-NR:` for the module, `linux:` for Proton
  patches, `ci:` for workflows.
- **Local until asked.** Never push to any remote unless the user asks for that specific ref
  (`dlssnr/design/DEVELOPMENT.md` invariant 9).
- **Never commit** `OptiScaler/resource_build_date.h`, `OptiScaler/resource_build_commit.h`, `x64/`,
  or `.winx/`. All are gitignored.
- **The tree layout is upstream's, and stays that way.** The usual C++ advice (Pitchfork, the
  Canonical Project Structure) says `src/`, `include/`, `tools/`, `docs/`. Do not apply it here.
  731 of the 943 tracked files live under `OptiScaler/` and belong to upstream; moving them turns
  every `git merge up/dlss-neural-rendering` into a whole-tree conflict, and this branch already
  carries 74 commits of our own. Organise inside what this fork owns — `tests/`, `build-local.sh`,
  `CLAUDE.md`, `.github/workflows/nr-tests.yml`, `.clang-format-ignore`, and the files listed under
  "Linux / Proton patches" — and leave upstream's paths byte-identical.

## Build

Windows-only C++ (`stdcpplatest`, MSVC), built from `OptiScaler.sln`. Two projects: `OptiScaler` (the
DLL) and `dlssnr_forwarder` (a ~13 KB shim, see below). Real target is `x64`; Win32 configurations
exist in the solution but CI and every script build only x64. Configurations: `Debug`, `Release`,
`ReleaseDebug`.

### Local, on this Linux workstation (msvc-wine)

```bash
./build-local.sh                # Release
./build-local.sh Debug
./build-local.sh Release /t:Rebuild   # extra args after the config go straight to msbuild
```

- MSVC 14.44 + Windows SDK 26100 under Wine at `~/.local/opt/msvc` (`MSVC_BIN`), prefix
  `~/.local/opt/msvc-wineprefix` (`WINEPREFIX`). Both env vars are overridable. The `msbuild` wrapper
  there is patched to export `VCToolsInstallDir_170`.
- No `/m`: MSBuild child nodes do not work under Wine. Parallelism comes from `/p:CL_MPCount=16`.
- **The build stalls intermittently under Wine, and lowering `CL_MPCount` does not reliably fix
  it.** Observed twice: once mid-compile with 8 `CL.exe` at `CL_MPCount=16`, once at
  `CL_MPCount=4` with only 2 `CL.exe`, on the trivial `dlssnr_forwarder.cpp`, *after*
  `OptiScaler.dll` had already linked. So it is not a parallelism threshold.
  **Signature** (measure CPU *deltas*; `ps` %CPU is an average since start and reads low for a
  process that worked then stalled): `MSBuild.exe` and `CL.exe` at ~0 ticks/s while `wineserver`
  spins at 50-65% of a core, the redirected log stops growing, and no new `.obj` appears.
  **Recovery**: kill the stalled PIDs, `WINEPREFIX=~/.local/opt/msvc-wineprefix wineserver -k`,
  then rerun — it resumes incrementally and usually completes.
  Extra args land after the script's own `/p:`, so `./build-local.sh Release /p:CL_MPCount=4`
  overrides the default if you want to try a lower count.
- **It stalls on any translation unit, not a particular one.** Five times in a row it stopped on
  `dlssnr_forwarder.cpp` (right after `OptiScaler.dll` linked), which made the forwarder look
  special; the sixth stall was on `FSR3_Dx12_FG.cpp` mid-compile. Five hypotheses were tested and
  *disproved*: `CL_MPCount` (stalls at 16, 4, 1 and the default), an orphaned Wine process, the
  file's own content (stalls formatted or not), a corrupted intermediate (stalls after deleting
  `x64/Release/dlssnr_forwarder/`), and `/GL`/LTCG — an isolated control passed both with
  `WholeProgramOptimization=true` and without it. **Root cause is unknown.**
- **Never run anything else against the compiler prefix while a build is in flight.** The sixth
  stall happened with `tests/dlssnr-loopback/run.py` compiling its harness through the same prefix
  at the same time. That is not proven to be the cause — stalls also happen alone — but it is the
  one condition that was added, and the wine test tier is serial for exactly this reason.
- **Observed recovery** (once; not proven deterministic): build the forwarder project on its own
  with the original flags, then run the normal build, which then completes and copies both DLLs.

```bash
WINEPREFIX=~/.local/opt/msvc-wineprefix WINEDEBUG=-all ~/.local/opt/msvc/bin/x64/msbuild \
  OptiScaler/dlssnr/forwarder/dlssnr_forwarder.vcxproj /nologo /v:normal \
  /p:Configuration=Release /p:Platform=x64 \
  '/p:SolutionDir=Z:\home\gabrielmaia\Projects\personal\OptiScaler_DLSSNR\' \
  /p:CL_MPCount=4 /p:WholeProgramOptimization=true \
  /p:PreBuildEventUseInBuild=false /p:PostBuildEventUseInBuild=false > /tmp/fwd.log 2>&1
./build-local.sh Release /p:CL_MPCount=4 > /tmp/release.log 2>&1
```

  Evidence, manifests and logs: `~/.local/state/crimson-desert-dlss5/gpu-timing-validation/1885828a/`.
- When killing a stalled build, **use a bracket pattern**: `pgrep -f '[M]SBuild\.exe'`. A plain
  `pgrep -f 'build-local'` matches the very shell running it, so the kill takes out your own
  command (exit 144) and leaves the stall untouched.
- **Never pipe the script** (`./build-local.sh ... | tail`). `wineserver` daemonises and inherits
  stdout, so the pipe never reaches EOF and the output stays hidden even after the build finished —
  a success looks exactly like a hang. Redirect to a file instead.
- The Visual Studio pre/post-build events are PowerShell and are disabled
  (`PreBuildEventUseInBuild=false`). The script writes `resource_build_date.h` and
  `resource_build_commit.h` itself. The in-game menu title shows this build id and timestamp; use it
  to confirm which build a screenshot came from.
- Outputs `x64/<Config>/OptiScaler.dll` and `x64/<Config>/a/nvngx.dll_dlssnr.dll`, then copies both to
  `x64/out/`.

### CI

```bash
gh workflow run just_build_no_signature.yml --ref dlss-neural-rendering   # workflow "Build (No Signing)"
```

The artifact downloaded through the API is named `.zip` but is a 7z; extract with `bsdtar`.

### Windows

Open `OptiScaler.sln` in Visual Studio 2022, or
`msbuild OptiScaler.sln /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal`.
`package_release.ps1` assembles the release zip from an explicit allow-list: it refuses a stale
forwarder (checks exports), refuses any `Enabled=true` in the packaged ini, and never ships
`nvngx_dlssnr.dll` (NVIDIA's model; users supply their own).

### Submodules

Do **not** run `git submodule update --init --recursive` blindly. `external/FidelityFX-SDK` is huge
and slow to clone, so it is deliberately not checked out: only the 12 headers under
`ffx-api/include/ffx_api/` were fetched file-by-file from commit `c6efa6b`, which is all the build
needs. `external/FidelityFX-SDK-v2` is not used by the build and is empty. The other submodules
(`simpleini`, `unordered_dense`, `xess`, `vulkan`, `spdlog`, `magic_enum`, `nvapi`) are initialised.
Vendored headers the build includes directly live in `OptiScaler/include/` and `OptiScaler/library/`.

### Formatting

CI runs clang-format 20 over `OptiScaler/` excluding `external/`, `OptiScaler/include/` and
`/precompile/`. Style is in `.clang-format`: LLVM base, Allman braces, 4-space indent, 120 columns,
`SortIncludes: false`, `Type* ptr` pointer alignment.

**Use the pinned binary, not the one on `PATH`.** `PATH` has clang-format 22, which disagrees with
CI: it reported 13 upstream files as violations that version 20 accepts.

```bash
CF=/usr/lib/llvm20/bin/clang-format          # Arch package clang20; PATH has 22, do not use it
for f in $(fd -e cpp -e h . OptiScaler --exclude include --exclude external); do
    "$CF" --dry-run --Werror "$f" >/dev/null 2>&1 || echo "$f"
done
```

`.clang-format-ignore` at the repo root excludes the generated shader bytecode headers
(`*_Shader.h`, `*_Shader_Dx11.h`, `*_Shader_Vk.h`). They are emitted by `create_header.py`, so
formatting them is undone by the next shader rebuild; the CI `exclude-regex` carries `/precompile/`
for the same reason. The tree is currently clean under version 20.

### Tests

`tests/README.md` is the index; read it before adding or changing a suite. Twelve directories under
`tests/`, each self-contained with its own `run.py` and README, registered in `tests/suites.toml`.

```bash
python3 tests/run_all.py            # host tier, the default; 9 suites, ~45 s
python3 tests/run_all.py --list     # registry, tiers, and what is runnable here
python3 tests/run_all.py --tier all # adds the two wine suites
python3 tests/<name>/run.py         # one suite, unchanged
```

Three tiers. **host** needs only Python plus `g++`/`clang++` and runs in parallel: the nine `nr-*`
suites. **wine** needs the msvc-wine prefix and runs serially, because both suites contend for the
same prefix `build-local.sh` uses: `nr-gpu-timing-d3d12`, `vulkan-overlay` (which also needs a
graphical session and a working Vulkan loader). **wip** is `dlssnr-loopback`, registered with a
README, a scene shader and `harness.cpp`, but still no `run.py`.

`suites.toml` is a drift guard, not just a config file. A directory with no entry, or an entry with
no directory, fails the run. That is deliberate: this list had gone stale before.

Know what they do **not** cover: the `nr-*` suites exercise decision logic against fakes — they do
not execute NGX, a real D3D12 device, or a real shader, so a green suite is not evidence about GPU
behaviour, image quality or performance. `vulkan-overlay` builds a real DLL and drives real Vulkan.
`nr-before-upscale/verify-invariants.py` is an orphan by design: a one-shot evidence script pinned to
baseline `660303ec` that no runner calls.

CI: `.github/workflows/nr-tests.yml` runs the host tier on Linux for pushes and PRs that touch
`tests/` or `OptiScaler/`. It checks out only `simpleini`, `spdlog` and `vulkan` — never
FidelityFX-SDK. Upstream's `test.yml` is **not** a test workflow despite the name; it publishes the
nightly release. Nothing runs the wine tier in CI; that stays local.

In-game validation is still required (see "Test target"). For NR changes, follow the
per-change-type review in `OptiScaler/dlssnr/design/DEVELOPMENT.md` §3 before considering a change
done.

## Test target

Crimson Desert (Steam 3321460), `~/.local/share/Steam/steamapps/common/Crimson Desert/bin64/`:

- `OptiScaler.dll` installed as `dxgi.dll`; `nvngx.dll_dlssnr.dll` beside it; marker file
  `optiscaler_skip_vulkan_hooks` present.
- `_nvngx.dll` is a copy of `/usr/lib/nvidia/wine/_nvngx.dll`; refresh it when the NVIDIA driver
  updates. `nvngx_dlssnr.dll` (310.8.SF, the NR model) is user-supplied and not in the repo.
- Steam launch options: `WINEDLLOVERRIDES="dxgi=n,b" mangohud gamemoderun %command%`.
- Ini: `[DlssNr] Enabled=true ToggleKey=0x76` (F7); `[Menu] ShortcutKey=0x24` (Home);
  `[Log] LogToFile=true LogLevel=2`. Runtime log: `bin64/OptiScaler.log`.
- Full history, backups and rollback notes:
  `~/.local/state/crimson-desert-dlss5/backups/20260902-201621-optiscaler/README.md`.

## Precompiled shaders

Every compute pass under `OptiScaler/shaders/<pass>/` ships its bytecode as a header in
`precompile/` (`<Name>_Shader.h` for DX12, `<Name>_Shader_Dx11.h` for DX11, `<Name>_Shader_Vk.h` for
Vulkan). Editing the `.hlsl` changes nothing until the header is regenerated. Scripts are in
`OptiScaler/shaders/shader_tools/` and expect to run from the shader's `precompile/` directory:

```
..\..\shader_tools\build_precompiled_shader.bat <Name>     # dxc cs_6_0 -> _Shader.h ; fxc cs_5_0 -> _Shader_Dx11.h
..\..\shader_tools\build_precompiled_shader_vk.bat <Name>  # dxc -spirv -D VK_MODE -> _Shader_Vk.h
```

On Linux, `dxc.exe`/`fxc.exe` run under the msvc-wine prefix
(`WINEPREFIX=~/.local/opt/msvc-wineprefix wine ../../shader_tools/dxc.exe ...`), then
`python3 ../../shader_tools/create_header.py <in.cso|in.spv> <out.h> <array_name>`.

For the NR shader (`shaders/dlssnr/precompile/dlssnr.hlsl`) rebuild **both** targets: DX12 via dxc
`cs_6_0` into `DlssNr_Shader.h` (array `DlssNr_cso`) and Vulkan via dxc `-spirv -D VK_MODE` into
`DlssNr_Shader_Vk.h` (array `dlssnr_spv`). The committed `.cso` is a DXIL container, so
`DEVELOPMENT.md` invariant 6 is authoritative; the fxc `cs_5_0` instruction in `dlssnr/README.md`
is stale.

## Architecture

### One DLL, many disguises

`OptiScaler.dll` is renamed by the user to a system DLL name the game loads early (`dxgi.dll`,
`winmm.dll`, `version.dll`, `dbghelp.dll`, `d3d12.dll`, `winhttp.dll`, `wininet.dll`) or installed as
an `.asi` plugin. `Source.def` exports the union of all those APIs; `exports/` and `proxies/` forward
them to the real system DLL. `dllmain.cpp` works out which name it was loaded under (`dllNames`),
loads the real library, then installs Detours hooks from `hooks/` (D3D11, D3D12, DXGI, Vulkan,
Streamline, NVAPI, kernel loader). `wrapped/` wraps the DXGI factory and swapchain so the overlay can
draw at present time.

### Inputs → parameters → outputs

- **Inputs** (`inputs/`): the game's upscaler calls are intercepted per SDK and API. NVNGX (DLSS),
  FfxApi/FSR2/FSR3, and XeSS entry points all normalise into an `NVSDK_NGX_Parameter` block
  (`NVNGX_Parameter.cpp`), which is the internal lingua franca.
- **Outputs** (`upscalers/`): `IFeature` is the backend base. `IFeature_Dx12`, `IFeature_Dx11`,
  `IFeature_Vk` are native; `IFeature_Dx11wDx12` and `IFeature_VkwDx12` run a DX12-only backend on
  a background D3D12 device (`with_dx12/`). Concrete backends: `dlss/`, `dlssd/`, `fsr2/`, `fsr2_212/`,
  `fsr31/`, `ffx/`, `xess/`. `FeatureProvider_<Api>` instantiates the one selected by
  `Config::Dx12Upscaler` / `Dx11Upscaler` / `VulkanUpscaler` and handles hot-swapping.
- **Frame generation** (`framegen/`, `inputs/FG/`, `hudfix/`): the same input/output split via
  `IFGFeature`, with FSR3-FG, XeFG, DLSS-G and NVNGX replacement outputs.
- **Post passes** (`shaders/`): RCAS, output scaling, format/depth transfers, the NR pass. Each is a
  class that dispatches an embedded shader; nothing compiles HLSL at runtime.

### Globals

- `Config::Instance()` (`Config.h/.cpp`): every INI key is a `CustomOptional<T>` mirroring a section
  of `OptiScaler.ini`. Adding a key means four edits: declare with default in `Config.h`, read in
  `Config.cpp`, save in `Config.cpp`, add to the shipped `OptiScaler.ini`. Missing one is a silent
  no-op or a setting lost on restart.
- `State::Instance()` (`State.h`): runtime state (devices, current feature, detected GPU, Linux/Wine
  flags). The `Scoped*` RAII classes at the bottom of `State.h` temporarily disable spoofing, hooks,
  or wrapping for a thread; use them rather than toggling the flags by hand.
- Logging is spdlog through the `LOG_*` macros in `Logger.h`.

### Precompiled header rule (from CONTRIBUTING.md)

Every `.cpp` includes `"pch.h"` as its first non-comment line. Never include `pch.h` from a header.
Only large stable third-party/system headers go in `pch.h`; utilities go in `SysUtils.h` or a new
header.

### Linux / Proton patches (this fork's own layer)

These must survive every merge from `up`. Why they exist: under vkd3d-proton the Vulkan overlay
competes with Streamline DLSS-G's swapchain/pacer, producing fence timeouts and
`VK_ERROR_DEVICE_LOST`; the D3D12 overlay path coexists with native FG.

| Commit | File | Change |
|---|---|---|
| `255ca14`, `a4f1bbb` | `dllmain.cpp` | On Wine, if marker file `optiscaler_skip_vulkan_hooks` exists beside the DLL, skip `VulkanHooks::Hook` and set `State::vulkanHooksSkipped` |
| `33f2961` | `State.h` | `vulkanHooksSkipped` field |
| `7fe23f4` | `wrapped/wrapped_swapchain.cpp` | With `vulkanHooksSkipped`, bypass the DXVK direct-present shortcut so `MenuOverlayDx` draws the menu via D3D12 |
| `10c6760` | `menu/menu_overlay_vk.cpp` | `vkWaitForFences` bounded to 1 s; skip the menu frame on timeout |

Wine detection (`wine_get_version` → `State::isRunningOnLinux`) is upstream. The upstream fix for
issue #1101 (dxvk-nvapi recursion in `misc/IdentifyGpu.cpp`) is already in the tree; do not re-apply
it. `.github/workflows/` are registered on this fork so CI builds this branch.

### DLSS Neural Rendering module

`OptiScaler/dlssnr/` (menu, capture, exposure scan, forwarder, driver-proxy experiment) and
`OptiScaler/shaders/dlssnr/` (the pass: encode → NGX feature 18 → resolve). Read
`OptiScaler/dlssnr/README.md` before touching it; its "Design notes worth knowing" section records
dead ends already paid for with device hangs (never free under the GPU, one lock, debounced feature
rebuild, no temporal accumulator).

- **Forwarder.** The NVIDIA snippet rejects callers whose module path lacks `nvngx.dll`, so all NGX
  calls go through `nvngx.dll_dlssnr.dll` (`dlssnr/forwarder/`, built by `dlssnr_forwarder.vcxproj`
  into `x64/<Config>/a/`, also buildable standalone with CMake). It must not tail-call the snippet;
  results go through a `volatile` local so the forwarder's frame stays on the stack.
- **Call sites outside the module** are deliberately one-liners: `inputs/NVNGX_DLSS_Dx12.cpp` and
  `NVNGX_DLSS_Vk.cpp` (evaluate after upscale), `upscalers/IFeature_Dx11wDx12.cpp` and
  `IFeature_VkwDx12.cpp` (bridges), `menu/menu_common.cpp` (panel, timing row, toggle key, compare
  tags, scan meter), `hooks/Vulkan_Hooks.cpp` (device extensions), `hooks/D3D12_Hooks.cpp` and
  `resource_tracking/ResTrack_dx12.cpp` (exposure-scan resource notes), and the `[DlssNr]` block in
  `Config.h/.cpp` marked `removable as one block`. Everything must be inert when `DlssNrEnabled` is off.
- **Invariants** (`dlssnr/design/DEVELOPMENT.md` §1, enforced by review): default-identical when off;
  no control shown for a thing that does not exist; one quantity, one control; four-point config
  round-trip; `DlssNrConstants` (C++) and the `Params` cbuffer (HLSL) are one ordered scalar list,
  append-only at the end in both; both shader targets rebuilt; `gPassthrough` gate everywhere the
  encode is reproduced; Vulkan resources drained before free and abandoned (never destroyed) when
  their device is gone.
- **Design-doc-first.** New NR behaviour gets a design note in `dlssnr/design/` before code
  (`frame-hold.md`, `multi-point-anchoring.md` are the pattern). `FORWARDER_INVESTIGATION.md` is the
  evidence log for the attempt to drop the forwarder via the driver core (`DlssNr_Proxy`).
- The colour composition is RenoDX's design (clshortfuse); `Licenses/RenoDX_ATTRIBUTION.txt` must
  ship with any distributed build.

## Reference docs

`Config.md` (every INI key), `Spoofing.md`, `Issues.md`, `Changelog.md` (upstream OptiScaler
releases), `setup_linux.sh` / `setup_windows.bat` (end-user install: pick the proxy DLL name, spoofing
choices).
