# Residual limit: a flat bound, in stops, on how far the model may move a pixel

Status: implemented in the composition shader, D3D12 and native Vulkan, behind `[DlssNr] ResidualLimit` (default 0,
off) and a slider in the card's "How much of it lands" section; **not yet run in a game**. It needs `dlssnr.hlsl`
recompiled for both targets (`DlssNr_Shader.h`, `DlssNr_Shader_Vk.h`); until then the shader ignores the new constant
and the control does nothing.

## The problem

The model sometimes re-decides a patch of the picture by a lot: a stretch of wall a stop brighter for a frame, a face
darkened and back. On a still camera that reads as blotches and flicker. The composition already bounds the model's
edit with the highlight guard (`MaxRatio`), but the guard is sized and shaped for light sources: two-sided at 2x by
default, with no room to lift near black and none to drop near white. Raising the guard to keep detail in highlights
widens it everywhere; it was never meant as a flicker bound. The AMD fork's "residual limit" names the missing control
(`external-perf-research.md`, item 5); this is our own version of it.

## What it is

After the guard has produced its ratio (`boundedRatio = clamp(amplified, drop, lift)`, `dlssnr.hlsl`), and only when
the limit L is above 0:

    boundedRatio = clamp(boundedRatio, 2^-L, 2^L)

The ratio is the one the composition applies to the pixel's luminance: the luminance-only end of the colour blend is
`original x boundedRatio`, the composed end is corrected to the same luminance (`upgraded *= boundedRatio / lumaRatio`),
and the colour blend between them moves chroma at that luminance. So L is how far, in stops, the model may move any
pixel's light, either way: 0.5 allows at most 1.41x, 1 at most 2x.

Properties, each checked by `tests/nr-residual-limit` on the shader's own text:

- **Off is the composition as it was.** At 0 the branch is not taken; the bound is the guard's, to the bit. Anything
  not above 0 (a negative, a NaN) is off too, in the shader and on the host.
- **Narrower only.** The guard's floor is at most 1 and its ceiling at least 1, and the limit's are 2^-L <= 1 <= 2^L,
  so the clamp only ever moves a ratio toward 1: never further from 1 than the guard left it, never across it, and a
  ratio the guard already left inside the limit passes untouched. It cannot widen the guard's shape near black or
  white.
- **Symmetric in stops.** Brightening and darkening are bounded by the same factor, as the amplification above it is.
- **Strength zero is still the frame, bit for bit.** The resolve returns the untouched frame before reading anything
  the model returned when Detail strength is 0, long before this line (README, "Ratio composition, not a delta").
- **Passthrough is untouched.** It changes nothing in the encode or in the matched residual's rebuilt proxy; the
  constant is read in this one clamp only (invariant 7).

What it does not cover:

- **The replace curves** (`ReversibleMode` 2 and 4) skip the composition and decode the model's answer directly, so they
  skip this as they skip the guard. The control is hidden while one is selected.
- **Colour strength above 1** boosts chroma in OkLab after the bound, and **highlight recovery** blends toward the frame
  after it; the first can move luminance slightly, the second only reduces the edit.
- **It does not tell a blotch from a correction.** A real correction larger than L is cut to L just as a blotch is.
  That is the price of a flat bound, and why it is off by default: there is no value that is right for every game, and
  the right one is found by eye, starting high.
- **Debug view 3** shows the model's raw edit, before the composition, and so before the limit.

## Controls and wiring

- `[DlssNr] ResidualLimit` (float, stops, default 0; 0..2), four-point round trip. The host sends
  `DlssNr::ResidualLimitStops(value)`, which turns anything outside 0..2 or not a number into the nearest legal value or
  off, on both the D3D12 resolve and the Vulkan encode whose constants the Vulkan resolve copies.
- `DlssNrConstants::ResidualLimit` and the cbuffer's `gResidualLimit`, appended last in both (invariant 5;
  `nr-invariants` checks the order).
- Menu: "Residual limit" under "How much of it lands", beside Detail and Colour strength, shown as "off" at 0. Hidden
  under the replace curves and under the D3D12 driver proxy, which returns before the resolve (invariant 2).
- Logged: the D3D12 composition line carries `residual limit off` or `residual limit 0.50 stops`, and a change of it is
  a configuration event (it logs at once and resets the output stabilizer, which compares the same report); the Vulkan
  path, which has no composition line, says it when it changes. A change also starts the pulse metric's window over.

## Why it is in the composition and not in the cadence

It bounds what the model does to a pixel, whatever frame the answer came from: a model frame, a carried one, a smoothed
one, Cadence 1. The composition is the one place every answer passes through and the one place the ratio exists.

## Cost

One branch per pixel when off, a clamp and two `exp2` of a constant when on. No resource, no allocation.

## Tests

`tests/nr-residual-limit` (host) slices the guard and the limit out of `dlssnr.hlsl`, compiles them under ASan/UBSan
against HLSL's intrinsics, and holds the bound against the guard alone over 5 guards x 12 luminances x 97 ratios
(1/16..16): off is the guard to the bit for 0, -0, a negative and NaN; on, every limit from 0.05 to 2 stops bounds both
ways, only narrows, never crosses 1, leaves a ratio inside the limit untouched, stays inside the guard, and is
symmetric. It checks the host's clamp, and pins by source where the constant is read (once, after the guard and the
strength-zero return, before the correction, the colour blend and the replace curves), set (D3D12 and Vulkan), logged
and shown. `nr-log-rate` checks a change of it is a configuration event; `nr-invariants` the round trip and the
struct/cbuffer order.

## In game

After both shader targets are rebuilt:

1. Default (0): the composition log line says `residual limit off`; a capture is byte-identical to one before the
   rebuild at the same settings (Hold frame on).
2. A still view where the model blotches (foliage, a textured wall at low light): raise the limit to 2, then lower it
   in steps; the blotches should fade by about 1 stop and the model's ordinary detail should survive to about 0.25.
   "Measure detail" before and after gives the detail added and the flicker as numbers; the pulse metric's every-frame
   light step should not rise.
3. Detail strength 0 with the limit on: the frame is the game's, bit for bit.
4. Replace curve selected: the slider is gone; native Vulkan (DOOM Eternal): the slider is there and the log says the
   value when it changes.
