# What the neural pass costs on a card that is not Ada

Status: **measurement, not a design.** Nothing here proposes a change. It records one session on a
second machine, because every number this project has is from one GPU, and a single architecture is
not a hardware requirement until somebody checks the next one.

## Why this exists

[model-cost-vs-working-scale.md](model-cost-vs-working-scale.md) settled what the pass costs and how
it answers to working scale. Every point in it came from the same RTX 4090 under Proton. That note is
careful about many things and silent about one: whether any of it transfers to another card.

It does not.

## The measurement

RTX 3060 (Ampere, sm_86), driver 610.62, **Windows 11 native — no Proton**, i9-12900K. Mortal Shell II
(Unreal Engine 5) at 1920x1080, DLSS on the after-upscale route, one pass, our `nvngx_dlssnr.dll`
(sha256 `6eb209e7…`, the same file byte-for-byte as the one all the 4090 numbers used), build
`4cbd0950`. `[DlssNr] GpuTiming=true`, `GpuTimingInterval=30`. 230 accepted samples, zero errors in
the log.

| model | Mpx | n | p10 | median | p90 |
|---|---|---|---|---|---|
| 960x540 | 0.52 | 123 | 16.77 | **16.80** | 17.08 |
| 1920x1080 | 2.07 | 107 | 52.98 | **53.32** | 53.81 |

The distributions are tight — p10 to p90 inside 0.8 ms at both points. This is steady-state cost, not
thermal drift and not warmup. `outside_model_ms` has a median of 0.326 ms, so **98.1% of the pass is
inside the NGX model**, which is the one thing that does transfer: the same conclusion the 4090 gave.

## The two cards, side by side

```
RTX 3060 (Ampere, Windows, driver 610.62)     model_ms = 4.62 + 23.48 x Mpx
RTX 4090 (Ada,    Proton,  driver 615.71.09)  model_ms = 1.76 +  0.930 x Mpx
```

The 3060 fit is two points, so it is a line through two points and not a tested law — the 4090 fit
earned its shape from four points and a blind test, and this one has not. Read it as a scale, not as
a model.

**Fixed term: 2.6x worse. Per-pixel term: 25.2x worse.**

That asymmetry is the finding, and it is the reason this note exists rather than a line in the other
one. A 3060 is roughly three to four times slower than a 4090 at general compute, and the fixed
term's 2.6x sits comfortably inside that. **25x on the per-pixel term does not.** Whatever separates
these two runs, it is not "one card is slower".

### The hypothesis, named as a hypothesis

Ada (sm_89) has FP8 tensor cores. Ampere (sm_86) does not. If the model runs an FP8 path on Ada and
falls back to FP16 or worse on Ampere, the signature is exactly this shape: the arithmetic term
explodes while the launch-and-occupancy term barely moves.

**Nothing here establishes that.** It is a hypothesis that fits, and it was not tested. It could also
be a driver path, a kernel selection, or something the model does that has nothing to do with
numeric format. What is measured is the shape; the cause is guessed.

## Frame generation does not exist there at all

Separate question, same session, and it is not close. NVIDIA's own plugin refuses at the capability
query, before anything in this fork runs:

```
dlss_gEntry.cpp:331   Disabling DLSS-G since it is not supported on current hardware
commonEntry.cpp:1115  NGX_*_GetFeatureRequirements feature: 11 FeatureSupported == AdapterUnsupported
commonEntry.cpp:1089  NGX_*_GetFeatureRequirements returned 0xbad00001 for adapter: 1 feature: 11
```

Feature 11 is `NVSDK_NGX_Feature_FrameGeneration` (`external/nvngx_dlss_sdk/nvsdk_ngx_defs.h:172`) and
`0xbad00001` is `FeatureNotSupported`.

So the MFG work in [mfg-count-override.md](mfg-count-override.md) has nothing to act on here. That
unlock raises a ceiling on a card where frame generation already works; on Ampere there is no frame
generation to multiply. This is a hardware gate in the vendor's runtime, not something a fork can
reach.

The session log also reports `arch=0x170` for this adapter. **No meaning is drawn from that number**
— `feed_opti.h` in the external survey records `0x160` for an RTX 5090, which means these ids are not
ordered by generation and this project cannot decode them. The capability answer is what settles it.

## What this changes

**There is a hardware floor, and it is Ada.** Not as a policy and not as a preference — as arithmetic:

```
an emulator at 60 fps has     16.67 ms of total budget
the pass at half scale costs  16.80 ms on a 3060
```

It does not fit. Not "it fits tightly" — the pass alone exceeds the whole frame before the emulator
draws anything. The same workload on the 4090 costs 2.24 ms of that 16.67 and leaves the frame
intact. At 1080p full scale the 3060 spends 53 ms, which is a 19 fps ceiling from the pass by itself.

This matters most for [nr-present-hook.md](nr-present-hook.md) and
[no-upscaler-titles.md](no-upscaler-titles.md), whose whole purpose is reaching emulators and titles
with no upscaler. That goal is real and this measurement does not retire it. It bounds who it can
serve, and it says so before the work is built rather than after.

It is also worth setting against what the surrounding ecosystem advertises. `DLSS5-Autopilot` and
`DLSS5-Swapper` both list "RTX 20/30/40/50". If this measurement generalises — one card, one session,
so that "if" is load-bearing — then for 20 and 30 series that claim is true in the sense that it runs
and false in the sense that anyone would want to.

## What this session does not establish

- **One card, one game, one session.** A 4060 or a 3090 might land anywhere.
- **Three variables moved at once** against the 4090 numbers: architecture, operating system (Windows
  rather than Proton) and driver (610.62 rather than 615.71.09). The effect is far too large to be
  explained by the last two, but it was not isolated.
- **Ray Reconstruction was not active** — all 358 timing lines report `stage=after`. So this is not
  yet a like-for-like comparison against the Cyberpunk configuration, which runs after-RR. That
  comparison is still owed.
- **Nothing about image quality.** This is milliseconds only.

## How to reproduce

Same as the other note: `[DlssNr] GpuTiming=true`, `GpuTimingInterval=30`, play for half a minute per
resolution point, then parse `DLSS-NR gpu timing confirmed` lines.

Parse with an anchored pattern. `model_ms=[0-9.]+` also matches the tail of `outside_model_ms=`, and
mixing the two populations halves the apparent median. That trap cost a wrong number once already in
the 4090 analysis; the parse behind this note was checked by asserting that no sample has
`model_ms == outside_model_ms`.
