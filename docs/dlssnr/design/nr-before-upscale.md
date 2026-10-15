# Optional Neural Rendering before upscaling

Status: experimental port built and host boundary tests passed (2026-09-05).
A September 15 Cyberpunk cost A/B is recorded in [stage-before-upscale-measured.md](stage-before-upscale-measured.md);
paired image-quality and temporal measurements remain open. The protocol below targets that gap.

Adapted from GrimsVerk/OptiScaler_NR_then_SR, branch `nr-before-upscale`, commit
`ba3ed2e401` and its Stage=1 implementation by that fork's contributors:
https://github.com/GrimsVerk/OptiScaler_NR_then_SR/tree/ba3ed2e401
The external Jedi Survivor measurements are motivation, not validation of this port.

## Contract

`[DlssNr] Stage=0` remains the existing after-upscale path. `Stage=1` is an opt-in
D3D12 experiment: read the game's render-resolution color, compose NR into a private
texture, lend that texture to one upscaler evaluation, then restore the original
parameter value and resource states. Never write the game's input color. Frame
generation evaluations do not participate. Ray reconstruction must use the existing
after-upscale path because its input is undenoised. Native Vulkan remains after-upscale.

The decision belongs to each evaluation's scope, not a global flag: interleaved FG
must not cause both pre- and post-upscale NR to execute for one upscale. A build or
skipped NR frame must leave the original input selected. A failed upscaler call must
still restore the parameter block. Typed and untyped NGX Color slots must be preserved.

Adapt the source/target split to our GPU exposure, supersampling, reversible proxy,
and frame-hold implementation. No composition shader or cbuffer change is intended.
When NR is disabled or Stage is zero, no pre-upscale resources are allocated.

## Improvements required over the reference

- Reject unsupported formats, multisampling, nonzero color origins and invalid
  subrect bounds before recording work. Do not silently clamp an invalid contract.
- Allocate every scratch with the resource flags its requested resting state requires;
  never fall back to a texture incapable of entering that state.
- Track scratch device ownership and the exact texture borrowed by each scope.
- Debounce changing render extents before allocating/building the pre-upscale path,
  so continuously changing dynamic resolution does not rebuild the model every frame.
- Require an observed present after creating a pre-upscale NGX feature. Multiple
  evaluations in one frame must not evaluate a feature on its creation frame.
- On the new pre-upscale path, restore output states on every Dispatch exit and
  protect the NGX creation frame's command-list bindings. The default post-upscale
  path keeps its original behavior; its PR #23 audit findings remain a separate fix.
- Expose the experimental stage only where it can apply, and describe fallback and
  the known image-quality tradeoff without promising a particular speedup.

## Review and validation

Verify config declaration/read/save/shipped default; stage routing including FG/RR;
typed/untyped parameter restoration; no scratch substitution until a successful write;
same-sized stage switches resetting temporal history; invalid/sRGB/MSAA/subrect cases;
allocation failure and changing-resolution behavior. Build Release under msvc-wine.
Review resource ownership and every early return manually against DEVELOPMENT.md.

Automated host tests can prove routing and ownership decisions, not the NVIDIA model's
behavior. Local game A/B testing must compare fixed camera and movement at the same
settings, recording model/total GPU cost, VRAM, actual render/model dimensions, and
edge stability. Test stage toggles, resolution changes, failed/unsupported paths and
FG interleaving. No installation or marker changes are part of this implementation.

## Adversarial review record

### Coverage logging follow-up

Before any game deployment, add INFO-level coverage for `before`, `after`, and
`after-fallback` separately. Count boundary evaluations, model API successes/failures,
recorded compositions actually selected by the boundary, skipped evaluations and
fallback requests. Log the first observation, first applied composition, first model
failure and a periodic summary even if every evaluation skips. Include the observed
present id, dimensions and last skip reason. A normal Stage 1 post-path stand-down
is not another skipped NR evaluation. Disabled NR must remain inert.

