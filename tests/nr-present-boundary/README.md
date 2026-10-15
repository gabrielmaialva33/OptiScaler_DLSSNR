# NR present boundary

```bash
python3 tests/nr-present-boundary/run.py
```

The runner first checks the production source, then compiles `retoque/dlssnr/DlssNr_PresentBoundary.h` with g++
C++20, ASan/UBSan and `-Werror` and runs `cases.cpp`.

## What this covers

The interim rule of `docs/dlssnr/design/nr-before-dlssg.md`: under the game's own DLSS-G a frame is edited before
DLSS-G or shown as the game drew it, never every other one.

- **No frame generation:** every `HookMethod` decides as before (1 upscaled, 2 present, Auto present once it has
  temporal inputs and a live present entry).
- **The game's DLSS-G with `HookMethod=2`:** the wrapped entry, which sits below DLSS-G, declines. Either signal is
  enough: a mode other than off passed to Streamline's DLSS-G, or a frame count at DLSS-G's last NGX evaluate. The
  upscaled route still defers and captures, so the present pass resumes the moment DLSS-G stops.
- **The game's DLSS-G with Auto:** upscaled, latched for the session the first time it is seen, so a game that switches
  DLSS-G off in its menus does not move the pass between routes. Upscaled even before the latch is noted.
- **`HookMethod=1`:** untouched.
- **OptiScaler's own frame generation** sets the same two signals but is not the game's: the FG hook's pass runs, the
  wrapped entry is left to its existing stand-down, and Auto does not latch.
- **The boundary** (step 1, built): a pass that runs above DLSS-G is never declined, and Auto is Auto again there.

Source guards: `RunPresentPass` decides through `PresentBoundary::Decide` and handles `Declined`; the upscaled route asks
`UpscaledDefersToPresent` and `UpscaledCapturesForPresent`; the inline Auto conditions they replaced are not back.

Step 0's probe (`DlssNr_SlBoundaryProbe.cpp`) is guarded the same way, by source: it installs nothing unless
`[DlssNr] Enabled` is true with `HookMethod=2`, its `Present` detours and every note it takes from production return at
once when it is not installed (its `Signal`/`Wait` detours forward first and then only count), and each of its five call sites (install, uninstall, the wrapped present, DLSS-G's NGX
evaluate, `ExecuteCommandLists`) is there exactly once. What it logs is a game run's to read.

## What this does not cover

The inputs come from `State` (`dlssgLastSetMode`, `dlssgDetectedInterpolationCount`, `currentFG`) in
`BoundaryInputs` (`DlssNr_Dx12.cpp`); whether those reflect the game's DLSS-G in a given title is a game run's
question, as is the picture. With `HookMethod=2` set before startup, the boundary should run before DLSS-G;
a live switch without the installed boundary should instead report the declined reason. Numeric Auto (0) stays
upscaled after seeing the game's DLSS-G unless an installed boundary is live.

## Quiet upscaler at the app boundary

The runner also extracts and executes the production no-new-capture admission predicate. It checks
an empty history, a newly captured frame, 1,000 presents after the same capture was consumed/settled,
and a resumed capture, for both entry types and both RequireDlss values. Only the app's own boundary
with optional guides continues through the fallback during the quiet period. Existing guide-identity
suite coverage proves consumed captures are refused; this admission test does not run D3D12 or NGX.
See `docs/dlssnr/design/hellraiser-video-present.md` for the game experiment and unverified limits.
