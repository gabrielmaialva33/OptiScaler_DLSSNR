# Passthrough chroma: composed versus direct model output

2026-09-14. Test baseline `76feb2cb`; production remains `4cbd0950`.

**The proposed cause of this fixture's fading is falsified.** Bypassing the composed result
with `gReversibleMode=2` does not remove or substantially reduce the fading. The direct output
already contains it. At the final 1280×720 capture, direct versus composed differs by only
**0.024931 RGB byte values on average, maximum 1**, while both differ from the source by
approximately 3.98. The compositing path is not the dominant cause of that alteration.

This does **not** establish that using encoded sRGB values in a linear-light colour transform
is correct. It rejects the causal explanation for this measurement, not every possible colour
space defect or every other operating point. No production source, HLSL, shader bytecode,
configuration or control was changed.

## Protocol and definitions

```bash
python3 tests/dlssnr-loopback/run.py --present-nr --composition-ab
```

RTX 4090, NVIDIA 615.71.09, Proton Experimental. The original step-1 source (no added HUD) and
sequence are used for every point: 1280×720 → 960×540 → 1280×720, 16 frames per generation,
full-resolution model, one pass, zero guides, no FG. Each generation independently creates the
model and resets its first evaluation. All NGX tuning remains the same. Only the harness's
composition constants vary, read through the unchanged production shader and shared cbuffer.
Passthrough remains 1 in both encode and resolve, including the mode-2 run.

Thirteen points: composed default; direct mode 2; TransferStrength 0/.25/.5/.75 with ColourStrength
1; ColourStrength 0/.25/.5/.75 with TransferStrength 1; ColourStrength 1.25/1.5 to exercise the
second OkLab branch; and a repeat of the composed default. The default supplies the endpoint 1
for both sweeps. Mode and strengths are recorded using an anchored `^COMPOSITION-POINT ...$`
pattern and checked against the analysis point table; the final coverage line is anchored too.

All **624 evaluations and Presents** succeeded, with **26 proven-pending resize drains** and
**39 byte-exact ApplyModel=0 controls**. Every frame rewrites a clean source and checks its GPU
readback against the fixture. As in step 1, disk captures are frames **0, 1 and 15** per generation:
117 before/after pairs. Full metrics are computed on those captures, not claimed for unsaved
frames. Frame 0 is the composition control; frames 1 and 15 apply NR.

Analysis reads native PPM RGB bytes directly: no alignment search, resized montage or PNG is
used for measurement. Every point's input is checked byte-for-byte against the default source;
every saved control must reproduce it exactly. The repeat matches the default output exactly
in all nine captures. Component/readback SHA-256 values are retained with the report.

- **Saturation:** arithmetic mean of per-pixel HSV S on encoded RGB, `(max-min)/max`, black S=0.
  Percentage changes below are ratios of these means, not a mean of per-pixel percentage changes.
- **Linear Y:** decode each channel with the piecewise sRGB EOTF, then mean BT.709 weighted Y.
  This is calculated normalized luminance, not photometer data or display calibration.
- **Encoded Y′:** the same weighted mean *without* decoding, also recorded to avoid conflating
  code-value luma with linear-light luminance.
- **Gamma:** per-channel `output=input^gamma`, fixed gain=1 and offset=0, least squares in log
  space; both input/output must be between .02 and .98. Sample counts and RGB RMSE are included.
  A model's spatial edits do not constitute a transfer function; this fit is descriptive only.
- **Regions:** bar band `x < width/2, y < height/8`; saturated bars additionally require source
  HSV S≥.5. The genuinely neutral mask is source S≤.05. The *rest of the scene* is separately
  reported: it contains coloured brick/foliage and must not be called neutral.
- **|delta|:** absolute per-channel RGB byte difference, averaged within each fixed source mask.

These definitions and raw pixel populations differ from a calculation over the composed PNG.
In particular, at 1280×720 the default encoded Y′ falls from .390025409 to .388549892:
**−.147552 percentage points**, or **−.378313% relative**. Linear Y falls **3.1910% relative**.
A figure near “−.14” can therefore describe an absolute encoded-luma delta, but is not
interchangeable with relative linear luminance. No assumption is made about the earlier fit's
exact implementation.

## Results

Final 1280×720 capture (generation 3, frame 15); gamma columns are R/G/B. All points, both
resolutions, captured times, means, maxima and region metrics are in the machine-readable data.

