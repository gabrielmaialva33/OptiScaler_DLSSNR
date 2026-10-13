# Cadence smoothing: the spread and the light limiter

Status: implemented on D3D12 behind `[DlssNr] CadenceSmoothing` (default **off** until measured; it acts only when
`Cadence` is above 1, and the cadence itself is off by default). **Not yet run in a game.** The cadence shader was
rebuilt with its four new modes on 2026-10-13. Native Vulkan has no cadence and no smoothing.

## The problem

Model cadence (`model-cadence.md`) runs the model on one frame in N and carries its last edit in between. The model
re-decides its light a little every time it runs, so the frames between two model frames show one decision and the
model frame shows the next: whatever the model drifted by over N frames lands on one frame. BeliyG3 saw "the whole
frame pulsed every N frames"; janblade measured offline (NBA 2K27, slow pan, `DLSS-TOOLS editcache/reuse_sim.py`, not
public) a regional light step of 0.022 stop on the model's frame at every 3rd frame, against 0.009 from frame to
frame with the model on every frame. `pulse-metric.md` is the instrument this fork built to see it.

## What is built

Two things on one switch, both on the cadence's own frames only, both display-only: what is stored for the next
carried frames and what the model is handed as its history stay the model's own answer.

### The spread

A new model answer is walked in over S frames instead of replacing the carried edit at once. On the k-th frame since
the model frame (k = 0 is the model frame itself) the share of the previous answer still shown is

    w_k = 1 - (k + 1) / S        (0 from k = S - 1 on)

with S = N, the whole interval:

| N | model frame | 1st carried | 2nd carried | 3rd carried |
|---|---|---|---|---|
| 2 | 1/2 | 0 | | |
| 3 | 2/3 | 1/3 | 0 | |
| 4 | 3/4 | 1/2 | 1/4 | 0 |

So the new answer arrives in N equal steps, a straight line from one answer to the next, and is whole on the last
carried frame -- never interrupted by the next model frame. The walk keeps the length its model frame decided: a cadence
changed in the middle neither drops the rest of the fade in one frame nor stretches it past the next model frame. On a still view with a drift of d between two answers the
light steps d/N on every frame instead of d on one; `tests/nr-cadence` checks exactly that.

**How, without a history.** On the model frame that follows carried frames, the answer a carried frame would have shown
there -- the previous answer, moved along this frame's chain, the same last link the model is handed as its vectors --
is still at hand, because the stored edit has not been replaced yet. The difference

    fade = accept x (carried answer - model answer)

is stored once, on the model frame's grid (`CadenceFade`). The model frame shows `answer + w_0 x fade`
(`CadenceFadeShown`); each carried frame of the walk shows the carry of the new answer plus `w_k x accept_now x fade(q)`,
read at the same q as the new edit (`CadenceSynthesizeFaded`). Both answers are moved along validated chains; neither
is a blend of earlier blends; nothing that was shown is ever stored.

janblade's spread reaches the same schedule recursively: each frame blends the new detail with *what was shown last
frame*, moved one frame, by a pull of (1 - (k+1)/S) / (1 - k/S), which needs the shown picture kept and resampled every
frame. The stored difference does it with one texture written once, no feedback, and no resampling chain longer than
the cadence's own. The arithmetic is the same straight line.

**Where the old answer does not belong.** The fade is weighed by the carry's acceptance at both ends: on the model
frame by the old carry's (where the last model frame did not show this surface, a disocclusion, the carried answer was
a fill -- a guess -- and the new answer is taken at once), and on each carried frame by the new chain's. A pixel taken
for interface (UI protection, present route) takes the model's answer as it is. This is Skynizz's soft refresh read the
other way round: he walks a pixel with nothing carried from what was on screen, except where the background reappears,
which takes the answer at once "because walking from what stood there would drag its light along". Here everything
with a carried edit of its own surface walks, and everything else, a reappearing background included, takes the answer
at once.

### The light limiter

