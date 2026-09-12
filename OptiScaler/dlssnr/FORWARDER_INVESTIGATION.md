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
- The core routes feature 18 (it does not answer "unknown feature"), so the snippet is being reached.
  It is the *initialisation* of the feature that fails.
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

## Theories not yet tried

- **Snippet discovery path.** The core loads snippets from the path list it was given at `Init` (the
  game's DLSS directory) plus the app directory. If `nvngx_dlssnr.dll` is not on a path the core
  searches, it finds feature 18 and has nothing to build it from -> UnableToInitialize. Since Init is
  idempotent we cannot add a path after the game's own Init; the snippet would have to sit where the
  core already looks. Test: place `nvngx_dlssnr.dll` beside the exe / in the game's DLSS plugin dir
  and check whether the core loads it (Streamline/NGX log should show the load).
- **Feature registration / discovery step.** The game only ever registers the SR/RR/FG snippets with
  the core, never NR. Feature 18 may need a discovery call (`GetFeatureRequirements` /
  `UpdateFeature`) before `CreateFeature` will build it. Test: call the D3D12 requirements query for
  feature 18 through the core before creating, and see whether that changes the create result.
- **Scratch buffer.** `CreateFeature` may need `GetScratchBufferSize(18)` satisfied first. Untested.
- **Answer the caller-path check instead of satisfying it.** The forwarder exists because the snippet
  rejects a caller whose module path lacks `nvngx.dll`, so we give it a module named that. There is
  another way to make that check pass, and a shipping project says it works:
  `SAOG0721/DaVinci-Resolve-DLSS5` (an OpenFX filter running feature 18 over video in DaVinci Resolve)
  lists among its verified contract "scoped `GetModuleFileNameW` IAT compatibility" -- it hooks the
  import the check goes through, scoped to the window in which the NGX call runs, rather than shipping a
  second DLL to be the answer. That is a different mechanism from everything tried above: the theories
  here are all about *why the proxy's CreateFeature fails* (`0xBAD0000B`), while this is about *removing
  the reason the forwarder exists at all*. Worth reading their implementation before assuming it
  generalises -- same-resolution creation, application ID `0x0876232C`, and SEH guards are listed as part
  of the same contract, so the IAT scoping may be load-bearing only together with the rest of it.
  Found 2026-09-11 while surveying the ecosystem; not tried here.

## How to reproduce
Set `[DlssNr] UseProxy=true`. The path is off by default and does not fall back automatically, so a
failure is visible rather than masked by the forwarder quietly doing the work.
