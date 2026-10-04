# Model cadence — run the model every Nth frame and carry its edit in between

Status: implemented on D3D12, **off by default**, **not yet measured in this fork**. Native Vulkan has
no implementation and the menu does not offer it there. The carry is a subset port of BeliyG3's
`optimizer-fps-dlss5` (MIT, `Licenses/OptimizerFps_LICENSE.txt`); what was taken and what was left is
listed below.

## What it is

An FPS option. With `[DlssNr] Cadence=N` (2..4) the model runs on one frame in N. On the frames in
between — *carried* frames — the edit the model made on its last frame is moved along the game's
motion vectors onto the current frame, and the existing resolve composes it exactly as it composes a
model answer. The model call and the extra passes are skipped on a carried frame; everything before
them (encode, the working-size proxy, guide matching, the UI mask) and everything after (resolve,
stabilizer, capture) runs as it always does.

It is **not** an accumulator and makes no stability claim. It trades image stability in motion for
the cost of N−1 model calls out of N. The saving is roughly `(N−1)/N × model time − carry cost`, and
the reduced working scale already shrinks the model time, so it is smaller here than BeliyG3's
synthetic bench (53 → 86 fps at N=2, 127 at N=4, model 13–15 ms at 4K).

## How it relates to the README's dead end — candidly

`dlssnr/README.md` records that temporal filtering of the model's answer was measured twice and
abandoned: the model re-decides detail with the framing, so old answers do not belong to new frames
(`e57e389e`, `ff6c84cb`).

What is genuinely different:

- **The goal.** The dead-end filters ran the model every frame and blended its answers for
  steadiness. Cadence runs it less often, for speed, and claims nothing about steadiness.
- **No blending of answers.** Every model frame is exactly the model's answer. Two model answers are
  never mixed in one pixel: the carried edit is always the *last* answer, moved, never a blend of
  the last two.
- **Still camera.** With nothing moving, a carried frame is the model frame's answer exactly (to
  half-float rounding of the stored residual). That shows the model's own frame-to-frame churn at
  1/N the rate, which is nearer to the output stabilizer than to the dead end.
- **The resolve still receives a complete answer picture**, in the proxy space, so the README's
  "ratio composition, not a delta" rule is respected: what is carried is `answer − proxy` at the
  model's working size, and on a carried frame the resolve is handed `proxy_now + carried edit` as
  if the model had returned it.

What still applies, in full, to every carried frame **in motion**:

1. **Pops at the cadence rate.** A carried frame is an old answer on a new frame. At each model frame
   the picture switches from "frame K's answer, moved" to "frame K+N's answer". BeliyG3 saw teeth
   snapping and "the whole frame pulsed every N frames"; neural-upstream concluded that "the residual
   flicker at cadence > 1 is the cost of reusing an effect across frames at all".
2. **Ghosting** where things are uncovered is reduced by the depth and colour tests and the fill, not
   removed. BeliyG3's own acceptance report measured 3.6 to 7.9 levels (of 255) of mean error
   against every-frame NR at N=4, with more machinery than is ported here.
3. **BeliyG3's fixes for the pops are the dead end.** Its cross-pass blend and phase-in mix old and
   new answers — exactly the class the README says under-stabilises detail and ghosts. They are not
   ported (see "Refused" below).
4. **Model brightness lag** across skipped frames: BeliyG3 measured the model 0.3–0.6 % darker at
   N=2 and up to 2.7 % at N=4 after a cut, "regardless of which vectors it is given". The README
   does not cover this cost; nobody has documented feature 18's behaviour across gaps.

So the honest description is the one the menu uses: it costs stability in motion, frame times
alternate long and short, and it is off by default.

## The carry

All at the model's working size W×H, in the encoded proxy space the model reads.

**Recorded on a model frame**, only when the model call and every requested extra pass succeeded,
right after the final answer becomes readable and before the resolve:

| Surface | Format | What |
|---|---|---|
| residual | RGBA16F | final answer − proxy (`modelInput`) |
| proxy snapshot | RGBA16F | the proxy the model saw, for the colour test |
| depth snapshot | R32F | `depthForModel` resampled to W×H, for the depth test |
| coarse residual | RGBA16F, ⌈W/16⌉×⌈H/16⌉ | the residual averaged over 16×16 blocks, for the fill |
| chain ×2 | R32G32F | displacement back to the residual's frame, work pixels |

32-bit chain because vectors in pixels reach the thousands, where 16-bit float has lost the
sub-pixel (the guide-match comment). About 36 bytes per working pixel: ~133 MB at a 2560×1440
working size, ~300 MB at 4K. Nothing when off.