| mode / strengths T,C | saturation Δ relative | linear Y Δ relative | fitted gamma R/G/B | bar band MAE | neutral MAE |
|---|---:|---:|---|---:|---:|
| composed 1,1 | −1.4797% | −3.1910% | .9634/.9941/.9844 | 14.0862 | 2.6867 |
| direct mode 2, 1,1 | −1.4947% | −3.1464% | .9632/.9938/.9841 | 14.0742 | 2.6748 |
| composed 0,1 | 0% | 0% | 1/1/1 | 0 | 0 |
| composed .25,1 | −.1833% | −1.0260% | .9899/.9989/.9968 | 3.5488 | .6745 |
| composed .5,1 | −.5948% | −1.7500% | .9811/.9969/.9912 | 7.0610 | 1.3347 |
| composed .75,1 | −1.0298% | −2.5046% | .9719/.9956/.9875 | 10.5886 | 2.0290 |
| composed 1,0 | +.0472% | −2.7958% | .9898/.9960/1.0019 | 10.3899 | 2.4946 |
| composed 1,.25 | −.3155% | −2.9190% | .9825/.9952/.9966 | 10.4240 | 2.4923 |
| composed 1,.5 | −.6454% | −3.0274% | .9762/.9947/.9918 | 11.3710 | 2.5065 |
| composed 1,.75 | −1.0152% | −3.1198% | .9698/.9944/.9879 | 12.7170 | 2.5558 |
| composed 1,1.25 | +12.5994% | +1.7211% | 1.0183/1.0412/1.0729 | 10.9974 | 2.9254 |
| composed 1,1.5 | +24.0571% | +6.8725% | 1.0889/1.0419/1.1788 | 12.4695 | 3.1615 |
| repeat composed 1,1 | identical to default | identical | identical | identical | identical |

For the *strictly saturated* bar mask, MAE is 16.8112 composed versus 16.7989 direct (maximum
51 in both). For the rest of the scene it is 3.3104 versus 3.3126. Whole-frame source/output
MAE is 3.983909 versus 3.985206, maximum 66 in both.

At **960×540**, the final mean saturation instead **increases**: +1.2530% composed, +1.2466%
direct. Both strength sweeps monotonically increase saturation toward that endpoint. At
1280×720 they monotonically decrease it. The two source sizes are the original procedural
fixture at those extents, not a claim of an identical resampled image. The paired comparison
within each extent is exact. Across all six non-control captures, direct versus composed MAE
is .024931–.030285, and the maximum channel difference is always 1.

[Source / composed / direct comparison](evidence/composition-vs-direct.png) makes the shared
alteration visible. It is an illustration only; upper views are resized and lower crops are 1:1.
[All raw-derived metrics and hashes](evidence/composition-report.json) and
[execution/component record](evidence/composition-run.txt) accompany it. The original PPMs are
under ignored `artifacts/composition-run/`, replaced on rerun.

## What this says about the hypothesis

The source structure cited in the question is real: `dlssnr.hlsl:740–741` does not decode
passthrough proxy/model; `:912` feeds them to HueOkLab; `:148–157` uses the stated RGB/LMS
matrices and cube roots. `:982–983` has a second OkLab use, but **only above colour strength 1**,
so that second branch cannot explain the default-strength observation.

**Confound 1:** mode 2 replaces the result with `modelDirect` (`:989–990`). It bypasses the
*effect* of ratio handling, the highlight guard and palette blend as well as HueOkLab; it is
not an isolated HueOkLab disable and says nothing here about GPU instruction cost. A large
difference could not have been assigned to HueOkLab alone. Instead, the fading stays in the
direct output and the entire composed/direct difference is at most one RGB8 level.

**Confound 2:** monotonicity in the sliders is not an OkLab diagnosis. Transfer strength blends
from the original toward the model-derived result at `:912`; colour strength mixes the
original-colour endpoint with that result at `:979`. Even a model that already desaturated its
output will exhibit that response. That is consistent with this experiment: both sweeps
follow the direction of the direct model's edit, including its opposite direction at 960×540.

At full resolution the proxy closely reproduces the original (`:650–662`), so the ratio at
`:891–900` is near 1. In the ideal equal-input case HueOkLab(model, model) is essentially a
transform/inverse-transform round trip, apart from its near-neutral threshold, gamut handling
and numerical precision (`:179–203`). Feeding an inappropriate colour space into that pair
does not by itself prove a large distortion in this particular call. This is code reasoning,
not a measurement of internal floating-point intermediates.

**Decision:** the prediction that fading disappears or drops sharply in mode 2 failed. For this
fixture and these defaults, the alteration is already in the model output; a production OkLab
change is not justified by this evidence. Other colour strengths/spaces, non-unit ratios and
working scales remain separate questions. We did not patch or numerically replace HueOkLab,
read raw floating-point model intermediates, or isolate which part of the remaining ≤1-level
composition difference comes from round trips, clamps, ratios or quantization.

## Verification and scope review

Tests-only, no new suite or production configuration. DEVELOPMENT.md §3 reviewed against the
diff: production menu/config/HLSL/cbuffer/Vulkan rules have no new changes to audit. The owned
GPU lifetime rules remain: constants, allocators and resources are reused only after completion;
model recreation is drained; all original resize checks remain. The original sequence was
extracted mechanically into one helper so existing and sweep modes execute the same flow.

Verification: the sweep compiled and completed twice under Proton. Pinned clang-format 20,
Python syntax checks and arithmetic controls (known gamma, identity delta, HSV black/grey/primary)
passed. The 12 host suites passed. The original `--present-nr` mode was rerun with the final
compiled harness and passed 48 evaluations/presents, two pending resize drains and three exact
controls. The earlier HUD, cold and upscale modes were not rerun in this change. No production
build was required or run; no files outside `tests/` changed.
