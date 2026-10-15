# windows-installer

Runs the real `tools/install-windows.ps1` against fake game folders and checks what it refused, what it wrote,
what its receipt says and what `-Uninstall` put back, the way `deploy-safety` does for
`tools/deploy-to-game.sh`. The fake DLLs are a few bytes carrying the strings both scripts recognise builds by
(`OptiScaler working as`, `dlssnr_call_`, `ReShade`), never a real binary. Every game path has brackets and
spaces in it (`game [x] dir`), which the PowerShell cmdlets' `-Path` would read as a wildcard.

Two parts:

- **Static checks**, always: `install-windows.ps1`, `install-windows.cmd` and `cases.ps1` are pure ASCII
  (Windows PowerShell 5.1 reads a script with no byte-order mark in the ANSI code page); the `.cmd` has CRLF
  line ends, starts the `.ps1` with `-NoProfile -ExecutionPolicy Bypass -File` and nothing else of note;
  `tools/package_release.ps1` ships both files and still has its stale-forwarder and `Enabled=true` refusals.
- **The cases** (`cases.ps1`), in PowerShell 7.4 LTS: in the pinned `mcr.microsoft.com/powershell` image when
  docker answers, otherwise in a local `pwsh`. The files are streamed into the container with tar, because the
  docker context on the development machine is a remote host where a bind mount would resolve. The container
  runs with `--network none`.

With neither docker nor `pwsh`, the cases print `SKIP:` and the suite passes on the static checks alone, so
the host tier stays green on a machine without them. `--runner docker` or `--runner local` makes a missing
runner a failure instead.

Cases:

- both scripts parse; the installer uses none of PowerShell 7's own syntax (ternary, `&&`/`||`, `??`, `?.`),
  no `$IsWindows`, no multi-child `Join-Path`;
- a clean install (the proxy, `OptiScaler.dll` beside it, the forwarder, the release's `OptiScaler\` support
  folder, the shipped ini), with the receipt's records, order and hashes, no byte-order mark and no CR in it,
  and the missing-model warning; the proxy name given positionally and in capitals; bad names refused;
- a re-install over our own build (no backup, the user's ini kept), an identical build left unwritten;
- earlier OptiScaler builds and an older support file with no receipt: backed up, replaced, restored;
- a foreign `dxgi.dll` refused (exit 4), and with `-ReplaceForeign` set aside and restored;
- ReShade as `dxgi.dll`: refused without a flag; with `-KeepReShade`, renamed and `[Plugins]
  LoadReshade=true` set by replacing the line, in the shipped ini (one line differs) and in a CRLF user ini
  with the key twice in other spellings and once in another section; the key added once when missing;
  refused when `ReShade64.dll` or `d3d12-ReShade64.dll` is taken;
- a second proxy name holding OptiScaler (`WINMM.DLL`) refused (exit 5), allowed with `-AllowSecondProxy`;
  a game's own `winmm.dll` not refused; a case variant (`DXGI.DLL`) refused on a case-sensitive file system;
- the game running (exit 3, nothing written, for install, uninstall and dry run) through each of the two checks;
  see below for what that means here;
- dry runs write nothing; a failure injected after step 2 (`RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP`, the variable
  the Linux script reads) leaves a `partial` receipt listing exactly those two files, which `-Uninstall`
  empties and a later install completes;
- `-Uninstall` leaves a file changed since in place and keeps it in the receipt; without a receipt it touches
  nothing; receipt paths that leave the folder (`../`, `..\`, `C:/`, `a/.. /`, a stream) are refused;
- libxell: not installed when the game ships its own, an older one of ours renamed to `.disabled`;
- `nvngx_dlssnr.dll` is never touched: not by install or uninstall, not when a receipt lists it, not when a
  release carries one;
- the zip extracted into the game folder and the script run from there (its default source);
- across the two scripts: a `deploy-to-game.sh` receipt (complete and partial) uninstalled by the PowerShell
  script and the reverse; both scripts given the same folder write the same receipt records and the same
  files (the edited ini byte for byte); each re-installs over the other's receipt without backups.

After every uninstall the folder is compared to a snapshot taken before the install: every entry, hidden ones
included, with its content hash. Every stdout line the installer prints is checked to start with a tag or an
indent, which catches output leaking from a function.

What it does not cover:

- **Windows itself.** The cases run on Linux. Whether a running game's DLL fails the exclusive open with a
  sharing violation, what `Get-Process` can read from an elevated or anti-cheat-protected game, `File.Replace`
  on NTFS, Windows path normalisation and case-insensitive names (on Windows a `DXGI.DLL` is the same file, so
  the case-variant refusal never applies there), and the `.cmd` wrapper: none of it is executed. The two
  running-game checks are exercised through their Linux stand-ins: .NET reads a Linux process's modules from
  `/proc/<pid>/maps`, so a copy of `sleep` run from the game folder is a process with a module loaded from it;
  and .NET emulates `FileShare.None` with `flock`, so a handle this process holds makes the exclusive open
  fail with the same `IOException` a loaded DLL causes on Windows.
- **Windows PowerShell 5.1.** Only PowerShell 7.4 runs the cases. 5.1 compatibility is held by the syntax
  checks above and by keeping to APIs .NET Framework has (`File.Replace` where 7 uses `File.Move` with
  overwrite).

```bash
python3 tests/windows-installer/run.py [--runner auto|docker|local]
```