**On a carried frame:**

1. *Chain step* (`dlssnr_cadence_rule.h`, `CadenceChainStep`): `acc = mv + accPrev(p + mv)`, read
   bilinearly, mv converted to work pixels (`game scale × work / the size the vectors are measured
   against`, the same reference `DlssNr_GuideMatch.h` uses). The first carried frame after a model
   frame starts from `mv` alone. Each step is validated: if the end of the chain shows another
   surface in the residual's frame (depth mismatch over twice the tolerance, or colour gate at or
   under 0.05), the chain restarts from this frame's own vector and the reprojection rejects it
   honestly.
2. *Synthesis* (`CadenceSynthesize`): `out = proxy_now + (1 − ui)·(accept·R(q) + (1 − accept)·fill)`,
   clamped to `[0, max(1, proxy_now)]` — saturate, except that a proxy already above 1 is not
   clipped below itself. q = p + acc.
   - **Acceptance** averages a depth test and a colour test over 9 taps 3 px apart (scaled with the
     working size), gated by the centre pixel's own result, and falls to zero where q leaves the
     picture.
   - **R(q)** is a 16-tap Catmull-Rom of the residual, clamped to the four texels around q (no
     ringing).
   - **Fill** is the coarse residual at q scaled by the luma ratio now/then (0.25..4); where
     acceptance is under 0.5, four rings of eight taps at 24/48/96/192 px (scaled), rotated per pixel,
     weighted by depth match and a looser colour gate. **No match, no fill**: if nothing around shows
     this pixel's surface, the fill is zero and the pixel shows the game's own frame until the model
     looks again. The fill keeps only its luma and applies it along the pixel's own chroma.
   - **UI protection** on the present route: the carried edit is multiplied by `1 − uiAlpha_now`, so a
     HUD pixel never receives scenery's carried edit.
   - The synthesized answer is written into `g_nr.output`, the surface the model writes, in its
     format, and the resolve runs unchanged — every transfer mode, working scale above and below 1,
     the reversible modes, compare and debug views.

**On the next model frame after carried frames**, the model is handed the summed chain as its motion
vectors (`CadenceModelMotion`): an R32G32F texture at the working size, origin 0, scale 1.0 — the shape
the guide match already hands it. Where the chain does not end on this pixel's own surface, the vector
points off the picture (4×W to the right) so the model treats the pixel as newly uncovered. BeliyG3's
reason: otherwise the model's own history is "misaligned by age−1 frames on every pass". A model frame
that was *forced* (a reset, a change, a gap) gets the game's vectors as always.

**Held reset.** A pending reset forces a model frame, so it is never lost on a carried frame; a carried
frame does not clear `g_nr.reset`. neural-upstream lost a reset that landed on a skipped frame and broke
the image after alt-tab; here it cannot land on one.

## Scheduling

By **age since the last model frame**, never by parity. Age is the number of distinct presents at which
the pass ran since the last model frame. Not the raw present delta: `State::frameCount` advances on every
wrapped-swapchain present, generated frames included (`wrapped_swapchain.cpp`, `_frameCounter++`), so with
frame generation the delta would count each rendered frame two or more times and run the model every
frame. A dropped call shifts the pattern by one frame; it cannot flip it.

A model frame is forced, and the stored residual dropped, on:

