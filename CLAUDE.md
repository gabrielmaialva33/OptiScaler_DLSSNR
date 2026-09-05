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

CI runs clang-format 20 over `OptiScaler/` excluding `external/` and `OptiScaler/include/`. Style is in
`.clang-format`: LLVM base, Allman braces, 4-space indent, 120 columns, `SortIncludes: false`,
`Type* ptr` pointer alignment. clang-format is not currently installed on this workstation.

```bash
clang-format --dry-run --Werror $(fd -e cpp -e h . OptiScaler --exclude include)
```

### Tests

There are none. Validation is in-game (see "Test target"). For NR changes, follow the per-change-type
review in `OptiScaler/dlssnr/design/DEVELOPMENT.md` §3 before considering a change done.

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
