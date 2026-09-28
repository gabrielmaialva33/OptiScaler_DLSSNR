# NVIDIA 616.64 and the loader's own route to feature 18

**Status (2026-09-28):** implemented behind no switch — a log line at the first feature build, and a
refusal on the one route that is exposed (`UseProxy`). Not yet checked in a game.

## What changed in the driver

NIGos/dlss5-bridge (MIT) measured it on an RTX 5090 and wrote it down in `dlss5-bridge.cpp`
(`NrSnippetIsOlderThanLoader`, `PatchNgxFeatureTable`) and its README (`ngx_loader`):

- The NGX loader `_nvngx.dll` carries a 19-entry table in writable data: feature id → snippet name,
  as absolute pointers to wide strings. Entry 1 is `L"dlss"`, entry 11 `L"dlssg"`.
- In **32.0.16.1656** (616.56) entry 18 points at `L""`. `NGXSecureLoadFeature` loads nothing for an
  empty name (dlss5-bridge reads `0xBAD00001` there), so the core's `CreateFeature(18)` fails. That
  is the `0xBAD0000B` this repository measured from `CreateFeature` on 2026-08-31 and 09-01, and that
  `FORWARDER_INVESTIGATION.md` explains.
- In **32.0.16.1664** (616.64) and 1686, entry 18 points at `L"dlssnr"`, and `DLSSNR.Available`
  appears in the file. **The loader now routes feature 18 into `nvngx_dlssnr.dll` itself.** Entries
  0–17 are byte-identical between the two versions.
- Model **310.8.0.0** on that route calls `SetDescriptorHeaps` with a heap whose description reads
  back as garbage, and D3D12 faults. With a ReShade NR add-on next door, the game terminated or
  stopped presenting. The same model on 1656's loader delivers frames, so the pairing is broken, not
  either half. dlss5-bridge presumes a newer model is fixed and stands down for one.
- Their fix, `ngx_loader=0` (the default), blanks entry 18 back to `L""` in the loaded image. Nothing
  on disk is touched.

## What this machine has

Read from the files on 2026-09-28 (`tests/nr-model-loader` repeats the scan every run when they are
present):

| File | Version | Finding |
|---|---|---|
| `/usr/lib/nvidia/wine/_nvngx.dll` (Linux driver 615.71.09, installed 2026-09-11) | **32.0.16.1691** | entry 18 names `dlssnr`: **routes feature 18 itself**. Table at +0x15fa60. `DLSSNR.Available` present |
| `nvngx_dlssnr.dll` in all ten game folders | **310.8.2.0**, 165 830 144 bytes | newer than the faulting 310.8.0.0; imports `GetModuleFileNameW` and `...A` by name |

Two things follow.

- **Linux gets the new route too, under a Windows number.** The wine loader shipped with a Linux
  driver carries a Windows-branch version of its own: 615.71.09 ships 32.0.16.1691, which NVIDIA's
  numbering reads as "616.91". So on Proton the log names the loader's build, not the driver. It is
  still the right number to know, because the route lives in that file.
- **Every NR session measured on this workstation since 2026-09-11 already ran under the new
  loader,** through the forwarder: Crimson Desert, Divinity, The Witcher 3, DOOM Eternal and PCSX2.
  That is the evidence that the forwarder route is not affected (see below).

**Rafael's driver version is not recoverable.** His copied logs
(`~/.local/state/dlss5/rafael-mhw-20260919/`) name the DriverStore folder
(`nvmdi.inf_amd64_21e6b42b376b80b2`, a hash) and Windows 11 10.0.26200, and nothing else. No line in
OptiScaler ever logged the driver, the loader or the model. That is the gap the log line below
closes, and his next session will answer it. He runs the forwarder route with `UseProxy` at its
default (off), so whatever his driver, he is on an unexposed route.

## Exposure, route by route

| Route | How it reaches the model | Takes the loader's route? |
|---|---|---|
| Forwarder (default; D3D12, Vulkan, D3D11 probe) | `nvngx.dll_dlssnr.dll` calls the model's own `NVSDK_NGX_D3D12_*` exports | **No.** The loader's table is never consulted. Measured working on the 1691 loader |
| `ModelLoader=direct` (new; D3D12) | OptiScaler calls the same exports itself | **No**, for the same reason |
| `UseProxy=true` (`DlssNr_Proxy`, off by default) | the core's `CreateFeature(18)` | **Yes, exactly this route.** Last measured 2026-09-01 on 610.57.04, before the route existed |
| A game shipping NR through Streamline | the game's own NGX calls | Yes, but it is not our code |

