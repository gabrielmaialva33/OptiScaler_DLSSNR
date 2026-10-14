# deploy-safety

Runs the real `tools/deploy-to-game.sh` against fake game directories built in a temp dir and checks
what it refused, what it wrote, what its receipt says, and what `--uninstall` put back. The fake DLLs
are a few bytes carrying the strings the script recognises builds by (`OptiScaler working as`,
`dlssnr_call_`, `ReShade`); the OptiScaler fake also names `ReShade64.dll`, as the real one does, so a
check that mistook OptiScaler for ReShade would fail here. The script is always given `--source-dir`,
so it never reaches `build-local.sh`.

Cases:

- a clean install, a re-install over our own build (no backup), an identical build left unwritten
  (same inode), and a proxy name given positionally;
- an OptiScaler build with no receipt (what the old script left): replaced, kept under
  `.retoque/backup/`, and restored by `--uninstall`;
- a foreign `dxgi.dll` refused (exit 4) and, with `--replace-foreign`, set aside and restored;
- ReShade as `dxgi.dll`: refused without a flag; with `--keep-reshade`, renamed to `ReShade64.dll` and
  `[Plugins] LoadReshade=true` set by replacing the line in place, in the shipped ini (exactly one line
  differs) and in a user's CRLF ini that has the key twice in other spellings plus once in another
  section (all `[Plugins]` lines true, the other section untouched, line count unchanged); the key added
  once when the section lacks it; refused when `ReShade64.dll` or `d3d12-ReShade64.dll` is taken;
- a second proxy name holding OptiScaler (`WINMM.DLL`, so the lookup ignores case) refused (exit 5),
  allowed with `--allow-second-proxy`, and the game's own non-OptiScaler `winmm.dll` not refused;
- a case variant of the proxy name (`DXGI.DLL`) refused;
- a Python child that mmaps the installed `dxgi.dll` stands in for a running game: deploy, uninstall
  and dry run all refuse (exit 3), name its pid, and change nothing;
- `--dry-run` writes nothing, for a fresh install, for Keep-ReShade and for `--uninstall`;
- a failure injected after the second step (`RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP`) leaves a receipt
  marked `partial` listing exactly the two files written; `--uninstall` from it empties the directory,
  and a later deploy completes;
- `--uninstall` leaves a file the user changed since (the ini) in place, notes it and keeps it in the
  receipt; without a receipt it touches nothing; a receipt path pointing outside the directory is
  refused;
- the libxell rename is recorded and reversed;
- `shellcheck -S warning` is clean, when shellcheck is installed.

After every uninstall the directory is compared to a snapshot taken before the deploy: every entry,
hidden ones included, with content hash and mode.

What it does not cover: Wine. Whether a game under Proton maps its DLLs in a way `/proc/<pid>/maps`
shows is taken from the 2026-09-19 investigation in `CLAUDE.md`, not tested here; the stand-in is a
Linux process mapping a file, which is the mechanism the check reads. Pure Python and bash, no compiler.

```bash
python3 tests/deploy-safety/run.py
```
