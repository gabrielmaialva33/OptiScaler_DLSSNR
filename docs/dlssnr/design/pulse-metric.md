# Pulse metric: the light step per frame, model frames against carried ones

Status: implemented on D3D12 behind `[DlssNr] PulseMetric` (default off) and a checkbox under Compare; **not yet
run in a game**. The shader's bytecode header is optional (`__has_include`): until `DlssNr_Pulse_Shader.h` is
generated the metric reports itself unavailable and the menu does not offer it. Native Vulkan has no
implementation and is not offered the control.

## Why

Model cadence (`model-cadence.md`) has a named artefact and no number for it. BeliyG3 saw "the whole frame pulsed
every N frames"; janblade measured offline that the regional light steps 0.022 stop on the frame the model runs at
every 3rd frame, against 0.009 from frame to frame with the model on every frame; Skynizz's v1.2.1 notes put the
step on the model's frame at 0.016 stop before a fix and 0.002 after. Nothing here measured it, so neither the
cadence nor anything meant to smooth it (`cadence-smoothing.md`) could be judged except by eye. This is the
measurement those two need, and it is built first.

## What it measures

Per measured frame t, over a quarter of the pixels (every second one each way):

    L_t = log2( sum of Y(output_t) / sum of Y(frame_t) )        the edit's light, in stops
    s_t = L_t - L_(t-1)                                          the light step

`Y` is Rec.709 luminance of the non-negative part. `output_t` is the pass's target after the resolve and the
output stabilizer, the picture that is shown; `frame_t` is the encode's untouched copy (`hdrCopy`), the game's own
frame the resolve composes against. Both are in the buffer's own units; the ratio does not depend on them.

Each step is filed under the kind of the later frame:

| Kind | The later frame was |
|---|---|
| model frames | a frame of a running cadence on which the model ran |
| carried frames | a frame on which the cadence carried the last model frame's edit |
| every frame | a frame on which the model ran with the cadence not in effect (Cadence=1, or a refusal) |

The step into a model frame is the pulse. The reading is the mean |s| per kind over a window, and their ratio,
model over carried.

### Why the edit's light and not the output's

The task's working definition was "log2 of the mean-luminance ratio of the NR output between consecutive frames",
`log2(sum Y(output_t) / sum Y(output_(t-1)))`. On a still camera that is the same number as `s_t`, because the
frame under it did not change. In motion it is not: a pan into a brighter room, the game's exposure adapting, a
muzzle flash all move it, and none of them is the edit's doing. Dividing by the frame's own light first cancels
those to first order, so `s_t` is the step the pass added. It does not cancel content moving under an edit that
differs from place to place (the model brightens a face more than a wall), which is why a pan still reads higher
than a still view, and why the note on the menu says a still scene reads cleanest.

### Why one global number and not a regional one

Skynizz's probe measures the step of the edit's *regional* light (a 12 px tent of `log2(shown / untouched)`, at a
quarter of the frame), each region against last frame's moved along the vectors, and averages |step| over the
regions; janblade's offline figures are regional too (`DLSS-TOOLS editcache/reuse_sim.py`, not public, not read).
A regional mean of |step| is larger than a global step, because regions that brighten and regions that darken
cancel in a global mean and not in a mean of absolute values. So **this metric's numbers are not comparable with
theirs**; only ratios within one session are.

The global form was chosen because it needs no history texture, no reprojection and no vectors -- one reduction
and a 4 KB readback -- and because the pulse the three forks describe is the whole picture's light stepping. A
regional reading would catch a pulse that cancels globally (half the frame up, half down, on the same frame). The
GPU already writes a 16x16 grid of sums, so a tile-wise variant is a CPU change if one is ever wanted; it would not
be motion-compensated, so it would read pans as steps.

## Where it is computed

A pass of its own (`dlssnr_pulse.hlsl`, `DlssNr_Pulse_Dx12`), one dispatch of 16x16 thread groups, each summing
its tile's luminances and reducing in group-shared memory into one RGBA32F texel `(Y out, Y frame, pixels, 0)`;
the 16x16 grid is copied into one of sixteen readback buffers. It runs once per Dispatch, last, after the
stabilizer and the detail measure, on the target as the pass leaves it.

The detail measure (`detail-measure.md`) was the obvious host and was not used: it is a 60-frame session on a still
scene that copies both frames every evaluate and throws away any frame where the input moved, while this has to run
continuously, in motion as well, and cost little. Folding a luminance sum into the composition shader was not
considered: the composition must not change for an instrument, and both of its targets would need a rebuild.

The CPU half is `dlssnr/DlssNr_Pulse.h`, pure and host-tested: the edit's light from a grid, the pairing of frames
into steps, the windows, the words and the log line.

### Pairing