So the combination dlss5-bridge measured, loader route plus 310.8.0.0, can reach OptiScaler only
through `UseProxy`. And with 310.8.2.0, which is what every machine here has, not even through that.

## What was done

1. **One log line per model at the first feature build** (`DlssNr_NgxInfo`, called from
   `DlssNr_Dx12::Dispatch` before `ModelLog::Install`):
   ```
   DLSS-NR NGX: loader <path> version 32.0.16.1691 (Windows-branch loader build 616.91, not the Linux driver version); it routes feature 18 itself (entry 18 names "dlssnr")
   DLSS-NR NGX: model <path> version 310.8.2.0, 165830144 bytes; not loaded in this process yet
   ```
   On Windows the parenthesis reads `(driver 616.64)`. The second line says `already loaded from ...`
   when something, the loader included, mapped the model before we did. The table is read from the
   loaded image under `__try`, so an unrecognised driver says "feature table not recognised" and
   nothing else happens.
2. **A warning when the pairing matches:** route Present and model 310.8.0.0 or older. The predicate
   is dlss5-bridge's (`NrSnippetIsOlderThanLoader`), so it stands down by itself for a newer model.
3. **`UseProxy` refuses that pairing before `CreateFeature(18)`**:
   `DLSS-NR (proxy): refused -- this driver's NGX loader creates feature 18 itself and the model ...`.
   It refuses rather than catches because the fault lands inside D3D12 on the game's thread, and the
   proxy path's core calls have no guard around them. A caught fault would still leave a broken
   command list behind.

Not done: **patching the table.** It fixes nothing on our two production routes, which never read
it. And blanking entry 18 in the process-wide loader would also take the route from a game that
ships NR through Streamline. If a future route of ours needs the core's `CreateFeature(18)` with an
old model, dlss5-bridge's `PatchNgxFeatureTable` is the reference, and `FindFeature18Route` already
returns the table's offset.

A consequence worth measuring: **`UseProxy` may now work.** `FORWARDER_INVESTIGATION.md` concluded
the core "has no feature 18". That was true of the loaders it was measured on and is not true of
1664 and later, this machine's included. With 310.8.2.0 on such a loader, the core route is the one
the NR add-on beside dlss5-bridge takes, and check 4 below will show whether it builds here.

## The direct loader

`[DlssNr] ModelLoader=direct` is the other half of the forwarder question. It is recorded, with its
provenance (wilsjo2, GPL-3), in `FORWARDER_INVESTIGATION.md` under 2026-09-28. It is independent of
the driver: it calls the model exactly as the forwarder does.

## Checks for the lead, in a game

Use Crimson Desert (after-upscale) or PCSX2 (present pass) with `LogLevel=2`, NR on.

1. **Default, nothing changed in the ini.** Expect `DLSS-NR forwarder loaded from ...`, both
   `DLSS-NR NGX:` lines as above, the route reported as *routes feature 18 itself*, the model as
   310.8.2.0, and no warning. The image is identical to the previous build.
2. **`ModelLoader=direct`**, with `nvngx.dll_dlssnr.dll` renamed away from the game folder, to prove
   it is not used. Expect:
   - `DLSS-NR: model loader is direct`;
   - `DLSS-NR direct: 2 caller-path import slot(s) of the model adapted (0 already), answering <dir>\nvngx.dll_dlssnr.dll ...`;
   - `DLSS-NR running after upscale ...`, and **no** `DLSS-NR create failed: init 0xBAD00002`
     (`FAIL_PlatformError`: the caller check refused the adapted answer);
   - an image that matches the forwarder's with the same settings, and a model time within noise.
   Then switch F7 off and on, and change Style in the menu (the feature rebuilds), to exercise
   release and create again.
3. **`ModelLoader=direct` on Divinity** (the D3D11 bridge host) for the same lines. On DOOM Eternal
   (Vulkan) the forwarder must still be loaded and used: direct is D3D12 only.
4. **`UseProxy=true`** with the forwarder present. With 310.8.2.0 there is no refusal, so this is the
   re-measurement of the core route on a loader that has it. Either `DLSS-NR (proxy)` builds a feature,
   in which case the forwarder question has a third answer, or it logs the failing code. Copy
   `nvngx_dlssnr.dll` 310.8.0.0 in, if one is at hand, to see the refusal line instead.
5. On Rafael's PC, after the next deploy: read the two `DLSS-NR NGX:` lines from his log. That is his
   driver version.

Open, and cheap if wanted: `sudo` into a root snapshot from before 2026-09-11 to recover the
610.57.04 wine loader and confirm entry 18 was empty there. The `0xBAD0000B` history says it was, but
nobody read that file.
