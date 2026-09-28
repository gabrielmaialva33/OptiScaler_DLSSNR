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

## Resolution (2026-09-28): the direct loader, and the core route reopened

### The direct loader is implemented: `[DlssNr] ModelLoader=direct`

`DlssNr_DirectRuntime` does step 3 above from inside OptiScaler.dll, with no second DLL. The default
stays `forwarder`, and the forwarder is unchanged.

**Provenance.** The technique is **wilsjo2**'s (`OptiScaler-DLSSNR-PreSR-Multipass`, releases
v0.8.1-nr-direct-runtime and v0.8.3, branch `codex/nr-direct-runtime` at `47e134cb`, GPL-3, the same
licence as this tree). It covers finding the model's `GetModuleFileNameW` **and** `...A` import slots
by name rather than by offset (`DlssNr_RuntimeImports.h`), and chaining whatever each slot already
held, loader and overlay wrappers included, instead of demanding the pristine export (the v0.8.3
change). It also covers answering only for the calling OptiScaler module and only during a direct
call. Ours is a rewrite against our own structures: `DlssNr_PeScan.h` for the parse, and the
forwarder's own call bodies. Attribution sits in both headers and in the commit.

**How it differs from wilsjo2's runtime, and from the recipe above:**

- **Explicit key, not a fallback.** wilsjo2 tries the driver first and falls back to direct after a
  driver failure. Here the user picks one.
- **The forwarder's calls, not Magpie's.** The application id stays `0x24480451` and the capability
  block stays; the recipe's `0x0876232C` and null block are not used. Every parameter write, in
  order, is the forwarder's, and so are the model calls, `FaultFilter` and `kFaulted`.
  `tests/nr-model-loader` holds the two files to that.
- **The alias is the forwarder's full path:** `<OptiScaler dir>\nvngx.dll_dlssnr.dll`, not
  `L"nvngx.dll"`. The model sees what it saw through the forwarder, directory included.
- **Scoped per thread.** A `thread_local` alias is set by `Guarded` for exactly the duration of the
  call. Every other query, from the model on another thread or about another module, goes to the
  previous target.
- **Never restored.** The model is never unloaded, as with the forwarder. OptiScaler pins itself
  (`GET_MODULE_HANDLE_EX_FLAG_PIN`) before adapting, so the slots cannot outlive the code they point
  at. wilsjo2 restores the slots at teardown instead.
- **Refuses rather than guesses.** An import table that does not parse, no caller-path slot, or two
  slots of one API with different targets: logged, `loadFailed` sticks, the menu shows why, and
  nothing is patched.
- **D3D12 only:** the after-upscale pass, the D3D12 present pass and the D3D11 bridge host. Native
  Vulkan and the native D3D11 probe keep the forwarder whatever the key says, as in wilsjo2's.

**What "still ours to establish" became:**

- *Under Wine/Proton:* still open in a game. The host test confirms that this machine's model
  (310.8.2.0) has one W and one A slot that the parser finds, which is the precondition.
- *Alongside the game's NGX:* answered by design. The slots answer for OptiScaler's module only, only
  on a thread inside our call, and chain everything else. It still needs measuring.
- *The ABI handshake:* not needed. The direct runtime and the host are one binary, so their argument
  lists cannot drift apart; `dlssnr_set_host_abi` stays the forwarder's.

### The core route is real again on current drivers

The 2026-09-12 answer above ("Core has no NR implementation") was right about the loaders it
described, and is **not** right about NVIDIA 32.0.16.1664 (616.64) and later. From that version the
loader's feature table names `dlssnr` at entry 18, and the loader routes `CreateFeature(18)` into the
model itself (NIGos/dlss5-bridge, MIT). This workstation's wine loader has been 32.0.16.1691 since the
2026-09-11 driver update, and it routes. `UseProxy` was last measured on 2026-09-01 under 610.57.04.

So the premise this document opened with, "let the core call the snippet", is live again. It has one
known failure: model 310.8.0.0 on that route faults inside D3D12. `UseProxy` now refuses exactly that
pairing before it creates anything, and allows 310.8.2.0 through. The details, the exposure of each
route and the checks are in `design/ngx-driver-616.md`.

## How to reproduce
Set `[DlssNr] UseProxy=true`. The path is off by default and does not fall back automatically, so a
failure is visible rather than masked by the forwarder quietly doing the work. On a loader that
routes feature 18 with a model at 310.8.0.0 or older, it refuses with a log line instead of trying.

For the direct loader, set `[DlssNr] ModelLoader=direct`. Rename `nvngx.dll_dlssnr.dll` away from the
game folder to prove it is not used on the D3D12 route.