These are CPU recording/selection observations, not GPU completion or displayed-pixel
evidence. Dispatches are not frames; any present-based counters must identify their
deduplication rule and cannot be summed across stages or outcomes. The test suite must
reject silent zero application and distinguish model success without a selected
composition from an applied result.

### GPU lifetime boundary for the first A/B

A complete fence solution exceeds this boundary/logging follow-up. The pass records
into a caller-owned command list; signaling a queue inside the pass could signal
before that list is submitted, and would falsely certify retirement. Correct retirement
needs the actual submission queue and a fence value ordered after the last use of
each retired feature/resource, including the upscaler's use of the borrowed scratch.
All participating queues and shutdown/device-loss paths must obey that contract.

No such submission tracking is added here. `TickNrRetired` counts evaluations, not
frames or GPU completions. Multiple evaluations in one present can exhaust its 32
ticks, and even 32 presents cannot bound delayed GPU work. `CreationFrameGate` only
prevents evaluation on the observed creation frame; `StableExtent` only reduces
rebuild frequency. Neither makes retirement safe by itself.

Consequently, DRS/resolution/format changes, live Stage/working-scale/model-setting
switches, repeated rebuilds and device recreation are **not approved for stress
validation with this port**. A later stress test requires submission-ordered retirement
or equivalent GPU-completion evidence, including delayed/multiple-list workloads.
The proposed first smoke A/B is fixed-resolution, DRS off, FG off, with Stage selected
before starting a separate game process for each run. Keep working scale/model settings
fixed, preserve markers, and retain rollback binaries. This reduces rebuild exposure;
it does not guarantee GPU safety, including startup, ordinary scratch reuse and teardown.
No game test or installation is part of this follow-up.

Reviewed by hand against DEVELOPMENT.md sections 1 and 3, before committing:

| Rule/type | Evidence and limits |
|---|---|
| 1, default-identical | Stage defaults to 0; inactive scope tests observe zero parameter reads/writes and zero scratch allocations. On the post path, `source == target`, the original binding envelope remains at its original position, and the original three output-restoration sites remain. Model, meter, encode and resolve arguments match the baseline after substituting this alias. No local rendered-pixel A/B comparison has been made. |
| 2/3, menu | One Stage combo, two choices. Native Vulkan shows effective after-stage status instead of an ineffective control. Stage 1 shows runtime skip/fallback status. Existing white-point source/anchor/trim rows are unchanged. Hold frame and the driver proxy explicitly fall back to post-upscale. |
| 4, config | `DlssNrStage { 0 }`, `readUInt`, `SetValue` and `[DlssNr] Stage=auto` checked mechanically; full game reload/save remains an integration test. |
| 5, cbuffer | Only CPU-side `DlssNrFrameInfo::OutputState` was appended. `DlssNrConstants` is byte-identical source to 660303ec; all 23 four-byte scalar fields match `Params` in order and type. |
| 6, shader | HLSL and both precompiled headers are byte-identical to 660303ec. No HLSL edit occurred, so no shader rebuild was required. |
| 7, passthrough | Encode/resolve flags and all HLSL passthrough branches, including `fullProxy`, are unchanged. The pre path reads original color in the same codec; no new composition formula is introduced. |
| 8, resources | No native Vulkan allocation/free code changed. New D3D12 scratch scopes restore color/guide/exposure states, reject incompatible flags/formats, serialize loans, and retire replaced scratch through the existing NR retirement mechanism. Host tests do not prove GPU completion. |

The host suite executes the production allocation/scope/post-routing boundary with
strict fake resources under ASan/UBSan: 40 cases, 1000 allocation-failure attempts
(one allocation), 1000 changing-resolution evaluations (zero allocations), and 1000
same-frame gate checks (no evaluation allowed). The fake composition controls its
write result; it does not execute `Dispatch`, HLSL, NGX or a real upscaler.

