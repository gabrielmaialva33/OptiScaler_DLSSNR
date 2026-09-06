# NR timing renderer boundaries

Run `python3 tests/nr-timing-boundary/run.py` from the repository root. The runner extracts the
production metadata builder and timing menu, compiles them with strict host fakes under ASan/UBSan,
and requires all 15 cases to execute. An assertion, compilation error, sanitizer report, or missing
source anchor is failure. Production declaration records are extracted from `DlssNr_GpuTiming.h`.

The executable checks configuration generations, stage, working-size and HDR/format changes, per-pass overrides,
reset/capture metadata, and immutable previously captured metadata. UI cases cover unsupported native
Vulkan/proxy paths, disabled measurement, enabling, interval edits, no confirmed sample, historical
sample age since recording and cadence, and disabling with an old sample present. No current-frame or averaged-cost
claim is permitted for the historical sample.

Source guards also require the production timing scope to begin before the first output barrier,
markers to surround each NGX call individually, successful completion to be armed after explicit
resource restores, and incomplete chains to be rejected. Legacy NR D3D12 timers and their split-cost
log must be absent. Destructor ordering is verified by inspection and the actual timing core tests;
source guards are not execution of the full Windows renderer.

The metadata cases include returning to a prior contract and repeated evaluation without a
generation/hash change.

These tests prove host metadata/UI decisions and detect marker-placement regressions. They do not run
NGX, submit D3D12 commands, validate actual GPU timestamps, or establish Crimson Desert acceptance.
The separate `tests/nr-gpu-timing` suite tests the production timing/certificate decision paths; a real
session with retained logs and external model-file hashes remains mandatory before interpreting an A/B.

## External Crimson Desert A/B acceptance

Do not install or change a game from this test runner. The user/Claude must separately install the
agreed instrumented build with a rollback copy, then record its stamp and both OptiScaler DLL hashes.
Record the SHA256 of the installed `nvngx_dlssnr.dll` before and after each process lifetime. The
runtime's model-file label (path, size, modification time and version, observed once per process) is
not a content hash and cannot distinguish modified files with identical attributes.

Use separate processes for SF-v2 and rtx40, with the same instrumented build, scene, route, NR tuning,
DLSS quality, stage, pass count, FG/DRS policy and sampling interval. Set WorkingScale explicitly to
**1.00** for this comparison; the previous Crimson Desert configuration used 0.75. With the intended
Quality setup, verify render 2293x960 and final output 3440x1440 in the actual session's source logs;
do not infer dimensions solely from the Quality label. Timing's `compose` field is the NR target,
which is smaller on Stage 1, not necessarily the final DLSS output.

Warm up consistently, then collect equal-duration windows long enough to contain many confirmed
samples. Do not change configuration, resize, or swap the model while measuring. Settings metadata
is a snapshot of legacy settings reads, not proof that every setting was read atomically by every
renderer operation. Discard warmup/reset and active-capture samples from the performance comparison;
report these exclusions separately from the instrument's own discard reasons. Retain all raw logs.

For each model report accepted sample count, instrument discards by reason, pending/retained samples,
interval and elapsed duration, followed by median and p95 of matched `dispatch_ms`, model and outside-
model intervals. Confirm stage, dimensions, requested/actual pass count and settings contracts match.
An empty or persistently pending result is a failed measurement, never zero cost. GPU timings establish
elapsed intervals on the observed queue; external frame-time measurement is still needed for impact
on whole-game performance. A different result from the old timer alone does not prove that the old
timer lied: workload, run variance and instrumentation overhead must also be considered.