- any pending reset (the game's, a tuning pulse, a hold release, a route switch, a format change) or a
  pending extra-pass reset;
- a feature rebuild (a feature generation counter);
- a change of frame or working size, colour format, route (stage, after-RR, present), or anything the
  model reads: preset, style, intensity, structure, tone, skin, auto mask, the pass count and per-pass
  settings, the colour transform (passthrough), the reversible proxy, the supersampling filter, UI
  protection, guide matching, the motion-scale rule, the game's depth orientation and vector scale;
- a gap over 250 ms since the pass last ran;
- a second call in one present (a title that runs the upscaler twice per present: carrying one view's
  edit onto the other would be wrong, so the cadence stands down for that call and the next).

Composition sliders (detail, colour, guard, paper white, transfer, debug, compare) do not force one: the
residual is taken before composition and the resolve reruns every frame.

## Refused — the model runs every frame, with a reason

Each refusal is logged once on change (`DLSS-NR cadence: ...`) and shown under the control.

| Refusal | Why |
|---|---|
| before-upscale stage | the input is jittered and the upscaler would accumulate the misplacement — the stabilizer stands down there for the same reason |
| no game depth and motion | zero guides (Divinity, Witcher 3 DX11) or synthesized motion (zero depth, 8×8 blocks, the NVIDIA source a frame late): the depth test does nothing and in-place reuse ghosts whenever the camera moves. Present routes carry only with captured game guides (`CapturedPresentGuides`) |
| native Vulkan | no implementation; the control is hidden (invariant 2). Six more images would also fall under invariant 8. A later port needs a SPIR-V build, its own layout and drain/abandon lifetime |
| driver proxy (`UseProxy`) | it returns before the resolve; the control is hidden as the stabilizer's is |
| frame hold, capture | both compare frames; a carried frame would compare a moved answer with itself |
| frame generation | frame times alternate long and short. neural-upstream measured DLSS-G unable to pace through 8.9/13.9 ms alternation, with stutter and flashes growing with the multiplier and the cadence; the same work with the effect at zero showed the same artefact, so it is pacing, not image. `CadenceWithFrameGen=true` opts in. Detected as OptiScaler's own FG active and unpaused, the game's DLSS-G through NGX (`dlssgDetectedInterpolationCount`), or a DLSS-G mode set through Streamline |
| no cadence shader | the bytecode header is optional (`__has_include`); without it the pass reports itself unavailable |
| surfaces could not be built | allocation failed |

## Not ported, on purpose

- BeliyG3's **cross-pass blend** (`PSResidual`'s blend) and **phase-in** (`PSResidualOld`/`PSApply`):
  they blend model answers, the README's dead-end class. Only behind a separate experiment measured
  against the README's claim, if ever.
- The **Lucas–Kanade refinement** (`PSRefine`), until the basic carry has been measured.
- The **background-queue mode**: both authors say a synchronisation bug there hangs the GPU, and it
  needs a host queue OptiScaler does not register.
- The **expected depth** along the chain (`PSExpect`), the **older passes**, the **cells** and the
  **edge-aware compose**, the **silhouette** strictness. This is a subset port; the quality gap to
  BeliyG3's 15-pass machine is unknown.

## What is not BeliyG3's

- The edge ramp only applies when the chain moved q nearer the edge than p is, so a still pixel by the
  border is not attenuated (BeliyG3's ramp attenuated every pixel within 16 px of the frame edge).
- Taps, rings and the edge ramp are scaled by working size / frame size, since BeliyG3's numbers are
  native 4K pixels and the carry runs at the model's working size.
- The depth snapshot is point-resampled to the working size (the rule the guide match uses), and the
  best-of-four depth test reads the four snapshot texels around q at the guide's texel spacing.
- The coarse residual is a plain 16×16 box average rather than 16 bilinear taps.
- Scheduling, refusals, the forced-frame rules and the timing kind are this fork's.

## Measurement plan

Before anyone calls it a win:

1. `tests/nr-cadence` (host): the scheduler (age-based, reset held, forced frames, every refusal and its
   reason, off allocates nothing) and the rule header over synthetic sequences (still scene exact,
   integer and sub-pixel pans within tolerance, an occluder restarting the chain, no match → no fill).
   It says nothing about the GPU, the model or image quality.
2. GPU timing: carried samples carry `carried=1` and their cadence in the confirmed line; model and
   carried frames are separate samples. With an interval that is a multiple of N every sample is the
   same kind — pick an interval prime to N to see both.
3. Wine tier, by hand: `dlssnr-loopback` with cadence 2 should confirm the real model accepts skipped
   frames, the held reset and the chain vectors (not yet wired).
4. In game:
   - Crimson Desert: MangoHud frame-time graph at N=1, 2 and 3, FG off and on (opted in); a slow pan,
     a fast turn, a character crossing the frame. Look for the cadence-rate pop, ghosting around
     uncovered areas, and brightness pumping.
   - DOOM Eternal (native Vulkan): the control is absent. Divinity (zero guides): the refusal shows.
   - A run on Rafael's native-Windows PC before calling it done: Proton is not a D3D12 validator, and
     this pass adds descriptors, a 256-byte constant buffer view and typed UAV stores into the model's
     output format.

## Attribution

The depth mismatch, the colour gate, the validated chain step, the 9-tap acceptance, the ring fill with
its "no match, no fill" rule, the fill's luma ratio and chroma transfer, and the off-picture vectors for
the model are **taken from BeliyG3's optimizer-fps-dlss5** (`core/shaders/temporal_mapping.hlsli`,
`temporal_reproject.hlsli`, `temporal_residual.hlsli`, commit `3636e6d8`, MIT, Copyright (c) 2026 Yuri
Grib). The held reset is neural-upstream's lesson (matiasLombo, MIT); no neural-upstream code is ported.