The coverage follow-up also extracts the production INFO reporting scope and tests
the real counter helper. Manual adversarial review checked: success without a resolve
does not increment application; success followed by all-skip intervals cannot reuse
an earlier positive status; fallback has its own post-stage bucket; the normal Stage 1
post stand-down and disabled NR create no coverage; present counters deduplicate
multiple evaluations without claiming GPU completion. The added optional Dispatch
output only observes model results and skip reasons. Model/encode/resolve arguments,
shader constants, HLSL and both bytecode headers still match baseline checks. No config,
menu or resource lifetime behavior changes in this follow-up.

See `tests/nr-before-upscale/README.md` for log fields and A/B acceptance criteria.

### Remaining integration limits

- The inherited NR retirement mechanism releases parked resources after 32 evaluates;
  this is not a GPU fence and does not prove safety for arbitrarily delayed or
  unsubmitted command lists. This port does not claim to solve that module-wide issue.
- Present counting prevents same-frame creation/evaluation but is not a GPU-completion
  fence either. Without observed presents, Stage 1 falls back to the post path.
- Device changes are refused by the pre-upscale scratch path; general NR device
  recreation remains outside this port. Restart for a changed D3D12 device.
- Scratch allocation failure is latched for the contract, avoiding retries every frame;
  changing dimensions/format or changing Stage in the menu permits another attempt.
- Typeless color, sRGB, MSAA, arrays, offset/incomplete/out-of-bounds color subrects,
  incompatible resource states and aliased inputs are not guessed. They fall back.
- Dynamic resolution waits for 500 ms of stable dimensions and format, during which
  the upscaler uses the original input without NR. This trades availability for fewer
  reallocations; it is not a claim of full continuous-DRS support.
- Local game quality/performance, D3D12 bridges, RR fallback and FG interleaving still
  need real integration validation. The existing Vulkan overlay harness covers none
  of the NR module. Existing post-upscale PR #23 audit findings remain open.

## 2026-09-27: against wilsjo2's pre-SR

wilsjo2's [OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
(GPL-3.0) was read at `1bd39091`. It has the same placement ("Apply before Super Resolution") and is
the fork people credit with it. What this tree has, what was measured, and what is missing:

- **Exists: `Stage=1`, D3D12, including the D3D11 and Vulkan bridges.** It declines typeless, sRGB,
  MSAA, arrays, a non-zero colour origin and Ray Reconstruction, and waits for 500 ms of stable
  extent. wilsjo2 declines the same layouts.
- **Measured:** Cyberpunk 2077 on an RTX 4090. Model cost fell from 7.03 to 3.85 ms median (-45%)
  ([stage-before-upscale-measured.md](stage-before-upscale-measured.md)). That was at
  `WorkingScale=1`, the one scale the guide fault below cannot touch.
- **Padded input** (their `docs/PADDED-PRESR.md`: an origin-zero picture inside a larger
  allocation). It already works here, by another route. The scratch is sized to the render subrect,
  and the upscaler reads it with the subrect it was given. Nothing is copied back, because the
  game's texture is never written; the parameter is swapped. Their 2558x1439-in-2560x1440 case is now
  a boundary case in `tests/nr-before-upscale/cases.h`.
- **Multipass** exists here as the pass chain (`Passes`, the `DLSS-NR chain:` line) at both stages.
- **Taken: the guides below 100%.** jlrouzies-fr's v0.8.92 of their fork found that the reduced path
  handed the model a working-size colour with frame-size depth and motion, plus a motion scale for
  the wrong texture. This tree had the same shape at both stages; see
  [reduced-scale-guides.md](reduced-scale-guides.md).
  - At `Stage=1` below 100%, the render-size guides are larger than the model, so fix 1 applies.
  - Fix 2 is a no-op there once the guides match, because the render-size colour is the vectors'
    reference size.
- **Not taken:**
  - Their deferred mode (`docs/DEFERRED-NR-DLSS.md`): NR's difference encoded into a carrier and
    enlarged by a second, private DLSS SR feature. It needs a second DLSS runtime instance and history,
    and by their own account DLSS may smooth or destabilize the biased carrier.
  - Their "finished picture" placement after frame generation. Our present-time pass is the nearest
    thing here.
- **Missing:** a non-zero colour origin; native Vulkan before the upscaler; the guide resample on
  Vulkan; the guide shader's bytecode; any measurement below 100% at either stage.


