# HUD protection through the model's UI correction

Status: **first slice built, not yet run on a GPU.** Opt-in `[DlssNr] UiProtection`, default off.
Written 2026-09-27 on branch `synth-motion` (`57131f07`).

## The problem

On the present routes, the model is handed the finished frame. That covers the D3D12 present pass
(emulators, D3D12 titles on `HookMethod=2`) and the D3D11 bridge host. The finished frame has the
interface in it, and the model edits the interface along with the scene.
[ui-correction.md](../../../tests/dlssnr-loopback/ui-correction.md) measured it on the HUD fixture:
bright glyphs and fine lines change at their edges and in their brightness (text panel MAE 2.78,
maximum channel change 84).

The after-upscale route does not have this problem: it runs before the game draws its interface.

## What the contract offers

kibblerz probed feature 18 of the same 310.8 package directly
([PRIVATE-CONTRACT-FINDINGS.md](https://github.com/kibblerz/DLSS5-Reshade-AIO/blob/main/lab/PRIVATE-CONTRACT-FINDINGS.md),
"UI, UIAlpha, and Backbuffer"):

- `DLSSNR.UICorrection=0` makes `UI`, `UIAlpha` and `Backbuffer` output-inert.
- With correction on, the alpha of `DLSSNR.UI`, or the separate `DLSSNR.UIAlpha`, is a per-pixel
  protection mask. 0 leaves NR on, 1 bypasses it, and a spatial mask bypasses it only where marked.
- `DLSSNR.Backbuffer` RGB supplies the pixels restored in protected regions. Its alpha is inert.
- The original input as `Backbuffer` plus a rectangular `UIAlpha` restored the original pixels in the
  rectangle and kept NR outside it.

## What we already knew, and how it fits

- **Our UICorrection null result is consistent with this.** ui-correction.md found 0 and 1
  byte-identical, but every trial had `UI`, `UIAlpha` and `Backbuffer` cleared. Without layers, per
  the contract, there is nothing to correct. That note already said separate layers were untested.
- **Production is one input short.** The model is created with `UICorrection=1`
  (`DlssNr_Dx12.cpp`, the create call). On a present source, `Backbuffer` is already the model's own
  input at the working size (`SetExtras(... frame.PresentSource ? modelInput ...)`). The only thing
  never supplied is `UIAlpha`, and a null alpha means "NR everywhere".
- **An accident showed the model reads `Backbuffer`.** On 2026-09-24, Generation Zero and Divinity at
  `WorkingScale=0.5` on a 3060 were handed a full-size `Backbuffer` while the model worked at
  960x540. The top-left quarter of the screen, HUD included, showed up magnified 2x and blended over
  the picture. That happened with `UIAlpha` null. One reading is that the model also composites from
  `Backbuffer` wherever it differs from `Color`: the Streamline design gives it a scene-only `Color`
  and the finished `Backbuffer`. On a present source the two are the same image, so that mechanism
  cannot find the HUD, and an explicit alpha is needed. This is a hypothesis; the harness mode
  below does not test it yet.
- **The ControlMask disagreement is not settled.** kibblerz reports that `DLSSNR.ControlMask`
  overrides the automatic mask. control-mask.md found no effect in 13 trials, with the mask written
  through the same `unsigned long long` slot the forwarder uses for `Color`, which works. Both used
  310.8. This slice does not depend on ControlMask. `--mask-ab` stays available to re-ask.

## Where a UI alpha can come from

| Route | Source | Cost | Quality |
|---|---|---|---|
| (a) after-upscale | not needed: NR runs before the HUD is drawn | – | – |
| (b) present, no UI buffer (emulators, bridge) | **static-overlay mask** synthesized from consecutive frames | one small compute pass | detects HUD only once the scene around it has moved; misses a HUD over a still scene |
| (c) present, hudless available (DLSS-G / FSR-FG titles that tag a hudless or UI buffer, Hudfix) | `abs(final - hudless)` thresholded, or the UI buffer's alpha | a diff pass | exact where the title supplies it |

**First slice: (b).** It is the route that has the problem and has nothing else to offer.
(c) is rare on the present routes: a title with a hudless buffer usually also has an upscaler and
runs NR after it.

## The static-overlay mask

The rule is afmf-linux's: **a pixel that stays the same while the pixels around it change is an
overlay.** It is computed on the model's input at the working size, in its own pass
(`precompile/dlssnr_uimask.hlsl`, class `DlssNr_UiMask_Dx12`). Per pixel:

1. **Is it still, and is its surround changing?** Take the luma of the model input and compare it with
   last frame's luma at the same pixel: `d = abs(cur - prev)`. Compare the same way at 16 points on
   two rings (6 and 14 px) and average: `ring`.
2. **Detail gate.** The pixel must have local contrast `detail` (the largest luma step to its four
   neighbours). HUD glyphs and lines are hard-edged; a flat sky that happens to sit still next to
   moving foliage must not be taken for interface.
3. **Candidate** = `d < StaticEps` and `ring > MotionTau` and `detail > DetailMin`.
4. **Hysteresis.** `acc = max(candidate, accPrev * Decay)`, with Decay 0.985, about a second and a
   half at 60 fps. The HUD is detected while the camera moves and stays protected for a while after
   it stops. A pixel whose own value jumps by more than `DropTau` (the HUD went away, a menu closed)
   loses its protection at once instead of decaying.
5. **Output.** `mask = max(acc, max3x3(accPrev) * Decay)`. The 3x3 comes from last frame's
   accumulation, so the protection grows one pixel over glyph edges without feeding back into itself
   and creeping frame after frame. It is written as RGBA8 with the same value in all four channels,
   so whichever channel the model reads, it reads the mask.

Luma and accumulation live in two ping-ponged R16_FLOAT pairs, so a pixel never reads a neighbour
another thread is writing. Everything rests in NON_PIXEL_SHADER_RESOURCE between dispatches, like the
stabilizer.

**The failure mode is known and cheap.** A falsely protected pixel shows the game's own pixel there,
the same as NR off, not a corrupted one. A HUD pixel that is never detected (static scene, HUD never
seen against motion) gets NR, as today.

## Wiring

In the shared evaluate (`DlssNr_Dx12.cpp`), which both present routes reach with
`frame.PresentSource`:
- `UiProtection` on: the mask pass runs on `modelInput` at the working size, and `SetExtras` hands the
  mask as both `UI` and `UIAlpha`, with `Backbuffer = modelInput` as today.
- Off, or not a present source: exactly the call made today.

`UICorrection` stays 1 at create, as it already is, so switching the key needs no model rebuild.

- In the protected pixels the model returns `Backbuffer`, which is its own input. The edit
  `model - proxy` is then zero, and the matched-residual resolve returns the full-resolution frame
  untouched: the HUD stays crisp even when the model runs at 50%.
- The mask object follows the stabilizer's lifetime: parked on a size change, on switch-off and at
  shutdown, never freed under the GPU.
- Vulkan is not wired in this slice. The Vulkan evaluate has no extras export yet.

## How it is measured

`python3 tests/dlssnr-loopback/run.py --present-nr --ui-protect-ab` runs the HUD fixture with an
oracle mask (the fixture's own HUD cover). It tests the contract, not the detector:

| trial | UICorrection | UI / UIAlpha | Backbuffer | expected |
|---|---|---|---|---|
| base | 1 | null | null | today's after-upscale call: NR over the HUD |
| bb-only | 1 | null | model input | today's present call: same as base |
| alpha-hud | 1 | UIAlpha = HUD cover | model input | HUD region MAE vs source ≈ 0; the rest as base |
| ui-a-hud | 1 | UI (alpha = cover) | model input | the same through `UI` |
| alpha-hud-uic0 | 0 | UIAlpha = HUD cover | model input | as base (gated off) |
| alpha-hud-nobb | 1 | UIAlpha = HUD cover | null | shows where restored pixels come from without `Backbuffer` |

The detector itself needs a moving scene, which the fixture does not have. It is judged in PCSX2 from
the save state (`~/Games/pcsx2-nr-test/run-state.sh`): pan the camera with the key on and off, and
capture the HUD.

## Open

- (c), the hudless-diff mask, for titles that tag a hudless or UI buffer on a present route.
- A debug view of the mask. It needs a slot in the NR shader's debug views, which means both shader
  targets rebuilt, so it is left for the next slice.
- Vulkan.
- The ControlMask disagreement with kibblerz's findings.
