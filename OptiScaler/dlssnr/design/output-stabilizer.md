# Output stabilizer — hold still pixels still, follow the model where the game moved

Status: implemented on D3D12, off by default, **not yet measured in this fork**. Native Vulkan has no
implementation and the menu does not offer it there. Ported from DLSS5-Feeder's `feed_hold12.h`
(v1.17, MIT, Jean-Laurent Rouzies — `Licenses/DLSS5-Feeder_LICENSE.txt`), with the changes listed
under "What is not Feeder's".

## The problem

On a still camera the model keeps re-deciding what a region should look like from frame-to-frame
differences too small to matter to the game: a slope under a tree brightens while the camera pans and
darkens over the frames after it stops, although the game drew it the same way every frame. Feeder
measured that it is not the model's history catching up (nine evaluates a frame changed nothing) and
not the history at all (a reset every frame still flickers and still darkens). So the fix cannot be on
the input side, and it has to tell model drift from a real change.

That measurement is Feeder's, on its own pipeline. This fork has not reproduced it; the reason to take
the pass is that the argument does not depend on the pipeline — only on the model — and the pass is
cheap, gated and default-off.

## Why this is not the dead end in the README

`dlssnr/README.md` records that temporal filtering of the model's answer was measured twice and
abandoned: the model re-decides detail with the framing, so old answers do not belong to new frames.
That was filtering *across motion* — an old answer reprojected or blended onto content that moved.

This is gated by the **game's input**, not by the model's output, and nothing is reprojected. A pixel
whose input changed shows the current model answer that frame, exactly; only pixels whose input did
not change keep what was shown. The README's objection is about old answers on new content, and here
there is no new content on a held pixel by construction.

## The pass

Per output pixel:

1. `a` = a 3x3 box of the **witness**, so one shimmering texel is not "change". The witness is the
   frame as the upscaler left it (`hdrCopy`), divided by the white point the composition used and
   encoded with sRGB's curve continued past 1. See "The witness" below for why it is not the proxy.
2. `b` = the **anchor**: the same box as it was when this pixel last moved.
3. `rel` = the largest channel difference `|a - b|` over the local brightness, so a global exposure
   drift that moves dark pixels by tiny absolute amounts does not unlock the whole frame.
4. `change` = 0 below the tolerance, 1 at twice it, soft in between.
5. Shown = the model's answer where it changed; last frame's shown picture where it did not, with the
   model's new opinion creeping in at `1 - strength` per frame. The anchor follows at `change`.

Why an anchor and not last frame's input: a slow change (clouds, a light fading) moves the input by
less than the tolerance every frame, so against last frame it would never count and the pixel would sit
on a stale picture until something finally crossed the line, then jump. Against the anchor the drift
accumulates, the gate opens as it nears the tolerance, and the anchor follows at the same rate.

## The witness

The first version of this port gated on the proxy the model is shown (`colorCopy`), on the argument
that when what the model saw did not change, a different answer is the model's drift. The argument is
half right, and an adversarial review found the half that is wrong. The composed picture depends on
two things from the frame: the model's answer, which depends on the proxy, and the frame itself, which
the resolve reads linearly for everything the proxy cannot carry — above all the highlights. The
proxy's soft knee reaches its asymptote within a stop of white, so stops of real change in a muzzle
flash, a fire or a specular read as about 0.001 there, far under any tolerance: at strength 1 they
froze on a still pixel.

So the witness is the linear frame, which moves whenever either input moves:

