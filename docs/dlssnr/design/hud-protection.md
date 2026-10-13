# HUD protection through the model's UI correction

Status: **first slice built, not yet run on a GPU.** Opt-in `[DlssNr] UiProtection`, default off.
Written 2026-09-27 on branch `synth-motion` (`57131f07`).
**2026-09-28: the shipped pass protects nearly every pixel, because of a macro defect in its shader;** see
[The mask macros: every pixel protected](#the-mask-macros-every-pixel-protected-found-2026-09-28-not-fixed).

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

### The first slice left a rim, and the rule was tightened (2026-09-28)

**Seen.** PCSX2, God Hand from a save state, WorkingScale 0.5. With `UiProtection=true` a bright,
dotted golden outline ran round the character's coat against the sand. With it false, everything else
equal, the edges were clean (A/B from the same state, `scratchpad/iso2/grid12.png`). The motion
estimator and the guide resample were ruled out: the rim was as strong with both off.

**Why.**
- The sand beside the swaying coat passed every gate: it was still, the coat moved inside its rings,
  and grains of sand had contrast above 0.08.
- The 1 px growth and the 1.5 s decay then held it.
- A protected pixel shows the raw frame, and the raw frame is brighter than the NR output: mean luma
  99 against 84 in the 09-27 A/B. So the false positives read as a light line.
- The dots were the contrast gate passing only some grains, in a mask built at 418 px and stretched
  to 836.

"A falsely protected pixel shows the game's own pixel" holds, but NR shifts tone globally, so a raw
pixel among NR pixels is a visible seam. False positives have to be rare, not merely harmless.

**The rule now** (`precompile/dlssnr_uimask_rule.h`, shared with the host test `nr-uimask-rule`; since
2026-09-28 it is `shaders/synth_motion/precompile/static_overlay_rule.h`, unchanged, shared with
synthesized frame generation's HUD mask as well):

1. **Still pixel:** `own < StaticEps` (0.008), as before.
2. **Still core:** every pixel within 2 px (the 5x5) also changed less than `CoreEps` (0.012).
   - A glyph's inner edges pass: the fill against its outline, where the whole 5x5 is interface.
   - Scenery hugging a moving silhouette fails, because the silhouette is inside the core.
3. **Moving on every side:** the four axis directions (right, left, down, up) each have a sample at 6,
   14 or 24 px that changed by more than `MotionTau` (0.02), and all four must (`SidesMin` 4).
   - Axis only: from a pixel beside a tall silhouette, a diagonal lands on the silhouette.
   - A scene panning behind an overlay moves on every side. Scenery beside something that moves sees
     it on one side, two at a corner, three in a concave gap.
4. **Detail:** `detail > DetailMin`, raised to 0.15. Glyph edges clear it easily; most sand grains
   don't.
5. **Entry hysteresis:** a pixel must be a candidate for `StreakMin` (8) frames in a row before it is
   protected. An already protected pixel re-arms at once, so a pause in the scene's motion does not
   restart it. Exit is as before: decay 0.985, and dropped at once when its own change exceeds
   `DropTau`.
6. **Export needs support:** the exported mask (last frame's protection grown 1 px) is kept only where
   last frame's 5x5 holds at least `SupportMin` (3) protected pixels. An isolated grain that passed
   every gate is dropped; a glyph's edge, a line of them, is kept.

State per pixel: the accumulation pair became R16G16_FLOAT, with `.x` the protection and `.y` the
candidate streak in whole frames. Still two ping-ponged textures, as before.

**What it costs in recall.**
- Interface is protected only while the scene moves on every side of it, and for about 1.5 s after.
- Large solid HUD shapes are protected at their inner, outlined edges only; their outer edge sees
  motion on one side.
- The model does little to a flat HUD interior anyway.

**Tested on the host** (`tests/nr-uimask-rule`), running the header the shader runs:
- A glyph over a 3 px/frame pan: nothing protected before the streak, then 103 inner-edge pixels, and
  0 outside the glyph plus its growth.
- Grainy sand beside a coat swaying ±3 px: 4872 protected sand pixels under the first slice's
  thresholds, 0 under the new rule.
- Sand between two swaying legs under a swaying coat: 0.
- A glyph that vanishes: every pixel that changed past `DropTau` is unprotected on that frame.

It has not been re-run in PCSX2 yet.

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
- Building the mask at the frame's size, or testing stillness on the full-size source. The mask is at
  the working size and stretched by the resolve; at 50% a protected edge is 2 px wide on screen.

## Verified in PCSX2, 2026-09-28

Build `4fa5912b`, God Hand from the save state, WorkingScale 0.5, synthesized motion on (NR and FG).
- **Before the tightened rule** (`c95ad19c`): with UiProtection on, a bright golden rim outlined the
  coat against the sand. With it off, the edges were clean.
- **After:** the rim is gone. On the coat and sand, UiProtection on and off look alike.
- **Not yet measured:** HUD recall in a moving scene.

## The mask macros: every pixel protected (found 2026-09-28, not fixed)

Found while building synthesized frame generation's copy of this mask (synthesized-frame-generation.md, "The
HUD: near depth and a UI layer"). That copy, run on a real device in `tests/synth-motion-d3d12`, marked
every pixel of a static frame from its second frame on. The cause is in the macros `dlssnr_uimask.hlsl`
hands the rule, and this pass has it too.

- **The defect.** `#define UM_PROT(x, y) AccPrevAt(int2(x, y)).x`. A function-like macro substitutes every
  token that matches a parameter, and the `x` of the swizzle is one. So `UM_PROT(x + sx, y + sy)`, the
  call in the rule's export loop, expands to `AccPrevAt(int2(x + sx, y + sy)).x + sx`. `UM_STREAK` has the
  same shape, but the rule only calls it at the centre, where the argument is `x` or `y` itself and the
  expansion is harmless.
- **In the committed bytecode.** Disassembled `DlssNr_UiMask_Shader.cso` loads last frame's protection and
  adds -2, -1, +1 or +2 to it before the `> 0.5` support test (`fadd fast float %883, -2.0`, then
  `fcmp ogt ..., 0.5`).
- **The effect.** The `sx = +1` and `+2` columns of the 5x5 always count as protected, so support is at
  least 10 and always passes. The 3x3 growth reads at least `prot + 1`, so the exported mask is
  `saturate((0.985 - 0.25) * 2) = 1`.
  - So `UIAlpha` is 1 on every pixel whose own luma did not jump by more than 0.2 since the last frame,
    from the second frame after any reset. That is nearly all of them.
  - With `UiProtection` on, the model is told to leave nearly the whole frame as its input. The measured
    rule (the gates above) never gets a say, and "UiProtection on and off look alike" in PCSX2 cannot be
    taken as evidence about it.
- **Why the tests missed it.** `tests/nr-uimask-rule` includes the same rule, but its macros are plain
  function calls (`Prot(x, y)`) with no swizzle to substitute. It tests the rule, not the macros.
- **The fix,** not applied because this branch had to leave DLSS-NR byte-identical: name the parameters
  anything but `x` and `y`, as frame generation's `synth_overlay_detect.hlsl` does. For example,
  `#define UM_PROT(px, py) AccPrevAt(int2(px, py)).x`, and the same for the other three. Then rebuild
  `DlssNr_UiMask_Shader.h`, and look at UiProtection in PCSX2 again: it will start protecting only what the
  rule finds.
- **Guard.** `tests/fg-synth-policy` refuses the hazard in any shader that includes the rule. It listed this
  file as the one known exception until the fix below.
- **Fixed** on 2026-09-28: all four macros take `px_, py_`, and `DlssNr_UiMask_Shader.cso`/`.h` were rebuilt
  with the command in the shader's header (dxc `cs_6_0 -O3 -Qstrip_debug -Qstrip_reflect`). In the
  disassembly, `fadd` of a ±1/±2 constant fell from 21 to 1, and the exception is gone from the guard.
  Everything this note says UiProtection measured before the fix was measured with the defect.
