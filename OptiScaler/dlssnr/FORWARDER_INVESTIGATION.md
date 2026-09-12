# Removing the forwarder — evidence log

The forwarder (`nvngx.dll_dlssnr.dll`) exists only to satisfy the snippet's caller check: the model
resolves its caller's module via `RtlPcToFileHeader` and rejects anything whose path does not contain
`nvngx.dll`, with `FAIL_PlatformError`, before looking at a single argument. Naming the shim
`nvngx.dll_dlssnr.dll` gets past that.

The way to remove it is the **proxy path**: don't call the snippet directly, call the driver core's
`NVSDK_NGX_D3D12_CreateFeature(18)` and let the core call the snippet — the snippet then sees the core
(`_nvngx.dll`) as its caller and the check passes for free. This is how RenoDX avoids a forwarder: it
detours the core's Create/Evaluate rather than calling the snippet itself.

The proxy path is already past the caller check. It fails later, at feature creation, with
`0xBAD0000B FAIL_UnableToInitializeFeature`. Everything below is about that.

Each entry: what was tried, what the log said, what it rules out.

---

## What is established

- The caller check is **not** the proxy path's blocker. The proxy is past it; `0xBAD0000B` is a real
  "could not build the feature", downstream of the caller check.
- The core routes feature 18 (it does not answer "unknown feature"). **This was read as "so the
  snippet is being reached", and that inference was wrong** -- see "The answer" below. The core
  accepts 18 as a known id and then has nothing to build it with.
- Re-initialising the core with `Init_Ext` is idempotent: it returns success and changes nothing,
  reporting the app id and SDK version the core first came up with. So the proxy path cannot change
  the app id or SDK version out from under the game's own DLSS. (log: "re-init at SDK 0x15 returned
  0x1 (idempotent)")
- The float setter lives at vtable slot 6 on the driver's capability block, same as the forwarder
  path finds. So the block is being driven correctly.

## Theories tried and disproven

### Warm-up retry — DISPROVEN (2026-09-01)
Feeder projects note the feature "re-creates a few seconds in, which normally clears" a failed state.
Tried: retry `CreateFeature(18)` up to 20 times, ~1 attempt / 20 frames, ~1.3s total, in Cyberpunk
with the game's own DLSS running (core fully warm).
Result: all 20 attempts returned `0xBAD0000B`, none succeeded.
Rules out: a transient warm-up window as the cause. The failure is stable, not timing.

## The answer (2026-09-12) — the proxy path was never possible

`SAOG0721/Magpie` (the experimental fork behind the DaVinci Resolve DLSS 5 filter) runs feature 18 in
production and documented its route. `bmitch87/DLSS5VKLayer` carries that write-up as
`extracted_pipeline_notes.md`, mined from Magpie's source with file and line references. It states,
flatly:

> Feature 18 is **not** created through the Core `nvngx.dll` route. Magpie uses a "signed snippet"
> route. Core is used only for parameter-block allocation and process-global init.

and records the same failure code this log has been chasing:

> Core `CreateFeature(18)` -> `0xbad0000b` (**Core has no NR implementation**).
> Direct `Init_Ext` without correct AppID / data dir / caller hook -> `0xbad00002`.
> Working route: `created=true path=signed-snippet`, Evaluate `result=0x1`.

So `0xBAD0000B` is not a snippet that failed to initialise. It is the driver core saying it has no
feature 18 to build. **The proxy path cannot be made to work**, and the three theories that used to be
listed here -- snippet discovery path, a discovery/registration call before Create, and
`GetScratchBufferSize(18)` -- were all hunting for a cause that does not exist. They are dropped, not
untested.

That also disposes of the premise this document opened with: "let the core call the snippet, and the
snippet sees the core as its caller". The core never calls the snippet for feature 18.

## The route that does work, and how to drop the forwarder

The same notes give the whole recipe, verified against a shipped binary:

1. `LoadLibraryExW(appDir\nvngx_dlssnr.dll, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`.
2. `GetProcAddress` for exactly five exports: `NVSDK_NGX_D3D12_Init_Ext`, `..._CreateFeature`,
   `..._EvaluateFeature`, `..._ReleaseFeature`, `..._Shutdown1`. All five are present in the model we
   ship against (checked here, 310.8.SF).
3. **Patch the snippet's own import table.** Find its `KERNEL32!GetModuleFileNameW` IAT slot and point
   it at a replacement that answers `L"nvngx.dll"` when the snippet asks about our module. Restore it
   on teardown.
4. `Init_Ext(0x0876232C, applicationDirectory, device12, NVSDK_NGX_Version_API, nullptr)` -- the AppID
   is a constant, and the DaVinci filter independently lists the same one. The data path is the DLL's
   own directory. No capability parameter block.
5. Create / Evaluate / Release / Shutdown all through the **snippet's** exports. The core is used only
   for `AllocateParameters` / `GetCapabilityParameters` / `DestroyParameters` and process-global init.
6. Teardown order is fixed: GPU drain -> snippet `ReleaseFeature` -> core `DestroyParameters` ->
   snippet `Shutdown1` -> restore the IAT -> `FreeLibrary` -> core `Shutdown1`.
7. Every call crosses a `__try/__except` boundary, fail-closed to pass-through.

### Why patching the second half of the check is enough

This log said the snippet resolves its caller with `RtlPcToFileHeader`. That is true and it is only
half of it: `RtlPcToFileHeader` turns a return address into a module base, and `GetModuleFileNameW`
turns that base into a path to compare. Both are imported by the model shipped here -- verified with
`winedump -j import`: `RtlPcToFileHeader` at ordinal 1279 and `GetModuleFileNameW` at 657, from
`KERNEL32.dll`.

The first half cannot be faked from outside; the second half is an ordinary IAT slot. That is the
whole trick, and it is why the forwarder is avoidable without a second DLL.

### What is still ours to establish

None of the above has been run in this tree. Before it replaces the forwarder:

- Under Wine/Proton, not just Windows. Every source above is a Windows project.
- Alongside the game's own NGX use. Magpie owns its process and its own D3D12 device; we live inside a
  game that is already talking to the core. An IAT patch on the snippet is process-global for as long
  as it is installed, which is why the teardown order above restores it.
- The forwarder is not only a caller-check answer here: it is also the ABI v2 handshake boundary
  (`dlssnr_set_host_abi`, `dlssnr_abi_version`) and the caller gate that keeps a mismatched host from
  running. Dropping the DLL means finding a new home for that, or accepting its loss deliberately.

## How to reproduce
Set `[DlssNr] UseProxy=true`. The path is off by default and does not fall back automatically, so a
failure is visible rather than masked by the forwarder quietly doing the work.