The renderer counts every present at which the pass is asked to run (`NotePulsePresent`, at the top of Dispatch,
before its first early return), whether it then runs, skips or fails, and tags each
measurement with that count. A step is taken only between two measurements whose counts are consecutive and whose
times are under 250 ms apart (the cadence's own gap rule), so a present that could not be measured -- the pass
returned early or did not write the target, the readback slot was still in flight, the command list could not be
tracked -- breaks the pair instead of bridging it. A skipped frame shows the game's own picture, and a step bridged
across it would hide two real steps. Readbacks are collected oldest first; one that arrives after a newer one is
ignored. The counter is one comparison per call and runs whether the metric is on or not, so the first measurement
after it goes on has a number to pair with.

Only the first call in a present is measured. A title that runs the pass twice per present (the cadence's
`SamePresent`) would otherwise pair two views. Where presents are not counted at all (the counter stays 0) every
call is measured; those routes are refused by the cadence anyway (`NoPresentCount`), so all their frames are
"every frame". A title that ran the pass twice per present on such a route would pair its two views; none is known.
Presents at which Dispatch is never reached at all (the toggle key, a game skipping its upscaler) are not counted, and
are bridged only when they last under 250 ms.

## Cost

Off: nothing. No object, no allocation, no dispatch, no barrier; the key is read and the present counter compared
once per Dispatch.

On: one compute dispatch reading a quarter of two frame-size textures (2 x 2 M loads at 4K) and writing 256
texels, two transitions of the target around it, and a 4 KB copy. Memory: a 4 KB grid and sixteen 4 KB readback
buffers, sixteen 256-byte constant buffers and descriptor heaps. The pass object does not depend on the frame's size,
so a resize does not rebuild it. It is parked through the retired list when the metric goes off, like every other
object the last recordings may still reference.

## How it is shown, and how often

- **Menu**, under Compare beside Measure detail, while the key is on: one line from the latest window, e.g.
  `Light step, stops per frame: model frames 0.0123, carried frames 0.0041 (model / carried 3.0x)`, or
  `... every frame 0.0039` at Cadence=1. "measuring..." until the first window completes.
- **Log**, at INFO: the first window after the metric starts or what it measures changes, then at most one line every
  10 s (a window completes every two seconds). The line carries every number: mean |step|, count, signed mean and
  largest step per kind, the ratio and the cadence:

      DLSS-NR pulse: light step over 120 frame pairs at cadence 3 (mean |change| of log2 output light / frame
      light, stops); model frames 0.01230 (n=40, signed +0.00010, max 0.0300); carried frames 0.00410 (n=80, ...);
      model / carried 3.00x

## How it is reset

- **Windows.** A reading is 120 steps (two seconds at 60 fps, 40 cadence cycles at cadence 3). Each completed
  window replaces the last one, so the reading follows a setting within a few seconds without a reset button.
- **What changes the meaning of a step** starts the window over and drops the published reading ("measuring..."):
  the frame's size or format, the configured cadence (and, since `cadence-smoothing.md`, its smoothing; since
  `residual-limit.md`, the residual limit). Readbacks recorded before the change are collected and discarded.
- **Switching it off** parks the pass and forgets everything.
- **Not a reset:** composition sliders. Moving one makes one large step, which lands in one window; the next window is
  clean. A reading taken while dragging a slider is not a reading.

## What it does not prove

- It is not image quality. A cadence can read a ratio of 1 and still ghost; the carry's error against the model's
  real answer is a different measurement (Skynizz's `measure_ghosting.py` shape), not attempted here.
- It is global: a pulse that brightens half the frame while darkening the other half reads as no pulse.
- In motion the scene's own content under a non-uniform edit adds steps to every kind alike. Compare kinds within
  one window, and compare settings on the same still view.
- On a frame the game already tone mapped (passthrough, the present route), luminance is of encoded values, so
  "light" is perceptual rather than physical. Ratios between kinds are unaffected.

## Tests

`tests/nr-pulse-metric` (host): the CPU header compiled under ASan/UBSan -- the edit's light from a grid (tile
weighting, empty and non-finite tiles), pairing (holes, gaps, non-finite frames, late readbacks), windows by kind,
reset, the menu's words and the log line, the log's rate, and a synthetic cadence whose model drifts by a random walk
(every frame steps by the drift; at cadence 3 the carried frames step by nothing and the model frame by three frames'
worth). Source guards: constants equal the cbuffer, registers equal the root signature, one grid size on both sides,
optional bytecode, off builds and records nothing, one sample per present, stale readbacks dropped, the target's state
kept, parked, registered in the project. `nr-invariants` checks the header against its `.cso` once both are committed.

## In game

With the bytecode generated and the key on, on Crimson Desert (after-upscale route):

1. Cadence 1, a still view: the log shows "every frame" only. That number is the model's own frame-to-frame drift and
   the floor everything else is read against.
2. Cadence 3, same view, smoothing off: "model frames" well above "carried frames" (the carried frames of a still view
   repeat one answer, so their step is near zero; the model frame carries three frames of drift). That ratio is the
   pulse this metric exists to see.
3. Cadence 3, smoothing on (`cadence-smoothing.md`): the ratio falls towards 1 and the model frames' step towards a
   third of step 2's.
4. A slow pan with each of the above: every kind rises; the ordering should hold.
