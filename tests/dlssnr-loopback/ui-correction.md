# UI correction on a controlled presented frame

2026-09-14. Production baseline `4cbd0950`; present transport baseline `dbe6894c`.

**Result: no observable effect of `DLSSNR.UICorrection` on the presented RGB8 output in this
experiment.** Creating the model with 0 versus 1 produced byte-identical images throughout the
sequence. It neither improved nor worsened this HUD. This does not establish that the parameter
is unused in the model, or that it is ineffective with separate UI inputs, other models, HDR,
working scales, presets, or reconstructed guides.

## Experiment

```bash
python3 tests/dlssnr-loopback/run.py --present-nr --hud-ab
```

The opt-in mode requires numpy and Pillow in the runner's Python environment for analysis.
It retains the existing compiler-prefix busy check and isolated Proton runtime. All changes are
under `tests/`; there is no production key, menu control, new host hook or shader modification.
Generated readbacks occupy approximately 1.1 GB in ignored `artifacts/hud-run/` and are replaced
on rerun. No game install is read or modified by this mode.

Hardware: RTX 4090, NVIDIA 615.71.09, Proton Experimental. Full-resolution 1280×720 SDR,
production composition bytecode and feature-18 forwarder, one model pass, preset/style 0,
intensity/local strengths 1, Auto Skin Mask 1, Transfer 1, zero R32_FLOAT depth and R16G16_FLOAT
motion, no confidence. Guides are cleared once per model generation, submitted and completed.
The explicit SDR passthrough and all transport/allocator/resize lifetime rules from step 1 remain.

`hud_fixture.h` adds a test-authored hard-edged 5×7 font at 1×/2×/3×/4× sizes, a translucent
menu over the brick texture, highlighted menu rows, single-pixel minimap roads, 2×2 markers,
and an outlined counter. No external font rasterizer or antialiasing changes the fixture.
Each iteration rewrites the entire source from clean bytes and validates a GPU source readback.

| frames | source state |
|---|---|
| 0–7 | counter `100` on left, panel visible |
| 8–15 | counter moves right and changes to `075`, panel remains |
| 16–23 | counter stays right, panel disappears |
| 24–31 | original counter position/value and panel return |

Only frame 0 resets model history. HUD changes deliberately do not reset it. Frame 0 also runs
NR with `ApplyModel=0` during composition, demanding byte-exact source reproduction; all other
frames apply the model. One submission completes before each Present, and every feature is
drained/released before the next trial starts.

Six independent 32-frame trials use identical sources and history:

1. create with 0;
2. create with 1;
3. repeat create-0;
4. repeat create-1;
5. create with 0, then write 1 into the real capability block before every evaluate from frame 8;
6. create with 1, then write 0 from frame 8.

Both creation and subsequent writes are read back with `Get("DLSSNR.UICorrection", unsigned*)`;
all succeed and equal the intended value. This proves parameter-block storage, **not** consumption
by the opaque model. The existing extras export explicitly clears `UI`, `UIAlpha`, `Backbuffer`
and their extents, matching the production call with no extra layers. The HUD exists in `Color`.

## Measured result and image inspection

- **192 successful evaluations and Presents**, six exact composition controls, 192 before/after
  pairs, successful create/release-generation sequence and final shutdown.
- Every paired source matches exactly. The analyzer also checks the intended three source states
  actually occurred; a static fixture accidentally used for all frames cannot pass.
- **Create-0 versus create-1: MAE 0, maximum 0, changed RGB channels 0** for all 32 frames.
- Both same-setting repetitions are byte-identical across the sequence. No observed run-to-run
  noise is being misattributed to the flag.
- Both evaluate-only switches likewise produce identical output to the constant-setting trials.

The model still changes the HUD. At frame 7, compared with the clean source (byte units 0–255):

| region | MAE | maximum channel change |
|---|---:|---:|
| whole frame | 3.715716 | 84 |
| hard-edged text panel | 2.779026 | 84 |
| translucent menu | 3.160829 | 27 |
| minimap | 2.131687 | 53 |

