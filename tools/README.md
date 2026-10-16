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

## test-rendering.sh

Runs the host tier with four workers, builds Release, then runs D3D12 camera/texture
regressions, the moving-scene production NR A/B fixture and the complete production
DLSS-G provider fixture, in that order. Wine/GPU work is serial. Any failed check stops
the command; missing DLSS-G prerequisites fail the combined run too.

```bash
tools/test-rendering.sh              # complete sequence, without opening a game
tools/test-rendering.sh --host-only  # portable host policy checks
tools/test-rendering.sh --no-build   # use the already built x64/out DLL
```

Each GPU suite retains its own ignored `artifacts/` directory with logs and results.
The model and plugin must already be available locally; the fixtures never download
them or deploy to game directories. A passing run covers the synthetic scenarios;
image metrics and GPU timings are measurements, not a claim of better quality or FPS.
See [the tests index](../tests/README.md) for the individual boundaries.

## deploy-to-game.sh

Copies the built `x64/out/OptiScaler.dll` and forwarder into a game directory under a proxy DLL
name, installs `OptiScaler.ini` only if the game has none, and reports the Vulkan-hook state.
Builds first if there is no output yet. Every change is planned before the first write, and it
writes nothing when any of these holds:

- **The game is running** (exit 3): some process maps a file under the directory, read from
  `/proc/<pid>/maps`. It names the pid, its `comm` and its executable. There is no override. `pgrep -x` is
  not used because `comm` is cut at 15 characters, which is how Cyberpunk was overwritten while it ran
  (`CLAUDE.md`). Files are written to a temp file and renamed into place, so the old inode is never
  truncated even then.
- **A file at one of our names is not ours** (exit 4). Ours means the receipt's hash matches, or the
  file is byte-identical to the build being installed. An OptiScaler build or forwarder with no receipt
  (anything the script deployed before it had receipts) is backed up and replaced without a flag.
  Anything else needs a flag. ReShade at the proxy name takes `--keep-reshade`. Anything else takes
  `--replace-foreign`. A name that exists in another case (`DXGI.DLL`), a symlink or a directory is
  refused too.
- **Another proxy name already holds OptiScaler** (exit 5), e.g. `winmm.dll` when deploying as
  `dxgi.dll`. The Divinity trap: both load and their hooks fight. Override with `--allow-second-proxy`.

`--keep-reshade` renames ReShade (`dxgi.dll` to `ReShade64.dll`, `d3d12.dll` to `d3d12-ReShade64.dll`)
and sets `[Plugins] LoadReshade=true` in `OptiScaler.ini`. It replaces the existing line, keeping its
spelling and line ending, and adds one only when the section has none, because the parser keeps the last
of two. It refuses when `ReShade64.dll` already exists. A user ini it edits is backed up first.

What a deploy wrote, set aside or renamed goes into `<game>/.retoque/receipt.txt`. Backups go into
`<game>/.retoque/backup/<UTC stamp>/`, never `*.bak` beside the DLL. The receipt is tab-separated text
(format in the script's header) and is rewritten atomically after every step. A deploy that stops
part-way leaves it marked `partial`, listing exactly what was done; the next deploy, or `--uninstall`,
picks up from it. `--uninstall` removes only recorded files whose hash still matches, restores the
backups, and undoes the renames (ReShade, libxell). Anything that changed since, such as an ini you
edited, is left in place with a note and stays in the receipt.

```bash
tools/deploy-to-game.sh [options] <game-directory> [dxgi.dll|winmm.dll|version.dll|d3d12.dll|dbghelp.dll|winhttp.dll|wininet.dll|OptiScaler.dll|<name>.asi]
tools/deploy-to-game.sh --uninstall [--dry-run] <game-directory>

  -n, --dry-run           print the plan (and any refusal), write nothing
  --keep-reshade          keep a ReShade proxy as ReShade64.dll, loaded by OptiScaler
  --replace-foreign       replace a file that is not ours; the original goes to .retoque/backup/
  --allow-second-proxy    deploy although another proxy name here holds OptiScaler
  --source-dir DIR        take the DLLs from DIR instead of x64/out (never builds then)
```

`tests/deploy-safety` runs it against fake game directories.

## install-windows.ps1 and install-windows.cmd

The Windows counterpart of `deploy-to-game.sh`, for end users: both ship in the release zip beside the DLLs
(`package_release.ps1` refuses to build a release without them). The `.ps1` runs on Windows PowerShell 5.1
and PowerShell 7 with no modules. The `.cmd` is the double-click entry: it asks for the game folder and
whether to install, uninstall or dry-run, then starts the `.ps1` with `-ExecutionPolicy Bypass` for that one
`powershell.exe` process (no policy is changed for the user or the machine). With arguments it passes them
through and adds nothing.

It installs from its own folder: `OptiScaler.dll` under the proxy name and as `OptiScaler.dll` beside it, the
forwarder, the release's `OptiScaler\` support folder (FSR, XeSS, Agility SDK), and `OptiScaler.ini` only
when the game has none. The refusals, exit codes, Keep-ReShade handling, backups and `-Uninstall` are those of
`deploy-to-game.sh` above. The running-game check is Windows-native: refuse when any process has a module
loaded from the game folder, or when a file it would touch cannot be opened for writing with no sharing (an
elevated or anti-cheat-protected game hides its module list, but not that). The receipt is the same file in
the same format, so either script uninstalls what the other installed; the one difference is that the Linux
script, which creates no folders, leaves the emptied `OptiScaler\` folders behind. `nvngx_dlssnr.dll`,
NVIDIA's model, is never copied, moved or removed; a missing one is reported.

```powershell
install-windows.cmd                                         # asks
install-windows.cmd -GameDir "D:\Games\Crimson Desert\bin64" [-ProxyName winmm.dll] [-DryRun]
install-windows.cmd -GameDir "D:\Games\Crimson Desert\bin64" -Uninstall [-DryRun]

  -KeepReShade        keep a ReShade proxy as ReShade64.dll, loaded by OptiScaler
  -ReplaceForeign     replace a file that is not ours; the original goes to .retoque\backup\
  -AllowSecondProxy   install although another proxy name here holds OptiScaler
  -SourceDir <dir>    take the files from <dir> instead of the script's folder
```

`tests/windows-installer` runs it against fake game folders in a PowerShell 7 container, crossing receipts
with `deploy-to-game.sh` both ways. Windows itself (a real loaded DLL, NTFS, Windows PowerShell 5.1, the `.cmd`)
is not exercised there; its README says what stands in for what.

## package_release.ps1

Windows. Assembles the release zip from an explicit allow-list under `release/`; it refuses a stale
forwarder, refuses any `Enabled=true` in the packaged ini, and never ships `nvngx_dlssnr.dll`. It adds
`install-windows.ps1` and `install-windows.cmd` from `tools/` (the `.cmd` rewritten with CRLF line ends) and
refuses when either is missing.

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
