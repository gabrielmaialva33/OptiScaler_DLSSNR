# Transfer 3: matched residual with a sharp enlargement

Status: **implemented, opt-in, not yet measured in a game.** `Transfer=3`. The default stays 1.

**Why 3 and not 2:** upstream's open multi-pass PR (optiscaler/OptiScaler#1158) already gives
`[DlssNr] Transfer=2` another meaning, "private DLSS SR matched residual". Taking 2 here would make
the same ini line mean two things in two builds. That mode has since been ported with the same
number: see [dlss-enlargement.md](dlss-enlargement.md).

## Where this came from

A draft (Gemini, 2026-09-27) proposed a third enlargement mode, "Native + edit". It laid the model's
difference directly onto the frame, `original + edit`, to keep "100% native sharpness on text and
edges" when the model runs below the frame's size. The goal is right. Three things about how it got
there were not:

1. **Matched residual already keeps the frame at native sharpness.** Transfer 1 rebuilds the frame's
   own proxy at full resolution (`SoftKnee` is a pure function) and carries only the model's
   *difference* up from the small raster. Nothing of the frame passes through an enlargement.
2. **In midtones, the draft is Transfer 1.** Where the knee is the identity, `fullProxy` is the frame.
   `proxyLuma` is then `originalLuma`, the ratio collapses to 1, and the composition returns
   `fullProxy + edit`, which is `original + edit`. The draft only differed where it was worse:
   - Above the knee, and in every `ReversibleMode` from 1 to 4, it added a difference measured in
     the proxy's compressed space to linear light, so its magnitude was wrong.
   - Its guard scaled luminance down by one scalar, so nothing bounded chroma. This is the "adding a
     difference is what let colour run away" that the shader's composition comment records as the
     reason additive composition was removed.
3. Its bytecode could not be reproduced from its HLSL (invariant 6). The draft also computed a
   `gainSharp` it never used.

## What actually softens at reduced scale

The model's edit itself. Both Classic and Transfer 1 enlarge `model - proxy` with the sampler's
bilinear filter. Bilinear reconstruction attenuates everything near the small raster's Nyquist
limit, and that band is where the model's synthesised fine structure lives. That is the "fine
structure it synthesises does not [survive], and softens" that the Model resolution help text
already concedes.

## What Transfer 3 does

Transfer 1, with one change: the edit is enlarged with **Catmull-Rom** instead of bilinear.

- **Sixteen texels.** They are loaded from the model's raster and from the proxy's, each decoded on
  its own, so the difference is taken in linear light before it is interpolated. The bilinear path
  interpolates encoded values and decodes afterwards.
- **Clamped per channel to the box of the four nearest texels' edits.** Catmull-Rom overshoots at
  steps, and a difference that overshoots becomes a halo; the same guard is standard for bicubic
  history sampling in temporal AA. It is a box, not the four edits' convex hull. The weights sum to
  one, so inside the box the result is a blend, and only overshoot is cut. But the cut is per channel,
  so at a coloured edge it can land on a channel mix none of the four texels had. Bilinear cannot.
  Worth watching for in the evaluation below.
- **Then exactly Transfer 1:** `CubeScaleResidual(fullProxy, fullProxy + edit)` and the same
  ratio composition. The colour safety of the composition is untouched.

Taken only when the model ran below the frame (`modelRanSmall`), as Transfer 1 is. At 100%, 1 and 3
are the same code path, bit for bit.

The Difference debug view shows the edit that is actually applied: the sharp one under Transfer 3 at
a reduced size. Otherwise that view would show the two modes as identical.

**Cost:** on the reduced path only, 32 texel loads and 32 sRGB decodes per output pixel. They come on
top of the two bilinear samples the resolve still takes; they do not replace them. At 50%, each
small texel is loaded about sixteen times over. See the measurement below.

## Measured (loopback, 2026-09-27)

`python3 tests/dlssnr-loopback/run.py --present-nr --transfer-ab` runs production's pipeline around a
real model, at 640×360 under a 1280×720 frame:

- **Pipeline:** encode at full size, then production's downsample, the model, and the resolve back
  at full size.
- **Hardware:** RTX 4090, clocks locked at 2100 / 10501 MHz.
- **Frames:** 24 per trial, the same model history every trial.
- **Inputs:** eight-bit passthrough and linear.
- **Bytecode:** the whole run was repeated with the previous bytecode for comparison.

Results:

- **The reduced default path is unchanged.** Transfer 0 and 1 are byte-identical between the previous
  bytecode and this one, in both inputs, at frames 0, 1 and 23. That is the path RTX 20 and 30 cards
  now take by default (`WorkingScale=auto` is 0.5 there).
- **Transfer 3 changes the picture, a little and safely.**
  - Against Transfer 1: MAE 0.10 / 255, 7–9% of channels differ, maximum 4–5 in passthrough and
    31–35 in linear.
  - No pixel went black that was not black before.
  - The largest edit is the same as Transfer 1's, so the clamp holds.
- **Finer edit, modestly.** The applied edit's high-pass energy (after − before, minus its 3×3 mean)
  rises from 0.18 to 0.22 in passthrough (+20%) and from 1.28 to 1.31 in linear (+2%). The fixture is
  synthetic brick, which gives the model little fine structure to draw. Game content is the real test.
- **Cost.** Median of encode + downsample + resolve:
  - Transfer 3: 0.051 → 0.071 ms in passthrough and 0.061 → 0.106 ms in linear, at 0.92 Mpx on a 4090.
  - Default path: 0.048 → 0.050 ms, within noise.
  - Scaled by area and by a 3060 being three to four times slower, Transfer 3 comes to roughly 0.5 ms
    at 1440p and 1.5 ms at 4K in linear on a 3060, against a model that costs 17 ms and more there.

## Invariants

- **Default-identical.** Transfer stays 1 by default. Before this change a 3 ran Classic, because the
  shader tested `gTransfer == 1`. Neither the menu nor the ini offered it, so only a hand-edited ini
  could hold one. Any value the shader does not know still runs Classic, and the menu shows it as
  Classic.
- **No dead control.** The Enlargement combo already goes grey at 100% and above, where 1 and 3 are
  identical.
- **Passthrough.** The per-texel decode is gated on `gPassthrough` exactly as the resolve's own
  decode is.
- **Both shader targets** are rebuilt with the flags `DEVELOPMENT.md` §1.6 names, and the headers are
  regenerated from them.

## How to evaluate

At Model resolution 50%, compare Enlargement "Matched residual" against "Matched residual, sharp"
with the compare wipe on a still scene with fine texture (foliage, fabric, stone). Expected: finer
synthesised detail, and no new halos at strong edges. If halos show, the clamp is not holding and
this needs to be revisited before it can be recommended.