## 2026-10-15 measurement kit: implementation contract

`tools/nr_ab_compare.py` reads comparison sidecars with their PNG or linear-HDR PFM pairs. It measures
changes against each pair's own original; it must never silently resize render-input Stage 1 into
upscaler-output Stage 0 or call that a final-image quality score. Check actual route/stage, picture
settings, model/build/adapter provenance, completion proof, and within-pair dimensions before measuring.
Report native-pixel gradient/Laplacian energy, low-gradient residual variation (a noise proxy), RGB
bias/opponent-colour residual, and temporal residual variation with original-frame motion alongside it.
These are diagnostic statistics, not perceptual quality or ground truth. Sampling and units must be explicit.

Support the existing eight-recording raw burst for temporal work as well as a sequence of sidecar pairs.
The legacy burst has no frame ids or JSON provenance and assumes delayed completion; label those limits
and require a companion pair from the same arm for interpretation. Repeated empty triggers are sparse
pairs, not consecutive frames. Never claim continuous-frame flicker from their timestamps alone.
The tool stays stdlib-only so its synthetic PNG/PFM/raw tests run in the existing host CI without packages.


### What today's audit established

At `171f4574`, `ScopedPreUpscale` still substitutes its private render-sized colour only after a
successful composition and restores the original typed/untyped Color slots on scope exit. The post
path stands down for that evaluation; a declined contract is marked separately as `after-fallback`.
The 500 ms extent gate, no-present/creation-frame gate, scratch-loan serialization, format/state/
subrect checks, device ownership and allocation-failure latch remain. RR, NR Hold frame, the driver
proxy and HookMethod=2 cannot serve as a before-stage sample. Native Vulkan has no before stage.
The targeted `nr-before-upscale`, `nr-capture`, `nr-render-size`, `nr-gpu-timing` and `nr-invariants`
suites pass. That verifies host decisions, not that Crimson Desert currently executes Stage 1 on GPU.
No new runtime defect was found and no runtime code was changed by this kit.

The opening 2026-09-05 status and its shader-field counts above are historical, not the current
measurement status: September already measured cost. The remaining question is image quality and
stability with reproducible captures, especially at stronger upscaling ratios.

### Gabriel's first Crimson Desert A/B

Use **separate game processes** for Stage 0 and Stage 1, fixed resolution and model settings, following
the lifetime boundary above. No live stage/resolution/working-scale stress test is implied. The Claude
side manages configuration/captures; this kit neither deploys nor launches the game.

1. Use D3D12, the same save, display resolution and DLSS **Quality** for the first comparison. In the
   game turn **Ray Reconstruction off**, **frame generation off** and **dynamic resolution off**.
   Keep HDR, exposure-related game settings, time of day, weather, sharpening, motion blur and camera
   FOV identical. DLAA gives no render-resolution saving. Do Balanced/Performance later as separate
   experiments, never pooled into the first result.
2. Before launch, replace existing values in the DLL-side INI (do not append duplicate sections/keys):

   ```ini
   [DlssNr]
   Enabled=true
   ApplyModel=true
   HookMethod=1
   Stage=0
   ApplyAfterRR=false
   UseProxy=false
   HoldFrame=false
   Passes=1
   WorkingScale=1.0
   Transfer=0
   ReversibleMode=0
   PeripheryCompression=false
   UiProtection=false
   StabilizerStrength=0
   Cadence=1
   CadenceSmoothing=0
   Compare=0
   DebugView=0
   WhitePointSource=0
   WhitePointScale=1.0
   GpuTiming=true
   GpuTimingInterval=30

   [Log]
   LogToFile=true
   LogLevel=2
   ```

   For the second process change **only `Stage=1`**. Keep preset/style/intensity/detail/colour/guard,
   highlight controls and every other setting unchanged, and retain both INIs with the result. Also retain the model DLL SHA-256: the sidecar
   version and byte count alone do not authenticate identical model contents. A
   different fixed paper-white scalar is acceptable if chosen before **both** runs; record it.
   Fixing source 0 avoids interpreting eye-adaptation normalization as temporal change; the recorded CPU
   white can differ from the actual live-exposure shader white under source 1. This is a
   controlled experiment, not a proposed new everyday preset. Sidecars do not contain every INI key.