The mean light of what the cadence shows, relative to the proxy, may move by at most **1.5 stops per second** while the
frame's own light holds still:

    raw   = log2( sum Y(shown) / sum Y(proxy) )      the edit's light this frame, before the limit
    input = log2( sum Y(proxy) )                     the frame's own light
    if a frame before was limited, |input - input_prev| < 0.15 stop, and both are finite:
        shown_light = clamp(raw, shown_prev - 1.5 x dt, shown_prev + 1.5 x dt)
    else
        shown_light = raw                            (a cut, a flash, another room, or nothing to hold to)
    gain = clamp(shown_light - raw, -1, +1) stops    applied as a uniform gain to the shown answer's light

Y is Rec.709 luminance in the space the composition compares the answer and its proxy in: sRGB decoded (as
`dlssnr.hlsl`'s `SrgbToLinear`), or the values themselves on a frame the game already tone mapped. dt is the time since
the pass last ran, clamped to 1/500..1/15 s so a hitch cannot pass a whole re-grade and a frame of nothing does not
freeze the light. No gain is the answer exactly, not a round trip through the curve.

It runs on the GPU in the same frame, after the shown answer exists: `light` sums Y over a 16x16 grid of tiles (every
second pixel each way), `limit` is one thread group that adds the grid, applies the rule (`CadenceLimitLight`) and keeps
four numbers in a 4x1 R32F texture, and `apply` writes the shown answer times the gain into the surface the resolve
reads. Interface pixels (UI protection, present route) are left out of both: they do not count in the light, and they
keep their answer as it is. Every other pixel takes the gain, including one the carry left as the game drew it ("no
match, no fill"): the gain holds back the picture's light, not one pixel's edit, and is bounded and transient. A frame-late CPU readback was not an option: a re-grade would pass whole for the readback's latency and then be
pulled back -- a flash, the thing it is there to stop.

**Why global, and what that costs.** Skynizz's anti pop-in is regional: the 12 px tent of the edit's log light, against
last frame's shown one reprojected, rate-limited where the frame's own regional light stayed put -- and it lives in his
luminance-stability pass, a temporal stabiliser of the regional edit, which is the class of thing the README records as
a dead end. A global rate needs no history but one number and no reprojection. The cost is reach: a regional re-grade
that brightens one half of the picture and darkens the other is not held.

**When it binds.** At 60 fps the limit is 0.025 stop per frame. The cadence pulse three forks describe is 0.016-0.022
stop in *regional* terms, and a global step is smaller than a regional one, so on ordinary drift the limiter does not
bind; the spread is what removes the pulse. The limiter binds on a re-grade: a light entering the picture, or the model
deciding the whole frame differently. In `tests/nr-cadence`, an answer 0.15 brighter in encoded values (0.85 stop of
light) arriving on a still view is held for 32 frames, about half a second at 60 fps, every step within the rate.

The rate is Skynizz's design-note example (a 0.5 stop re-grade in about a third of a second); his shipped default is
0.75 stop/s. 1.5 was chosen as the gentler of the two because this limiter is global and also holds back legitimate
global change on frames where the scene is still.

## Controls: one switch

`CadenceSmoothing` (bool, default false): the checkbox "Smooth the model's frames" under the cadence combo, shown only
above a cadence of 1 (invariant 2). One quantity, one control (invariant 3): the switch says whether a new answer's
light arrives gradually on the cadence's frames, and both parts serve that.

The numbers are fixed, as the carry's are (BeliyG3 fixed everything but N):

- **S = N.** Shorter concentrates each answer's change on fewer frames, the pulse again at a smaller size: Skynizz
  measured, at an 8-frame interval, "the step after a run 6.5x the other frames with 3 [frames of walk], 3.9x with 0
  [the whole interval]". Longer is interrupted by the next model frame and drops what it had blended in at once:
  janblade's `SpreadFor` refuses a spread longer than the interval for that reason (330db46c).
- **1.5 stops/s, 0.15 stop, 1 stop.** See above. A key for the rate is cheap to add if the pulse metric shows a need.

## Defaults, and why off for now

- **Cadence = 1 is bit-identical.** The decision is the `Off` refusal before anything about smoothing is read, the
  cadence object is never built (`ReleasesSurfaces(Off)`), nothing is dispatched, and the composition is untouched.
- **Cadence > 1: off until the pulse metric has read it in this fork** (invariant 1). Both other forks that carry edits
  ship their version on (Skynizz's crossfade and soft refresh are on, his anti pop-in is on at 0.75 stop/s; janblade's
  spread is automatic at every 3rd frame), and an earlier draft of this note took that as reason to make it the default
  under a running cadence. It stays off because nothing here has measured it yet. What would turn it on: the pulse
  metric showing the model/carried ratio falling toward 1 (in-game steps 1-3 of `pulse-metric.md`) with no report of
  the double detail described below. Turning it on is one line in `Config.h` and the ini's "Default (auto) is" text.

## Where it lives

In the cadence pass (`dlssnr_cadence.hlsl`, `dlssnr_cadence_rule.h`, `DlssNr_Cadence_Dx12`), not the composition. The
composition stays what it is: re-anchored every frame to the one answer it is handed, with no history of its own. Only
the answer it is handed on a smoothed frame differs.

## Why this does not reopen the README's dead end -- and where it touches it

The README records that temporal filtering of the model's answer was measured twice and abandoned: the model re-decides
detail with the framing, so old answers do not belong to new frames, and blending them with reprojected history only
moves where the disagreement lands. `model-cadence.md` went further for the cadence: "Two model answers are never mixed
in one pixel", and BeliyG3's cross-pass blend and phase-in were refused, "only behind a separate experiment measured
against the README's claim, if ever". **This is that experiment, and it does mix two answers.** What separates it from
the dead end:

1. **Finite, not recursive.** Exactly two answers, the older one's weight falling to zero in a straight line within
   the interval. No answer survives past S - 1 frames after its successor arrives, and nothing that was shown is stored
   or fed back. The accumulators the README describes were recursive: every answer folded into a history that carried
   all the earlier ones.
2. **Display only.** The stored edit is the new answer as the model made it, and the model is handed the chain to its
   own last answer, so neither the carry nor the model's history ever sees a blend.
3. **Gated where the old answer does not belong**: by the carry's depth and colour acceptance at both ends, and never on
   the interface. A disocclusion takes the new answer at once.
4. **Only where reuse already is.** Carried frames are old answers on new frames by construction (`model-cadence.md`
   says the cadence hits the dead end in motion). The spread turns a step between two such reuses into a ramp; it adds
   no reuse to a frame that had none, and none at Cadence = 1.

What it does not escape: the older answer stays on screen, fading, for up to N - 1 frames longer than without it (its
oldest showing is 2N - 2 frames after it was made instead of N - 1). In motion, where the model re-decided some detail
between the two answers, those frames show a blend of two decisions: a softening, or a faint double of fine detail,
along the walk. The README's claim predicts exactly that, so it is the first thing the in-game check looks for. Whether
a ramp between two decisions reads better than a step is a measurement, not an argument, which is why the pulse metric
was built first.

The limiter keeps one number per frame and no per-pixel state; it holds no detail. It is an exposure adaptation for the
edit's light, not an accumulator.

## Cost

Off (and at Cadence = 1): nothing. On, with a cadence running:

- **Memory:** the fade and the shown answer, RGBA16F at the working size (+16 bytes per working pixel: +56 MB at a
  2560x1440 working size, +127 MB at 3840x2160), a 16x16 RG32F grid and a 4x1 R32F state. The cadence object's
  descriptor heaps and constant buffers go from 48 to 112 slots (sixteen frames at up to seven dispatches).
- **Carried frame:** chain, synthesize (into the shown answer), light, limit, apply: five dispatches instead of two.
  Light reads a quarter of two working-size textures; limit is one thread group; apply reads one texture and writes one.
- **Model frame after carried ones:** model motion, then after the model: fade (the carry's synthesis once more, against
  the outgoing edit), record, coarse, light, limit, apply.

## Refusals, resets, toggles

Smoothing runs only on a running cadence's frames: a scheduled model frame fades, a carried frame walks and is limited.
A forced model frame (a reset, a change, a gap, nothing stored, a second view) and every refusal show the model's
answer as it is, store no fade, and start the limiter over on the next cadence frame. A model frame whose fade or
apply could not be recorded is stored without its fade, so its carried frames do not walk. If the smoothing surfaces
cannot be allocated, the cadence carries without smoothing rather than being refused. The switch is part of what
the cadence surfaces are built for and of the settings the scheduler watches, so a toggle builds the surfaces anew and
forces a model frame. The pulse metric starts a new window on a toggle.

## Tests

`tests/nr-cadence` (host), on the production headers:

- scheduler: the weights (equal steps, whole on frame S-1, zero below S = 2), the limiter's step and its clamps; per
  frame, the fade only on a scheduled model frame, its walk only after a model frame that faded, the limiter on every
  frame of a running cadence and on none other, a reset of it after anything that was not one, and nothing at all with
  smoothing off;
- rules: at weight 0 a carried frame is `CadenceSynthesize` to the bit, with the fade texture full of NaN to prove it is
  not read; a fade that is not finite is dropped at any weight; a still view's drift arrives in equal thirds; under an
  integer pan every frame of the walk is the straight line between the two stand-in models of the content under the
  pixel; a disocclusion and the interface take the answer at once; the limiter's rule (pass, hold, let go, clamp,
    non-finite), its light and gain agree in both spaces, and a 0.85-stop re-grade is held to the rate and then follows a
  jump of the frame's own light at once; a cadence changed mid-walk keeps the walk's length;
- source guards: the surfaces are built only with the switch, the fade before the record and the show after it, the
  limiter's grid one size on both sides, the group-wide modes on whole groups only, the interface out of the light and
  the gain.

`tests/nr-menu`: the checkbox only above a cadence of 1, never on native Vulkan, under the proxy or without the shader.

## In game

On Crimson Desert with `PulseMetric=true`:

1. Cadence 3, smoothing off, a still view: the pulse metric's model/carried ratio well above 1 (`pulse-metric.md`).
2. Same with smoothing on: the ratio towards 1, and the model frames' step towards a third of step 1's.
3. A slow pan, a fast turn, a character crossing the frame, smoothing on against off: look for a softening or a faint
   double of fine detail along the walk, and for ghosting around uncovered areas that lasts longer than without it.
4. A re-grade: walk out of a dark interior into daylight, or switch a light on; the edit's light should follow within
   about a third of a second per half stop, and a cut should pass at once.
5. GPU timing at an interval prime to 3: carried samples cost three small dispatches more than before.
6. A run on Rafael's native-Windows PC before calling it done: this adds an eleventh input to the cadence's root
   signature, typed UAV loads from R32F, group-shared memory in the cadence shader and four new modes, and Proton is not
   a D3D12 validator.

## Attribution

The idea of walking a new answer in over the interval, display-only, and of not letting it run past the interval, is
janblade's (`DetailReuseSpread`, `DlssNrDetailReuse.h`, OptiScaler-F5-DLSSNR-Multipass `a40bfac9`, `330db46c`,
GPL-3.0). The keyframe crossfade over the whole interval, the soft refresh's rule for where to walk and where to take the
answer at once, and the light pop-in rate with its gate on the frame's own light are Skynizz's (optiscaler-dlss5 v1.2.1,
`edit-cache.md`, `dlssnr_cache.hlsl`, GPL-3.0). No code of either is ported: the stored difference that replaces the
recursion, the global form of the limiter and its same-frame GPU path are this fork's.
