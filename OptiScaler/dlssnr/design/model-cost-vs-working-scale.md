# What the neural pass costs, and how that cost answers to working scale

Status: **measurement, not a design.** Nothing here proposes a change. It records four fence-confirmed
GPU measurements taken in one Cyberpunk 2077 session on 2026-09-13, because they settle several
questions that had been argued from inference, and because the log they came from is rotated by the
game.

## Why this exists

Every optimisation discussion about this module before today was conducted without a number. Both a
delegated internal audit and a delegated external survey produced ranked lists of candidates —
descriptor caching, mutex avoidance in the exposure scan, bypassing HUD bookkeeping in the resource
tracker, caching COM identity and format support — and every one of them was ranked on plausibility.

The module has carried `DlssNr::GpuTiming` for a while: fence-confirmed timing, off by default,
interval-sampled, with the model's own span separated from everything around it. Turning it on ended
the argument in one session.

## The measurements

RTX 4090 (Ada, sm_89), driver 615.71.09, Arch Linux, Proton. Cyberpunk 2077 at 3440x1440, native
DLSS-G, DLSS-NR on the after-Ray-Reconstruction route, one pass, `nvngx_dlssnr.dll` 310.8.SF, build
`85bef821`. `[DlssNr] GpuTiming=true`, `GpuTimingInterval=30`.

`model_ms` is the model's own span. `outside_model_ms` is everything else the pass does on the GPU —
encode, resolve, composition, barriers.

| working scale | model | Mpx | n | p10 | median | p90 | p99 | outside |
|---|---|---|---|---|---|---|---|---|
| 0.25 | 860x360 | 0.31 | 109 | 2.31 | **2.32** | 2.53 | 2.58 | 0.252 |
| 0.50 | 1720x720 | 1.24 | 518 | 2.88 | **2.90** | 3.12 | 3.17 | 0.205 |
| 0.75 | 2580x1080 | 2.79 | 81 | 4.35 | **4.37** | 4.61 | 4.81 | 0.226 |
| 1.00 | 3440x1440 | 4.95 | 193 | 6.34 | **6.36** | 6.61 | 7.00 | 0.202 |

A separate three-hour session the previous evening, entirely at 1.0, gives the full-resolution figure
independently: **n = 11398, p10 6.37, median 6.54, p90 6.90, p99 7.21**. Two sessions, different
scenes, hours apart, agree to 0.18 ms.

### Scene content barely matters

That agreement is itself a result. A three-hour session across the whole city and a few minutes in one
place produce the same median within 3%. The inference is a fixed-size workload; what is on screen does
not change it. Anyone measuring this again does not need to reproduce a scene — half a minute anywhere
is a sample.

## The cost law

Fitting the three points at 0.50, 0.75 and 1.00:

```
model_ms  =  1.76  +  0.930 x Mpx
```

That fit reproduces its own three points to within 0.02 ms. It was then tested blind against 0.25,
which it had never seen:

| scale | measured | predicted from the other three | error |
|---|---|---|---|
| 0.25 | 2.32 | 2.05 | **+0.27** |
| 0.50 | 2.90 | 2.91 | -0.01 |
| 0.75 | 4.37 | 4.35 | +0.02 |
| 1.00 | 6.36 | 6.37 | -0.01 |

The blind point missed by twenty times the residual of the fitted ones. With 109 samples and a p10-p90
spread of 0.22 ms that is not noise: **the curve flattens faster than a line as the model gets small.**
Refitting on all four gives `1.93 + 0.886 x Mpx`, and even that is likely to understate the floor,
because the shape of the miss is the shape of a cost becoming dominated by launch and occupancy rather
than by pixels.

Take the floor as **at least 1.9 ms, reached asymptotically**, not as an exact constant.

### Marginal return per step

```
1.00x -> 0.75x :  -1.99 ms
0.75x -> 0.50x :  -1.47 ms
0.50x -> 0.25x :  -0.58 ms
```

**0.50 is where the curve stops paying.** Of the ~4.4 ms that scale can remove at all, 0.50 collects
3.46 — about 78% of the available saving — with the model still at 1720x720. The step below it divides
the model's area by four again and returns 0.58 ms. That is image quality spent for almost nothing.

`DlssNrRRWorkingScale` already defaults to 0.5. This measurement says the default was right.

## What this settles

**1. Host-side optimisation of this pass is not worth doing.** `outside_model_ms` is ~0.20 ms against
6.36 ms at full resolution: **96.9% of the pass is inside the NGX model.** Every candidate that
targeted our own code — descriptor caching (`708f1dd8`), the exposure scan's mutex on `NoteUav`,
selective HUD bookkeeping in `ResTrack_dx12`, caching `CheckFeatureSupport` in `ExposureTextureUsable`
— competes for a slice of 3%, and several of them for a fraction of that slice. They were declined
before this measurement for lack of evidence; they are declined now with it.

One caveat worth keeping honest: `outside_model_ms` is GPU time. It does not bound the pass's CPU cost,
and nothing here measured CPU. It does bound how much GPU time host-side work could possibly return.

**2. Working scale is the only lever with real range.** 6.36 -> 2.90 ms is 3.46 ms of GPU per frame.
On a 165 Hz display that is more than half a frame's entire budget.

**3. Upstream PR #1158's headline number does not survive contact with this.** That PR adds a third
composition mode, "Matched Residual + DLSS", in which the residual is enlarged by a private DLSS Super
Resolution instance, and claims it takes the pass from ~4.5 ms to under 1.8 ms.

- 1.8 ms is **below the measured floor of this model**, which does not go under ~1.9 ms even at 0.31
  megapixels. A number beneath the floor is not a faster path through the pixels.
- The PR's own timer stops before the added work runs: `ngxTime->End` precedes
  `EnlargeMatchedResidual` in its `DlssNr_Dx12_Run.cpp:381`. The private SR pass the mode exists to add
  is outside the number the mode is advertised by.
- Its published tests are Windows on an RTX 5090; nothing establishes Ada under Proton.
- Arithmetically, 6.36 x 0.25 = 1.59 ms. Their figure is approximately what this tree already gets by
  setting a config key it already ships.

None of that makes the mode worthless. It relocates what it is: **not a speed feature, a quality
feature** — a better answer to "what does the frame look like when the model only saw a quarter of it".
That is settled by looking at images at a fixed scale, not by comparing milliseconds, and this note
should not be cited as an argument against evaluating it on those terms.

## How to reproduce

Set `[DlssNr] GpuTiming=true` and `GpuTimingInterval=30`; on a Ray Reconstruction title also
`ApplyAfterRR=true`, without which the pass never runs at all. Play for half a minute per scale point.
Each accepted sample logs one `DLSS-NR gpu timing confirmed` line carrying `model=WxH`,
`working_scale=`, `model_ms=` and `outside_model_ms=`.

`model` is its own field, distinct from `render` and `compose`. Read it rather than assuming the model
ran at output resolution — on the after-RR route the governing control is `RRWorkingScale`, and the
"Model resolution" slider is not read at all.

Parse with an anchored pattern. `model_ms=[0-9.]+` also matches the tail of `outside_model_ms=`, which
silently mixes the two populations and halves the apparent median; an early pass of this analysis
reported 3.44 ms for exactly that reason.