3. Home → **Neural Rendering**: confirm Hook is the upscaled route (INI `HookMethod=1`), then Stage
   **After the upscaler** or **Before the upscaler (experimental)** as appropriate. Confirm NR is running.
   Under Compare, **Hold frame stays unchecked**, comparison/debug views off. The button there is
   **Capture original and processed**. Under GPU measurement enable **Measure NR on the GPU**, sample
   every 30 eligible evaluations. Close the menu and allow at least 10 seconds for history to settle.
   Select the stage before the run; do not use this walkthrough as permission for repeated live toggles.
4. Stand at a reproducible landmark facing a wall/sign with small texture detail, a diagonal roof/rail,
   and some foliage or hair against the sky. Include a smooth shadow patch. Keep the camera still;
   avoid combat, particles and NPCs crossing the analysis area. Record a reference screenshot and
   camera/FOV/save location so the other process can reproduce it. Animation and lighting can still
   move in a stationary-camera scene; the original images are the control for that.
5. In each process collect **five stationary-camera pairs**, then **three eight-recording raw bursts**
   without moving. Next do the same slow, constant-direction pan across the landmark: **five pairs**
   over the pan and **three eight-recording bursts**, repeating the same pan for each burst. Return to
   the marker between repetitions. That is **10 primary pairs plus 6 companion pairs and 6 raw bursts (48 recordings) per stage**;
   only 16 of the session's 64 pair slots are needed. Keep still and pan directories separate.
6. Collect at least **100 eligible GPU samples per stage** in a separate capture-free interval with
   the same view/workload. At interval 30 that is 3,000 model evaluations; wait for the actual count,
   not a guessed number of seconds. Save the log before rotation. Repeat a second Stage 0 process
   (A/B/A) if time permits, to expose scene or thermal drift.

**A held frame is a diagnostic control, not the Stage 1 arm.** NR's Hold frame explicitly declines
pre-upscale and uses the post path. The main protocol therefore holds the *camera*, not NR's input.
Optionally, in a separate Stage 0 control, check Hold frame, wait 10 evaluations, take two pairs, and
compare decoded original pixels; they should be identical. PNG file hashes will differ because the
embedded provenance changes. Mark that directory `stage0-held-control`; the A/B tool rejects it, as
it must. A game's own photo/pause mode is useful only if it still evaluates the upscaler, preserves the
same render pipeline and does not change resolution/post-processing; verify coverage before using it.
Do not turn Hold frame on during the Stage 1 slow-pan/flicker measurement.

### Trigger capture from outside, without menu interaction

The commands below are for the Claude-side operator during the arranged run, **not deployment**.
Set `GAME_DIR` to the directory containing the actually loaded proxy DLL, and `ARM_DIR` to a fresh
analysis directory outside the game, e.g. under `~/.cache/retoque-stage-ab/run1/stage0-still`.
Use a new arm directory for the pan and each stage/process. Do not overwrite a pending trigger.

```bash
export GAME_DIR="/path/to/Crimson Desert/bin64"
export ARM_DIR="$HOME/.cache/retoque-stage-ab/run1/stage0-still"
uv run python - <<'PY_CAPTURE'
import json, os, shutil, time
from pathlib import Path
game, out = Path(os.environ['GAME_DIR']), Path(os.environ['ARM_DIR'])
out.mkdir(parents=True, exist_ok=False)
captures, trigger = game / 'retoque-captures', game / 'dlssnr-capture.trigger'
for n in range(5):
    seen = set(captures.glob('*.json'))
    with trigger.open('x') as f:  # empty means one original/processed pair
        pass
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        found = []
        for p in set(captures.glob('*.json')) - seen:
            r = json.loads(p.read_text())
            files = [captures / r[k] for k in ('original_file', 'processed_file')]
            if all(f.is_file() for f in files):
                found.append((p, files))
        if found:
            if len(found) != 1:
                raise RuntimeError('multiple captures arrived; isolate the operator/run')
            p, files = found[0]
            for f in [*files, p]:
                shutil.copy2(f, out / f.name)
            print(n + 1, p.name, flush=True)
            break
        time.sleep(.25)
    else:
        raise RuntimeError('capture did not complete; inspect the capture refusal/status log')
    time.sleep(2)
PY_CAPTURE
```

