# Running the model before the upscaler: measured

Status: **measurement, not a design.** Nothing here proposes a change, including a change of default.
It records one Cyberpunk 2077 session on 2026-09-15, because it settles the largest cost question
this module has, and because it also caught a mistake in
[model-cost-vs-working-scale.md](model-cost-vs-working-scale.md).

## Why this exists

`[DlssNr] Stage` has shipped in this tree for a while: `0` runs the model after the upscaler over the
display-size frame, `1` runs it before, over the game's render-size colour, and hands the upscaler the
edited copy. `ScopedPreUpscale` implements it, `tests/nr-before-upscale/` covers its boundaries, and
the menu exposes it as a live combo.

**It defaults to `0`, and every measurement this project had ever taken was on `stage=after`.** The
biggest lever in the module was shipped, tested and unmeasured. The press has meanwhile credited the
technique to other forks — `OptiScaler-DLSSNR-PreSR-Multipass`, and the "Neural Upstream" route in
DLSS5-Autopilot — with reported frame-rate recoveries on Blackwell. None of that is a number for this
tree on Ada under Proton.

## The measurement

RTX 4090 (Ada, sm_89), driver 615.71.09, Proton. Cyberpunk 2077 at 3440x1440, **DLSS Quality**
(render 2293x960), **Ray Reconstruction off in the game**, `[DlssNr] ApplyAfterRR=false`, one pass,
build `4cbd0950`, `GpuTiming=true`, `GpuTimingInterval=30`. The stage was switched live from the menu
inside one session, so both arms share a scene, a driver, a build and a model.

| stage | model runs at | Mpx | n | p10 | median | p90 | outside model |
|---|---|---|---|---|---|---|---|
| after | 3440x1440 | 4.95 | 142 | 6.77 | **7.03** | 7.33 | 0.179 |
| before | 2293x960 | 2.20 | 72 | 3.76 | **3.85** | 4.06 | 0.082 |

**3.18 ms, or 45% of the pass.** On a 165 Hz display the whole frame budget is 6.06 ms, so this is the
difference between the pass not fitting and fitting.

**The transport halved too**, 0.179 to 0.082 ms, which was not predicted. It follows once stated: on
this route the copies are render-sized rather than display-sized. `nr-present-hook.md` notes that the
present host's copies are full resolution "whatever the model runs at" — that is true there and false
here, and the two should not be confused.

### The prediction held on one side and failed on the other

The four-point law in the sibling note is `model_ms = 1.76 + 0.930 x Mpx`. Predictions were written
down before this session:

| arm | Mpx | predicted | measured | error |
|---|---|---|---|---|
| before | 2.20 | 3.81 | 3.85 | **+0.04** |
| after | 4.95 | 6.36 | 7.03 | **+0.67** |

The arm that was being tested came in at 1%. **The baseline missed by 10%**, and that is the finding
hiding inside this one.

## The after-upscale and after-RR routes do not cost the same

Every point in the four-point law was taken on the **after-Ray-Reconstruction** route. This session
ran **after-upscale**, and the same model size cost **0.67 ms more**.

That is not noise: 142 samples with a p10-p90 spread of 0.56 ms. It is a different scene and a
different session, so it is not isolated either — but the sibling note presents its law as *the* cost
of the pass, and it is at best the cost on one of the two routes. **Quote 7.03 ms for after-upscale at
4.95 Mpx, not 6.36.** Why the routes differ is unexplained and was not chased.

## Image quality

**Reported as no visible difference by the person playing it.** That is a real data point and it is
the one that decides whether the cost is worth taking, so it is recorded rather than discounted.

What it is not: it is one game, one session, one subjective judgement, at **DLSS Quality — the mildest
ratio there is**. At Quality the model still sees 67% of the output's linear dimension. Below that it
sees progressively less, and the external reports are consistent that this is where the route starts
to cost something: the model works from a jittered, pre-antialiasing frame, and the upscaler then
enlarges whatever it made of it. The menu's own help text warns about shimmer and unstable detail on
moving edges.

**Nothing here establishes the route at Balanced, Performance or Ultra Performance, and nothing here
is a still-image comparison.** Those are separate sessions.

## What this does not change

The default. `DlssNrStage` stays `0`. One session in one game at the mildest ratio, with a subjective
quality report, is the beginning of the case for flipping a default, not the end of it. The honest
recommendation today is narrower and worth stating plainly: **for a D3D12 title with DLSS and no Ray
Reconstruction, `Stage=1` is worth turning on and looking at**, because it costs 45% less and at
Quality at least one player could not see the difference.

Ray Reconstruction stays on stage 0 by construction — the pre-upscale path declines it, because its
colour input is undenoised and the model has no business synthesising detail into noise. That is why
every previous Cyberpunk session measured `after`: the route was never available while RR was on.

## How to reproduce

`[DlssNr] Enabled=true`, `GpuTiming=true`, `GpuTimingInterval=30`, `ApplyAfterRR=false`. **Turn Ray
Reconstruction off in the game** — without that the pre-upscale path declines and both arms measure
the same thing. **Do not use DLAA**: the saving comes entirely from render resolution being below
output, and at DLAA they are equal and the saving is exactly zero.

Then switch the stage from the menu mid-session rather than restarting, so both arms share a scene.
Parse with an anchored pattern; `model_ms=[0-9.]+` also matches the tail of `outside_model_ms=`.
Separate the populations on `stage=`, which the timing line carries.