These are differences from the source, not a perceptual score. Visually, text remains readable,
but bright glyphs and fine lines acquire changes around their edges and in their brightness;
menu/background tone also changes. The two switch positions provide **no protection difference**.
No dramatic persistent old counter or large menu halo was apparent in the inspected frames 7,
8 and 16. This is not a measured absence of ghosting: we did not isolate history effects with a
same-current-frame, different-history counterfactual, and the ordinary model edit also changes
background pixels. The report's `vacated_icon` ROI is an instrument, not a ghosting classifier.

[Static HUD comparison](evidence/ui-correction-f7.png) and
[HUD transition comparison](evidence/ui-correction-f8.png) show source, create-0 and create-1.
Upper images are resized previews; lower crops are 1:1, without sharpening or amplified deltas.
[Machine-readable results](evidence/ui-correction-report.json) include every PPM SHA-256,
per-frame full/ROI MAE and maximum, and repeat/switch comparisons. Full originals and additional
frames 16/24/31 remain under `artifacts/hud-run/`. Hashes of the tested binary components and
selected execution results are in [the run record](evidence/ui-correction-run.txt).

## Does a change require feature recreation?

**The supported interface in this tree is creation-time.** The three forwarder create paths
write the unsigned key before calling feature 18 (`dlssnr_forwarder.cpp:652`, `:748`, `:880`).
The D3D12 comment at `:871` says the model reads tuning when creating the feature. The D3D12
evaluate signature at `:889` has no UI-correction argument; its parameter writes never overwrite
this key. Vulkan and D3D11 likewise have no evaluate argument for it.

This experiment recreated the feature for the actual A/B. The additional direct-block writes
show that a caller *can store* the key per frame, but gave no image effect. **That does not prove
a latch:** with no observable difference even between create-0 and create-1, the test cannot
distinguish create-only sampling from per-evaluate sampling behind an inactive condition.
A future control should conservatively follow the existing create-time contract until a scenario
with a demonstrated effect can distinguish those cases. No live setter is established here.

There is also an important distinction in the production comment. “No UI layer fed to it”
(`DlssNr_Dx12.cpp:2556`) is not equivalent to “no UI pixels in Color.” The forwarder exposes
separate `DLSSNR.UI`, `DLSSNR.UIAlpha` and `DLSSNR.Backbuffer` inputs at `:972–974`;
`DlssNr_Dx12.cpp:3083` clears those extras. Whether separate layers activate UI correction is
**untested**, not an explanation established by this negative result. A generic present host
cannot assume it has those layers just because the flattened backbuffer has text in it.

Consequently, `no-upscaler-titles.md:114–115` overstates the evidence when it says the parameter
becomes the one that decides the route's usability once HUD is present. The narrower conclusion
is that there is a parameter to investigate; **this measurement found no help for flattened HUD**.
It supports deferring a production control, not declaring HUD preservation solved.

## Review and limits

DEVELOPMENT.md §3 review performed against the actual diff: no production UI, config, HLSL,
cbuffer, Vulkan or non-NR execution path changes. The mandatory reviews for those change types
are inapplicable; production default-off behaviour is unchanged because no production file
changed. For the test-only resource change, CPU upload/constant/parameter/descriptor reuse and
feature release remain after a proven fence; explicit resource states and failure-before-unwind
are preserved. New modes are opt-in, and no new suite directory requires a suites.toml entry.

The result is for composed RGB8 readbacks, not raw floating-point model outputs. Sub-LSB changes
could be hidden by composition/quantization. There is no FG, camera movement, reconstructed motion,
real-game HUD, separate UI layer, multi-swapchain concurrency or GPU removal fault injection.

Verification: the HUD harness compiled and ran successfully under Proton; pinned clang-format
20 and Python syntax checks passed; the 12 host suites passed. The unchanged `--present-nr`
scenario was rerun using the same newly compiled harness and passed 48 evaluations/presents,
two proven-pending resize drains and three byte-exact controls. No production build was needed
or run for these tests-only changes. `--cold-nr` and the old upscale sweep were not rerun in this
change (their successful runs belong to the preceding step-1 commit).