The trigger is checked every **60 NR dispatches**, not every display frame or exactly once a second.
Wait for completed files as above; a deleted trigger alone is not proof of capture. Busy/no-write/
no-submission completion can refuse or delay it. These five pairs are **sparse samples**, not a
consecutive-frame burst. Linear-HDR surfaces produce `.original.pfm`/`.processed.pfm` plus `.json`;
SDR/code-value surfaces produce PNG. The tool handles both; do not tone-map PFM into PNG first.

For each of the six raw bursts, first take/retain one additional companion pair in the same view and
unchanged settings (use the script above with `range(1)` and a fresh `*-burstN-companion` directory), then request:

```bash
printf 'burst\n' > "$GAME_DIR/dlssnr-capture.trigger"
```

Only do this with no pending pair/trigger/burst. Wait for `dlssnr-capture/manifest.txt` to be newly
written after the request, `frames 8`, and all `before_00.raw`…`before_07.raw` and matching `after`
files to have finished writing. Copy the **entire** `dlssnr-capture/` to a fresh external directory
such as `stage0-still-burst1` before requesting another: the next burst overwrites names, and a new
process clears the old directory on its first burst. The manifest's old sentence “as the upscaler
produced it” is misleading for Stage 1: today's code records `hdrCopy`, the actual input of NR, on
both stages. Dark initial captures may re-arm; do not harvest an old manifest.

**Raw burst limitation:** eight adjacent successful recording opportunities, without present ids,
timestamps, per-frame settings, or GPU-fence proof. The legacy path waits eight dispatches before
mapping. Treat its temporal results as exploratory until a frame/present trace and completion proof
are available. This kit does not relabel that path as fence-safe. Do not infer display-frame flicker
frequency from it, and do not launch another burst while its readback/write is pending. The paired
capture must say `completion=submission-fence`; the comparator rejects assumed-completion pairs.

### Run and interpret the comparator

From this checkout, with copies outside the game:

```bash
uv run tools/nr_ab_compare.py \
  --stage0 "$AB_ROOT/stage0-still" --stage1 "$AB_ROOT/stage1-still" \
  --stage0-log "$AB_ROOT/stage0-timing.log" --stage1-log "$AB_ROOT/stage1-timing.log" \
  --output "$AB_ROOT/still-report.json"
```

Set `AB_ROOT` to the experiment root. For a raw-burst report, point `--stage0` and `--stage1` at the
respective `*-burstN-companion` directories and add `--stage0-burst` / `--stage1-burst` pointing at
the saved raw directories. Repeat for pan and each burst, using a fresh output name (the
tool refuses to overwrite). No dependencies beyond Python's stdlib; `uv` supplies the runner. One
sidecar instead of a directory is accepted for a companion/control pair, but temporal analysis needs
at least three captures for a useful sequence. The raw-burst association is the operator's assertion;
the tool checks extents/formats but cannot establish its stage or timestamp from the manifest.

The JSON contains both arms, per-pair statistics, medians, provenance, input SHA-256s, temporal
transitions, timing groups, units, sampling parameters and explicit limitations. No automatic winner.
No cropping or image registration is performed. Default `--grid 128` uniformly samples at most
128 centres per dimension; derivatives use adjacent native pixels, not grid neighbours. Increase to
`--grid 512` for a more expensive sensitivity check; keep it equal across reports. Full-image decode
still occurs. Source-1/HDR white-point changes normalize each pair separately and can confound temporal
values; the protocol fixes source 0. Raw values use the companion pair's white, explicitly assumed constant.

