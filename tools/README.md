# tools/

Scripts this fork owns. Upstream's own scripts (`setup_linux.sh`, `setup_windows.bat`,
`OptiScaler/shaders/shader_tools/`) stay where upstream put them.

Every script derives the repo root from its own location, so each runs from any cwd.

## build-local.sh

Builds OptiScaler and the DLSS-NR forwarder on Linux with msvc-wine (MSVC under Wine), under a
watchdog that recovers from Wine's intermittent msbuild stalls.

```bash
tools/build-local.sh [Release|Debug] [extra msbuild args]
```

Outputs go to `x64/<Config>/` and `x64/out/`. `./build-local.sh` at the repo root is a stub that
`exec`s this script; it exists because that path is in muscle memory and in old notes.
Do not pipe its output; redirect it to a file (see `CLAUDE.md`, "Build").

## deploy-to-game.sh

Copies the built `x64/out/OptiScaler.dll` and forwarder into a game directory under a proxy DLL
name, installs `OptiScaler.ini` only if the game has none, and reports the Vulkan-hook state.
Builds first if there is no output yet. Never run it while the game is running.

```bash
tools/deploy-to-game.sh <game-directory> [dxgi.dll|version.dll|d3d12.dll|winmm.dll|OptiScaler.dll]
```

## package_release.ps1

Windows. Assembles the release zip from an explicit allow-list under `release/`; it refuses a stale
forwarder, refuses any `Enabled=true` in the packaged ini, and never ships `nvngx_dlssnr.dll`.

```powershell
pwsh tools\package_release.ps1 [-Version <tag>] [-SkipBuild]
```

## nrlog_summary.py

Summarises an `OptiScaler.log` from a DLSS-NR run (build, NR coverage, composition changes,
cadence, DLSS-G transitions, errors, GPU timing statistics), or compares two logs side by side.
Plain Python 3, no dependencies.

```bash
python3 tools/nrlog_summary.py <OptiScaler.log> [...]
python3 tools/nrlog_summary.py --compare <LogA> <LogB>
```

## local/

`tools/local/` is gitignored. Keep personal, machine-specific helper scripts there.