- divided by the white point, because that is what the composition divides by, and a white point
  that moves (the meter, the game's exposure) changes the output and should read as change;
- encoded with sRGB's curve continued past 1 rather than kneed, so nothing above white saturates, and
  the tolerance keeps the meaning it was tuned with on Feeder's encoded frames — a relative difference
  in linear light would be about 2.2 times as sensitive for the same number;
- left alone when the frame is already a finished, encoded picture (passthrough).

`HoldFrame` still locks the whole picture: the held frame is what the encode and its untouched copy
both read, and the white point is snapshotted while held.

## What is not Feeder's

- **The witness** above: Feeder's input is already an encoded, finished frame; here it is open-ended
  linear light and has to be brought into the space the tolerance means something in.
- **Quantisation-aware creep.** Feeder keeps its history in the output's own format and creeps with a
  plain `lerp`. A store rounds to the format, so a step smaller than half a unit of the format is lost
  and the pixel never reaches the model's value: with an 8-bit output at strength 0.9 every difference
  under 5 LSB is stuck forever, at 0.95 under 10 LSB (4%). A float output is worse — R11G11B10's 6-bit
  mantissa sticks differences up to ~8% relative at 0.9. The shader here moves at least one unit of
  the output format per frame toward the model's value (never past it), which makes the creep converge
  on every format without spending memory on a wider history. At strength 1 it does not creep at all:
  1 means hold, as in Feeder.
- **Rec.709 luminance weights** for the brightness denominator, the primaries these frames are in,
  where Feeder used Rec.601's. It moves the denominator only.
- **Routes.** It runs after the resolve wherever the pass edits the frame in place — after the
  upscaler, after Ray Reconstruction, at present, and in the D3D11 bridge's present host. It stands
  down on the **before-upscale** stage: there the input is the jittered render-resolution frame about
  to go into the upscaler, where TAA jitter reads as change on every pixel, and a held pixel would
  feed the upscaler a stale jittered sample and damage its own accumulation.
- **Resets that mean something here.** Feeder resets on a camera cut. This pass also resets when
  anything that changes the composed picture changes — composition sliders, compare mode and split,
  debug view, the model's settings, the supersampling filter, working size, route — because otherwise,
  at strength 1, moving a
  slider on a still scene would appear to do nothing. It does **not** reset on the zero-guide reset
  policy (`ZeroGuideReset`), which drops the model's history every frame for its own reasons; a still
  picture on zero guides is exactly where a stabilizer earns its keep.
- **Lifetime.** Feeder frees its history on a resize and relies on its callers to have drained. This
  fork never frees under the GPU: a stabilizer built for another size, format or device — or switched
  off, or on the before-upscale stage — is parked with the other retired objects and deleted once the
  recordings that used it are done. An output with mips, slices or samples is refused once, with a
  reason in the log, because the copy back would cover subresources the history does not have.
- **State.** Feeder's history is `SIMULTANEOUS_ACCESS`, relying on promotion and decay at each
  submission, which is only right while there is one dispatch per submission. Here the history rests
  in `NON_PIXEL_SHADER_RESOURCE` with explicit barriers around every use.

## Guards (per DEVELOPMENT.md)

- Default-identical: at strength 0 nothing is allocated and nothing is dispatched.
- No dead control: hidden on native Vulkan and under the driver proxy, which returns before the
  resolve; its help says it stands down before the upscaler.
- Config round-trip: `StabilizerStrength`, `StabilizerTolerance`, all four points.
- The composition log line carries both values, so a screenshot taken with it on can be told apart
  from a bug.
- Its precompiled header is checked against its `.cso` by `tests/nr-invariants`.

## Cost

One full-resolution compute dispatch (nine bilinear taps, three loads) and one full-resolution copy.
Memory: two history textures in the output's format and two RGBA16F anchors — about 200 MB at 4K,
50 MB at 1080p. Nothing when off.

## How to evaluate

A still camera on foliage or a textured slope, with the pass on. Compare strength 0, 0.5 and 1, then
pan and stop. Things to look for, because they are the ways it can fail:

- A slow change (sky, a fading light) advancing in visible steps instead of smoothly.
- On the present route the HUD is in the frame: a HUD element whose change is small against its 3x3
  neighbourhood could be held a few frames.
- At strength 1 a still pixel never takes the model's new opinion. That is the definition, not a bug,
  but it means a still region keeps whatever it looked like when it stopped.

It cannot help content that is itself moving; those pixels show the model's answer as it is.