| Metric | Definition / interpretation | Cannot establish |
|---|---|---|
| `gradient_change_percent`, `laplacian_change_percent` | Change in RMS central-difference luminance gradient / four-neighbour Laplacian vs that pair's original. Null when original energy is zero | Added true detail; noise and ringing can increase both |
| `flat_residual_laplacian_rms` | RMS Laplacian of processed-minus-original in original regions with gradient ≤ `--flat-threshold` (default .01); report selected fraction | Sensor/model noise separately from texture, checkerboard aliasing or sharpening |
| `luma_edit_mae`, `luma_edit_bias`, `rgb_edit_bias` | Absolute and signed changes vs the same original; RGB bias keeps signs | Whether the change is more correct or pleasing |
| `opponent_colour_edit_rms` | RMS of residual R−G and B−G differences | Perceptual Delta E, display gamut/tonemapper effect |
| `edit_delta_rms` | RMS change in the NR luminance edit between successive captures, with original/processed change RMS alongside | Motion-compensated flicker or frequency in Hz |
| `stable_original_edit_delta_rms` | Same, only where original luminance changes ≤ threshold; selected fraction reported | True correspondence; similar luminance can hide motion/disocclusion |

PNG metrics use stored code values / full scale, with no gamma conversion. PFM metrics use linear
RGB / the recorded white point, retaining negative and above-white values. Raw values retain their
native precision rather than PNG quantization. Compare matching domains only. Native-pixel statistics
at different compose resolutions are not spatial-frequency-equivalent, even though their within-pair
percent changes are useful diagnostics. Inspect the originals and images along with the numbers.

### GPU-cost and acceptance gate

Look for both of these actual markers in their respective logs:

```text
DLSS-NR coverage stage=after ... window_model_ok=... window_applied=...
DLSS-NR coverage stage=before ... window_model_ok=... window_applied=...
DLSS-NR gpu timing confirmed: ... stage=after ... dispatch_ms=... model_ms=... outside_model_ms=... proof=sealed-single-execution-fence
DLSS-NR gpu timing confirmed: ... stage=before ... dispatch_ms=... model_ms=... outside_model_ms=... proof=sealed-single-execution-fence
```

The ellipses above mark omitted fields, not literal log text. Require sustained nonzero intended-stage
application after warm-up; report skips/fallback as proportions. `after-fallback`, `after-rr`, missing
coverage or `ZERO_APPLIED_IN_WINDOW` invalidate a Stage 1 result. Capture `route=before-upscale`,
`surface=render-input`, `settings` containing `stage=before` is an additional check. The tool rejects
wrong stages, held/carried frames, different build/model/adapter/picture settings, changed render extent,
missing provenance, missing fence proof, malformed/non-finite images and within-pair size mismatches.

The optional log reader accepts only the requested stage with `capture_active=0`, `model_reset=false`,
`carried=false`, cadence 1, all requested passes completed, and sealed single-execution fence proof.
It reports n/p10/median/p90 (linear-interpolated percentiles) for **dispatch**, **model** and **outside
model** GPU ms, separately for each run/contract/model identity/render/compose/model-size tuple.
It never accidentally matches `model_ms` inside `outside_model_ms`. Keep the log interval capture-free
anyway: the final draining capture/write can overlap a later non-capture sample. Timing-to-sidecar
association and equal workload remain operator checks. Do not pool startup or different contract groups.
`dispatch_ms` is NR cost, not full-frame time, input latency or an FPS prediction.

Accept the collection only with matched build/model/settings/game state, correct actual stages and
completion, ≥100 eligible cost samples per arm, and interpretable originals. Record a finding such as
“model median fell X ms; within-arm sharpness/noise/colour changed Y; stationary residual variation Z;
these surfaces differ in resolution” rather than “Stage 1 is objectively better.” A final displayed-image
quality comparison still needs matched screenshots/video after the upscaler and game post-processing:
Stage 1's existing pair is **not** that final surface. No final-image or new in-game performance claim
is made by this kit.
